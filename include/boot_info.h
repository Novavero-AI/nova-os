/* What nova-boot hands the kernel at its entry, kernel_main(BOOT_INFO *).
   The record lives in nova-boot's image, which the memory map types as
   loader code or data, so it survives ExitBootServices. Every pointer in
   it, and the pointer to it, is a direct-map address (memory_layout.h);
   docs/spec/boot-protocol.md gives the full contract. */

#ifndef NOVA_BOOT_INFO_H
#define NOVA_BOOT_INFO_H

#include "types.h"
#include "gfx.h"
#include "native_abi.h"

/* "NOVABOOT" in memory order: the record is nova-boot's. */
#define BOOT_INFO_MAGIC 0x544F4F4241564F4EULL

/* The layout below and the machine state that comes with it. */
#define BOOT_INFO_VERSION 5

/* The EFI_RNG_PROTOCOL instances nova-boot asks for a seed, at most, and
   the bytes it asks each for: 256 bits, the security level UEFI 37.5
   sets for a deterministic generator over an entropy source, and the
   size of the kernel generator's key. OVMF installs an instance where
   the processor has RDRAND and one for each virtio-rng device, so four
   covers the first and three devices. */
#define BOOT_RANDOM_SEEDS 4
#define BOOT_RANDOM_SEED_SIZE 32

typedef struct {
    UINT64 status; /* the EFI_STATUS of asking; bytes is a seed only if 0 */
    UINT8 bytes[BOOT_RANDOM_SEED_SIZE];
} BOOT_RANDOM_SEED;

/* A file nova-boot read from \nova\: its name, ASCII and NUL-terminated
   as the firmware reported it, and its contents. */
typedef struct {
    char name[BOOT_FILE_NAME_SIZE];
    UINT8 *data;
    UINTN size;
} BOOT_FILE;

typedef struct {
    UINT64 magic;
    UINT64 version;
    SCREEN screen;
    SPINNER spinner;
    UINT64 tsc_per_ms; /* timestamp-counter ticks per millisecond */
    UINT8 *memory_map; /* UEFI memory descriptors, descriptor_size apart */
    UINTN memory_map_size;
    UINTN descriptor_size;
    UINT64 random_seed_count; /* entries of random_seeds filled in */
    BOOT_RANDOM_SEED random_seeds[BOOT_RANDOM_SEEDS];
    UINT64 file_count; /* entries of files filled in */
    BOOT_FILE files[BOOT_FILES_MAX]; /* \nova\init.exe among them */
} BOOT_INFO;

#endif
