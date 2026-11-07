/* The boot screen: on black, the wordmark "nova" in anti-aliased strokes
   and a thin ring whose bright head sweeps clockwise over a faint track.
   Geometry is in eighths of a pixel, and a pixel's coverage is how many of
   16 sample points fall inside a stroke; inside tests compare squared
   distances, so no square root is taken. */

#include "gfx.h"

#define SUBPIXEL 8
#define SAMPLES 4
#define COVERAGE_MAX (SAMPLES * SAMPLES)

typedef struct {
    INT64 r, g, b;
} RGB;

static const RGB ink = {242, 242, 246};

/* The ink at a brightness of 0..255 times coverage, over black. */
static void put(const SCREEN *screen, INT64 x, INT64 y, INT64 brightness,
                INT64 coverage)
{
    INT64 a = brightness * coverage / COVERAGE_MAX;
    INT64 r = ink.r * a / 255, g = ink.g * a / 255, b = ink.b * a / 255;

    if (x < 0 || y < 0 || x >= screen->width || y >= screen->height)
        return;
    screen->pixels[y * screen->stride + x] =
        (UINT32)(screen->format == PIXEL_BGR ? r << 16 | g << 8 | b
                                             : b << 16 | g << 8 | r);
}

void gfx_clear(const SCREEN *screen)
{
    INT64 x, y;

    for (y = 0; y < screen->height; y++)
        for (x = 0; x < screen->width; x++)
            screen->pixels[y * screen->stride + x] = 0;
}

/* ------------------------------------------------------------------------
   Strokes */

#define CAPSULE 0 /* the segment (ax, ay)-(bx, by), radius r */
#define RING 1    /* centre (ax, ay), radius bx, half-width r */
#define ARCH 2    /* the upper half of a RING */

typedef struct {
    int kind;
    INT64 ax, ay, bx, by, r;
} SHAPE;

static int inside(const SHAPE *s, INT64 px, INT64 py)
{
    INT64 qx = px - s->ax, qy = py - s->ay;

    if (s->kind == CAPSULE) {
        INT64 dx = s->bx - s->ax, dy = s->by - s->ay;
        INT64 dot = qx * dx + qy * dy, len2 = dx * dx + dy * dy;
        INT64 cross;

        if (dot <= 0)
            return qx * qx + qy * qy <= s->r * s->r;
        if (dot >= len2) {
            INT64 ex = px - s->bx, ey = py - s->by;
            return ex * ex + ey * ey <= s->r * s->r;
        }
        /* The squared distance to the line is cross^2 / len2. */
        cross = qx * dy - qy * dx;
        return cross * cross <= s->r * s->r * len2;
    } else {
        INT64 d2 = qx * qx + qy * qy;
        INT64 lo = s->bx - s->r, hi = s->bx + s->r;

        if (s->kind == ARCH && py > s->ay)
            return 0;
        if (lo < 0)
            lo = 0;
        return d2 >= lo * lo && d2 <= hi * hi;
    }
}

static INT64 coverage(const SHAPE *shapes, int count, INT64 x, INT64 y)
{
    INT64 sx, sy, hits = 0;
    int i;

    for (sy = 0; sy < SAMPLES; sy++)
        for (sx = 0; sx < SAMPLES; sx++) {
            INT64 px = x * SUBPIXEL + sx * (SUBPIXEL / SAMPLES) + 1;
            INT64 py = y * SUBPIXEL + sy * (SUBPIXEL / SAMPLES) + 1;

            for (i = 0; i < count; i++)
                if (inside(&shapes[i], px, py)) {
                    hits++;
                    break;
                }
        }
    return hits;
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

static void draw_wordmark(const SCREEN *screen, INT64 left, INT64 top,
                          INT64 h)
{
    INT64 r = h / 13, x = left, y;
    INT64 right = (left + wordmark_width(h)) / SUBPIXEL + 1;

    wordmark_count = 0;
    x += letter_n(x, top, h, r) + h * LETTER_GAP / 100;
    x += letter_o(x, top, h, r) + h * LETTER_GAP / 100;
    x += letter_v(x, top, h, r) + h * LETTER_GAP / 100;
    letter_a(x, top, h, r);
    for (y = top / SUBPIXEL; y <= (top + h) / SUBPIXEL + 1; y++)
        for (x = left / SUBPIXEL - 1; x <= right; x++)
            put(screen, x, y, 255,
                coverage(wordmark, wordmark_count, x, y));
}

void gfx_boot_screen(const SCREEN *screen, SPINNER *spinner)
{
    INT64 h = screen->width * SUBPIXEL / 22;
    INT64 top = screen->height * SUBPIXEL * 44 / 100 - h / 2;

    draw_wordmark(screen, (screen->width * SUBPIXEL - wordmark_width(h)) / 2,
                  top, h);
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

void gfx_spinner_step(const SCREEN *screen, SPINNER *sp)
{
    SHAPE ring;
    INT64 x, y, reach = (sp->radius + sp->width) / SUBPIXEL + 2;

    ring.kind = RING;
    ring.ax = sp->cx * SUBPIXEL + SUBPIXEL / 2;
    ring.ay = sp->cy * SUBPIXEL + SUBPIXEL / 2;
    ring.bx = sp->radius;
    ring.r = sp->width;
    for (y = sp->cy - reach; y <= sp->cy + reach; y++)
        for (x = sp->cx - reach; x <= sp->cx + reach; x++) {
            INT64 behind =
                ((sp->head - clock_angle(x - sp->cx, y - sp->cy)) % TURN +
                 TURN) % TURN;
            INT64 brightness = TRACK;

            if (behind < TAIL) {
                INT64 t = (TAIL - behind) * 255 / TAIL;

                brightness = TRACK + (255 - TRACK) * t * t / (255 * 255);
            }
            put(screen, x, y, brightness, coverage(&ring, 1, x, y));
        }
    sp->head = (sp->head + STEP) % TURN;
}
