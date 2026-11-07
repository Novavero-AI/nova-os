/* The kernel's page tables, which nova-boot built and loaded before the
   kernel's entry (docs/spec/boot-protocol.md, Paging). The calls that
   change them may come from any thread: each runs with interrupts
   disabled. */

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

/* Make the direct map's page for physical absent, so that any access
   through it faults; the page is a guard. physical is a page the caller
   holds from the page allocator. Returns 0 if a large page must be split
   and no page is free for the table; every translation is then as it
   was. */
int paging_unmap_direct(UINT64 physical);

/* Map a page paging_unmap_direct made absent again, as it was. The
   tables that translate it exist by then, so this needs no page. */
void paging_restore_direct(UINT64 physical);

#endif
