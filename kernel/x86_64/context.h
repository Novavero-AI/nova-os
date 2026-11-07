/* A thread's processor context on x86-64 and the switch from one thread
   to another. switch.S includes this file too, for the field offsets; the
   C part is hidden from it. */

#ifndef NOVA_CONTEXT_H
#define NOVA_CONTEXT_H

/* ARCH_CONTEXT's fields, by byte offset. */
#define CONTEXT_RSP 0

#ifndef __ASSEMBLER__

#include "types.h"

/* What a switched-out thread keeps outside its stack: the stack pointer,
   at which switch.S has pushed the rest; the top of its kernel stack,
   where the processor and the system-call entry switch to when user code
   enters the kernel; the physical address of the top page table it runs
   on; and, for a thread that runs user code, the area its x87, SSE and
   AVX state is kept in while it is switched out (fpu.c), or 0 for a
   kernel thread, which has none of its own. */
typedef struct {
    UINT64 rsp;
    UINT64 kernel_stack_top;
    UINT64 address_space;
    UINT8 *fpu;
} ARCH_CONTEXT;

/* Prepare context so that the first switch to it calls start on a fresh
   kernel stack that ends at stack_top, a 16-byte boundary, on the
   kernel's own page tables. start must not return. */
void arch_context_init(ARCH_CONTEXT *context, UINT64 stack_top,
                       void (*start)(void));

/* Describe the running code, the boot thread, as a context: it runs on
   the stack nova-boot gave the kernel and on the kernel's tables. */
void arch_context_init_boot(ARCH_CONTEXT *context);

/* Prepare context so that the first switch to it enters user mode at
   entry with RSP stack, RCX argument1 and RDX argument2, every other
   general register zero and RFLAGS 0x202 (docs/spec/syscalls.md S8),
   on the page tables at address_space, with its user state in fpu, which
   fpu_state_init has prepared. Its kernel stack ends at stack_top. The
   first switch calls thread_user_first_run before it leaves the kernel. */
void arch_context_init_user(ARCH_CONTEXT *context, UINT64 stack_top,
                            UINT64 address_space, UINT8 *fpu, UINT64 entry,
                            UINT64 stack, UINT64 argument1,
                            UINT64 argument2);

/* Keep the running thread's context in from and continue the thread whose
   context is in to. Called with interrupts disabled; it returns, with
   interrupts still disabled, when a later switch continues from. */
void arch_switch(ARCH_CONTEXT *from, ARCH_CONTEXT *to);

#endif
#endif
