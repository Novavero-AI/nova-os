/* The nova-os kernel's entry. nova-boot calls kernel_main after
   ExitBootServices, with interrupts disabled, on the kernel's own page
   tables and stack (docs/spec/boot-protocol.md), so the firmware is gone
   and the kernel owns the machine.

   It checks that BOOT_INFO is the record it was built for; installs its
   own descriptor tables and exception handlers and removes the lower-half
   mapping nova-boot left for the switch; builds the physical page
   allocator, checks it and reports on the first serial port; starts the
   timer; and keeps the boot screen's spinner turning on the timer's
   ticks. */

#include "boot_info.h"
#include "cpu.h"
#include "mm/page.h"
#include "x86_64/arch.h"
#include "x86_64/serial.h"

#define FRAME_MS 33
#define FRAME_TICKS (FRAME_MS * TIMER_HZ / 1000)
#define BYTES_PER_MIB (1024 * 1024)

static void serial_mib(UINT64 pages)
{
    serial_number(pages * PAGE_SIZE / BYTES_PER_MIB);
    serial_write(" MiB");
}

static void report_memory(const BOOT_INFO *info)
{
    const char *failure;

    serial_write("nova: memory ");
    serial_mib(page_free_count());
    serial_write(" free of ");
    serial_mib(page_total_count());
    serial_write("\r\n");

    failure = page_self_test(info);
    if (failure) {
        serial_write("nova: page allocator self-test failed: ");
        serial_write(failure);
    } else {
        serial_write("nova: page allocator self-test passed");
    }
    serial_write("\r\n");
}

/* A record from another loader, or from a nova-boot with another layout,
   cannot be read past its first two fields. */
static void check_boot_info(const BOOT_INFO *info)
{
    if (info->magic == BOOT_INFO_MAGIC && info->version == BOOT_INFO_VERSION)
        return;
    serial_write("nova: boot info magic ");
    serial_hex(info->magic);
    serial_write(" version ");
    serial_number(info->version);
    serial_write(", expected version ");
    serial_number(BOOT_INFO_VERSION);
    serial_write(", halted\r\n");
    halt_forever();
}

VOID kernel_main(BOOT_INFO *info)
{
    UINT64 next;

    serial_write("nova: kernel running, boot services exited\r\n");
    check_boot_info(info);
    serial_write("nova: screen ");
    serial_number(info->screen.width);
    serial_write("x");
    serial_number(info->screen.height);
    serial_write("\r\n");
    arch_init();
    if (!page_init(info)) {
        serial_write("nova: no memory for the page allocator\r\n");
        halt_forever();
    }
    report_memory(info);
    arch_start_timer(info->tsc_per_ms);

    next = timer_ticks();
    for (;;) {
        gfx_spinner_step(&info->screen, &info->spinner);
        next += FRAME_TICKS;
        timer_sleep_until(next);
    }
}
