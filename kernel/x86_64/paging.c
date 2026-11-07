/* The kernel's page tables and user address spaces. nova-boot builds the
   kernel's tables and loads them as it switches to the kernel; the
   kernel reaches each table through the direct map, at the physical
   address the entry above it holds.

   A user address space is a top table of its own whose upper half
   repeats the kernel's top-level entries, so the kernel is mapped in
   every space alike, and whose lower half holds the process's mappings.
   The kernel's upper-half top-level entries are fixed once paging_init
   returns: a later kernel mapping that would need a new one fails
   instead (page_entry), so the copy each space takes when it is created
   never goes stale. Every kernel mapping made after boot lies in the
   direct map, whose top-level entries nova-boot made for the memory the
   map describes. */

#include "paging.h"
#include "cpu.h"
#include "memory_layout.h"
#include "page_table.h"
#include "serial.h"
#include "../mm/page.h"

/* IA32_PAT's layout is in page_table.h: eight one-byte entries, each a
   memory type in its low three bits (SDM Vol. 3A 14.12.2, Table 14-10),
   indexed by a page's PWT, PCD and PAT bits (Table 14-11). */
#define PAT_ENTRIES 8
#define PAT_TYPE_MASK 0x7

static UINT64 kernel_space; /* the kernel's top table, physical */
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

    kernel_space = read_cr3() & PTE_ADDRESS;
    top_table = direct_map(kernel_space);
    levels = paging_levels();
    no_execute = (rdmsr(IA32_EFER) & EFER_NO_EXECUTE) ? PTE_NO_EXECUTE : 0;
    for (i = 0; i < LOWER_HALF_ENTRIES; i++)
        top_table[i] = 0;
    flush_translations();
    serial_write("nova: ");
    serial_number((UINT64)levels);
    serial_write("-level paging, lower half unmapped\r\n");
}

