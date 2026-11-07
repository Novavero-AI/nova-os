/* The boot screen: on black, the wordmark "nova" in anti-aliased strokes
   and a thin ring whose bright head sweeps clockwise over a faint track.
   Coverage mixes the ink with the black in linear light (raster.h). */

#include "boot_screen.h"

typedef struct {
    INT64 r, g, b;
} RGB;

static const RGB ink = {242, 242, 246};

/* The pixel code for colour code over black, covering coverage of its
   COVERAGE_MAX samples. Most pixels are covered whole or not at all, and
   those need no search: a code mixed with no light from black is itself,
   and with none of its own black. */
static INT64 mix(INT64 code, INT64 coverage)
{
    if (coverage == COVERAGE_MAX)
        return code;
    if (coverage == 0)
        return 0;
    return gfx_cross_fade(0, code, coverage, COVERAGE_MAX);
}

static INT64 smaller(INT64 a, INT64 b) { return a < b ? a : b; }

static INT64 larger(INT64 a, INT64 b) { return a > b ? a : b; }

/* The ink, its codes scaled by a brightness of 0..255, over black at
   coverage of COVERAGE_MAX samples. Brightness picks the colour a fully
   covered pixel shows; coverage mixes that colour's light with black. */
static void put(const SCREEN *screen, INT64 x, INT64 y, INT64 brightness,
                INT64 coverage)
{
    INT64 r, g, b;

    if (x < 0 || y < 0 || x >= screen->width || y >= screen->height)
        return;
    r = mix(ink.r * brightness / 255, coverage);
    g = mix(ink.g * brightness / 255, coverage);
    b = mix(ink.b * brightness / 255, coverage);
    screen->pixels[y * screen->stride + x] = gfx_pixel(screen, r, g, b);
}

/* ------------------------------------------------------------------------
   The wordmark, in geometric strokes: x-height h, stroke radius r. Each
   letter adds its strokes with its top-left corner at (x, y) and returns
   its width. */

#define MAX_SHAPES 8
#define LETTER_GAP 32 /* percent of h */

static SHAPE wordmark[MAX_SHAPES];
static int wordmark_count;

static void add(int kind, INT64 ax, INT64 ay, INT64 bx, INT64 by, INT64 r)
{
    SHAPE *s = &wordmark[wordmark_count++];

    s->kind = kind;
    s->ax = ax;
    s->ay = ay;
    s->bx = bx;
    s->by = by;
    s->r = r;
}

static INT64 letter_n(INT64 x, INT64 y, INT64 h, INT64 r)
{
    INT64 w = h * 82 / 100, arc = w / 2 - r;

    add(CAPSULE, x + r, y + r, x + r, y + h - r, r);
    add(ARCH, x + w / 2, y + r + arc, arc, 0, r);
    add(CAPSULE, x + w - r, y + r + arc, x + w - r, y + h - r, r);
    return w;
}

static INT64 letter_o(INT64 x, INT64 y, INT64 h, INT64 r)
{
    add(RING, x + h / 2, y + h / 2, h / 2 - r, 0, r);
    return h;
}

static INT64 letter_v(INT64 x, INT64 y, INT64 h, INT64 r)
{
    INT64 w = h * 90 / 100;

    add(CAPSULE, x + r, y + r, x + w / 2, y + h - r, r);
    add(CAPSULE, x + w / 2, y + h - r, x + w - r, y + r, r);
    return w;
}

static INT64 letter_a(INT64 x, INT64 y, INT64 h, INT64 r)
{
    add(RING, x + h / 2, y + h / 2, h / 2 - r, 0, r);
    add(CAPSULE, x + h - r, y + r, x + h - r, y + h - r, r);
    return h;
}

static INT64 wordmark_width(INT64 h)
{
    return h * 82 / 100 + h + h * 90 / 100 + h + 3 * (h * LETTER_GAP / 100);
}

/* Where the wordmark goes on a screen: its x-height h and its top-left
   corner. */
static void wordmark_place(const SCREEN *screen, INT64 *h, INT64 *left,
                           INT64 *top)
{
    *h = screen->width * SUBPIXEL / 22;
    *left = (screen->width * SUBPIXEL - wordmark_width(*h)) / 2;
    *top = screen->height * SUBPIXEL * 44 / 100 - *h / 2;
}

/* The pixels draw_wordmark visits, which hold every stroke. */
static void wordmark_rect(INT64 h, INT64 left, INT64 top, RECT *rect)
{
    rect->left = left / SUBPIXEL - 1;
    rect->top = top / SUBPIXEL;
    rect->right = (left + wordmark_width(h)) / SUBPIXEL + 2;
    rect->bottom = (top + h) / SUBPIXEL + 2;
}

