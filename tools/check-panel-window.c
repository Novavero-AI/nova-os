/* Checks the console's panel in a window on the host (console/panel.c,
   docs/spec/console.md C2, Window mode). The pixel of a window with a
   glyph's coverage and the caret's at it is checked for every pair of
   coverages against the spec's formulas written out, the cover table
   literally and the overlap's colour, the caret's ink mixed over the
   glyph's by the caret's cover over the pixel's, in linear light from
   the sRGB formulas in floating point, to within a code of what
   gfx/raster.c's integer tables give. The cells view.h puts in view are
   checked against the text's lines. The drawing is checked against a
   full render made one cell at a time from the layout: over generated
   sequences of frames, drawn in two buffers by turns as the console
   draws them, each buffer after each draw is the render of what it was
   asked to show and nothing past its rows is written, the damage of a
   frame is the smallest rectangle that holds the cells that differ from
   the frame the compositor last had, and every pixel that differs from
   that frame lies in it. The buffer a frame is drawn in is the first the
   compositor does not hold, and a model of the exchange of presents,
   configures, frames and releases that display.md D6 and D7 give, run
   over generated schedules and over the one where a configure crosses a
   present the compositor then ignores, shows that the console is never
   left with no buffer to draw in once the messages in flight are taken,
   and never draws in one the compositor reads. Prints each failure and a
   count, and exits nonzero if any check fails.

   cc -std=c11 -Wall -Wextra -pedantic -I include -I gfx -I console \
     -I lib -o check-panel-window tools/check-panel-window.c \
     console/panel.c console/text.c gfx/gfx.c gfx/font.c gfx/raster.c \
     gfx/limb.c -lm && ./check-panel-window */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "font.h"
#include "panel.h"

#define COVERAGES (COVERAGE_MAX + 1)
#define COVER_MAX 255
#define COVER_SHIFT 24
#define CHANNEL_BITS 8
#define CODES 256
#define COLOUR_MASK 0x00FFFFFFu
#define LIGHT_TOLERANCE 1
#define FAILURES_SHOWN 10

#define PRINTABLE_FIRST 0x20
#define PRINTABLE_LAST 0x7E
#define PRINTABLES (PRINTABLE_LAST - PRINTABLE_FIRST + 1)
#define BLANK ' '

/* The buffers a window is drawn in by turns. */
#define BUFFERS 2

/* The frames drawn at each size, and the pixels past a buffer's rows,
   which are GUARD, so that a draw that strays shows. */
#define FRAMES 80
#define STRIDE_PAD 7
#define GUARD_ROWS 2
#define GUARD 0xC3A5F00Du

static const struct {
    INT64 width, height;
} sizes[] = {{1280, 800}, {2560, 1600}, {640, 480}, {1366, 768}, {200, 240}};
#define SIZES (sizeof sizes / sizeof sizes[0])

/* (coverage * 255 + 8) / 16 for each coverage, rounded down. */
static const UINT32 covers[] = {0,   16,  32,  48,  64,  80,  96,  112, 128,
                                143, 159, 175, 191, 207, 223, 239, 255};
_Static_assert(sizeof covers / sizeof covers[0] == COVERAGES,
               "covers lists every coverage");

/* The inks, as console.md C5 and look.md V3 give them: the text's, and
   the caret's warm light. */
static const UINT32 text_ink[PANEL_CHANNELS] = {232, 234, 240};
static const UINT32 caret_ink[PANEL_CHANNELS] = {236, 170, 96};

static unsigned failures;

static void check(int passed, const char *what, INT64 width, INT64 height,
                  UINT64 detail)
{
    if (!passed && failures++ < FAILURES_SHOWN)
        printf("failed: %s at %lldx%lld (%llu)\n", what, (long long)width,
               (long long)height, (unsigned long long)detail);
}

static UINT64 random_state = 0x9e3779b97f4a7c15ULL;

/* xorshift64 (Marsaglia, "Xorshift RNGs", 2003), seeded as the other
   checks seed it, so every run checks the same values. */
static UINT64 next_random(void)
{
    random_state ^= random_state << 13;
    random_state ^= random_state >> 7;
    random_state ^= random_state << 17;
    return random_state;
}

static UINT64 random_below(UINT64 n)
{
    return next_random() % n;
}

