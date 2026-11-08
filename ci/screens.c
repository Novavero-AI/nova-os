/* The screens ci/boot.py expects the guest to show
   (docs/spec/display.md D19), each drawn on the host by the C that draws
   it in the guest, from the inputs that make it: the screen's size and
   what was typed and done. A screen is a pure function of them, so the
   check needs no image kept.

   Usage: screens SCREEN WIDTH HEIGHT [INPUT ...]

   Writes SCREEN at WIDTH x HEIGHT pixels as a binary PPM on standard
   output. The screens:

   console  Limb, as the kernel's fade leaves it, under the console's
            panel (console/panel.c) showing its view of the text
            (console/view.c). Each INPUT is two arguments: "line TEXT",
            TEXT typed at a read with echo and ended with Enter; "write
            TEXT", TEXT a program writes while no read waits; "wheel
            DETENTS", a pointer report that turns the wheel, up if
            positive; or "caret on" or "caret off", the phase of the
            caret's blink the screen shows, on if not given. The text
            starts with the first input, and once the inputs are done a
            read with echo waits at the output position. The guest's
            panel also shows what came before, which the screen leaves
            blank, so it matches only where its view holds none of it.

   cc -std=c11 -O2 -Wall -Wextra -pedantic -I include -I gfx -I console \
     -I lib -o screens ci/screens.c console/text.c console/view.c \
     console/panel.c gfx/gfx.c gfx/font.c gfx/raster.c gfx/limb.c */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "font.h"
#include "keymap.h"
#include "limb.h"
#include "panel.h"
#include "view.h"

#define EXIT_DRAWN 0
#define EXIT_NOT_DRAWN 1
#define EXIT_USAGE 2

#define PPM_MAXVAL 255
#define RGB_BYTES 3
#define CHANNEL_BITS 8
#define CHANNEL_MASK 0xFFu
#define DECIMAL 10
#define PRINTABLE_FIRST 0x20
#define PRINTABLE_LAST 0x7E

/* The arguments before the inputs, and those an input takes. */
#define ARGUMENT_SCREEN 1
#define ARGUMENT_WIDTH 2
#define ARGUMENT_HEIGHT 3
#define ARGUMENT_INPUTS 4
#define INPUT_ARGUMENTS 2

#define INPUT_LINE "line"
#define INPUT_WRITE "write"
#define INPUT_WHEEL "wheel"
#define INPUT_CARET "caret"
#define CARET_ON "on"
#define CARET_OFF "off"

/* A report's wheel, an evdev value (input_protocol.h). */
#define WHEEL_LARGEST 0x7fffffffL
#define WHEEL_SMALLEST (-WHEEL_LARGEST - 1)

static const char usage[] = "usage: screens SCREEN WIDTH HEIGHT [INPUT ...]\n";

/* Whether text is a whole decimal number from low to high, in *value. */
static int number(const char *text, long low, long high, long *value)
{
    char *end;

    errno = 0;
    *value = strtol(text, &end, DECIMAL);
    return end != text && *end == '\0' && errno == 0 && *value >= low &&
           *value <= high;
}

/* ------------------------------------------------------------------------
   Limb. */

/* Limb over the whole screen: the fade's last step, Limb alone
   (gfx/background.c). Returns 0 if Limb has no size for the screen. */
static int draw_limb(const SCREEN *screen)
{
    LIMB limb;
    INT64 x, y;

    if (!limb_init(&limb, screen->width, screen->height))
        return 0;
    for (y = 0; y < screen->height; y++)
        for (x = 0; x < screen->width; x++) {
            UINT32 rgb = limb_pixel(&limb, x, y);

            screen->pixels[y * screen->stride + x] =
                gfx_pixel(screen, rgb >> 2 * CHANNEL_BITS & CHANNEL_MASK,
                          rgb >> CHANNEL_BITS & CHANNEL_MASK,
                          rgb & CHANNEL_MASK);
        }
    return 1;
}

/* ------------------------------------------------------------------------
   The console. */

static TEXT text;
static EDITOR editor;
static INKS inks;
static PANEL_SHOWN shown;

/* Type line at a read with echo and end it, as the console takes the
   keys: a key that is not Page Up or Page Down brings the view back to
   the end (docs/spec/console.md C3). Returns 0 if a key would not type
   it: a byte that is not printable ASCII, or more than a line holds. */
static int type_line(const char *line, UINT64 *back)
{
    size_t length = strlen(line), i;

    if (length > CONSOLE_LINE_MAX)
        return 0;
    for (i = 0; i < length; i++)
        if (line[i] < PRINTABLE_FIRST || line[i] > PRINTABLE_LAST)
            return 0;
    editor_start(&editor, &text, CONSOLE_LINE_MAX, 1);
    for (i = 0; i < length; i++)
        editor_key(&editor, &text, EDIT_INSERT, (UINT8)line[i]);
    editor_key(&editor, &text, EDIT_ENTER, 0);
    *back = 0;
    return 1;
}

/* Take the inputs into the text, the view's place, back, and whether the
   caret is in the lit phase of its blink, lit. Returns 0, with the input
   that is not one said, if one is not. */
