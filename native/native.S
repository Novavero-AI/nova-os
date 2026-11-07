/* The native layer's system-call stubs (docs/spec/syscalls.md S4, S8).
   Each is a leaf function under the Microsoft x64 convention: it
   allocates no stack, so at the SYSCALL its RSP is what it was at entry,
   with the return address at [RSP], the caller's home area for the four
   register arguments at [RSP+0x08] to [RSP+0x27], and the fifth and later
   arguments from [RSP+0x28] (x64 calling convention, "Parameter passing";
   x64 stack usage). Argument 1 moves from RCX to R10, since SYSCALL
   overwrites RCX with the return address; arguments 2 to 4 stay in RDX,
   R8 and R9, and the kernel reads 5 to 8 from the stack. Results come
   back in RDX, R8 and R9. A stub that returns a result through a pointer
   argument passed in a register keeps the pointer in that argument's
   home slot, which the callee owns ("these 4 stack locations are
   effectively owned by the called function", x64 stack usage), and
   stores the result after the SYSCALL, also when the status is an error,
   when the result is 0. The status is in EAX. */

#include "native_abi.h"

/* Home slots of the first three register arguments, and the stack slots
   of the sixth and seventh arguments. */
#define HOME_RCX 0x08
#define HOME_RDX 0x10
#define HOME_R8 0x18
#define ARGUMENT_6 0x30
#define ARGUMENT_7 0x38

    .text

/* UINT32 handle_close(UINT32 handle) */
    .globl handle_close
handle_close:
    movq %rcx, %r10
    movl $CALL_HANDLE_CLOSE, %eax
    syscall
    ret

/* UINT32 channel_read(UINT32 channel, void *bytes, UINT32 byte_capacity,
                       RECEIVED_HANDLE *handles, UINT32 handle_capacity,
                       UINT32 *byte_count, UINT32 *handle_count) */
    .globl channel_read
channel_read:
    movq %rcx, %r10
    movl $CALL_CHANNEL_READ, %eax
    syscall
    movq ARGUMENT_6(%rsp), %rcx
    movl %edx, (%rcx)
    movq ARGUMENT_7(%rsp), %rcx
    movl %r8d, (%rcx)
    ret

/* UINT32 clock_monotonic(UINT64 *nanoseconds) */
    .globl clock_monotonic
clock_monotonic:
    movq %rcx, HOME_RCX(%rsp)
    movl $CALL_CLOCK_MONOTONIC, %eax
    syscall
    movq HOME_RCX(%rsp), %rcx
    movq %rdx, (%rcx)
    ret

/* void process_exit(UINT32 exit_code), which does not return; UD2 makes
   a return a fault in the caller's process rather than a jump into
   whatever follows. */
    .globl process_exit
process_exit:
    movq %rcx, %r10
    movl $CALL_PROCESS_EXIT, %eax
    syscall
    ud2

/* UINT32 process_info(UINT32 process, UINT32 *state, UINT32 *exit_code) */
    .globl process_info
process_info:
    movq %rdx, HOME_RDX(%rsp)
    movq %r8, HOME_R8(%rsp)
    movq %rcx, %r10
    movl $CALL_PROCESS_INFO, %eax
    syscall
    movq HOME_RDX(%rsp), %rcx
    movl %edx, (%rcx)
    movq HOME_R8(%rsp), %rcx
    movl %r8d, (%rcx)
    ret

/* void thread_exit(void), which does not return. */
    .globl thread_exit
thread_exit:
    movl $CALL_THREAD_EXIT, %eax
    syscall
    ud2

/* UINT32 debug_write(UINT32 resource, const void *bytes, UINT64 count) */
    .globl debug_write
debug_write:
    movq %rcx, %r10
    movl $CALL_DEBUG_WRITE, %eax
    syscall
    ret
