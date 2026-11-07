/* x86-64 paging as nova-boot and the kernel use it: the control bits that
   enable and modify it (SDM Vol. 3A 5.1) and the paging-structure entries
   of 4-level and 5-level paging (SDM Vol. 3A 5.5.4, Tables 5-14 to 5-20).
   Levels count up from the page table: 1 holds PTEs, 2 PDEs, 3 PDPTEs, 4
   PML4Es and, with 5-level paging, 5 PML5Es. */

#ifndef NOVA_PAGE_TABLE_H
#define NOVA_PAGE_TABLE_H

#include "cpu.h"

/* CR0.WP makes read-only pages bind supervisor writes too (SDM Vol. 3A
   5.1.3); CR4.PGE enables global pages and CR4.LA57 5-level paging (SDM
   Vol. 3A 5.1). */
#define CR0_WRITE_PROTECT (1ULL << 16)
#define CR4_GLOBAL_PAGES (1ULL << 7)
#define CR4_FIVE_LEVELS (1ULL << 12)

/* IA32_EFER, at C000_0080h (AMD APM Vol. 2 3.1.7), whose NXE bit makes
   the XD bit of an entry forbid instruction fetches (SDM Vol. 3A 2.2.1,
   Table 2-1, and 5.1.3). */
#define IA32_EFER 0xc0000080u
#define EFER_NO_EXECUTE (1ULL << 11)

#define PTE_PRESENT (1ULL << 0)
#define PTE_WRITABLE (1ULL << 1)
#define PTE_WRITE_THROUGH (1ULL << 3) /* PWT */
#define PTE_CACHE_DISABLE (1ULL << 4) /* PCD */
/* PS: a level 2 or 3 entry with it set maps a 2 MiB or 1 GiB page. A
   PTE's bit 7 is its PAT bit instead; an entry that maps a large page
   has its PAT bit at 12. */
#define PTE_LARGE (1ULL << 7)
#define PTE_PAT (1ULL << 7)
#define PTE_LARGE_PAT (1ULL << 12)
/* XD: no instruction fetch through the entry. Reserved, and so a fault
   if set, while IA32_EFER.NXE is clear. */
#define PTE_NO_EXECUTE (1ULL << 63)
/* Bits 51:12: the next table's address, or a 4 KiB page's. CR3 holds the
   top table's address in the same bits (SDM Vol. 3A Table 5-12). */
#define PTE_ADDRESS 0x000FFFFFFFFFF000ULL

#define PAGE_TABLE_ENTRIES 512
#define PAGE_OFFSET_BITS 12
#define PAGE_INDEX_BITS 9

#define PAGE_TABLE_LEVEL 1
#define PAGE_DIRECTORY_LEVEL 2
#define PDPT_LEVEL 3
#define FOUR_LEVELS 4
#define FIVE_LEVELS 5

/* The top table's entries below this index map the lower half. The
   highest bit of the top table's index is the one canonical addresses
   sign-extend: bit 47 with 4-level paging, 56 with 5-level (SDM Vol. 3A
   5.1.1 and 5.5.4). */
#define LOWER_HALF_ENTRIES (PAGE_TABLE_ENTRIES / 2)

/* The paging depth in use. CR4.LA57 cannot change while paging is on in
   IA-32e mode (SDM Vol. 3A 5.1.2), so nova-boot's tables and the
   kernel's have the depth the firmware chose. */
static inline int paging_levels(void)
{
    return (read_cr4() & CR4_FIVE_LEVELS) ? FIVE_LEVELS : FOUR_LEVELS;
}

/* The bytes one entry at level spans: 4 KiB at level 1, then 512 times
   more per level. */
static inline UINT64 level_span(int level)
{
    return 1ULL << (PAGE_OFFSET_BITS + PAGE_INDEX_BITS * (level - 1));
}

/* The index of the entry at level that translates address. */
static inline UINT64 table_index(UINT64 address, int level)
{
    return (address >> (PAGE_OFFSET_BITS + PAGE_INDEX_BITS * (level - 1))) &
           (PAGE_TABLE_ENTRIES - 1);
}

#endif
