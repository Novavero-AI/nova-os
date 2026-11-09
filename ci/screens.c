/* The screens ci/boot.py expects the guest to show
   (docs/spec/display.md D19), each drawn on the host by the C that draws
   it in the guest, from the inputs that make it: the screen's size and
   what was typed and done. A screen is a pure function of them, so the
   check needs no image kept.

   Usage: screens SCREEN WIDTH HEIGHT [INPUT ...]

   Writes SCREEN at WIDTH x HEIGHT pixels as a binary PPM on standard
   output. The screens:

   limb     Limb alone, as the kernel's fade leaves it, drawn by
            panel_limb, which is not the compositor's code. It takes no
            INPUT.

   console  Limb, as the kernel's fade leaves it and as the console
            draws it after a compositor (panel_limb), under the
            console's panel (console/panel.c) showing its view of the
            text (console/view.c). Each INPUT is two arguments: "line
            TEXT", TEXT typed at a read with echo and ended with Enter;
            "write TEXT", TEXT a program writes while no read waits; "wheel
            DETENTS", a pointer report that turns the wheel, up if
            positive; or "caret on" or "caret off", the phase of the
            caret's blink the screen shows, on if not given. The text
            starts with the first input, and once the inputs are done a
            read with echo waits at the output position. The guest's
            panel also shows what came before, which the screen leaves
            blank, so it matches only where its view holds none of it.

   console-window
            The console in a window on the compositor: Limb computed as
            the compositor computes its wallpaper, under the console's
            window, which holds the panel's pixels as the console draws
            them in a buffer (panel_view and panel_window_draw, which
            call the same function that makes a window's pixel as the
            host's render does), composed as the compositor composes a
            window of the PANEL surface (lib/window.c, lib/compose.c).
            The inputs are those of console, and then, if the
            pointer has a place, "pointer COLUMN ROW" as window
            takes it. As for console, the guest's panel also shows what
            came before, which the screen leaves blank. The window is
            placed by lib/window.c, and the screen fails to be drawn
            unless that is the place the panel stands in direct mode,
            which is where the window's size and place are the console's
            panel's box (docs/spec/display.md D6).

   window   Limb computed as the compositor computes its wallpaper, under
            the window the test client (test/wintest.h) shows when it has
            presented its second frame. It takes no INPUT, or one, "pointer
            COLUMN ROW": the pointer's image, the theme's arrow
            (look.md V15) at the screen's scale, drawn once over the
            whole screen composed, its hotspot on the top left corner of
            the pixel at COLUMN and ROW. The window is
            made, sized and placed by lib/window.c from the size the
            client asks for on a screen this size, filled with the
            client's pixels, and composed over the wallpaper by
            lib/compose.c, as compositor/serve.c does. The screen is
            therefore not an independent account of what a window looks
            like: that is tools/check-compose.c's and tools/check-window.c's
            to prove, against the specification. It checks that the
            compositor wires those functions to the guest as the
            specification says: that it reads a buffer by its stride and
            not its width, shows the cover the client sets, places the
            window and composes the damage of a second frame over the
            first, that the client's messages reach the compositor
            whole, and that the pointer is drawn over every band it
            touches, as it is over the finished screen. A mistake in
            lib/compose.c or lib/window.c shows on both sides alike and
            goes unseen here.

   cc -std=c11 -O2 -Wall -Wextra -pedantic -I include -I gfx -I console \
     -I lib -I test -o screens ci/screens.c console/text.c console/view.c \
     console/panel.c lib/compose.c lib/outline.c lib/window.c gfx/gfx.c \
     gfx/font.c gfx/raster.c gfx/limb.c */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "compose.h"
#include "display_protocol.h"
#include "font.h"
#include "keymap.h"
#include "limb.h"
#include "panel.h"
#include "view.h"
#include "window.h"
#include "wintest.h"

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
#define INPUT_POINTER_AT "pointer"
#define POINTER_ARGUMENTS 3
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

/* The panel's layout on screen, and the text the inputs make with a read
   with echo waiting at its end, in *layout, the global text and editor,
   *back and *lit. Returns 0, with the reason said, if there is no panel
   on a screen this size or an input is not one. */
static int console_text(const SCREEN *screen, int count, char **inputs,
                        LAYOUT *layout, UINT64 *back, int *lit)
{
    *back = 0;
    *lit = 1;
    if (!panel_layout(layout, screen->width, screen->height)) {
        fprintf(stderr, "screens: console: no panel on a screen this "
                        "size\n");
        return 0;
    }
    text_init(&text, layout->columns);
    if (!take_inputs(count, inputs, layout->rows, back, lit))
        return 0;
    editor_start(&editor, &text, CONSOLE_LINE_MAX, 1);
    return 1;
}

