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

/* Remove the direct map's translations of the size bytes at physical,
   page aligned, which the kernel never reaches through it, so that the
   memory types of other mappings alone decide the pages' type. Returns 0
   if a large page at the range's edge must be split and no page is free
   for the table; what was removed by then stays removed. */
int paging_unmap_direct_range(UINT64 physical, UINT64 size);

/* What a user mapping allows. Only a process whose creator granted it
   may have a mapping both writable and executable (docs/spec/objects.md
   O5); process_map checks that before it asks for one. */
typedef enum {
    USER_READ,
    USER_READ_WRITE,
    USER_READ_EXECUTE,
    USER_READ_WRITE_EXECUTE
} USER_PROTECTION;

/* The memory type a user mapping gives its page: write-back, as RAM is
   mapped, or the two section_create_physical offers (ipc.md I6). */
typedef enum {
    USER_WRITE_BACK,
    USER_UNCACHED,
    USER_WRITE_COMBINING
} USER_MEMORY_TYPE;

/* Whether the direct map maps the page at physical with a memory type
   other than type, through another PAT entry than a user mapping of that
   type selects. */
int paging_direct_type_differs(UINT64 physical, USER_MEMORY_TYPE type);

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
   paging_map_user does, with the memory type type, but leave the page
   its owner's, a section's: the space never frees it. Returns 0 if a
   table is needed and no page is free. */
int paging_map_user_borrowed(UINT64 space, UINT64 linear, UINT64 physical,
                             USER_PROTECTION protection,
                             USER_MEMORY_TYPE type);

/* Whether anything is mapped at the user page linear in space. */
int paging_user_mapped(UINT64 space, UINT64 linear);

/* Remove the borrowed page paging_map_user_borrowed mapped at linear in
   space, leaving the page to its owner. */
void paging_unmap_user_borrowed(UINT64 space, UINT64 linear);

/* The physical-address width, MAXPHYADDR (SDM Vol. 3A 5.1.4): entries
   map only addresses below 2 to its power. */
UINT32 paging_physical_bits(void);

/* Remove every lower-half mapping of space, freeing the pages it owns
   and the tables that mapped them. The space stays usable as a kernel
   address space. */
void paging_space_clear(UINT64 space);

/* Clear space and free its top table. No processor may be using it. */
void paging_space_destroy(UINT64 space);

#endif
