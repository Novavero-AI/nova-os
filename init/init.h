/* What init's parts share: its debug log (log.h), the files nova-boot
   read from \nova\ and the store image, which the kernel passed as
   read-only sections, the mapping of sections into init's own address
   space, and the window init lays out the programs it starts through
   (program.h). */

#ifndef NOVA_INIT_H
#define NOVA_INIT_H

#include "log.h"
#include "native.h"
#include "program.h"

/* init's address space above its image: the seed chain's outputs from
   OUTPUT_BASE up to SCRATCH_BASE (view.c), and from SCRATCH_BASE up the
   sections map_into_init maps, each at the next free address. */
#define OUTPUT_BASE 0x0000000100000000ULL
#define SCRATCH_BASE 0x0000000200000000ULL

/* The window init lays out each program it starts through: at 64 TiB,
   far above what map_into_init reaches from SCRATCH_BASE, and
   SECTION_MAX_SIZE bytes, the largest image a section holds. */
#define LAYOUT_BASE 0x0000400000000000ULL
#define LAYOUT_SIZE SECTION_MAX_SIZE

/* A boot file or the store image, mapped read-only into init. */
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

/* init's window for laying out the programs it starts (program.h). */
const LAYOUT_WINDOW *init_layout(void);

/* program_load of the boot file file's bytes through init's window;
   ERR_NOT_FOUND if file is 0. */
UINT32 load_boot_file(UINT32 job, const BOOT_FILE_VIEW *file,
                      PROGRAM *program);

/* Map size bytes of section, from its start, at a fresh address in
   init's own space with MAP_ protection, and return the address; 0 if
   the call fails. */
UINT8 *map_into_init(UINT32 section, UINT64 size, UINT32 protection);

/* Remove the mapping map_into_init made at address for size bytes,
   which it rounds up to whole pages as map_into_init does. Returns the
   call's status. */
UINT32 unmap_from_init(const UINT8 *address, UINT64 size);

/* Map size bytes of section, from its start, at address in init's own
   space with MAP_ protection, and return the address; 0 if the call
   fails. */
UINT8 *map_into_init_at(UINT32 section, UINT64 address, UINT64 size,
                        UINT32 protection);

/* Run stage0-pe32's chain from its hex0 seed to M1, each step as a
   32-bit process created in job, and check each link it builds against
   nova-nix's (chain.c). Returns 1 if every link matched or the boot
   volume holds none of the chain's inputs, 0 if not. */
int chain_run(UINT32 job);

/* The store service and the namespace server, which store_run starts
   and leaves running for the life of the system, through init's
   channels to them: the namespace's root, with every right, and the
   store service's control channel, with CLOSURE. init keeps both, and
   opens through them what it gives the programs it starts after
   startup. */
typedef struct {
    UINT32 names;
    UINT32 control;
} STORE_SERVICES;

/* Start the store service over image and the namespace server, publish
   /store, and run the test program from /store in job with a view of
   its closure (store.c), creating each program's debug log from
   debug_log. Returns 1, with init's channels to the two services, which
   run on, in *services, if the program passed its checks; 0, with both
   HANDLE_INVALID, if not. */
int store_run(UINT32 job, UINT32 debug_log, const BOOT_FILE_VIEW *image,
              STORE_SERVICES *services);

#endif