static UINT8 random_printable(void)
{
    return (UINT8)(PRINTABLE_FIRST + random_below(PRINTABLES));
}

/* ------------------------------------------------------------------------
   Light, computed from the sRGB formulas in floating point (IEC 61966-2-1,
   as gfx/raster.c cites them) */

static double light[CODES];

static void make_light(void)
{
    int code;

    for (code = 0; code < CODES; code++) {
        double c = code / 255.0;

        light[code] = c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4);
    }
}

static int code_of(double value)
{
    double c = value <= 0.0031308 ? 12.92 * value
                                  : 1.055 * pow(value, 1.0 / 2.4) - 0.055;
    int code = (int)floor(c * 255.0 + 0.5);

    return code < 0 ? 0 : code > 255 ? 255 : code;
}

/* The code of the light `from` and `to` mix to, `to` by weight / total. */
static int mixed(int from, int to, long weight, long total)
{
    double w = (double)weight / (double)total;

    return code_of((1.0 - w) * light[from] + w * light[to]);
}

/* ------------------------------------------------------------------------
   The pixel */

static UINT32 channel(UINT32 pixel, int k)
{
    return pixel >> ((PANEL_CHANNELS - 1 - k) * CHANNEL_BITS) & 0xFFu;
}

static void check_pixel_cover(UINT32 glyph, UINT32 caret, UINT32 pixel)
{
    UINT32 g = covers[glyph], c = covers[caret];
    UINT32 cover = g + c - g * c / COVER_MAX;

    check(pixel >> COVER_SHIFT == cover, "the pixel's cover", glyph, caret,
          pixel);
    check((cover == 0) == (pixel == 0),
          "only both coverages 0 give the pixel 0", glyph, caret, pixel);
    check(cover >= g && cover >= c && cover <= g + c && cover <= COVER_MAX,
          "the cover holds each and no more than both", glyph, caret, cover);
}

static void check_pixel_colour(UINT32 glyph, UINT32 caret, UINT32 pixel)
{
    UINT32 c = covers[caret], cover = pixel >> COVER_SHIFT;
    int k;

    if (pixel == 0)
        return;
    for (k = 0; k < PANEL_CHANNELS; k++) {
        UINT32 code = channel(pixel, k);
        UINT32 low = text_ink[k] < caret_ink[k] ? text_ink[k] : caret_ink[k];
        UINT32 high = text_ink[k] < caret_ink[k] ? caret_ink[k] : text_ink[k];

        if (caret == 0)
            check(code == text_ink[k], "a glyph alone is the text's ink",
                  glyph, caret, pixel);
        else if (glyph == 0 || caret == COVERAGE_MAX)
            check(code == caret_ink[k],
                  "a caret alone, or at full coverage, is the caret's ink",
                  glyph, caret, pixel);
        else
            check(abs((int)code - mixed((int)text_ink[k], (int)caret_ink[k],
                                        (long)c, (long)cover)) <=
                      LIGHT_TOLERANCE,
                  "the overlap is the caret's ink mixed over the glyph's in "
                  "linear light",
                  glyph, caret, pixel);
        check(code >= low && code <= high,
              "the colour lies between the two inks", glyph, caret, pixel);
    }
    check((pixel & ~COLOUR_MASK) == (cover << COVER_SHIFT),
          "nothing but the cover is above the colour", glyph, caret, pixel);
}

static void check_pixels(void)
{
    UINT32 glyph, caret;

    check(panel_window_pixel(0, 0) == 0, "no ink leaves the pixel 0", 0, 0,
          0);
    for (glyph = 0; glyph < COVERAGES; glyph++)
        for (caret = 0; caret < COVERAGES; caret++) {
            UINT32 pixel = panel_window_pixel((UINT8)glyph, (UINT8)caret);

            check_pixel_cover(glyph, caret, pixel);
            check_pixel_colour(glyph, caret, pixel);
            if (glyph > 0)
                check(pixel >> COVER_SHIFT >=
                          panel_window_pixel((UINT8)(glyph - 1),
                                             (UINT8)caret) >>
                              COVER_SHIFT,
                      "the cover grows with the glyph's coverage", glyph,
                      caret, pixel);
            if (caret > 0)
                check(pixel >> COVER_SHIFT >=
                          panel_window_pixel((UINT8)glyph,
                                             (UINT8)(caret - 1)) >>
                              COVER_SHIFT,
                      "the cover grows with the caret's coverage", glyph,
                      caret, pixel);
        }
}

