/* What nova-boot hands the kernel at its entry, kernel_main(BOOT_INFO *).
   The record lives in nova-boot's image, which the memory map types as
   loader code or data, so it survives ExitBootServices. Every pointer in
   it, and the pointer to it, is a direct-map address (memory_layout.h);
   docs/spec/boot-protocol.md gives the full contract. */

#ifndef NOVA_BOOT_INFO_H
#define NOVA_BOOT_INFO_H

#include "types.h"
#include "gfx.h"

/* "NOVABOOT" in memory order: the record is nova-boot's. */
#define BOOT_INFO_MAGIC 0x544F4F4241564F4EULL

/* The layout below and the machine state that comes with it. */
#define BOOT_INFO_VERSION 1

typedef struct {
    UINT64 magic;
    UINT64 version;
    SCREEN screen;
    SPINNER spinner;
    UINT64 tsc_per_ms; /* timestamp-counter ticks per millisecond */
    UINT8 *memory_map; /* UEFI memory descriptors, descriptor_size apart */
    UINTN memory_map_size;
    UINTN descriptor_size;
} BOOT_INFO;

#endif
