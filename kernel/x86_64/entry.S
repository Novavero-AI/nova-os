/* Interrupt entry. Each installed vector's gate points at a stub here that
   pushes a zero where the CPU pushes no error code, then the vector, and
   joins interrupt_common, so every interrupt reaches interrupt_dispatch
   with the same INTERRUPT_FRAME (x86_64.h). From kernel mode the frame
   goes on the interrupted code's stack, which is safe because the
   Microsoft x64 convention keeps no data below RSP; from user mode the
   processor has switched to the thread's kernel stack from the TSS's
   RSP0 first (SDM Vol. 3A 7.14.4). NMI, double fault and machine check
   switch to stacks of their own (gdt.c) and join ist_common instead.
   TinyCC runs the C preprocessor over this file and assembles it as AT&T
   syntax, where ';' separates statements; that lets one macro write a
   whole stub. */

#include "x86_64.h"
#include "native_abi.h"

/* The Microsoft x64 convention: the caller reserves 32 bytes above the
   return address for the callee's four register arguments. */
#define SHADOW_SPACE 32
#define VECTOR_AND_ERROR_CODE 16

/* Where the interrupted CS lies on entry to interrupt_common, above the
   vector, the error code and RIP. */
#define ENTRY_CS 24

/* SWAPGS (SDM Vol. 2B, SWAPGS: 0F 01 F8) and CLAC (Vol. 2A, CLAC: 0F 01
   CA), which TinyCC's assembler lacks. */
#define SWAPGS .byte 0x0f, 0x01, 0xf8
#define CLAC .byte 0x0f, 0x01, 0xca

#define PUSH_REGISTERS                                       \
    pushq %rax; pushq %rbx; pushq %rcx; pushq %rdx;          \
    pushq %rsi; pushq %rdi; pushq %rbp; pushq %r8;           \
    pushq %r9; pushq %r10; pushq %r11; pushq %r12;           \
    pushq %r13; pushq %r14; pushq %r15

#define POP_REGISTERS                                        \
    popq %r15; popq %r14; popq %r13; popq %r12;              \
    popq %r11; popq %r10; popq %r9; popq %r8;                \
    popq %rbp; popq %rdi; popq %rsi; popq %rdx;              \
    popq %rcx; popq %rbx; popq %rax

/* An exception's stub. TinyCC's assembler has no .if, so .rept, repeating
   zero times or once, pushes the placeholder for vectors outside
   ERROR_CODE_VECTORS. */
#define EXCEPTION_TO(vector, common)                         \
    exception_##vector:                                      \
    .rept 1 - ((ERROR_CODE_VECTORS >> (vector)) & 1);        \
    pushq $0;                                                \
    .endr;                                                   \
    pushq $(vector);                                         \
    jmp common

#define EXCEPTION(vector) EXCEPTION_TO(vector, interrupt_common)
#define EXCEPTION_IST(vector) EXCEPTION_TO(vector, ist_common)

/* An external interrupt's stub; the CPU pushes no error code for one. */
#define INTERRUPT(name, vector)                              \
    .globl name;                                             \
    name:                                                    \
    pushq $0;                                                \
    pushq $(vector);                                         \
    jmp interrupt_common

    .text
EXCEPTION(0)
EXCEPTION(1)
EXCEPTION_IST(2)
EXCEPTION(3)
EXCEPTION(4)
EXCEPTION(5)
EXCEPTION(6)
EXCEPTION(7)
EXCEPTION_IST(8)
EXCEPTION(9)
EXCEPTION(10)
EXCEPTION(11)
EXCEPTION(12)
EXCEPTION(13)
EXCEPTION(14)
EXCEPTION(15)
EXCEPTION(16)
EXCEPTION(17)
EXCEPTION_IST(18)
EXCEPTION(19)
EXCEPTION(20)
EXCEPTION(21)
EXCEPTION(22)
EXCEPTION(23)
EXCEPTION(24)
EXCEPTION(25)
EXCEPTION(26)
EXCEPTION(27)
EXCEPTION(28)
EXCEPTION(29)
EXCEPTION(30)
EXCEPTION(31)
INTERRUPT(timer_stub, TIMER_VECTOR)
INTERRUPT(spurious_stub, SPURIOUS_VECTOR)
/* INT n pushes no error code (SDM Vol. 3A 7.13), so the 32-bit lane's gate
   takes the external interrupt's stub. */
