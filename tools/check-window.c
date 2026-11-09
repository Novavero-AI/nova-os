/* Host check of lib/window.c, the rules the compositor enforces on a
   window: docs/spec/display.md D6 (placement, size bounds and buffers),
   D7 (presenting) and D19's phase 3 arrangement, rules 4 and 5.

   Each decision is compared with the rule as the spec words it, written
   again here in another form (a size as the nearest one inside bounds,
   a buffer's sections as a loop, a block rectangle row by row over
   pixel addresses) and, where the console's panel already stands where
   D6 says, with console/panel.c's layout. Random cases come from a fixed
   seed, so a failure is the same one every run.

   cc -std=c11 -I include -I lib -I gfx -I console tools/check-window.c
      lib/window.c lib/compose.c lib/outline.c console/panel.c
      console/text.c gfx/gfx.c gfx/font.c gfx/raster.c gfx/limb.c -lm */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compose.h"
#include "panel.h"
#include "window.h"

#define FAILURES_SHOWN 12

#define SEED 0x9e3779b97f4a7c15ull

#define RANDOM_AREAS 1500
#define RANDOM_GIVES 40000
#define PRESENT_SESSIONS 1500

#define PAGE_BYTES 4096ull
#define SECTION_BYTES 0x200000ull
#define BUFFER_BYTES_LIMIT (64ull * SECTION_BYTES)
#define UINT32_LARGEST 0xFFFFFFFFu

/* More sections than a valid buffer has, so an array holds the sections
   of any buffer a case builds and one more. */
#define SECTIONS_MOST 66

/* The pixels a block of the framebuffer holds. */
#define BLOCK_PIXELS (COMPOSE_BLOCK_BYTES / sizeof(UINT32))

/* The memory a test screen's pixels sit in. */
#define SCREEN_MEMORY_PIXELS (1u << 16)

static unsigned failures;
static unsigned long long checks;

static void check(int passed, const char *what, long long a, long long b)
{
    checks++;
    if (!passed && failures++ < FAILURES_SHOWN)
        printf("failed: %s (%lld, %lld)\n", what, a, b);
}

static unsigned long long random_state = SEED;

static unsigned long long next_random(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return random_state;
}

static unsigned long long below(unsigned long long limit)
{
    return next_random() % limit;
}

static unsigned long long between(unsigned long long low,
                                  unsigned long long high)
{
    return low + below(high - low + 1);
}

static long long least_of(long long a, long long b) { return a < b ? a : b; }
static long long most_of(long long a, long long b) { return a > b ? a : b; }

/* ------------------------------------------------------------------------
   The areas, and the spec's bounds on a size */

static const WINDOW_AREA fixed_areas[] = {
    {1280, 800, 0},       {1280, 800, 32},      {2560, 1600, 0},
    {2560, 1600, 64},     {2880, 1800, 0},      {2880, 1800, 72},
    {1366, 768, 0},       {1366, 768, 30},      {7680, 4320, 0},
    {8000, 5000, 40},     {1, 1, 0},            {2, 3, 1},
    {3, 2, 0},            {64, 40, 0},          {5, 1, 0},
    {10, 10, 9},          {UINT32_LARGEST, UINT32_LARGEST, 0x7FFFFFFFu},
};
#define FIXED_AREAS (sizeof fixed_areas / sizeof fixed_areas[0])

/* Areas with no pixel to put a window in. */
static const WINDOW_AREA empty_areas[] = {
    {10, 10, 10}, {10, 10, 11}, {10, 10, UINT32_LARGEST},
    {0, 10, 0},   {0, 0, 0},    {1, 1, 1},
};
#define EMPTY_AREAS (sizeof empty_areas / sizeof empty_areas[0])

static WINDOW_AREA random_area(void)
{
    WINDOW_AREA area;

    area.screen_width = (UINT32)between(1, 9000);
    area.screen_height = (UINT32)between(1, 5200);
    area.strip = below(4) == 0 ? 0 : (UINT32)below(area.screen_height);
    return area;
}

/* The most a window is across and high in an area: the area's, and
   D6's 7680 x 4320. */
static long long widest(const WINDOW_AREA *area)
{
    return least_of(area->screen_width, WINDOW_WIDTH_MAX);
}

static long long tallest(const WINDOW_AREA *area)
{
    return least_of((long long)area->screen_height - area->strip,
                    WINDOW_HEIGHT_MAX);
}

/* The size in [low, high] nearest want, found by distance. */
static long long nearest(long long want, long long low, long long high)
{
    long long to_low = want > low ? want - low : low - want;
    long long to_high = want > high ? want - high : high - want;

    if (want >= low && want <= high)
        return want;
    return to_low < to_high ? low : high;
}

/* A request around the bounds of a dimension, where each rule changes. */
static UINT32 request_near(long long low, long long high)
{
    switch (below(8)) {
    case 0:
        return 0;
    case 1:
        return (UINT32)most_of(low - 1, 0);
    case 2:
        return (UINT32)low;
    case 3:
        return (UINT32)most_of(high - 1, 0);
    case 4:
        return (UINT32)high;
    case 5:
        return (UINT32)(high + 1);
    case 6:
        return UINT32_LARGEST;
    default:
        return (UINT32)below((unsigned long long)high + 10);
    }
}

/* ------------------------------------------------------------------------
   Size and place (D6) */

static const UINT32 reaches[] = {
    0,           1,           2,  5,  6,  22, 44,
    88,          5000,        0x7FFFFFFFu,
    0x80000000u, 0x80000001u, UINT32_LARGEST};
#define REACHES (sizeof reaches / sizeof reaches[0])

static void check_size_case(const WINDOW_AREA *area, UINT32 reach,
                            UINT32 width, UINT32 height)
{
    long long high_width = widest(area);
    long long high_height = tallest(area);
    long long low_width = least_of(2 * (long long)reach, high_width);
    long long low_height = least_of(2 * (long long)reach, high_height);
    UINT32 fitted_width, fitted_height, again_width, again_height;

    window_size(area, reach, width, height, &fitted_width, &fitted_height);
    check(fitted_width >= low_width && fitted_width <= high_width,
          "a window's width is within the bounds", fitted_width, width);
    check(fitted_height >= low_height && fitted_height <= high_height,
          "a window's height is within the bounds", fitted_height, height);
    check(fitted_width == nearest(width, low_width, high_width),
          "a width outside the bounds gets the nearest", fitted_width, width);
    check(fitted_height == nearest(height, low_height, high_height),
          "a height outside the bounds gets the nearest", fitted_height,
          height);
    window_size(area, reach, fitted_width, fitted_height, &again_width,
                &again_height);
    check(again_width == fitted_width && again_height == fitted_height,
          "fitting a fitted size changes nothing", again_width, again_height);
}

static void check_size_monotone(const WINDOW_AREA *area, UINT32 reach,
                                UINT32 a, UINT32 b)
{
    UINT32 first = a < b ? a : b;
    UINT32 second = a < b ? b : a;
    UINT32 first_width, first_height, second_width, second_height;

    window_size(area, reach, first, first, &first_width, &first_height);
    window_size(area, reach, second, second, &second_width, &second_height);
    check(first_width <= second_width, "a larger request is not fitted"
                                       " narrower",
          first, second);
    check(first_height <= second_height, "a larger request is not fitted"
                                         " shorter",
          first, second);
}

static void check_sizes(void)
{
    size_t i, r;
    int round;

    for (i = 0; i < FIXED_AREAS + RANDOM_AREAS; i++) {
        WINDOW_AREA area = i < FIXED_AREAS ? fixed_areas[i] : random_area();

        for (r = 0; r < REACHES; r++)
            for (round = 0; round < 8; round++) {
                UINT32 reach = i < FIXED_AREAS || round < 6
                                   ? reaches[r]
                                   : (UINT32)below(200);

                check_size_case(
                    &area, reach,
                    request_near(2 * (long long)reach, widest(&area)),
                    request_near(2 * (long long)reach, tallest(&area)));
                check_size_monotone(&area, reach,
                                    request_near(0, widest(&area)),
                                    request_near(0, widest(&area)));
            }
    }
}

