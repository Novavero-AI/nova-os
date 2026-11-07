/* The kernel's page tables. nova-boot builds them and loads them as it
   switches to the kernel; the kernel reaches each table through the
   direct map, at the physical address the entry above it holds. */

#include "paging.h"
#include "cpu.h"
#include "memory_layout.h"
#include "page_table.h"
#include "serial.h"
#include "../mm/page.h"

/* IA32_PAT (SDM Vol. 3A 14.12.2): eight one-byte entries, each a memory
   type in its low three bits, where 00H is UC (Table 14-10). Every
   processor with 4-level paging has it (SDM Vol. 3A 5.9.1). A page's
   PWT, PCD and PAT bits form the index of its entry (Table 14-11). */
#define IA32_PAT 0x277u
#define PAT_ENTRIES 8
#define PAT_ENTRY_BITS 8
#define PAT_TYPE_MASK 0x7
#define PAT_UNCACHEABLE 0x0
#define PAT_INDEX_PWT 1
#define PAT_INDEX_PCD 2
#define PAT_INDEX_PAT 4

static UINT64 *top_table;
static int levels;
static UINT64 no_execute; /* PTE_NO_EXECUTE while IA32_EFER.NXE is set */

static UINT64 *table_at(UINT64 entry)
{
    return direct_map(entry & PTE_ADDRESS);
}

/* Invalidate every cached translation, global ones included. Changing
   CR4.PGE does that; with CR4.PGE clear no translation is global, and
   loading CR3 does it (SDM Vol. 3A 5.10.4.1). */
static void flush_translations(void)
{
    UINT64 cr4 = read_cr4();

    if (cr4 & CR4_GLOBAL_PAGES) {
        write_cr4(cr4 & ~CR4_GLOBAL_PAGES);
        write_cr4(cr4);
    } else {
        write_cr3(read_cr3());
    }
}

/* The lower half holds only boot_switch's pages, which nova-boot mapped
   for the CR3 load. */
void paging_init(void)
{
    UINT64 i;

    top_table = table_at(read_cr3());
    levels = paging_levels();
    no_execute = (rdmsr(IA32_EFER) & EFER_NO_EXECUTE) ? PTE_NO_EXECUTE : 0;
    for (i = 0; i < LOWER_HALF_ENTRIES; i++)
        top_table[i] = 0;
    flush_translations();
    serial_write("nova: ");
    serial_number((UINT64)levels);
    serial_write("-level paging, lower half unmapped\r\n");
}

/* The PWT, PCD and PAT bits of a PTE that select a PAT entry holding UC,
   or 0 if no entry does. */
static int uncached_bits(UINT64 *bits)
{
    UINT64 pat = rdmsr(IA32_PAT);
    UINT64 i;

    for (i = 0; i < PAT_ENTRIES; i++)
        if ((pat >> (i * PAT_ENTRY_BITS) & PAT_TYPE_MASK) == PAT_UNCACHEABLE) {
            *bits = ((i & PAT_INDEX_PWT) ? PTE_WRITE_THROUGH : 0) |
                    ((i & PAT_INDEX_PCD) ? PTE_CACHE_DISABLE : 0) |
                    ((i & PAT_INDEX_PAT) ? PTE_PAT : 0);
            return 1;
        }
    return 0;
}

/* Point entry at a new, empty table. Like nova-boot's, the entry grants
   write and leaves restrictions to the entries below. */
static int add_table(UINT64 *entry)
{
    UINT64 page = page_alloc();
    UINT64 *table;
    UINT64 i;

    if (!page)
        return 0;
    table = direct_map(page);
    for (i = 0; i < PAGE_TABLE_ENTRIES; i++)
        table[i] = 0;
    *entry = page | PTE_PRESENT | PTE_WRITABLE;
    return 1;
}

/* Replace the large page entry maps at level with a table of the next
   level down mapping the same memory with the same bits, so that one part
   of it can change. A 1 GiB page becomes 2 MiB pages, whose bits keep
   their places; a 2 MiB page becomes 4 KiB pages, which have no PS bit
   and hold the PAT bit at 7 rather than 12 (SDM Vol. 3A Tables 5-16, 5-18
   and 5-20). */
static int split(UINT64 *entry, int level)
{
    UINT64 large = *entry;
    UINT64 base = large & PTE_ADDRESS & ~(level_span(level) - 1);
    UINT64 flags = large & ~PTE_ADDRESS;
    UINT64 page = page_alloc();
    UINT64 *table;
    UINT64 i;

    if (!page)
        return 0;
    if (level - 1 > PAGE_TABLE_LEVEL)
        flags |= large & PTE_LARGE_PAT;
    else if (large & PTE_LARGE_PAT)
        flags = (flags & ~PTE_LARGE) | PTE_PAT;
    else
        flags &= ~PTE_LARGE;
    table = direct_map(page);
    for (i = 0; i < PAGE_TABLE_ENTRIES; i++)
        table[i] = (base + i * level_span(level - 1)) | flags;
    *entry = page | PTE_PRESENT | PTE_WRITABLE;
    return 1;
}

/* The PTE that translates linear, adding tables and splitting large
   pages on the way down; 0 if that needs a page and none is free. */
static UINT64 *page_entry(UINT64 linear)
{
    UINT64 *table = top_table;
    int level;

    for (level = levels; level > PAGE_TABLE_LEVEL; level--) {
        UINT64 *entry = &table[table_index(linear, level)];

        if (!(*entry & PTE_PRESENT)) {
            if (!add_table(entry))
                return 0;
        } else if (*entry & PTE_LARGE) {
            if (!split(entry, level))
                return 0;
        }
        table = table_at(*entry);
    }
    return &table[table_index(linear, PAGE_TABLE_LEVEL)];
}

UINT64 paging_map_uncached(UINT64 physical)
{
    UINT64 linear = DIRECT_MAP_BASE + physical;
    UINT64 memory_type, flags;
    UINT64 *entry;

    if (physical % PAGE_SIZE != 0 || physical >= DIRECT_MAP_SIZE ||
        !uncached_bits(&memory_type))
        return 0;
    flags = save_and_disable_interrupts();
    entry = page_entry(linear);
    if (entry) {
        *entry =
            physical | PTE_PRESENT | PTE_WRITABLE | no_execute | memory_type;
        flush_translations();
    }
    restore_interrupts(flags);
    return entry ? linear : 0;
}

/* Clearing P alone keeps the rest of the entry, which the processor
   ignores while P is 0 (SDM Vol. 3A 5.5.4, Figure 5-11), for
   paging_restore_direct to bring back. The flush drops the old
   translation, which may be the large page the guard was split from. */
int paging_unmap_direct(UINT64 physical)
{
    UINT64 flags = save_and_disable_interrupts();
    UINT64 *entry = page_entry(DIRECT_MAP_BASE + physical);

    if (entry) {
        *entry &= ~PTE_PRESENT;
        flush_translations();
    }
    restore_interrupts(flags);
    return entry != 0;
}

/* No TLB or paging-structure cache entry is made from an entry whose P
   flag is 0, so setting it needs no invalidation (SDM Vol. 3A 5.10.4.3). */
void paging_restore_direct(UINT64 physical)
{
    UINT64 flags = save_and_disable_interrupts();

    *page_entry(DIRECT_MAP_BASE + physical) |= PTE_PRESENT;
    restore_interrupts(flags);
}
