/* Threads and the scheduler.

   One processor for now. Its scheduling state is a CPU record that
   cpu_self finds, and every change to that record, or to the scheduling
   fields of a thread once it is started, is made with interrupts
   disabled, which on one processor keeps out both the timer interrupt
   and preemption. A second
   processor adds a lock to each such section and finds its own record
   through its per-CPU block (docs/spec/syscalls.md S2).

   Ready threads run round robin: a thread runs until it blocks or exits,
   or until the first timer tick after it has run for TIME_SLICE_MS on the
   monotonic clock; then the next in the queue runs, and a preempted
   thread goes to the back, as does a thread whose wait ends. Measuring
   the slice on the clock rather than in ticks keeps its length when ticks
   arrive late or are missed. With nothing ready the boot thread runs,
   halting until an interrupt; it never blocks and never queues.

   A thread's kernel stack is a run of pages from the page allocator used
   through the direct map, whose lowest page is a guard: its direct-map
   translation is removed, so running off the stack faults there, and the
   double-fault handler, on a stack of its own, reports it (idt.c). A
   thread cannot free the stack it runs on, so an exiting thread leaves
   itself in its CPU's record and the thread that runs next frees the
   stack right after the switch, before anything else runs, then asserts
   THREAD_TERMINATED and, for a user thread, hands the THREAD back to its
   process. */

#include "thread.h"
#include "cpu.h"
#include "memory_layout.h"
#include "../mm/page.h"
#include "../object/process.h"
#include "../x86_64/clock.h"
#include "../x86_64/paging.h"

#define TIME_SLICE_NS (TIME_SLICE_MS * NS_PER_MS)

typedef struct {
    THREAD *current;
    THREAD *idle;
    THREAD *ready_head, *ready_tail;
    THREAD *timeouts;      /* blocked threads with a deadline, earliest first */
    THREAD *exited;        /* the thread that exited, until its stack is free */
    UINT64 slice_end;      /* when the current thread's slice ends */
    UINT64 last_tick;      /* when the latest timer interrupt arrived */
    UINT64 longest_tick;   /* the longest interval between two since taken */
} CPU;

static CPU boot_cpu;
static THREAD boot_thread;

static CPU *cpu_self(void)
{
    return &boot_cpu;
}

THREAD *thread_current(void)
{
    return cpu_self()->current;
}

/* ------------------------------------------------------------------------
   The ready queue and the timeout list. */

static void make_ready(CPU *cpu, THREAD *thread)
{
    thread->state = THREAD_READY;
    thread->next_ready = 0;
    if (cpu->ready_tail)
        cpu->ready_tail->next_ready = thread;
    else
        cpu->ready_head = thread;
    cpu->ready_tail = thread;
}

static THREAD *take_ready(CPU *cpu)
{
    THREAD *thread = cpu->ready_head;

    if (thread) {
        cpu->ready_head = thread->next_ready;
        if (!cpu->ready_head)
            cpu->ready_tail = 0;
    }
    return thread;
}

/* Behind every thread with the same deadline, so equal deadlines wake in
   the order they were set. */
static void add_timeout(CPU *cpu, THREAD *thread)
{
    THREAD **link = &cpu->timeouts;

    while (*link && (*link)->deadline <= thread->deadline)
        link = &(*link)->next_timeout;
    thread->next_timeout = *link;
    *link = thread;
}

static void remove_timeout(CPU *cpu, THREAD *thread)
{
    THREAD **link;

    for (link = &cpu->timeouts; *link; link = &(*link)->next_timeout)
        if (*link == thread) {
            *link = thread->next_timeout;
            return;
        }
}

/* ------------------------------------------------------------------------
   Stacks. */

static void free_pages(UINT64 run)
{
    UINT64 i;

    for (i = 0; i < THREAD_STACK_RUN; i++)
        page_free(run + i * PAGE_SIZE);
}

static UINT64 alloc_stack(void)
{
    UINT64 run = page_alloc_run(THREAD_STACK_RUN);

    if (run && !paging_unmap_direct(run)) {
        free_pages(run);
        run = 0;
    }
    return run;
}

static void free_stack(UINT64 run)
{
    paging_restore_direct(run);
    free_pages(run);
}

/* ------------------------------------------------------------------------
   Switching. */

/* Runs on the incoming thread right after every switch, with interrupts
   still disabled. */
static void finish_switch(CPU *cpu)
{
    THREAD *exited = cpu->exited;

    if (!exited)
        return;
    cpu->exited = 0;
    free_stack(exited->stack);
    waitable_update(&exited->waitable, 0, THREAD_TERMINATED);
    if (exited->process)
        process_thread_reaped(exited);
}

/* Give the processor to the next ready thread, or to the boot thread if
   none is, with interrupts disabled. The running thread has been marked
   blocked or dead if it is not to run again; otherwise it is preempted. */
static void reschedule(CPU *cpu)
{
    THREAD *previous = cpu->current;
    THREAD *next;

    if (previous->state == THREAD_RUNNING) {
        if (previous == cpu->idle)
            previous->state = THREAD_READY;
        else
            make_ready(cpu, previous);
    }
    next = take_ready(cpu);
    if (!next)
        next = cpu->idle;
    next->state = THREAD_RUNNING;
    cpu->slice_end = clock_monotonic() + TIME_SLICE_NS;
    if (next == previous)
        return;
    next->switches++;
    cpu->current = next;
    arch_switch(&previous->context, &next->context);
    finish_switch(cpu_self());
}

