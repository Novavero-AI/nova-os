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

#endif
