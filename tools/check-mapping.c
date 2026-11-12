/* Host check of lib/mapping.c's calls that map a section the program was
   handed: mapping_section_bytes, how many bytes a section holds, found
   with process_map and process_unmap alone, since no call reports a
   section's size; mapping_image, an image's sections one after another
   at the next address; and the ranges, each of which holds one image at
   a time at addresses kept for it (docs/spec/display.md D6).

   The calls are stubs that keep the mappings as the kernel keeps a
   process's (kernel/object/process.c): a range is mapped if its section
   holds it, no page of it is mapped already and the table of TABLE_MAX
   mappings has room, and refused with ERR_OUT_OF_RANGE, ERR_BAD_STATE
   or ERR_NO_MEMORY if not, the first of them that applies; an unmapping
   removes whole mappings that fill its range and no others, and is
   refused if they do not. So a call of the library that the kernel
   would refuse is refused here, and fails its case.

   The search: a range is mapped at the address the mapping gives and
   unmapped as it was mapped, none stays mapped, no two are mapped at
   once, no range is empty, and the calls are few. The sizes the library
   gives are those of every section of 0 to SECTION_PAGES pages and every
   limit the sizes around a page and a section's own; a call that fails,
   at each place in each case, ends the search with its status and no
   byte, which a refusal other than ERR_OUT_OF_RANGE is not mistaken for.
   Past ranges that hold images a search still finds its section's size,
   since nothing is mapped where it maps, while the table has one mapping
   left, and is refused with ERR_NO_MEMORY where the images fill it, as
   display.md D6 counts a buffer's size reads.

   The ranges, as laws over GIVES images given one after another: to one
   range, to the ranges in turn, and to ranges and of section counts
   drawn at random, where the table refuses each image it has no room
   for beside the other ranges'. After every one that is mapped the
   image starts where the range's first did and the range holds its
   sections, each at its place, and no other mapping; after every one
   the table refuses, with ERR_NO_MEMORY, the range holds nothing. Either
   way the mapping's next address is where the reservations left it and
   the other ranges hold what they held, so the addresses used do not
   grow with the images given. The first process_map of an image finds
   nothing mapped in its range, and every one after it maps a section of
   that image, so that no two images of a range are mapped at once, and
   the most sections ever mapped at once in one range are one image's.

   A refusal: for an image of every count of sections, process_map
   failing at each of its sections in turn, with each status, in a range
   that holds no image and in one that holds one. The image is refused
   with that status, nothing of it or of the image the range held stays
   mapped, the other ranges' images stay, and the next image given to the
   range is mapped at the range's start. An unmapping that fails refuses
   the image with its status before any section is mapped, and the range
   holds what it held.

   mapping_image: the image starts at the next address, which moves past
   its whole pages, and one refused at each of its sections leaves
   nothing mapped and the next address where it was.

   It does not show what the compositor gives the library or what else
   the compositor holds of the kernel's table, which display.md D6
   counts: compositor/serve.c is read, and the boots run it for the
   first buffer given under each number alone.

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

/* Where the program maps next, and its own process and the searched
   section's handle, which the stubs read. */
#define NEXT 0x100000000ull
#define SELF 7
#define SECTION 9

/* The ranges kept, as many as a window has buffer numbers, and the images
   given under each law. */
#define RANGES 3
#define GIVES 1000000u

/* The handle of the first section of the first image given; each image's
   sections take the CHANNEL_MAX_HANDLES handles after the last image's, so
   that a handle names one section of one image. */
#define IMAGE_FIRST_HANDLE 64

/* The sections of the image the one-range and in-turn laws give, a
   2880 x 1800 buffer's (display.md D6). */
#define USUAL_SECTIONS 10

/* The mappings the stubs keep for the process at once, the kernel's
   PROCESS_MAX_MAPPINGS (kernel/object/process.h), which is not a header
   a host build reads. */
#define TABLE_MAX 128

/* The stubs keep the mappings apart for each zone of MAPPING_RANGE_BYTES
   of addresses from NEXT, in the order of their addresses, so that a
   call looks at those of its own zone alone and a give takes time in
   proportion to its sections: each range kept from NEXT is a zone, and
   the zone after them is where a search maps once they are kept. A zone
   holds the largest image's sections, or a search's one. */