/* The placement D6 words, in signed integers. */
static void check_place_case(const WINDOW_AREA *area, UINT32 width,
                             UINT32 height)
{
    long long screen_width = area->screen_width;
    long long screen_height = area->screen_height;
    long long strip = area->strip;
    long long left = (screen_width - width) / 2;
    long long top = strip + (screen_height - strip) * 40 / 100 - height / 2;
    UINT32 x, y;

    if (top < strip)
        top = strip;
    if (screen_height - height >= strip && top > screen_height - height)
        top = screen_height - height;
    window_place(area, width, height, &x, &y);
    check(x == left, "a window's left is (W - w) / 2", x, left);
    check(y == top, "a window's top is the 40 percent line, kept inside", y,
          top);
    check(x + (long long)width <= screen_width, "a window is within the width",
          x, width);
    check(y >= strip, "a window is not in the strip", y, strip);
    if (height <= screen_height - strip)
        check(y + (long long)height <= screen_height,
              "a window that fits is above the bottom", y, height);
    check(screen_width - width - x - x >= 0 &&
              screen_width - width - x - x <= 1,
          "a window is centred across", x, width);
}

static UINT32 size_within(long long high)
{
    switch (below(4)) {
    case 0:
        return 1;
    case 1:
        return (UINT32)high;
    case 2:
        return (UINT32)(high / 2 + 1);
    default:
        return (UINT32)between(1, (unsigned long long)high);
    }
}

/* D6 lowers a top to at most H - h; a window within the area never needs
   it (lib/window.c says why), which every such size confirms. */
static void check_lowering_is_never_needed(void)
{
    static const UINT32 strips[] = {0, 1, 5, 40};
    UINT32 depth, height;
    size_t s;

    for (s = 0; s < sizeof strips / sizeof strips[0]; s++)
        for (depth = 1; depth <= 400; depth++)
            for (height = 1; height <= depth; height++) {
                WINDOW_AREA area;
                UINT32 x, y;

                area.screen_width = 1;
                area.screen_height = strips[s] + depth;
                area.strip = strips[s];
                window_place(&area, 1, height, &x, &y);
                check(y >= strips[s] && y + height <= area.screen_height,
                      "a window within the area is placed within it", y,
                      height);
            }
}

static void check_places(void)
{
    size_t i;
    int round;

    check_lowering_is_never_needed();
    for (i = 0; i < FIXED_AREAS + RANDOM_AREAS; i++) {
        WINDOW_AREA area = i < FIXED_AREAS ? fixed_areas[i] : random_area();

        for (round = 0; round < 12; round++)
            check_place_case(
                &area, size_within(widest(&area)),
                size_within(round < 8 ? tallest(&area) : area.screen_height));
    }
}

/* Where the console's panel stands on its own is where a window of the
   panel's size stands (D6 and D19, rule 4): the panel's box is the window,
   its radius the corners' reach. */
static void check_panel_agrees(void)
{
    static const long long ratios[][2] = {{16, 10}, {4, 3}, {16, 9}, {1, 2}};
    INT64 height;
    size_t i;
    unsigned compared = 0;

    for (height = 100; height <= 4400; height += 11)
        for (i = 0; i < sizeof ratios / sizeof ratios[0]; i++) {
            INT64 width = height * ratios[i][0] / ratios[i][1];
            WINDOW_AREA area;
            LAYOUT layout;
            UINT32 fitted_width, fitted_height, x, y;
            INT64 box_width, box_height;

            if (!panel_layout(&layout, width, height))
                continue;
            area.screen_width = (UINT32)width;
            area.screen_height = (UINT32)height;
            area.strip = 0;
            box_width = layout.box.right - layout.box.left;
            box_height = layout.box.bottom - layout.box.top;
            window_size(&area, (UINT32)layout.radius, (UINT32)box_width,
                        (UINT32)box_height, &fitted_width, &fitted_height);
            window_place(&area, fitted_width, fitted_height, &x, &y);
            check(fitted_width == box_width && fitted_height == box_height,
                  "the panel's size is a window's size", width, height);
            check(x == layout.box.left && y == layout.box.top,
                  "the panel's place is a window's place", width, height);
            compared++;
        }
    check(compared > 1000, "the panel is laid out on most screens compared",
          compared, 0);
}

/* ------------------------------------------------------------------------
   Making a window (D19, rule 4) */

static int is_one_layer(UINT32 layer)
{
    static const UINT32 layers[] = {
        DISPLAY_LAYER_APP,     DISPLAY_LAYER_NOTIFICATION,
        DISPLAY_LAYER_OVERLAY, DISPLAY_LAYER_TRUSTED,
        DISPLAY_LAYER_SYSTEM};
    size_t i;

    for (i = 0; i < sizeof layers / sizeof layers[0]; i++)
        if (layer == layers[i])
            return 1;
    return 0;
}

/* The refusal D19's rule 4 gives, in its order of words: the layer, the
   surface, then the count. */
static UINT32 expected_open(const WINDOW_AREA *area, UINT32 layers,
                            UINT32 open, UINT32 layer, UINT32 surface)
{
    int empty = area->screen_width == 0 || area->strip >= area->screen_height;

    if (!is_one_layer(layer) || (layers & layer) != layer)
        return ERR_ACCESS_DENIED;
    if (surface != DISPLAY_SURFACE_PANEL)
        return ERR_NOT_SUPPORTED;
    if (open != 0 || empty)
        return ERR_NO_RESOURCES;
    return OK;
}

static void check_open_case(const WINDOW_AREA *area, UINT32 reach,
                            UINT32 layers, UINT32 open, UINT32 layer,
                            UINT32 surface, UINT32 width, UINT32 height)
{
    DISPLAY_WINDOW_REQUEST request;
    WINDOW_RECORD window, before;
    UINT32 status, expected;

    memset(&request, 0, sizeof request);
    request.width = width;
    request.height = height;
    request.layer = layer;
    request.surface = surface;
    memset(&window, 0xA5, sizeof window);
    before = window;
    status = window_open(area, reach, layers, open, &request, &window);
    expected = expected_open(area, layers, open, layer, surface);
    check(status == expected, "a window is made or refused as rule 4 says",
          status, expected);
    if (expected != OK) {
        check(memcmp(&window, &before, sizeof window) == 0,
              "a refused window leaves the record as it was", status, layer);
        return;
    }
    {
        /* Whatever the record held, a new window is this and no more: all
           zero but its size, place, layer, surface and shown buffer. */
        WINDOW_RECORD made;

        memset(&made, 0, sizeof made);
        window_size(area, reach, width, height, &made.width, &made.height);
        window_place(area, made.width, made.height, &made.x, &made.y);
        made.layer = layer;
        made.surface = surface;
        made.shown = WINDOW_NO_BUFFER;
        check(memcmp(&window, &made, sizeof made) == 0,
              "a new window is sized, placed and otherwise empty",
              window.width, window.height);
    }
}

/* The refusals and the one acceptance, as D19's rule 4 words them. */
static void check_open_table(void)
{
    static const struct {
        UINT32 layers, open, layer, surface, status;
        const char *what;
    } cases[] = {
        {DISPLAY_LAYER_SYSTEM, 0, DISPLAY_LAYER_SYSTEM, DISPLAY_SURFACE_PANEL,
         OK, "the console's window is made"},
        {DISPLAY_LAYER_SYSTEM, 1, DISPLAY_LAYER_SYSTEM, DISPLAY_SURFACE_PANEL,
         ERR_NO_RESOURCES, "a second window is refused"},
        {DISPLAY_LAYER_SYSTEM, 0, DISPLAY_LAYER_APP, DISPLAY_SURFACE_PANEL,
         ERR_ACCESS_DENIED, "a layer outside the connection's is refused"},
        {DISPLAY_LAYER_APP | DISPLAY_LAYER_SYSTEM, 0, DISPLAY_LAYER_APP,
         DISPLAY_SURFACE_PANEL, OK, "one of several layers is made"},
        {DISPLAY_LAYER_SYSTEM, 0, DISPLAY_LAYER_SYSTEM, DISPLAY_SURFACE_WINDOW,
         ERR_NOT_SUPPORTED, "the WINDOW surface is refused"},
        {DISPLAY_LAYER_SYSTEM, 0, DISPLAY_LAYER_SYSTEM, DISPLAY_SURFACE_RAISED,
         ERR_NOT_SUPPORTED, "the RAISED surface is refused"},
        {DISPLAY_LAYER_SYSTEM, 1, DISPLAY_LAYER_APP, DISPLAY_SURFACE_PANEL,
         ERR_ACCESS_DENIED, "a layer is refused before a second window"},
        {DISPLAY_LAYER_SYSTEM, 1, DISPLAY_LAYER_SYSTEM, DISPLAY_SURFACE_RAISED,
         ERR_NOT_SUPPORTED, "a surface is refused before a second window"},
        {DISPLAY_LAYER_SYSTEM, 0, DISPLAY_LAYER_APP, DISPLAY_SURFACE_RAISED,
         ERR_ACCESS_DENIED, "a layer is refused before a surface"},
        {DISPLAY_LAYER_SYSTEM, 0, 0, DISPLAY_SURFACE_PANEL, ERR_ACCESS_DENIED,
         "no layer is refused"},
        {DISPLAY_LAYER_APP, 0, DISPLAY_LAYER_APP | DISPLAY_LAYER_SYSTEM,
         DISPLAY_SURFACE_PANEL, ERR_ACCESS_DENIED,
         "two layers, one of them the connection's, are refused"},
        {DISPLAY_LAYER_APP | DISPLAY_LAYER_SYSTEM, 0,
         DISPLAY_LAYER_APP | DISPLAY_LAYER_SYSTEM, DISPLAY_SURFACE_PANEL,
         ERR_ACCESS_DENIED, "two layers, both the connection's, are refused"},
        {DISPLAY_LAYERS, 0, 0x20, DISPLAY_SURFACE_PANEL, ERR_ACCESS_DENIED,
         "a layer there is not is refused"},
    };
    static const WINDOW_AREA area = {1280, 800, 0};
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        DISPLAY_WINDOW_REQUEST request;
        WINDOW_RECORD window;
        UINT32 status;

        memset(&request, 0, sizeof request);
        request.width = 400;
        request.height = 300;
        request.layer = cases[i].layer;
        request.surface = cases[i].surface;
        status = window_open(&area, 22, cases[i].layers, cases[i].open,
                             &request, &window);
        check(status == cases[i].status, cases[i].what, status,
              cases[i].status);
    }
}

