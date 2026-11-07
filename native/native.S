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

/* Home slots of the four register arguments, and the stack slots of the
   sixth and seventh arguments. */
#define HOME_RCX 0x08
#define HOME_RDX 0x10
#define HOME_R8 0x18
#define HOME_R9 0x20
#define ARGUMENT_5 0x28
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

/* UINT32 object_wait_one(UINT32 handle, UINT32 signals, UINT64 deadline,
                          UINT32 *observed) */
    .globl object_wait_one
object_wait_one:
    movq %r9, HOME_R9(%rsp)
    movq %rcx, %r10
    movl $CALL_OBJECT_WAIT_ONE, %eax
    syscall
    movq HOME_R9(%rsp), %rcx
    movl %edx, (%rcx)
    ret

/* UINT32 object_wait_many(WAIT_ENTRY *items, UINT32 count, UINT64 deadline) */
    .globl object_wait_many
object_wait_many:
    movq %rcx, %r10
    movl $CALL_OBJECT_WAIT_MANY, %eax
    syscall
    ret

/* UINT32 channel_create(UINT32 capacity, UINT32 *first, UINT32 *second) */
    .globl channel_create
channel_create:
    movq %rdx, HOME_RDX(%rsp)
    movq %r8, HOME_R8(%rsp)
    movq %rcx, %r10
    movl $CALL_CHANNEL_CREATE, %eax
    syscall
    movq HOME_RDX(%rsp), %rcx
    movl %edx, (%rcx)
    movq HOME_R8(%rsp), %rcx
    movl %r8d, (%rcx)
    ret

/* UINT32 channel_write(UINT32 channel, const void *bytes, UINT32 byte_count,
                        const HANDLE_TRANSFER *handles, UINT32 handle_count) */
    .globl channel_write
channel_write:
    movq %rcx, %r10
    movl $CALL_CHANNEL_WRITE, %eax
    syscall
    ret

/* UINT32 process_create(UINT32 job, UINT32 options, UINT32 *process) */
    .globl process_create
process_create:
    movq %r8, HOME_R8(%rsp)
    movq %rcx, %r10
    movl $CALL_PROCESS_CREATE, %eax
    syscall
    movq HOME_R8(%rsp), %rcx
    movl %edx, (%rcx)
    ret

/* UINT32 job_create(UINT32 parent, const JOB_LIMITS *limits, UINT32 *job) */
    .globl job_create
job_create:
    movq %r8, HOME_R8(%rsp)
    movq %rcx, %r10
    movl $CALL_JOB_CREATE, %eax
    syscall
    movq HOME_R8(%rsp), %rcx
    movl %edx, (%rcx)
    ret

/* UINT32 process_start(UINT32 process, UINT32 thread, UINT64 entry,
                        UINT64 stack, UINT32 bootstrap, UINT64 argument) */
    .globl process_start
process_start:
    movq %rcx, %r10
    movl $CALL_PROCESS_START, %eax
    syscall
    ret

/* UINT32 thread_create(UINT32 process, UINT32 *thread) */
    .globl thread_create
thread_create:
    movq %rdx, HOME_RDX(%rsp)
    movq %rcx, %r10
    movl $CALL_THREAD_CREATE, %eax
    syscall
    movq HOME_RDX(%rsp), %rcx
    movl %edx, (%rcx)
    ret

/* UINT32 section_create(UINT64 size, UINT32 options, UINT32 *section) */
    .globl section_create
section_create:
    movq %r8, HOME_R8(%rsp)
    movq %rcx, %r10
    movl $CALL_SECTION_CREATE, %eax
    syscall
    movq HOME_R8(%rsp), %rcx
    movl %edx, (%rcx)
    ret

/* UINT32 process_map(UINT32 process, UINT32 section, UINT64 offset,
                      UINT64 address, UINT64 size, UINT32 protection);
   the mapping's address, the call's result, is the address given. */
    .globl process_map
process_map:
    movq %rcx, %r10
    movl $CALL_PROCESS_MAP, %eax
    syscall
    ret

/* UINT32 handle_duplicate(UINT32 handle, UINT32 rights, UINT32 *duplicate) */
    .globl handle_duplicate
