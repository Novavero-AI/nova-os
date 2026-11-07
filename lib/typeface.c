/* Reading a nova-os font (typeface.h, include/font_format.h).
   typeface_load checks every count and record once; the other calls
   then read fields byte by byte with no check, as the format's records
   need no alignment. design/text/typeface.py is the prototype. */

#include "typeface.h"
#include "font_format.h"

#define BYTE_BITS 8
#define SIGN_16 0x8000
#define RANGE_16 0x10000

static UINT64 le(const UINT8 *p, int bytes)
{
    UINT64 value = 0;
    int i;

    for (i = bytes - 1; i >= 0; i--)
        value = value << BYTE_BITS | p[i];
    return value;
}

static INT64 s16(const UINT8 *p)
{
    INT64 v = (INT64)le(p, 2);

    return v & SIGN_16 ? v - RANGE_16 : v;
}

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

static int same(const UINT8 *a, const char *b, UINT32 count)
{
    UINT32 i;

    for (i = 0; i < count; i++)
        if (a[i] != (UINT8)b[i])
            return 0;
    return 1;
}

/* Whether the records after the header are in range and order. */
static int records_valid(const TYPEFACE *f, UINT32 left_classes,
                         UINT32 contour_count, UINT32 point_count)
{
    UINT64 last = 0, running = 0, previous = 0;
    UINT32 i;

    for (i = 0; i < f->range_count; i++) {
        const UINT8 *r = f->ranges + (UINT64)i * FONT_RANGE_SIZE;
        UINT64 first = le(r + FONT_RANGE_FIRST, 4);
        UINT64 count = le(r + FONT_RANGE_COUNT, 2);

        if (first < last || count == 0 ||
            le(r + FONT_RANGE_GLYPH, 2) + count > f->glyph_count)
            return 0;
        last = first + count;
    }
    for (i = 0; i < f->glyph_count; i++) {
        const UINT8 *g = f->glyphs + (UINT64)i * FONT_GLYPH_SIZE;

        if (le(g + FONT_GLYPH_LEFT_CLASS, 2) >= left_classes ||
            le(g + FONT_GLYPH_RIGHT_CLASS, 2) >= f->right_classes ||
            le(g + FONT_GLYPH_FIRST_CONTOUR, 4) != running)
            return 0;
        running += le(g + FONT_GLYPH_CONTOURS, 2);
    }
    if (running != contour_count)
        return 0;
    for (i = 0; i < contour_count; i++) {
        UINT64 end = le(f->contours + (UINT64)i * FONT_CONTOUR_SIZE, 4);

        if (end <= previous)
            return 0;
        previous = end;
    }
    if (previous != point_count)
        return 0;
    for (i = 0; i < point_count; i++)
        if (f->flags[i] & ~FONT_ON_CURVE)
            return 0;
    return 1;
}

int typeface_load(TYPEFACE *face, const UINT8 *bytes, UINT64 size)
{
    TYPEFACE f;
    UINT64 left_classes, contour_count, point_count, range_count, expected;

    if (size < FONT_HEADER_SIZE ||
        !same(bytes + FONT_HEADER_MAGIC, FONT_MAGIC, FONT_MAGIC_SIZE) ||
        le(bytes + FONT_HEADER_VERSION, 4) != FONT_VERSION)
        return 0;
    f.units = (INT64)le(bytes + FONT_HEADER_UNITS, 2);
    f.ascender = s16(bytes + FONT_HEADER_ASCENDER);
    f.descender = s16(bytes + FONT_HEADER_DESCENDER);
    f.line_gap = s16(bytes + FONT_HEADER_LINE_GAP);
    f.x_height = s16(bytes + FONT_HEADER_X_HEIGHT);
    f.cap_height = s16(bytes + FONT_HEADER_CAP_HEIGHT);
    f.glyph_count = (UINT32)le(bytes + FONT_HEADER_GLYPHS, 2);
    range_count = le(bytes + FONT_HEADER_RANGES, 2);
    left_classes = le(bytes + FONT_HEADER_LEFT_CLASSES, 2);
    f.right_classes = (UINT32)le(bytes + FONT_HEADER_RIGHT_CLASSES, 2);
    contour_count = le(bytes + FONT_HEADER_CONTOURS, 4);
    point_count = le(bytes + FONT_HEADER_POINTS, 4);
    if (f.units == 0 || f.glyph_count == 0 || left_classes == 0 ||
        f.right_classes == 0)
        return 0;
    f.range_count = (UINT32)range_count;
    expected = FONT_HEADER_SIZE + range_count * FONT_RANGE_SIZE +
               (UINT64)f.glyph_count * FONT_GLYPH_SIZE +
               contour_count * FONT_CONTOUR_SIZE +
               point_count * FONT_POINT_SIZE +
               left_classes * f.right_classes * FONT_KERN_SIZE +
               point_count * FONT_FLAG_SIZE;
    if (expected != size)
        return 0;
    f.ranges = bytes + FONT_HEADER_SIZE;
    f.glyphs = f.ranges + range_count * FONT_RANGE_SIZE;
    f.contours = f.glyphs + (UINT64)f.glyph_count * FONT_GLYPH_SIZE;
    f.points = f.contours + contour_count * FONT_CONTOUR_SIZE;
    f.kerning = f.points + point_count * FONT_POINT_SIZE;
    f.flags = f.kerning + left_classes * f.right_classes * FONT_KERN_SIZE;
    if (!records_valid(&f, (UINT32)left_classes, (UINT32)contour_count,
                       (UINT32)point_count))
        return 0;
    *face = f;
    return 1;
}

