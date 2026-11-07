/* The system-call entry and exit (docs/spec/syscalls.md S3 to S5), and
   the routine that copies between kernel and user memory (S6).

   SYSCALL saves the return address in RCX and RFLAGS in R11, clears the
   IA32_FMASK flags, IF among them, loads the kernel's CS and SS and
   jumps to IA32_LSTAR, leaving RSP the user's (SDM Vol. 3A 6.8.8). The
   entry swaps GS to the per-CPU block, moves to the thread's kernel
   stack and pushes an INTERRUPT_FRAME (x86_64.h) like the one an
   interrupt from user mode leaves, then enables interrupts and calls
   syscall_dispatch, which puts the status and results into the frame.
   The exit disables interrupts, checks the return address, restores the
   registers S4 preserves and leaves by SYSRETQ. */

#include "x86_64.h"
#include "native_abi.h"

#define SHADOW_SPACE 32

/* SWAPGS (SDM Vol. 2B, SWAPGS: 0F 01 F8) and STAC (Vol. 2B, STAC: 0F 01
   CB), which TinyCC's assembler lacks. */
#define SWAPGS .byte 0x0f, 0x01, 0xf8
#define STAC .byte 0x0f, 0x01, 0xcb

/* The RFLAGS a thread returns to (S5): CF, PF, AF, ZF, SF, TF, DF, OF,
   AC and ID as SYSCALL saved them, IF and the always-set bit 1 forced
   on, and IOPL, NT and RF clear (SDM Vol. 3A 2.3, Figure 2-5). */
#define USER_FLAGS_KEPT 0x240dd5
#define USER_FLAGS_SET 0x202

/* Offsets from the vector slot, where RSP is once the general registers
   are popped. */
#define POPPED_RIP (FRAME_RIP - FRAME_VECTOR)
#define POPPED_RFLAGS (FRAME_RFLAGS - FRAME_VECTOR)
#define POPPED_RSP (FRAME_RSP - FRAME_VECTOR)

/* Steps 2 and 3 of S3's entry, which run at CPL 0 on the user's stack
   with the user's GS base until the SWAPGS, and with interrupts disabled
   throughout, as IA32_FMASK leaves them. */
#define SWITCH_TO_KERNEL_STACK                               \
    SWAPGS;                                                  \
    movq %rsp, %gs:PER_CPU_USER_RSP;                         \
    movq %gs:PER_CPU_KERNEL_STACK, %rsp

    .text
    .globl syscall_entry
syscall_entry:
    SWITCH_TO_KERNEL_STACK
    pushq $USER_DATA_SELECTOR
    pushq %gs:PER_CPU_USER_RSP
    pushq %r11
    pushq $USER_CODE_SELECTOR
    pushq %rcx
    pushq $0
    pushq $SYSCALL_FRAME_VECTOR
    pushq %rax
    pushq %rbx
    pushq %rcx
    pushq %rdx
    pushq %rsi
    pushq %rdi
    pushq %rbp
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    sti
    movq %rsp, %rcx
    subq $SHADOW_SPACE, %rsp
    call syscall_dispatch
    addq $SHADOW_SPACE, %rsp
    cli
    /* S5: SYSRETQ with a non-canonical RCX raises #GP at CPL 0 on the
       user's stack (SDM Vol. 3A 6.8.8). A return address above USER_TOP
       can only come from a kernel bug, and ends the thread's process. */
    movq FRAME_RIP(%rsp), %rcx
    cmpq user_top(%rip), %rcx
    ja 1f
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    /* R11 and RCX are loaded below, from the flags and return address. */
    addq $8, %rsp
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    addq $8, %rsp
    popq %rbx
    popq %rax
    movq POPPED_RIP(%rsp), %rcx
    movq POPPED_RFLAGS(%rsp), %r11
    andq $USER_FLAGS_KEPT, %r11
    orq $USER_FLAGS_SET, %r11
    movq POPPED_RSP(%rsp), %rsp
    SWAPGS
    sysretq
1:
    movq %rsp, %rcx
    subq $SHADOW_SPACE, %rsp
    call user_bad_return

/* SYSCALL from compatibility mode enters here (IA32_CSTAR). No user code
   segment is 32-bit, so user code cannot reach compatibility mode; if it
   ever does, the calling thread ends (S2). The kernel stack's top is
   16-byte aligned, so RSP is aligned at the call. */
    .globl syscall_compat_entry
syscall_compat_entry:
    SWITCH_TO_KERNEL_STACK
    sti
    subq $SHADOW_SPACE, %rsp
    call syscall_compat

/* STATUS user_copy(void *to, const void *from, UINT64 size), arguments
   in RCX, RDX and R8: copy size bytes, one of the two ranges in user
   memory and already checked to lie in it (S6). Returns OK, or
   ERR_BAD_ADDRESS if the access faults: the page-fault handler then
   continues at user_copy_fault (user_copy_fixup). With SMAP on,
   RFLAGS.AC, which lets the kernel reach user pages (SDM Vol. 3A 5.6.1),
   is set only inside this routine: interrupts stay disabled throughout,
   the page fault its access may raise clears AC on entry (entry.S) and
   IRETQ sets it again, and POPFQ clears it with the caller's flags. */
    .globl user_copy
user_copy:
    pushfq
    cli
    pushq %rdi
    pushq %rsi
    movq %rcx, %rdi
    movq %rdx, %rsi
    movq %r8, %rcx
    cmpb $0, smap_enabled(%rip)
    je 2f
    STAC
2:
    .globl user_copy_access
user_copy_access:
    rep movsb
    xorl %eax, %eax
3:
    popq %rsi
    popq %rdi
    popfq
    ret
    .globl user_copy_fault
user_copy_fault:
    movl $ERR_BAD_ADDRESS, %eax
    jmp 3b