/* ------------------------------------------------------------------------
   The view */

static TEXT text;

/* The bytes a console's text might be written: printable, the line feed,
   the carriage return, the tab and what no cell holds. */
static UINT8 random_byte(void)
{
    UINT64 pick = random_below(16);

    if (pick == 0)
        return '\n';
    if (pick == 1)
        return '\r';
    if (pick == 2)
        return '\t';
    if (pick == 3)
        return (UINT8)random_below(PRINTABLE_FIRST);
    return random_printable();
}

static void check_view(const LAYOUT *layout)
{
    UINT64 written = (UINT64)layout->rows * layout->columns * 3, i, top;
    PANEL_SHOWN want;

    memset(&want, 0, sizeof want);
    text_init(&text, layout->columns);
    for (i = 0; i < written; i++) {
        UINT8 byte = random_byte();

        text_write(&text, &byte, 1);
    }
    for (top = text_first(&text); top <= text.lines + 2; top++) {
        VIEW_CARET caret = {random_below(layout->rows),
                            (UINT32)random_below(layout->columns + 1),
                            (UINT32)random_below(2)};
        UINT64 row;
        UINT32 column;
        int same = 1;

        panel_view(layout, &text, top, &caret, &want);
        for (row = 0; row < layout->rows; row++)
            for (column = 0; column < layout->columns; column++) {
                UINT64 line = top + row;
                UINT8 expected =
                    line < text.lines
                        ? text.cells[line % TEXT_LINES][column]
                        : (UINT8)BLANK;

                same = same && want.cells[row][column] == expected;
            }
        check(same, "the view's cells are the text's lines from top",
              layout->columns, layout->rows, top);
        check(want.caret.row == caret.row &&
                  want.caret.column == caret.column &&
                  want.caret.lit == caret.lit,
              "the view's caret is the caret", layout->columns, layout->rows,
              top);
    }
}

/* ------------------------------------------------------------------------
   The drawing */

typedef struct {
    LAYOUT layout;
    UINT8 *masks;
    UINT32 width, height, stride;
    UINT32 *buffers[2];
    int drawn[2]; /* the buffer has been drawn in, and is not blank */
    UINT32 *expected, *before;
} WINDOW;

/* Whether a caret of frame stands in cell (row, column). */
static int caret_at(const PANEL_SHOWN *frame, UINT64 row, UINT32 column)
{
    return frame->caret.lit && frame->caret.row == row &&
           frame->caret.column == column;
}

/* The character cell (row, column) of frame shows: the column after the
   last holds none. */
static UINT8 shows(const LAYOUT *layout, const PANEL_SHOWN *frame,
                   UINT64 row, UINT32 column)
{
    return column < layout->columns ? frame->cells[row][column]
                                    : (UINT8)BLANK;
}

/* The window's coordinates of cell (row, column)'s top left corner: the
   cells start at the panel's text origin, and the window's origin is the
   panel's box's. */
static INT64 cell_x(const LAYOUT *layout, UINT32 column)
{
    return layout->text_left - layout->box.left +
           (INT64)column * layout->cell_width;
}

static INT64 cell_y(const LAYOUT *layout, UINT64 row)
{
    return layout->text_top - layout->box.top +
           (INT64)row * layout->cell_height;
}

/* The pixel for each pair of coverages, made once. */
static UINT32 pixels[COVERAGES][COVERAGES];

static void make_pixels(void)
{
    UINT32 glyph, caret;

    for (glyph = 0; glyph < COVERAGES; glyph++)
        for (caret = 0; caret < COVERAGES; caret++)
            pixels[glyph][caret] =
                panel_window_pixel((UINT8)glyph, (UINT8)caret);
}

/* The whole window showing frame, drawn from the layout a cell at a
   time: nothing but the cells, and the column past the last for a caret,
   has a pixel but 0. */
