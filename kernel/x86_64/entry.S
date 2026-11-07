/* Interrupt entry. Each installed vector's gate points at a stub here that
   pushes a zero where the CPU pushes no error code, then the vector, and
   joins interrupt_common, so every interrupt reaches interrupt_dispatch
   with the same INTERRUPT_FRAME (x86_64.h). The frame goes on the
   interrupted code's stack, which is safe because the Microsoft x64
   convention keeps no data below RSP; a double fault or page fault
   switches to a stack of its own first (gdt.c). TinyCC runs the C
   preprocessor over this file and assembles it as AT&T syntax, where ';'
   separates statements; that lets one macro write a whole stub. */

#include "x86_64.h"

/* The Microsoft x64 convention: the caller reserves 32 bytes above the
   return address for the callee's four register arguments. */
#define SHADOW_SPACE 32
#define VECTOR_AND_ERROR_CODE 16

/* An exception's stub. TinyCC's assembler has no .if, so .rept, repeating
   zero times or once, pushes the placeholder for vectors outside
   ERROR_CODE_VECTORS. */
#define EXCEPTION(vector)                                    \
    exception_##vector:                                      \
    .rept 1 - ((ERROR_CODE_VECTORS >> (vector)) & 1);        \
    pushq $0;                                                \
    .endr;                                                   \
    pushq $(vector);                                         \
    jmp interrupt_common

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
EXCEPTION(2)
EXCEPTION(3)
EXCEPTION(4)
EXCEPTION(5)
EXCEPTION(6)
EXCEPTION(7)
EXCEPTION(8)
EXCEPTION(9)
EXCEPTION(10)
EXCEPTION(11)
EXCEPTION(12)
EXCEPTION(13)
EXCEPTION(14)
EXCEPTION(15)
EXCEPTION(16)
EXCEPTION(17)
EXCEPTION(18)
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

/* The CPU aligns RSP to 16 bytes before it pushes its frame (SDM Vol. 3A
   7.14.2), and INTERRUPT_FRAME is a multiple of 16 bytes, so RSP is
   aligned at the call, as the convention requires. */
interrupt_common:
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
    /* The convention expects the direction flag clear, and the
       interrupted code may have set it; IRETQ restores its RFLAGS. */
    cld
    movq %rsp, %rcx
    subq $SHADOW_SPACE, %rsp
    call interrupt_dispatch
    addq $SHADOW_SPACE, %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %r11
    popq %r10
    popq %r9
    popq %r8
    popq %rbp
    popq %rdi
    popq %rsi
    popq %rdx
    popq %rcx
    popq %rbx
    popq %rax
    addq $VECTOR_AND_ERROR_CODE, %rsp
    iretq

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
