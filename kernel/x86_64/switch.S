/* switch_stacks(from, to), with from in RCX and to in RDX (Microsoft x64
   convention): the part of arch_switch (context.c) that moves from one
   kernel stack to another. The processor's task switch does not exist in
   64-bit mode, so threads are switched in software (SDM Vol. 3A 10.7).

   A thread is switched as a call: the registers the convention has a
   callee preserve, RBX, RBP, RDI, RSI and R12 to R15 ("Caller/callee
   saved registers"), go on the outgoing thread's stack and RSP into from;
   then the incoming thread's RSP comes from to, its registers come off
   its stack, and RET continues it after its own call to switch_stacks,
   or where arch_context_init or arch_context_init_user started a new
   thread. The other general registers are volatile across any call, so
   no caller expects them kept. RFLAGS is not kept either: every switch is
   made with interrupts disabled and DF clear, and each thread sets IF
   again itself as it leaves the code that switched: by IRETQ from the
   timer interrupt or into user mode, restore_interrupts after a wait,
   the STI of wait_for_interrupt in the idle loop, or, for a new kernel
   thread, thread_run.

   The convention also makes XMM6 to XMM15, the x87 control word and
   MXCSR's control bits nonvolatile (same page, "FPCSR", "MXCSR"). Kernel
   code executes no x87, SSE or AVX instruction
   (docs/spec/boot-protocol.md, Flags and floating point; syscalls.md S4),
   so those registers hold the user state of the last user thread that
   ran, which arch_switch saves and restores around this. A user thread's
   general registers need no room here: its entry to the kernel pushed
   them on its kernel stack (entry.S, syscall.S), where they stay while
   it is switched out.

   RET and JMP are followed by INT3 (docs/spec/speculation.md M17). */

#include "context.h"
#include "x86_64.h"

#define SHADOW_SPACE 32

    .text
    .globl switch_stacks
switch_stacks:
    pushq %rbx
    pushq %rbp
    pushq %rdi
    pushq %rsi
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq %rsp, CONTEXT_RSP(%rcx)
    movq CONTEXT_RSP(%rdx), %rsp
    popq %r15
    popq %r14
    popq %r13
    popq %r12
    popq %rsi
    popq %rdi
    popq %rbp
    popq %rbx
    ret
    int3

/* Where a new user thread's first switch returns, with RSP at the
   INTERRUPT_FRAME arch_context_init_user placed at the top of its kernel
   stack, 16-byte aligned, and interrupts disabled. */
    .globl user_first_switch
user_first_switch:
    subq $SHADOW_SPACE, %rsp
    call thread_user_first_run
    addq $SHADOW_SPACE, %rsp
    jmp return_to_user
    int3

/* void rsb_overwrite(void): the 32-CALL overwrite that a process switch
   on some AMD processors makes after its IBPB (M7, rule 3), so that no
   return address the last process left predicts a later RET. Its own
   RET is predicted to one of the overwrite's INT3s, where that
   misprediction ends, and returns to the caller. */
    .globl rsb_overwrite
rsb_overwrite:
    RSB_OVERWRITE
    ret
    int3