static void render(const WINDOW *w, const PANEL_SHOWN *frame, UINT32 *out)
{
    const LAYOUT *layout = &w->layout;
    INT64 cw = layout->cell_width, ch = layout->cell_height;
    const UINT8 *caret_mask = font_mask(w->masks, cw, FONT_CARET);
    UINT64 row;
    UINT32 column;

    memset(out, 0, (size_t)w->width * w->height * sizeof *out);
    for (row = 0; row < layout->rows; row++)
        for (column = 0; column <= layout->columns; column++) {
            const UINT8 *glyph =
                font_mask(w->masks, cw, shows(layout, frame, row, column));
            int lit = caret_at(frame, row, column);
            INT64 x, y;

            for (y = 0; y < ch; y++)
                for (x = 0; x < cw; x++)
                    out[(cell_y(layout, row) + y) * w->width +
                        cell_x(layout, column) + x] =
                        pixels[glyph[y * cw + x]]
                              [lit ? caret_mask[y * cw + x] : 0];
        }
}

/* The smallest rectangle, in the window's pixels, that holds every cell
   where a and b differ, a character or whether the caret is there.
   Returns 0 if none does. */
static int bounds(const LAYOUT *layout, const PANEL_SHOWN *a,
                  const PANEL_SHOWN *b, RECT *box)
{
    int found = 0;
    UINT64 row;
    UINT32 column;

    for (row = 0; row < layout->rows; row++)
        for (column = 0; column <= layout->columns; column++) {
            RECT cell;

            if (shows(layout, a, row, column) == shows(layout, b, row, column) &&
                caret_at(a, row, column) == caret_at(b, row, column))
                continue;
            cell.left = cell_x(layout, column);
            cell.top = cell_y(layout, row);
            cell.right = cell.left + layout->cell_width;
            cell.bottom = cell.top + layout->cell_height;
            if (!found) {
                *box = cell;
                found = 1;
                continue;
            }
            box->left = cell.left < box->left ? cell.left : box->left;
            box->top = cell.top < box->top ? cell.top : box->top;
            box->right = cell.right > box->right ? cell.right : box->right;
            box->bottom =
                cell.bottom > box->bottom ? cell.bottom : box->bottom;
        }
    return found;
}

static int same_rect(const RECT *a, const RECT *b)
{
    return a->left == b->left && a->top == b->top && a->right == b->right &&
           a->bottom == b->bottom;
}

/* The next frame: nothing visible, a caret that moves, goes out or goes
   to the column past the last, a few cells, a row or every cell changed,
   and changes the window never shows, cells past the last column and a
   caret that is not lit. */
static void next_frame(const LAYOUT *layout, PANEL_SHOWN *frame)
{
    UINT64 row, i, changes;
    UINT32 column;

    switch (random_below(8)) {
    case 0:
        break;
    case 1:
        frame->caret.row = random_below(layout->rows);
        frame->caret.column = (UINT32)random_below(layout->columns + 1);
        frame->caret.lit = (UINT32)random_below(2);
        break;
    case 2:
        for (i = 0, changes = 1 + random_below(4); i < changes; i++)
            frame->cells[random_below(layout->rows)]
                        [random_below(layout->columns)] = random_printable();
        break;
    case 3:
        row = random_below(layout->rows);
        for (column = 0; column < layout->columns; column++)
            frame->cells[row][column] = random_printable();
        break;
    case 4:
        for (row = 0; row < layout->rows; row++)
            for (column = 0; column < layout->columns; column++)
                frame->cells[row][column] = random_printable();
        break;
    case 5:
        frame->caret.lit = 0;
        break;
    case 6:
        frame->caret.row = random_below(layout->rows);
        frame->caret.column = layout->columns;
        frame->caret.lit = 1;
        break;
    default:
        for (row = 0; row < layout->rows; row++)
            for (column = layout->columns; column < TEXT_COLUMNS_MAX; column++)
                frame->cells[row][column] = random_printable();
        if (!frame->caret.lit) {
            frame->caret.row = random_below(layout->rows);
            frame->caret.column = (UINT32)random_below(layout->columns + 1);
        }
        break;
    }
}

static void start_frame(const LAYOUT *layout, PANEL_SHOWN *frame)
{
    UINT64 row;
    UINT32 column;

    for (row = 0; row < layout->rows; row++)
        for (column = 0; column < TEXT_COLUMNS_MAX; column++)
            frame->cells[row][column] = random_printable();
    frame->caret.row = random_below(layout->rows);
    frame->caret.column = (UINT32)random_below(layout->columns + 1);
    frame->caret.lit = (UINT32)random_below(2);
}

/* Whether the pixels of buffer, in the window's rows, are expected's, and
   nothing past them is not GUARD. */