#define ZONES (RANGES + 1)
#define ZONE_LIVE_MAX CHANNEL_MAX_HANDLES

/* The multiplier and increment of the generator the laws draw from:
   Knuth's MMIX constants, a full-period generator modulo 2^64. */
#define DRAW_MULTIPLIER 6364136223846793005ull
#define DRAW_INCREMENT 1442695040888963407ull
#define DRAW_SHIFT 33

static unsigned failures;
static unsigned long long checks;

static void check(int passed, const char *what, long long a, long long b)
{
    checks++;
    if (!passed && failures++ < FAILURES_SHOWN)
        printf("failed: %s (%lld, %lld)\n", what, a, b);
}

static UINT64 drawn = 1;

/* A number below limit, drawn in a fixed order. */
static unsigned below(unsigned limit)
{
    drawn = drawn * DRAW_MULTIPLIER + DRAW_INCREMENT;
    return (unsigned)((drawn >> DRAW_SHIFT) % limit);
}

/* What one process_map call mapped and holds. */
typedef struct {
    UINT64 address;
    UINT64 size;
    UINT32 section;
} LIVE;

/* What the stubs do and what they have seen.

   The searched section, SECTION, holds pages pages and is mapped at
   probe_at. The image being given has image_count sections, whose handles
   count up from image_first, every one of SECTION_PAGES pages but the
   last, of image_last_pages, and is mapped from image_at.

   The call numbered fail_at, counting from 1 over maps and unmaps
   together, and the process_map numbered fail_map_at, counting from 1,
   fail with fail_status (0 for none).

   table holds the live mappings of each zone, zone_live of them in the
   order of their addresses, no two sharing a page, and live counts them
   all; most_live is the most there have been at once, mixed counts the
   images whose first process_map found a mapping in its zone, and misuse
   the calls a correct program does not make, among them one whose
   addresses are not all of one zone or that a zone has no room for. A
   call the table has no room for is the kernel's to refuse, and is no
   misuse. */
static struct {
    UINT64 pages;
    UINT64 probe_at;
    UINT64 image_at;
    UINT32 image_first;
    unsigned image_count;
    UINT64 image_last_pages;
    unsigned fail_at;
    unsigned fail_map_at;
    UINT32 fail_status;
    unsigned calls;
    unsigned maps;
    unsigned live;
    unsigned most_live;
    unsigned mixed;
    unsigned misuse;
    unsigned zone_live[ZONES];
    LIVE table[ZONES][ZONE_LIVE_MAX];
} world;

/* A world with nothing mapped, a section of pages pages to search and no
   image, in which the call numbered fail_at fails with fail_status. */
static void reset(UINT64 pages, unsigned fail_at, UINT32 fail_status)
{
    unsigned zone;

    for (zone = 0; zone < ZONES; zone++)
        world.zone_live[zone] = 0;
    world.pages = pages;
    world.probe_at = NEXT;
    world.image_at = 0;
    world.image_first = IMAGE_FIRST_HANDLE;
    world.image_count = 0;
    world.image_last_pages = 0;
    world.fail_at = fail_at;
    world.fail_map_at = 0;
    world.fail_status = fail_status;
    world.calls = 0;
    world.maps = 0;
    world.live = 0;
    world.most_live = 0;
    world.mixed = 0;
    world.misuse = 0;
}

/* Whether section is one of the image being given. */
static int of_image(UINT32 section)
{
    return section >= world.image_first &&
           section - world.image_first < world.image_count;
}

/* Whether two ranges of addresses share a page. */
static int meet(UINT64 a, UINT64 a_size, UINT64 b, UINT64 b_size)
{
    return a < b + b_size && b < a + a_size;
}

/* Whether the mapping held lies wholly in the size bytes at address. */
static int inside(const LIVE *held, UINT64 address, UINT64 size)
{
    return address <= held->address &&
           held->address + held->size <= address + size;
}

/* The zone that holds the size bytes at address, or ZONES if none holds
   them all. */