handle_duplicate:
    movq %r8, HOME_R8(%rsp)
    movq %rcx, %r10
    movl $CALL_HANDLE_DUPLICATE, %eax
    syscall
    movq HOME_R8(%rsp), %rcx
    movl %edx, (%rcx)
    ret

/* UINT32 port_create(UINT32 capacity, UINT32 *port) */
    .globl port_create
port_create:
    movq %rdx, HOME_RDX(%rsp)
    movq %rcx, %r10
    movl $CALL_PORT_CREATE, %eax
    syscall
    movq HOME_RDX(%rsp), %rcx
    movl %edx, (%rcx)
    ret

/* UINT32 port_wait(UINT32 port, UINT64 deadline, PORT_PACKET *packet) */
    .globl port_wait
port_wait:
    movq %rcx, %r10
    movl $CALL_PORT_WAIT, %eax
    syscall
    ret

/* UINT32 resource_create(UINT32 parent, UINT64 base, UINT64 size,
                          UINT32 *resource) */
    .globl resource_create
resource_create:
    movq %r9, HOME_R9(%rsp)
    movq %rcx, %r10
    movl $CALL_RESOURCE_CREATE, %eax
    syscall
    movq HOME_R9(%rsp), %rcx
    movl %edx, (%rcx)
    ret

/* UINT32 section_create_physical(UINT32 resource, UINT64 base,
                                  UINT64 size, UINT32 cache,
                                  UINT32 *section) */
    .globl section_create_physical
section_create_physical:
    movq %rcx, %r10
    movl $CALL_SECTION_CREATE_PHYSICAL, %eax
    syscall
    movq ARGUMENT_5(%rsp), %rcx
    movl %edx, (%rcx)
    ret

/* UINT32 interrupt_create(UINT32 resource, UINT64 source, UINT32 options,
                           UINT32 *interrupt, UINT64 *address,
                           UINT32 *data) */
    .globl interrupt_create
interrupt_create:
    movq %r9, HOME_R9(%rsp)
    movq %rcx, %r10
    movl $CALL_INTERRUPT_CREATE, %eax
    syscall
    movq HOME_R9(%rsp), %rcx
    movl %edx, (%rcx)
    movq ARGUMENT_5(%rsp), %rcx
    movq %r8, (%rcx)
    movq ARGUMENT_6(%rsp), %rcx
    movl %r9d, (%rcx)
    ret

/* UINT32 interrupt_bind(UINT32 interrupt, UINT32 port, UINT64 key) */
    .globl interrupt_bind
interrupt_bind:
    movq %rcx, %r10
    movl $CALL_INTERRUPT_BIND, %eax
    syscall
    ret

/* UINT32 interrupt_ack(UINT32 interrupt) */
    .globl interrupt_ack
interrupt_ack:
    movq %rcx, %r10
    movl $CALL_INTERRUPT_ACK, %eax
    syscall
    ret

/* UINT32 initiator_create(UINT32 resource, UINT32 requester,
                           UINT32 *initiator) */
    .globl initiator_create
initiator_create:
    movq %r8, HOME_R8(%rsp)
    movq %rcx, %r10
    movl $CALL_INITIATOR_CREATE, %eax
    syscall
    movq HOME_R8(%rsp), %rcx
    movl %edx, (%rcx)
    ret

/* UINT32 section_pin(UINT32 initiator, UINT32 section, UINT64 offset,
                      UINT64 size, UINT32 options, UINT64 *addresses,
                      UINT32 *pin) */
    .globl section_pin
section_pin:
    movq %rcx, %r10
    movl $CALL_SECTION_PIN, %eax
    syscall
    movq ARGUMENT_7(%rsp), %rcx
    movl %edx, (%rcx)
    ret

/* UINT32 pin_release(UINT32 pin) */
    .globl pin_release
pin_release:
    movq %rcx, %r10
    movl $CALL_PIN_RELEASE, %eax
    syscall
    ret

/* UINT32 initiator_release_quarantine(UINT32 initiator) */
    .globl initiator_release_quarantine
initiator_release_quarantine:
    movq %rcx, %r10
    movl $CALL_INITIATOR_RELEASE_QUARANTINE, %eax
    syscall
    ret
