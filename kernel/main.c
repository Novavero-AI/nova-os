/* The nova-os kernel's entry. nova-boot calls kernel_main after
   ExitBootServices, so the firmware is gone: the kernel owns the machine.
   It still runs on the firmware's stack and identity-mapped page tables,
   which it will replace with its own.

   nova-boot calls it with interrupts disabled. It reports itself and the
   memory it was handed on the first serial port; installs its own descriptor tables and interrupt
   handlers; and keeps the boot screen's spinner turning on the Local APIC
   timer's ticks. */

#include "boot_info.h"
#include "efi.h"
#include "x86_64/arch.h"
#include "x86_64/serial.h"

#define FRAME_MS 33
#define FRAME_TICKS (FRAME_MS * TIMER_HZ / 1000)

/* Pages free for the kernel to allocate now that boot services have
   exited: boot-services code and data, and conventional memory (UEFI 2.11,
   Table 7.10). Loader code and data are the kernel's too, but hold the
   kernel, nova-boot's image with BOOT_INFO, and the memory map. */
static UINT64 usable_pages(const BOOT_INFO *info)
{
    UINT64 pages = 0;
    UINTN offset;

    for (offset = 0; offset + info->descriptor_size <= info->memory_map_size;
         offset += info->descriptor_size) {
        const EFI_MEMORY_DESCRIPTOR *d =
            (const EFI_MEMORY_DESCRIPTOR *)(info->memory_map + offset);

        if (d->Type == EFI_BOOT_SERVICES_CODE ||
            d->Type == EFI_BOOT_SERVICES_DATA ||
            d->Type == EFI_CONVENTIONAL_MEMORY)
            pages += d->NumberOfPages;
    }
    return pages;
}

VOID kernel_main(BOOT_INFO *info)
{
    UINT64 next;

    serial_write("nova: kernel running, boot services exited\r\n");
    serial_write("nova: ");
    serial_number(usable_pages(info) * EFI_PAGE_SIZE / (1024 * 1024));
    serial_write(" MiB of memory usable, screen ");
    serial_number(info->screen.width);
    serial_write("x");
    serial_number(info->screen.height);
    serial_write("\r\n");

    arch_init(info->tsc_per_ms);

    next = timer_ticks();
    for (;;) {
        gfx_spinner_step(&info->screen, &info->spinner);
        next += FRAME_TICKS;
        timer_sleep_until(next);
    }
}
