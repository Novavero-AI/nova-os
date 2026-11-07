/* Processes and their user threads. A process's memory is three pages:
   the PROCESS, its handle table and its top page table, or four under
   page-table isolation, which gives it a user table too (paging.c); each
   user thread
   adds a page for its USER_THREAD and one for its vector state (fpu.h),
   besides its kernel stack. Its address space owns the pages the kernel
   maps for it, its first image and stack, and borrows the pages of the
   sections process_map maps, which the PROCESS holds until it ends. */

#include <stddef.h>

#include "process.h"
#include "cpu.h"
#include "../mm/page.h"
#include "../x86_64/fpu.h"
#include "../x86_64/paging.h"
#include "../x86_64/user.h"

/* A 64-bit thread's stack is 8 more than a multiple of 16, as after a
   call pushed its return address (docs/spec/syscalls.md S8). A 32-bit
   thread's entry and stack lie below 4 GiB, the most compatibility mode
   addresses, and its stack is 4-byte aligned, the alignment the x86
   conventions widen every argument to (S11). */
#define STACK_ALIGNMENT 16
#define STACK_START_OFFSET 8
#define COMPAT_ADDRESS_LIMIT 0x100000000ULL
#define COMPAT_STACK_ALIGNMENT 4

/* A process never started is destroyed with its last handle; a started
   one runs on until it exits or is killed (O10). */
static void last_handle(OBJECT *object)
{
    PROCESS *process = (PROCESS *)object;

    if (process->state == PROCESS_STATE_NEW)
        process_terminate(process, 0);
}

/* No thread holds the process any more, so none runs on its tables. */
static void destroy(OBJECT *object)
{
    PROCESS *process = (PROCESS *)object;

    paging_space_destroy(process->space);
    handle_table_destroy(process->handles);
    object_release(&process->job->object);
    page_free_mapped(process);
}

static WAITABLE *process_waitable(OBJECT *object)
{
    return &((PROCESS *)object)->waitable;
}

static const OBJECT_TYPE process_type = {TYPE_PROCESS, last_handle, destroy,
                                         process_waitable, process_waitable};

STATUS process_create(JOB *job, UINT32 options, PROCESS **out)
{
    PROCESS *process = page_alloc_mapped();

    if (!process)
        return ERR_NO_MEMORY;
    process->handles = handle_table_create();
    process->space = process->handles ? paging_space_create() : 0;
    if (!process->space) {
        if (process->handles)
            page_free_mapped(process->handles);
        page_free_mapped(process);
        return ERR_NO_MEMORY;
    }
    object_init(&process->object, &process_type);
    waitable_init(&process->waitable);
    object_retain(&job->object);
    process->job = job;
    process->options = options;
    process->state = PROCESS_STATE_NEW;
    process->terminating = 0;
    process->exit_code = 0;
    process->threads = 0;
    process->running = 0;
    process->mapping_count = 0;
    *out = process;
    return OK;
}

int process_is_32_bit(const PROCESS *process)
{
    return (process->options & PROCESS_OPTION_32_BIT) != 0;
}

/* ------------------------------------------------------------------------
   Threads. A started thread's object holds one reference for the
   scheduler, which process_thread_reaped drops; a thread never started
   is destroyed with its last handle (O10). A started thread is on its
   process's running list until it exits. */

static void thread_destroy(OBJECT *object)
{
    USER_THREAD *user = (USER_THREAD *)object;

    page_free_mapped(user->fpu);
    object_release(&user->process->object);
    page_free_mapped(user);
}

static WAITABLE *thread_waitable(OBJECT *object)
{
    return &((USER_THREAD *)object)->thread.waitable;
}

static const OBJECT_TYPE thread_type = {TYPE_THREAD, 0, thread_destroy,
                                        thread_waitable, thread_waitable};

STATUS process_thread_create(PROCESS *process, USER_THREAD **out)
{
    USER_THREAD *user = page_alloc_mapped();
    UINT8 *fpu = user ? page_alloc_mapped() : 0;

    if (!fpu) {
        if (user)
            page_free_mapped(user);
        return ERR_NO_MEMORY;
    }
    fpu_state_init(fpu);
    object_init(&user->object, &thread_type);
    waitable_init(&user->thread.waitable);
    object_retain(&process->object);
    user->process = process;
    user->fpu = fpu;
    user->started = 0;
    *out = user;
    return OK;
}

static int start_allowed(const PROCESS *process, UINT64 entry, UINT64 stack)
{
    if (process_is_32_bit(process))
        return entry < COMPAT_ADDRESS_LIMIT && stack < COMPAT_ADDRESS_LIMIT &&
               stack % COMPAT_STACK_ALIGNMENT == 0;
    return entry < user_space_top() && stack < user_space_top() &&
           stack % STACK_ALIGNMENT == STACK_START_OFFSET;
}

