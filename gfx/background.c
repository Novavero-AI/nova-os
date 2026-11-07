/* The desktop background's arrival. Limb is drawn once into memory from
   the page allocator and the part of the screen the boot screen drew on
   is saved; then each frame mixes the two images in linear light, as the
   boot screen's anti-aliasing mixes ink with black, one step of
   FADE_FRAMES further toward Limb. The timer paces the steps over FADE_MS,
   and none is skipped: a step that comes due while the one before is
   still being drawn follows straight after it, so a slow machine shows
   every step, only later. */

#include "background.h"
#include "cpu.h"
#include "limb.h"
#include "memory_layout.h"
#include "mm/page.h"
#include "x86_64/arch.h"
#include "x86_64/serial.h"

/* A step per refresh of a 60 Hz display, which shows no more than that,
   over four tenths of a second; each step moves a twenty-fourth of the
   light. */
#define FADE_FRAMES 24
#define FADE_MS 400
#define MS_PER_SECOND 1000
#define FADE_TICKS (FADE_MS * TIMER_HZ / MS_PER_SECOND)

/* A pixel holds three 8-bit channels from bit 0 up, in the screen's
   order, which both images share; the fade treats each alike. */
#define CHANNELS 3
#define CHANNEL_BITS 8
#define CHANNEL_MASK 0xFFu
#define COLOUR_MASK 0xFFFFFFu
#define CODES 256

/* ------------------------------------------------------------------------
   Offscreen images. The page allocator hands out one page at a time, not
   necessarily next to the one before, so an image is a chain of pages,
   filled and read in order, row after row. */

typedef struct PIXEL_PAGE PIXEL_PAGE;

#define PAGE_PIXELS \
    ((INT64)((PAGE_SIZE - sizeof(PIXEL_PAGE *)) / sizeof(UINT32)))

struct PIXEL_PAGE {
    PIXEL_PAGE *next;
    UINT32 pixels[PAGE_PIXELS];
};

_Static_assert(sizeof(PIXEL_PAGE) == PAGE_SIZE, "a pixel page fills a page");

static void chain_free(PIXEL_PAGE *page)
{
    while (page) {
        PIXEL_PAGE *next = page->next;

        page_free((UINT64)page - DIRECT_MAP_BASE);
        page = next;
    }
}

/* Allocate a chain of pages for count pixels into *chain. Returns 0, with
   every page it took freed, if the allocator runs out. A page reads as
   zero when allocated, so its next is 0 until the page after it is
   linked. */
static int chain_alloc(INT64 count, PIXEL_PAGE **chain)
{
    PIXEL_PAGE **link = chain;
    INT64 held;

    *chain = 0;
    for (held = 0; held < count; held += PAGE_PIXELS) {
        UINT64 physical = page_alloc();

        if (!physical) {
            chain_free(*chain);
            *chain = 0;
            return 0;
        }
        *link = direct_map(physical);
        link = &(*link)->next;
    }
    return 1;
}

/* A place in a chain: the next pixel is page->pixels[index]. */
typedef struct {
    PIXEL_PAGE *page;
    INT64 index;
} CURSOR;

static void cursor_start(CURSOR *c, PIXEL_PAGE *chain)
{
    c->page = chain;
    c->index = 0;
}

/* The pixels from c to the end of its page, at most limit. */
static INT64 cursor_run(const CURSOR *c, INT64 limit)
{
    INT64 room = PAGE_PIXELS - c->index;

    return room < limit ? room : limit;
}

static UINT32 *cursor_at(const CURSOR *c)
{
    return c->page->pixels + c->index;
}

/* Move c past count pixels, no more than cursor_run gave. */
static void cursor_advance(CURSOR *c, INT64 count)
{
    c->index += count;
    if (c->index == PAGE_PIXELS) {
        c->page = c->page->next;
        c->index = 0;
    }
}

/* ------------------------------------------------------------------------
   Drawing and saving. */

static UINT32 screen_pixel(const SCREEN *screen, UINT32 rgb)
{
    return gfx_pixel(screen, rgb >> 2 * CHANNEL_BITS & CHANNEL_MASK,
                     rgb >> CHANNEL_BITS & CHANNEL_MASK, rgb & CHANNEL_MASK);
}