static void draw_wordmark(const SCREEN *screen, INT64 h, INT64 left,
                          INT64 top)
{
    INT64 r = h / 13, x = left, y;
    RECT rect;

    wordmark_count = 0;
    x += letter_n(x, top, h, r) + h * LETTER_GAP / 100;
    x += letter_o(x, top, h, r) + h * LETTER_GAP / 100;
    x += letter_v(x, top, h, r) + h * LETTER_GAP / 100;
    letter_a(x, top, h, r);
    wordmark_rect(h, left, top, &rect);
    for (y = rect.top; y < rect.bottom; y++)
        for (x = rect.left; x < rect.right; x++)
            put(screen, x, y, 255,
                gfx_coverage(wordmark, wordmark_count, x, y));
}

void gfx_boot_screen(const SCREEN *screen, SPINNER *spinner)
{
    INT64 h, left, top;

    wordmark_place(screen, &h, &left, &top);
    draw_wordmark(screen, h, left, top);
    spinner->cx = screen->width / 2;
    spinner->cy = (top + h) / SUBPIXEL + h * 13 / 10 / SUBPIXEL;
    spinner->radius = h * 26 / 100;
    spinner->width = h * 3 / 100 + 1;
    spinner->head = 0;
}

/* ------------------------------------------------------------------------
   The spinner. Angles are in sixteenths of a degree, clockwise from up. */

#define TURN (360 * 16)
#define TAIL (TURN * 3 / 4)
#define STEP (TURN / 48) /* the head's advance per frame */
#define TRACK 22         /* the track's brightness, 0..255 */

/* atan of z/4096 for 0 <= z <= 4096, in sixteenths of a degree, from
   atan(z) ~ 45z + 15.64z(1 - z) degrees, within a quarter degree. */
static INT64 atan_octant(INT64 z)
{
    return (720 * z + 250 * z * (4096 - z) / 4096) / 4096;
}

/* The angle of (dx, dy), with y growing downward, clockwise from up. */
static INT64 clock_angle(INT64 dx, INT64 dy)
{
    INT64 ny = -dy;
    INT64 ax = dx < 0 ? -dx : dx, ay = ny < 0 ? -ny : ny;
    INT64 base, theta;

    if (ax == 0 && ay == 0)
        return 0;
    base = ax >= ay ? atan_octant(ay * 4096 / ax)
                    : TURN / 4 - atan_octant(ax * 4096 / ay);
    if (dx >= 0)
        theta = ny >= 0 ? base : TURN - base;
    else
        theta = ny >= 0 ? TURN / 2 - base : TURN / 2 + base;
    return ((TURN / 4 - theta) % TURN + TURN) % TURN;
}

/* The pixels gfx_spinner_step visits: a square around the ring, two
   pixels wider than its outer edge on each side. */
static void spinner_rect(const SPINNER *sp, RECT *rect)
{
    INT64 reach = (sp->radius + sp->width) / SUBPIXEL + 2;

    rect->left = sp->cx - reach;
    rect->top = sp->cy - reach;
    rect->right = sp->cx + reach + 1;
    rect->bottom = sp->cy + reach + 1;
}

void gfx_spinner_step(const SCREEN *screen, SPINNER *sp)
{
    SHAPE ring;
    RECT rect;
    INT64 x, y;

    ring.kind = RING;
    ring.ax = sp->cx * SUBPIXEL + SUBPIXEL / 2;
    ring.ay = sp->cy * SUBPIXEL + SUBPIXEL / 2;
    ring.bx = sp->radius;
    ring.r = sp->width;
    spinner_rect(sp, &rect);
    for (y = rect.top; y < rect.bottom; y++)
        for (x = rect.left; x < rect.right; x++) {
            INT64 behind =
                ((sp->head - clock_angle(x - sp->cx, y - sp->cy)) % TURN +
                 TURN) % TURN;
            INT64 brightness = TRACK;

            if (behind < TAIL) {
                INT64 t = (TAIL - behind) * 255 / TAIL;

                brightness = TRACK + (255 - TRACK) * t * t / (255 * 255);
            }
            put(screen, x, y, brightness, gfx_coverage(&ring, 1, x, y));
        }
    sp->head = (sp->head + STEP) % TURN;
    gfx_flush();
}

/* ------------------------------------------------------------------------ */

void gfx_boot_bounds(const SCREEN *screen, const SPINNER *spinner,
                     RECT *bounds)
{
    INT64 h, left, top;
    RECT ring;

    wordmark_place(screen, &h, &left, &top);
    wordmark_rect(h, left, top, bounds);
    spinner_rect(spinner, &ring);
    bounds->left = larger(smaller(bounds->left, ring.left), 0);
    bounds->top = larger(smaller(bounds->top, ring.top), 0);
    bounds->right = smaller(larger(bounds->right, ring.right), screen->width);
    bounds->bottom =
        smaller(larger(bounds->bottom, ring.bottom), screen->height);
    bounds->right = larger(bounds->right, bounds->left);
    bounds->bottom = larger(bounds->bottom, bounds->top);
}
