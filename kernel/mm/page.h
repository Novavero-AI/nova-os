/* The physical page allocator: the 4 KiB pages the UEFI memory map gives
   the OS once boot services have exited, handed out one at a time by
   physical address. */

#ifndef NOVA_PAGE_H
#define NOVA_PAGE_H

#include "boot_info.h"

/* The x86-64 base page, and the unit of the UEFI memory map
   (UEFI 2.11, 7.2.3). */
#define PAGE_SIZE 4096

/* For use before page_init: the address of the first run, in map order,
   of the given number of contiguous pages of conventional memory that the
   kernel can write, or 0 if there is none. Nothing is marked taken until
   page_init is told. */
UINT64 page_find_early(const BOOT_INFO *info, UINT64 pages);

/* Build the allocator from the memory map, on a stack outside
   boot-services memory, which it hands out. The run of held_pages pages
   at held, from page_find_early, stays allocated. Returns 0 if no
   conventional memory can hold the allocator's own bitmap. */
int page_init(const BOOT_INFO *info, UINT64 held, UINT64 held_pages);

/* A free page's physical address, now allocated, or 0 if none is free. */
UINT64 page_alloc(void);

/* Free an allocated page. Returns 1, or 0 with nothing changed if address
   is not a page the allocator holds as allocated: unaligned, beyond the
   memory it manages, page 0, or free already. A reserved page cannot be
   told from an allocated one, so freeing one is not caught. */
int page_free(UINT64 address);

UINT64 page_free_count(void);

/* Pages of the memory types the OS owns as general memory, whether free
   now or not. */
UINT64 page_total_count(void);

/* Check the allocator against the memory map and the run page_init was
   told to hold. Returns 0 if every check passes, otherwise the one that
   failed. */
const char *page_self_test(const BOOT_INFO *info, UINT64 held,
                           UINT64 held_pages);

#endif
