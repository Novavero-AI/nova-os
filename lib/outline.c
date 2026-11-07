/* Filled outlines (outline.h). Curves become lines; then each pixel row
   is cut into slices at every line's ends and at every crossing of two
   lines within it, so that within a slice the lines that cross it keep
   their order across it. Counting their directions from the left gives
   the winding number between each two, and only the lines where it
   turns from zero to nonzero or back bound what the slice fills: each
   adds the area right of it within each pixel it passes, positive for
   the first, negative for the second, and the running sum across the
   row is each pixel's area inside the outline, exactly, whatever the
   contours' overlaps. design/text/outline.py is the prototype, and
   design/text/README.md gives the choice of this method over summing
   every line's signed area.

   The prototype's integers are unbounded and its // rounds toward minus
   infinity. Here every value fits an INT64 for coordinates within
   OUTLINE_COORDINATE_MAX, and every quotient that can be negative goes
   through floor_div. */

#include "outline.h"
#include "raster.h"

/* Twice a pixel's area, in units: what a row's running sum reaches
   over a pixel the outline fills whole. */
#define FULL (2 * OUTLINE_ONE * OUTLINE_ONE)

/* A curve is cut into the fewest lines, at most SEGMENTS_MAX, that keep
   within FLATNESS of it. A quadratic's lines stray at most |d| / 4n^2
   from it, for d its second difference, so n lines do when
   16 FLATNESS^2 n^4 >= |d|^2; a cubic's at most 3 max|d| / 4n^2, so
   CUBIC_FACTOR times as much. */
#define FLATNESS (OUTLINE_ONE / 16)
#define SEGMENTS_MAX 64
#define QUAD_FACTOR 1
#define CUBIC_FACTOR 9

/* A circle's quarter as a cubic: control points KAPPA of the radius
   from its ends along their tangents, 4(sqrt 2 - 1)/3, which puts the
   curve's midpoint on the circle and keeps it at most 0.027253 percent
   of the radius outside it (Spencer Mortensen, "Approximate a circle
   with cubic Bezier curves", its standard approximation). */
#define KAPPA_NUM 55228475
#define KAPPA_DEN 100000000

#define CHANNEL_BITS 8
#define CHANNEL_MASK 0xFFu

/* a / b rounded toward minus infinity, for b > 0. */
static INT64 floor_div(INT64 a, INT64 b)
{
    INT64 q = a / b;

    return q * b > a ? q - 1 : q;
}

/* n / d rounded to the nearest integer, halves up, for d > 0. */
static INT64 rounded(INT64 n, INT64 d)
{
    return floor_div(2 * n + d, 2 * d);
}

static INT64 magnitude(INT64 v)
{
    return v < 0 ? -v : v;
}

/* ------------------------------------------------------------------------
   Paths. */

static int in_range(INT64 x, INT64 y)
{
    return magnitude(x) <= OUTLINE_COORDINATE_MAX &&
           magnitude(y) <= OUTLINE_COORDINATE_MAX;
}

/* Add a line from the current point to (x, y); a horizontal one bounds
   nothing and is left out. */
static void add_line(OUTLINE *o, INT64 x, INT64 y)
{
    if (!o->open || !in_range(x, y)) {
        o->failed = 1;
        return;
    }
    if (y != o->y) {
        if (o->count == OUTLINE_EDGES_MAX) {
            o->failed = 1;
            return;
        }
        o->edges[o->count].x0 = o->x;
        o->edges[o->count].y0 = o->y;
        o->edges[o->count].x1 = x;
        o->edges[o->count].y1 = y;
        o->count++;
    }
    o->x = x;
    o->y = y;
}

void outline_start(OUTLINE *outline)
{
    outline->count = 0;
    outline->failed = 0;
    outline->open = 0;
}

void outline_close(OUTLINE *outline)
{
    if (outline->open &&
        (outline->x != outline->start_x || outline->y != outline->start_y))
        add_line(outline, outline->start_x, outline->start_y);
    outline->open = 0;
}

void outline_move(OUTLINE *outline, INT64 x, INT64 y)
{
    outline_close(outline);
    if (!in_range(x, y)) {
        outline->failed = 1;
        return;
    }
    outline->start_x = x;
    outline->start_y = y;
    outline->x = x;
    outline->y = y;
    outline->open = 1;
}

void outline_line(OUTLINE *outline, INT64 x, INT64 y)
{
    add_line(outline, x, y);
}

/* The lines a curve whose second difference has squared length
   second2 is cut into, factor its bound's. */
static INT64 segments(INT64 second2, INT64 factor)
{
    INT64 n = 1;

    while (n < SEGMENTS_MAX &&
           16 * FLATNESS * FLATNESS * n * n * n * n < factor * second2)
        n++;
    return n;
}

