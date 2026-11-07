/* The nova-os kernel's entry. nova-boot calls kernel_main after
   ExitBootServices, so the firmware is gone: the kernel owns the machine.
   It still runs on the loader's stack and the firmware's identity-mapped
   page tables, which it will replace with its own.

   It turns interrupts off, since the firmware's handlers sit in memory the
   kernel now owns; reports itself and the memory it was handed on the
   first serial port; and keeps the boot screen's spinner turning, timed by
   the timestamp counter at the rate nova-boot measured. */

#include "boot_info.h"
#include "efi.h"

#define COM1 0x3f8
#define LINE_STATUS 5
#define TRANSMIT_EMPTY 0x20
#define FRAME_MS 33

static void outb(UINT16 port, UINT8 value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "d"(port));
}

static UINT8 inb(UINT16 port)
{
    UINT8 value;

    __asm__ volatile("inb %1, %0" : "=a"(value) : "d"(port));
    return value;
}

static UINT64 rdtsc(void)
{
    UINT32 lo, hi;

    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (UINT64)hi << 32 | lo;
}

/* COM1 as the firmware left it configured. */
static void serial_write(const char *s)
{
    for (; *s; s++) {
        while (!(inb(COM1 + LINE_STATUS) & TRANSMIT_EMPTY))
            ;
        outb(COM1, (UINT8)*s);
    }
}

static void serial_number(UINT64 n)
{
    char digits[21];
    int i = 20;

    digits[i] = 0;
    do {
        digits[--i] = (char)('0' + n % 10);
        n /= 10;
    } while (n);
    serial_write(digits + i);
}

/* Pages the kernel owns now that boot services have exited: boot-services
   code and data, and conventional memory, as UEFI 2.10's note on memory
   after ExitBootServices lists them. */
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

    __asm__ volatile("cli");
    serial_write("nova: kernel running, boot services exited\r\n");
    serial_write("nova: ");
    serial_number(usable_pages(info) * EFI_PAGE_SIZE / (1024 * 1024));
    serial_write(" MiB of memory usable, screen ");
    serial_number(info->screen.width);
    serial_write("x");
    serial_number(info->screen.height);
    serial_write("\r\n");

    next = rdtsc();
    for (;;) {
        gfx_spinner_step(&info->screen, &info->spinner);
        next += FRAME_MS * info->tsc_per_ms;
        while (rdtsc() < next)
            ;
    }
}
