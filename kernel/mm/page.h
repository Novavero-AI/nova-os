/* The physical page allocator: the 4 KiB pages the UEFI memory map gives
   the OS once boot services have exited, handed out one at a time by
   physical address. The kernel reaches a page through the direct map
   (memory_layout.h). */

#ifndef NOVA_PAGE_H
#define NOVA_PAGE_H

#include "boot_info.h"
#include "memory_layout.h"

/* Build the allocator from the memory map. Returns 0 if no conventional
   memory can hold the allocator's own bitmap. */
int page_init(const BOOT_INFO *info);

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

/* Check the allocator against the memory map. Returns 0 if every check
   passes, otherwise the one that failed. */
const char *page_self_test(const BOOT_INFO *info);

#endif
