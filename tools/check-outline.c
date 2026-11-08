/* Checks on the host the coverage lib/outline.c gives each pixel
   (outline_coverage, docs/spec/ui.md U2 rule 6) and that outline_fill
   mixes by it: against the exact area of rectangles, of rectangles
   that overlap with the same winding and with opposite winding, and of
   triangles, each worked out here from the geometry alone; against
   sampled geometry for rounded rectangles and circles; that the four
   corners of a rounded rectangle agree, that a plane narrower than the
   shape is the same pixels clipped, that a shape moved a whole pixel
   moves its coverage, that a shape that grows never loses coverage, that
   every byte of a plane is written and a refused call writes none, and
   that outline_fill's pixels are gfx_cross_fade's mix by the coverage
   the plane holds. Prints each failure and a count, and exits nonzero if
   any check fails.

   cc -std=c99 -Wall -Wextra -pedantic -I include -I lib -I gfx \
     -o check-outline tools/check-outline.c lib/outline.c gfx/raster.c &&
     ./check-outline */

#include <stdio.h>

#include "outline.h"
#include "raster.h"

/* A pixel's area in units, which a pixel's coverage is the share of. */
#define FULL_AREA (OUTLINE_ONE * OUTLINE_ONE)

/* The widest and tallest plane a check makes, in pixels. */
#define SIDE_MAX 96
#define PLANE_BYTES (SIDE_MAX * SIDE_MAX)

/* A plane's byte before a call writes it, which no coverage equals: a
   byte left as it is shows a pixel the call did not write. */
#define UNWRITTEN 0xAA

/* The bytes before and after a plane that a check sets to UNWRITTEN and
   shows a call leaves alone: the most a row's width, SIDE_MAX, a call
   could write above or below a plane by a row. */
#define GUARD_BYTES 256

/* How many random shapes each check draws. */
#define CASES 400

/* How many failures are printed; every one is counted. */
#define FAILURES_PRINTED 20

/* The most a triangle's pixel may differ from its exact area, in
   coverage codes. A line is cut where it crosses a pixel row's slices at
   its x rounded to the nearest 256th (ui.md U2 rule 3), which moves the
   edge by at most a 512th of a pixel across the pixel's height: under
   one code of 255 for each of a pixel's two edges, and the area's own
   rounding adds half. */
#define POLYGON_TOLERANCE 2

/* The most a curved shape's pixel may differ from the shape's area
   sampled at CURVE_SAMPLES points a side, in coverage codes. A curve's
   lines stray from it by at most a sixteenth of a pixel (ui.md U2
   rule 2) and a pixel's edge within it is at most a diagonal long, which
   is 0.0884 of a pixel's area; sampling is off by at most a perimeter's
   worth of samples, 1.5 sides of CURVE_SAMPLES of its square, 0.0234: in
   all 0.1118 of 255, which is 28.5 codes. */
#define CURVE_TOLERANCE 29
#define CURVE_SAMPLES 64

/* The most a pixel of a rounded rectangle may differ from its mirror
   image across or down, in coverage codes: check_corners gives the
   reason. */
#define MIRROR_TOLERANCE 1

/* Radii, in half pixels, above which a curved shape is sampled once, for
   the time sampling every pixel of a large one takes. */
#define CURVE_LARGE 30

static OUTLINE outline;
static UINT8 plane_store[GUARD_BYTES + PLANE_BYTES + GUARD_BYTES];
static UINT8 reference_store[GUARD_BYTES + PLANE_BYTES + GUARD_BYTES];
static UINT8 clipped_store[GUARD_BYTES + PLANE_BYTES + GUARD_BYTES];
static UINT8 *const plane = plane_store + GUARD_BYTES;
static UINT8 *const reference = reference_store + GUARD_BYTES;
static UINT8 *const clipped = clipped_store + GUARD_BYTES;
static UINT32 canvas[PLANE_BYTES];
static UINT32 expected_canvas[PLANE_BYTES];
static UINT8 wide_store[GUARD_BYTES + (OUTLINE_WIDTH_MAX + 1) * 2 +
                        GUARD_BYTES];
static UINT8 *const wide_plane = wide_store + GUARD_BYTES;

static unsigned checks;
static unsigned failures;

static void check(int passed, const char *what)
{
    checks++;
    if (passed)
        return;
    if (failures < FAILURES_PRINTED)
        printf("failed: %s\n", what);
    failures++;
}

static unsigned long long random_state = 0x2545F4914F6CDD1Dull;

/* A number from low to high, both included, from a fixed sequence. */
static INT64 between(INT64 low, INT64 high)
{
    random_state ^= random_state >> 12;
    random_state ^= random_state << 25;
    random_state ^= random_state >> 27;
    return low + (INT64)((random_state * 0x9E3779B97F4A7C15ull >> 11) %
                         (unsigned long long)(high - low + 1));
}

/* ------------------------------------------------------------------------
   What coverage is, from the geometry alone. */