static int holds(const WINDOW *w, const UINT32 *buffer, const UINT32 *expected)
{
    UINT32 x, y;

    for (y = 0; y < w->height; y++) {
        for (x = 0; x < w->width; x++)
            if (buffer[(UINT64)y * w->stride + x] !=
                expected[(UINT64)y * w->width + x])
                return 0;
        for (x = w->width; x < w->stride; x++)
            if (buffer[(UINT64)y * w->stride + x] != GUARD)
                return 0;
    }
    for (y = w->height; y < w->height + GUARD_ROWS; y++)
        for (x = 0; x < w->stride; x++)
            if (buffer[(UINT64)y * w->stride + x] != GUARD)
                return 0;
    return 1;
}

/* Whether every pixel where a and b differ lies in box, or box is 0. */
static int differences_in(const WINDOW *w, const UINT32 *a, const UINT32 *b,
                          const RECT *box)
{
    UINT32 x, y;

    for (y = 0; y < w->height; y++)
        for (x = 0; x < w->width; x++) {
            UINT64 at = (UINT64)y * w->width + x;

            if (a[at] != b[at] &&
                (!box || (INT64)x < box->left || (INT64)x >= box->right ||
                 (INT64)y < box->top || (INT64)y >= box->bottom))
                return 0;
        }
    return 1;
}

static void fill_guard(const WINDOW *w, UINT32 *buffer)
{
    UINT64 i, total = (UINT64)(w->height + GUARD_ROWS) * w->stride;
    UINT32 x, y;

    for (i = 0; i < total; i++)
        buffer[i] = GUARD;
    for (y = 0; y < w->height; y++)
        for (x = 0; x < w->width; x++)
            buffer[(UINT64)y * w->stride + x] = 0;
}

static int same_cells(const LAYOUT *layout, const PANEL_SHOWN *a,
                      const PANEL_SHOWN *b)
{
    UINT64 row;
    UINT32 column;

    for (row = 0; row < layout->rows; row++)
        for (column = 0; column < layout->columns; column++)
            if (a->cells[row][column] != b->cells[row][column])
                return 0;
    return a->caret.row == b->caret.row && a->caret.column == b->caret.column &&
           a->caret.lit == b->caret.lit;
}

/* One frame, the console's way: a first one drawn whole, and each after it
   drawn in the buffer the compositor does not have, damaged against the
   frame the compositor does. on_screen is the frame it has. */
static void check_frame(WINDOW *w, UINT64 step, int *presented,
                        PANEL_SHOWN *shown, PANEL_SHOWN *on_screen,
                        const PANEL_SHOWN *want)
{
    const LAYOUT *layout = &w->layout;
    INT64 width = w->width, height = w->height;
    const int available[BUFFERS] = {*presented != 0, *presented != 1};
    int target = panel_window_buffer(available, BUFFERS);
    RECT damage, expected_damage;
    int found = 0, expected_found = 0;

    if (*presented >= 0) {
        found = panel_window_damage(layout, on_screen, want, &damage);
        expected_found = bounds(layout, on_screen, want, &expected_damage);
        check(found == expected_found, "damage is found where a cell differs",
              width, height, step);
        check(!found || same_rect(&damage, &expected_damage),
              "damage is the cells that differ's smallest rectangle", width,
              height, step);
        if (!found) {
            render(w, on_screen, w->before);
            render(w, want, w->expected);
            check(memcmp(w->before, w->expected,
                         (size_t)w->width * w->height * sizeof *w->before) ==
                      0,
                  "frames with no cell differing are the same pixels", width,
                  height, step);
            return;
        }
    }
    if (!w->drawn[target]) {
        panel_window_forget(layout, &shown[target]);
        w->drawn[target] = 1;
    }
    panel_window_draw(layout, w->masks, w->buffers[target], w->stride,
                      &shown[target], want);
    render(w, want, w->expected);
    check(holds(w, w->buffers[target], w->expected),
          "the buffer is the render of the frame, and nothing is written past "
          "its rows",
          width, height, step);
    check(same_cells(layout, &shown[target], want),
          "the drawn buffer shows the frame", width, height, step);
    if (*presented >= 0) {
        render(w, on_screen, w->before);
        check(differences_in(w, w->before, w->expected, found ? &damage : 0),
              "every pixel the frame changes lies in its damage", width,
              height, step);
    }
    *on_screen = *want;
    *presented = target;
}

