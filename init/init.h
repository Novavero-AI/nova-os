/* What init's parts share: its debug log (log.h), the files nova-boot
   read from \nova\, which the kernel passed as read-only sections, and
   the mapping of sections into init's own address space. */

#ifndef NOVA_INIT_H
#define NOVA_INIT_H

#include "log.h"
#include "native.h"

/* init's address space above its image: the seed chain's outputs from
   OUTPUT_BASE up to SCRATCH_BASE (view.c), and from SCRATCH_BASE up the
   sections map_into_init maps, each at the next free address. */
#define OUTPUT_BASE 0x0000000100000000ULL
#define SCRATCH_BASE 0x0000000200000000ULL

/* A boot file, mapped read-only into init. */
typedef struct {
    char name[BOOT_FILE_NAME_SIZE];
    UINT32 section;
    UINT64 size;
    const UINT8 *data;
} BOOT_FILE_VIEW;

/* Whether two ASCII names are the same regardless of case, as FAT's
   are. */
int same_name(const char *a, const char *b);

/* The boot file named name, regardless of case, or 0. */
const BOOT_FILE_VIEW *boot_file(const char *name);

/* Map size bytes of section, from its start, at a fresh address in
   init's own space with MAP_ protection, and return the address; 0 if
   the call fails. */
UINT8 *map_into_init(UINT32 section, UINT64 size, UINT32 protection);

/* Map size bytes of section, from its start, at address in init's own
   space with MAP_ protection, and return the address; 0 if the call
   fails. */
UINT8 *map_into_init_at(UINT32 section, UINT64 address, UINT64 size,
                        UINT32 protection);

/* Run stage0-pe32's chain from its hex0 seed to M1, each step as a
   32-bit process created in job, and check each link it builds against
   nova-nix's (chain.c). Returns 1 if every link matched or the boot
   volume lacks the seed, 0 if not. */
int chain_run(UINT32 job);

#endif
