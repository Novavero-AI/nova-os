/* Building 4-level or 5-level page tables (page_table.h) one entry at a
   time. Every table is one EfiLoaderData page, so the kernel's page
   allocator leaves the tables alone once they are live. */

#include "tables.h"
#include "memory_layout.h"
#include "page_table.h"

_Static_assert(PAGE_SIZE == EFI_PAGE_SIZE, "a table is one UEFI page");

/* CPUID.80000001H:EDX reports execute-disable support in bit 20 and
   1 GiB pages in bit 26; a processor without that leaf has neither (SDM
   Vol. 3A 5.1.4). */
#define CPUID_EXTENDED_MAX 0x80000000u
#define CPUID_EXTENDED_FEATURES 0x80000001u
#define CPUID_NO_EXECUTE (1u << 20)
#define CPUID_PAGES_1GB (1u << 26)

static EFI_STATUS new_table(PAGE_TABLES *tables, UINT64 *table)
{
    EFI_STATUS status;
    UINT64 *entries;
    UINT64 i;

    status = tables->boot->AllocatePages(ALLOCATE_ANY_PAGES, EFI_LOADER_DATA,
                                         1, table);
    if (status != EFI_SUCCESS)
        return status;
    entries = (UINT64 *)*table;
    for (i = 0; i < PAGE_TABLE_ENTRIES; i++)
        entries[i] = 0;
    return EFI_SUCCESS;
}

EFI_STATUS tables_create(EFI_BOOT_SERVICES *boot, PAGE_TABLES *tables)
{
    UINT32 a, b, c, d;

    tables->boot = boot;
    tables->top = 0;
    tables->levels = paging_levels();
    tables->largest_level = PAGE_DIRECTORY_LEVEL;
    tables->no_execute = 0;
    cpuid(CPUID_EXTENDED_MAX, &a, &b, &c, &d);
    if (a >= CPUID_EXTENDED_FEATURES) {
        cpuid(CPUID_EXTENDED_FEATURES, &a, &b, &c, &d);
        if (d & CPUID_NO_EXECUTE)
            tables->no_execute = PTE_NO_EXECUTE;
        if (d & CPUID_PAGES_1GB)
            tables->largest_level = PDPT_LEVEL;
    }
    return new_table(tables, &tables->top);
}

/* Map one page of level's size. The entries on the way down grant write
   and execute and leave the restrictions to the entry that maps the page,
   since a translation's rights are the most restrictive of every entry it
   uses (SDM Vol. 3A 5.6.1). */
static EFI_STATUS map_page(PAGE_TABLES *tables, UINT64 linear,
                           UINT64 physical, int level, UINT64 flags)
{
    UINT64 *table = (UINT64 *)tables->top;
    UINT64 *entry;
    int at;

    for (at = tables->levels; at > level; at--) {
        entry = &table[table_index(linear, at)];
        if (!(*entry & PTE_PRESENT)) {
            UINT64 next;
            EFI_STATUS status = new_table(tables, &next);

            if (status != EFI_SUCCESS)
                return status;
            *entry = next | PTE_PRESENT | PTE_WRITABLE;
        } else if (*entry & PTE_LARGE) {
            return EFI_LOAD_ERROR;
        }
        table = (UINT64 *)(*entry & PTE_ADDRESS);
    }
    entry = &table[table_index(linear, level)];
    if (*entry & PTE_PRESENT)
        return EFI_LOAD_ERROR;
    *entry = physical | flags | PTE_PRESENT |
             (level > PAGE_TABLE_LEVEL ? PTE_LARGE : 0);
    return EFI_SUCCESS;
}

EFI_STATUS tables_map(PAGE_TABLES *tables, UINT64 linear, UINT64 physical,
                      UINT64 size, UINT64 flags, int largest_level)
{
    if ((linear | physical | size) % PAGE_SIZE != 0)
        return EFI_LOAD_ERROR;
    if (!tables->no_execute)
        flags &= ~PTE_NO_EXECUTE;
    if (largest_level > tables->largest_level)
        largest_level = tables->largest_level;
    while (size != 0) {
        int level = largest_level;
        EFI_STATUS status;

        while (level > PAGE_TABLE_LEVEL &&
               ((linear | physical) % level_span(level) != 0 ||
                size < level_span(level)))
            level--;
        status = map_page(tables, linear, physical, level, flags);
        if (status != EFI_SUCCESS)
            return status;
        linear += level_span(level);
        physical += level_span(level);
        size -= level_span(level);
    }
    return EFI_SUCCESS;
}

static void free_table(EFI_BOOT_SERVICES *boot, UINT64 table, int level)
{
    const UINT64 *entries = (const UINT64 *)table;
    UINT64 i;

    if (level > PAGE_TABLE_LEVEL)
        for (i = 0; i < PAGE_TABLE_ENTRIES; i++)
            if ((entries[i] & PTE_PRESENT) && !(entries[i] & PTE_LARGE))
                free_table(boot, entries[i] & PTE_ADDRESS, level - 1);
    boot->FreePages(table, 1);
}

void tables_free(PAGE_TABLES *tables)
{
    if (tables->top)
        free_table(tables->boot, tables->top, tables->levels);
    tables->top = 0;
}