static unsigned zone_of(UINT64 address, UINT64 size)
{
    UINT64 from;

    if (address < NEXT)
        return ZONES;
    from = address - NEXT;
    if (from / MAPPING_RANGE_BYTES >= ZONES || size > MAPPING_RANGE_BYTES ||
        from % MAPPING_RANGE_BYTES > MAPPING_RANGE_BYTES - size)
        return ZONES;
    return (unsigned)(from / MAPPING_RANGE_BYTES);
}

UINT32 process_map(UINT32 process, UINT32 section, UINT64 offset,
                   UINT64 address, UINT64 size, UINT32 protection)
{
    UINT64 pages = 0, at = 0;
    unsigned zone, live, place, i;
    LIVE *table;

    world.calls++;
    world.maps++;
    if (section == SECTION) {
        pages = world.pages;
        at = world.probe_at;
    } else if (of_image(section)) {
        UINT32 index = section - world.image_first;

        pages = index + 1 < world.image_count ? SECTION_PAGES
                                              : world.image_last_pages;
        at = world.image_at + (UINT64)index * SECTION_MAX_SIZE;
        /* An image's section is mapped whole. */
        if (size != pages * PAGE_SIZE)
            world.misuse++;
    } else {
        world.misuse++;
    }
    if (process != SELF || offset != 0 || address != at || size == 0 ||
        size % PAGE_SIZE || protection != MAP_READ)
        world.misuse++;
    if (world.calls == world.fail_at || world.maps == world.fail_map_at)
        return world.fail_status;
    if (size > pages * PAGE_SIZE)
        return ERR_OUT_OF_RANGE;
    zone = zone_of(address, size);
    if (zone == ZONES) {
        world.misuse++;
        return ERR_INVALID_ARGS;
    }
    table = world.table[zone];
    live = world.zone_live[zone];
    if (of_image(section) && world.maps == 1 && live != 0)
        world.mixed++;
    /* The new mapping's place in the order. Those of the zone share no
       page, so one that shares a page with the new one is the one before
       its place or the one at it. */
    for (place = live; place > 0 && table[place - 1].address > address;)
        place--;
    if ((place > 0 && meet(address, size, table[place - 1].address,
                           table[place - 1].size)) ||
        (place < live &&
         meet(address, size, table[place].address, table[place].size)))
        return ERR_BAD_STATE;
    if (world.live == TABLE_MAX)
        return ERR_NO_MEMORY;
    if (live == ZONE_LIVE_MAX) {
        world.misuse++;
        return ERR_NO_MEMORY;
    }
    for (i = live; i > place; i--)
        table[i] = table[i - 1];
    table[place].address = address;
    table[place].size = size;
    table[place].section = section;
    world.zone_live[zone]++;
    world.live++;
    if (world.live > world.most_live)
        world.most_live = world.live;
    return OK;
}

UINT32 process_unmap(UINT32 process, UINT64 address, UINT64 size)
{
    UINT64 within = 0;
    unsigned zone = zone_of(address, size), live, first, after, across = 0;
    LIVE *table;

    world.calls++;
    if (process != SELF)
        world.misuse++;
    if (world.calls == world.fail_at)
        return world.fail_status;
    if (size == 0 || size % PAGE_SIZE || zone == ZONES) {
        world.misuse++;
        return ERR_INVALID_ARGS;
    }
    table = world.table[zone];
    live = world.zone_live[zone];
    /* The mappings from first up to after are those that share a page
       with the range: the range is unmapped if each lies wholly in it
       and together they fill it. */
    for (first = 0; first < live &&
                    !meet(address, size, table[first].address,
                          table[first].size);)
        first++;
    for (after = first; after < live &&
                        meet(address, size, table[after].address,
                             table[after].size);
         after++) {
        if (inside(&table[after], address, size))
            within += table[after].size;
        else
            across++;
    }
    if (across || within != size) {
        world.misuse++;
        return ERR_BAD_STATE;
    }
    world.zone_live[zone] -= after - first;
    world.live -= after - first;
    for (; first < world.zone_live[zone]; first++, after++)
        table[first] = table[after];
    return OK;
}

/* The rest of lib/mapping.c's calls, which the calls checked make none
   of. */
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

/* ------------------------------------------------------------------------
   The search. */

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
    check(world.most_live <= 1, "no two ranges are mapped at once",
          (long long)pages, (long long)limit);
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