static void check_opens(void)
{
    static const UINT32 layer_sets[] = {
        0,
        DISPLAY_LAYER_SYSTEM,
        DISPLAY_LAYER_APP,
        DISPLAY_LAYER_APP | DISPLAY_LAYER_TRUSTED,
        DISPLAY_LAYERS,
    };
    static const UINT32 layers[] = {0,
                                    DISPLAY_LAYER_APP,
                                    DISPLAY_LAYER_NOTIFICATION,
                                    DISPLAY_LAYER_OVERLAY,
                                    DISPLAY_LAYER_TRUSTED,
                                    DISPLAY_LAYER_SYSTEM,
                                    DISPLAY_LAYER_APP | DISPLAY_LAYER_SYSTEM,
                                    DISPLAY_LAYERS,
                                    0x20,
                                    0x80000000u,
                                    UINT32_LARGEST};
    static const UINT32 surfaces[] = {0,
                                      DISPLAY_SURFACE_WINDOW,
                                      DISPLAY_SURFACE_RAISED,
                                      DISPLAY_SURFACE_PANEL,
                                      4,
                                      UINT32_LARGEST};
    static const UINT32 opens[] = {0, 1, 2, UINT32_LARGEST};
    size_t a, s, l, y, o;

    check_open_table();
    for (a = 0; a < 4 + EMPTY_AREAS; a++) {
        WINDOW_AREA area = a < 4 ? fixed_areas[a] : empty_areas[a - 4];

        for (s = 0; s < sizeof layer_sets / sizeof layer_sets[0]; s++)
            for (l = 0; l < sizeof layers / sizeof layers[0]; l++)
                for (y = 0; y < sizeof surfaces / sizeof surfaces[0]; y++)
                    for (o = 0; o < sizeof opens / sizeof opens[0]; o++)
                        check_open_case(&area, 22, layer_sets[s], opens[o],
                                        layers[l], surfaces[y], 700, 500);
    }
    for (a = 0; a < RANDOM_AREAS; a++) {
        WINDOW_AREA area = random_area();
        UINT32 reach = (UINT32)below(100);

        check_open_case(
            &area, reach, DISPLAY_LAYER_SYSTEM, 0, DISPLAY_LAYER_SYSTEM,
            DISPLAY_SURFACE_PANEL,
            request_near(2 * (long long)reach, widest(&area)),
            request_near(2 * (long long)reach, tallest(&area)));
    }
}

static void check_configure(void)
{
    WINDOW_RECORD window;
    UINT32 serial, last;
    int i;

    memset(&window, 0, sizeof window);
    for (i = 1; i <= 5; i++) {
        serial = window_configure(&window);
        check(serial == (UINT32)i && window.serial == serial,
              "a configure's serial is one more than the last", serial, i);
    }
    window.serial = UINT32_LARGEST - 2;
    last = window.serial;
    for (i = 0; i < 5; i++) {
        serial = window_configure(&window);
        check(serial >= last && serial != 0 && window.serial == serial,
              "a serial never falls, and never returns to 0", serial, last);
        last = serial;
    }
    check(last == UINT32_LARGEST, "a serial stays at the greatest", last, 0);
}

/* ------------------------------------------------------------------------
   Buffers (D6) */

/* The bytes of a buffer, as D6 words them, where nothing overflows. */
static unsigned long long oracle_bytes(unsigned long long stride,
                                       unsigned long long height)
{
    unsigned long long bytes = stride * height * 4;

    return (bytes + PAGE_BYTES - 1) / PAGE_BYTES * PAGE_BYTES;
}

/* The sections of a buffer of size bytes, one after another. */
static UINT32 oracle_bands(unsigned long long size, UINT64 *sections)
{
    UINT32 count = 0;

    while (size > 0) {
        unsigned long long take = size < SECTION_BYTES ? size : SECTION_BYTES;

        sections[count++] = take;
        size -= take;
    }
    return count;
}

static void check_buffer_sizes(void)
{
    static const struct {
        UINT32 width, height, bytes, sections;
    } rows[] = { /* D6's table: a buffer's bytes before rounding to pages */
        {1280, 800, 4096000, 2},
        {2560, 1600, 16384000, 8},
        {2880, 1800, 20736000, 10},
        {7680, 4320, 132710400, 64},
        {8192, 4096, 134217728, 64},
    };
    size_t i;
    int round;

    for (i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        UINT64 bytes = window_buffer_size(rows[i].width, rows[i].height);

        check(bytes == (rows[i].bytes + PAGE_BYTES - 1) / PAGE_BYTES *
                           PAGE_BYTES,
              "a buffer's bytes are D6's table's, in whole pages",
              (long long)bytes, rows[i].bytes);
        check(window_buffer_sections(bytes) == rows[i].sections,
              "a buffer's sections, as D6's table",
              window_buffer_sections(bytes), rows[i].sections);
    }
    check(window_buffer_sections(0) == 0, "no bytes need no section", 0, 0);
    check(window_buffer_sections(1) == 1, "a byte needs a section", 0, 0);
    check(window_buffer_sections(SECTION_BYTES) == 1,
          "a section's size needs one section", 0, 0);
    check(window_buffer_sections(SECTION_BYTES + 1) == 2,
          "a byte more needs a second", 0, 0);
    check(window_buffer_size(1, 1) == PAGE_BYTES,
          "a pixel's buffer is a page", 0, 0);
    check(window_buffer_size(UINT32_LARGEST, UINT32_LARGEST) >
              BUFFER_BYTES_LIMIT,
          "the greatest buffer is more than a message holds", 0, 0);
    check(window_buffer_sections(0xFFFFFFFFFFFFFFFFull) == 1ull << 43,
          "the sections of any size are counted without wrapping", 0, 0);
    check(window_buffer_size(0x80000000u, 0x80000000u) > BUFFER_BYTES_LIMIT,
          "a size that wraps to 0 is beyond the most", 0, 0);
    check(window_buffer_size(0x7FFFFFFFu, 0x80000001u) > BUFFER_BYTES_LIMIT,
          "a size that overflows in rounding is beyond the most", 0, 0);
    for (round = 0; round < 20000; round++) {
        UINT32 stride = (UINT32)between(1, 1u << 16);
        UINT32 height = (UINT32)between(1, 1u << 16);
        UINT64 bytes = window_buffer_size(stride, height);
        UINT64 sections = window_buffer_sections(bytes);

        check(bytes == oracle_bytes(stride, height),
              "a buffer's bytes are whole pages", stride, height);
        check(sections * SECTION_BYTES >= bytes &&
                  (sections - 1) * SECTION_BYTES < bytes,
              "a buffer's sections hold its bytes and no more", stride,
              height);
    }
    for (round = 0; round < 20000; round++) {
        UINT32 stride = (UINT32)(below(2) ? next_random() : between(1, 9000));
        UINT32 height = (UINT32)(below(2) ? next_random() : between(1, 9000));
        UINT64 bytes = window_buffer_size(stride, height);
        UINT64 exact = (UINT64)stride * height;

        if (exact <= 1ull << 40)
            check(bytes == oracle_bytes(stride, height),
                  "a buffer's bytes where they fit", stride, height);
        else
            check(bytes > BUFFER_BYTES_LIMIT,
                  "a buffer's bytes where they overflow stay beyond the most",
                  stride, height);
    }
}

