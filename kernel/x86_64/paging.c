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
   map describes.

   Under page-table isolation (docs/spec/speculation.md M11) a space is
   two top tables in consecutive pages: the kernel's, as above, and the
   user's, at the next page, which shares the lower half's tables and in
   the upper half maps only the entry region, the code and data the
   processor and the entry code reach under it, through tables of its own
   that every user table shares. With PCIDs the kernel's table runs under
   KERNEL_PCID and the user's under USER_PCID, so that the entry's and
   exit's CR3 loads keep each other's translations (SDM Vol. 3A 5.10.1,
   5.10.4.1). */

#include "paging.h"
#include "cpu.h"
#include "memory_layout.h"
#include "page_table.h"
#include "serial.h"
#include "x86_64.h"
#include "../mm/page.h"

/* IA32_PAT's layout is in page_table.h: eight one-byte entries, each a
   memory type in its low three bits (SDM Vol. 3A 14.12.2, Table 14-10),
   indexed by a page's PWT, PCD and PAT bits (Table 14-11). */
#define PAT_ENTRIES 8
#define PAT_TYPE_MASK 0x7

/* CPUID.80000008H:EAX[7:0] gives the physical-address width; without
   that leaf it is 36 where CPUID.01H:EDX.PAE[6] is set (SDM Vol. 3A
   5.1.4), as on every processor with long mode. CPUID.80000000H:EAX
   gives the highest extended leaf. */
#define CPUID_EXTENDED_MAX 0x80000000u
#define CPUID_ADDRESS_SIZES 0x80000008u
#define PHYSICAL_BITS_MASK 0xff
#define PHYSICAL_BITS_DEFAULT 36

static UINT64 kernel_space; /* the kernel's top table, physical */
static UINT64 *top_table;
static int levels;
static UINT64 no_execute; /* PTE_NO_EXECUTE while IA32_EFER.NXE is set */

/* CR4.PCIDE enables PCIDs, CR3's bits 11:0 then name the PCID, and a CR3
   load with bit 63 set keeps the new PCID's translations (SDM Vol. 3A
   2.5, 5.10.4.1). */
#define CR4_PCIDE (1ULL << 17)
#define CR3_NO_FLUSH (1ULL << 63)
#define KERNEL_PCID 0
#define USER_PCID 1

/* A user table is the page after its space's kernel table. */
#define SPACE_TABLES 2

/* The entry region's tables, below the one top-level entry every user
   table has for the kernel half: one for each level below the top with
   5-level paging, so one page table, enough while the region lies within
   one page table's 2 MiB, as the image does; with 4-level paging one is
   spare. map_entry_page stops the kernel if they run out. In .data, where
   TinyCC honours the page alignment (gdt.c). */
#define ENTRY_TABLES (FIVE_LEVELS - 1)

static UINT64 entry_tables[ENTRY_TABLES][PAGE_TABLE_ENTRIES]
    __attribute__((aligned(PAGE_SIZE), section(".data")));
static UINT32 entry_tables_used;
static UINT64 entry_top_entry; /* what each user table holds at entry_top */
static UINT64 entry_top;       /* its index in the top table */

static int isolation;
static UINT64 cr3_no_flush; /* CR3_NO_FLUSH with PCIDs, otherwise 0 */
static UINT64 user_pcid;    /* USER_PCID with PCIDs, otherwise 0 */

/* The space of the last user thread this processor ran, or 0 if none or
   if that space was destroyed, so that a new space at the same address is
   another process's. */
static UINT64 last_user_space;

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
    serial_write("nova: paging levels=");
    serial_number((UINT64)levels);
    serial_write("\r\n");
}

UINT64 paging_kernel_space(void)
{
    return kernel_space;
}

/* The entry of the kernel's tables that maps kernel address linear, at
   the level it is found, which is where the walk ends: a page or a large
   page, or 0 where nothing is mapped. */
static UINT64 kernel_leaf(UINT64 linear, int *level)
{
    UINT64 *table = top_table;
    UINT64 entry = 0;

    for (*level = levels; *level >= PAGE_TABLE_LEVEL; (*level)--) {
        entry = table[table_index(linear, *level)];
        if (!(entry & PTE_PRESENT) || *level == PAGE_TABLE_LEVEL ||
            (entry & PTE_LARGE))
            break;
        table = table_at(entry);
    }
    return entry & PTE_PRESENT ? entry : 0;
}

