/* A new thread's first switch. arch_switch pops callee-saved registers
   and returns, so a new stack starts with the frame it pops, returning
   into the thread's start function as if that had just been called. */

#include <stddef.h>

#include "context.h"

_Static_assert(offsetof(ARCH_CONTEXT, rsp) == CONTEXT_RSP,
               "switch.S finds RSP at CONTEXT_RSP");

/* What switch.S pops, lowest address first, then the address its RET
   continues at. */
typedef struct {
    UINT64 r15, r14, r13, r12, rsi, rdi, rbp, rbx;
    UINT64 rip;
} SWITCH_FRAME;

/* A called function finds its return address at RSP, its caller's
   32-byte home area for the four register arguments above that, and RSP
   16-byte aligned before the call pushed the return address (x64 calling
   convention; x64 stack usage). */
#define SHADOW_SPACE 32
#define RETURN_ADDRESS_SIZE 8

void arch_context_init(ARCH_CONTEXT *context, UINT64 stack_top,
                       void (*start)(void))
{
    UINT64 entry_rsp = stack_top - SHADOW_SPACE - RETURN_ADDRESS_SIZE;
    SWITCH_FRAME *frame = (SWITCH_FRAME *)entry_rsp - 1;

    /* start never returns. A return address of 0 ends the frame chain
       that RBP 0 starts, and returning to it would fault, since the
       lower half maps nothing. */
    *(UINT64 *)entry_rsp = 0;
    frame->r15 = 0;
    frame->r14 = 0;
    frame->r13 = 0;
    frame->r12 = 0;
    frame->rsi = 0;
    frame->rdi = 0;
    frame->rbp = 0;
    frame->rbx = 0;
    frame->rip = (UINT64)start;
    context->rsp = (UINT64)frame;
}