/* The coverage of a pixel an outline covers area units of, whole being
   FULL_AREA: area times OUTLINE_COVERAGE_MAX over FULL_AREA, rounded to
   the nearest, halves up (ui.md U2 rule 3). */
static INT64 coverage_of(INT64 area, INT64 whole)
{
    return (2 * area * OUTLINE_COVERAGE_MAX + whole) / (2 * whole);
}

/* The length two spans share. */
static INT64 overlap(INT64 a0, INT64 a1, INT64 b0, INT64 b1)
{
    INT64 low = a0 > b0 ? a0 : b0;
    INT64 high = a1 < b1 ? a1 : b1;

    return high > low ? high - low : 0;
}

/* A rectangle, in units. */
typedef struct {
    INT64 left, top, right, bottom;
} BOX;

/* The area of the pixel at column x and row y that box covers. */
static INT64 box_area(const BOX *box, INT64 x, INT64 y)
{
    return overlap(box->left, box->right, x * OUTLINE_ONE,
                   (x + 1) * OUTLINE_ONE) *
           overlap(box->top, box->bottom, y * OUTLINE_ONE,
                   (y + 1) * OUTLINE_ONE);
}

static BOX box_between(const BOX *a, const BOX *b)
{
    BOX box;

    box.left = a->left > b->left ? a->left : b->left;
    box.top = a->top > b->top ? a->top : b->top;
    box.right = a->right < b->right ? a->right : b->right;
    box.bottom = a->bottom < b->bottom ? a->bottom : b->bottom;
    return box;
}

/* A box, in units, up to margin pixels outside a width x height plane. */
static BOX random_box(INT64 width, INT64 height, INT64 margin)
{
    BOX box;

    box.left = between(-margin * OUTLINE_ONE, (width + margin) * OUTLINE_ONE);
    box.top = between(-margin * OUTLINE_ONE, (height + margin) * OUTLINE_ONE);
    box.right = box.left + between(0, (width + 2 * margin) * OUTLINE_ONE);
    box.bottom = box.top + between(0, (height + 2 * margin) * OUTLINE_ONE);
    return box;
}

static void rectangle(const BOX *box, int clockwise)
{
    outline_move(&outline, box->left, box->top);
    if (clockwise) {
        outline_line(&outline, box->right, box->top);
        outline_line(&outline, box->right, box->bottom);
        outline_line(&outline, box->left, box->bottom);
    } else {
        outline_line(&outline, box->left, box->bottom);
        outline_line(&outline, box->right, box->bottom);
        outline_line(&outline, box->right, box->top);
    }
    outline_close(&outline);
}

static void set_bytes(UINT8 *bytes, INT64 count, UINT8 value)
{
    INT64 i;

    for (i = 0; i < count; i++)
        bytes[i] = value;
}

