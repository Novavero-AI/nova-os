/* Setting text in lines (typeset.h). A line's glyphs are placed in font
   units, their advances and kerning summed exactly, and only each
   point's place is scaled and rounded (typeface_outline), so a line
   drifts by no rounding however long it is. design/text/typeface.py is
   the prototype. */

#include "typeset.h"

#define SPACE 0x20
#define REPLACEMENT 0xFFFD

/* UTF-8 (RFC 3629, 4): the lead bytes of 2, 3 and 4-byte sequences, the
   continuation bytes' form, and the code points each may encode. */
#define ASCII_END 0x80
#define LEAD_2_FIRST 0xC2
#define LEAD_2_LAST 0xDF
#define LEAD_3_FIRST 0xE0
#define LEAD_3_LAST 0xEF
#define LEAD_4_FIRST 0xF0
#define LEAD_4_LAST 0xF4
#define LEAD_2_BITS 0x1F
#define LEAD_3_BITS 0x0F
#define LEAD_4_BITS 0x07
#define CONTINUATION_MASK 0xC0
#define CONTINUATION 0x80
#define CONTINUATION_BITS 0x3F
#define CONTINUATION_SHIFT 6
#define LEAST_2 0x80
#define LEAST_3 0x800
#define LEAST_4 0x10000
#define CODE_POINT_MAX 0x10FFFF
#define SURROGATE_FIRST 0xD800
#define SURROGATE_LAST 0xDFFF

/* a / b rounded toward minus infinity, for b > 0. */
static INT64 floor_div(INT64 a, INT64 b)
{
    INT64 q = a / b;

    return q * b > a ? q - 1 : q;
}

static INT64 rounded(INT64 n, INT64 d)
{
    return floor_div(2 * n + d, 2 * d);
}

/* The code point at *at in the length bytes of text, moving *at past
   it: U+FFFD, one byte on, for a byte that does not start a well-formed
   sequence. */
static UINT32 decode(const UINT8 *text, UINT64 length, UINT64 *at)
{
    UINT64 i = *at, n, k;
    UINT32 b = text[i], code, least;

    if (b < ASCII_END) {
        *at = i + 1;
        return b;
    }
    if (b >= LEAD_2_FIRST && b <= LEAD_2_LAST) {
        n = 1, code = b & LEAD_2_BITS, least = LEAST_2;
    } else if (b >= LEAD_3_FIRST && b <= LEAD_3_LAST) {
        n = 2, code = b & LEAD_3_BITS, least = LEAST_3;
    } else if (b >= LEAD_4_FIRST && b <= LEAD_4_LAST) {
        n = 3, code = b & LEAD_4_BITS, least = LEAST_4;
    } else {
        *at = i + 1;
        return REPLACEMENT;
    }
    *at = i + 1;
    if (i + n >= length)
        return REPLACEMENT;
    for (k = 1; k <= n; k++) {
        UINT32 c = text[i + k];

        if ((c & CONTINUATION_MASK) != CONTINUATION)
            return REPLACEMENT;
        code = code << CONTINUATION_SHIFT | (c & CONTINUATION_BITS);
    }
    if (code < least || code > CODE_POINT_MAX ||
        (code >= SURROGATE_FIRST && code <= SURROGATE_LAST))
        return REPLACEMENT;
    *at = i + n + 1;
    return code;
}

/* A walk along a line's glyphs: each glyph's origin, pen, in font
   units from the line's start, the kerning with the one before it
   included. */
typedef struct {
    const TYPEFACE *face;
    const UINT8 *text;
    UINT64 length, at;
    UINT32 glyph;
    int started;
    INT64 pen;
} WALK;

static void walk_start(WALK *w, const TYPEFACE *face, const UINT8 *text,
                       UINT64 length)
{
    w->face = face;
    w->text = text;
    w->length = length;
    w->at = 0;
    w->started = 0;
    w->pen = 0;
}

/* Step to the next glyph; 0 at the line's end. */
static int walk_next(WALK *w)
{
    UINT32 next;

    if (w->at >= w->length)
        return 0;
    next = typeface_glyph(w->face, decode(w->text, w->length, &w->at));
    if (w->started)
        w->pen += typeface_advance(w->face, w->glyph) +
                  typeface_kerning(w->face, w->glyph, next);
    w->glyph = next;
    w->started = 1;
    return 1;
}

INT64 typeset_width(const TYPEFACE *face, INT64 size, const UINT8 *text,
                    UINT64 length)
{
    WALK w;

    walk_start(&w, face, text, length);
    while (walk_next(&w))
        ;
    if (!w.started)
        return 0;
    return rounded((w.pen + typeface_advance(face, w.glyph)) * size,
                   face->units);
}

UINT64 typeset_break(const TYPEFACE *face, INT64 size, const UINT8 *text,
                     UINT64 length, INT64 width, UINT64 *next)
{
    UINT64 end = 0, i = 0, following;
    int found = 0;

    /* Spaces before the first word go with it. */
    while (i < length && text[i] == SPACE)
        i++;
    while (i < length) {
        while (i < length && text[i] != SPACE)
            i++;
        if (found && typeset_width(face, size, text, i) > width)
            break;
        end = i;
        found = 1;
        while (i < length && text[i] == SPACE)
            i++;
    }
    for (following = end; following < length && text[following] == SPACE;
         following++)
        ;
    *next = following;
    return end;
}

INT64 typeset_line_height(const TYPEFACE *face, INT64 size)
{
    return rounded((face->ascender - face->descender + face->line_gap) * size,
                   face->units * OUTLINE_ONE);
}

int typeset_draw(const CANVAS *canvas, const TYPEFACE *face, INT64 size,
                 INT64 left, INT64 baseline, const UINT8 *text,
                 UINT64 length, UINT32 ink, UINT32 opacity,
                 OUTLINE *outline)
{
    WALK w;

    walk_start(&w, face, text, length);
    while (walk_next(&w)) {
        outline_start(outline);
        typeface_outline(face, w.glyph, left, w.pen, baseline, size,
                         outline);
        if (!outline_fill(outline, canvas, ink, opacity))
            return 0;
    }
    return 1;
}
