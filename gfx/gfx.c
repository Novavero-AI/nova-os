/* The boot screen: on black, the wordmark "nova" in anti-aliased strokes
   and a thin ring whose bright head sweeps clockwise over a faint track.
   Geometry is in eighths of a pixel, and a pixel's coverage is how many of
   16 sample points fall inside a stroke; only pixels near a stroke's edge
   are sampled. Inside tests compare squared distances, so no
   square root is taken. Coverage mixes the ink with the black in linear
   light. */

#include "gfx.h"

#define SUBPIXEL 8
#define SAMPLES 4
#define COVERAGE_MAX (SAMPLES * SAMPLES)

/* How far a sample point can lie from its pixel's centre, in eighths,
   rounded up: the samples sit 1, 3, 5 and 7 eighths into the pixel on
   each axis, at most 3 from the centre's 4, so 3 * sqrt(2) ~ 4.24 away. */
#define SAMPLE_REACH 5

/* ------------------------------------------------------------------------
   sRGB. A pixel's value is an sRGB code, which encodes light by sRGB's
   transfer function (IEC 61966-2-1, sold by the IEC; the ICC's "How to
   interpret the sRGB color space (specified in IEC 61966-2-1) for ICC
   profiles" gives the encoding in A.8 and the decoding in B.1). A pixel a
   stroke covers in part emits that share of the stroke's light, so
   coverage scales the light a code decodes to, and the result is encoded
   back; scaling the code itself would darken every partial pixel. */

#define CODE_MAX 255
#define HALF_CODES (2 * CODE_MAX)
#define LIGHT_BITS 30
#define LIGHT_ONE ((UINT64)1 << LIGHT_BITS)

/* Decoding code C: C / 12.92 for C up to 0.04045, else
   ((C + 0.055) / 1.055)^2.4, with each constant an exact fraction. */
#define KNEE_NUM 4045 /* 0.04045 */
#define KNEE_DEN 100000
#define SLOPE_NUM 1292 /* 12.92 */
#define SLOPE_DEN 100
#define OFFSET_NUM 55  /* 0.055 */
#define SCALE_NUM 1055 /* 1.055 */
#define OFFSET_DEN 1000 /* of OFFSET_NUM and SCALE_NUM */

/* light[j] is the light of code j / 2 as a fraction of LIGHT_ONE: the
   even entries decode the 256 codes, and the odd ones are the midpoints
   between them, where encoding rounds up. gfx_boot_screen and
   gfx_spinner_step compute it on first use, in integers, so it follows
   from the formula above with no generator in the build and no floating
   point in either image. */
static UINT32 light[HALF_CODES + 1];
static int light_ready;

/* a * b for fractions of LIGHT_ONE. */
static UINT64 times(UINT64 a, UINT64 b)
{
    return a * b >> LIGHT_BITS;
}

/* The fifth root of a fraction of LIGHT_ONE: the largest fraction, set
   bit by bit, whose fifth power by times does not exceed it. */
static UINT64 fifth_root(UINT64 a)
{
    UINT64 root = 0, bit;

    for (bit = LIGHT_ONE; bit != 0; bit >>= 1) {
        UINT64 next = root + bit;

        if (next <= LIGHT_ONE &&
            times(times(times(times(next, next), next), next), next) <= a)
            root = next;
    }
    return root;
}

/* The light of code j / 2, for C = j / HALF_CODES. Above the knee,
   t^2.4 = t^2 * (t^2)^(1/5). */
static UINT64 decode_half(UINT64 j)
{
    UINT64 t, t2;

    if (j * KNEE_DEN <= KNEE_NUM * HALF_CODES)
        return j * SLOPE_DEN * LIGHT_ONE / (HALF_CODES * SLOPE_NUM);
    t = (j * OFFSET_DEN + OFFSET_NUM * HALF_CODES) * LIGHT_ONE /
        (SCALE_NUM * HALF_CODES);
    t2 = times(t, t);
    return times(t2, fifth_root(t2));
}

static void fill_light(void)
{
    UINT64 j;

    if (light_ready)
        return;
    for (j = 0; j <= HALF_CODES; j++)
        light[j] = (UINT32)decode_half(j);
    light_ready = 1;
}

