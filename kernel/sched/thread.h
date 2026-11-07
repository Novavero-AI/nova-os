/* Threads and the scheduler: preemptive round robin on one processor,
   driven by the Local APIC timer, with an idle thread that halts when
   nothing is ready. A kernel thread runs kernel code only; a user thread
   belongs to a process and runs its code in user mode, entering the
   kernel on its own kernel stack. */

#ifndef NOVA_THREAD_H
#define NOVA_THREAD_H

#include "wait.h"
#include "../x86_64/context.h"

/* How long a thread runs before the timer gives the processor to the
   next ready thread, on the monotonic clock. */
#define TIME_SLICE_MS 10

/* A thread's stack: one guard page whose direct-map translation is
   removed, then the THREAD_STACK_PAGES pages the thread runs on. */
#define THREAD_STACK_PAGES 4
#define THREAD_STACK_RUN (1 + THREAD_STACK_PAGES)

/* The signal a thread asserts once it has exited and its stack is free
   (docs/spec/objects.md O9). */
#define THREAD_TERMINATED (1u << 0)

typedef void (*THREAD_ENTRY)(void *argument);

/* No state is 0, so thread_start refuses a zeroed THREAD that was never
   created. */
typedef enum {
    THREAD_NEW = 1,
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_BLOCKED,
    THREAD_DEAD
} THREAD_STATE;

typedef struct THREAD THREAD;
struct PROCESS;

struct THREAD {
    ARCH_CONTEXT context;
    THREAD_STATE state;
    THREAD *next_ready;
    THREAD *next_timeout;
    UINT64 deadline;    /* of the wait it is blocked in */
    STATUS wait_status; /* how its last wait ended */
    UINT64 stack;       /* physical address of the run's guard page */
    THREAD_ENTRY entry;
    void *argument;
    UINT64 switches; /* times the scheduler has switched to it */
    WAITABLE waitable;
    struct PROCESS *process; /* a user thread's process; 0 for a kernel thread */
};

/* Make thread a new thread that will run entry(argument) and then exit,
   with a stack from the page allocator, which it holds until it exits:
   a created thread is to be started. The caller owns the THREAD and keeps
   it, not creating it again, until the thread has terminated and every
   wait on it has returned, since a woken waiter's WAIT_ITEM stays linked
   to the thread until the waiter runs. Returns OK or ERR_NO_MEMORY. */
STATUS thread_create(THREAD *thread, THREAD_ENTRY entry, void *argument);

/* Make thread a new user thread of process, which will enter user mode at
   entry with RSP stack and arguments argument1 and argument2
   (arch_context_init_user), on the page tables at address_space, with its
   vector state in fpu. It gets a kernel stack as thread_create's threads
   do. The process owns the THREAD and the fpu area; process_thread_reaped
   is called once the thread has exited and its stack is free. Returns OK
   or ERR_NO_MEMORY. */
STATUS thread_create_user(THREAD *thread, struct PROCESS *process,
                          UINT64 address_space, UINT8 *fpu, UINT64 entry,
                          UINT64 stack, UINT64 argument1, UINT64 argument2);

/* Make a new thread ready to run. Returns OK, or ERR_BAD_STATE if it has
   been started before. */
STATUS thread_start(THREAD *thread);

/* End the running thread. Does not return. */
_Noreturn void thread_exit(void);

/* Called on a new user thread's first switch, before it enters user mode,
   as thread_run is for a kernel thread (switch.S). */
void thread_user_first_run(void);

THREAD *thread_current(void);

/* Make the running code, kernel_main's, the boot thread: the thread the
   processor runs when no other is ready. From then on the rest of
   kernel_main runs only while no thread it has started is ready. */
void sched_init(void);

/* The boot thread's last call: run whatever is ready, otherwise halt
   until an interrupt. Does not return. */
void sched_idle(void);

/* Called on every timer interrupt, after its EOI: ends the waits whose
   deadline has passed, and switches to the next ready thread when the
   running one has used its slice or is the boot thread. The interrupted
   thread's frame stays on its own stack until it is switched back to. */
void sched_timer_interrupt(void);

/* The longest interval between two timer interrupts, on the monotonic
   clock, since the previous call. The timer is programmed for TIMER_HZ;
   an emulator can deliver it later and skip periods. */
UINT64 sched_take_longest_tick(void);

/* For waits, with interrupts disabled. sched_block blocks the running
   thread, which is not the boot thread, until sched_wake or the deadline
   ends its wait, and returns OK or ERR_TIMED_OUT accordingly. sched_wake
   leaves a thread that is not blocked as it is: a wait ends once. */
STATUS sched_block(UINT64 deadline);
void sched_wake(THREAD *thread);

/* The boot-time self-test of threads, scheduling and waits, run as a
   thread whose argument is the spinner's thread. It logs each result to
   the serial port, halts with the reason at the first failure, and exits
   when every check passes. */
void sched_self_test(void *spinner);

#endif