typedef struct {
    UINT32 number, width, height, stride;
} BUFFER_ASK;

/* What D6 and D19's rule 5 say of a buffer given, in words. */
static int expected_buffer_valid(const WINDOW_RECORD *window,
                                 const BUFFER_ASK *ask, UINT32 count,
                                 const UINT64 *sections)
{
    UINT64 want[SECTIONS_MOST];
    UINT32 n;

    if (ask->number > WINDOW_BUFFER_LAST || ask->number == window->shown)
        return 0;
    if (ask->width < 1 || ask->height < 1 || ask->stride < ask->width)
        return 0;
    if (oracle_bytes(ask->stride, ask->height) > BUFFER_BYTES_LIMIT)
        return 0;
    n = oracle_bands(oracle_bytes(ask->stride, ask->height), want);
    return count == n && memcmp(sections, want, n * sizeof want[0]) == 0;
}

/* Give the buffer ask describes with the sections made by kind: 0 to 5
   as D6 bands it, then each fault a sender can make. */
static void check_give_case(WINDOW_RECORD *window, const BUFFER_ASK *ask,
                            int kind)
{
    WINDOW_BUFFER_REQUEST request;
    WINDOW_RECORD before = *window;
    int beyond = ask->height != 0 &&
                 ask->stride > BUFFER_BYTES_LIMIT / 4 / ask->height;
    UINT64 bands[SECTIONS_MOST];
    UINT64 *sections;
    UINT32 count, status, i;
    int expected;

    if (beyond) {
        count = (UINT32)below(4);
        for (i = 0; i < SECTIONS_MOST; i++)
            bands[i] = SECTION_BYTES;
    } else {
        count = oracle_bands(oracle_bytes(ask->stride, ask->height), bands);
        if (count > 0 && kind == 6)
            count--;
        else if (kind == 7)
            bands[count++] = below(2) ? SECTION_BYTES : 0;
        else if (count > 0 && kind == 8)
            bands[0] += PAGE_BYTES;
        else if (count > 0 && kind == 9) {
            bands[0] -= PAGE_BYTES;
            if (count > 1)
                bands[count - 1] += PAGE_BYTES;
        } else if (count > 1 && kind == 10)
            bands[below(count - 1)] -= PAGE_BYTES;
    }
    sections = malloc((count ? count : 1) * sizeof *sections);
    memcpy(sections, bands, count * sizeof *sections);
    memset(&request, 0, sizeof request);
    request.buffer = ask->number;
    request.width = ask->width;
    request.height = ask->height;
    request.stride = ask->stride;
    status = window_give_buffer(window, &request, sections, count);
    expected = expected_buffer_valid(&before, ask, count, sections);
    check(status == (expected ? OK : ERR_INVALID_ARGS),
          "a buffer is recorded or refused as D6 and rule 5 say", status,
          kind);
    if (!expected) {
        check(memcmp(window, &before, sizeof before) == 0,
              "a refused buffer leaves the record as it was", status, kind);
    } else {
        WINDOW_RECORD after = before;

        after.buffers[ask->number].given = 1;
        after.buffers[ask->number].width = ask->width;
        after.buffers[ask->number].height = ask->height;
        after.buffers[ask->number].stride = ask->stride;
        check(memcmp(window, &after, sizeof after) == 0,
              "a given buffer is recorded and nothing else changes", status,
              kind);
    }
    free(sections);
}

static UINT32 given_dimension(unsigned long long high)
{
    static const UINT32 edges[] = {0, 1, 2, WINDOW_WIDTH_MAX,
                                   WINDOW_WIDTH_MAX + 1, UINT32_LARGEST};

    if (below(10) < 7)
        return (UINT32)between(1, high);
    return edges[below(sizeof edges / sizeof edges[0])];
}

static void check_gives(void)
{
    static const UINT32 shown_states[] = {WINDOW_NO_BUFFER, 0, 1, 2};
    static const UINT32 numbers[] = {0, 1, 2, 3, UINT32_LARGEST};
    int round;
    size_t i;

    for (round = 0; round < RANDOM_GIVES; round++) {
        WINDOW_RECORD window;
        BUFFER_ASK ask;

        memset(&window, 0, sizeof window);
        window.shown = shown_states[below(4)];
        for (i = 0; i < WINDOW_BUFFERS_MAX; i++) {
            window.buffers[i].given = (UINT32)below(2);
            window.buffers[i].width = (UINT32)below(300);
            window.buffers[i].stride = (UINT32)below(300);
        }
        ask.number = below(4) == 0 ? numbers[below(5)] : (UINT32)below(3);
        ask.width = given_dimension(WINDOW_WIDTH_MAX);
        ask.height = given_dimension(WINDOW_HEIGHT_MAX);
        switch (below(8)) {
        case 0:
            ask.stride = ask.width - 1;
            break;
        case 1:
            ask.stride = 0;
            break;
        case 2:
            ask.stride = (UINT32)between(1, 9000);
            break;
        case 3:
            ask.stride = UINT32_LARGEST;
            break;
        case 4:
            ask.stride = ask.width + (UINT32)below(65);
            break;
        default:
            ask.stride = ask.width;
            break;
        }
        check_give_case(&window, &ask, below(10) < 5 ? 0 : (int)below(11));
    }
}

/* The sections of a buffer, as D6 bands them, and as a sender can get them
   wrong. */
static void check_band_table(void)
{
    static const struct {
        UINT32 width, height, stride, count;
        UINT64 sections[3];
        int valid;
        const char *what;
    } cases[] = {
        {512, 1024, 512, 1, {SECTION_BYTES}, 1, "2 MiB in one section"},
        {512, 1024, 512, 2, {SECTION_BYTES, 0}, 0, "an empty section after"},
        {512, 1024, 512, 2, {SECTION_BYTES / 2, SECTION_BYTES / 2}, 0,
         "2 MiB in two halves"},
        {512, 1024, 512, 0, {0}, 0, "no section"},
        {512, 1024, 512, 1, {SECTION_BYTES + PAGE_BYTES}, 0,
         "a section a page too large"},
        {512, 1024, 512, 1, {SECTION_BYTES - PAGE_BYTES}, 0,
         "a section a page too small"},
        {100, 100, 100, 1, {40960}, 1, "40000 bytes as 10 pages"},
        {100, 100, 100, 1, {40000}, 0, "40000 bytes not in whole pages"},
        {100, 100, 100, 2, {36864, 4096}, 0, "10 pages in two sections"},
        {1280, 800, 1280, 2, {SECTION_BYTES, 1998848}, 1,
         "1280 x 800 in a full section and the rest"},
        {1280, 800, 1280, 2, {1998848, SECTION_BYTES}, 0,
         "the rest before the full section"},
        {1280, 800, 1280, 2, {SECTION_BYTES, SECTION_BYTES}, 0,
         "two full sections for less"},
        {1280, 800, 1280, 2, {SECTION_BYTES - PAGE_BYTES, 1998848}, 0,
         "a first section a page short and the last right"},
        {1280, 800, 1280, 2, {SECTION_BYTES + PAGE_BYTES, 1998848}, 0,
         "a first section a page long and the last right"},
        {1280, 800, 1280, 3, {SECTION_BYTES, SECTION_BYTES, 0}, 0,
         "a third, empty, section"},
    };
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        WINDOW_RECORD window;
        WINDOW_BUFFER_REQUEST request;

        memset(&window, 0, sizeof window);
        window.shown = WINDOW_NO_BUFFER;
        memset(&request, 0, sizeof request);
        request.buffer = 2;
        request.width = cases[i].width;
        request.height = cases[i].height;
        request.stride = cases[i].stride;
        check(window_give_buffer(&window, &request, cases[i].sections,
                                 cases[i].count) ==
                  (cases[i].valid ? OK : ERR_INVALID_ARGS),
              cases[i].what, cases[i].count, cases[i].valid);
    }
}