UINT32 typeface_glyph(const TYPEFACE *face, UINT32 code)
{
    UINT32 low = 0, high = face->range_count;

    while (low < high) {
        UINT32 mid = (low + high) / 2;
        const UINT8 *r = face->ranges + (UINT64)mid * FONT_RANGE_SIZE;
        UINT64 first = le(r + FONT_RANGE_FIRST, 4);

        if (code < first)
            high = mid;
        else if (code >= first + le(r + FONT_RANGE_COUNT, 2))
            low = mid + 1;
        else
            return (UINT32)(le(r + FONT_RANGE_GLYPH, 2) + code - first);
    }
    return 0;
}

static const UINT8 *glyph_record(const TYPEFACE *face, UINT32 glyph)
{
    return face->glyphs +
           (UINT64)(glyph < face->glyph_count ? glyph : 0) * FONT_GLYPH_SIZE;
}

INT64 typeface_advance(const TYPEFACE *face, UINT32 glyph)
{
    return (INT64)le(glyph_record(face, glyph) + FONT_GLYPH_ADVANCE, 2);
}

INT64 typeface_kerning(const TYPEFACE *face, UINT32 left, UINT32 right)
{
    UINT64 row = le(glyph_record(face, left) + FONT_GLYPH_LEFT_CLASS, 2);
    UINT64 column = le(glyph_record(face, right) + FONT_GLYPH_RIGHT_CLASS, 2);

    return s16(face->kerning +
               (row * face->right_classes + column) * FONT_KERN_SIZE);
}

/* A point of the glyph, placed: its x and y in units and whether it lies
   on the curve. */
typedef struct {
    INT64 x, y;
    int on;
} PLACED;

static PLACED place(const TYPEFACE *face, UINT64 index, INT64 left, INT64 pen,
                    INT64 baseline, INT64 size)
{
    const UINT8 *p = face->points + index * FONT_POINT_SIZE;
    PLACED placed;

    placed.x = left + rounded((pen + s16(p + FONT_POINT_X)) * size,
                              face->units);
    placed.y = baseline * OUTLINE_ONE -
               rounded(s16(p + FONT_POINT_Y) * size, face->units);
    placed.on = face->flags[index] & FONT_ON_CURVE;
    return placed;
}

/* The point midway between a and b, rounded toward minus infinity. */
static PLACED midpoint(PLACED a, PLACED b)
{
    PLACED m;

    m.x = floor_div(a.x + b.x, 2);
    m.y = floor_div(a.y + b.y, 2);
    m.on = 1;
    return m;
}

void typeface_outline(const TYPEFACE *face, UINT32 glyph, INT64 left,
                      INT64 pen, INT64 baseline, INT64 size,
                      OUTLINE *outline)
{
    const UINT8 *g = glyph_record(face, glyph);
    UINT64 contour = le(g + FONT_GLYPH_FIRST_CONTOUR, 4);
    UINT64 last = contour + le(g + FONT_GLYPH_CONTOURS, 2);
    UINT64 start = contour ? le(face->contours + (contour - 1) *
                                                     FONT_CONTOUR_SIZE,
                                4)
                           : 0;

    for (; contour < last; contour++) {
        UINT64 end = le(face->contours + contour * FONT_CONTOUR_SIZE, 4);
        PLACED first = place(face, start, left, pen, baseline, size);
        PLACED final = place(face, end - 1, left, pen, baseline, size);
        PLACED origin, control;
        UINT64 from = start, to = end, i;
        int pulled = 0;

        /* Start on the curve: at the first point, else the last, else
           midway between them. */
        if (first.on) {
            origin = first;
            from = start + 1;
        } else if (final.on) {
            origin = final;
            to = end - 1;
        } else {
            origin = midpoint(final, first);
        }
        control = origin;
        outline_move(outline, origin.x, origin.y);
        for (i = from; i < to; i++) {
            PLACED p = place(face, i, left, pen, baseline, size);

            if (p.on) {
                if (pulled)
                    outline_quad(outline, control.x, control.y, p.x, p.y);
                else
                    outline_line(outline, p.x, p.y);
                pulled = 0;
            } else {
                if (pulled) {
                    PLACED m = midpoint(control, p);

                    outline_quad(outline, control.x, control.y, m.x, m.y);
                }
                control = p;
                pulled = 1;
            }
        }
        if (pulled)
            outline_quad(outline, control.x, control.y, origin.x, origin.y);
        outline_close(outline);
        start = end;
    }
}