/* The code nearest the encoding of value, a fraction of LIGHT_ONE: the
   number of midpoints at or below it. */
static INT64 encode(UINT64 value)
{
    INT64 low = 0, high = CODE_MAX;

    while (low < high) {
        INT64 mid = (low + high + 1) / 2;

        if (light[2 * mid - 1] <= value)
            low = mid;
        else
            high = mid - 1;
    }
    return low;
}

/* The pixel code for colour code over black, covering coverage of its
   COVERAGE_MAX samples. Most pixels are covered whole or not at all, and
   those need no search: encode gives back every code from its light, and
   0 from no light. */
static INT64 mix(INT64 code, INT64 coverage)
{
    if (coverage == COVERAGE_MAX)
        return code;
    if (coverage == 0)
        return 0;
    return encode(light[2 * code] * (UINT64)coverage / COVERAGE_MAX);
}

/* ------------------------------------------------------------------------ */

typedef struct {
    INT64 r, g, b;
} RGB;

static const RGB ink = {242, 242, 246};

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

/* Whether (px, py) lies inside s with its radius r, and an ARCH's cut,
   moved outward by grow eighths, or inward where grow is negative. For a
   distance d, a point within d of the shape lies inside it grown by d,
   and a point within d of one inside it shrunk by d lies inside the
   shape: r bounds the distance to a segment or a circle, which changes no
   more than the point moves, and the cut bounds y alone. */
static int inside(const SHAPE *s, INT64 px, INT64 py, INT64 grow)
{
    INT64 qx = px - s->ax, qy = py - s->ay, r = s->r + grow;

    if (r < 0)
        return 0;
    if (s->kind == CAPSULE) {
        INT64 dx = s->bx - s->ax, dy = s->by - s->ay;
        INT64 dot = qx * dx + qy * dy, len2 = dx * dx + dy * dy;
        INT64 cross;

        if (dot <= 0)
            return qx * qx + qy * qy <= r * r;
        if (dot >= len2) {
            INT64 ex = px - s->bx, ey = py - s->by;
            return ex * ex + ey * ey <= r * r;
        }
        /* The squared distance to the line is cross^2 / len2. */
        cross = qx * dy - qy * dx;
        return cross * cross <= r * r * len2;
    } else {
        INT64 d2 = qx * qx + qy * qy;
        INT64 lo = s->bx - r, hi = s->bx + r;

        if (s->kind == ARCH && py > s->ay + grow)
            return 0;
        if (lo < 0)
            lo = 0;
        return d2 >= lo * lo && d2 <= hi * hi;
    }
}

/* How many of pixel (x, y)'s sample points lie inside any of the shapes.
   Every sample lies within SAMPLE_REACH of the pixel's centre, so a centre
   inside a shape shrunk by SAMPLE_REACH means all of them do, and a centre
   outside every shape grown by SAMPLE_REACH means none does; only the
   pixels between are sampled. */
static INT64 coverage(const SHAPE *shapes, int count, INT64 x, INT64 y)
{
    INT64 cx = x * SUBPIXEL + SUBPIXEL / 2, cy = y * SUBPIXEL + SUBPIXEL / 2;
    INT64 sx, sy, hits = 0;
    int i, near = 0;

    for (i = 0; i < count; i++) {
        if (inside(&shapes[i], cx, cy, -SAMPLE_REACH))
            return COVERAGE_MAX;
        if (inside(&shapes[i], cx, cy, SAMPLE_REACH))
            near = 1;
    }
    if (!near)
        return 0;
    for (sy = 0; sy < SAMPLES; sy++)
        for (sx = 0; sx < SAMPLES; sx++) {
            INT64 px = x * SUBPIXEL + sx * (SUBPIXEL / SAMPLES) + 1;
            INT64 py = y * SUBPIXEL + sy * (SUBPIXEL / SAMPLES) + 1;

            for (i = 0; i < count; i++)
                if (inside(&shapes[i], px, py, 0)) {
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

    fill_light();
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

    fill_light();
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