/* ------------------------------------------------------------------------
   The ranges. */

/* The mapping the ranges are kept in, the ranges, where each lay and the
   mapping's next address once they were kept, and the sections of the
   image each was given last, 0 for none. */
static MAPPING program;
static MAPPING_RANGE ranges[RANGES];
static UINT64 bases[RANGES];
static UINT64 reserved_next;
static unsigned holds[RANGES];
static unsigned images;

/* The bytes of an image of count sections whose last holds last_pages
   pages, less slack bytes of its last page. */
static UINT64 image_bytes(unsigned count, UINT64 last_pages, UINT64 slack)
{
    return (UINT64)(count - 1) * SECTION_MAX_SIZE + last_pages * PAGE_SIZE -
           slack;
}

/* Make the next image the one being given: count sections, the last of
   last_pages pages, to be mapped from at, with the process_map numbered
   fail_map_at failing with failing. Its sections' handles are written to
   handles. */
static void next_image(RECEIVED_HANDLE *handles, unsigned count,
                       UINT64 last_pages, UINT64 at, unsigned fail_map_at,
                       UINT32 failing)
{
    unsigned i;

    world.image_first =
        IMAGE_FIRST_HANDLE + (UINT32)images++ * CHANNEL_MAX_HANDLES;
    world.image_count = count;
    world.image_last_pages = last_pages;
    world.image_at = at;
    world.fail_at = 0;
    world.fail_map_at = fail_map_at;
    world.fail_status = failing;
    world.calls = 0;
    world.maps = 0;
    for (i = 0; i < count; i++) {
        handles[i].handle = world.image_first + i;
        handles[i].type = 0;
        handles[i].rights = 0;
    }
}

/* Give the range numbered number the next image. Returns what
   mapping_range_image does. */
static const void *give(unsigned number, unsigned count, UINT64 last_pages,
                        unsigned fail_map_at, UINT32 failing, UINT32 *status)
{
    RECEIVED_HANDLE handles[CHANNEL_MAX_HANDLES];

    next_image(handles, count, last_pages, bases[number], fail_map_at,
               failing);
    return mapping_range_image(&program, &ranges[number], handles, count,
                               image_bytes(count, last_pages, 0), status);
}

static unsigned held_sections(void)
{
    unsigned number, total = 0;

    for (number = 0; number < RANGES; number++)
        total += holds[number];
    return total;
}

/* Whether the range numbered number, which is the zone of that number
   since the ranges are kept from NEXT, holds the first count sections of
   the image being given, each at its place, and no other mapping. */
static int range_holds_image(unsigned number, unsigned count)
{
    const LIVE *table = world.table[number];
    unsigned i;

    if (world.zone_live[number] != count)
        return 0;
    for (i = 0; i < count; i++)
        if (table[i].section != world.image_first + i ||
            table[i].address != bases[number] + (UINT64)i * SECTION_MAX_SIZE)
            return 0;
    return 1;
}

/* A world with nothing mapped and the ranges kept anew from NEXT, which
   keeping them neither maps nor calls for. */
static void reserve(void)
{
    unsigned number;

    reset(0, 0, 0);
    program.self = SELF;
    program.next = NEXT;
    for (number = 0; number < RANGES; number++) {
        mapping_range_reserve(&program, &ranges[number]);
        bases[number] = ranges[number].base;
        holds[number] = 0;
        check(bases[number] == NEXT + number * MAPPING_RANGE_BYTES,
              "a range is kept at the next address, past the one before",
              (long long)number, (long long)bases[number]);
        check(ranges[number].mapped == 0, "a range kept holds no image",
              (long long)number, (long long)ranges[number].mapped);
    }
    reserved_next = program.next;
    check(reserved_next == NEXT + RANGES * MAPPING_RANGE_BYTES,
          "the next address is past every range kept", RANGES,
          (long long)reserved_next);
    check(world.calls == 0, "keeping a range makes no call", world.calls, 0);
    world.probe_at = reserved_next;
}

/* Give the range numbered number an image of count sections, the i-th
   image given, and check what the laws say of it: it is mapped if the
   table has room for its sections beside the other ranges' images, and
   refused with ERR_NO_MEMORY if not. Returns whether it is to be
   mapped. */
