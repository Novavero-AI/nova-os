/* Sections (docs/spec/objects.md O2): pages that can be mapped into
   processes, as process_map does (docs/spec/syscalls.md S8). A memory
   section is zero-filled pages it holds for its whole life; a device
   section is a range of physical addresses outside RAM that an MMIO
   resource covers (docs/spec/ipc.md I6). A mapping holds a reference to
   its section, so a memory section's pages outlive every mapping of
   them. */

#ifndef NOVA_SECTION_H
#define NOVA_SECTION_H

#include "resource.h"
#include "../x86_64/paging.h"

/* The rights a new section's handle carries: every right a section has
   (O5, rule 1), and for a device section neither EXECUTE nor PIN (ipc.md
   I6). A section is not waitable, so neither handle has WAIT. */
#define SECTION_RIGHTS                                                    \
    (SECTION_RIGHT_READ | SECTION_RIGHT_WRITE | SECTION_RIGHT_EXECUTE |   \
     SECTION_RIGHT_PIN | RIGHT_DUPLICATE | RIGHT_TRANSFER)
#define DEVICE_SECTION_RIGHTS                                             \
    (SECTION_RIGHT_READ | SECTION_RIGHT_WRITE | RIGHT_DUPLICATE |         \
     RIGHT_TRANSFER)

typedef enum { SECTION_MEMORY = 1, SECTION_DEVICE } SECTION_KIND;

typedef struct {
    OBJECT object;
    SECTION_KIND kind;
    USER_MEMORY_TYPE memory_type; /* what its mappings give its pages */
    UINT64 pages;     /* how many */
    UINT64 *physical; /* a memory section's pages, in a page of their own */
    UINT64 base;      /* a device section's first physical address */
} SECTION;

/* A new memory section of size bytes, rounded up to whole pages, every
   byte zero, with its creator's handle. Returns OK; ERR_INVALID_ARGS for
   a size of 0; ERR_OUT_OF_RANGE for one above SECTION_MAX_SIZE; or
   ERR_NO_MEMORY. */
STATUS section_create(UINT64 size, SECTION **section);

/* section_create_physical: a device section over [base, base + size)
   with cache, a CACHE_ value, and its creator's handle. The range must
   be page aligned and covered by resource, an MMIO resource; it may
   overlap no memory the memory map describes other than memory-mapped
   I/O, except the PCI Express configuration space the ACPI MCFG
   describes, and none of the registers the kernel keeps: the local
   APIC's page, the MSI range (SDM Vol. 3A 13.11.1) and those acpi.h
   names. A device section holds at most SECTION_MAX_SIZE bytes, as a
   memory section does, so that mapping one is as short as mapping the
   other; a larger range takes several. Returns OK; ERR_INVALID_ARGS for
   a size of 0, an unaligned or wrapping range or an unknown cache value;
   ERR_OUT_OF_RANGE for a size above SECTION_MAX_SIZE or a range the
   resource does not cover; ERR_ACCESS_DENIED if the range reaches memory
   or kept registers; or ERR_NO_MEMORY. */
STATUS section_create_device(const RESOURCE *resource, UINT64 base,
                             UINT64 size, UINT32 cache, SECTION **section);

/* The physical address of page index of section, index below its
   pages. */
UINT64 section_page(const SECTION *section, UINT64 index);

/* Copy size bytes from data, kernel memory, into memory section section
   from its first byte; the section holds at least size bytes. */
void section_fill(SECTION *section, const UINT8 *data, UINT64 size);

#endif