/* Draw Limb into image, a chain the size of the screen. */
static void draw_offscreen(const SCREEN *screen, const LIMB *limb,
                           PIXEL_PAGE *image)
{
    CURSOR c;
    INT64 x, y;

    cursor_start(&c, image);
    for (y = 0; y < screen->height; y++)
        for (x = 0; x < screen->width;) {
            INT64 run = cursor_run(&c, screen->width - x), i;
            UINT32 *out = cursor_at(&c);

            for (i = 0; i < run; i++)
                out[i] = screen_pixel(screen, limb_pixel(limb, x + i, y));
            cursor_advance(&c, run);
            x += run;
        }
}

static void draw_onscreen(const SCREEN *screen, const LIMB *limb)
{
    INT64 x, y;

    for (y = 0; y < screen->height; y++)
        for (x = 0; x < screen->width; x++)
            screen->pixels[y * screen->stride + x] =
                screen_pixel(screen, limb_pixel(limb, x, y));
}

/* Copy the screen's pixels in box into saved, a chain the size of box. */
static void save(const SCREEN *screen, const RECT *box, PIXEL_PAGE *saved)
{
    CURSOR c;
    INT64 x, y;

    cursor_start(&c, saved);
    for (y = box->top; y < box->bottom; y++)
        for (x = box->left; x < box->right;) {
            INT64 run = cursor_run(&c, box->right - x), i;
            UINT32 *out = cursor_at(&c);
            const UINT32 *in = screen->pixels + y * screen->stride + x;

            for (i = 0; i < run; i++)
                out[i] = in[i];
            cursor_advance(&c, run);
            x += run;
        }
}

/* ------------------------------------------------------------------------
   The fade. */

/* One frame's mix, weight steps of FADE_FRAMES toward Limb. Where the
   boot screen is black, a channel's mix depends on Limb's code alone, so
   over_black holds it for every code; that covers every pixel but the
   strokes. */
typedef struct {
    INT64 weight;
    UINT32 over_black[CODES];
} MIX;

static void mix_start(MIX *mix, INT64 weight)
{
    INT64 code;

    mix->weight = weight;
    for (code = 0; code < CODES; code++)
        mix->over_black[code] =
            (UINT32)gfx_cross_fade(0, code, weight, FADE_FRAMES);
}

static UINT32 mix_pixels(const MIX *mix, UINT32 from, UINT32 to)
{
    UINT32 out = 0;
    int shift;

    for (shift = 0; shift < CHANNELS * CHANNEL_BITS; shift += CHANNEL_BITS)
        out |= (UINT32)gfx_cross_fade(from >> shift & CHANNEL_MASK,
                                      to >> shift & CHANNEL_MASK,
                                      mix->weight, FADE_FRAMES)
               << shift;
    return out;
}

/* Mix count pixels of Limb, read at limb, into out: with the saved boot
   screen read at boot, or with black where boot is 0. This is the fade's
   inner loop, so the mix with black is written out in it: TinyCC inlines
   no call. */
static void fade_span(const MIX *mix, CURSOR *limb, CURSOR *boot,
                      UINT32 *out, INT64 count)
{
    const UINT32 *over_black = mix->over_black;

    while (count > 0) {
        INT64 run = cursor_run(limb, count), i;
        const UINT32 *to = cursor_at(limb), *from = 0;

        if (boot) {
            run = cursor_run(boot, run);
            from = cursor_at(boot);
        }
        for (i = 0; i < run; i++) {
            UINT32 p = to[i];

            out[i] = from && from[i] & COLOUR_MASK
                         ? mix_pixels(mix, from[i], p)
                         : over_black[p & CHANNEL_MASK] |
                               over_black[p >> CHANNEL_BITS & CHANNEL_MASK]
                                   << CHANNEL_BITS |
                               over_black[p >> 2 * CHANNEL_BITS &
                                          CHANNEL_MASK]
                                   << 2 * CHANNEL_BITS;
        }
        if (boot)
            cursor_advance(boot, run);
        cursor_advance(limb, run);
        out += run;
        count -= run;
    }
}