static int given(unsigned number, unsigned count, UINT64 last_pages,
                 long long i)
{
    UINT32 status = ERR_NOT_SUPPORTED;
    int room = held_sections() - holds[number] + count <= TABLE_MAX;
    const void *at = give(number, count, last_pages, 0, 0, &status);

    holds[number] = room ? count : 0;
    if (room) {
        check(at == (const void *)bases[number],
              "an image starts where its range's first did", i, status);
        check(ranges[number].mapped == image_bytes(count, last_pages, 0),
              "the range holds the image's bytes", i,
              (long long)ranges[number].mapped);
    } else {
        check(at == 0 && status == ERR_NO_MEMORY,
              "an image the table has no room for is refused with "
              "ERR_NO_MEMORY",
              i, status);
        check(ranges[number].mapped == 0,
              "and its range holds no image", i,
              (long long)ranges[number].mapped);
    }
    check(range_holds_image(number, holds[number]),
          "the range holds the image's sections, each at its place, and "
          "no other mapping",
          i, world.zone_live[number]);
    check(program.next == reserved_next,
          "the next address is where the reservations left it", i,
          (long long)program.next);
    check(ranges[number].base == bases[number],
          "the range lies where it was kept", i, number);
    check(world.live == held_sections(),
          "the sections mapped are those of each range's last image", i,
          world.live);
    check(world.mixed == 0,
          "no two images of a range are mapped at once", i, world.mixed);
    check(world.misuse == 0,
          "every call is of the program's own process, a section of the "
          "image, whole, at its place in the range, read only",
          i, world.misuse);
    return room;
}

/* One range given GIVES images of one size: the most sections ever mapped
   at once are one image's. */
static void one_range(void)
{
    unsigned i;

    reserve();
    for (i = 0; i < GIVES; i++)
        given(0, USUAL_SECTIONS, SECTION_PAGES, i);
    check(world.most_live == USUAL_SECTIONS,
          "one range holds at most one image's sections at any time",
          world.most_live, USUAL_SECTIONS);
}

/* The ranges given GIVES images in turn: the most sections ever mapped at
   once are one image's for each range. */
static void ranges_in_turn(void)
{
    unsigned i;

    reserve();
    for (i = 0; i < GIVES; i++)
        given(i % RANGES, USUAL_SECTIONS, SECTION_PAGES, i);
    check(world.most_live == RANGES * USUAL_SECTIONS,
          "each range holds at most one image's sections at any time",
          world.most_live, RANGES * USUAL_SECTIONS);
}

/* GIVES images, each to a range and of a count of sections and a last
   section's pages drawn at random, so that an image of every count
   follows one of every other in a range, and the table has room for
   some and refuses others. */
static void differing_counts(void)
{
    unsigned i, mapped = 0;

    reserve();
    for (i = 0; i < GIVES; i++) {
        unsigned number = below(RANGES);
        unsigned count = 1 + below(CHANNEL_MAX_HANDLES);
        UINT64 last_pages = 1 + below(SECTION_PAGES);

        mapped += (unsigned)given(number, count, last_pages, i);
    }
    check(mapped != 0 && mapped != GIVES,
          "the images drawn are some the table has room for and some it "
          "refuses",
          mapped, GIVES);
}

/* A search of a section of SECTION_MAX_SIZE at the mapping's next
   address, past the ranges. Returns the bytes it finds. */
static UINT64 search_past(UINT32 *status)
{
    world.pages = SECTION_PAGES;
    world.calls = 0;
    world.maps = 0;
    return mapping_section_bytes(&program, SECTION, SECTION_MAX_SIZE, status);
}

/* A search maps where nothing is mapped, past ranges that hold images: it
   finds its section's size while the table has one mapping left, and is
   refused with ERR_NO_MEMORY once the images fill the table. */