/* The physical address of kernel address linear, which the kernel's
   tables map. */
static UINT64 kernel_physical(UINT64 linear)
{
    int level;
    UINT64 entry = kernel_leaf(linear, &level);
    UINT64 span = level_span(level);

    return (entry & PTE_ADDRESS & ~(span - 1)) + linear % span;
}

_Noreturn static void stop(const char *what, UINT64 address)
{
    serial_write("nova: paging failed reason=");
    serial_value(what);
    serial_write(" address=");
    serial_hex(address);
    serial_write("\r\n");
    serial_halt();
}

void paging_write_text(UINT64 linear, const UINT8 *bytes, UINT64 count)
{
    UINT64 i;
    int level;

    for (i = 0; i < count; i++) {
        if (!kernel_leaf(linear + i, &level))
            stop("text to write is not mapped", linear + i);
        *(UINT8 *)direct_map(kernel_physical(linear + i)) = bytes[i];
    }
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

/* The entry is cleared whole, not just its P flag: a load through a
   not-present entry can still read the L1D line at the frame the entry
   names (Intel L1TF, "How L1TF Works"), so every not-present entry the
   kernel makes names page 0, which holds nothing of the kernel or of any
   process (docs/spec/speculation.md M12). The flush drops the old
   translation, which may be the large page the guard was split from. */
int paging_unmap_direct(UINT64 physical)
{
    UINT64 flags = save_and_disable_interrupts();
    UINT64 *entry = page_entry(DIRECT_MAP_BASE + physical);

    if (entry) {
        *entry = 0;
        flush_translations();
    }
    restore_interrupts(flags);
    return entry != 0;
}

/* The page is RAM the allocator gave the caller, which the direct map
   maps writable, not executable and write-back, with no PAT, PCD or PWT
   bit (nova-boot, map_direct). No TLB or paging-structure cache entry is
   made from an entry whose P flag is 0, so setting it needs no
   invalidation (SDM Vol. 3A 5.10.4.3). */
void paging_restore_direct(UINT64 physical)
{
    UINT64 flags = save_and_disable_interrupts();

    *page_entry(DIRECT_MAP_BASE + physical) =
        physical | PTE_PRESENT | PTE_WRITABLE | no_execute;
    restore_interrupts(flags);
}

/* A translation through a cacheable type may have read lines ahead,
   speculatively, which a later uncached mapping of the page must not
   find: "changing a cache line's memory type from WB to UC to be used
   for memory-mapped I/O" needs the caches flushed after the TLBs (SDM
   Vol. 3A 14.12.4), and WBINVD does that for a range of any size. A
   large page the range covers whole is cleared, one it covers in part
   split, and the tables on the way stay. Nothing past the direct map's
   window is mapped there. */
int paging_unmap_direct_range(UINT64 physical, UINT64 size)
{
    UINT64 linear, end, flags;
    int complete = 1;

    if (physical >= DIRECT_MAP_SIZE)
        return 1;
    if (size > DIRECT_MAP_SIZE - physical)
        size = DIRECT_MAP_SIZE - physical;
    linear = DIRECT_MAP_BASE + physical;
    end = linear + size;
    flags = save_and_disable_interrupts();

    while (complete && linear < end) {
        UINT64 *table = top_table;
        int level = levels;

        for (;;) {
            UINT64 *entry = &table[table_index(linear, level)];
            UINT64 span = level_span(level);

            if (!(*entry & PTE_PRESENT)) {
                linear += span - linear % span;
                break;
            }
            if (level == PAGE_TABLE_LEVEL || (*entry & PTE_LARGE)) {
                if (linear % span == 0 && end - linear >= span) {
                    *entry = 0;
                    linear += span;
                    break;
                }
                if (!split(entry, level)) {
                    complete = 0;
                    break;
                }
            }
            table = table_at(*entry);
            level--;
        }
    }
    flush_translations();
    write_back_and_invalidate_caches();
    restore_interrupts(flags);
    return complete;
}

/* ------------------------------------------------------------------------
   Page-table isolation. */

/* Map the page at linear in the entry tables with the kernel tables'
   physical address and rights: present, writable as the kernel's
   mapping is, executable as it is, and supervisor-only, so user code
   reaches none of it (M11). The tables on the way grant write and leave
   restrictions to the page's entry, as nova-boot's do. */
static void map_entry_page(UINT64 linear)
{
    UINT64 *table = entry_tables[0];
    UINT64 leaf;
    int level, leaf_level;

    if (table_index(linear, levels) != entry_top)
        stop("entry region outside its top-level entry", linear);
    for (level = levels - 1; level > PAGE_TABLE_LEVEL; level--) {
        UINT64 *entry = &table[table_index(linear, level)];

        if (!(*entry & PTE_PRESENT)) {
            if (entry_tables_used == ENTRY_TABLES)
                stop("entry region needs more tables", linear);
            *entry = kernel_physical((UINT64)entry_tables[entry_tables_used]) |
                     PTE_PRESENT | PTE_WRITABLE;
            entry_tables_used++;
        }
        table = table_at(*entry);
    }
    leaf = kernel_leaf(linear, &leaf_level);
    if (!leaf)
        stop("entry region not mapped", linear);
    table[table_index(linear, PAGE_TABLE_LEVEL)] =
        (kernel_physical(linear) & PTE_ADDRESS) | PTE_PRESENT |
        (leaf & (PTE_WRITABLE | PTE_NO_EXECUTE));
}

/* A range that is not whole pages would share a page with data the user
   tables must not map. */
static void map_entry_range(ENTRY_RANGE range)
{
    UINT64 linear;

    if (range.start % PAGE_SIZE != 0 || range.end % PAGE_SIZE != 0)
        stop("entry region not page aligned", range.start);
    for (linear = range.start; linear < range.end; linear += PAGE_SIZE)
        map_entry_page(linear);
}

/* The entry region: the entry and exit code, with VERW's operand, the
   IDT, and the CPU's entry area of GDT, TSS, per-CPU block, entry stack
   and IST stacks (M11). Each range is page aligned at both ends, so no
   other kernel data shares a page with it. */
void paging_isolate(int pcid)
{
    PER_CPU *block = per_cpu_block();
    ENTRY_RANGE range;

    entry_top = table_index((UINT64)entry_text, levels);
    entry_tables_used = 1;
    range.start = (UINT64)entry_text;
    range.end = (UINT64)entry_text_end;
    map_entry_range(range);
    range.start = (UINT64)syscall_text;
    range.end = (UINT64)syscall_text_end;
    map_entry_range(range);
    map_entry_range(idt_entry_range());
    map_entry_range(gdt_entry_range());
    entry_top_entry =
        kernel_physical((UINT64)entry_tables[0]) | PTE_PRESENT | PTE_WRITABLE;
    if (pcid) {
        write_cr4(read_cr4() | CR4_PCIDE);
        cr3_no_flush = CR3_NO_FLUSH;
        user_pcid = USER_PCID;
    }
    block->cr3_no_flush = cr3_no_flush;
    block->kernel_cr3 = kernel_space | KERNEL_PCID | cr3_no_flush;
    tss_set_kernel_stack(block->entry_stack);
    isolation = 1;
}

int paging_isolated(void)
{
    return isolation;
}

static UINT64 *user_table(UINT64 space)
{
    return direct_map(space + PAGE_SIZE);
}

/* Under isolation the CR3 values the entry and exit load become the
   space's, the user table's without CR3_NO_FLUSH, so the next exit drops
   what USER_PCID cached for another space, and the entry area forgets
   what the last process left in it. */
int paging_enter_user_space(UINT64 space)
{
    PER_CPU *block;

    if (space == last_user_space)
        return 0;
    last_user_space = space;
    if (isolation) {
        block = per_cpu_block();
        block->kernel_cr3 = space | KERNEL_PCID | cr3_no_flush;
        block->user_cr3 = (space + PAGE_SIZE) | user_pcid;
        entry_area_wipe();
    }
    return 1;
}

/* A mapping of space was removed. Where space is loaded its translations
   are dropped by loading CR3 again, which without CR3_NO_FLUSH drops the
   current PCID's (SDM Vol. 3A 5.10.4.1); and where it is the last user
   space, the user table's, cached under USER_PCID, at the next exit. No
   user translation is global, and a space not loaded and not the last
   has none cached. */
static void space_changed(UINT64 space)
{
    if ((read_cr3() & PTE_ADDRESS) == space)
        write_cr3(space | KERNEL_PCID);
    if (space == last_user_space)
        per_cpu_block()->user_cr3 &= ~CR3_NO_FLUSH;
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

/* The PWT, PCD and PAT bits of a PTE that select type in NOVA_PAT
   (page_table.h): entry 0 for WB, entry 3 for UC, entry 6 for WC (SDM
   Vol. 3A Table 14-11). */
static UINT64 memory_type_bits(USER_MEMORY_TYPE type)
{
    switch (type) {
    case USER_UNCACHED:
        return PTE_CACHE_DISABLE | PTE_WRITE_THROUGH;
    case USER_WRITE_COMBINING:
        return PTE_WRITE_COMBINING;
    case USER_WRITE_BACK:
        break;
    }
    return 0;
}

/* A PTE has its PAT bit at 7, an entry that maps a large page at 12
   (SDM Vol. 3A Tables 5-16, 5-18 and 5-20); the comparison is in a PTE's
   terms. */
int paging_direct_type_differs(UINT64 physical, USER_MEMORY_TYPE type)
{
    UINT64 linear = DIRECT_MAP_BASE + physical;
    UINT64 *table = top_table;
    UINT64 flags, entry, bits;
    int level, differs = 0;

    if (physical >= DIRECT_MAP_SIZE)
        return 0;
    flags = save_and_disable_interrupts();
    for (level = levels; level >= PAGE_TABLE_LEVEL; level--) {
        entry = table[table_index(linear, level)];
        if (!(entry & PTE_PRESENT))
            break;
        if (level == PAGE_TABLE_LEVEL || (entry & PTE_LARGE)) {
            bits = entry & (PTE_WRITE_THROUGH | PTE_CACHE_DISABLE);
            if (entry & (level == PAGE_TABLE_LEVEL ? PTE_PAT : PTE_LARGE_PAT))
                bits |= PTE_PAT;
            differs = bits != memory_type_bits(type);
            break;
        }
        table = table_at(entry);
    }
    restore_interrupts(flags);
    return differs;
}

_Static_assert(((NOVA_PAT >> (PAT_ENTRY_BITS *
                              (PAT_INDEX_PCD | PAT_INDEX_PWT))) &
                PAT_TYPE_MASK) == PAT_UNCACHEABLE,
               "PCD and PWT select UC");
_Static_assert(((NOVA_PAT >> (PAT_ENTRY_BITS * PAT_WRITE_COMBINING_ENTRY)) &
                PAT_TYPE_MASK) == PAT_WRITE_COMBINING,
               "PAT and PCD select WC");

/* page_alloc's pages read as zero, so the lower half maps nothing. */
UINT64 paging_space_create(void)
{
    UINT64 page = isolation ? page_alloc_run(SPACE_TABLES) : page_alloc();
    UINT64 *table;
    UINT64 i;

    if (!page)
        return 0;
    table = direct_map(page);
    for (i = LOWER_HALF_ENTRIES; i < PAGE_TABLE_ENTRIES; i++)
        table[i] = top_table[i];
    if (isolation)
        user_table(page)[entry_top] = entry_top_entry;
    return page;
}

/* The PTE that maps user address linear in space, adding the tables on
   the way down when add is set; 0 if a table is missing and add is
   clear, or if one is needed and no page is free. With interrupts
   disabled. Under isolation a new top-level entry goes in the user
   table too, which shares every table below it. */
static UINT64 *user_entry(UINT64 space, UINT64 linear, int add)
{
    UINT64 *table = direct_map(space);
    int level;

    for (level = levels; level > PAGE_TABLE_LEVEL; level--) {
        UINT64 *entry = &table[table_index(linear, level)];

        if (!(*entry & PTE_PRESENT)) {
            if (!add || !add_table(entry, PTE_USER))
                return 0;
            if (isolation && level == levels)
                user_table(space)[table_index(linear, level)] = *entry;
        }
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
                             USER_PROTECTION protection,
                             USER_MEMORY_TYPE type)
{
    return map_user(space, linear,
                    physical | protection_bits(protection) |
                        memory_type_bits(type) | PTE_BORROWED);
}

int paging_user_mapped(UINT64 space, UINT64 linear)
{
    UINT64 flags = save_and_disable_interrupts();
    UINT64 *entry = user_entry(space, linear, 0);
    int mapped = entry && (*entry & PTE_PRESENT);

    restore_interrupts(flags);
    return mapped;
}

int paging_tables_agree(UINT64 space)
{
    const UINT64 *top = direct_map(space);
    UINT64 flags, i;
    int agree = 1;

    if (!isolation)
        return 1;
    flags = save_and_disable_interrupts();
    for (i = 0; i < LOWER_HALF_ENTRIES; i++)
        if (user_table(space)[i] != top[i])
            agree = 0;
    restore_interrupts(flags);
    return agree;
}

static int table_empty(const UINT64 *table)
{
    UINT64 i;

    for (i = 0; i < PAGE_TABLE_ENTRIES; i++)
        if (table[i] & PTE_PRESENT)
            return 0;
    return 1;
}

/* Clear the entries of table, at level, that map [linear, end), and free
   each table below it that is left with no entry present, so that no
   table outlives the mappings it held. mirror is the space's user table
   when table is its top table under isolation, whose lower half repeats
   the kernel table's entries, else 0. The pages mapped are borrowed, so
   none is freed. The depth is the paging depth, at most five. */
static void clear_user_range(UINT64 *table, UINT64 *mirror, int level,
                             UINT64 linear, UINT64 end)
{
    UINT64 span = level_span(level), next, index, physical;

    for (; linear < end; linear = next) {
        next = linear - linear % span + span;
        index = table_index(linear, level);
        if (!(table[index] & PTE_PRESENT))
            continue;
        if (level == PAGE_TABLE_LEVEL) {
            table[index] = 0;
            continue;
        }
        physical = table[index] & PTE_ADDRESS;
        clear_user_range(direct_map(physical), 0, level - 1, linear,
                         next < end ? next : end);
        if (table_empty(direct_map(physical))) {
            table[index] = 0;
            if (mirror)
                mirror[index] = 0;
            page_free(physical);
        }
    }
}

/* The processor may cache a translation for a speculative access the
   code never makes (SDM Vol. 3A 5.10.2.2), and paging-structure entries
   for the tables freed here (5.10.3), so the space's cached
   translations are dropped (space_changed): the kernel PCID's here, and
   with PCIDs the user PCID's at the next exit's CR3 load. Until then the
   processor uses none of them, since it uses only the current PCID's
   entries (5.10.1) and the kernel's PCID is current, so a freed table
   or page may be handed out again before that load. A freed table is
   all not-present entries, and the allocator never writes a freed
   page. */
void paging_unmap_user_borrowed(UINT64 space, UINT64 linear, UINT64 size)
{
    UINT64 flags = save_and_disable_interrupts();

    clear_user_range(direct_map(space), isolation ? user_table(space) : 0,
                     levels, linear, linear + size);
    space_changed(space);
    restore_interrupts(flags);
}

UINT32 paging_physical_bits(void)
{
    UINT32 a, b, c, d;

    cpuid(CPUID_EXTENDED_MAX, &a, &b, &c, &d);
    if (a < CPUID_ADDRESS_SIZES)
        return PHYSICAL_BITS_DEFAULT;
    cpuid(CPUID_ADDRESS_SIZES, &a, &b, &c, &d);
    return a & PHYSICAL_BITS_MASK;
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

/* A space keeps translations of what was freed until they are dropped
   (space_changed). */
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
        if (isolation)
            user_table(space)[i] = 0;
    }
    space_changed(space);
    restore_interrupts(flags);
}

/* The per-CPU block's tables become the kernel's if they were space's,
   which an NMI or machine check would otherwise load once freed. */
void paging_space_destroy(UINT64 space)
{
    UINT64 flags;

    paging_space_clear(space);
    flags = save_and_disable_interrupts();
    if (space == last_user_space) {
        last_user_space = 0;
        if (isolation)
            per_cpu_block()->kernel_cr3 =
                kernel_space | KERNEL_PCID | cr3_no_flush;
    }
    restore_interrupts(flags);
    page_free(space);
    if (isolation)
        page_free(space + PAGE_SIZE);
}