static USER_MODE mode_of(const PROCESS *process)
{
    return process_is_32_bit(process) ? USER_MODE_COMPATIBILITY
                                      : USER_MODE_64_BIT;
}

static USER_THREAD *user_thread_of(THREAD *thread)
{
    return (USER_THREAD *)((UINT8 *)thread - offsetof(USER_THREAD, thread));
}

/* Make a thread whose THREAD thread_create_user has set up runnable,
   holding the scheduler's reference, on its process's running list.
   With interrupts disabled. */
static void run_thread(PROCESS *process, USER_THREAD *thread)
{
    object_retain(&thread->object);
    thread->started = 1;
    thread->next_running = process->running;
    process->running = thread;
    process->threads++;
    thread_start(&thread->thread);
}

/* The thread's object holds a reference to its process, and the process
   is RUNNING from here until it terminates (O10). Interrupts stay
   disabled from the state's check until the thread runs, so no
   termination begins in between. */
STATUS process_start(PROCESS *process, USER_THREAD *thread, UINT64 entry,
                     UINT64 stack, OBJECT *bootstrap, UINT32 rights,
                     UINT64 argument)
{
    UINT64 flags = save_and_disable_interrupts();
    STATUS status = OK;
    UINT32 index;

    if (process->state != PROCESS_STATE_NEW || process->terminating ||
        thread->started)
        status = ERR_BAD_STATE;
    else if (thread->process != process ||
             !start_allowed(process, entry, stack))
        status = ERR_INVALID_ARGS;
    else if (handle_reserve(process->handles, &index) != OK)
        status = ERR_NO_MEMORY;
    else if (thread_create_user(&thread->thread, process, process->space,
                                thread->fpu, mode_of(process), entry, stack,
                                handle_value(process->handles, index),
                                argument) != OK) {
        handle_unreserve(process->handles, index);
        status = ERR_NO_MEMORY;
    }
    if (status == OK) {
        handle_place(process->handles, index, bootstrap, rights);
        process->state = PROCESS_STATE_RUNNING;
        run_thread(process, thread);
    }
    restore_interrupts(flags);
    return status;
}

/* Interrupts stay disabled from the state's check until the thread is on
   the running list, so a termination that begins meanwhile stops it. */
STATUS process_thread_start(USER_THREAD *thread, UINT64 entry, UINT64 stack,
                            UINT64 argument1, UINT64 argument2)
{
    PROCESS *process = thread->process;
    UINT64 flags = save_and_disable_interrupts();
    STATUS status = OK;

    if (thread->started || process->state != PROCESS_STATE_RUNNING ||
        process->terminating)
        status = ERR_BAD_STATE;
    else if (!start_allowed(process, entry, stack))
        status = ERR_INVALID_ARGS;
    else if (thread_create_user(&thread->thread, process, process->space,
                                thread->fpu, mode_of(process), entry, stack,
                                argument1, argument2) != OK)
        status = ERR_NO_MEMORY;
    if (status == OK)
        run_thread(process, thread);
    restore_interrupts(flags);
    return status;
}

/* The THREAD is inside its object, which the scheduler's reference keeps
   until here; waiters on the thread hold references of their own. */
void process_thread_reaped(THREAD *thread)
{
    object_release(&user_thread_of(thread)->object);
}

/* ------------------------------------------------------------------------
   Mappings. */

static int protection_of(UINT32 protection, USER_PROTECTION *out)
{
    switch (protection) {
    case MAP_READ:
        *out = USER_READ;
        return 1;
    case MAP_READ | MAP_WRITE:
        *out = USER_READ_WRITE;
        return 1;
    case MAP_READ | MAP_EXECUTE:
        *out = USER_READ_EXECUTE;
        return 1;
    case MAP_READ | MAP_WRITE | MAP_EXECUTE:
        *out = USER_READ_WRITE_EXECUTE;
        return 1;
    }
    return 0;
}

UINT32 process_map_rights(UINT32 protection)
{
    return ((protection & MAP_READ) ? SECTION_RIGHT_READ : 0) |
           ((protection & MAP_WRITE) ? SECTION_RIGHT_WRITE : 0) |
           ((protection & MAP_EXECUTE) ? SECTION_RIGHT_EXECUTE : 0);
}

static int page_aligned(UINT64 value)
{
    return value % PAGE_SIZE == 0;
}

/* With interrupts disabled from the checks to the last page, the range
   cannot change under the call. */
