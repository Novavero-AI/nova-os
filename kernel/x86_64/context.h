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
   at which switch.S has pushed the rest. */
typedef struct {
    UINT64 rsp;
} ARCH_CONTEXT;

/* Prepare context so that the first switch to it calls start on a fresh
   stack that ends at stack_top, a 16-byte boundary. start must not
   return. */
void arch_context_init(ARCH_CONTEXT *context, UINT64 stack_top,
                       void (*start)(void));

/* Keep the running thread's context in from and continue the thread whose
   context is in to. Called with interrupts disabled; it returns, with
   interrupts still disabled, when a later switch continues from. */
void arch_switch(ARCH_CONTEXT *from, ARCH_CONTEXT *to);

#endif
#endif
