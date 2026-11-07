/* The kernel's page tables, which nova-boot built and loaded before the
   kernel's entry (docs/spec/boot-protocol.md, Paging), and the address
   spaces of user processes. The calls that change them may come from any
   thread: each runs with interrupts disabled. */

#ifndef NOVA_PAGING_H
#define NOVA_PAGING_H

#include "types.h"

/* Take the tables over: remove every lower-half mapping, which leaves
   only the upper half nova-boot built. */
void paging_init(void);

/* The physical address of the kernel's top table, which kernel threads
   run on. */
UINT64 paging_kernel_space(void);

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

/* What a user mapping allows. Only a process whose creator granted it
   may have a mapping both writable and executable (docs/spec/objects.md
   O5); process_map checks that before it asks for one. */
typedef enum {
    USER_READ,
    USER_READ_WRITE,
    USER_READ_EXECUTE,
    USER_READ_WRITE_EXECUTE
} USER_PROTECTION;

/* A new address space: a top table whose upper half is the kernel's and
   whose lower half maps nothing. Returns its physical address, for CR3,
   or 0 if no page is free. */
UINT64 paging_space_create(void);

/* Map the 4 KiB page at physical at user address linear, a page boundary
   in the lower half not yet mapped, in space. The space owns the page
   from then on and frees it when it unmaps it. Returns 0 if a table is
   needed and no page is free. */
int paging_map_user(UINT64 space, UINT64 linear, UINT64 physical,
                    USER_PROTECTION protection);

/* Map the 4 KiB page at physical at user address linear in space, as
   paging_map_user does, but leave the page its owner's, a section's: the
   space never frees it. Returns 0 if a table is needed and no page is
   free. */
int paging_map_user_borrowed(UINT64 space, UINT64 linear, UINT64 physical,
                             USER_PROTECTION protection);

/* Whether anything is mapped at the user page linear in space. */
int paging_user_mapped(UINT64 space, UINT64 linear);

/* Remove the borrowed page paging_map_user_borrowed mapped at linear in
   space, leaving the page to its owner. */
void paging_unmap_user_borrowed(UINT64 space, UINT64 linear);

/* Remove every lower-half mapping of space, freeing the pages it owns
   and the tables that mapped them. The space stays usable as a kernel
   address space. */
void paging_space_clear(UINT64 space);

/* Clear space and free its top table. No processor may be using it. */
void paging_space_destroy(UINT64 space);

#endif
