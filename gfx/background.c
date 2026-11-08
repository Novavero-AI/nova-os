/* The desktop background's arrival. Limb is drawn once into memory
   sections while the boot screen's spinner still turns, since the
   drawing takes seconds under emulation on a large screen and a spinner
   standing still for that long looks like a hang; the kernel then hands
   the sections on to the first process where a message carries them all
   (first_process.c), and keeps its own handles to them for the fade.
   Once the spinner has stopped, the part of the screen the boot screen
   drew on is saved; then each frame mixes the two images in linear
   light, as the boot screen's anti-aliasing mixes ink with black, by a
   step of the fade's schedule (fade.h). A frame shows the step due when
   it will be done, judged by the longest frame so far: where a frame and
   a timer tick fit in a step, every step is shown, and where they do
   not, steps are skipped to keep to the fade's length. The last step,
   Limb alone, is always drawn. Judged by the longest, a frame runs past
   its step only when it is the slowest yet, and after a slow frame the
   rest of the fade comes early by up to as much. */

#include "background.h"
#include "fade.h"
#include "limb.h"
#include "memory_layout.h"
#include "sched/wait.h"
#include "x86_64/clock.h"
#include "x86_64/serial.h"

/* A pixel holds three 8-bit channels from bit 0 up, in the screen's
   order, which both images share; the fade treats each alike. */
#define CHANNELS 3
#define CHANNEL_BITS 8
#define CHANNEL_MASK 0xFFu
#define COLOUR_MASK 0xFFFFFFu
#define CODES 256

/* ------------------------------------------------------------------------
   Offscreen images. An image is bands of memory sections, every one
   SECTION_MAX_SIZE bytes but the last, whose pages are filled and read
   in order, row after row with no gap between rows: the layout the
   first process maps them in (native_abi.h). */

#define PAGE_PIXELS ((INT64)(PAGE_SIZE / sizeof(UINT32)))
#define BAND_PAGES (SECTION_MAX_SIZE / PAGE_SIZE)
#define BAND_PIXELS (SECTION_MAX_SIZE / sizeof(UINT32))

static void bands_close(BANDS *image)
{
    UINT32 i;

    for (i = 0; i < image->count; i++)
        object_close(&image->bands[i]->object);
    image->count = 0;
    image->pages = 0;
}

/* Take bands for count pixels, no more than a screen limb_init takes
   holds, into *image. Returns 0, with every band it took closed, if the
   allocator runs out. */
static int bands_alloc(UINT64 count, BANDS *image)
{
    UINT64 at;

    image->count = 0;
    image->pages = 0;
    for (at = 0; at < count; at += BAND_PIXELS) {
        UINT64 pixels = count - at < BAND_PIXELS ? count - at : BAND_PIXELS;

        if (section_create(pixels * sizeof(UINT32),
                           &image->bands[image->count]) != OK) {
            bands_close(image);
            return 0;
        }
        image->count++;
    }
    image->pages = (count * sizeof(UINT32) + PAGE_SIZE - 1) / PAGE_SIZE;
    return 1;
}

/* Page page of image, through the direct map, or 0 past its last. */
static UINT32 *band_page(const BANDS *image, UINT64 page)
{
    if (page >= image->pages)
        return 0;
    return direct_map(
        section_page(image->bands[page / BAND_PAGES], page % BAND_PAGES));
}

/* A place in an image: the next pixel is pixels[index], on the image's
   page page. */
typedef struct {
    const BANDS *image;
    UINT64 page;
    UINT32 *pixels;
    INT64 index;
} CURSOR;

static void cursor_start(CURSOR *c, const BANDS *image)
{
    c->image = image;
    c->page = 0;
    c->pixels = band_page(image, 0);
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
    return c->pixels + c->index;
}

