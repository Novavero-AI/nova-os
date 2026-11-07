/* Processes (docs/spec/objects.md O2, O6, O10): an address space, a
   handle table, and the user threads that run in them. A process starts
   with one thread and one handle, the bootstrap handle its creator
   passes; until thread_create exists that thread is its only one. */

#ifndef NOVA_PROCESS_H
#define NOVA_PROCESS_H

#include "object.h"
#include "../sched/thread.h"

/* The signal a process asserts once it has terminated (O9). */
#define PROCESS_TERMINATED (1u << 0)

typedef struct PROCESS PROCESS;

struct PROCESS {
    OBJECT object;
    WAITABLE waitable;
    UINT64 space; /* the physical address of its top page table */
    HANDLE_TABLE *handles;
    UINT32 state; /* PROCESS_STATE_NEW, _RUNNING or _TERMINATED */
    UINT32 exit_code;
    UINT32 threads; /* started and not yet exited */
};

/* A new process with an empty address space and handle table, and its
   creator's handle. Returns OK or ERR_NO_MEMORY. */
STATUS process_create(PROCESS **process);

/* Start a new process's first thread at entry, with RSP stack, the
   bootstrap handle's value in the process as its first argument and
   argument as its second (docs/spec/syscalls.md S8). bootstrap is a
   handle the caller holds, which passes to the process's table with
   rights. Returns OK; ERR_BAD_STATE if process has been started;
   ERR_INVALID_ARGS if entry or stack is at or above USER_TOP or stack is
   not 8 more than a multiple of 16 (S5, S8); or ERR_NO_MEMORY. On
   failure the caller keeps the handle. */
STATUS process_start(PROCESS *process, UINT64 entry, UINT64 stack,
                     OBJECT *bootstrap, UINT32 rights, UINT64 argument);

/* End process with exit_code: close every handle in its table, remove
   its mappings, record the code and assert PROCESS_TERMINATED (O10). A
   process already terminated keeps its first code. The caller is the
   process's last thread, which exits next, or the process has none. */
void process_terminate(PROCESS *process, UINT32 exit_code);

/* What process_info reports. The exit code is 0 until the process has
   terminated. */
void process_info(PROCESS *process, UINT32 *state, UINT32 *exit_code);

/* The running user thread's process. */
PROCESS *process_current(void);

/* End the calling thread's process with exit_code, then the thread. */
_Noreturn void process_exit(UINT32 exit_code);

/* End the calling thread. The process terminates with exit code 0 if it
   was the last (O10). */
_Noreturn void process_thread_exit(void);

/* The calling thread faulted in user mode with exception vector. */
_Noreturn void process_fault(UINT32 vector);

/* Called once a user thread has exited and its kernel stack is free:
   free the thread and drop its reference to its process. */
void process_thread_reaped(THREAD *thread);

/* The first process (docs/spec/ipc.md I6), as a kernel thread's entry
   whose argument is the BOOT_INFO: load the image nova-boot read from
   \nova\init.exe into a new process, pass it its bootstrap message, run
   it, and log the exit code process_info reports. Halts, with the
   reason, if the image cannot be loaded or memory runs out. */
void first_process_run(void *boot_info);

#endif
