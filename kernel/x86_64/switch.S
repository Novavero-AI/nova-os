/* arch_switch(from, to), with from in RCX and to in RDX (Microsoft x64
   convention). The processor's task switch does not exist in 64-bit mode,
   so threads are switched in software (SDM Vol. 3A 10.7).

   A kernel thread is switched as a call: the registers the convention has
   a callee preserve, RBX, RBP, RDI, RSI and R12 to R15 ("Caller/callee
   saved registers"), go on the outgoing thread's stack and RSP into from;
   then the incoming thread's RSP comes from to, its registers come off
   its stack, and RET continues it after its own call to arch_switch, or
   in the start function arch_context_init gave a new thread. The other
   general registers are volatile across any call, so no caller expects
   them kept. RFLAGS is not kept either: every switch is made with
   interrupts disabled and DF clear, and each thread sets IF again itself
   as it leaves the code that switched: by IRETQ from the timer interrupt,
   restore_interrupts after a wait, the STI of wait_for_interrupt in the
   idle loop, or, for a new thread, thread_run.

   The convention also makes XMM6 to XMM15, the x87 control word and
   MXCSR's control bits nonvolatile (same page, "FPCSR", "MXCSR"). Kernel
   code executes no x87, SSE or AVX instruction
   (docs/spec/boot-protocol.md, Flags and floating point; syscalls.md S4),
   so those registers hold the same values in every kernel thread and the
   switch keeps them by leaving them alone.

   A user thread will add two things here. First, its x87, SSE and AVX
   state, which S4 has the kernel preserve: XSAVE of the outgoing thread's
   state into an area its ARCH_CONTEXT points to and XRSTOR of the
   incoming thread's, the save and restore on every switch that SDM Vol.
   3A 16.4 describes for preemptive systems. Second, the incoming thread's
   kernel stack top in the TSS's RSP0, which the processor loads when an
   interrupt arrives from user mode (SDM Vol. 3A 7.14.4, 10.7). Its
   general registers need no room here: its entry pushes them on its
   kernel stack (S3), where they stay while it is switched out. Neither is
   needed yet, because no thread runs user code: no thread has vector
   state of its own, and no interrupt changes privilege level. */

#include "context.h"

    .text
    .globl arch_switch
arch_switch:
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
