/* Limb's shading and finish, ported from design/backgrounds/limb.py and
   common.py. The planet's edge is where the squared distance q to its
   centre equals R^2, and (q - R^2) / 2R stands in for the signed distance
   to it: it needs no square root and is exact at the edge. Each glow is
   an inverse-square (Lorentzian) falloff across the edge times another
   along it, both centred on the sun's column. Colour is carried in v16
   units, an sRGB code times 256, until the finish adds grain and an
   ordered dither and rounds to codes.

   The prototype's integers are unbounded and its // and >> round toward
   minus infinity. Here every value fits an INT64 for the sides limb_init
   accepts, and the two quotients that can be negative, the distance and
   the grain, go through floor_div. */

#include "limb.h"

#define ONE_BITS 16
#define ONE (1 << ONE_BITS) /* Q16 one */
#define Q8 256              /* Q8 units per pixel */
#define V16_BITS 8          /* v16 units per sRGB code: 1 << V16_BITS */
#define V16 (1 << V16_BITS)
#define CODE_MAX 255
#define CHANNELS 3
#define PERCENT 100

/* The finish. R2X and R2Y are 1/g and 1/g^2 for g the plastic number,
   scaled to 2^32: the top byte of frac(x / g + y / g^2) is an ordered
   dither threshold, R2_SHIFT bits down. The grain is triangular noise of
   peak GRAIN v16 units, seeded with "nova" in ASCII: two bytes of a hash
   summed, less their mean, lie in -TRIANGLE_MEAN..TRIANGLE_MEAN, and
   TRIANGLE_SCALE maps that to about -1..1. */
#define R2X 3242174889u
#define R2Y 2447445414u
#define R2_SHIFT 24
#define GRAIN 160
#define GRAIN_SEED 0x6E6F7661u
#define BYTE_MASK 0xFFu
#define BYTE_BITS 8
#define TRIANGLE_MEAN 255
#define TRIANGLE_SCALE 256

/* Peak colours, in v16 units, red first. The ground is the planet's own
   faint colour, under the glows on its night side. */
static const INT64 rim_colour[CHANNELS] = {240 * V16, 232 * V16, 218 * V16};
static const INT64 band_colour[CHANNELS] = {34 * V16, 78 * V16, 140 * V16};
static const INT64 warm_colour[CHANNELS] = {205 * V16, 148 * V16, 72 * V16};
static const INT64 haze_colour[CHANNELS] = {6 * V16, 14 * V16, 30 * V16};
static const INT64 night_colour[CHANNELS] = {10 * V16, 20 * V16, 36 * V16};
static const INT64 ground_colour[CHANNELS] = {2 * V16, 3 * V16, 5 * V16};
static const INT64 sky_colour[CHANNELS] = {0, 0, 0};

/* ------------------------------------------------------------------------
   Integer primitives, as common.py defines them. */

/* a / b rounded toward minus infinity, for b > 0, as Python's // and >>
   round; C's / rounds toward zero. */
static INT64 floor_div(INT64 a, INT64 b)
{
    INT64 q = a / b;

    return q * b > a ? q - 1 : q;
}

/* lowbias32 (Chris Wellons): a 32-bit integer avalanche hash. Unsigned
   arithmetic wraps at 32 bits, as the prototype's masks do. */
