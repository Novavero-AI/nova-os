/* The physical page allocator: a bitmap with one bit per page from
   address 0 to the end of the highest general memory, set while the page
   is allocated or reserved. The bitmap lives in conventional memory,
   reached through the direct map, and is marked allocated in itself.

   A bitmap rather than a free list threaded through the free pages: at
   one bit per page (32 KiB per GiB of address space) it sits in one
   place, so building it writes to no page but its own and nothing needs
   the free pages mapped, page_free can refuse a page that is already
   free, and consecutive free pages are a run of clear bits. page_alloc
   scans from the lowest word that can hold a free page, so the lowest
   free page goes first.

   General memory is EfiConventionalMemory, EfiBootServicesCode and
   EfiBootServicesData, which the OS owns once boot services have exited
   (UEFI 2.11, 7.2, Table 7.10 and the note after it). EfiLoaderCode and
   EfiLoaderData are the OS's too, but hold the kernel image, nova-boot's
   image with BOOT_INFO, and the memory map, so they stay reserved with
   every other type; so do the kernel's stack and page tables, which
   nova-boot allocates as EfiLoaderData. Within general memory, page 0
   stays reserved, since page_alloc returns 0 for no page. */

#include "page.h"
#include "cpu.h"
#include "efi.h"

_Static_assert(PAGE_SIZE == EFI_PAGE_SIZE, "a page is a memory map page");

#define BITS_PER_WORD 64
#define ALL_SET 0xFFFFFFFFFFFFFFFFULL

/* One past the last page a descriptor may reach: no page of one starts
   above 0xfffffffffffff000 (UEFI 2.11, 7.2.3). */
#define MAP_END_PAGE 0x10000000000000ULL

static UINT64 *bitmap;
static UINT64 bitmap_words;
static UINT64 limit_page; /* the bitmap covers pages below it */
static UINT64 free_pages;
static UINT64 total_pages;
static UINT64 search_word; /* no free page lies in a word below it */

/* ------------------------------------------------------------------------
   The memory map. Descriptors are descriptor_size apart, which may exceed
   the structure (UEFI 2.11, 7.2.3). */

static UINT64 descriptor_count(const BOOT_INFO *info)
{
    if (info->descriptor_size < sizeof(EFI_MEMORY_DESCRIPTOR))
        return 0;
    return info->memory_map_size / info->descriptor_size;
}

static const EFI_MEMORY_DESCRIPTOR *descriptor_at(const BOOT_INFO *info,
                                                  UINT64 index)
{
    return (const EFI_MEMORY_DESCRIPTOR *)(info->memory_map +
                                           index * info->descriptor_size);
}

/* A descriptor the page loops can trust: page-aligned, not empty, and
   within the address space UEFI allows. A 64-bit start always lies below
   MAP_END_PAGE, so only the length needs bounding. */
static int well_formed(const EFI_MEMORY_DESCRIPTOR *d)
{
    return d->PhysicalStart % PAGE_SIZE == 0 && d->NumberOfPages != 0 &&
           d->NumberOfPages <= MAP_END_PAGE - d->PhysicalStart / PAGE_SIZE;
}

static int general(const EFI_MEMORY_DESCRIPTOR *d)
{
    return d->Type == EFI_CONVENTIONAL_MEMORY ||
           d->Type == EFI_BOOT_SERVICES_CODE ||
           d->Type == EFI_BOOT_SERVICES_DATA;
}

static UINT64 first_page(const EFI_MEMORY_DESCRIPTOR *d)
{
    return d->PhysicalStart / PAGE_SIZE;
}

static UINT64 end_page(const EFI_MEMORY_DESCRIPTOR *d)
{
    return first_page(d) + d->NumberOfPages;
}

static UINT64 general_end(const BOOT_INFO *info)
{
    UINT64 i, end = 0;

    for (i = 0; i < descriptor_count(info); i++) {
        const EFI_MEMORY_DESCRIPTOR *d = descriptor_at(info, i);

        if (general(d) && well_formed(d) && end_page(d) > end)
            end = end_page(d);
    }
    return end;
}

/* ------------------------------------------------------------------------
   A home for the bitmap, found before the bitmap exists: the first page of
   the first run of `pages` pages inside one conventional descriptor, in
   map order, leaving out page 0; or 0 if there is none. Conventional
   memory is unallocated (UEFI 2.11, Table 7.9), so it holds nothing the
   firmware or nova-boot still uses. */