static void search_past_ranges(void)
{
    UINT32 status = ERR_NOT_SUPPORTED;
    UINT64 got;

    reserve();
    given(0, CHANNEL_MAX_HANDLES, SECTION_PAGES, 0);
    given(1, TABLE_MAX - CHANNEL_MAX_HANDLES - 1, SECTION_PAGES, 1);
    got = search_past(&status);
    check(status == OK && got == SECTION_MAX_SIZE,
          "a search finds a section's size while the table has one "
          "mapping left",
          status, (long long)got);
    check(world.live == TABLE_MAX - 1 && world.misuse == 0 &&
              program.next == reserved_next,
          "and leaves the ranges' images and the next address as they were",
          world.live, world.misuse);
    given(2, 1, SECTION_PAGES, 2);
    got = search_past(&status);
    check(status == ERR_NO_MEMORY && got == 0,
          "a search is refused with ERR_NO_MEMORY where the images fill "
          "the table",
          status, (long long)got);
    check(world.live == TABLE_MAX && world.misuse == 0 &&
              program.next == reserved_next,
          "and leaves the ranges' images and the next address as they were",
          world.live, world.misuse);
}

/* An image of count sections refused at its section numbered fail_map_at
   with failing, in the first range, which holds an image of before
   sections, or none for 0, while the second holds one: nothing of either
   image of the first range stays mapped, the second's stays, and the
   next image given to the first is mapped at its start. */
static void refused(unsigned count, unsigned fail_map_at, UINT32 failing,
                    unsigned before)
{
    UINT32 status = OK;
    const void *at;
    long long which = (long long)count * (CHANNEL_MAX_HANDLES + 1) +
                      fail_map_at;

    reserve();
    given(1, USUAL_SECTIONS, SECTION_PAGES, which);
    if (before)
        given(0, before, SECTION_PAGES, which);
    at = give(0, count, SECTION_PAGES, fail_map_at, failing, &status);
    holds[0] = 0;
    check(at == 0 && status == failing,
          "an image whose section is not mapped is refused with the "
          "call's status",
          which, status);
    check(world.live == held_sections(),
          "nothing of it or of the image its range held stays mapped, and "
          "the other range's image does",
          which, world.live);
    check(ranges[0].mapped == 0 && range_holds_image(0, 0),
          "its range holds no image and no mapping", which,
          (long long)ranges[0].mapped);
    check(world.maps == fail_map_at,
          "no section is mapped after the one refused", which, world.maps);
    check(program.next == reserved_next && world.misuse == 0 &&
              world.mixed == 0,
          "the next address is where it was, with no misuse", which,
          world.misuse);
    given(0, count, SECTION_PAGES, which);
}

/* An image given to a range whose image the kernel does not unmap: it is
   refused with that status before any of its sections is mapped, the
   range holds what it held, and the next image given is mapped. */
static void unmap_refused(UINT32 failing)
{
    RECEIVED_HANDLE handles[CHANNEL_MAX_HANDLES];
    UINT32 status = OK;
    const void *at;

    reserve();
    given(0, USUAL_SECTIONS, SECTION_PAGES, failing);
    next_image(handles, CHANNEL_MAX_HANDLES, SECTION_PAGES, bases[0], 0,
               failing);
    world.fail_at = 1;
    at = mapping_range_image(&program, &ranges[0], handles,
                             CHANNEL_MAX_HANDLES,
                             image_bytes(CHANNEL_MAX_HANDLES, SECTION_PAGES,
                                         0),
                             &status);
    check(at == 0 && status == failing,
          "an image is refused with the status of the unmapping that fails",
          failing, status);
    check(world.maps == 0, "before any of its sections is mapped", failing,
          world.maps);
    check(world.live == USUAL_SECTIONS &&
              ranges[0].mapped ==
                  image_bytes(USUAL_SECTIONS, SECTION_PAGES, 0),
          "and the range holds the image it held, still mapped", failing,
          world.live);
    given(0, CHANNEL_MAX_HANDLES, SECTION_PAGES, failing);
}

/* A range cleared holds no image, with nothing of its image mapped and
   the other ranges' images as they were; clearing a range that holds
   none makes no call; and one whose unmapping fails holds what it held. */