static int console_screen(const SCREEN *screen, int count, char **inputs)
{
    LAYOUT layout;
    PANEL panel;
    VIEW_CARET caret;
    UINT32 *backdrop = 0;
    UINT8 *masks = 0;
    UINT64 back, top;
    int lit, drawn = 0;

    if (!console_text(screen, count, inputs, &layout, &back, &lit))
        return 0;
    if (!panel_limb(screen)) {
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

/* Limb alone. */
static int limb_screen(const SCREEN *screen, int count, char **inputs)
{
    (void)inputs;
    if (count != 0) {
        fprintf(stderr, "screens: limb: takes no input\n");
        return 0;
    }
    if (!panel_limb(screen)) {
        fprintf(stderr, "screens: limb: no Limb on a screen this size\n");
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------------
   A window of a client, composed over Limb. */

/* A client's window: the size it asks for on the screen, and the pixels it
   fills the window the compositor made for it with, width x height, one to
   a word with no extra at the end of a row, from content. Returns 0, with
   the reason said, if it cannot. */
typedef struct {
    UINT32 width, height;
    const void *content;
    UINT32 *(*pixels)(const void *content, const WINDOW_RECORD *record);
} WINDOW_SOURCE;

/* Limb for a screen as the compositor computes it, one pixel to a word, in
   the screen's own format; 0 if Limb does not take the size. */
static UINT32 *limb_wallpaper(const SCREEN *screen)
{
    UINT32 *pixels = malloc((size_t)screen->width * (size_t)screen->height *
                            sizeof(UINT32));
    LIMB limb;
    INT64 x, y;

    if (!pixels)
        return 0;
    if (!limb_init(&limb, screen->width, screen->height)) {
        free(pixels);
        return 0;
    }
    for (y = 0; y < screen->height; y++)
        for (x = 0; x < screen->width; x++)
            pixels[y * screen->width + x] = limb_pixel(&limb, x, y);
    return pixels;
}

/* The pixels of a window of width x height; 0, said, if there is no
   memory. */
static UINT32 *window_memory(UINT32 width, UINT32 height)
{
    UINT32 *pixels = calloc((size_t)width * (size_t)height, sizeof(UINT32));

    if (!pixels)
        fprintf(stderr, "screens: no memory for the window\n");
    return pixels;
}

/* The theme's pointer, as a default (look.md V15). */
static const INT32 pointer_body_points[] = THEME_POINTER_BODY_DEFAULT_POINTS;
static const UINT8 pointer_body_flags[] = THEME_POINTER_BODY_DEFAULT_FLAGS;
static const UINT32 pointer_body_ends[] = THEME_POINTER_BODY_DEFAULT_ENDS;
static const INT32 pointer_edge_points[] = THEME_POINTER_EDGE_DEFAULT_POINTS;
static const UINT8 pointer_edge_flags[] = THEME_POINTER_EDGE_DEFAULT_FLAGS;
static const UINT32 pointer_edge_ends[] = THEME_POINTER_EDGE_DEFAULT_ENDS;

/* Draw the pointer's image with its hotspot on the top left corner of the
   pixel (column, row) over the whole screen, as one band of its own
   rectangle: the screen's words are 0x00RRGGBB, which PIXEL_BGR gives.
   Returns 0 if the image cannot be drawn. */
static int draw_pointer(const SCREEN *screen, OUTLINE *work, long column,
                        long row)
{
    COMPOSE_POINTER image;
    COMPOSE_BAND band;

    image.body.points = pointer_body_points;
    image.body.flags = pointer_body_flags;
    image.body.ends = pointer_body_ends;
    image.body.point_count = THEME_POINTER_BODY_DEFAULT_POINT_COUNT;
    image.body.contours = THEME_POINTER_BODY_DEFAULT_CONTOURS;
    image.edge.points = pointer_edge_points;
    image.edge.flags = pointer_edge_flags;
    image.edge.ends = pointer_edge_ends;
    image.edge.point_count = THEME_POINTER_EDGE_DEFAULT_POINT_COUNT;
    image.edge.contours = THEME_POINTER_EDGE_DEFAULT_CONTOURS;
    image.body_ink = THEME_POINTER_BODY_INK_DEFAULT;
    image.edge_ink = THEME_POINTER_EDGE_INK_DEFAULT;
    image.edge_percent = THEME_POINTER_EDGE_OPACITY_DEFAULT;
    band.pixels = screen->pixels;
    band.stride = screen->stride;
    band.rect.left = band.rect.top = 0;
    band.rect.right = screen->width;
    band.rect.bottom = screen->height;
    return compose_pointer(work, &image, &band, column, row,
                           window_scale((UINT32)screen->height));
}

/* Compose the client's window over the wallpaper, the whole screen, with
   the panel material and corners the compositor gives a window of the PANEL
   surface, and the pointer over it where pointer, if it is not 0, gives the
   pixel of its hotspot, a column and a row. Returns 0 if the window cannot
   be made, the pointer drawn or there is no memory. */
static int compose_window(const SCREEN *screen, const UINT32 *wallpaper,
                          const WINDOW_SOURCE *source, const long *pointer)
{
    static COMPOSE_MATERIAL material;
    static COMPOSE_CORNERS corners;
    COMPOSE_WALLPAPER under;
    COMPOSE_WINDOW placed;
    DISPLAY_WINDOW_REQUEST request;
    WINDOW_AREA area;
    WINDOW_RECORD record;
    RECT whole;
    OUTLINE *work = malloc(sizeof *work);
    UINT32 *memory = malloc((size_t)screen->width * TILE_ROWS *
                            sizeof(UINT32));
    UINT32 *pixels = 0;
    INT64 radius = font_panel_radius(font_cell_width(screen->height));
    int composed = 0;

    area.screen_width = (UINT32)screen->width;
    area.screen_height = (UINT32)screen->height;
    area.strip = 0;
    memset(&request, 0, sizeof request);
    request.width = source->width;
    request.height = source->height;
    request.layer = DISPLAY_LAYER_SYSTEM;
    request.surface = DISPLAY_SURFACE_PANEL;
    if (!work || !memory ||
        !compose_material(&material, THEME_MATERIAL_PANEL_DEFAULT_TINT,
                          THEME_MATERIAL_PANEL_DEFAULT_AMOUNT) ||
        !compose_corners(work, &corners, radius * OUTLINE_ONE) ||
        window_open(&area, (UINT32)corners.reach, DISPLAY_LAYER_SYSTEM, 0,
                    &request, &record) != OK) {
        fprintf(stderr, "screens: window: no window on a screen this size\n");
    } else if ((pixels = source->pixels(source->content, &record))) {
        under.pixels = wallpaper;
        under.stride = screen->width;
        under.format = screen->format;
        placed.pixels = pixels;
        placed.stride = record.width;
        placed.columns = record.width;
        placed.rows = record.height;
        placed.place.left = record.x;
        placed.place.top = record.y;
        placed.place.right = (INT64)record.x + record.width;
        placed.place.bottom = (INT64)record.y + record.height;
        placed.material = &material;
        placed.corners = &corners;
        whole.left = whole.top = 0;
        whole.right = screen->width;
        whole.bottom = screen->height;
        compose_frame(screen, &under, &placed, 1, &whole, memory, TILE_ROWS, 0);
        composed = !pointer ||
                   draw_pointer(screen, work, pointer[0], pointer[1]);
        if (!composed)
            fprintf(stderr, "screens: window: the pointer cannot be drawn\n");
    }
    free(pixels);
    free(memory);
    free(work);
    return composed;
}

/* The pointer's place from inputs, "pointer COLUMN ROW" with a pixel of the
   screen, into at[] as column and row. Returns 0, the reason said, if it
   is not one. */
static int pointer_input(const SCREEN *screen, char **inputs, long *at)
{
    if (strcmp(inputs[0], INPUT_POINTER_AT) == 0 &&
        number(inputs[1], 0, (long)(screen->width - 1), &at[0]) &&
        number(inputs[2], 0, (long)(screen->height - 1), &at[1]))
        return 1;
    fprintf(stderr, "screens: \"%s COLUMN ROW\" within the screen is the "
                    "pointer's place\n", INPUT_POINTER_AT);
    return 0;
}

/* The frame whose pixels the screen shows: the client's second, which is
   the last it presents. */
#define WINDOW_LAST_FRAME WINTEST_FRAMES

/* The client's pixels (test/wintest.h), WINDOW_LAST_FRAME's. */
static UINT32 *wintest_pixels(const void *content,
                              const WINDOW_RECORD *record)
{
    UINT32 *pixels = window_memory(record->width, record->height);
    UINT32 x, y;

    (void)content;
    if (!pixels)
        return 0;
    for (y = 0; y < record->height; y++)
        for (x = 0; x < record->width; x++)
            pixels[(size_t)y * record->width + x] = wintest_pixel(
                record->width, record->height, x, y, WINDOW_LAST_FRAME);
    return pixels;
}

/* Compose source's window over Limb on screen, with the pointer's hotspot
   at the column and row of pointer unless it is 0. Returns 0, with the
   reason said, if it cannot be drawn. */
static int compose_over_limb(const SCREEN *screen, const WINDOW_SOURCE *source,
                             const long *pointer)
{
    UINT32 *wallpaper = limb_wallpaper(screen);
    int drawn;

    if (!wallpaper) {
        fprintf(stderr, "screens: no Limb on a screen this size\n");
        return 0;
    }
    drawn = compose_window(screen, wallpaper, source, pointer);
    free(wallpaper);
    return drawn;
}

static int window_screen(const SCREEN *screen, int count, char **inputs)
{
    WINDOW_SOURCE source;
    long at[2];

    if (count != 0 &&
        !(count == POINTER_ARGUMENTS && pointer_input(screen, inputs, at))) {
        fprintf(stderr, "screens: window: takes no input, or the pointer's "
                        "place\n");
        return 0;
    }
    wintest_want((UINT32)screen->width, (UINT32)screen->height, &source.width,
                 &source.height);
    source.content = 0;
    source.pixels = wintest_pixels;
    return compose_over_limb(screen, &source, count ? at : 0);
}

/* ------------------------------------------------------------------------
   The console's window. */

/* What the console draws in its window: the panel's layout, the font's
   masks at its cell width, and the cells the text's view shows. */
typedef struct {
    const LAYOUT *layout;
    const UINT8 *masks;
    const PANEL_SHOWN *want;
} PANEL_CONTENT;

/* The console's pixels as it draws a first frame of a buffer: panel_view's
   cells through panel_window_draw. Fails, said, if the compositor placed
   or sized the window other than as the panel's box. */
static UINT32 *panel_pixels(const void *content, const WINDOW_RECORD *record)
{
    const PANEL_CONTENT *panel = content;
    const LAYOUT *layout = panel->layout;
    static PANEL_SHOWN window_shown;
    UINT32 *pixels;

    if (record->x != layout->box.left || record->y != layout->box.top ||
        (INT64)record->width != layout->box.right - layout->box.left ||
        (INT64)record->height != layout->box.bottom - layout->box.top) {
        fprintf(stderr, "screens: console-window: the window is not where "
                        "and as large as the panel stands in direct mode\n");
        return 0;
    }
    pixels = window_memory(record->width, record->height);
    if (!pixels)
        return 0;
    panel_window_forget(layout, &window_shown);
    panel_window_draw(layout, panel->masks, pixels, record->width, &window_shown,
                      panel->want);
    return pixels;
}

static int console_window_screen(const SCREEN *screen, int count,
                                 char **inputs)
{
    static PANEL_SHOWN want;
    LAYOUT layout;
    PANEL_CONTENT content;
    WINDOW_SOURCE source;
    VIEW_CARET caret;
    UINT64 back, top;
    UINT8 *masks;
    long at[2];
    int lit, pointer = 0, drawn;

    if (count >= POINTER_ARGUMENTS &&
        (count - POINTER_ARGUMENTS) % INPUT_ARGUMENTS == 0 &&
        strcmp(inputs[count - POINTER_ARGUMENTS], INPUT_POINTER_AT) == 0) {
        if (!pointer_input(screen, inputs + count - POINTER_ARGUMENTS, at))
            return 0;
        pointer = 1;
        count -= POINTER_ARGUMENTS;
    }
    if (!console_text(screen, count, inputs, &layout, &back, &lit))
        return 0;
    masks = malloc(font_masks_size(layout.cell_width));
    if (!masks) {
        fprintf(stderr, "screens: console-window: no memory for the "
                        "masks\n");
        return 0;
    }
    font_render(layout.cell_width, masks);
    top = view_top(&text, layout.rows, back);
    caret = view_caret(lit ? &editor : 0, &text, layout.rows, top);
    panel_view(&layout, &text, top, &caret, &want);
    content.layout = &layout;
    content.masks = masks;
    content.want = &want;
    source.width = (UINT32)(layout.box.right - layout.box.left);
    source.height = (UINT32)(layout.box.bottom - layout.box.top);
    source.content = &content;
    source.pixels = panel_pixels;
    drawn = compose_over_limb(screen, &source, pointer ? at : 0);
    free(masks);
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
    {"console-window", console_window_screen},
    {"limb", limb_screen},
    {"window", window_screen},
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
