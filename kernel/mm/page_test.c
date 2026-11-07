/* The page allocator's boot-time self-test. It first walks every free
   page and checks it against the memory map read here, independently of
   the allocator, and their number against the free count: allocating a
   page zeroes it, so a page outside general memory has to be found while
   it is still free. The walk reads no page, so a page the direct map
   lacks faults when it is first allocated. Then it takes TEST_PAGES
   pages, checks each reads as zero, fills them with a pattern no two
   words share, reads it back, frees them and checks a second free is
   refused. It takes the same pages again, freed with the pattern in
   them, and checks they read as zero; then fills a run of TEST_PAGES
   pages, frees it, takes it again and checks it reads as zero. The free
   count is checked after each round. */

#include "page.h"
#include "efi.h"

#define TEST_PAGES 64
#define WORDS_PER_PAGE (PAGE_SIZE / sizeof(UINT64))

static UINT64 taken[TEST_PAGES];
static UINT64 taken_count;

/* The general descriptor that held the last address asked about. The
   walk asks about pages in order, so most answers come from it. */
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

/* Every free page, before any is allocated. */
static const char *walk(const BOOT_INFO *info, UINT64 expected)
{
    UINT64 address = 0, count = 0;

    while ((address = page_next_free(address)) != 0) {
        if (!in_general_memory(info, address))
            return "a free page lies outside general memory";
        count++;
    }
    if (count != expected)
        return "the free count disagrees with the free pages";
    return 0;
}

static const char *misplaced(const BOOT_INFO *info, UINT64 address)
{
    if (address % PAGE_SIZE != 0)
        return "the allocator returned an address that is not page-aligned";
    if (!in_general_memory(info, address))
        return "the allocator returned a page outside general memory";
    return 0;
}

static int zeroed(UINT64 address)
{
    const UINT64 *words = direct_map(address);
    UINT64 i;

    for (i = 0; i < WORDS_PER_PAGE; i++)
        if (words[i] != 0)
            return 0;
    return 1;
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
    if (!zeroed(address))
        return "page_alloc returned a page that does not read as zero";
    fill(address);
    return 0;
}

/* page_alloc hands out the lowest free page first, and the pages
   give_back freed are the lowest free, so they come back in the order
   they were taken. */
static const char *take_again(void)
{
    UINT64 address = page_alloc();

    if (address != taken[taken_count]) {
        if (address)
            page_free(address);
        return "page_alloc did not hand back the pages just freed";
    }
    taken_count++;
    if (!zeroed(address))
        return "a page freed with data in it did not read as zero again";
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

static void free_run(UINT64 run)
{
    UINT64 i;

    for (i = 0; i < TEST_PAGES; i++)
        page_free(run + i * PAGE_SIZE);
}

static const char *check_run(const BOOT_INFO *info, UINT64 run)
{
    UINT64 i;

    for (i = 0; i < TEST_PAGES; i++) {
        const char *failure = misplaced(info, run + i * PAGE_SIZE);

        if (failure)
            return failure;
        if (!zeroed(run + i * PAGE_SIZE))
            return "page_alloc_run returned a page that does not read as zero";
    }
    return 0;
}

/* The lowest run is filled and freed; with nothing allocated in between,
   the same run comes back. */
static const char *run_twice(const BOOT_INFO *info)
{
    UINT64 run = page_alloc_run(TEST_PAGES), again, i;
    const char *failure;

    if (!run)
        return "page_alloc_run found no free run";
    failure = check_run(info, run);
    if (!failure)
        for (i = 0; i < TEST_PAGES; i++)
            fill(run + i * PAGE_SIZE);
    free_run(run);
    if (failure)
        return failure;
    again = page_alloc_run(TEST_PAGES);
    if (again != run) {
        if (again)
            free_run(again);
        return "page_alloc_run did not hand back the run just freed";
    }
    failure = check_run(info, again);
    free_run(again);
    return failure;
}

/* The pages give_back freed, taken again with the pattern still in them,
   then a run the same way. */
static const char *reuse(const BOOT_INFO *info)
{
    const char *failure = 0;
    const char *returning;

    taken_count = 0;
    while (!failure && taken_count < TEST_PAGES)
        failure = take_again();
    returning = give_back();
    if (!failure)
        failure = returning;
    if (!failure)
        failure = run_twice(info);
    return failure;
}

const char *page_self_test(const BOOT_INFO *info)
{
    UINT64 start = page_free_count();
    const char *failure = walk(info, start);
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
        failure = reuse(info);
    if (!failure && page_free_count() != start)
        failure = "the free count did not return to its start after reuse";
    return failure;
}