static void cleared(void)
{
    UINT32 status;

    reserve();
    given(0, USUAL_SECTIONS, SECTION_PAGES, 0);
    given(1, CHANNEL_MAX_HANDLES, 1, 1);
    world.calls = 0;
    world.fail_at = 1;
    world.fail_status = ERR_BAD_STATE;
    status = mapping_range_clear(&program, &ranges[1]);
    check(status == ERR_BAD_STATE && world.live == held_sections() &&
              ranges[1].mapped == image_bytes(CHANNEL_MAX_HANDLES, 1, 0),
          "a range whose unmapping fails gives the status and holds what "
          "it held",
          status, world.live);
    world.fail_at = 0;
    status = mapping_range_clear(&program, &ranges[1]);
    holds[1] = 0;
    check(status == OK && ranges[1].mapped == 0 &&
              ranges[1].base == bases[1],
          "a range cleared holds no image and lies where it was", status,
          (long long)ranges[1].mapped);
    check(world.live == held_sections() && world.misuse == 0,
          "nothing of its image stays mapped, and the other's does",
          world.live, world.misuse);
    world.calls = 0;
    status = mapping_range_clear(&program, &ranges[1]);
    check(status == OK && world.calls == 0,
          "clearing a range that holds no image makes no call", status,
          world.calls);
    check(program.next == reserved_next,
          "and the next address is where the reservations left it", 0,
          (long long)program.next);
    given(1, USUAL_SECTIONS, SECTION_PAGES, 2);
}

/* ------------------------------------------------------------------------
   mapping_image. */

/* An image of count sections whose last holds last_pages pages, less
   slack bytes, mapped at the next address, with the process_map numbered
   fail_map_at failing (0 for none). */
static void image(unsigned count, UINT64 last_pages, UINT64 slack,
                  unsigned fail_map_at)
{
    RECEIVED_HANDLE handles[CHANNEL_MAX_HANDLES];
    MAPPING mapping = {SELF, NEXT};
    UINT32 status = OK;
    const void *at;
    UINT64 whole = image_bytes(count, last_pages, 0);

    reset(0, 0, 0);
    next_image(handles, count, last_pages, NEXT, fail_map_at, ERR_NO_MEMORY);
    at = mapping_image(&mapping, handles, count,
                       image_bytes(count, last_pages, slack), &status);
    check(world.misuse == 0,
          "an image's sections are mapped whole, one after another from "
          "the next address, read only",
          count, fail_map_at);
    if (fail_map_at) {
        check(at == 0 && status == ERR_NO_MEMORY,
              "an image whose section is not mapped is refused with the "
              "call's status",
              count, fail_map_at);
        check(world.live == 0 && mapping.next == NEXT,
              "with nothing of it mapped and the next address where it was",
              world.live, (long long)mapping.next);
    } else {
        check(at == (const void *)NEXT && status == OK,
              "an image starts at the next address", count, status);
        check(world.live == count && mapping.next == NEXT + whole,
              "with every section mapped and the next address past its "
              "whole pages",
              world.live, (long long)mapping.next);
    }
}

int main(void)
{
    UINT64 pages, limit;
    unsigned at, calls, count, before;
    static const UINT32 statuses[] = {ERR_NO_MEMORY, ERR_ACCESS_DENIED,
                                      ERR_BAD_STATE, ERR_INVALID_ARGS};
    static const unsigned befores[] = {0, 1, USUAL_SECTIONS,
                                       CHANNEL_MAX_HANDLES};
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

    one_range();
    ranges_in_turn();
    differing_counts();
    search_past_ranges();
    for (count = 1; count <= CHANNEL_MAX_HANDLES; count++)
        for (at = 1; at <= count; at++)
            for (i = 0; i < sizeof statuses / sizeof statuses[0]; i++)
                for (before = 0;
                     before < sizeof befores / sizeof befores[0]; before++)
                    refused(count, at, statuses[i], befores[before]);
    for (i = 0; i < sizeof statuses / sizeof statuses[0]; i++)
        unmap_refused(statuses[i]);
    cleared();

    for (count = 1; count <= CHANNEL_MAX_HANDLES; count++) {
        image(count, SECTION_PAGES, 0, 0);
        image(count, 1, PAGE_SIZE - 1, 0);
        for (at = 1; at <= count; at++)
            image(count, 1 + (UINT64)at % SECTION_PAGES, 1, at);
    }

    printf("%llu checks, %u failed\n", checks, failures);
    return failures ? 1 : 0;
}
