/* What init's parts share: its debug log, the files nova-boot read from
   \nova\, which the kernel passed as read-only sections, and the mapping
   of sections into init's own address space. */

#ifndef NOVA_INIT_H
#define NOVA_INIT_H

#include "native.h"

/* A boot file, mapped read-only into init. */
typedef struct {
    char name[BOOT_FILE_NAME_SIZE];
    UINT32 section;
    UINT64 size;
    const UINT8 *data;
} BOOT_FILE_VIEW;

/* Write line to the kernel's debug log. */
void say(const char *line);

/* Write n to the debug log, in decimal. */
void say_number(UINT64 n);

/* Whether two ASCII names are the same regardless of case, as FAT's
   are. */
int same_name(const char *a, const char *b);

/* The boot file named name, regardless of case, or 0. */
const BOOT_FILE_VIEW *boot_file(const char *name);

/* Map size bytes of section, from its start, at a fresh address in
   init's own space with MAP_ protection, and return the address; 0 if
   the call fails. */
UINT8 *map_into_init(UINT32 section, UINT64 size, UINT32 protection);

/* Run stage0-pe32's seed, hex0-seed.exe, on hex0_x86.hex0 as a 32-bit
   process created in job, and check that it writes its own bytes.
   Returns 1 if it did or if the boot volume lacks the files, 0 if not. */
int seed_run(UINT32 job);

#endif