/* ------------------------------------------------------------------------
   The buffer a frame is drawn in. */

#define MODEL_STEPS 200
#define MODEL_SCHEDULES 400
#define NOTHING (-1)

/* A step sends at most two messages and one present, so a queue of this
   many never wraps in a schedule. */
#define MODEL_QUEUE 512
_Static_assert(MODEL_QUEUE > 2 * MODEL_STEPS + 8,
               "a schedule's messages fit the queues");

/* What the compositor sends the console, in the order it sends it. */
typedef enum { EVENT_CONFIGURE, EVENT_FRAME, EVENT_RELEASED } EVENT_KIND;

typedef struct {
    EVENT_KIND kind;
    int argument; /* a configure's serial, a release's buffer */
} EVENT;

/* The console's two buffers and the compositor's side of them, as
   display.md D7 gives it. The compositor shows a present that names its
   latest configure, answers it with a frame and releases the buffer it
   showed before; it ignores a present that names another, answers it with
   no frame and releases that present's buffer at once. The console
   draws in a buffer it is told is released, presents it naming the
   configure it was last told of, and waits for the frame, which a
   configure also ends the wait for. */
typedef struct {
    int available[BUFFERS]; /* the console's view: given none, or released */
    int owed;               /* a present's frame has not been taken */
    int serial;             /* the configure the console was last told of */
    int latest;             /* the compositor's latest configure */
    int shown;              /* the buffer the compositor reads, or NOTHING */
    struct {
        int buffer, serial;
    } presents[MODEL_QUEUE]; /* sent and not yet read by the compositor */
    unsigned presents_first, presents_last;
    EVENT events[MODEL_QUEUE]; /* sent and not yet taken by the console */
    unsigned events_first, events_last;
} MODEL;

static void model_start(MODEL *m)
{
    memset(m, 0, sizeof *m);
    m->available[0] = m->available[1] = 1;
    m->serial = m->latest = 1;
    m->shown = NOTHING;
}

static void model_send(MODEL *m, EVENT_KIND kind, int argument)
{
    m->events[m->events_last % MODEL_QUEUE].kind = kind;
    m->events[m->events_last % MODEL_QUEUE].argument = argument;
    m->events_last++;
}

/* The console draws and presents, if it waits for no frame and has a
   buffer. Returns whether the buffer it chose was one the compositor holds,
   which is a failure of the rule. */
static int model_console_shows(MODEL *m)
{
    int target;

    if (m->owed)
        return 0;
    target = panel_window_buffer(m->available, BUFFERS);
    if (target == PANEL_NO_BUFFER)
        return 0;
    if (target == m->shown)
        return 1;
    m->available[target] = 0;
    m->presents[m->presents_last % MODEL_QUEUE].buffer = target;
    m->presents[m->presents_last % MODEL_QUEUE].serial = m->serial;
    m->presents_last++;
    m->owed = 1;
    return 0;
}

static void model_compositor_configures(MODEL *m)
{
    m->latest++;
    model_send(m, EVENT_CONFIGURE, m->latest);
}

static int model_compositor_reads(MODEL *m)
{
    int buffer, serial;

    if (m->presents_first == m->presents_last)
        return 0;
    buffer = m->presents[m->presents_first % MODEL_QUEUE].buffer;
    serial = m->presents[m->presents_first % MODEL_QUEUE].serial;
    m->presents_first++;
    if (serial != m->latest) {
        model_send(m, EVENT_RELEASED, buffer);
        return 1;
    }
    if (m->shown != NOTHING)
        model_send(m, EVENT_RELEASED, m->shown);
    m->shown = buffer;
    model_send(m, EVENT_FRAME, 0);
    return 1;
}

static int model_console_takes(MODEL *m)
{
    EVENT event;

    if (m->events_first == m->events_last)
        return 0;
    event = m->events[m->events_first % MODEL_QUEUE];
    m->events_first++;
    if (event.kind == EVENT_CONFIGURE) {
        m->serial = event.argument;
        m->owed = 0;
    } else if (event.kind == EVENT_FRAME) {
        m->owed = 0;
    } else {
        m->available[event.argument] = 1;
    }
    return 1;
}

/* Take every message in flight, the compositor's reads and the console's. */
static void model_settle(MODEL *m)
{
    while (model_compositor_reads(m) || model_console_takes(m))
        ;
}