/* D6's edges: the least and the greatest a buffer is, and a page more. */
static void check_give_table(void)
{
    static const struct {
        UINT32 width, height, stride;
        int valid;
        const char *what;
    } cases[] = {
        {1, 1, 1, 1, "one pixel is a buffer"},
        {1280, 800, 1280, 1, "a buffer of the screen's size"},
        {7680, 4320, 7680, 1, "the largest window's buffer"},
        {7680, 4096, 8192, 1, "a buffer of exactly 128 MiB"},
        {7680, 4097, 8192, 0, "a buffer of more than 128 MiB"},
        {100, 100, 99, 0, "a stride below the width"},
        {100, 100, 0, 0, "a stride of 0"},
        {100, 100, 100, 1, "a stride of the width"},
        {100, 100, 4096, 1, "a stride beyond the width"},
        {0, 100, 100, 0, "no width"},
        {100, 0, 100, 0, "no height"},
    };
    size_t i;

    for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        WINDOW_RECORD window;
        UINT64 sections[SECTIONS_MOST];
        WINDOW_BUFFER_REQUEST request;
        UINT32 count = oracle_bands(
            oracle_bytes(cases[i].stride, cases[i].height), sections);

        memset(&window, 0, sizeof window);
        window.shown = WINDOW_NO_BUFFER;
        memset(&request, 0, sizeof request);
        request.buffer = 1;
        request.width = cases[i].width;
        request.height = cases[i].height;
        request.stride = cases[i].stride;
        check(window_give_buffer(&window, &request, sections, count) ==
                  (cases[i].valid ? OK : ERR_INVALID_ARGS),
              cases[i].what, cases[i].width, cases[i].stride);
    }
    {
        /* The buffer being read is not given again. */
        WINDOW_RECORD window;
        BUFFER_ASK ask = {1, 100, 100, 100};

        memset(&window, 0, sizeof window);
        window.shown = 1;
        window.buffers[1].given = 1;
        check_give_case(&window, &ask, 0);
        window.shown = 0;
        check_give_case(&window, &ask, 0);
    }
}

/* ------------------------------------------------------------------------
   Presenting (D7; D19, rule 5)

   A present is checked three ways. Its release and its damage are compared
   with what the spec and the notes in display.md D19 say, on every
   present. And the pixels are simulated: a client draws into buffers and
   names the rectangles that differ from the frame it presented before, as
   D7 asks; the compositor composes the library's damage from the buffer
   shown onto a model screen; and that screen must equal what the buffer
   shown has on it after every present, whatever came before: ignored
   presents, buffers given again, buffers of other sizes, many
   configures, a window partly or wholly off the screen. */

/* The rectangle of the screen's pixels in the given one, widened to whole
   blocks of the framebuffer as the output writes them (D8), found row by
   row over the pixels' addresses; empty where no pixel is on the screen. */
static RECT brute_blocks(const SCREEN *screen, INT64 left, INT64 top,
                         INT64 right, INT64 bottom)
{
    RECT out = {0, 0, 0, 0};
    INT64 y;

    left = most_of(left, 0);
    top = most_of(top, 0);
    right = least_of(right, screen->width);
    bottom = least_of(bottom, screen->height);
    if (left >= right || top >= bottom)
        return out;
    out.left = right;
    out.right = left;
    for (y = top; y < bottom; y++) {
        const UINT32 *row = screen->pixels + y * screen->stride;
        INT64 first = left, last = right;

        while (first > 0 && (uintptr_t)(row + first) % COMPOSE_BLOCK_BYTES != 0)
            first--;
        while (last < screen->width &&
               (uintptr_t)(row + last) % COMPOSE_BLOCK_BYTES != 0)
            last++;
        out.left = least_of(out.left, first);
        out.right = most_of(out.right, last);
    }
    out.top = top;
    out.bottom = bottom;
    return out;
}

/* The sizes of the model: an area and screen no larger than this, and
   buffers a little beyond their window. */
#define MODEL_AREA_WIDTH_MAX 100
#define MODEL_AREA_HEIGHT_MAX 50
#define MODEL_BUFFER_EXTRA 12
#define MODEL_STRIDE_EXTRA 5
#define MODEL_BUFFER_PIXELS                                                   \
    ((MODEL_AREA_WIDTH_MAX + MODEL_BUFFER_EXTRA + MODEL_STRIDE_EXTRA) *       \
     (MODEL_AREA_HEIGHT_MAX + MODEL_BUFFER_EXTRA))
#define MODEL_STEPS 40

/* What the screen holds beneath a window, and what a window shows of its
   material where its buffer does not reach; a client's colours are below
   COLOUR_RANGE, so they are neither. */
#define BACKGROUND 0x7F000001u
#define MATERIAL 0x7F000002u
#define COLOUR_RANGE 0x1000000u

/* A buffer or a frame the client has: width 0 before it is made. */
typedef struct {
    UINT32 width, height, stride;
    UINT32 pixels[MODEL_BUFFER_PIXELS];
} MODEL_BUFFER;

/* A window, its client and its compositor, as pixels. shown and owed are
   the model's own account of the buffer read and of the damage owed. */
typedef struct {
    WINDOW_RECORD window;
    SCREEN screen;
    MODEL_BUFFER buffers[WINDOW_BUFFERS_MAX];
    MODEL_BUFFER frame; /* the frame the client presented last, as presented */
    UINT32 client_serial;
    UINT32 shown, owed;
    UINT32 owned; /* a bit for each buffer presented and not yet released */
    UINT32 composed[MODEL_AREA_WIDTH_MAX * MODEL_AREA_HEIGHT_MAX];
} SESSION;

static SESSION session;

static UINT32 pixel_at(const MODEL_BUFFER *buffer, UINT32 x, UINT32 y)
{
    return buffer->pixels[(size_t)y * buffer->stride + x];
}

static UINT32 random_colour(void)
{
    return (UINT32)between(1, COLOUR_RANGE - 1);
}

static void paint(MODEL_BUFFER *buffer, UINT32 x, UINT32 y, UINT32 width,
                  UINT32 height, UINT32 colour)
{
    UINT32 row, column;

    for (row = y; row < y + height && row < buffer->height; row++)
        for (column = x; column < x + width && column < buffer->width; column++)
            buffer->pixels[(size_t)row * buffer->stride + column] = colour;
}

/* Paint a rectangle somewhere in the buffer in a colour of its own. */
static void paint_part(MODEL_BUFFER *buffer)
{
    UINT32 x = (UINT32)below(buffer->width);
    UINT32 y = (UINT32)below(buffer->height);

    paint(buffer, x, y, (UINT32)between(1, buffer->width - x),
          (UINT32)between(1, buffer->height - y), random_colour());
}

static void new_buffer(MODEL_BUFFER *buffer, UINT32 width, UINT32 height,
                       UINT32 stride)
{
    UINT32 parts = (UINT32)below(4);

    buffer->width = width;
    buffer->height = height;
    buffer->stride = stride;
    paint(buffer, 0, 0, width, height, random_colour());
    for (; parts > 0; parts--)
        paint_part(buffer);
}

/* What a client does with a buffer it owns before it presents it: starts
   from the frame it presented last where that is the buffer's size, then
   draws a few rectangles, or all of it. */
static void client_draws(const SESSION *s, MODEL_BUFFER *buffer)
{
    UINT32 parts = (UINT32)below(4), x, y;

    if (s->frame.width == buffer->width && s->frame.height == buffer->height &&
        below(10) < 6)
        for (y = 0; y < buffer->height; y++)
            for (x = 0; x < buffer->width; x++)
                buffer->pixels[(size_t)y * buffer->stride + x] =
                    pixel_at(&s->frame, x, y);
    for (; parts > 0; parts--)
        paint_part(buffer);
    if (below(8) == 0)
        paint(buffer, 0, 0, buffer->width, buffer->height, random_colour());
}

/* The rectangles a client that follows D7 names for a buffer: they cover
   every pixel that differs from the frame it presented before, and may
   cover more; none names the whole buffer. */
