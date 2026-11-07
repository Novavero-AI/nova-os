/* Thread contexts. A new kernel thread's first switch returns into its
   start function: arch_switch pops callee-saved registers and returns,
   so a new stack starts with the frame it pops. A new user thread's
   first switch returns into user_first_switch (switch.S), which leaves
   for user mode through an INTERRUPT_FRAME at the top of the thread's
   kernel stack. The switch itself also moves the per-thread processor
   state that lives outside the stack: RSP0, the page tables and the
   user's vector state. */

#include <stddef.h>

#include "context.h"
#include "cpu.h"
#include "fpu.h"
#include "memory_layout.h"
#include "page_table.h"
#include "paging.h"
#include "x86_64.h"

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

/* The RFLAGS a user thread starts with: IF and the always-set bit 1
   (docs/spec/syscalls.md S8; SDM Vol. 3A 2.3). */
#define USER_START_FLAGS 0x202

/* switch.S. */
void switch_stacks(ARCH_CONTEXT *from, ARCH_CONTEXT *to);
void user_first_switch(void);

static SWITCH_FRAME *switch_frame_below(UINT64 address, UINT64 rip)
{
    SWITCH_FRAME *frame = (SWITCH_FRAME *)address - 1;

    frame->r15 = 0;
    frame->r14 = 0;
    frame->r13 = 0;
    frame->r12 = 0;
    frame->rsi = 0;
    frame->rdi = 0;
    frame->rbp = 0;
    frame->rbx = 0;
    frame->rip = rip;
    return frame;
}

void arch_context_init(ARCH_CONTEXT *context, UINT64 stack_top,
                       void (*start)(void))
{
    UINT64 entry_rsp = stack_top - SHADOW_SPACE - RETURN_ADDRESS_SIZE;

    /* start never returns. A return address of 0 ends the frame chain
       that RBP 0 starts, and returning to it would fault, since the
       kernel's tables map nothing in the lower half. */
    *(UINT64 *)entry_rsp = 0;
    context->rsp = (UINT64)switch_frame_below(entry_rsp, (UINT64)start);
    context->kernel_stack_top = stack_top;
    context->address_space = paging_kernel_space();
    context->fpu = 0;
}

void arch_context_init_boot(ARCH_CONTEXT *context)
{
    context->rsp = 0;
    context->kernel_stack_top = KERNEL_STACK_TOP;
    context->address_space = paging_kernel_space();
    context->fpu = 0;
}

/* The frame lies at the very top of the kernel stack, where an interrupt
   or system call from the thread's user code will build its own. */
void arch_context_init_user(ARCH_CONTEXT *context, UINT64 stack_top,
                            UINT64 address_space, UINT8 *fpu, UINT64 entry,
                            UINT64 stack, UINT64 argument1, UINT64 argument2)
{
    INTERRUPT_FRAME *frame = (INTERRUPT_FRAME *)stack_top - 1;

    frame->r15 = 0;
    frame->r14 = 0;
    frame->r13 = 0;
    frame->r12 = 0;
    frame->r11 = 0;
    frame->r10 = 0;
    frame->r9 = 0;
    frame->r8 = 0;
    frame->rbp = 0;
    frame->rdi = 0;
    frame->rsi = 0;
    frame->rdx = argument2;
    frame->rcx = argument1;
    frame->rbx = 0;
    frame->rax = 0;
    frame->vector = 0;
    frame->error_code = 0;
    frame->rip = entry;
    frame->cs = USER_CODE_SELECTOR;
    frame->rflags = USER_START_FLAGS;
    frame->rsp = stack;
    frame->ss = USER_DATA_SELECTOR;
    context->rsp =
        (UINT64)switch_frame_below((UINT64)frame, (UINT64)user_first_switch);
    context->kernel_stack_top = stack_top;
    context->address_space = address_space;
    context->fpu = fpu;
}

/* The outgoing thread's vector state is saved and the incoming one's
   restored on every switch, the scheme SDM Vol. 3A 16.4 describes for
   preemptive systems. Kernel code executes no x87, SSE or AVX
   instruction (docs/spec/boot-protocol.md, Flags and floating point), so
   a kernel thread leaves the registers as it found them. Loading CR3
   invalidates the lower half's translations (SDM Vol. 3A 5.10.4.1); no
   kernel entry is global, so the upper half's go too, and are refilled
   from the same tables. */
void arch_switch(ARCH_CONTEXT *from, ARCH_CONTEXT *to)
{
    if (from->fpu)
        fpu_save(from->fpu);
    tss_set_kernel_stack(to->kernel_stack_top);
    per_cpu_set_kernel_stack(to->kernel_stack_top);
    if ((read_cr3() & PTE_ADDRESS) != to->address_space)
        write_cr3(to->address_space);
    if (to->fpu)
        fpu_restore(to->fpu);
    switch_stacks(from, to);
}
