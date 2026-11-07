/* The rasterizer (raster.h). A stroke's inside tests compare squared
   distances, so no square root is taken, and only pixels near a stroke's
   edge are sampled. Mixing decodes sRGB codes to light, mixes the light
   and encodes the result. */

#include "raster.h"

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
   between them, where encoding rounds up. The functions that draw or mix
   compute it on first use, in integers, so it follows from the formula
   above with no generator in the build and no floating point in either
   image. */
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

/* The code whose light mixes the light of codes from and to, to's
   weighted by weight of total and from's by the rest. */
static INT64 blend(INT64 from, INT64 to, INT64 weight, INT64 total)
{
    return encode((light[2 * from] * (UINT64)(total - weight) +
                   light[2 * to] * (UINT64)weight) /
                  (UINT64)total);
}

INT64 gfx_cross_fade(INT64 from, INT64 to, INT64 weight, INT64 total)
{
    fill_light();
    return blend(from, to, weight, total);
}


/* ------------------------------------------------------------------------
   Strokes */

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

/* Every sample lies within SAMPLE_REACH of the pixel's centre, so a
   centre inside a shape shrunk by SAMPLE_REACH means all of them do, and
   a centre outside every shape grown by SAMPLE_REACH means none does;
   only the pixels between are sampled. */
INT64 gfx_coverage(const SHAPE *shapes, int count, INT64 x, INT64 y)
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