static UINT32 client_rects(const SESSION *s, const MODEL_BUFFER *buffer,
                           WINDOW_RECT *rects)
{
    UINT32 left = buffer->width, top = buffer->height, right = 0, bottom = 0;
    UINT32 count, i, x, y;

    if (s->frame.width != buffer->width || s->frame.height != buffer->height ||
        below(4) == 0)
        return 0;
    for (y = 0; y < buffer->height; y++)
        for (x = 0; x < buffer->width; x++)
            if (pixel_at(buffer, x, y) != pixel_at(&s->frame, x, y)) {
                left = (UINT32)least_of(left, x);
                top = (UINT32)least_of(top, y);
                right = (UINT32)most_of(right, x + 1);
                bottom = (UINT32)most_of(bottom, y + 1);
            }
    if (right <= left || bottom <= top) {
        if (below(2) == 0)
            return 0;
        rects[0].x = (UINT32)below(buffer->width + 4);
        rects[0].y = (UINT32)below(buffer->height + 4);
        rects[0].width = (UINT32)between(1, buffer->width);
        rects[0].height = (UINT32)between(1, buffer->height);
        return 1;
    }
    count = (UINT32)between(1, least_of(WINDOW_DAMAGE_MAX, bottom - top));
    for (i = 0; i < count; i++) {
        UINT32 band_top = top + (bottom - top) * i / count;
        UINT32 band_bottom = top + (bottom - top) * (i + 1) / count;

        rects[i].x = left;
        rects[i].y = band_top;
        rects[i].width = right - left;
        rects[i].height = band_bottom - band_top;
        if (below(4) == 0) {
            UINT32 before = (UINT32)least_of(rects[i].x, below(3));

            rects[i].x -= before;
            rects[i].width += before + (UINT32)below(6);
        }
    }
    if (count < WINDOW_DAMAGE_MAX && below(8) == 0) {
        /* A rectangle whose far edges are 2^32, past what 32 bits hold. */
        UINT32 start = 1 + (UINT32)below(3);

        rects[count].x = start;
        rects[count].y = start;
        rects[count].width = 0u - start;
        rects[count].height = 0u - start;
        count++;
    }
    return count;
}

/* What the screen must show at (x, y): the window's buffer where it is on
   the window, anchored at the window's top left corner, the window's
   material where the buffer does not reach, and what is beneath the window
   everywhere else. */
static UINT32 should_show(const SESSION *s, INT64 x, INT64 y)
{
    INT64 column = x - s->window.x, row = y - s->window.y;
    const MODEL_BUFFER *buffer;

    if (s->shown == WINDOW_NO_BUFFER || column < 0 || row < 0 ||
        column >= s->window.width || row >= s->window.height)
        return BACKGROUND;
    buffer = &s->buffers[s->shown];
    if (column >= buffer->width || row >= buffer->height)
        return MATERIAL;
    return pixel_at(buffer, (UINT32)column, (UINT32)row);
}

static UINT32 *composed_at(SESSION *s, INT64 x, INT64 y)
{
    return &s->composed[y * MODEL_AREA_WIDTH_MAX + x];
}

/* Compose rect onto the model screen from the buffer shown, as the
   compositor does with the damage the library gives. */
static void compose_rect(SESSION *s, const RECT *rect)
{
    INT64 bottom = least_of(rect->bottom, s->screen.height);
    INT64 right = least_of(rect->right, s->screen.width);
    INT64 x, y;

    for (y = most_of(rect->top, 0); y < bottom; y++)
        for (x = most_of(rect->left, 0); x < right; x++)
            *composed_at(s, x, y) = should_show(s, x, y);
}

/* The pixels of the model screen that are not what the buffer shown has. */
static unsigned wrong_pixels(SESSION *s)
{
    unsigned wrong = 0;
    INT64 x, y;

    for (y = 0; y < s->screen.height; y++)
        for (x = 0; x < s->screen.width; x++)
            wrong += *composed_at(s, x, y) != should_show(s, x, y);
    return wrong;
}

/* Give the buffer, with its sections as D6 bands them; returns the
   library's answer. */
static UINT32 give_record(WINDOW_RECORD *window, UINT32 number, UINT32 width,
                          UINT32 height, UINT32 stride)
{
    UINT64 sections[SECTIONS_MOST];
    UINT32 count = oracle_bands(oracle_bytes(stride, height), sections);
    WINDOW_BUFFER_REQUEST request;

    memset(&request, 0, sizeof request);
    request.buffer = number;
    request.width = width;
    request.height = height;
    request.stride = stride;
    return window_give_buffer(window, &request, sections, count);
}

static void give_step(SESSION *s)
{
    UINT32 number = (UINT32)below(WINDOW_BUFFERS_MAX);
    UINT32 width, height, stride, status;

    switch (below(4)) {
    case 0:
        width = s->window.width;
        height = s->window.height;
        break;
    case 1:
        width = s->shown == WINDOW_NO_BUFFER ? s->window.width
                                             : s->buffers[s->shown].width;
        height = s->shown == WINDOW_NO_BUFFER ? s->window.height
                                              : s->buffers[s->shown].height;
        break;
    default:
        width = (UINT32)between(1, s->window.width + MODEL_BUFFER_EXTRA);
        height = (UINT32)between(1, s->window.height + MODEL_BUFFER_EXTRA);
        break;
    }
    stride = width + (UINT32)below(MODEL_STRIDE_EXTRA + 1);
    status = give_record(&s->window, number, width, height, stride);
    check(status == (number == s->shown ? ERR_INVALID_ARGS : OK),
          "a buffer is given unless it is the one being read", status, number);
    if (number != s->shown)
        new_buffer(&s->buffers[number], width, height, stride);
}

/* A present the library refuses: of a number there is not, of a buffer
   never given, or of too many rectangles; it changes nothing. */
static void refused_present(SESSION *s)
{
    WINDOW_RECT rects[WINDOW_DAMAGE_MAX + 4];
    WINDOW_PRESENT_REQUEST request;
    WINDOW_PRESENTED got, untouched;
    WINDOW_RECORD before = s->window;
    UINT32 number = WINDOW_BUFFER_LAST + 1 + (UINT32)below(3);
    UINT32 i, count = 0;

    memset(rects, 0, sizeof rects);
    if (below(2) == 0)
        for (i = 0; i < WINDOW_BUFFERS_MAX; i++)
            if (s->buffers[i].width == 0)
                number = i;
    if (number <= WINDOW_BUFFER_LAST && s->buffers[number].width != 0)
        count = WINDOW_DAMAGE_MAX + 1 + (UINT32)below(4);
    memset(&request, 0, sizeof request);
    request.buffer = number;
    request.serial = s->window.serial;
    request.rectangles = count;
    memset(&got, 0xA5, sizeof got);
    untouched = got;
    check(window_present(&s->window, &s->screen, &request, rects, &got) ==
              ERR_INVALID_ARGS,
          "a present of a buffer never given, or of too many rectangles, is "
          "refused",
          number, count);
    check(memcmp(&s->window, &before, sizeof before) == 0 &&
              memcmp(&got, &untouched, sizeof got) == 0,
          "a refused present leaves everything as it was", number, count);
}

/* One present, compared with what D7 and rule 5 say, with the notes in
   D19, and with the pixels. rects holds count rectangles. */