STATUS process_map(PROCESS *process, SECTION *section, UINT64 offset,
                   UINT64 address, UINT64 size, UINT32 protection)
{
    USER_PROTECTION user_protection;
    UINT64 pages, i, flags;
    STATUS status = OK;

    if (!protection_of(protection, &user_protection) ||
        !page_aligned(offset) || !page_aligned(address) ||
        !page_aligned(size) || size == 0 || !user_range_valid(address, size))
        return ERR_INVALID_ARGS;
    if (user_protection == USER_READ_WRITE_EXECUTE &&
        !(process->options & PROCESS_OPTION_WRITE_EXECUTE))
        return ERR_ACCESS_DENIED;
    pages = size / PAGE_SIZE;
    if (offset / PAGE_SIZE > section->pages ||
        pages > section->pages - offset / PAGE_SIZE)
        return ERR_OUT_OF_RANGE;
    flags = save_and_disable_interrupts();
    if (process->terminating)
        status = ERR_BAD_STATE;
    for (i = 0; status == OK && i < pages; i++)
        if (paging_user_mapped(process->space, address + i * PAGE_SIZE))
            status = ERR_BAD_STATE;
    if (status == OK && process->mapping_count == PROCESS_MAX_MAPPINGS)
        status = ERR_NO_MEMORY;
    for (i = 0; status == OK && i < pages; i++)
        if (!paging_map_user_borrowed(
                process->space, address + i * PAGE_SIZE,
                section_page(section, offset / PAGE_SIZE + i),
                user_protection, section->memory_type))
            status = ERR_NO_MEMORY;
    if (status == ERR_NO_MEMORY)
        while (i-- > 0)
            paging_unmap_user_borrowed(process->space, address + i * PAGE_SIZE);
    if (status == OK) {
        object_retain(&section->object);
        process->mappings[process->mapping_count++] = section;
    }
    restore_interrupts(flags);
    return status;
}

/* ------------------------------------------------------------------------
   Termination. */

/* The last step, once no thread of the process is left: the space is
   cleared before the sections go, so no translation of a section's page
   outlives the reference that keeps the page. */
static void finish(PROCESS *process)
{
    UINT64 flags = save_and_disable_interrupts();
    UINT32 count = process->mapping_count, i;

    process->mapping_count = 0;
    restore_interrupts(flags);
    handle_table_close_all(process->handles);
    paging_space_clear(process->space);
    for (i = 0; i < count; i++)
        object_release(&process->mappings[i]->object);
    flags = save_and_disable_interrupts();
    process->state = PROCESS_STATE_TERMINATED;
    restore_interrupts(flags);
    waitable_update(&process->waitable, 0, PROCESS_TERMINATED);
}

/* A thread not yet blocked when it is stopped finds stopping set when it
   next would block or return to user mode. */
void process_terminate(PROCESS *process, UINT32 exit_code)
{
    UINT64 flags = save_and_disable_interrupts();
    USER_THREAD *thread;
    UINT32 left;

    if (process->terminating) {
        restore_interrupts(flags);
        return;
    }
    process->terminating = 1;
    process->exit_code = exit_code;
    for (thread = process->running; thread; thread = thread->next_running) {
        thread->thread.stopping = 1;
        sched_end_wait(&thread->thread, ERR_CANCELED);
    }
    left = process->threads;
    restore_interrupts(flags);
    if (left == 0)
        finish(process);
}

void process_user_return(void)
{
    if (thread_current()->stopping)
        process_thread_exit();
}

void process_info(PROCESS *process, UINT32 *state, UINT32 *exit_code)
{
    UINT64 flags = save_and_disable_interrupts();

    *state = process->state;
    *exit_code =
        process->state == PROCESS_STATE_TERMINATED ? process->exit_code : 0;
    restore_interrupts(flags);
}

PROCESS *process_current(void)
{
    return thread_current()->process;
}

_Noreturn void process_exit(UINT32 exit_code)
{
    process_terminate(process_current(), exit_code);
    process_thread_exit();
}

/* The thread leaves the running list; the last to leave finishes the
   process, terminating it with exit code 0 if nothing had (O10). */
_Noreturn void process_thread_exit(void)
{
    USER_THREAD *self = user_thread_of(thread_current());
    PROCESS *process = self->process;
    UINT64 flags = save_and_disable_interrupts();
    USER_THREAD **link = &process->running;
    UINT32 left;

    while (*link != self)
        link = &(*link)->next_running;
    *link = self->next_running;
    left = --process->threads;
    if (left == 0 && !process->terminating) {
        process->terminating = 1;
        process->exit_code = 0;
    }
    restore_interrupts(flags);
    if (left == 0)
        finish(process);
    thread_exit();
}

/* A fault goes first to the process's exception channel (O10), which no
   process can open yet, so no handler resolves it: the process ends with
   the vector as its exit code. */
_Noreturn void process_fault(UINT32 vector)
{
    process_exit(vector);
}