void outline_quad(OUTLINE *outline, INT64 cx, INT64 cy, INT64 x, INT64 y)
{
    INT64 x0 = outline->x, y0 = outline->y, n, nn, i, ddx, ddy;

    if (!outline->open || !in_range(cx, cy) || !in_range(x, y)) {
        outline->failed = 1;
        return;
    }
    ddx = x0 - 2 * cx + x;
    ddy = y0 - 2 * cy + y;
    n = segments(ddx * ddx + ddy * ddy, QUAD_FACTOR);
    nn = n * n;
    for (i = 1; i <= n; i++) {
        INT64 a = (n - i) * (n - i), b = 2 * i * (n - i), c = i * i;

        add_line(outline, rounded(a * x0 + b * cx + c * x, nn),
                 rounded(a * y0 + b * cy + c * y, nn));
    }
}

void outline_cubic(OUTLINE *outline, INT64 c1x, INT64 c1y, INT64 c2x,
                   INT64 c2y, INT64 x, INT64 y)
{
    INT64 x0 = outline->x, y0 = outline->y, n, nnn, i, ax, ay, bx, by, a2, b2;

    if (!outline->open || !in_range(c1x, c1y) || !in_range(c2x, c2y) ||
        !in_range(x, y)) {
        outline->failed = 1;
        return;
    }
    ax = x0 - 2 * c1x + c2x;
    ay = y0 - 2 * c1y + c2y;
    bx = c1x - 2 * c2x + x;
    by = c1y - 2 * c2y + y;
    a2 = ax * ax + ay * ay;
    b2 = bx * bx + by * by;
    n = segments(a2 > b2 ? a2 : b2, CUBIC_FACTOR);
    nnn = n * n * n;
    for (i = 1; i <= n; i++) {
        INT64 j = n - i;
        INT64 a = j * j * j, b = 3 * i * j * j, c = 3 * i * i * j;
        INT64 d = i * i * i;

        add_line(outline, rounded(a * x0 + b * c1x + c * c2x + d * x, nnn),
                 rounded(a * y0 + b * c1y + c * c2y + d * y, nnn));
    }
}

void outline_rounded_rect(OUTLINE *outline, INT64 left, INT64 top,
                          INT64 right, INT64 bottom, INT64 radius)
{
    INT64 r = radius, k;

    if (!in_range(left, top) || !in_range(right, bottom) ||
        !in_range(radius, 0)) {
        outline->failed = 1;
        return;
    }
    k = rounded(radius * KAPPA_NUM, KAPPA_DEN);
    outline_move(outline, left + r, top);
    outline_line(outline, right - r, top);
    outline_cubic(outline, right - r + k, top, right, top + r - k, right,
                  top + r);
    outline_line(outline, right, bottom - r);
    outline_cubic(outline, right, bottom - r + k, right - r + k, bottom,
                  right - r, bottom);
    outline_line(outline, left + r, bottom);
    outline_cubic(outline, left + r - k, bottom, left, bottom - r + k, left,
                  bottom - r);
    outline_line(outline, left, top + r);
    outline_cubic(outline, left, top + r - k, left + r - k, top, left + r,
                  top);
    outline_close(outline);
}

void outline_circle(OUTLINE *outline, INT64 x, INT64 y, INT64 radius)
{
    if (!in_range(x, y) || !in_range(radius, 0)) {
        outline->failed = 1;
        return;
    }
    outline_rounded_rect(outline, x - radius, y - radius, x + radius,
                         y + radius, radius);
}

/* ------------------------------------------------------------------------
   Coverage. */

/* The x of piece's edge at y, between its ends' y. */
static INT64 x_at(const OUTLINE_PIECE *p, INT64 y)
{
    return p->top_x + rounded((y - p->top_y) * (p->bottom_x - p->top_x),
                              p->bottom_y - p->top_y);
}

/* Add the part of a bounding edge that lies in one column, from
   (xp, yp) to (xq, yq): twice the area right of it within the column to
   the column, and the rest to the next, so that the running sum across
   the row is each pixel's area. */
static void cell(INT64 *area, INT64 width, INT64 xp, INT64 yp, INT64 xq,
                 INT64 yq, INT64 sign)
{
    INT64 dy = magnitude(yq - yp), c, f;

    if (dy == 0)
        return;
    c = floor_div(xp + xq, 2 * OUTLINE_ONE);
    if (c < 0) {
        area[0] += sign * dy * 2 * OUTLINE_ONE;
    } else if (c < width) {
        f = xp + xq - 2 * c * OUTLINE_ONE;
        area[c] += sign * dy * (2 * OUTLINE_ONE - f);
        area[c + 1] += sign * dy * f;
    }
}

