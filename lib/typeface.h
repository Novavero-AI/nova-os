/** @file
    A nova-os font (include/font_format.h), read in place from its bytes:
    checked whole once, when it is loaded, so that each glyph is then
    read with no check to follow. Sizes, positions and widths are in
    OUTLINE_ONE units to a pixel (outline.h); lengths in the font are in
    its own units, units to the em. */

#ifndef NOVA_TYPEFACE_H
#define NOVA_TYPEFACE_H

#include "outline.h"

/** A loaded font: its metrics, in font units, y growing upward from
    the baseline, its counts, and where its records lie in its bytes. */
typedef struct {
    INT64 units;          /**< font units to the em */
    INT64 ascender;       /**< above the baseline */
    INT64 descender;      /**< negative, below the baseline */
    INT64 line_gap;       /**< between one line's descender and the next's ascender */
    INT64 x_height;       /**< a lowercase x's height */
    INT64 cap_height;     /**< a capital's height */
    UINT32 glyph_count;   /**< glyphs, glyph 0 the missing-glyph shape */
    UINT32 range_count;   /**< ranges of the character map */
    UINT32 right_classes; /**< columns of the kerning matrix */
    const UINT8 *ranges;   /**< the character map's ranges */
    const UINT8 *glyphs;   /**< the glyph records */
    const UINT8 *contours; /**< the contour ends */
    const UINT8 *points;   /**< the points */
    const UINT8 *kerning;  /**< the kerning matrix */
    const UINT8 *flags;    /**< the points' flags */
} TYPEFACE;

/** Read the size bytes at bytes as a font into face, which then points
    into them, so they must last as long as face is used. Returns 1; or
    0, with face unset, if they are not a font of FONT_VERSION exactly:
    a header past what it allows, records that do not fill the size, or
    one out of range or order. */
int typeface_load(TYPEFACE *face, const UINT8 *bytes, UINT64 size);

/** The glyph face draws code, a Unicode code point, with: its own, or
    0, the missing-glyph shape, for one its map does not hold. */
UINT32 typeface_glyph(const TYPEFACE *face, UINT32 code);

/** Glyph's advance width, in font units. A glyph past the font's
    counts as glyph 0, here and in the calls below. */
INT64 typeface_advance(const TYPEFACE *face, UINT32 glyph);

/** The kerning of glyph left followed by glyph right, in font units, to
    add to left's advance. */
INT64 typeface_kerning(const TYPEFACE *face, UINT32 left, UINT32 right);

/** Add glyph's contours to outline, at size units to the em: its origin
    pen font units right of left, in units, on baseline, a pixel row. A
    point (x, y) of the glyph goes to left + (pen + x) * size / em
    across and baseline * OUTLINE_ONE - y * size / em down, each
    rounded to the nearest unit, so that glyphs placed along a line from
    one left round alike wherever they fall. */
void typeface_outline(const TYPEFACE *face, UINT32 glyph, INT64 left,
                      INT64 pen, INT64 baseline, INT64 size,
                      OUTLINE *outline);

#endif
