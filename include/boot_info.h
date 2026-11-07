/* What nova-boot hands the kernel at its entry, kernel_main(BOOT_INFO *).
   The record lives in nova-boot's image, which the memory map marks as
   loader data, so it survives ExitBootServices. */

#ifndef NOVA_BOOT_INFO_H
#define NOVA_BOOT_INFO_H

#include "types.h"
#include "gfx.h"

typedef struct {
    SCREEN screen;
    SPINNER spinner;
    UINT64 tsc_per_ms; /* timestamp-counter ticks per millisecond */
    UINT8 *memory_map; /* UEFI memory descriptors, descriptor_size apart */
    UINTN memory_map_size;
    UINTN descriptor_size;
} BOOT_INFO;

#endif
