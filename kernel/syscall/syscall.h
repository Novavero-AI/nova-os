/* The system calls (docs/spec/syscalls.md S8), as the architecture's
   entry hands them over: a call number, up to SYSCALL_ARGUMENTS_MAX
   arguments, and room for SYSCALL_RESULTS results. */

#ifndef NOVA_SYSCALL_H
#define NOVA_SYSCALL_H

#include "../status.h"

#define SYSCALL_ARGUMENTS_MAX 8
#define SYSCALL_RESULTS 3

/* The number of arguments call number takes, or 0 for a number that
   names no call this kernel implements. */
UINT32 syscall_argument_count(UINT64 number);

/* Whether the 32-bit lane serves call number (S11). */
int syscall_served_32_bit(UINT64 number);

/* Whether call number reports its results although it failed with
   status: every call reports the sizes needed with ERR_BUFFER_TOO_SMALL
   (S7), and object_wait_one the signals it observed, HANDLE_CLOSED among
   them, when a closing handle cancels its wait (docs/spec/objects.md
   O9). Every other failure reports 0 in every result. */
int syscall_reports_results(UINT64 number, STATUS status);

/* Run call number on arguments, writing its results; returns its status,
   or ERR_NOT_SUPPORTED for a number that names no call (S3). The calling
   thread's process is the caller. */
STATUS syscall_invoke(UINT64 number, const UINT64 *arguments,
                      UINT64 *results);

#endif