static UINT64 find_run(const BOOT_INFO *info, UINT64 pages)
{
    UINT64 i;

    for (i = 0; i < descriptor_count(info); i++) {
        const EFI_MEMORY_DESCRIPTOR *d = descriptor_at(info, i);
        UINT64 run = first_page(d) == 0 ? 1 : first_page(d);

        if (d->Type == EFI_CONVENTIONAL_MEMORY && well_formed(d) &&
            pages <= end_page(d) - run)
            return run;
    }
    return 0;
}

/* ------------------------------------------------------------------------
   The bitmap. */

static UINT64 word_of(UINT64 page) { return page / BITS_PER_WORD; }

static UINT64 bit_of(UINT64 page) { return 1ULL << (page % BITS_PER_WORD); }

static int is_set(UINT64 page)
{
    return (bitmap[word_of(page)] & bit_of(page)) != 0;
}

static void release(UINT64 page)
{
    if (is_set(page)) {
        bitmap[word_of(page)] &= ~bit_of(page);
        free_pages++;
    }
}

static void hold(UINT64 page)
{
    if (page < limit_page && !is_set(page)) {
        bitmap[word_of(page)] |= bit_of(page);
        free_pages--;
    }
}

static void hold_run(UINT64 first, UINT64 pages)
{
    UINT64 page;

    for (page = first; page < first + pages; page++)
        hold(page);
}

static void release_general(const BOOT_INFO *info)
{
    UINT64 i, page;

    for (i = 0; i < descriptor_count(info); i++) {
        const EFI_MEMORY_DESCRIPTOR *d = descriptor_at(info, i);

        if (!general(d) || !well_formed(d))
            continue;
        total_pages += d->NumberOfPages;
        for (page = first_page(d); page < end_page(d); page++)
            if (page != 0)
                release(page);
    }
}

int page_init(const BOOT_INFO *info)
{
    UINT64 end = general_end(info);
    UINT64 words = (end + BITS_PER_WORD - 1) / BITS_PER_WORD;
    UINT64 pages = (words * sizeof(UINT64) + PAGE_SIZE - 1) / PAGE_SIZE;
    UINT64 home = find_run(info, pages);
    UINT64 i;

    if (!home)
        return 0;
    bitmap = direct_map(home * PAGE_SIZE);
    bitmap_words = words;
    limit_page = end;
    for (i = 0; i < words; i++)
        bitmap[i] = ALL_SET;
    release_general(info);
    hold_run(home, pages);
    return 1;
}

/* ------------------------------------------------------------------------
   Allocation. */

/* bsf gives the lowest set bit of its source, which must not be 0, so
   word must have a clear bit. */
static UINT64 lowest_clear_bit(UINT64 word)
{
    UINT64 bit;

    __asm__("bsf %1, %0" : "=r"(bit) : "r"(~word));
    return bit;
}

UINT64 page_alloc(void)
{
    UINT64 flags = save_and_disable_interrupts();
    UINT64 address = 0;

    for (; search_word < bitmap_words; search_word++)
        if (bitmap[search_word] != ALL_SET)
            break;
    if (search_word < bitmap_words) {
        UINT64 page = search_word * BITS_PER_WORD +
                      lowest_clear_bit(bitmap[search_word]);

        hold(page);
        address = page * PAGE_SIZE;
    }
    restore_interrupts(flags);
    return address;
}

/* The lowest run goes first, as with single pages. The scan starts at
   search_word, below which no page is free, and leaves it there: the
   words it passes may still hold free pages. */
UINT64 page_alloc_run(UINT64 pages)
{
    UINT64 flags = save_and_disable_interrupts();
    UINT64 page, run = 0, address = 0;

    for (page = search_word * BITS_PER_WORD; pages && page < limit_page;
         page++) {
        if (is_set(page)) {
            run = 0;
        } else if (++run == pages) {
            hold_run(page + 1 - pages, pages);
            address = (page + 1 - pages) * PAGE_SIZE;
            break;
        }
    }
    restore_interrupts(flags);
    return address;
}

int page_free(UINT64 address)
{
    UINT64 page = address / PAGE_SIZE;
    UINT64 flags;
    int freed = 0;

    if (address % PAGE_SIZE != 0 || page == 0 || page >= limit_page)
        return 0;
    flags = save_and_disable_interrupts();
    if (is_set(page)) {
        release(page);
        if (word_of(page) < search_word)
            search_word = word_of(page);
        freed = 1;
    }
    restore_interrupts(flags);
    return freed;
}

UINT64 page_free_count(void) { return free_pages; }

UINT64 page_total_count(void) { return total_pages; }