static UINT32 hash32(UINT32 x)
{
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

static UINT32 hash3(UINT32 a, UINT32 b, UINT32 c)
{
    return hash32(a * 0x8DA6B343u ^ b * 0xD8163841u ^ c * 0xCB1AB31Fu);
}

/* w2 / (w2 + d^2) in Q16, the inverse-square falloff at distance d from
   a glow of squared width w2 > 0. */
static INT64 lorentz(INT64 d, INT64 w2)
{
    return w2 * ONE / (w2 + d * d);
}

/* A glow's weight in Q16: its falloff across the edge, at distance d with
   squared width across, scaled by its weight along the edge. */
static INT64 glow(INT64 d, INT64 across, INT64 along)
{
    return lorentz(d, across) * along >> ONE_BITS;
}

/* A v16 value with its grain and threshold added, as an sRGB code. */
static UINT32 quantise(INT64 v)
{
    if (v < 0)
        return 0;
    if (v >> V16_BITS > CODE_MAX)
        return CODE_MAX;
    return (UINT32)(v >> V16_BITS);
}

/* ------------------------------------------------------------------------
   Geometry. */

/* The squared width, in Q8 pixels, of a glow 1 / fraction of the height
   across the edge. */
static INT64 across(INT64 height, INT64 fraction)
{
    INT64 w = height * Q8 / fraction;

    return w * w;
}

/* The squared width, in pixels, of a glow percent of the width along the
   edge. */
static INT64 along(INT64 width, INT64 percent)
{
    INT64 w = width * percent / PERCENT;

    return w * w;
}

int limb_init(LIMB *limb, INT64 width, INT64 height)
{
    if (width < LIMB_MIN_SIDE || height < LIMB_MIN_SIDE ||
        width > LIMB_MAX_SIDE || height > LIMB_MAX_SIDE)
        return 0;
    /* A circle of radius four heights, its crest low and right of centre. */
    limb->radius = 4 * height;
    limb->cx = width * 68 / PERCENT;
    limb->cy = height * 78 / PERCENT + limb->radius;
    limb->sun_x = width * 57 / PERCENT;
    /* Widths across the edge, sky side and planet side. */
    limb->rim_out = across(height, 900);
    limb->rim_in = across(height, 2000);
    limb->band_out = across(height, 38);
    limb->band_in = across(height, 400);
    limb->warm_out = across(height, 120);
    limb->warm_in = across(height, 1200);
    limb->haze_out = across(height, 4);
    limb->night_in = across(height, 30);
    /* Widths along the edge. */
    limb->along_rim = along(width, 20);
    limb->along_band = along(width, 34);
    limb->along_warm = along(width, 9);
    limb->along_haze = along(width, 70);
    return 1;
}

/* ------------------------------------------------------------------------
   Shading and finish. */

UINT32 limb_pixel(const LIMB *limb, INT64 x, INT64 y)
{
    INT64 dx = x - limb->cx, dy = y - limb->cy, t = x - limb->sun_x;
    INT64 d = floor_div((dx * dx + dy * dy - limb->radius * limb->radius) * Q8,
                        2 * limb->radius);
    INT64 a_rim = lorentz(t, limb->along_rim);
    INT64 a_band = lorentz(t, limb->along_band);
    INT64 a_warm = lorentz(t, limb->along_warm);
    INT64 a_haze = lorentz(t, limb->along_haze);
    INT64 rim, band, warm, outer, threshold, grain;
    const INT64 *base, *outer_colour;
    UINT32 h, pixel = 0;
    int k;

    if (d >= 0) {
        rim = glow(d, limb->rim_out, a_rim);
        band = glow(d, limb->band_out, a_band);
        warm = glow(d, limb->warm_out, a_warm);
        outer = glow(d, limb->haze_out, a_haze);
        base = sky_colour;
        outer_colour = haze_colour;
    } else {
        rim = glow(d, limb->rim_in, a_rim);
        band = glow(d, limb->band_in, a_band);
        warm = glow(d, limb->warm_in, a_warm);
        outer = glow(d, limb->night_in, a_haze);
        base = ground_colour;
        outer_colour = night_colour;
    }

    threshold = ((UINT32)x * R2X + (UINT32)y * R2Y) >> R2_SHIFT;
    h = hash3((UINT32)x, (UINT32)y, GRAIN_SEED);
    grain = floor_div(((INT64)(h & BYTE_MASK) +
                       (INT64)(h >> BYTE_BITS & BYTE_MASK) - TRIANGLE_MEAN) *
                          GRAIN,
                      TRIANGLE_SCALE);
    for (k = 0; k < CHANNELS; k++) {
        INT64 v = base[k] + ((rim_colour[k] * rim + band_colour[k] * band +
                              warm_colour[k] * warm +
                              outer_colour[k] * outer) >>
                             ONE_BITS);

        pixel = pixel << BYTE_BITS | quantise(v + grain + threshold);
    }
    return pixel;
}
