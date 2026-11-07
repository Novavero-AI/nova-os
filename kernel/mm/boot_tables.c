/* Reading the firmware's page tables. Entries follow x86-64 paging as
   EDK2 defines it (MdeModulePkg/Core/DxeIplPeim/X64/VirtualMemory.h,
   edk2-stable202608): bit 0 present, bit 1 writable, bit 7 a large page
   in the second and third levels (2 MiB and 1 GiB), and, in an entry
   that is not a page, the next table's address in bits 12 to 51, which
   is also how CR3 holds the top table. Each level indexes 512 entries by
   9 bits of the address, above the 12 bits of the offset into a page.
   Under memory encryption (AMD SEV) one of bits 12 to 51 marks encrypted
   pages instead, which EDK2 strips as AddressEncMask; this walk does not,
   so it reads the tables only of a machine without it. */

#include "boot_tables.h"

#define ENTRY_PRESENT 0x1ULL
#define ENTRY_WRITABLE 0x2ULL
#define ENTRY_LARGE 0x80ULL
#define ENTRY_ADDRESS 0x000FFFFFFFFFF000ULL

#define ENTRIES_PER_TABLE 512
#define OFFSET_BITS 12
#define INDEX_BITS 9

#define LOWEST_LEVEL 1
#define LOWEST_LARGE_LEVEL 2
#define HIGHEST_LARGE_LEVEL 3

/* CR0.WP makes the writable bit bind the kernel as well as user code, and
   CR4.LA57 adds a fifth level above the usual four (EDK2's IA32_CR0 and
   IA32_CR4 in MdePkg/Include/Library/BaseLib.h, and LevelOfPaging in
   VirtualMemory.c). */
#define CR0_WRITE_PROTECT (1ULL << 16)
#define CR4_FIVE_LEVELS (1ULL << 12)
#define FOUR_LEVELS 4
#define FIVE_LEVELS 5

static UINT64 read_cr0(void)
{
    UINT64 value;

    __asm__ volatile("mov %%cr0, %0" : "=r"(value));
    return value;
}

static UINT64 read_cr3(void)
{
    UINT64 value;

    __asm__ volatile("mov %%cr3, %0" : "=r"(value));
    return value;
}

static UINT64 read_cr4(void)
{
    UINT64 value;

    __asm__ volatile("mov %%cr4, %0" : "=r"(value));
    return value;
}

static int top_level(void)
{
    return (read_cr4() & CR4_FIVE_LEVELS) ? FIVE_LEVELS : FOUR_LEVELS;
}

static const UINT64 *table_at(UINT64 entry)
{
    return (const UINT64 *)(entry & ENTRY_ADDRESS);
}

static int maps_page(UINT64 entry, int level)
{
    return level == LOWEST_LEVEL ||
           (level >= LOWEST_LARGE_LEVEL && level <= HIGHEST_LARGE_LEVEL &&
            (entry & ENTRY_LARGE));
}

static UINT64 index_at(UINT64 address, int level)
{
    return (address >> (OFFSET_BITS + INDEX_BITS * (level - LOWEST_LEVEL))) &
           (ENTRIES_PER_TABLE - 1);
}

/* A write needs the writable bit in every entry on the way down, as
   EDK2's CpuPageTableLib combines them (PageTableLibGetPleBMapAttribute
   and its siblings in CpuPageTableParse.c), unless CR0.WP is clear. */
int boot_tables_writable(UINT64 address)
{
    const UINT64 *table = table_at(read_cr3());
    UINT64 rights = ENTRY_WRITABLE;
    int level;

    for (level = top_level();; level--) {
        UINT64 entry = table[index_at(address, level)];

        if (!(entry & ENTRY_PRESENT))
            return 0;
        rights &= entry;
        if (maps_page(entry, level))
            break;
        table = table_at(entry);
    }
    return (rights & ENTRY_WRITABLE) || !(read_cr0() & CR0_WRITE_PROTECT);
}

static void visit_table(const UINT64 *table, int level,
                        void (*visit)(UINT64 page))
{
    UINT64 i;

    visit((UINT64)table);
    for (i = 0; i < ENTRIES_PER_TABLE; i++)
        if ((table[i] & ENTRY_PRESENT) && !maps_page(table[i], level))
            visit_table(table_at(table[i]), level - 1, visit);
}

void boot_tables_each_page(void (*visit)(UINT64 page))
{
    visit_table(table_at(read_cr3()), top_level(), visit);
}
