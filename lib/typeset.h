/** @file
    Text set in lines of a nova-os font (design/text/README.md): UTF-8,
    left to right, each glyph after the last by its advance and the
    pair's kerning, with no other shaping, broken into lines at spaces.
    Positions across are kept to OUTLINE_ONE units, a 256th of a pixel,
    and baselines are whole pixel rows. */

#ifndef NOVA_TYPESET_H
#define NOVA_TYPESET_H

#include "typeface.h"

/** The width of the length bytes of text, set as one line at size
    units to the em, in units: the advances of its glyphs and the
    kerning of each pair, scaled. */
INT64 typeset_width(const TYPEFACE *face, INT64 size, const UINT8 *text,
                    UINT64 length);

/** The first line of the length bytes of text, set at size units to
    the em, that is at most width units wide, broken at a space: its
    length, and in *next where the line after it starts, past the spaces
    the break takes. A first word wider than width, with any spaces
    before it, is a line alone. */
UINT64 typeset_break(const TYPEFACE *face, INT64 size, const UINT8 *text,
                     UINT64 length, INT64 width, UINT64 *next);

/** The distance from one baseline to the next at size units to the em,
    in whole pixels: the ascender, the descender and the line gap,
    scaled and rounded. */
INT64 typeset_line_height(const TYPEFACE *face, INT64 size);

/** Draw the length bytes of text as one line at size units to the em
    on canvas, the first glyph's origin at left, in units, on baseline,
    a pixel row, in ink at opacity (outline_fill). Each glyph is filled
    on its own, in outline, so where two overlap both are mixed. A byte
    that does not start a well-formed UTF-8 sequence is drawn as
    U+FFFD. Returns 1, or 0 once a glyph's outline_fill fails. */
int typeset_draw(const CANVAS *canvas, const TYPEFACE *face, INT64 size,
                 INT64 left, INT64 baseline, const UINT8 *text,
                 UINT64 length, UINT32 ink, UINT32 opacity,
                 OUTLINE *outline);

#endif