UINT64 paging_kernel_space(void)
{
    return kernel_space;
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

/* Point entry at a new, empty table: page_alloc's page reads as zero, so
   every entry in it is not present. Like nova-boot's, the entry grants
   write and leaves restrictions to the entries below; user_bit is
   PTE_USER for a table of user mappings, whose every level must allow
   user access, else 0. */
static int add_table(UINT64 *entry, UINT64 user_bit)
{
    UINT64 page = page_alloc();

    if (!page)
        return 0;
    *entry = page | PTE_PRESENT | PTE_WRITABLE | user_bit;
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

/* The PTE that translates kernel address linear, adding tables below
   the top level and splitting large pages on the way down; 0 if that
   needs a page and none is free, or needs a new top-level entry. */
static UINT64 *page_entry(UINT64 linear)
{
    UINT64 *table = top_table;
    int level;

    for (level = levels; level > PAGE_TABLE_LEVEL; level--) {
        UINT64 *entry = &table[table_index(linear, level)];

        if (!(*entry & PTE_PRESENT)) {
            if (level == levels || !add_table(entry, 0))
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

/* ------------------------------------------------------------------------
   User address spaces. */

/* What each protection sets in a PTE. XD is set wherever execution is
   not allowed; user_mode_init has checked that IA32_EFER.NXE is set, so
   XD is not reserved (SDM Vol. 3A 5.5.4). */
static UINT64 protection_bits(USER_PROTECTION protection)
{
    switch (protection) {
    case USER_READ_WRITE:
        return PTE_WRITABLE | PTE_NO_EXECUTE;
    case USER_READ_EXECUTE:
        return 0;
    case USER_READ_WRITE_EXECUTE:
        return PTE_WRITABLE;
    case USER_READ:
        break;
    }
    return PTE_NO_EXECUTE;
}

/* page_alloc's page reads as zero, so the lower half maps nothing. */
UINT64 paging_space_create(void)
{
    UINT64 page = page_alloc();
    UINT64 *table;
    UINT64 i;

    if (!page)
        return 0;
    table = direct_map(page);
    for (i = LOWER_HALF_ENTRIES; i < PAGE_TABLE_ENTRIES; i++)
        table[i] = top_table[i];
    return page;
}

/* The PTE that maps user address linear in space, adding the tables on
   the way down when add is set; 0 if a table is missing and add is
   clear, or if one is needed and no page is free. With interrupts
   disabled. */
static UINT64 *user_entry(UINT64 space, UINT64 linear, int add)
{
    UINT64 *table = direct_map(space);
    int level;

    for (level = levels; level > PAGE_TABLE_LEVEL; level--) {
        UINT64 *entry = &table[table_index(linear, level)];

        if (!(*entry & PTE_PRESENT) && (!add || !add_table(entry, PTE_USER)))
            return 0;
        table = table_at(*entry);
    }
    return &table[table_index(linear, PAGE_TABLE_LEVEL)];
}

static int map_user(UINT64 space, UINT64 linear, UINT64 bits)
{
    UINT64 flags = save_and_disable_interrupts();
    UINT64 *entry = user_entry(space, linear, 1);

    if (entry)
        *entry = bits | PTE_PRESENT | PTE_USER;
    restore_interrupts(flags);
    return entry != 0;
}

int paging_map_user(UINT64 space, UINT64 linear, UINT64 physical,
                    USER_PROTECTION protection)
{
    return map_user(space, linear, physical | protection_bits(protection));
}

int paging_map_user_borrowed(UINT64 space, UINT64 linear, UINT64 physical,
                             USER_PROTECTION protection)
{
    return map_user(space, linear,
                    physical | protection_bits(protection) | PTE_BORROWED);
}

int paging_user_mapped(UINT64 space, UINT64 linear)
{
    UINT64 flags = save_and_disable_interrupts();
    UINT64 *entry = user_entry(space, linear, 0);
    int mapped = entry && (*entry & PTE_PRESENT);

    restore_interrupts(flags);
    return mapped;
}

/* The processor may cache a translation for a speculative access the
   code never makes (SDM Vol. 3A 5.10.2.2), so a space in use is
   reloaded; no user translation is global, and a space not in use has
   none cached, since loading CR3 invalidated them (5.10.4.1). */
void paging_unmap_user_borrowed(UINT64 space, UINT64 linear)
{
    UINT64 flags = save_and_disable_interrupts();
    UINT64 *entry = user_entry(space, linear, 0);

    if (entry)
        *entry = 0;
    if ((read_cr3() & PTE_ADDRESS) == space)
        write_cr3(space);
    restore_interrupts(flags);
}

/* Free the pages table maps at level and below, other than borrowed
   ones, and the tables below it. The depth is the paging depth, at most
   five. */
static void free_below(UINT64 *table, int level)
{
    UINT64 i;

    for (i = 0; i < PAGE_TABLE_ENTRIES; i++) {
        if (!(table[i] & PTE_PRESENT))
            continue;
        if (level > PAGE_TABLE_LEVEL)
            free_below(table_at(table[i]), level - 1);
        if (level > PAGE_TABLE_LEVEL || !(table[i] & PTE_BORROWED))
            page_free(table[i] & PTE_ADDRESS);
        table[i] = 0;
    }
}

/* A space in use keeps translations of what was freed until CR3 is
   loaded again (SDM Vol. 3A 5.10.4.1). */
void paging_space_clear(UINT64 space)
{
    UINT64 flags = save_and_disable_interrupts();
    UINT64 *top = direct_map(space);
    UINT64 i;

    for (i = 0; i < LOWER_HALF_ENTRIES; i++) {
        if (!(top[i] & PTE_PRESENT))
            continue;
        free_below(table_at(top[i]), levels - 1);
        page_free(top[i] & PTE_ADDRESS);
        top[i] = 0;
    }
    if ((read_cr3() & PTE_ADDRESS) == space)
        write_cr3(space);
    restore_interrupts(flags);
}

void paging_space_destroy(UINT64 space)
{
    paging_space_clear(space);
    page_free(space);
}