/* Where a new thread's first switch arrives (arch_context_init). */
static void thread_run(void)
{
    THREAD *self;

    finish_switch(cpu_self());
    self = thread_current();
    enable_interrupts();
    self->entry(self->argument);
    thread_exit();
}

void thread_user_first_run(void)
{
    finish_switch(cpu_self());
}

/* ------------------------------------------------------------------------
   Threads. */

/* The fields every new thread starts with; returns its stack's top, or 0
   if no stack is free. */
static UINT64 init_thread(THREAD *thread, THREAD_ENTRY entry, void *argument,
                          struct PROCESS *process)
{
    UINT64 run = alloc_stack();

    if (!run)
        return 0;
    thread->state = THREAD_NEW;
    thread->next_ready = 0;
    thread->next_timeout = 0;
    thread->deadline = 0;
    thread->wait_status = OK;
    thread->stack = run;
    thread->entry = entry;
    thread->argument = argument;
    thread->switches = 0;
    thread->process = process;
    return (UINT64)direct_map(run + THREAD_STACK_RUN * PAGE_SIZE);
}

STATUS thread_create(THREAD *thread, THREAD_ENTRY entry, void *argument)
{
    UINT64 top = init_thread(thread, entry, argument, 0);

    if (!top)
        return ERR_NO_MEMORY;
    waitable_init(&thread->waitable);
    arch_context_init(&thread->context, top, thread_run);
    return OK;
}

STATUS thread_create_user(THREAD *thread, struct PROCESS *process,
                          UINT64 address_space, UINT8 *fpu, USER_MODE mode,
                          UINT64 entry, UINT64 stack, UINT64 argument1,
                          UINT64 argument2)
{
    UINT64 top = init_thread(thread, 0, 0, process);

    if (!top)
        return ERR_NO_MEMORY;
    arch_context_init_user(&thread->context, top, address_space, fpu, mode,
                           entry, stack, argument1, argument2);
    return OK;
}

void thread_set_tls_base(UINT64 base)
{
    UINT64 flags = save_and_disable_interrupts();

    arch_set_tls_base(&thread_current()->context, base);
    restore_interrupts(flags);
}

STATUS thread_start(THREAD *thread)
{
    UINT64 flags = save_and_disable_interrupts();
    STATUS status = ERR_BAD_STATE;

    if (thread->state == THREAD_NEW) {
        make_ready(cpu_self(), thread);
        status = OK;
    }
    restore_interrupts(flags);
    return status;
}

/* A dead thread is never queued, so the switch never returns here and
   the halt after it is never reached. */
_Noreturn void thread_exit(void)
{
    CPU *cpu;

    disable_interrupts();
    cpu = cpu_self();
    cpu->current->state = THREAD_DEAD;
    cpu->exited = cpu->current;
    reschedule(cpu);
    halt_forever();
}

/* ------------------------------------------------------------------------
   The scheduler. */

void sched_init(void)
{
    CPU *cpu = cpu_self();

    arch_context_init_boot(&boot_thread.context);
    boot_thread.state = THREAD_RUNNING;
    cpu->current = &boot_thread;
    cpu->idle = &boot_thread;
}

/* An interrupt that makes a thread ready while this thread halts switches
   to it before returning here; one that arrives between the check and
   the halt wakes the halt, as wait_for_interrupt describes. */
void sched_idle(void)
{
    CPU *cpu = cpu_self();

    disable_interrupts();
    for (;;) {
        if (cpu->ready_head)
            reschedule(cpu);
        else
            wait_for_interrupt();
    }
}

static void end_wait(CPU *cpu, THREAD *thread, STATUS status)
{
    thread->wait_status = status;
    make_ready(cpu, thread);
}

void sched_timer_interrupt(void)
{
    CPU *cpu = cpu_self();
    UINT64 now = clock_monotonic();
    THREAD *due;

    if (cpu->last_tick && now - cpu->last_tick > cpu->longest_tick)
        cpu->longest_tick = now - cpu->last_tick;
    cpu->last_tick = now;
    while ((due = cpu->timeouts) != 0 && due->deadline <= now) {
        cpu->timeouts = due->next_timeout;
        end_wait(cpu, due, ERR_TIMED_OUT);
    }
    if (cpu->current == cpu->idle ? cpu->ready_head != 0
                                  : now >= cpu->slice_end)
        reschedule(cpu);
}

UINT64 sched_take_longest_tick(void)
{
    UINT64 flags = save_and_disable_interrupts();
    CPU *cpu = cpu_self();
    UINT64 longest = cpu->longest_tick;

    cpu->longest_tick = 0;
    restore_interrupts(flags);
    return longest;
}

STATUS sched_block(UINT64 deadline)
{
    CPU *cpu = cpu_self();
    THREAD *self = cpu->current;

    self->state = THREAD_BLOCKED;
    self->deadline = deadline;
    if (deadline != DEADLINE_INFINITE)
        add_timeout(cpu, self);
    reschedule(cpu);
    return self->wait_status;
}

void sched_end_wait(THREAD *thread, STATUS status)
{
    CPU *cpu = cpu_self();

    if (thread->state != THREAD_BLOCKED)
        return;
    if (thread->deadline != DEADLINE_INFINITE)
        remove_timeout(cpu, thread);
    end_wait(cpu, thread, status);
}