/* Once the messages in flight are taken, the console waits for no frame
   and holds a buffer to draw in, which is not the one the compositor
   reads. */
static void check_settled(MODEL *m, const char *what, UINT64 detail)
{
    int target = panel_window_buffer(m->available, BUFFERS);

    check(m->owed == 0, what, 0, 0, detail);
    check(target != PANEL_NO_BUFFER && target != m->shown, what, 0, 0,
          detail);
}

/* The choice of a buffer, and the exchange where a configure crosses a
   present. The console presents A, the compositor shows it; the console
   presents B, and the compositor's configure crosses it, so that the
   compositor ignores B, releases it at once and goes on reading A. The
   console's next buffer is B: choosing the one that was not presented
   last gives A, which is read and never released, and the window would
   never be drawn in again. */
static void check_buffers(void)
{
    static const int none[BUFFERS] = {0, 0}, both[BUFFERS] = {1, 1},
                     first[BUFFERS] = {1, 0}, second[BUFFERS] = {0, 1};
    MODEL m;
    UINT64 schedule, step;
    int drew_in_held = 0;

    check(panel_window_buffer(none, BUFFERS) == PANEL_NO_BUFFER,
          "no buffer is chosen where none is available", 0, 0, 0);
    check(panel_window_buffer(both, BUFFERS) == 0,
          "the first buffer is chosen where both are available", 0, 0, 0);
    check(panel_window_buffer(first, BUFFERS) == 0,
          "the first is chosen where it alone is available", 0, 0, 0);
    check(panel_window_buffer(second, BUFFERS) == 1,
          "the second is chosen where it alone is available", 0, 0, 0);
    check(panel_window_buffer(both, 0) == PANEL_NO_BUFFER,
          "no buffer is chosen of none", 0, 0, 0);

    model_start(&m);
    model_console_shows(&m); /* A, naming configure 1 */
    model_settle(&m);        /* shown, framed */
    check(m.shown == 0 && !m.owed, "the first present is shown and framed", 0,
          0, 0);
    check(model_console_shows(&m) == 0 &&
              m.presents[(m.presents_last - 1) % MODEL_QUEUE].buffer == 1,
          "B is presented while A is read", 0, 0, 0);
    model_compositor_configures(&m); /* crosses B's present */
    check(model_compositor_reads(&m) == 1, "the compositor reads B's present",
          0, 0, 0);
    check(model_console_takes(&m) == 1 && m.serial == 2 && !m.owed,
          "the configure ends the wait for a frame", 0, 0, 0);
    check(model_console_takes(&m) == 1 && m.available[1] && !m.available[0],
          "the ignored present's buffer is released at once, A still read", 0,
          0, 0);
    check(panel_window_buffer(m.available, BUFFERS) == 1,
          "the buffer drawn in after the crossing is the released one", 0, 0,
          0);
    check(model_console_shows(&m) == 0 && m.owed,
          "and it is presented", 0, 0, 0);
    model_settle(&m);
    check_settled(&m, "after the crossing is settled", 0);

    for (schedule = 0; schedule < MODEL_SCHEDULES; schedule++) {
        model_start(&m);
        for (step = 0; step < MODEL_STEPS; step++) {
            switch (random_below(4)) {
            case 0:
                drew_in_held |= model_console_shows(&m);
                break;
            case 1:
                model_compositor_reads(&m);
                break;
            case 2:
                model_console_takes(&m);
                break;
            default:
                if (random_below(3) == 0 && m.latest - m.serial < 4)
                    model_compositor_configures(&m);
                break;
            }
        }
        model_settle(&m);
        check_settled(&m, "after any schedule is settled", schedule);
    }
    check(!drew_in_held, "no frame is drawn in a buffer the compositor reads", 0,
          0, 0);
}

/* A buffer that is forgotten is drawn whole, whatever it held and
   whatever *shown said it did: a buffer that is dirty, and one that is
   blank again under a *shown that still has the caret in the column past
   the last. */
