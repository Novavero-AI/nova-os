/* Host check of mapping_section_bytes in lib/mapping.c: how many bytes a
   section holds, found with process_map and process_unmap alone, since no
   call reports a section's size.

   The calls are stubs over a section of a given number of pages that map a
   range if the section holds it and refuse it with ERR_OUT_OF_RANGE if
   not, as the kernel does, and keep the ranges mapped, so that a case
   checks what the kernel would: that a range is mapped at the address the
   mapping gives and unmapped as it was mapped, that none stays mapped, that
   no range is empty, and that the calls are few. The sizes the library
   gives are those of every section of 0 to SECTION_PAGES pages and every
   limit the sizes around a page and a section's own; a call that fails,
   at each place in each case, ends the search with its status and no
   byte, which a refusal other than ERR_OUT_OF_RANGE is not mistaken for.

   cc -std=c11 -Wall -Wextra -pedantic -I include -I lib -I native -I gfx
     tools/check-mapping.c lib/mapping.c */

#include <stdio.h>

#include "mapping.h"
#include "native.h"

#define FAILURES_SHOWN 12

/* The sections tried hold up to this many pages: a whole section's
   SECTION_MAX_SIZE and a few more, which a limit beyond it asks about. */
#define SECTION_PAGES (SECTION_MAX_SIZE / PAGE_SIZE)
#define PAGES_TRIED (SECTION_PAGES + 3)

/* Where the program maps next, and its own process and the section's
   handles, which the stubs read. */
#define NEXT 0x100000000ull
#define SELF 7
#define SECTION 9

/* The ranges the stubs keep mapped at once: the search keeps none past a
   call, so one is the most that is ever live. */
#define LIVE_MAX 4

static unsigned failures;
static unsigned long long checks;

static void check(int passed, const char *what, long long a, long long b)
{
    checks++;
    if (!passed && failures++ < FAILURES_SHOWN)
        printf("failed: %s (%lld, %lld)\n", what, a, b);
}

/* What the stubs do: the section's pages, the status the call numbered
   fail_at, counting from 1 over maps and unmaps together, fails with
   (0 for none), and what they have seen. */
static struct {
    UINT64 pages;
    unsigned fail_at;
    UINT32 fail_status;
    unsigned calls;
    unsigned maps;
    unsigned live;
    UINT64 live_address[LIVE_MAX];
    UINT64 live_size[LIVE_MAX];
    unsigned misuse;
} world;

static void reset(UINT64 pages, unsigned fail_at, UINT32 fail_status)
{
    world.pages = pages;
    world.fail_at = fail_at;
    world.fail_status = fail_status;
    world.calls = 0;
    world.maps = 0;
    world.live = 0;
    world.misuse = 0;
}

UINT32 process_map(UINT32 process, UINT32 section, UINT64 offset,
                   UINT64 address, UINT64 size, UINT32 protection)
{
    world.calls++;
    world.maps++;
    if (process != SELF || section != SECTION || offset != 0 ||
        address != NEXT || size == 0 || size % PAGE_SIZE ||
        protection != MAP_READ || world.live == LIVE_MAX)
        world.misuse++;
    if (world.calls == world.fail_at)
        return world.fail_status;
    if (size > world.pages * PAGE_SIZE)
        return ERR_OUT_OF_RANGE;
    if (world.live < LIVE_MAX) {
        world.live_address[world.live] = address;
        world.live_size[world.live] = size;
        world.live++;
    }
    return OK;
}

UINT32 process_unmap(UINT32 process, UINT64 address, UINT64 size)
{
    unsigned i;

    world.calls++;
    if (process != SELF)
        world.misuse++;
    if (world.calls == world.fail_at)
        return world.fail_status;
    for (i = 0; i < world.live; i++) {
        if (world.live_address[i] == address && world.live_size[i] == size) {
            world.live--;
            world.live_address[i] = world.live_address[world.live];
            world.live_size[i] = world.live_size[world.live];
            return OK;
        }
    }
    world.misuse++;
    return ERR_BAD_STATE;
}

/* The rest of lib/mapping.c's calls, which the search makes none of. */
UINT32 section_create(UINT64 size, UINT32 options, UINT32 *section)
{
    (void)size;
    (void)options;
    (void)section;
    world.misuse++;
    return ERR_NOT_SUPPORTED;
}

