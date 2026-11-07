/* The kernel's page tables, which nova-boot built and loaded before the
   kernel's entry (docs/spec/boot-protocol.md, Paging). */

#ifndef NOVA_PAGING_H
#define NOVA_PAGING_H

#include "types.h"

/* Take the tables over: remove every lower-half mapping, which leaves
   only the upper half nova-boot built. */
void paging_init(void);

/* Map the 4 KiB page at physical, at its direct-map address, with the
   strong uncacheable memory type, which memory-mapped registers such as
   the Local APIC's need. Returns that address, or 0 if no PAT entry holds
   UC or a table is needed and no page is free. */
UINT64 paging_map_uncached(UINT64 physical);

#endif
