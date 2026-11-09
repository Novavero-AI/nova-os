/* Checks the console's panel backdrop from the kernel's background on
   the host (docs/spec/console.md C5). At each size, for each pixel
   format, Limb's pixels encoded as the kernel encodes its image's, with
   gfx_pixel, row after row with no gap (native_abi.h), give through
   panel_backdrop_image the backdrop panel_backdrop computes, which
   ci/screens.c draws the expected screens with, whatever a pixel's
   unused byte holds. The image's bands are not built here: the system
   disk's boots map them. A background is refused unless it is Limb, in
   a format the panel reads, at the display's size, with a side Limb
   takes and a layout for it, and each refusal names its cause. Limb
   drawn over a whole screen by panel_limb, as the console draws it after
   a compositor, is the kernel's image pixel for pixel, in both formats,
   on rows longer than the screen is wide, and writes nothing past a row
   and nothing on a screen Limb has no size for. Prints each failure and
   a count, and exits nonzero if any check fails.

   cc -std=c11 -Wall -Wextra -pedantic -I include -I gfx -I console \
     -I lib -o check-backdrop tools/check-backdrop.c console/panel.c \
     console/text.c gfx/gfx.c gfx/font.c gfx/raster.c gfx/limb.c \
     && ./check-backdrop */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "limb.h"
#include "panel.h"

#define CHANNEL_BITS 8
#define CHANNEL_MASK 0xFFu
#define UNUSED_BYTE 0xA5000000u
#define COLOUR_MASK 0x00FFFFFFu
#define SCREEN_BEFORE 0x5A5A5A5Au
#define ROW_GAP 5
#define FAILURES_SHOWN 10

/* The sizes checked: the two CI boots and the README name, a common
   laptop's, which no cell divides evenly, and the smallest a 4:3 screen
   commonly has. */
static const struct {
    INT64 width, height;
} sizes[] = {{1280, 800}, {2560, 1600}, {2880, 1800}, {1366, 768}, {640, 480}};
#define SIZES (sizeof sizes / sizeof sizes[0])

static const UINT32 formats[] = {PIXEL_RGB, PIXEL_BGR};
#define FORMATS (sizeof formats / sizeof formats[0])

static unsigned failures;

static void check(int passed, const char *what, INT64 width, INT64 height)
{
    if (!passed && failures++ < FAILURES_SHOWN)
        printf("failed: %s at %lldx%lld\n", what, (long long)width,
               (long long)height);
}

/* Limb on a width x height screen in format, as the kernel's image
   holds it, each pixel's unused byte set; 0 if memory runs out. */
static UINT32 *kernel_image(INT64 width, INT64 height, UINT32 format)
{
    SCREEN screen = {0, width, height, width, format};
    UINT32 *image = malloc((size_t)(width * height) * sizeof *image);
    LIMB limb;
    INT64 x, y;

    if (!image || !limb_init(&limb, width, height)) {
        free(image);
        return 0;
    }
    for (y = 0; y < height; y++)
        for (x = 0; x < width; x++) {
            UINT32 rgb = limb_pixel(&limb, x, y);

            image[y * width + x] =
                gfx_pixel(&screen, rgb >> 2 * CHANNEL_BITS & CHANNEL_MASK,
                          rgb >> CHANNEL_BITS & CHANNEL_MASK,
                          rgb & CHANNEL_MASK) |
                UNUSED_BYTE;
        }
    return image;
}

static void check_backdrops(INT64 width, INT64 height)
{
    LAYOUT layout;
    UINT32 *computed, *read;
    size_t size, i;

    if (!panel_layout(&layout, width, height)) {
        check(0, "a panel lays out", width, height);
        return;
    }
    size = panel_backdrop_size(&layout);
    computed = malloc(size);
    read = malloc(size);
    if (!computed || !read ||
        !panel_backdrop(&layout, width, height, computed)) {
        check(0, "the backdrop is computed", width, height);
        free(computed);
        free(read);
        return;
    }
    for (i = 0; i < FORMATS; i++) {
        UINT32 *image = kernel_image(width, height, formats[i]);

        if (!image) {
            check(0, "the kernel's image is drawn", width, height);
            continue;
        }
        panel_backdrop_image(&layout, image, width, formats[i], read);
        check(memcmp(computed, read, size) == 0,
              formats[i] == PIXEL_RGB
                  ? "the backdrop from an RGB image is the computed one"
                  : "the backdrop from a BGR image is the computed one",
              width, height);
        free(image);
    }
    free(computed);
    free(read);
}

/* panel_limb on a screen whose rows are ROW_GAP pixels longer than it is
   wide, in each format, against the kernel's image: every pixel of the
   screen is the image's, and every pixel in a row's gap is as it was. */
