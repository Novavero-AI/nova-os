/* Sections (docs/spec/objects.md O2): ranges of zero-filled pages that
   can be mapped into processes, as process_map does
   (docs/spec/syscalls.md S8). A section holds its pages for its whole
   life; a mapping holds a reference to the section, so the pages outlive
   every mapping of them. */

#ifndef NOVA_SECTION_H
#define NOVA_SECTION_H

#include "object.h"

/* The rights a new section's handle carries: every right a section has
   (O5, rule 1). A section is not waitable, so the handle has no WAIT. */
#define SECTION_RIGHTS                                                    \
    (SECTION_RIGHT_READ | SECTION_RIGHT_WRITE | SECTION_RIGHT_EXECUTE |   \
     SECTION_RIGHT_PIN | RIGHT_DUPLICATE | RIGHT_TRANSFER)

typedef struct {
    OBJECT object;
    UINT64 pages;     /* how many */
    UINT64 *physical; /* each page's physical address, in a page of its own */
} SECTION;

/* A new section of size bytes, rounded up to whole pages, every byte
   zero, with its creator's handle. Returns OK; ERR_INVALID_ARGS for a
   size of 0; ERR_OUT_OF_RANGE for one above SECTION_MAX_SIZE; or
   ERR_NO_MEMORY. */
STATUS section_create(UINT64 size, SECTION **section);

/* Copy size bytes from data, kernel memory, into section from its first
   byte; the section holds at least size bytes. */
void section_fill(SECTION *section, const UINT8 *data, UINT64 size);

#endif