static void check_forget(WINDOW *w)
{
    const LAYOUT *layout = &w->layout;
    PANEL_SHOWN shown, first, second;
    int blank;

    start_frame(layout, &first);
    start_frame(layout, &second);
    first.cells[0][0] = 'W';
    second.cells[0][0] = BLANK;
    first.caret.row = second.caret.row = layout->rows - 1;
    first.caret.column = second.caret.column = layout->columns;
    first.caret.lit = second.caret.lit = 1;
    for (blank = 0; blank < 2; blank++) {
        fill_guard(w, w->buffers[0]);
        memset(&shown, 0, sizeof shown);
        panel_window_forget(layout, &shown);
        panel_window_draw(layout, w->masks, w->buffers[0], w->stride, &shown,
                          &first);
        if (blank)
            fill_guard(w, w->buffers[0]);
        panel_window_forget(layout, &shown);
        panel_window_draw(layout, w->masks, w->buffers[0], w->stride, &shown,
                          &second);
        render(w, &second, w->expected);
        check(holds(w, w->buffers[0], w->expected),
              "a forgotten buffer is drawn whole", w->width, w->height,
              (UINT64)blank);
    }
    fill_guard(w, w->buffers[0]);
}

static void check_forgotten(const WINDOW *w)
{
    const LAYOUT *layout = &w->layout;
    PANEL_SHOWN shown, want;
    RECT damage, expected;

    memset(&shown, 0, sizeof shown);
    start_frame(layout, &want);
    want.caret.lit = 0;
    panel_window_forget(layout, &shown);
    expected.left = cell_x(layout, 0);
    expected.top = cell_y(layout, 0);
    expected.right = cell_x(layout, layout->columns);
    expected.bottom = cell_y(layout, layout->rows);
    check(panel_window_damage(layout, &shown, &want, &damage) &&
              same_rect(&damage, &expected),
          "a forgotten buffer is damaged over every cell", w->width, w->height,
          0);
    want.caret.lit = 1;
    want.caret.row = layout->rows - 1;
    want.caret.column = layout->columns;
    expected.right += layout->cell_width;
    check(panel_window_damage(layout, &shown, &want, &damage) &&
              same_rect(&damage, &expected),
          "and over the column past the last where the caret is", w->width,
          w->height, 0);
}

static int make_window(WINDOW *w, INT64 width, INT64 height)
{
    UINT64 total;
    int i;

    memset(w, 0, sizeof *w);
    if (!panel_layout(&w->layout, width, height))
        return 0;
    w->width = (UINT32)(w->layout.box.right - w->layout.box.left);
    w->height = (UINT32)(w->layout.box.bottom - w->layout.box.top);
    w->stride = w->width + STRIDE_PAD;
    total = (UINT64)(w->height + GUARD_ROWS) * w->stride;
    w->masks = malloc((size_t)font_masks_size(w->layout.cell_width));
    w->expected = malloc((size_t)w->width * w->height * sizeof(UINT32));
    w->before = malloc((size_t)w->width * w->height * sizeof(UINT32));
    for (i = 0; i < 2; i++)
        w->buffers[i] = malloc((size_t)total * sizeof(UINT32));
    if (!w->masks || !w->expected || !w->before || !w->buffers[0] ||
        !w->buffers[1])
        return 0;
    font_render(w->layout.cell_width, w->masks);
    for (i = 0; i < 2; i++)
        fill_guard(w, w->buffers[i]);
    return 1;
}

static void free_window(WINDOW *w)
{
    free(w->masks);
    free(w->expected);
    free(w->before);
    free(w->buffers[0]);
    free(w->buffers[1]);
}

static void check_size(INT64 width, INT64 height)
{
    static PANEL_SHOWN shown[2], on_screen, want;
    WINDOW w;
    int presented = -1;
    UINT64 step;

    if (!make_window(&w, width, height)) {
        check(0, "a window is made", width, height, 0);
        free_window(&w);
        return;
    }
    check_view(&w.layout);
    check_forgotten(&w);
    check_forget(&w);
    memset(shown, 0, sizeof shown);
    start_frame(&w.layout, &want);
    for (step = 0; step < FRAMES; step++) {
        if (step > 0)
            next_frame(&w.layout, &want);
        check_frame(&w, step, &presented, shown, &on_screen, &want);
    }
    free_window(&w);
}

int main(void)
{
    size_t i;

    make_light();
    check_pixels();
    check_buffers();
    make_pixels();
    for (i = 0; i < SIZES; i++)
        check_size(sizes[i].width, sizes[i].height);
    printf("%u failures\n", failures);
    return failures != 0;
}
