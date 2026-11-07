/* Processes and their user threads. A process's memory is three pages:
   the PROCESS, its handle table and its top page table; each user thread
   adds a page for its THREAD and one for its vector state (fpu.h), besides
   its kernel stack. Its address space owns every page it maps, since no
   mapping is shared yet (paging.h). */

#include "process.h"
#include "cpu.h"
#include "../mm/page.h"
#include "../x86_64/fpu.h"
#include "../x86_64/paging.h"
#include "../x86_64/user.h"

/* The stack a new thread starts on is 8 more than a multiple of 16, as
   after a call pushed its return address (docs/spec/syscalls.md S8). */
#define STACK_ALIGNMENT 16
#define STACK_START_OFFSET 8

typedef struct {
    THREAD thread;
    UINT8 *fpu;
} USER_THREAD;

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
    page_free_mapped(process);
}

static const OBJECT_TYPE process_type = {TYPE_PROCESS, last_handle, destroy};

STATUS process_create(PROCESS **out)
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
    process->state = PROCESS_STATE_NEW;
    process->exit_code = 0;
    process->threads = 0;
    *out = process;
    return OK;
}

/* The thread holds a reference to its process while it runs (O10). */
STATUS process_start(PROCESS *process, UINT64 entry, UINT64 stack,
                     OBJECT *bootstrap, UINT32 rights, UINT64 argument)
{
    USER_THREAD *user;
    UINT8 *fpu;
    UINT32 index;

    if (process->state != PROCESS_STATE_NEW)
        return ERR_BAD_STATE;
    if (entry >= user_space_top() || stack >= user_space_top() ||
        stack % STACK_ALIGNMENT != STACK_START_OFFSET)
        return ERR_INVALID_ARGS;
    if (handle_reserve(process->handles, &index) != OK)
        return ERR_NO_MEMORY;
    user = page_alloc_mapped();
    fpu = user ? page_alloc_mapped() : 0;
    if (fpu) {
        fpu_state_init(fpu);
        user->fpu = fpu;
    }
    if (!fpu || thread_create_user(&user->thread, process, process->space,
                                   fpu, entry, stack,
                                   handle_value(process->handles, index),
                                   argument) != OK) {
        if (fpu)
            page_free_mapped(fpu);
        if (user)
            page_free_mapped(user);
        handle_unreserve(process->handles, index);
        return ERR_NO_MEMORY;
    }
    handle_place(process->handles, index, bootstrap, rights);
    object_retain(&process->object);
    process->state = PROCESS_STATE_RUNNING;
    process->threads = 1;
    thread_start(&user->thread);
    return OK;
}

void process_terminate(PROCESS *process, UINT32 exit_code)
{
    UINT64 flags = save_and_disable_interrupts();

    if (process->state == PROCESS_STATE_TERMINATED) {
        restore_interrupts(flags);
        return;
    }
    process->state = PROCESS_STATE_TERMINATED;
    process->exit_code = exit_code;
    restore_interrupts(flags);
    handle_table_close_all(process->handles);
    paging_space_clear(process->space);
    waitable_update(&process->waitable, 0, PROCESS_TERMINATED);
}

void process_info(PROCESS *process, UINT32 *state, UINT32 *exit_code)
{
    UINT64 flags = save_and_disable_interrupts();

    *state = process->state;
    *exit_code = process->exit_code;
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

_Noreturn void process_thread_exit(void)
{
    PROCESS *process = process_current();
    UINT64 flags = save_and_disable_interrupts();
    UINT32 left = --process->threads;

    restore_interrupts(flags);
    if (left == 0)
        process_terminate(process, 0);
    thread_exit();
}

/* A fault goes first to the process's exception channel (O10), which no
   process can open yet, so no handler resolves it: the process ends with
   the vector as its exit code. */
_Noreturn void process_fault(UINT32 vector)
{
    process_exit(vector);
}

/* The THREAD goes as soon as it has asserted THREAD_TERMINATED, which
   is safe because nothing waits on a user thread: no handle to one
   exists yet. */
void process_thread_reaped(THREAD *thread)
{
    USER_THREAD *user = (USER_THREAD *)thread;
    PROCESS *process = thread->process;

    page_free_mapped(user->fpu);
    page_free_mapped(user);
    object_release(&process->object);
}
