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
   registers S4 preserves and leaves by SYSRETQ.

   Both carry the speculative-execution measures of
   docs/spec/speculation.md M3 in the order M3 gives, each a measure site
   (x86_64.h, MEASURE) skipped where the processor does not need it. The
   entry and exit lie in the syscall text, which page-table isolation
   maps in every user table (M11), since the processor runs it before the
   entry's CR3 load and after the exit's; user_copy lies outside it. RET
   and JMP are followed by INT3 (M17). */

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

/* Steps 2 and 3 of S3's entry, which run at CPL 0 on the user's stack
   with the user's GS base until the SWAPGS, and with interrupts disabled
   throughout, as IA32_FMASK leaves them. Under page-table isolation the
   kernel's table is loaded between them (M3, step 1), with RSP, saved
   in the per-CPU block, the one register free; the block lies in the
   entry area, which the user table maps. The SWAPGS is unconditional, so
   no fence follows it (M5). */
#define SWITCH_TO_KERNEL_STACK                               \
    SWAPGS;                                                  \
    movq %rsp, %gs:PER_CPU_USER_RSP;                         \
    MEASURE(MEASURE_ISOLATION);                              \
    movq %gs:PER_CPU_KERNEL_CR3, %rsp;                       \
    movq %rsp, %cr3;                                         \
    MEASURE_END;                                             \
    movq %gs:PER_CPU_KERNEL_STACK, %rsp

    MEASURE_TABLE(syscall_measures)

    .text
    .balign PAGE_SIZE, 0xcc
    .globl syscall_text
syscall_text:

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
    USER_ENTRY_MEASURES
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
    MEASURE(MEASURE_IBRS)
    WRITE_SPEC_CTRL(spec_ctrl_user)
    MEASURE_END
    /* The user's RSP waits in the per-CPU block, which the user table
       maps, for the step that loads it after the CR3 load. */
    movq FRAME_RSP(%rsp), %rcx
    movq %rcx, %gs:PER_CPU_USER_RSP
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
    /* The last access to the kernel stack is behind; RSP is the one
       register free for the CR3 load (M3, exit step 2). */
    MEASURE(MEASURE_ISOLATION)
    LOAD_USER_CR3(%rsp)
    MEASURE_END
    movq %gs:PER_CPU_USER_RSP, %rsp
    SWAPGS
    /* M13: VERW last, once every general register holds the user's value
       or zero. SYSRETQ loads RFLAGS from R11, so the ZF VERW writes never
       reaches user mode. */
    MEASURE(MEASURE_VERW)
    verw verw_selector(%rip)
    MEASURE_END
    sysretq
1:
    movq %rsp, %rcx
    subq $SHADOW_SPACE, %rsp
    call user_bad_return
    int3

/* SYSCALL from compatibility mode enters here (IA32_CSTAR) on AMD
   processors. A 32-bit thread enters by its own gate (S11), so this
   entry ends the calling process (S2), after the entry's measures, since
   it runs kernel code entered from user mode. The kernel stack's top is
   16-byte aligned, so RSP is aligned at the call. */
    .globl syscall_compat_entry
syscall_compat_entry:
    SWITCH_TO_KERNEL_STACK
    USER_ENTRY_MEASURES
    sti
    subq $SHADOW_SPACE, %rsp
    call syscall_compat
    int3

    .balign PAGE_SIZE, 0xcc
    .globl syscall_text_end
syscall_text_end:

    MEASURE_TABLE_END(syscall_measures_end)

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
    int3
    .globl user_copy_fault
user_copy_fault:
    movl $ERR_BAD_ADDRESS, %eax
    jmp 3b
    int3
