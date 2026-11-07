/* The nova-os kernel's entry. nova-boot calls kernel_main after
   ExitBootServices, with interrupts disabled, so the firmware is gone and
   the kernel owns the machine. It still runs on the firmware's
   identity-mapped page tables.

   It moves from the firmware's stack, which is boot-services memory, to
   one of its own; installs its own descriptor tables, interrupt handlers
   and timer, so that of the firmware's structures only the page tables
   are still in use; builds the physical page allocator, checks it and
   reports on the first serial port; and keeps the boot screen's spinner
   turning on the timer's ticks. */

#include "boot_info.h"
#include "mm/page.h"
#include "x86_64/arch.h"
#include "x86_64/cpu.h"
#include "x86_64/serial.h"

#define FRAME_MS 33
#define FRAME_TICKS (FRAME_MS * TIMER_HZ / 1000)
#define BYTES_PER_MIB (1024 * 1024)

/* Everything the kernel runs runs on this stack, which nothing guards
   from overflowing, so it errs large. */
#define KERNEL_STACK_PAGES 16

/* The callee's home slots for its four register arguments, just above
   its return address: TinyCC's callers always reserve them (gfunc_call
   in x86_64-gen.c) and its prologue stores the register parameters a
   function declares into them (gfunc_prolog). */
#define SHADOW_SPACE 32

static void serial_mib(UINT64 pages)
{
    serial_number(pages * PAGE_SIZE / BYTES_PER_MIB);
    serial_write(" MiB");
}

static void report_memory(const BOOT_INFO *info, UINT64 stack)
{
    const char *failure;

    serial_write("nova: memory ");
    serial_mib(page_free_count());
    serial_write(" free of ");
    serial_mib(page_total_count());
    serial_write("\r\n");

    failure = page_self_test(info, stack, KERNEL_STACK_PAGES);
    if (failure) {
        serial_write("nova: page allocator self-test failed: ");
        serial_write(failure);
    } else {
        serial_write("nova: page allocator self-test passed");
    }
    serial_write("\r\n");
}

/* The kernel past its entry, on the stack at [stack, stack +
   KERNEL_STACK_PAGES pages). The firmware's IDT and exception handlers
   are boot-services memory, so the kernel's own replace them before the
   page allocator can hand that memory out. */
_Noreturn static VOID kernel_run(BOOT_INFO *info, UINT64 stack)
{
    UINT64 next;

    serial_write("nova: screen ");
    serial_number(info->screen.width);
    serial_write("x");
    serial_number(info->screen.height);
    serial_write("\r\n");
    arch_init(info->tsc_per_ms);
    if (!page_init(info, stack, KERNEL_STACK_PAGES)) {
        serial_write("nova: no memory for the page allocator\r\n");
        halt_forever();
    }
    report_memory(info, stack);

    next = timer_ticks();
    for (;;) {
        gfx_spinner_step(&info->screen, &info->spinner);
        next += FRAME_TICKS;
        timer_sleep_until(next);
    }
}

/* Call kernel_run on the new stack. rsp starts SHADOW_SPACE below the
   top, which is page-aligned, so the call is made with rsp 16-byte
   aligned (UEFI 2.11, 2.3.4.2) and the home slots lie inside the stack.
   rbp is cleared so that the frame chain ends here, not in the old stack,
   which is free memory from here on. */
static void switch_stack(BOOT_INFO *info, UINT64 stack)
{
    UINT64 top = stack + KERNEL_STACK_PAGES * PAGE_SIZE - SHADOW_SPACE;

    __asm__ volatile("mov %0, %%rsp\n\t"
                     "xor %%ebp, %%ebp\n\t"
                     "call *%1"
                     :
                     : "r"(top), "a"(kernel_run), "c"(info), "d"(stack)
                     : "memory");
}

VOID kernel_main(BOOT_INFO *info)
{
    UINT64 stack;

    serial_write("nova: kernel running, boot services exited\r\n");
    stack = page_find_early(info, KERNEL_STACK_PAGES);
    if (!stack) {
        serial_write("nova: no memory for the kernel stack\r\n");
        halt_forever();
    }
    switch_stack(info, stack);
}