static void check_limb_screen(INT64 width, INT64 height)
{
    size_t i;

    for (i = 0; i < FORMATS; i++) {
        INT64 stride = width + ROW_GAP, x, y, wrong = 0, touched = 0;
        UINT32 *pixels = malloc((size_t)(stride * height) * sizeof *pixels);
        UINT32 *image = kernel_image(width, height, formats[i]);
        SCREEN screen = {pixels, width, height, stride, formats[i]};

        if (!pixels || !image) {
            check(0, "Limb over the screen is drawn", width, height);
            free(pixels);
            free(image);
            continue;
        }
        for (y = 0; y < stride * height; y++)
            pixels[y] = SCREEN_BEFORE;
        check(panel_limb(&screen) == 1, "Limb over the screen is drawn",
              width, height);
        for (y = 0; y < height; y++) {
            for (x = 0; x < width; x++)
                wrong += pixels[y * stride + x] !=
                         (image[y * width + x] & COLOUR_MASK);
            for (x = width; x < stride; x++)
                touched += pixels[y * stride + x] != SCREEN_BEFORE;
        }
        check(wrong == 0,
              formats[i] == PIXEL_RGB
                  ? "Limb over an RGB screen is the kernel's image"
                  : "Limb over a BGR screen is the kernel's image",
              width, height);
        check(touched == 0, "Limb over the screen leaves a row's gap alone",
              width, height);
        free(pixels);
        free(image);
    }
}

/* A screen Limb has no size for is refused and left as it was. */
static void check_limb_screen_refusal(void)
{
    const INT64 side = LIMB_MIN_SIDE - 1;
    UINT32 pixels[LIMB_MIN_SIDE * LIMB_MIN_SIDE];
    SCREEN screen = {pixels, side, side, LIMB_MIN_SIDE, PIXEL_BGR};
    size_t i, touched = 0;

    for (i = 0; i < LIMB_MIN_SIDE * LIMB_MIN_SIDE; i++)
        pixels[i] = SCREEN_BEFORE;
    check(panel_limb(&screen) == 0, "a screen Limb has no size for is refused",
          side, side);
    for (i = 0; i < LIMB_MIN_SIDE * LIMB_MIN_SIDE; i++)
        touched += pixels[i] != SCREEN_BEFORE;
    check(touched == 0, "a screen Limb has no size for is left as it was",
          side, side);
}

/* Whether background is refused on display for reason, or taken where
   reason is 0. */
static int refused_for(UINT32 background, UINT32 width, UINT32 height,
                       UINT32 format, UINT32 display_width,
                       UINT32 display_height, const char *reason)
{
    BOOTSTRAP_BACKGROUND label = {background, width, height, format};
    BOOTSTRAP_DISPLAY display = {0, display_width, display_height,
                                 display_width, PIXEL_BGR};
    const char *refusal = panel_background_refusal(&label, &display);

    if (!reason)
        return refusal == 0;
    return refusal && strcmp(refusal, reason) == 0;
}

static void check_refusals(void)
{
    const UINT32 w = 1280, h = 800;
    const UINT32 small = LIMB_MIN_SIDE - 1, tiny = LIMB_MIN_SIDE;
    const UINT32 large = LIMB_MAX_SIDE + 1;

    check(refused_for(BOOTSTRAP_BACKGROUND_LIMB, w, h, PIXEL_BGR, w, h, 0),
          "Limb at the display's size is taken", w, h);
    check(refused_for(BOOTSTRAP_BACKGROUND_LIMB, w, h, PIXEL_RGB, w, h, 0),
          "Limb in the other format is taken", w, h);
    check(refused_for(BOOTSTRAP_BACKGROUND_NONE, 0, 0, 0, w, h, "no image"),
          "no image is refused", w, h);
    check(refused_for(BOOTSTRAP_BACKGROUND_LIMB + 1, w, h, PIXEL_BGR, w, h,
                      "not limb"),
          "another background is refused", w, h);
    check(refused_for(BOOTSTRAP_BACKGROUND_LIMB, w, h, PIXEL_BGR + 1, w, h,
                      "unknown format"),
          "an unknown format is refused", w, h);
    check(refused_for(BOOTSTRAP_BACKGROUND_LIMB, w, h, PIXEL_BGR, w + 1, h,
                      "another size"),
          "a display a column wider is refused", w, h);
    check(refused_for(BOOTSTRAP_BACKGROUND_LIMB, w, h, PIXEL_BGR, w, h + 1,
                      "another size"),
          "a display a row taller is refused", w, h);
    check(refused_for(BOOTSTRAP_BACKGROUND_LIMB, small, small, PIXEL_BGR,
                      small, small, "no size"),
          "a side Limb does not take is refused", small, small);
    check(refused_for(BOOTSTRAP_BACKGROUND_LIMB, large, h, PIXEL_BGR,
                      large, h, "no size"),
          "a side past Limb's largest is refused", large, h);
    check(refused_for(BOOTSTRAP_BACKGROUND_LIMB, tiny, tiny, PIXEL_BGR, tiny,
                      tiny, "no room for a cell"),
          "a screen no cell fits is refused", tiny, tiny);
}

int main(void)
{
    size_t i;

    for (i = 0; i < SIZES; i++) {
        check_backdrops(sizes[i].width, sizes[i].height);
        check_limb_screen(sizes[i].width, sizes[i].height);
    }
    check_refusals();
    check_limb_screen_refusal();
    printf("%u failures\n", failures);
    return failures != 0;
}
