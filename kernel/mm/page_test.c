/* The page allocator's boot-time self-test. Every page it is handed is
   checked, before anything is written to it, against the memory map read
   here, independently of the allocator. It takes TEST_PAGES pages, fills
   them with a pattern no two words share, reads it back, frees them and
   checks a second free is refused. Then it drains the allocator, writing
   to every free page through the direct map, so a page the direct map
   lacks faults here rather than in later use, and frees them all again.
   Each step checks the free count. */

#include "page.h"
#include "efi.h"

#define TEST_PAGES 64
#define WORDS_PER_PAGE (PAGE_SIZE / sizeof(UINT64))

static UINT64 taken[TEST_PAGES];
static UINT64 taken_count;

/* The general descriptor that held the last address asked about. The
   drain asks about pages in runs, so most answers come from it. */
static UINT64 cached_start, cached_pages;

static int within(UINT64 address, UINT64 start, UINT64 pages)
{
    return address >= start && (address - start) / PAGE_SIZE < pages;
}

static int in_general_memory(const BOOT_INFO *info, UINT64 address)
{
    UINTN offset;

    if (within(address, cached_start, cached_pages))
        return 1;
    if (info->descriptor_size < sizeof(EFI_MEMORY_DESCRIPTOR))
        return 0;
    for (offset = 0; offset + info->descriptor_size <= info->memory_map_size;
         offset += info->descriptor_size) {
        const EFI_MEMORY_DESCRIPTOR *d =
            (const EFI_MEMORY_DESCRIPTOR *)(info->memory_map + offset);

        if ((d->Type == EFI_CONVENTIONAL_MEMORY ||
             d->Type == EFI_BOOT_SERVICES_CODE ||
             d->Type == EFI_BOOT_SERVICES_DATA) &&
            within(address, d->PhysicalStart, d->NumberOfPages)) {
            cached_start = d->PhysicalStart;
            cached_pages = d->NumberOfPages;
            return 1;
        }
    }
    return 0;
}

static const char *misplaced(const BOOT_INFO *info, UINT64 address)
{
    if (address % PAGE_SIZE != 0)
        return "page_alloc returned an address that is not page-aligned";
    if (!in_general_memory(info, address))
        return "page_alloc returned a page outside general memory";
    return 0;
}

/* Each word holds its own physical address, so a page that aliases
   another, or keeps a stale word, reads back wrong. */
static void fill(UINT64 address)
{
    UINT64 *words = direct_map(address);
    UINT64 i;

    for (i = 0; i < WORDS_PER_PAGE; i++)
        words[i] = address + i * sizeof(UINT64);
}

static int filled(UINT64 address)
{
    const UINT64 *words = direct_map(address);
    UINT64 i;

    for (i = 0; i < WORDS_PER_PAGE; i++)
        if (words[i] != address + i * sizeof(UINT64))
            return 0;
    return 1;
}

static int taken_before(UINT64 address)
{
    UINT64 i;

    for (i = 0; i + 1 < taken_count; i++)
        if (taken[i] == address)
            return 1;
    return 0;
}

/* The page is recorded before it is checked, so give_back frees it
   whatever the checks find. */
static const char *take(const BOOT_INFO *info)
{
    UINT64 address = page_alloc();
    const char *failure;

    if (!address)
        return "page_alloc found no free page";
    taken[taken_count++] = address;
    failure = misplaced(info, address);
    if (failure)
        return failure;
    if (taken_before(address))
        return "page_alloc returned a page twice";
    fill(address);
    return 0;
}

static const char *check_fills(void)
{
    UINT64 i;

    for (i = 0; i < taken_count; i++)
        if (!filled(taken[i]))
            return "a page did not keep the pattern written to it";
    return 0;
}

static const char *give_back(void)
{
    const char *failure = 0;
    UINT64 i;

    for (i = 0; i < taken_count; i++)
        if (!page_free(taken[i]) && !failure)
            failure = "page_free refused an allocated page";
    return failure;
}

/* Allocate every free page, linking each to the one before through its
   first word, then follow the links to free them. A misplaced page is
   freed unwritten and ends the allocating. */
static const char *drain(const BOOT_INFO *info, UINT64 expected)
{
    UINT64 last = 0, address, allocated = 0, freed = 0;
    const char *failure = 0;

    while (!failure && (address = page_alloc()) != 0) {
        failure = misplaced(info, address);
        if (failure) {
            page_free(address);
        } else {
            *(UINT64 *)direct_map(address) = last;
            last = address;
            allocated++;
        }
    }
    while (last) {
        UINT64 previous = *(const UINT64 *)direct_map(last);

        if (!page_free(last))
            return "page_free refused a drained page";
        last = previous;
        freed++;
    }
    if (failure)
        return failure;
    if (allocated != expected)
        return "the free count disagrees with the pages allocated";
    if (freed != allocated)
        return "a drained page did not keep its link";
    return 0;
}

const char *page_self_test(const BOOT_INFO *info)
{
    UINT64 start = page_free_count();
    const char *failure = 0;
    const char *returning;

    taken_count = 0;
    while (!failure && taken_count < TEST_PAGES)
        failure = take(info);
    if (!failure && page_free_count() != start - TEST_PAGES)
        failure = "the free count did not fall by the pages allocated";
    if (!failure)
        failure = check_fills();
    returning = give_back();
    if (!failure)
        failure = returning;
    if (!failure && page_free(taken[0]))
        failure = "page_free freed a page that was already free";
    if (!failure && page_free_count() != start)
        failure = "the free count did not return to its start";
    if (!failure)
        failure = drain(info, start);
    if (!failure && page_free_count() != start)
        failure = "the free count did not return to its start after the drain";
    return failure;
}