/* Add the bounding edge from (xa, ya) to (xb, yb), row-local, cut at
   each column it crosses. What lies left of the canvas covers every
   column from 0, so it goes to column 0 whole; what lies right of it
   covers none. */
static void deposit(INT64 *area, INT64 width, INT64 xa, INT64 ya, INT64 xb,
                    INT64 yb, INT64 sign)
{
    INT64 limit = width * OUTLINE_ONE, x, y, t;

    if (xa == xb) {
        cell(area, width, xa, ya, xb, yb, sign);
        return;
    }
    if (xa > xb) {
        t = xa, xa = xb, xb = t;
        t = ya, ya = yb, yb = t;
    }
    if (xb <= 0) {
        area[0] += sign * magnitude(yb - ya) * 2 * OUTLINE_ONE;
        return;
    }
    if (xa >= limit)
        return;
    x = xa;
    y = ya;
    if (x < 0) {
        INT64 y0 = ya + rounded(-xa * (yb - ya), xb - xa);

        area[0] += sign * magnitude(y0 - ya) * 2 * OUTLINE_ONE;
        x = 0;
        y = y0;
    }
    for (;;) {
        INT64 boundary = (floor_div(x, OUTLINE_ONE) + 1) * OUTLINE_ONE, next;

        if (boundary >= xb) {
            cell(area, width, x, y, xb, yb, sign);
            return;
        }
        next = ya + rounded((boundary - xa) * (yb - ya), xb - xa);
        cell(area, width, x, y, boundary, next, sign);
        x = boundary;
        y = next;
        if (x >= limit)
            return;
    }
}

/* Add y to the row's sorted splits, once. Every split is a whole unit
   from the row's top to its bottom, so they fit o->splits. */
static void split_at(OUTLINE *o, UINT32 *count, INT64 y)
{
    UINT32 low = 0, high = *count, i;

    while (low < high) {
        UINT32 mid = (low + high) / 2;

        if (o->splits[mid] < y)
            low = mid + 1;
        else
            high = mid;
    }
    if (low < *count && o->splits[low] == y)
        return;
    for (i = *count; i > low; i--)
        o->splits[i] = o->splits[i - 1];
    o->splits[low] = y;
    (*count)++;
}

/* Split the row at the crossing of pieces p and q, if they cross within
   it, rounded down to a unit, which lies within both. */
static void split_crossing(OUTLINE *o, UINT32 *count, const OUTLINE_PIECE *p,
                           const OUTLINE_PIECE *q)
{
    INT64 lo = p->from > q->from ? p->from : q->from;
    INT64 hi = p->to < q->to ? p->to : q->to;
    INT64 dl, dh, num, den, y;

    if (lo >= hi)
        return;
    dl = x_at(p, lo) - x_at(q, lo);
    dh = x_at(p, hi) - x_at(q, hi);
    if (!((dl < 0 && dh > 0) || (dh < 0 && dl > 0)))
        return;
    num = (hi - lo) * dl;
    den = dl - dh;
    if (den < 0) {
        num = -num;
        den = -den;
    }
    y = lo + floor_div(num, den);
    if (y > lo)
        split_at(o, count, y);
}

/* Fill one slice, from ya to yb, of the row from top: order the pieces
   that cross it by where they do and add each that turns the winding
   number from zero or back to it. */
static void slice(OUTLINE *o, UINT32 pieces, INT64 top, INT64 ya, INT64 yb,
                  INT64 width)
{
    UINT32 n = 0, i, j;
    INT64 winding = 0;

    for (i = 0; i < pieces; i++) {
        const OUTLINE_PIECE *p = &o->pieces[i];
        OUTLINE_CROSSING c;

        if (p->from > ya || p->to < yb)
            continue;
        c.top = x_at(p, ya);
        c.bottom = x_at(p, yb);
        c.key = c.top + c.bottom;
        c.direction = p->direction;
        for (j = n; j > 0 && o->crossings[j - 1].key > c.key; j--)
            o->crossings[j] = o->crossings[j - 1];
        o->crossings[j] = c;
        n++;
    }
    for (i = 0; i < n; i++) {
        const OUTLINE_CROSSING *c = &o->crossings[i];
        INT64 before = winding;

        winding += c->direction;
        if (before == 0 && winding != 0)
            deposit(o->area, width, c->top, ya - top, c->bottom, yb - top, 1);
        else if (before != 0 && winding == 0)
            deposit(o->area, width, c->top, ya - top, c->bottom, yb - top,
                    -1);
    }
}

/* Add row top's areas, top in units, to o->area. Returns whether a line
   crosses the row. */