static int same_bytes(const UINT8 *a, const UINT8 *b, INT64 count)
{
    INT64 i;

    for (i = 0; i < count; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

/* The coverage of the outline built so far in a width x height plane,
   or 0 if the call refused. The bytes around the plane are shown to be
   left as they were. */
static int cover(UINT8 *bytes, INT64 width, INT64 height)
{
    INT64 size = width * height, i;
    int accepted, kept = 1;

    set_bytes(bytes - GUARD_BYTES, size + 2 * GUARD_BYTES, UNWRITTEN);
    accepted = outline_coverage(&outline, bytes, width, height);
    for (i = -GUARD_BYTES; i < 0; i++)
        kept = kept && bytes[i] == UNWRITTEN;
    for (i = size; i < size + GUARD_BYTES; i++)
        kept = kept && bytes[i] == UNWRITTEN;
    check(kept, "a call writes no byte outside its plane");
    return accepted;
}

/* Rectangles have edges on whole units, so nothing is rounded but the
   area's share: their coverage is exactly the area's. */
static void check_rectangles(void)
{
    BOX half = {0, 0, OUTLINE_ONE / 2, OUTLINE_ONE};
    int trial;

    outline_start(&outline);
    rectangle(&half, 1);
    check(cover(plane, 1, 1) == 1 && plane[0] == 128,
          "half a pixel is 128: 127.5 rounds up");
    for (trial = 0; trial < CASES; trial++) {
        INT64 width = between(1, 40), height = between(1, 40), x, y, bad = 0;
        BOX box = random_box(width, height, 3);

        outline_start(&outline);
        rectangle(&box, (int)between(0, 1));
        if (!cover(plane, width, height)) {
            check(0, "a rectangle's coverage is written");
            continue;
        }
        for (y = 0; y < height; y++)
            for (x = 0; x < width; x++)
                if (plane[y * width + x] !=
                    coverage_of(box_area(&box, x, y), FULL_AREA))
                    bad++;
        check(bad == 0, "a rectangle's coverage is its exact area");
    }
}

/* The nonzero winding rule: where two contours wind the same way the
   area covered is their union's, and where they wind opposite ways the
   overlap is a hole. */
static void check_winding(void)
{
    int trial;

    for (trial = 0; trial < CASES; trial++) {
        INT64 width = between(1, 40), height = between(1, 40), x, y;
        INT64 union_bad = 0, hole_bad = 0;
        BOX a = random_box(width, height, 2), b = random_box(width, height, 2);
        BOX both = box_between(&a, &b);

        outline_start(&outline);
        rectangle(&a, 1);
        rectangle(&b, 1);
        cover(plane, width, height);
        outline_start(&outline);
        rectangle(&a, 1);
        rectangle(&b, 0);
        cover(reference, width, height);
        for (y = 0; y < height; y++)
            for (x = 0; x < width; x++) {
                INT64 area_a = box_area(&a, x, y), area_b = box_area(&b, x, y);
                INT64 area_both = box_area(&both, x, y);

                if (plane[y * width + x] !=
                    coverage_of(area_a + area_b - area_both, FULL_AREA))
                    union_bad++;
                if (reference[y * width + x] !=
                    coverage_of(area_a + area_b - 2 * area_both, FULL_AREA))
                    hole_bad++;
            }
        check(union_bad == 0,
              "overlapping contours of one winding cover their union");
        check(hole_bad == 0,
              "overlapping contours of opposite winding leave a hole");
    }
}

/* A point, in pixels. */
typedef struct {
    double x, y;
} POINT;

#define POLYGON_POINTS_MAX 16

/* The part of polygon in that lies on one side of the line at bound on
   one axis, into out, by Sutherland and Hodgman's rule. Returns how many
   points out holds. */
static int clip(const POINT *in, int count, POINT *out, int vertical,
                double bound, int keep_above)
{
    int i, kept = 0;

    for (i = 0; i < count; i++) {
        POINT now = in[i], before = in[(i + count - 1) % count];
        double now_at = vertical ? now.x : now.y;
        double before_at = vertical ? before.x : before.y;
        int now_in = keep_above ? now_at >= bound : now_at <= bound;
        int before_in = keep_above ? before_at >= bound : before_at <= bound;

        if (now_in != before_in) {
            double t = (bound - before_at) / (now_at - before_at);

            out[kept].x = before.x + t * (now.x - before.x);
            out[kept].y = before.y + t * (now.y - before.y);
            kept++;
        }
        if (now_in)
            out[kept++] = now;
    }
    return kept;
}

/* The area, in pixels, of a polygon of count points that lies in the
   pixel at column x and row y. */
static double area_in_pixel(const POINT *polygon, int count, INT64 x,
                            INT64 y)
{
    POINT a[POLYGON_POINTS_MAX], b[POLYGON_POINTS_MAX];
    double twice = 0;
    int i, n;

    for (i = 0; i < count; i++)
        a[i] = polygon[i];
    n = clip(a, count, b, 1, (double)x, 1);
    n = n ? clip(b, n, a, 1, (double)(x + 1), 0) : 0;
    n = n ? clip(a, n, b, 0, (double)y, 1) : 0;
    n = n ? clip(b, n, a, 0, (double)(y + 1), 0) : 0;
    for (i = 0; i < n; i++) {
        const POINT *p = &a[i], *q = &a[(i + 1) % n];

        twice += p->x * q->y - q->x * p->y;
    }
    return (twice < 0 ? -twice : twice) / 2;
}

/* Triangles have slanted lines, which are cut at each pixel row's slices
   at a x rounded to a 256th: their coverage is their exact area's to
   within POLYGON_TOLERANCE. */
static void check_triangles(void)
{
    INT64 worst = 0;
    int trial;

    for (trial = 0; trial < CASES; trial++) {
        INT64 width = between(1, 40), height = between(1, 40), x, y, bad = 0;
        INT64 px[3], py[3];
        POINT triangle[3];
        int i;

        outline_start(&outline);
        for (i = 0; i < 3; i++) {
            px[i] = between(-2 * OUTLINE_ONE, (width + 2) * OUTLINE_ONE);
            py[i] = between(-2 * OUTLINE_ONE, (height + 2) * OUTLINE_ONE);
            triangle[i].x = (double)px[i] / OUTLINE_ONE;
            triangle[i].y = (double)py[i] / OUTLINE_ONE;
            if (i == 0)
                outline_move(&outline, px[i], py[i]);
            else
                outline_line(&outline, px[i], py[i]);
        }
        if (!cover(plane, width, height)) {
            check(0, "a triangle's coverage is written");
            continue;
        }
        for (y = 0; y < height; y++)
            for (x = 0; x < width; x++) {
                double exact = area_in_pixel(triangle, 3, x, y);
                INT64 want = (INT64)(exact * OUTLINE_COVERAGE_MAX + 0.5);
                INT64 gap = plane[y * width + x] - want;

                gap = gap < 0 ? -gap : gap;
                worst = gap > worst ? gap : worst;
                if (gap > POLYGON_TOLERANCE)
                    bad++;
            }
        check(bad == 0, "a triangle's coverage is its exact area's, nearly");
    }
    printf("triangles: worst distance from the exact area %lld code(s)\n",
           worst);
}

/* ------------------------------------------------------------------------
   Rounded rectangles and circles, against sampled geometry. */

/* Whether the point (x, y) lies in the rounded rectangle from (left, top)
   to (right, bottom) with corners of radius radius, in one unit: its
   distance from the rectangle shrunk by radius is at most radius. */
static int in_rounded_rect(INT64 x, INT64 y, INT64 left, INT64 top,
                           INT64 right, INT64 bottom, INT64 radius)
{
    INT64 cx = x < left + radius ? left + radius
                                 : (x > right - radius ? right - radius : x);
    INT64 cy = y < top + radius ? top + radius
                                : (y > bottom - radius ? bottom - radius : y);

    if (x < left || x > right || y < top || y > bottom)
        return 0;
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= radius * radius;
}

/* A rounded rectangle, in half pixels: its sides and its corners' radius. */
typedef struct {
    INT64 left, top, right, bottom, radius;
} ROUND;

static void rounded(const ROUND *r)
{
    outline_rounded_rect(&outline, r->left * OUTLINE_ONE / 2,
                         r->top * OUTLINE_ONE / 2,
                         r->right * OUTLINE_ONE / 2,
                         r->bottom * OUTLINE_ONE / 2,
                         r->radius * OUTLINE_ONE / 2);
}

/* The coverage of the pixel at column x and row y that the rounded
   rectangle has, by CURVE_SAMPLES points a side: a sample at the middle
   of each cell, in units of a 128th of a pixel, so that none lies on an
   edge, which are on even units. */
static INT64 sampled(const ROUND *r, INT64 x, INT64 y)
{
    const INT64 unit = 2 * CURVE_SAMPLES;
    INT64 inside = 0, i, j;

    for (j = 0; j < CURVE_SAMPLES; j++)
        for (i = 0; i < CURVE_SAMPLES; i++)
            inside += in_rounded_rect(
                x * unit + 2 * i + 1, y * unit + 2 * j + 1,
                r->left * unit / 2, r->top * unit / 2, r->right * unit / 2,
                r->bottom * unit / 2, r->radius * unit / 2);
    return coverage_of(inside, CURVE_SAMPLES * CURVE_SAMPLES);
}

static void check_curves(void)
{
    static const INT64 radii[] = {2, 3, 5, 8, 13, 20, 21, 33, 45, 79};
    INT64 worst = 0;
    unsigned k;

    for (k = 0; k < sizeof radii / sizeof radii[0]; k++) {
        int trial, trials = radii[k] > CURVE_LARGE ? 1 : 4;

        for (trial = 0; trial < trials; trial++) {
            ROUND r;
            INT64 width, height, x, y, bad = 0;

            /* A side of whole pixels, so that the radius, in half pixels,
               is at most half of it. */
            r.radius = radii[k];
            r.left = between(0, 4);
            r.top = between(0, 4);
            r.right = r.left + 2 * between(r.radius, r.radius + 8);
            r.bottom = r.top + 2 * between(r.radius, r.radius + 8);
            width = (r.right + 1) / 2 + 2;
            height = (r.bottom + 1) / 2 + 2;
            outline_start(&outline);
            rounded(&r);
            if (!cover(plane, width, height)) {
                check(0, "a rounded rectangle's coverage is written");
                continue;
            }
            for (y = 0; y < height; y++)
                for (x = 0; x < width; x++) {
                    INT64 gap = plane[y * width + x] - sampled(&r, x, y);

                    gap = gap < 0 ? -gap : gap;
                    worst = gap > worst ? gap : worst;
                    if (gap > CURVE_TOLERANCE)
                        bad++;
                }
            check(bad == 0,
                  "a rounded rectangle's coverage is its sampled area's");
        }
    }
    printf("rounded rectangles: worst distance from sampled area %lld "
           "code(s)\n",
           worst);
}

/* The pixel whole inside a rectangle is 255, whole outside is 0, and a
   rounded rectangle's far corner pixel, which lies outside its arc, is 0;
   every byte of the plane is written. */
static void check_ends(void)
{
    BOX box = {3 * OUTLINE_ONE, 2 * OUTLINE_ONE, 9 * OUTLINE_ONE,
               7 * OUTLINE_ONE};
    INT64 x, y, bad = 0, unwritten = 0;
    INT64 radius;

    outline_start(&outline);
    rectangle(&box, 1);
    cover(plane, 12, 10);
    for (y = 0; y < 10; y++)
        for (x = 0; x < 12; x++) {
            int in = x >= 3 && x < 9 && y >= 2 && y < 7;

            if (plane[y * 12 + x] != (in ? OUTLINE_COVERAGE_MAX : 0))
                bad++;
            if (plane[y * 12 + x] == UNWRITTEN)
                unwritten++;
        }
    check(bad == 0, "a pixel whole inside is 255 and whole outside is 0");
    check(unwritten == 0, "every byte of the plane is written");

    for (radius = 4; radius <= 40; radius++) {
        INT64 size = 2 * radius + 3;
        INT64 c = size - 1, mid = size / 2;
        ROUND r;

        r.left = 0;
        r.top = 0;
        r.right = 2 * size;
        r.bottom = 2 * size;
        r.radius = 2 * radius;
        outline_start(&outline);
        rounded(&r);
        cover(plane, size, size);
        check(plane[0] == 0 && plane[c] == 0 && plane[c * size] == 0 &&
                  plane[c * size + c] == 0,
              "a rounded rectangle's four corner pixels are 0");
        check(plane[mid * size] == OUTLINE_COVERAGE_MAX &&
                  plane[mid * size + c] == OUTLINE_COVERAGE_MAX &&
                  plane[mid] == OUTLINE_COVERAGE_MAX &&
                  plane[c * size + mid] == OUTLINE_COVERAGE_MAX,
              "a rounded rectangle's edge pixels midway are 255");
    }
}

/* An empty outline covers nothing, and a plane of no pixels is written
   as no bytes. */
static void check_empty(void)
{
    outline_start(&outline);
    check(cover(plane, 5, 4) == 1, "an empty outline's coverage is written");
    set_bytes(reference, 20, 0);
    check(same_bytes(plane, reference, 20), "an empty outline covers 0");
    check(cover(plane, 0, 5) == 1 && cover(plane, 5, 0) == 1,
          "a plane of no pixels is accepted");
    outline_start(&outline);
    outline_move(&outline, 0, 0);
    outline_line(&outline, OUTLINE_ONE, OUTLINE_ONE);
    outline_close(&outline);
    check(cover(plane, 3, 3) == 1, "a contour of no area is accepted");
    set_bytes(reference, 9, 0);
    check(same_bytes(plane, reference, 9), "a contour of no area covers 0");
}

/* The four corners of a rounded rectangle do not repeat each other
   exactly: each corner's curve is rounded to 256ths from its own points,
   and the roundings do not mirror, which moves an edge by a 256th of a
   pixel, under one code of coverage. They agree to within
   MIRROR_TOLERANCE, and the radius by radius plane of one corner is
   exactly the same pixels of the whole. */
static void check_corners(void)
{
    INT64 radius, worst_across = 0, worst_down = 0, pairs = 0, differing = 0;

    for (radius = 2; radius <= 80; radius++) {
        int extra;

        for (extra = 0; extra < 3; extra++) {
            INT64 width = (radius + 1) / 2 * 2 + 2 + extra * 5;
            INT64 height = (radius + 1) / 2 * 2 + 2 + extra * 3;
            INT64 corner = (radius + 1) / 2, x, y, bad = 0;
            ROUND r;

            if (width > SIDE_MAX || height > SIDE_MAX)
                continue;
            r.left = 0;
            r.top = 0;
            r.right = 2 * width;
            r.bottom = 2 * height;
            r.radius = radius;
            outline_start(&outline);
            rounded(&r);
            cover(plane, width, height);
            for (y = 0; y < height; y++)
                for (x = 0; x < width; x++) {
                    INT64 across = plane[y * width + x] -
                                   plane[y * width + (width - 1 - x)];
                    INT64 down = plane[y * width + x] -
                                 plane[(height - 1 - y) * width + x];

                    across = across < 0 ? -across : across;
                    down = down < 0 ? -down : down;
                    if (across > worst_across)
                        worst_across = across;
                    if (down > worst_down)
                        worst_down = down;
                    pairs += 2;
                    differing += (across != 0) + (down != 0);
                }
            outline_start(&outline);
            rounded(&r);
            cover(clipped, corner, corner);
            for (y = 0; y < corner; y++)
                for (x = 0; x < corner; x++)
                    if (clipped[y * corner + x] != plane[y * width + x])
                        bad++;
            check(bad == 0,
                  "a corner's plane is the same pixels of the whole");
        }
    }
    check(worst_across <= MIRROR_TOLERANCE && worst_down <= MIRROR_TOLERANCE,
          "a rounded rectangle's corners mirror each other to a code");
    printf("corners: worst mirrored difference %lld across, %lld down; "
           "%lld of %lld mirrored pixel pairs differ\n",
           worst_across, worst_down, differing, pairs);
}

/* ------------------------------------------------------------------------
   Shapes of every kind, for the properties that hold of any. */

/* The kinds of figure a check draws. */
enum {
    FIGURE_RECTANGLE,
    FIGURE_TRIANGLE,
    FIGURE_ROUNDED,
    FIGURE_CIRCLE,
    FIGURE_TWO_RECTANGLES,
    FIGURE_CURVES,
    FIGURE_KINDS
};

/* How many cubic curves a FIGURE_CURVES figure has. */
#define FIGURE_CURVES_COUNT 3

/* A figure, by what builds it, so that one can be built again moved. */
typedef struct {
    int kind;
    int clockwise;
    BOX box;
    BOX other;
    INT64 radius;
    INT64 curve[FIGURE_CURVES_COUNT][6];
} FIGURE;

/* A random coordinate in or near a width x height plane, in units. */
static INT64 near_plane(INT64 extent)
{
    return between(-3 * OUTLINE_ONE, (extent + 3) * OUTLINE_ONE);
}

static FIGURE random_figure(INT64 width, INT64 height)
{
    FIGURE figure;
    INT64 span, high, i, k;

    figure.kind = (int)between(0, FIGURE_KINDS - 1);
    figure.clockwise = (int)between(0, 1);
    figure.box = random_box(width, height, 3);
    figure.other = random_box(width, height, 3);
    span = figure.box.right - figure.box.left;
    high = figure.box.bottom - figure.box.top;
    figure.radius = between(0, (span < high ? span : high) / 2);
    if (figure.kind == FIGURE_CIRCLE)
        figure.radius = between(0, 12 * OUTLINE_ONE);
    for (i = 0; i < FIGURE_CURVES_COUNT; i++)
        for (k = 0; k < 6; k += 2) {
            figure.curve[i][k] = near_plane(width);
            figure.curve[i][k + 1] = near_plane(height);
        }
    return figure;
}

/* A box moved by (dx, dy) units. */
static BOX moved_box(const BOX *box, INT64 dx, INT64 dy)
{
    BOX moved;

    moved.left = box->left + dx;
    moved.top = box->top + dy;
    moved.right = box->right + dx;
    moved.bottom = box->bottom + dy;
    return moved;
}

/* Build figure, moved by (dx, dy) units, as the outline's only path. */
static void build_figure(const FIGURE *figure, INT64 dx, INT64 dy)
{
    BOX box = moved_box(&figure->box, dx, dy);
    BOX other = moved_box(&figure->other, dx, dy);
    int i;

    outline_start(&outline);
    switch (figure->kind) {
    case FIGURE_RECTANGLE:
        rectangle(&box, figure->clockwise);
        break;
    case FIGURE_TRIANGLE:
        outline_move(&outline, box.left, box.top);
        outline_line(&outline, box.right, box.bottom);
        outline_line(&outline, other.left, other.bottom);
        outline_close(&outline);
        break;
    case FIGURE_ROUNDED:
        outline_rounded_rect(&outline, box.left, box.top, box.right,
                             box.bottom, figure->radius);
        break;
    case FIGURE_CIRCLE:
        outline_circle(&outline, box.left, box.top, figure->radius);
        break;
    case FIGURE_TWO_RECTANGLES:
        rectangle(&box, 1);
        rectangle(&other, figure->clockwise);
        break;
    default:
        outline_move(&outline, box.left, box.top);
        for (i = 0; i < FIGURE_CURVES_COUNT; i++)
            outline_cubic(&outline, figure->curve[i][0] + dx,
                          figure->curve[i][1] + dy, figure->curve[i][2] + dx,
                          figure->curve[i][3] + dy, figure->curve[i][4] + dx,
                          figure->curve[i][5] + dy);
        outline_close(&outline);
        break;
    }
}

/* A smaller plane holds the same pixels of a shape as a larger one:
   clipping cuts, it does not rescale. The same outline gives the same
   plane when asked again. */
static void check_clipping(void)
{
    int trial;

    for (trial = 0; trial < CASES; trial++) {
        INT64 width = between(8, 60), height = between(8, 60);
        INT64 narrow = between(1, width), shorter = between(1, height);
        INT64 x, y, bad = 0;
        FIGURE figure = random_figure(width, height);

        build_figure(&figure, 0, 0);
        cover(plane, width, height);
        cover(reference, width, height);
        check(same_bytes(plane, reference, width * height),
              "the same outline gives the same plane twice");
        cover(clipped, narrow, shorter);
        for (y = 0; y < shorter; y++)
            for (x = 0; x < narrow; x++)
                if (clipped[y * narrow + x] != plane[y * width + x])
                    bad++;
        check(bad == 0, "a smaller plane holds the same pixels, clipped");
    }
}

/* A shape moved by whole pixels has its coverage moved by as many. */
static void check_translation(void)
{
    int trial;

    for (trial = 0; trial < CASES; trial++) {
        INT64 side = 40, dx = between(-6, 6), dy = between(-6, 6);
        INT64 x, y, bad = 0;
        FIGURE figure = random_figure(side, side);

        build_figure(&figure, 0, 0);
        cover(plane, side, side);
        build_figure(&figure, dx * OUTLINE_ONE, dy * OUTLINE_ONE);
        cover(reference, side, side);
        for (y = 0; y < side; y++)
            for (x = 0; x < side; x++) {
                INT64 moved_x = x + dx, moved_y = y + dy;

                if (moved_x >= 0 && moved_x < side && moved_y >= 0 &&
                    moved_y < side &&
                    plane[y * side + x] !=
                        reference[moved_y * side + moved_x])
                    bad++;
            }
        check(bad == 0, "a shape moved whole pixels has its coverage moved");
    }
}

/* A circle that grows by a pixel, and a rectangle that grows, lose no
   pixel's coverage. */
static void check_growth(void)
{
    const INT64 side = 90, centre = 45 * OUTLINE_ONE;
    INT64 radius, i, bad_circles = 0, bad_boxes = 0;
    int trial;

    for (radius = 1; radius < 40; radius++) {
        outline_start(&outline);
        outline_circle(&outline, centre, centre, radius * OUTLINE_ONE);
        cover(plane, side, side);
        outline_start(&outline);
        outline_circle(&outline, centre, centre, (radius + 1) * OUTLINE_ONE);
        cover(reference, side, side);
        for (i = 0; i < side * side; i++)
            if (plane[i] > reference[i])
                bad_circles++;
    }
    check(bad_circles == 0, "a circle that grows by a pixel loses coverage");
    for (trial = 0; trial < CASES; trial++) {
        BOX small = random_box(30, 30, 0), large = small;

        large.left -= between(0, 2 * OUTLINE_ONE);
        large.top -= between(0, 2 * OUTLINE_ONE);
        large.right += between(0, 2 * OUTLINE_ONE);
        large.bottom += between(0, 2 * OUTLINE_ONE);
        outline_start(&outline);
        rectangle(&small, 1);
        cover(plane, 36, 36);
        outline_start(&outline);
        rectangle(&large, 1);
        cover(reference, 36, 36);
        for (i = 0; i < 36 * 36; i++)
            if (plane[i] > reference[i])
                bad_boxes++;
    }
    check(bad_boxes == 0, "a rectangle that grows loses coverage");
}

/* ------------------------------------------------------------------------
   Calls that are refused, and the widest plane. */

static void check_refusals(void)
{
    INT64 i, intact;

    /* A line with no contour open. */
    outline_start(&outline);
    outline_line(&outline, OUTLINE_ONE, OUTLINE_ONE);
    check(cover(plane, 4, 4) == 0, "a line with no contour open is refused");
    set_bytes(reference, 16, UNWRITTEN);
    check(same_bytes(plane, reference, 16), "a refused call writes nothing");

    /* A coordinate past the furthest. */
    outline_start(&outline);
    outline_move(&outline, OUTLINE_COORDINATE_MAX + 1, 0);
    check(cover(plane, 4, 4) == 0, "a coordinate too far is refused");
    check(same_bytes(plane, reference, 16),
          "a coordinate too far writes nothing");

    /* More lines than an outline holds. */
    outline_start(&outline);
    outline_move(&outline, 0, 0);
    for (i = 0; i <= OUTLINE_EDGES_MAX; i++)
        outline_line(&outline, i % 2 ? 0 : OUTLINE_ONE, i + 1);
    check(cover(plane, 4, 4) == 0,
          "more lines than an outline holds are refused");
    check(same_bytes(plane, reference, 16), "too many lines write nothing");

    /* A plane wider than the widest, and a negative size. */
    outline_start(&outline);
    outline_move(&outline, 0, 0);
    outline_line(&outline, OUTLINE_ONE, 0);
    outline_line(&outline, OUTLINE_ONE, OUTLINE_ONE);
    outline_close(&outline);
    set_bytes(wide_plane, (OUTLINE_WIDTH_MAX + 1) * 2, UNWRITTEN);
    check(outline_coverage(&outline, wide_plane, OUTLINE_WIDTH_MAX + 1,
                           2) == 0,
          "a plane wider than OUTLINE_WIDTH_MAX is refused");
    for (i = 0, intact = 1; i < (OUTLINE_WIDTH_MAX + 1) * 2; i++)
        intact = intact && wide_plane[i] == UNWRITTEN;
    check(intact, "a plane too wide is left as it was");
    set_bytes(plane, 16, UNWRITTEN);
    check(outline_coverage(&outline, plane, -1, 4) == 0 &&
              outline_coverage(&outline, plane, 4, -1) == 0,
          "a negative width or height is refused");
    check(same_bytes(plane, reference, 16),
          "a negative size writes nothing");

    /* A refused outline is refused by outline_fill too. */
    outline_start(&outline);
    outline_line(&outline, OUTLINE_ONE, OUTLINE_ONE);
    {
        CANVAS target;
        UINT32 pixels[16] = {0};

        target.pixels = pixels;
        target.width = 4;
        target.height = 4;
        target.stride = 4;
        check(outline_fill(&outline, &target, 0xFFFFFF,
                           OUTLINE_OPACITY_ONE) == 0,
              "outline_fill refuses what outline_coverage refuses");
    }
}

/* A long, shallow curve can leave a pixel wholly inside it a unit more
   than whole and one wholly outside it a unit less than none; coverage is
   held between 0 and OUTLINE_COVERAGE_MAX, where a byte would wrap to 0
   or to 255. Each curve here is one cubic thousands of pixels long and two
   high, on the widest plane, among the first found by searching random
   cubics that reach one end. */
static void check_drift(void)
{
    static UINT32 target_pixels[OUTLINE_WIDTH_MAX * 2];
    const INT64 width = OUTLINE_WIDTH_MAX, background = 0x102030;
    CANVAS target;
    INT64 i;

    target.pixels = target_pixels;
    target.width = width;
    target.height = 2;
    target.stride = width;

    outline_start(&outline);
    outline_move(&outline, 2089111, 676);
    outline_cubic(&outline, 43167, -233, 3729047, 635, 212610, 240);
    outline_close(&outline);
    cover(wide_plane, width, 2);
    check(wide_plane[width + 5757] == OUTLINE_COVERAGE_MAX,
          "a pixel whole inside a curve on the widest plane is 255, not 0");
    for (i = 0; i < width * 2; i++)
        target_pixels[i] = (UINT32)background;
    outline_fill(&outline, &target, 0xFFFFFF, OUTLINE_OPACITY_ONE);
    check(target_pixels[width + 5757] == 0xFFFFFF,
          "outline_fill draws that pixel as the ink");

    outline_start(&outline);
    outline_move(&outline, 3942177, -98);
    outline_cubic(&outline, 381869, 169, 1775525, 402, 2626492, -229);
    outline_close(&outline);
    cover(wide_plane, width, 2);
    check(wide_plane[7504] == 0,
          "a pixel whole outside a curve on the widest plane is 0, not 255");
    for (i = 0; i < width * 2; i++)
        target_pixels[i] = (UINT32)background;
    outline_fill(&outline, &target, 0xFFFFFF, OUTLINE_OPACITY_ONE);
    check(target_pixels[7504] == background,
          "outline_fill leaves that pixel as it was");
}

/* The widest plane: a rectangle across all of it. */
static void check_widest(void)
{
    BOX box = {OUTLINE_ONE / 2, 0,
               OUTLINE_WIDTH_MAX * OUTLINE_ONE - OUTLINE_ONE / 2,
               2 * OUTLINE_ONE};

    outline_start(&outline);
    rectangle(&box, 1);
    check(cover(wide_plane, OUTLINE_WIDTH_MAX, 2) == 1,
          "a plane OUTLINE_WIDTH_MAX wide is accepted");
    check(wide_plane[0] == 128 && wide_plane[1] == OUTLINE_COVERAGE_MAX &&
              wide_plane[OUTLINE_WIDTH_MAX - 1] == 128 &&
              wide_plane[OUTLINE_WIDTH_MAX - 2] == OUTLINE_COVERAGE_MAX &&
              wide_plane[OUTLINE_WIDTH_MAX + 8000] == OUTLINE_COVERAGE_MAX,
          "a rectangle across the widest plane is covered to its ends");
}

/* ------------------------------------------------------------------------
   outline_fill mixes by the coverage. */

/* The pixel a fill of ink at opacity gives over background where the
   coverage is coverage: gfx_cross_fade's mix of each channel, weighted
   by coverage and opacity of their wholes (ui.md U2 rule 4); the
   background where it is 0. */
static UINT32 mixed(UINT32 background, UINT32 ink, INT64 coverage,
                    INT64 opacity)
{
    UINT32 out = 0;
    int shift;

    if (coverage == 0)
        return background;
    for (shift = 16; shift >= 0; shift -= 8)
        out |= (UINT32)gfx_cross_fade((background >> shift) & 0xFF,
                                      (ink >> shift) & 0xFF,
                                      coverage * opacity,
                                      OUTLINE_COVERAGE_MAX *
                                          (INT64)OUTLINE_OPACITY_ONE)
               << shift;
    return out;
}

static void check_fill(void)
{
    int trial;

    for (trial = 0; trial < CASES; trial++) {
        INT64 width = between(1, 40), height = between(1, 40);
        INT64 stride = width + between(0, 3), i, x, y, bad = 0;
        UINT32 ink = (UINT32)between(0, 0xFFFFFF), opacity;
        CANVAS target;
        FIGURE figure;

        switch (between(0, 3)) {
        case 0: opacity = 0; break;
        case 1: opacity = (UINT32)between(0, OUTLINE_OPACITY_ONE); break;
        default: opacity = OUTLINE_OPACITY_ONE; break;
        }
        figure = random_figure(width, height);
        build_figure(&figure, 0, 0);
        if (!cover(plane, width, height)) {
            check(0, "a shape's coverage is written");
            continue;
        }
        for (i = 0; i < stride * height; i++)
            canvas[i] = expected_canvas[i] = (UINT32)between(0, 0xFFFFFF);
        for (y = 0; y < height; y++)
            for (x = 0; x < width; x++)
                expected_canvas[y * stride + x] =
                    mixed(canvas[y * stride + x], ink, plane[y * width + x],
                          opacity);
        target.pixels = canvas;
        target.width = width;
        target.height = height;
        target.stride = stride;
        check(outline_fill(&outline, &target, ink, opacity) == 1,
              "outline_fill draws");
        for (i = 0; i < stride * height; i++)
            if (canvas[i] != expected_canvas[i])
                bad++;
        check(bad == 0,
              "outline_fill mixes ink by the coverage outline_coverage gives");
    }
}

int main(void)
{
    check_rectangles();
    check_winding();
    check_triangles();
    check_curves();
    check_ends();
    check_empty();
    check_corners();
    check_clipping();
    check_translation();
    check_growth();
    check_refusals();
    check_drift();
    check_widest();
    check_fill();
    printf("%u checks, %u failed\n", checks, failures);
    return failures ? 1 : 0;
}
