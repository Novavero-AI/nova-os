/* The console font's masks. Each stroke of glyphs.h becomes shapes in
   eighths of a pixel: a LINE a CAPSULE, a RING a RING, and an ARC an ARC
   with a dot, a CAPSULE of no length, at each end, so that every stroke
   ends round. Scaling rounds half up, and every coordinate is positive,
   so design/font/font.py's floor division rounds alike. */

#include "font.h"

/* The kind of each stroke is the shape it becomes. */
#define LINE CAPSULE
#define CARET FONT_CARET

/* A stroke's values, as glyphs.h lists them for its kind. */
#define STROKE_VALUES 7

typedef struct {
    char code;
    UINT8 kind;
    UINT16 v[STROKE_VALUES];
} STROKE;

static const STROKE strokes[] = {
#include "glyphs.h"
};

#define STROKE_COUNT (sizeof strokes / sizeof strokes[0])

/* The most shapes one glyph becomes; glyphs.h gives none more, which
   font.py checks, and font_render draws a glyph's first this many. */
#define GLYPH_SHAPES_MAX 32
#define ARC_SHAPES 3

/* Font units in eighths of a pixel, for cells width pixels wide. */
static INT64 scale(INT64 v, INT64 width)
{
    return (v * width * SUBPIXEL + FONT_UNITS_WIDE / 2) / FONT_UNITS_WIDE;
}

static void set(SHAPE *s, int kind, INT64 ax, INT64 ay, INT64 bx, INT64 r)
{
    s->kind = kind;
    s->ax = ax;
    s->ay = ay;
    s->bx = bx;
    s->by = ay;
    s->r = r;
    s->sx = 0;
    s->sy = 0;
    s->ex = 0;
    s->ey = 0;
}

/* A dot of radius r at (x, y). */
static void dot(SHAPE *s, INT64 x, INT64 y, INT64 r)
{
    set(s, CAPSULE, x, y, x, r);
}

/* Add stroke's shapes for cells width pixels wide at shapes + count, if
   they fit; returns the count after it. */
static int add(const STROKE *stroke, INT64 width, SHAPE *shapes, int count)
{
    INT64 v[STROKE_VALUES], r = scale(FONT_STROKE_RADIUS, width);
    SHAPE *s = shapes + count;
    int i;

    if (count + ARC_SHAPES > GLYPH_SHAPES_MAX)
        return count;
    for (i = 0; i < STROKE_VALUES; i++)
        v[i] = scale(stroke->v[i], width);
    if (stroke->kind == CAPSULE) {
        set(s, CAPSULE, v[0], v[1], v[2], r);
        s->by = v[3];
        return count + 1;
    }
    if (stroke->kind == RING) {
        set(s, RING, v[0], v[1], v[2], r);
        return count + 1;
    }
    set(s, ARC, v[0], v[1], v[2], r);
    s->sx = v[3] - v[0];
    s->sy = v[4] - v[1];
    s->ex = v[5] - v[0];
    s->ey = v[6] - v[1];
    dot(s + 1, v[3], v[4], r);
    dot(s + 2, v[5], v[6], r);
    return count + ARC_SHAPES;
}

INT64 font_height(INT64 width)
{
    return width * FONT_UNITS_TALL / FONT_UNITS_WIDE;
}

UINT64 font_masks_size(INT64 width)
{
    return (UINT64)(FONT_MASKS * width * font_height(width));
}

const UINT8 *font_mask(const UINT8 *masks, INT64 width, UINT32 code)
{
    return masks + (code - FONT_FIRST) * width * font_height(width);
}

void font_render(INT64 width, UINT8 *masks)
{
    SHAPE shapes[GLYPH_SHAPES_MAX];
    INT64 height = font_height(width), x, y;
    UINT32 code;
    UINT64 i;

    for (code = FONT_FIRST; code <= FONT_CARET; code++) {
        UINT8 *mask = masks + (code - FONT_FIRST) * width * height;
        int count = 0;

        for (i = 0; i < STROKE_COUNT; i++)
            if ((UINT8)strokes[i].code == code)
                count = add(&strokes[i], width, shapes, count);
        for (y = 0; y < height; y++)
            for (x = 0; x < width; x++)
                mask[y * width + x] =
                    (UINT8)gfx_coverage(shapes, count, x, y);
    }
}