static int row(OUTLINE *o, INT64 top, INT64 width)
{
    INT64 bottom = top + OUTLINE_ONE;
    UINT32 pieces = 0, splits = 0, i, j;

    for (i = 0; i < o->count; i++) {
        const OUTLINE_EDGE *e = &o->edges[i];
        OUTLINE_PIECE *p = &o->pieces[pieces];
        int down = e->y0 < e->y1;

        p->top_x = down ? e->x0 : e->x1;
        p->top_y = down ? e->y0 : e->y1;
        p->bottom_x = down ? e->x1 : e->x0;
        p->bottom_y = down ? e->y1 : e->y0;
        if (p->top_y >= bottom || p->bottom_y <= top)
            continue;
        p->from = p->top_y > top ? p->top_y : top;
        p->to = p->bottom_y < bottom ? p->bottom_y : bottom;
        p->direction = down ? 1 : -1;
        pieces++;
    }
    if (pieces == 0)
        return 0;
    for (i = 0; i < pieces; i++) {
        split_at(o, &splits, o->pieces[i].from);
        split_at(o, &splits, o->pieces[i].to);
    }
    for (i = 0; i < pieces; i++)
        for (j = i + 1; j < pieces; j++)
            split_crossing(o, &splits, &o->pieces[i], &o->pieces[j]);
    for (i = 0; i + 1 < splits; i++)
        slice(o, pieces, top, o->splits[i], o->splits[i + 1], width);
    return 1;
}

/* Mix ink into pixel at coverage, of OUTLINE_COVERAGE_MAX, and opacity. */
static UINT32 mix(UINT32 pixel, UINT32 ink, INT64 coverage, INT64 opacity)
{
    INT64 weight = coverage * opacity;
    INT64 total = OUTLINE_COVERAGE_MAX * (INT64)OUTLINE_OPACITY_ONE;
    UINT32 out = 0;
    int shift;

    if (weight == total)
        return ink;
    for (shift = 2 * CHANNEL_BITS; shift >= 0; shift -= CHANNEL_BITS)
        out |= (UINT32)gfx_cross_fade(pixel >> shift & CHANNEL_MASK,
                                      ink >> shift & CHANNEL_MASK, weight,
                                      total)
               << shift;
    return out;
}

int outline_fill(OUTLINE *outline, const CANVAS *canvas, UINT32 ink,
                 UINT32 opacity)
{
    INT64 xmin, xmax, ymin, ymax, first, last, left, right, r, x;
    UINT32 i;

    outline_close(outline);
    if (outline->failed || canvas->width > OUTLINE_WIDTH_MAX ||
        opacity > OUTLINE_OPACITY_ONE)
        return 0;
    if (outline->count == 0)
        return 1;
    xmin = xmax = outline->edges[0].x0;
    ymin = ymax = outline->edges[0].y0;
    for (i = 0; i < outline->count; i++) {
        const OUTLINE_EDGE *e = &outline->edges[i];

        xmin = e->x0 < xmin ? e->x0 : xmin;
        xmin = e->x1 < xmin ? e->x1 : xmin;
        xmax = e->x0 > xmax ? e->x0 : xmax;
        xmax = e->x1 > xmax ? e->x1 : xmax;
        ymin = e->y0 < ymin ? e->y0 : ymin;
        ymin = e->y1 < ymin ? e->y1 : ymin;
        ymax = e->y0 > ymax ? e->y0 : ymax;
        ymax = e->y1 > ymax ? e->y1 : ymax;
    }
    first = floor_div(ymin, OUTLINE_ONE);
    last = floor_div(ymax - 1, OUTLINE_ONE) + 1;
    left = floor_div(xmin, OUTLINE_ONE);
    right = floor_div(xmax, OUTLINE_ONE) + 1;
    first = first < 0 ? 0 : first;
    last = last > canvas->height ? canvas->height : last;
    left = left < 0 ? 0 : left;
    right = right > canvas->width ? canvas->width : right;
    if (first >= last || left >= right)
        return 1;
    for (r = first; r < last; r++) {
        UINT32 *pixels = canvas->pixels + r * canvas->stride;
        INT64 area = 0;

        for (x = left; x <= right; x++)
            outline->area[x] = 0;
        if (!row(outline, r * OUTLINE_ONE, canvas->width))
            continue;
        for (x = left; x < right; x++) {
            INT64 coverage;

            area += outline->area[x];
            coverage = rounded(area * OUTLINE_COVERAGE_MAX, FULL);
            if (coverage <= 0)
                continue;
            if (coverage > OUTLINE_COVERAGE_MAX)
                coverage = OUTLINE_COVERAGE_MAX;
            pixels[x] = mix(pixels[x], ink, coverage, opacity);
        }
    }
    return 1;
}