/* Move c past count pixels, no more than cursor_run gave. */
static void cursor_advance(CURSOR *c, INT64 count)
{
    c->index += count;
    if (c->index == PAGE_PIXELS) {
        c->page++;
        c->pixels = band_page(c->image, c->page);
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

/* Draw Limb into image, bands the size of the screen. */
static void draw_offscreen(const SCREEN *screen, const LIMB *limb,
                           const BANDS *image)
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

/* Copy the screen's pixels in box into saved, bands the size of box. */
static void save(const SCREEN *screen, const RECT *box, const BANDS *saved)
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

/* One frame's mix, weight of FADE_WEIGHT toward Limb. Where the boot
   screen is black, a channel's mix depends on Limb's code alone, so
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
            (UINT32)gfx_cross_fade(0, code, weight, FADE_WEIGHT);
}

static UINT32 mix_pixels(const MIX *mix, UINT32 from, UINT32 to)
{
    UINT32 out = 0;
    int shift;

    for (shift = 0; shift < CHANNELS * CHANNEL_BITS; shift += CHANNEL_BITS)
        out |= (UINT32)gfx_cross_fade(from >> shift & CHANNEL_MASK,
                                      to >> shift & CHANNEL_MASK,
                                      mix->weight, FADE_WEIGHT)
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
                       const BANDS *image, const BANDS *saved, INT64 weight)
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
   Timing and the log. Times come from the kernel's clock (clock.h), which
   reads the timestamp counter, and the fade's deadlines are times on it.
   A sleep still ends on a timer tick, but the ticks fall behind under
   QEMU's TCG while the processor is busy, and a schedule counted in them
   would fall behind with them. */

/* How long the fade took and its frames' drawing, in nanoseconds, and
   the frames drawn. */
typedef struct {
    UINT64 faded, busy, busiest, frames;
} TIMES;

static void report(const PREPARATION *preparation, const TIMES *times)
{
    UINT64 flags = serial_lock();

    serial_write("nova: background draw_ms=");
    serial_ms(preparation->drawn);
    serial_write(" prepare_ms=");
    serial_ms(preparation->prepared);
    serial_write(" spinner_frames=");
    serial_number(preparation->spinner_frames);
    serial_write(" fade_frames=");
    serial_number(times->frames);
    serial_write(" fade_ms=");
    serial_ms(times->faded);
    serial_write(" frame_avg_ms=");
    serial_ms(times->busy / times->frames);
    serial_write(" frame_max_ms=");
    serial_ms(times->busiest);
    serial_write("\r\n");
    serial_unlock(flags);
}

/* Draw frames until one shows the last step, each started and given its
   step by the schedule. The first, with no frame before it, is judged to
   take no time. */
static void fade(const SCREEN *screen, const RECT *box, const BANDS *image,
                 const BANDS *saved, TIMES *times)
{
    FADE_SCHEDULE schedule;
    UINT64 began = clock_monotonic();

    fade_begin(&schedule);
    times->busy = 0;
    times->frames = 0;
    while (schedule.step < FADE_STEPS) {
        UINT64 frame, took;

        sleep_until(began + fade_next(&schedule));
        frame = clock_monotonic();
        fade_frame(screen, box, image, saved,
                   fade_weight(fade_show(&schedule, frame - began)));
        gfx_flush();
        took = clock_monotonic() - frame;
        fade_took(&schedule, took);
        times->busy += took;
        times->frames++;
    }
    times->busiest = schedule.longest;
    times->faded = clock_monotonic() - began;
}

/* ------------------------------------------------------------------------
   Preparing and showing. */

void background_prepare(const BOOT_INFO *info, BACKGROUND *background)
{
    const SCREEN *screen = &info->screen;
    const RECT *box = &background->box;

    background->image.count = 0;
    background->saved.count = 0;
    if (!limb_init(&background->limb, screen->width, screen->height)) {
        background->state = BACKGROUND_NO_SIZE;
        return;
    }
    /* The spinner's thread changes only its head, which the bounds do not
       depend on. */
    gfx_boot_bounds(screen, &info->spinner, &background->box);
    if (!bands_alloc((UINT64)(screen->width * screen->height),
                     &background->image) ||
        !bands_alloc((UINT64)((box->right - box->left) *
                              (box->bottom - box->top)),
                     &background->saved)) {
        bands_close(&background->image);
        background->state = BACKGROUND_NO_MEMORY;
        return;
    }
    draw_offscreen(screen, &background->limb, &background->image);
    background->state = BACKGROUND_READY;
}

UINT32 background_label(const BOOT_INFO *info, const BACKGROUND *background,
                        BOOTSTRAP_BACKGROUND *label)
{
    const SCREEN *screen = &info->screen;

    if (background->state != BACKGROUND_READY ||
        background->image.count > BOOTSTRAP_BACKGROUND_SECTIONS_MAX) {
        label->background = BOOTSTRAP_BACKGROUND_NONE;
        label->width = 0;
        label->height = 0;
        label->format = 0;
        return 0;
    }
    label->background = BOOTSTRAP_BACKGROUND_LIMB;
    label->width = (UINT32)screen->width;
    label->height = (UINT32)screen->height;
    label->format = screen->format;
    return background->image.count;
}

void background_discard(BACKGROUND *background)
{
    bands_close(&background->image);
    bands_close(&background->saved);
}

static void fade_in(const SCREEN *screen, BACKGROUND *background,
                    const PREPARATION *preparation)
{
    TIMES times;

    save(screen, &background->box, &background->saved);
    fade(screen, &background->box, &background->image, &background->saved,
         &times);
    background_discard(background);
    report(preparation, &times);
}

static void draw_unfaded(const SCREEN *screen, const LIMB *limb)
{
    UINT64 start = clock_monotonic(), flags;

    draw_onscreen(screen, limb);
    gfx_flush();
    flags = serial_lock();
    serial_write("nova: background unfaded reason=\"no memory\" draw_ms=");
    serial_ms(clock_monotonic() - start);
    serial_write("\r\n");
    serial_unlock(flags);
}

void background_show(const BOOT_INFO *info, BACKGROUND *background,
                     const PREPARATION *preparation)
{
    UINT64 flags;

    switch (background->state) {
    case BACKGROUND_READY:
        fade_in(&info->screen, background, preparation);
        break;
    case BACKGROUND_NO_MEMORY:
        draw_unfaded(&info->screen, &background->limb);
        break;
    case BACKGROUND_NO_SIZE:
        flags = serial_lock();
        serial_write("nova: background absent reason=\"no size\"\r\n");
        serial_unlock(flags);
        break;
    }
}
