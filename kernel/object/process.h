/* Processes and their user threads (docs/spec/objects.md O2, O6, O10):
   an address space, a handle table, and the user threads that run in
   them. A process belongs to a job and starts with one thread and one
   handle, the bootstrap handle its creator passes. */

#ifndef NOVA_PROCESS_H
#define NOVA_PROCESS_H

#include "job.h"
#include "section.h"
#include "../sched/thread.h"

/* The most process_map calls one process takes; each holds its section
   until the process terminates. */
#define PROCESS_MAX_MAPPINGS 128

/* The rights a new process's handle carries (O5, rule 1). */
#define PROCESS_RIGHTS                                                    \
    (PROCESS_RIGHT_MAP | PROCESS_RIGHT_CREATE_THREAD |                    \
     PROCESS_RIGHT_TERMINATE | PROCESS_RIGHT_INSPECT | RIGHT_DUPLICATE |  \
     RIGHT_TRANSFER | RIGHT_WAIT)

/* The rights a new thread's handle carries (O5, rule 1). */
#define THREAD_RIGHTS                                                     \
    (THREAD_RIGHT_START | THREAD_RIGHT_TERMINATE | RIGHT_DUPLICATE |      \
     RIGHT_TRANSFER | RIGHT_WAIT)

typedef struct PROCESS PROCESS;
typedef struct USER_THREAD USER_THREAD;

struct PROCESS {
    OBJECT object;
    WAITABLE waitable;
    JOB *job;     /* held for the process's life */
    UINT32 options; /* PROCESS_OPTION_32_BIT, _WRITE_EXECUTE */
    UINT64 space; /* the physical address of its top page table */
    HANDLE_TABLE *handles;
    UINT32 state; /* PROCESS_STATE_NEW, _RUNNING or _TERMINATED */
    UINT32 terminating; /* from process_terminate's start on */
    UINT32 exit_code;
    UINT32 threads; /* started and not yet exited, on the list below */
    USER_THREAD *running;
    UINT32 mapping_count;
    SECTION *mappings[PROCESS_MAX_MAPPINGS];
};

/* A user thread as an object: created for a process by thread_create
   and run by process_start or thread_start. */
struct USER_THREAD {
    OBJECT object;
    THREAD thread;
    UINT8 *fpu;
    PROCESS *process; /* held for the thread object's life */
    USER_THREAD *next_running;
    UINT32 started;
};

/* A new process in job, with an empty address space and handle table,
   and its creator's handle. options are PROCESS_OPTION_ bits, which the
   caller has checked against its job handle. Returns OK or
   ERR_NO_MEMORY. */
STATUS process_create(JOB *job, UINT32 options, PROCESS **process);

/* Whether the process's threads run in compatibility mode (S11). */
int process_is_32_bit(const PROCESS *process);

/* A new thread of process, not yet started, with its creator's handle.
   Returns OK or ERR_NO_MEMORY. */
STATUS process_thread_create(PROCESS *process, USER_THREAD **thread);

/* Start a new process's first thread, thread, at entry with stack
   pointer stack, the bootstrap handle's value in the process as its
   first argument and argument as its second (docs/spec/syscalls.md S8,
   S11). bootstrap is a handle the caller holds, which passes to the
   process's table with rights. Returns OK; ERR_BAD_STATE if process or
   thread has been started, or process is terminating; ERR_INVALID_ARGS
   if thread belongs to another process, or entry or stack lies outside
   what the process's mode allows (S5, S8, S11); or ERR_NO_MEMORY. On
   failure the caller keeps the handle. */
STATUS process_start(PROCESS *process, USER_THREAD *thread, UINT64 entry,
                     UINT64 stack, OBJECT *bootstrap, UINT32 rights,
                     UINT64 argument);

/* thread_start: start thread, a further thread of a running process, at
   entry with stack pointer stack and its two arguments (S8). Returns OK;
   ERR_BAD_STATE if thread has been started, or its process was never
   started or is terminating; ERR_INVALID_ARGS if entry or stack lies
   outside what the process's mode allows; or ERR_NO_MEMORY. */
STATUS process_thread_start(USER_THREAD *thread, UINT64 entry, UINT64 stack,
                            UINT64 argument1, UINT64 argument2);

/* The section rights a mapping with protection, MAP_ bits, needs (O5). */
UINT32 process_map_rights(UINT32 protection);

/* Map size bytes of section from offset at address in process, with
   protection MAP_ bits; the caller holds the section by a handle with
   process_map_rights(protection). Returns OK; ERR_INVALID_ARGS for a
   protection that is not read, read-write, read-execute or all three, or
   an offset, address or size that is not a whole number of pages or
   leaves user memory; ERR_ACCESS_DENIED if the protection is more than
   section_protections allows, or writable and executable and the
   process was not created with PROCESS_OPTION_WRITE_EXECUTE (O5);
   ERR_OUT_OF_RANGE if the range passes the section's end; ERR_BAD_STATE
   if the process is terminating or a page of the range is mapped; or
   ERR_NO_MEMORY. A call that fails maps nothing. */
STATUS process_map(PROCESS *process, SECTION *section, UINT64 offset,
                   UINT64 address, UINT64 size, UINT32 protection);

/* End process with exit_code (O10). Its threads stop first: each
   blocked one's wait ends with ERR_CANCELED, none blocks again, and each
   exits at its next return to user mode instead of returning. Once the
   last has exited, the kernel closes every handle in the process's
   table, removes its mappings and asserts PROCESS_TERMINATED, so no
   thread of the process runs, and none can add a handle, after its
   handles close. A process with no thread left ends here and now. A
   process already terminating keeps its first code. A caller that is a
   thread of process calls process_thread_exit next. */
void process_terminate(PROCESS *process, UINT32 exit_code);

/* The running user thread is about to return to user mode, with
   interrupts disabled until it does: if its process is terminating, it
   exits here instead. */
void process_user_return(void);

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
   drop the scheduler's reference to its thread object. */
void process_thread_reaped(THREAD *thread);

/* The boot-time self-test of kernel objects (object_test.c), run before
   the first process with the BOOT_INFO. It logs each result to the
   serial port, halts with the reason at the first failure, and returns
   when every check passes. */
void object_self_test(void *boot_info);

/* The first process (docs/spec/ipc.md I6), as a kernel thread's entry
   whose argument is the BOOT_INFO: load the image nova-boot read from
   \nova\init.exe into a new process, pass it its bootstrap message, run
   it, and log the exit code process_info reports. Halts, with the
   reason, if the image cannot be loaded or memory runs out. */
void first_process_run(void *boot_info);

#endif