static void fade_frame(const SCREEN *screen, const RECT *box,
                       PIXEL_PAGE *image, PIXEL_PAGE *saved, INT64 weight)
{
    MIX mix;
    CURSOR limb, boot;
    INT64 y;

    mix_start(&mix, weight);
    cursor_start(&limb, image);
    cursor_start(&boot, saved);
    for (y = 0; y < screen->height; y++) {
        UINT32 *row = screen->pixels + y * screen->stride;

        if (y < box->top || y >= box->bottom) {
            fade_span(&mix, &limb, 0, row, screen->width);
            continue;
        }
        fade_span(&mix, &limb, 0, row, box->left);
        fade_span(&mix, &limb, &boot, row + box->left, box->right - box->left);
        fade_span(&mix, &limb, 0, row + box->right,
                  screen->width - box->right);
    }
}

/* ------------------------------------------------------------------------
   Timing and the log. Durations come from the timestamp counter, finer
   than the timer's millisecond ticks; under QEMU's TCG the ticks also
   fall behind while the processor is busy. */

#define TENTHS_PER_MS 10

/* How long each part took, in timestamp-counter cycles. */
typedef struct {
    UINT64 drawn, faded, busy, busiest;
} TIMES;

static void serial_ms(UINT64 cycles, UINT64 tsc_per_ms)
{
    UINT64 tenths = cycles * TENTHS_PER_MS / tsc_per_ms;

    serial_number(tenths / TENTHS_PER_MS);
    serial_write(".");
    serial_number(tenths % TENTHS_PER_MS);
    serial_write(" ms");
}

static void report(const TIMES *times, UINT64 tsc_per_ms)
{
    UINT64 flags = serial_lock();

    serial_write("nova: background drawn in ");
    serial_ms(times->drawn, tsc_per_ms);
    serial_write(", faded in over ");
    serial_number(FADE_FRAMES);
    serial_write(" frames in ");
    serial_ms(times->faded, tsc_per_ms);
    serial_write(" (");
    serial_ms(times->busy / FADE_FRAMES, tsc_per_ms);
    serial_write(" a frame on average, ");
    serial_ms(times->busiest, tsc_per_ms);
    serial_write(" at most)\r\n");
    serial_unlock(flags);
}

/* Show step k of the fade k / FADE_FRAMES of FADE_MS after it begins,
   or as soon as step k - 1 is done if that is later. */
static void fade(const SCREEN *screen, const RECT *box, PIXEL_PAGE *image,
                 PIXEL_PAGE *saved, TIMES *times)
{
    UINT64 began = timer_ticks(), start = rdtsc();
    INT64 weight;

    times->busy = 0;
    times->busiest = 0;
    for (weight = 1; weight <= FADE_FRAMES; weight++) {
        UINT64 frame, took;

        timer_sleep_until(began + weight * FADE_TICKS / FADE_FRAMES);
        frame = rdtsc();
        fade_frame(screen, box, image, saved, weight);
        gfx_flush();
        took = rdtsc() - frame;
        times->busy += took;
        if (took > times->busiest)
            times->busiest = took;
    }
    times->faded = rdtsc() - start;
}

void background_fade_in(const BOOT_INFO *info)
{
    const SCREEN *screen = &info->screen;
    LIMB limb;
    RECT box;
    PIXEL_PAGE *image, *saved;
    TIMES times;
    UINT64 start, flags;

    if (!limb_init(&limb, screen->width, screen->height)) {
        flags = serial_lock();
        serial_write("nova: no background for a screen this size\r\n");
        serial_unlock(flags);
        return;
    }
    gfx_boot_bounds(screen, &info->spinner, &box);
    if (!chain_alloc(screen->width * screen->height, &image) ||
        !chain_alloc((box.right - box.left) * (box.bottom - box.top),
                     &saved)) {
        chain_free(image);
        start = rdtsc();
        draw_onscreen(screen, &limb);
        gfx_flush();
        flags = serial_lock();
        serial_write("nova: background drawn in ");
        serial_ms(rdtsc() - start, info->tsc_per_ms);
        serial_write(", with no memory to fade it in\r\n");
        serial_unlock(flags);
        return;
    }
    start = rdtsc();
    draw_offscreen(screen, &limb, image);
    times.drawn = rdtsc() - start;
    save(screen, &box, saved);
    fade(screen, &box, image, saved, &times);
    chain_free(image);
    chain_free(saved);
    report(&times, info->tsc_per_ms);
}