INTERRUPT(compat_gate_stub, COMPAT_GATE_VECTOR)

/* The CPU aligns RSP to 16 bytes before it pushes its frame (SDM Vol. 3A
   7.14.2), and INTERRUPT_FRAME is a multiple of 16 bytes, so RSP is
   aligned at the call, as the convention requires.

   An interrupt from user mode, whose saved CS has RPL 3, arrives with the
   user's GS base and the per-CPU block in IA32_KERNEL_GS_BASE; SWAPGS
   exchanges them, and the return to user mode exchanges them back
   (docs/spec/syscalls.md S3). From kernel mode GS is already the
   kernel's: the only kernel code that runs with the user's GS base is
   the system-call entry and exit, with interrupts disabled.

   Delivery clears TF, NT, RF and IF but not AC (SDM Vol. 3A 7.12.1.3),
   which user code sets with POPF and user_copy sets on purpose. With
   SMAP on, AC set would let the handler, and any thread the scheduler
   switches to from it, reach user pages (S6), so every entry clears it;
   IRETQ restores the interrupted code's flags. CLAC raises #UD where the
   processor has no SMAP, and AC then has no effect at CPL 0. */
interrupt_common:
    testb $SELECTOR_RPL_MASK, ENTRY_CS(%rsp)
    jz 1f
    SWAPGS
1:
    PUSH_REGISTERS
    cmpb $0, smap_enabled(%rip)
    je 2f
    CLAC
2:
    /* The convention expects the direction flag clear, and the
       interrupted code may have set it; IRETQ restores its RFLAGS. */
    cld
    movq %rsp, %rcx
    subq $SHADOW_SPACE, %rsp
    call interrupt_dispatch
    addq $SHADOW_SPACE, %rsp
    testb $SELECTOR_RPL_MASK, FRAME_CS(%rsp)
    jnz return_to_user
    POP_REGISTERS
    addq $VECTOR_AND_ERROR_CODE, %rsp
    iretq

/* The return to user mode through an INTERRUPT_FRAME at RSP, with
   interrupts disabled: from an interrupt, and on a new user thread's
   first switch (switch.S). IRETQ faults on a non-canonical RIP at CPL 0
   with the user's GS base already swapped in (docs/spec/syscalls.md S5),
   so a return address above USER_TOP, which only a kernel bug can
   produce, ends the thread's process instead (user_bad_return). */
    .globl return_to_user
return_to_user:
    movq FRAME_RIP(%rsp), %rax
    cmpq user_top(%rip), %rax
    ja 3f
    POP_REGISTERS
    addq $VECTOR_AND_ERROR_CODE, %rsp
    SWAPGS
    iretq
3:
    movq %rsp, %rcx
    subq $SHADOW_SPACE, %rsp
    call user_bad_return

/* NMI, double fault and machine check: each can arrive inside the
   system-call entry or exit, at CPL 0 with the user's GS base, so these
   never execute SWAPGS and never use GS (S3). Each stops the kernel. */
ist_common:
    PUSH_REGISTERS
    cld
    movq %rsp, %rcx
    subq $SHADOW_SPACE, %rsp
    call exception_halt

    .data
    .balign 8
    .globl exception_stubs
exception_stubs:
    .quad exception_0, exception_1, exception_2, exception_3
    .quad exception_4, exception_5, exception_6, exception_7
    .quad exception_8, exception_9, exception_10, exception_11
    .quad exception_12, exception_13, exception_14, exception_15
    .quad exception_16, exception_17, exception_18, exception_19
    .quad exception_20, exception_21, exception_22, exception_23
    .quad exception_24, exception_25, exception_26, exception_27
    .quad exception_28, exception_29, exception_30, exception_31