UINT32 section_create_physical(UINT32 resource, UINT64 base, UINT64 size,
                               UINT32 cache, UINT32 *section)
{
    (void)resource;
    (void)base;
    (void)size;
    (void)cache;
    (void)section;
    world.misuse++;
    return ERR_NOT_SUPPORTED;
}

UINT32 handle_close(UINT32 handle)
{
    (void)handle;
    world.misuse++;
    return ERR_NOT_SUPPORTED;
}

/* The pages the search for a limit of pages asks about at most: one
   for the limit and a bisection of them, that is the bits of the
   page count. */
static unsigned most_maps(UINT64 pages)
{
    unsigned bits = 0;

    for (; pages; pages >>= 1)
        bits++;
    return 1 + bits;
}

static UINT64 smaller(UINT64 a, UINT64 b)
{
    return a < b ? a : b;
}

/* One search over a section of pages pages and a limit of limit bytes. */
static void search(UINT64 pages, UINT64 limit)
{
    MAPPING mapping = {SELF, NEXT};
    UINT32 status = ERR_NOT_SUPPORTED;
    UINT64 got;

    reset(pages, 0, 0);
    got = mapping_section_bytes(&mapping, SECTION, limit, &status);
    check(status == OK, "the search succeeds", (long long)pages,
          (long long)limit);
    check(got == smaller(pages, limit / PAGE_SIZE) * PAGE_SIZE,
          "it finds the pages the section holds, up to the limit",
          (long long)pages, (long long)limit);
    check(world.live == 0, "no range stays mapped", (long long)pages,
          (long long)limit);
    check(world.misuse == 0,
          "every call is of the program's own process, the section, the "
          "address next, whole pages, read only and never empty",
          (long long)pages, (long long)limit);
    check(world.maps <= most_maps(limit / PAGE_SIZE),
          "it maps no more than a bisection does", (long long)pages,
          (long long)limit);
    check(mapping.next == NEXT, "it leaves the next address where it was",
          (long long)pages, (long long)limit);
}

/* The same search with the call numbered fail_at failing with status. */
static void fail(UINT64 pages, UINT64 limit, unsigned fail_at,
                 UINT32 failing)
{
    MAPPING mapping = {SELF, NEXT};
    UINT32 status = OK;
    UINT64 got;

    reset(pages, fail_at, failing);
    got = mapping_section_bytes(&mapping, SECTION, limit, &status);
    check(status == failing, "a call that fails gives its status",
          (long long)fail_at, (long long)failing);
    check(got == 0, "and no byte", (long long)fail_at, (long long)failing);
    check(world.misuse == 0, "and no misuse before it", (long long)fail_at,
          (long long)failing);
}

/* The limits tried: around no page, one, a section, and the sizes the
   pages tried reach, each with a byte under and over. */
static void limits(UINT64 pages)
{
    UINT64 page;

    search(pages, 0);
    search(pages, PAGE_SIZE - 1);
    for (page = 1; page <= PAGES_TRIED; page++) {
        search(pages, page * PAGE_SIZE - 1);
        search(pages, page * PAGE_SIZE);
        search(pages, page * PAGE_SIZE + 1);
    }
    search(pages, SECTION_MAX_SIZE);
}

int main(void)
{
    UINT64 pages, limit;
    unsigned at, calls;
    static const UINT32 statuses[] = {ERR_NO_MEMORY, ERR_ACCESS_DENIED,
                                      ERR_BAD_STATE, ERR_INVALID_ARGS};
    unsigned i;

    for (pages = 0; pages <= PAGES_TRIED; pages++)
        limits(pages);

    /* A call failing at each place: how many calls a search makes is
       found with none failing. */
    for (pages = 0; pages <= 20; pages++) {
        for (limit = PAGE_SIZE; limit <= 24 * PAGE_SIZE; limit += PAGE_SIZE) {
            MAPPING mapping = {SELF, NEXT};
            UINT32 status;

            reset(pages, 0, 0);
            mapping_section_bytes(&mapping, SECTION, limit, &status);
            calls = world.calls;
            for (at = 1; at <= calls; at++)
                for (i = 0; i < sizeof statuses / sizeof statuses[0]; i++)
                    fail(pages, limit, at, statuses[i]);
        }
    }

    printf("%llu checks, %u failed\n", checks, failures);
    return failures ? 1 : 0;
}