static int take_inputs(int count, char **inputs, UINT32 rows, UINT64 *back,
                       int *lit)
{
    long detents;
    int i;

    if (count % INPUT_ARGUMENTS) {
        fprintf(stderr, "screens: console: %s has no value\n",
                inputs[count - 1]);
        return 0;
    }
    for (i = 0; i < count; i += INPUT_ARGUMENTS) {
        const char *kind = inputs[i], *value = inputs[i + 1];

        if (strcmp(kind, INPUT_LINE) == 0 && type_line(value, back))
            continue;
        /* A write, as a key does, brings the view back to the end (C3). */
        if (strcmp(kind, INPUT_WRITE) == 0) {
            text_write(&text, (const UINT8 *)value, strlen(value));
            *back = 0;
            continue;
        }
        if (strcmp(kind, INPUT_CARET) == 0 &&
            (strcmp(value, CARET_ON) == 0 || strcmp(value, CARET_OFF) == 0)) {
            *lit = strcmp(value, CARET_ON) == 0;
            continue;
        }
        if (strcmp(kind, INPUT_WHEEL) == 0 &&
            number(value, WHEEL_SMALLEST, WHEEL_LARGEST, &detents)) {
            *back = view_scroll(&text, rows, *back,
                                (INT64)VIEW_WHEEL_LINES * detents);
            continue;
        }
        fprintf(stderr, "screens: console: not an input: %s %s\n", kind,
                value);
        return 0;
    }
    return 1;
}

static int console_screen(const SCREEN *screen, int count, char **inputs)
{
    LAYOUT layout;
    PANEL panel;
    VIEW_CARET caret;
    UINT32 *backdrop = 0;
    UINT8 *masks = 0;
    UINT64 back = 0, top;
    int lit = 1, drawn = 0;

    if (!panel_layout(&layout, screen->width, screen->height)) {
        fprintf(stderr, "screens: console: no panel on a screen this "
                        "size\n");
        return 0;
    }
    text_init(&text, layout.columns);
    if (!take_inputs(count, inputs, layout.rows, &back, &lit))
        return 0;
    editor_start(&editor, &text, CONSOLE_LINE_MAX, 1);
    if (!draw_limb(screen)) {
        fprintf(stderr, "screens: console: no Limb on a screen this size\n");
        return 0;
    }
    backdrop = malloc(panel_backdrop_size(&layout));
    masks = malloc(font_masks_size(layout.cell_width));
    if (backdrop && masks &&
        panel_backdrop(&layout, screen->width, screen->height, backdrop)) {
        font_render(layout.cell_width, masks);
        panel_inks(&inks);
        panel.screen = screen;
        panel.layout = &layout;
        panel.backdrop = backdrop;
        panel.masks = masks;
        panel.inks = &inks;
        panel_show(screen, &layout, backdrop, &shown);
        top = view_top(&text, layout.rows, back);
        caret = view_caret(lit ? &editor : 0, &text, layout.rows, top);
        panel_text(&panel, &shown, &text, top, &caret);
        drawn = 1;
    } else {
        fprintf(stderr, "screens: console: no memory for the panel\n");
    }
    free(masks);
    free(backdrop);
    return drawn;
}

/* ------------------------------------------------------------------------
   The screens and their output. */

typedef struct {
    const char *name;
    int (*draw)(const SCREEN *screen, int count, char **inputs);
} SCREEN_KIND;

static const SCREEN_KIND kinds[] = {
    {"console", console_screen},
};

#define KINDS (sizeof kinds / sizeof kinds[0])

/* Write screen, whose pixels read as 0x00RRGGBB, as a binary PPM
   (Netpbm's PPM format) on out. Returns 0 if a write fails. */
static int write_ppm(const SCREEN *screen, FILE *out)
{
    unsigned char *row = malloc((size_t)screen->width * RGB_BYTES);
    INT64 x, y;
    int written = row != 0 &&
                  fprintf(out, "P6\n%lld %lld\n%d\n", screen->width,
                          screen->height, PPM_MAXVAL) > 0;

    for (y = 0; written && y < screen->height; y++) {
        for (x = 0; x < screen->width; x++) {
            UINT32 p = screen->pixels[y * screen->stride + x];

            row[x * RGB_BYTES] = (unsigned char)(p >> 2 * CHANNEL_BITS);
            row[x * RGB_BYTES + 1] = (unsigned char)(p >> CHANNEL_BITS);
            row[x * RGB_BYTES + 2] = (unsigned char)p;
        }
        written = fwrite(row, RGB_BYTES, (size_t)screen->width, out) ==
                  (size_t)screen->width;
    }
    free(row);
    return written && fflush(out) == 0;
}

int main(int argc, char **argv)
{
    SCREEN screen;
    const SCREEN_KIND *kind = 0;
    long width, height;
    size_t i;
    int status = EXIT_NOT_DRAWN;

    if (argc < ARGUMENT_INPUTS ||
        !number(argv[ARGUMENT_WIDTH], LIMB_MIN_SIDE, LIMB_MAX_SIDE, &width) ||
        !number(argv[ARGUMENT_HEIGHT], LIMB_MIN_SIDE, LIMB_MAX_SIDE,
                &height)) {
        fputs(usage, stderr);
        return EXIT_USAGE;
    }
    for (i = 0; i < KINDS; i++)
        if (strcmp(argv[ARGUMENT_SCREEN], kinds[i].name) == 0)
            kind = &kinds[i];
    if (!kind) {
        fprintf(stderr, "screens: no screen named %s\n",
                argv[ARGUMENT_SCREEN]);
        return EXIT_USAGE;
    }
    screen.width = width;
    screen.height = height;
    screen.stride = width;
    screen.format = PIXEL_BGR;
    screen.pixels = malloc((size_t)width * (size_t)height * sizeof(UINT32));
    if (!screen.pixels) {
        fputs("screens: no memory for the screen\n", stderr);
    } else if (kind->draw(&screen, argc - ARGUMENT_INPUTS,
                          argv + ARGUMENT_INPUTS)) {
        if (write_ppm(&screen, stdout))
            status = EXIT_DRAWN;
        else
            fputs("screens: cannot write the screen\n", stderr);
    }
    free(screen.pixels);
    return status;
}
