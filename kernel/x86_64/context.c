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
#include "speculation.h"
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
                            UINT64 address_space, UINT8 *fpu, USER_MODE mode,
                            UINT64 entry, UINT64 stack, UINT64 argument1,
                            UINT64 argument2)
{
    INTERRUPT_FRAME *frame = (INTERRUPT_FRAME *)stack_top - 1;
    UINT32 i;

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
    frame->cs = mode == USER_MODE_COMPATIBILITY ? USER_CODE32_SELECTOR
                                                : USER_CODE_SELECTOR;
    frame->rflags = USER_START_FLAGS;
    frame->rsp = stack;
    frame->ss = USER_DATA_SELECTOR;
    context->rsp =
        (UINT64)switch_frame_below((UINT64)frame, (UINT64)user_first_switch);
    context->kernel_stack_top = stack_top;
    context->address_space = address_space;
    context->fpu = fpu;
    context->fs_base = 0;
    for (i = 0; i < DATA_SEGMENTS; i++)
        context->data_selectors[i] = USER_DATA_SELECTOR;
}

void arch_set_tls_base(ARCH_CONTEXT *context, UINT64 base)
{
    context->fs_base = base;
    wrmsr(IA32_FS_BASE, base);
}

/* A user thread's data segment selectors, which it may change with MOV or
   POP. 64-bit mode ignores them, but compatibility mode uses them, and a
   null one faults on any access through it (SDM Vol. 3A 6.4.1, 6.4.1.1),
   so they are the thread's, not the processor's. A user thread could
   load each, so the kernel can load each back. Loading GS takes its base
   from the descriptor (SDM Vol. 3A 3.4.4), so the kernel's, the per-CPU
   block, is written back after. Memory operands keep TinyCC from
   choosing a register the convention has a callee preserve, which it
   would not save (cpu.h, cpuid). */
static void save_data_segments(UINT16 *selectors)
{
    __asm__ volatile("movw %%ds, %0" : "=m"(selectors[0]));
    __asm__ volatile("movw %%es, %0" : "=m"(selectors[1]));
    __asm__ volatile("movw %%fs, %0" : "=m"(selectors[2]));
    __asm__ volatile("movw %%gs, %0" : "=m"(selectors[3]));
}

static void load_data_segments(const UINT16 *selectors)
{
    UINT64 kernel_gs = rdmsr(IA32_GS_BASE);

    __asm__ volatile("movw %0, %%ds" : : "m"(selectors[0]) : "memory");
    __asm__ volatile("movw %0, %%es" : : "m"(selectors[1]) : "memory");
    __asm__ volatile("movw %0, %%fs" : : "m"(selectors[2]) : "memory");
    __asm__ volatile("movw %0, %%gs" : : "m"(selectors[3]) : "memory");
    wrmsr(IA32_GS_BASE, kernel_gs);
}

/* The outgoing thread's vector state, data segment selectors and FS base
   are saved and the incoming one's restored on every switch, the scheme
   SDM Vol. 3A 16.4 describes for preemptive systems; the FS base is read
   back because user code can change it by loading FS (S10), and is
   loaded after FS's selector, which would replace it. Kernel code
   executes no x87, SSE or AVX instruction (docs/spec/boot-protocol.md,
   Flags and floating point) and its 64-bit code reads no selector, so a
   kernel thread leaves them all as it found them.

   A switch to a user thread of another process than the last user
   thread this processor ran takes the speculation barrier before the new
   CR3 (docs/spec/speculation.md M3). Under page-table isolation RSP0
   stays at the entry stack, from which the entry moves the frame to the
   kernel stack in the per-CPU block (entry.S). Loading CR3 invalidates
   the lower half's translations (SDM Vol. 3A 5.10.4.1); no kernel entry
   is global, so the upper half's go too, and are refilled from the same
   tables. */
void arch_switch(ARCH_CONTEXT *from, ARCH_CONTEXT *to)
{
    if (from->fpu) {
        fpu_save(from->fpu);
        save_data_segments(from->data_selectors);
        from->fs_base = rdmsr(IA32_FS_BASE);
    }
    if (!paging_isolated())
        tss_set_kernel_stack(to->kernel_stack_top);
    per_cpu_block()->kernel_stack = to->kernel_stack_top;
    if (to->fpu && paging_enter_user_space(to->address_space))
        speculation_process_switch();
    if ((read_cr3() & PTE_ADDRESS) != to->address_space)
        write_cr3(to->address_space);
    if (to->fpu) {
        fpu_restore(to->fpu);
        load_data_segments(to->data_selectors);
        wrmsr(IA32_FS_BASE, to->fs_base);
    }
    switch_stacks(from, to);
}