static void present_step(SESSION *s, UINT32 number, UINT32 serial,
                         UINT32 count, const WINDOW_RECT *rects)
{
    WINDOW_PRESENT_REQUEST request;
    WINDOW_PRESENTED got;
    WINDOW_RECORD after = s->window;
    const MODEL_BUFFER *buffer = &s->buffers[number];
    int stale = serial == 0 || serial != s->window.serial;
    UINT32 released = 0, wanted = 0, status, i;
    RECT expected[WINDOW_DAMAGE_MAX];
    UINT32 x = s->window.x, y = s->window.y;

    memset(&request, 0, sizeof request);
    request.buffer = number;
    request.serial = serial;
    request.rectangles = count;
    memset(&got, 0xA5, sizeof got);
    status = window_present(&s->window, &s->screen, &request,
                            count ? rects : NULL, &got);
    check(status == OK, "a present of a given buffer is answered", status,
          number);
    if (stale) {
        if (number != s->shown)
            released = 1u << number;
        s->owed = 1;
    } else {
        const MODEL_BUFFER *old = s->shown == WINDOW_NO_BUFFER
                                      ? NULL
                                      : &s->buffers[s->shown];
        int whole = s->owed || old == NULL || old->width != buffer->width ||
                    old->height != buffer->height;
        long long columns = least_of(buffer->width, s->window.width);
        long long rows = least_of(buffer->height, s->window.height);

        if (old != NULL && s->shown != number)
            released = 1u << s->shown;
        for (i = 0; i < (whole || count == 0 ? 1 : count); i++) {
            long long left = 0, top = 0;
            long long right = whole ? s->window.width : columns;
            long long bottom = whole ? s->window.height : rows;
            RECT blocks;

            if (!whole && count != 0) {
                left = rects[i].x;
                top = rects[i].y;
                right = least_of((long long)rects[i].x + rects[i].width,
                                 columns);
                bottom = least_of((long long)rects[i].y + rects[i].height,
                                  rows);
            }
            if (left >= right || top >= bottom)
                continue;
            blocks = brute_blocks(&s->screen, x + left, y + top, x + right,
                                  y + bottom);
            if (blocks.left < blocks.right && blocks.top < blocks.bottom)
                expected[wanted++] = blocks;
        }
        s->shown = number;
        s->owed = 0;
    }
    after.shown = s->shown;
    after.owed = s->owed;
    check(memcmp(&s->window, &after, sizeof after) == 0,
          "a present changes the shown buffer and what is owed, and nothing "
          "else",
          number, serial);
    check(got.shown == (UINT32)!stale,
          "a present is shown if its serial is the latest", got.shown, serial);
    check(got.released == released, "a present releases what it stops reading",
          got.released, released);
    check(got.count == wanted, "a present's damage has a rectangle each",
          got.count, wanted);
    for (i = 0; i < got.count && i < wanted; i++) {
        check(got.damage[i].left == expected[i].left &&
                  got.damage[i].top == expected[i].top &&
                  got.damage[i].right == expected[i].right &&
                  got.damage[i].bottom == expected[i].bottom,
              "a rectangle is cut to its buffer and widened to blocks",
              got.damage[i].left, expected[i].left);
        check(got.damage[i].left >= 0 &&
                  got.damage[i].right <= s->screen.width &&
                  got.damage[i].top >= (INT64)s->window.y &&
                  got.damage[i].bottom <=
                      (INT64)s->window.y + (INT64)s->window.height,
              "damage is on the screen and in the window's rows", i,
              got.damage[i].top);
    }
    for (i = 0; i < got.count && i < WINDOW_DAMAGE_MAX; i++)
        compose_rect(s, &got.damage[i]);
    check(wrong_pixels(s) == 0,
          "after a present the screen shows what the buffer shown has, "
          "whatever the client's frames before",
          wrong_pixels(s), number);
    s->owned = (s->owned | 1u << number) & ~got.released;
    check(s->owned == (s->shown == WINDOW_NO_BUFFER ? 0 : 1u << s->shown),
          "the compositor reads exactly the buffer shown, and every other "
          "presented is released",
          s->owned, s->shown);
    s->frame = *buffer;
}

/* One step of a client and a compositor. */
static void session_step(SESSION *s)
{
    WINDOW_RECT rects[WINDOW_DAMAGE_MAX];
    UINT32 number = (UINT32)below(WINDOW_BUFFERS_MAX);
    UINT32 serial = s->client_serial, count;

    switch (below(12)) {
    case 0:
    case 1:
        window_configure(&s->window);
        return;
    case 2:
    case 3:
        s->client_serial = s->window.serial;
        return;
    case 4:
        give_step(s);
        return;
    case 5:
        refused_present(s);
        return;
    default:
        break;
    }
    if (s->buffers[number].width == 0)
        return;
    if (number != s->shown)
        client_draws(s, &s->buffers[number]);
    count = client_rects(s, &s->buffers[number], rects);
    switch (below(14)) {
    case 0:
        serial = 0;
        break;
    case 1:
        serial = UINT32_LARGEST;
        break;
    case 2:
        serial = s->window.serial + 1;
        break;
    default:
        break;
    }
    present_step(s, number, serial, count, rects);
}

static void check_present_sessions(UINT32 *memory)
{
    int round, step;

    for (round = 0; round < PRESENT_SESSIONS; round++) {
        SESSION *s = &session;
        WINDOW_AREA area;
        DISPLAY_WINDOW_REQUEST request;
        UINT32 i;

        memset(s, 0, sizeof *s);
        area.screen_width = (UINT32)between(16, MODEL_AREA_WIDTH_MAX);
        area.screen_height = (UINT32)between(10, MODEL_AREA_HEIGHT_MAX);
        area.strip = below(2) ? 0 : (UINT32)below(area.screen_height / 3);
        memset(&request, 0, sizeof request);
        request.width = (UINT32)between(1, area.screen_width + 8);
        request.height = (UINT32)between(1, area.screen_height + 8);
        request.layer = DISPLAY_LAYER_SYSTEM;
        request.surface = DISPLAY_SURFACE_PANEL;
        check(window_open(&area, (UINT32)below(4), DISPLAY_LAYER_SYSTEM, 0,
                          &request, &s->window) == OK,
              "a window is made on a test screen", area.screen_width,
              area.screen_height);
        /* The screen is the area, or less than it, so that the window is
           partly, or wholly, off it. */
        s->screen.width = area.screen_width;
        s->screen.height = area.screen_height;
        switch (below(3)) {
        case 1:
            s->screen.width = (INT64)between(1, area.screen_width);
            s->screen.height = (INT64)between(1, area.screen_height);
            break;
        case 2:
            if (s->window.x > 0)
                s->screen.width = (INT64)between(1, s->window.x);
            else if (s->window.y > 0)
                s->screen.height = (INT64)between(1, s->window.y);
            break;
        default:
            break;
        }
        s->screen.stride = s->screen.width + (INT64)below(20);
        s->screen.pixels = memory + below(BLOCK_PIXELS);
        s->screen.format = below(2) ? PIXEL_RGB : PIXEL_BGR;
        s->shown = WINDOW_NO_BUFFER;
        for (i = 0; i < MODEL_AREA_WIDTH_MAX * MODEL_AREA_HEIGHT_MAX; i++)
            s->composed[i] = BACKGROUND;
        for (i = 0; i < WINDOW_BUFFERS_MAX; i++)
            if (below(4) != 0)
                give_step(s);
        if (below(10) < 8)
            window_configure(&s->window);
        for (step = 0; step < MODEL_STEPS; step++)
            session_step(s);
    }
}

/* A window on a screen of whole blocks, in numbers a reader can follow:
   128 x 64 pixels, blocks of 16, a 40 x 20 window at (44, 15). */
static void check_present_table(UINT32 *memory)
{
    static const WINDOW_AREA area = {128, 64, 0};
    SCREEN screen;
    DISPLAY_WINDOW_REQUEST request;
    WINDOW_RECORD window;
    WINDOW_PRESENT_REQUEST present;
    WINDOW_PRESENTED got;
    WINDOW_RECT rect;
    UINT32 status;

    screen.pixels = memory;
    screen.width = 128;
    screen.height = 64;
    screen.stride = 128;
    screen.format = PIXEL_BGR;
    memset(&request, 0, sizeof request);
    request.width = 40;
    request.height = 20;
    request.layer = DISPLAY_LAYER_SYSTEM;
    request.surface = DISPLAY_SURFACE_PANEL;
    memset(&window, 0, sizeof window);
    status = window_open(&area, 0, DISPLAY_LAYER_SYSTEM, 0, &request, &window);
    check(status == OK && window.x == 44 && window.y == 15,
          "the table's window is at (44, 15)", window.x, window.y);
    check(give_record(&window, 0, 40, 20, 40) == OK &&
              give_record(&window, 1, 40, 20, 40) == OK,
          "the table's buffers are given", 0, 0);
    memset(&present, 0, sizeof present);
    memset(&got, 0, sizeof got);
    present.buffer = 0;
    present.serial = window_configure(&window);
    status = window_present(&window, &screen, &present, NULL, &got);
    check(status == OK && got.shown == 1 && got.released == 0 &&
              got.count == 1 && got.damage[0].left == 32 &&
              got.damage[0].top == 15 && got.damage[0].right == 96 &&
              got.damage[0].bottom == 35,
          "a first frame damages the whole window, in blocks", got.count,
          got.damage[0].left);
    present.buffer = 1;
    rect.x = 0;
    rect.y = 0;
    rect.width = 1;
    rect.height = 1;
    present.rectangles = 1;
    status = window_present(&window, &screen, &present, &rect, &got);
    check(status == OK && got.shown == 1 && got.released == 1u << 0 &&
              got.count == 1 && got.damage[0].left == 32 &&
              got.damage[0].top == 15 && got.damage[0].right == 48 &&
              got.damage[0].bottom == 16,
          "a pixel's damage is its block, and the buffer before is "
          "released",
          got.released, got.damage[0].right);
    present.buffer = 0;
    rect.x = 30;
    rect.y = 10;
    rect.width = 100;
    rect.height = 100;
    status = window_present(&window, &screen, &present, &rect, &got);
    check(status == OK && got.released == 1u << 1 && got.count == 1 &&
              got.damage[0].left == 64 && got.damage[0].top == 25 &&
              got.damage[0].right == 96 && got.damage[0].bottom == 35,
          "a rectangle beyond the buffer is cut to it", got.damage[0].left,
          got.damage[0].right);
    rect.x = 50;
    rect.y = 0;
    status = window_present(&window, &screen, &present, &rect, &got);
    check(status == OK && got.shown == 1 && got.count == 0,
          "a rectangle outside the buffer damages nothing, and the frame is "
          "still shown",
          status, got.count);
    present.rectangles = 0;
    present.serial = 2;
    status = window_present(&window, &screen, &present, NULL, &got);
    check(status == OK && got.shown == 0 && got.released == 0 &&
              got.count == 0 && window.shown == 0,
          "the buffer being read, presented with a serial that is not the "
          "latest, changes nothing",
          got.shown, got.released);
    present.buffer = 1;
    status = window_present(&window, &screen, &present, NULL, &got);
    check(status == OK && got.shown == 0 && got.released == 1u << 1 &&
              window.shown == 0,
          "another buffer with a serial that is not the latest is released "
          "at once",
          got.shown, got.released);
    present.serial = 0;
    status = window_present(&window, &screen, &present, NULL, &got);
    check(status == OK && got.shown == 0 && got.released == 1u << 1,
          "serial 0 names no configure", got.shown, got.released);
    present.buffer = 2;
    status = window_present(&window, &screen, &present, NULL, &got);
    check(status == ERR_INVALID_ARGS, "a buffer never given is refused",
          status, 0);
    present.buffer = WINDOW_BUFFERS_MAX;
    status = window_present(&window, &screen, &present, NULL, &got);
    check(status == ERR_INVALID_ARGS, "a buffer number there is not is refused",
          status, 0);
    present.buffer = 0;
    present.serial = 1;
    present.rectangles = WINDOW_DAMAGE_MAX + 1;
    status = window_present(&window, &screen, &present, NULL, &got);
    check(status == ERR_INVALID_ARGS, "more rectangles than the most are "
                                      "refused",
          status, 0);
}

