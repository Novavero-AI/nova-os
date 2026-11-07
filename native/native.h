/* The native layer: the C functions through which a user program calls
   the nova-os kernel (docs/spec/syscalls.md S8). Each is a stub in
   native.S that enters the kernel with SYSCALL in the register
   convention of S4. Each returns the call's status (S7), and writes its
   results through the pointers it takes, 0 when the call fails, except
   where a function says otherwise. These are the calls the kernel
   implements so far. */

#ifndef NOVA_NATIVE_H
#define NOVA_NATIVE_H

#include "native_abi.h"

/* Remove handle from the caller's table and drop its reference. */
UINT32 handle_close(UINT32 handle);

/* Read the first message queued at channel, which needs CHANNEL_RIGHT_READ:
   copy its bytes to bytes and move its handles into the caller's table,
   describing each at handles. Reports the message's sizes in byte_count
   and handle_count, also when it fails with ERR_BUFFER_TOO_SMALL, which
   leaves the message queued. An empty queue gives ERR_SHOULD_WAIT, or
   ERR_PEER_CLOSED once the other end is closed. */
UINT32 channel_read(UINT32 channel, void *bytes, UINT32 byte_capacity,
                    RECEIVED_HANDLE *handles, UINT32 handle_capacity,
                    UINT32 *byte_count, UINT32 *handle_count);

/* The time on the monotonic clock in nanoseconds. */
UINT32 clock_monotonic(UINT64 *nanoseconds);

/* End the calling process with exit_code, which its creator reads with
   process_info. */
_Noreturn void process_exit(UINT32 exit_code);

/* The state of process, PROCESS_STATE_NEW, _RUNNING or _TERMINATED, and
   its exit code, 0 until it has terminated. Needs PROCESS_RIGHT_INSPECT. */
UINT32 process_info(UINT32 process, UINT32 *state, UINT32 *exit_code);

/* End the calling thread; the process ends with exit code 0 if it was
   the last. */
_Noreturn void thread_exit(void);

/* Write count bytes, at most DEBUG_WRITE_MAX_BYTES, to the kernel's debug
   log. Needs RESOURCE_RIGHT_USE on a resource that covers the debug log. */
UINT32 debug_write(UINT32 resource, const void *bytes, UINT64 count);

/* Wait until any of signals is active on the object handle names, which
   needs RIGHT_WAIT, or the deadline on the monotonic clock passes
   (ERR_TIMED_OUT), and report the signals observed. */
UINT32 object_wait_one(UINT32 handle, UINT32 signals, UINT64 deadline,
                       UINT32 *observed);

/* object_wait_one on up to WAIT_MANY_MAX objects at once, writing each
   entry's observed signals. */
UINT32 object_wait_many(WAIT_ENTRY *items, UINT32 count, UINT64 deadline);

/* A new channel whose endpoints each queue up to capacity messages. */
UINT32 channel_create(UINT32 capacity, UINT32 *first, UINT32 *second);

/* Write a message of byte_count bytes and handle_count handles, which
   leave the caller's table, to channel, which needs CHANNEL_RIGHT_WRITE.
   A full queue at the other end gives ERR_SHOULD_WAIT; a closed one
   ERR_PEER_CLOSED. */
UINT32 channel_write(UINT32 channel, const void *bytes, UINT32 byte_count,
                     const HANDLE_TRANSFER *handles, UINT32 handle_count);

/* A new process in job, which needs JOB_RIGHT_CREATE_PROCESS, with
   PROCESS_OPTION_ bits; PROCESS_OPTION_WRITE_EXECUTE also needs
   JOB_RIGHT_WRITE_EXECUTE. */
UINT32 process_create(UINT32 job, UINT32 options, UINT32 *process);

/* Start process's first thread, thread, at entry with stack, passing it
   bootstrap, which leaves the caller's table, and argument. Needs
   PROCESS_RIGHT_CREATE_THREAD, THREAD_RIGHT_START and RIGHT_TRANSFER on
   the bootstrap handle. */
UINT32 process_start(UINT32 process, UINT32 thread, UINT64 entry,
                     UINT64 stack, UINT32 bootstrap, UINT64 argument);

/* A new thread of process, which needs PROCESS_RIGHT_CREATE_THREAD. */
UINT32 thread_create(UINT32 process, UINT32 *thread);

/* A new section of size bytes, rounded up to pages, all zero; options
   must be 0. */
UINT32 section_create(UINT64 size, UINT32 options, UINT32 *section);

/* Map size bytes of section from offset at address in process, which
   needs PROCESS_RIGHT_MAP, with MAP_ protection bits, which need the
   matching section rights. */
UINT32 process_map(UINT32 process, UINT32 section, UINT64 offset,
                   UINT64 address, UINT64 size, UINT32 protection);

#endif