/* A present that is ignored leaves the screen as it was, so the next
   frame shown must damage the whole window, not only the rectangles the
   client names against the frame that was ignored. A window of 100 x 40 at
   (14, 5) on a screen of 128 x 64. */
static void check_ignored_present_table(UINT32 *memory)
{
    static const WINDOW_AREA area = {128, 64, 0};
    SCREEN screen;
    DISPLAY_WINDOW_REQUEST request;
    WINDOW_RECORD window;
    WINDOW_PRESENT_REQUEST present;
    WINDOW_PRESENTED got;
    WINDOW_RECT rect;
    UINT32 first, second, status;

    screen.pixels = memory;
    screen.width = 128;
    screen.height = 64;
    screen.stride = 128;
    screen.format = PIXEL_BGR;
    memset(&request, 0, sizeof request);
    request.width = 100;
    request.height = 40;
    request.layer = DISPLAY_LAYER_SYSTEM;
    request.surface = DISPLAY_SURFACE_PANEL;
    memset(&window, 0, sizeof window);
    status = window_open(&area, 0, DISPLAY_LAYER_SYSTEM, 0, &request, &window);
    check(status == OK && window.x == 14 && window.y == 5,
          "the ignored table's window is at (14, 5)", window.x, window.y);
    check(give_record(&window, 0, 100, 40, 100) == OK &&
              give_record(&window, 1, 100, 40, 100) == OK &&
              give_record(&window, 2, 100, 40, 100) == OK,
          "the ignored table's buffers are given", 0, 0);
    memset(&present, 0, sizeof present);
    present.serial = first = window_configure(&window);
    check(window_present(&window, &screen, &present, NULL, &got) == OK &&
              got.shown == 1 && got.count == 1 && got.damage[0].left == 0 &&
              got.damage[0].top == 5 && got.damage[0].right == 128 &&
              got.damage[0].bottom == 45,
          "the first frame damages the whole window", got.count, 0);
    second = window_configure(&window);
    present.buffer = 1;
    present.serial = first;
    present.rectangles = 1;
    rect.x = 0;
    rect.y = 0;
    rect.width = 16;
    rect.height = 16;
    check(window_present(&window, &screen, &present, &rect, &got) == OK &&
              got.shown == 0 && got.released == 1u << 1 && got.count == 0,
          "a frame for the configure before is ignored and released", got.shown,
          got.released);
    present.buffer = 2;
    present.serial = second;
    rect.x = 80;
    rect.y = 20;
    check(window_present(&window, &screen, &present, &rect, &got) == OK &&
              got.shown == 1 && got.released == 1u << 0 && got.count == 1 &&
              got.damage[0].left == 0 && got.damage[0].top == 5 &&
              got.damage[0].right == 128 && got.damage[0].bottom == 45,
          "the frame shown after an ignored one damages the whole window",
          got.count, got.damage[0].top);
    present.buffer = 1;
    rect.x = 0;
    rect.y = 0;
    rect.width = 1;
    rect.height = 1;
    check(window_present(&window, &screen, &present, &rect, &got) == OK &&
              got.shown == 1 && got.released == 1u << 2 && got.count == 1 &&
              got.damage[0].left == 0 && got.damage[0].top == 5 &&
              got.damage[0].right == 16 && got.damage[0].bottom == 6,
          "and the frame after that damages the rectangles it names",
          got.count, got.damage[0].right);
}

/* A window partly and wholly off the screen: its damage is what lies on
   the screen, and a frame with none is still shown. A 100 x 40 window at
   (100, 5) in an area of 300 x 64. */
static void check_off_screen_table(UINT32 *memory)
{
    static const WINDOW_AREA area = {300, 64, 0};
    static const INT64 widths[] = {128, 64};
    SCREEN screen;
    DISPLAY_WINDOW_REQUEST request;
    size_t i;

    memset(&request, 0, sizeof request);
    request.width = 100;
    request.height = 40;
    request.layer = DISPLAY_LAYER_SYSTEM;
    request.surface = DISPLAY_SURFACE_PANEL;
    for (i = 0; i < sizeof widths / sizeof widths[0]; i++) {
        WINDOW_RECORD window;
        WINDOW_PRESENT_REQUEST present;
        WINDOW_PRESENTED got;
        UINT32 status;

        screen.pixels = memory;
        screen.width = widths[i];
        screen.height = 64;
        screen.stride = widths[i];
        screen.format = PIXEL_BGR;
        memset(&window, 0, sizeof window);
        status = window_open(&area, 0, DISPLAY_LAYER_SYSTEM, 0, &request,
                             &window);
        check(status == OK && window.x == 100 && window.y == 5 &&
                  give_record(&window, 0, 100, 40, 100) == OK,
              "the off-screen window is at (100, 5)", window.x, window.y);
        memset(&present, 0, sizeof present);
        present.serial = window_configure(&window);
        check(window_present(&window, &screen, &present, NULL, &got) == OK &&
                  got.shown == 1,
              "a frame off the screen is shown", got.shown, i);
        if (i == 0)
            check(got.count == 1 && got.damage[0].left == 96 &&
                      got.damage[0].top == 5 && got.damage[0].right == 128 &&
                      got.damage[0].bottom == 45,
                  "a window partly off the screen damages what is on it",
                  got.count, got.damage[0].left);
        else
            check(got.count == 0, "a window wholly off the screen damages "
                                  "nothing",
                  got.count, 0);
    }
}

int main(void)
{
    UINT32 *memory = aligned_alloc(COMPOSE_BLOCK_BYTES,
                                   SCREEN_MEMORY_PIXELS * sizeof *memory);

    if (memory == NULL) {
        printf("out of memory\n");
        return 2;
    }
    /* A library that fails a rule may also fault, and the failures printed
       before the fault are what says which rule. */
    setvbuf(stdout, NULL, _IOLBF, 0);
    check_sizes();
    check_places();
    check_panel_agrees();
    check_opens();
    check_configure();
    check_buffer_sizes();
    check_give_table();
    check_band_table();
    check_gives();
    check_present_table(memory);
    check_ignored_present_table(memory);
    check_off_screen_table(memory);
    check_present_sessions(memory);
    free(memory);
    printf("%llu checks, %u failed\n", checks, failures);
    return failures != 0;
}
