/** @file
    The nova-os font format, version 1 (docs/spec/font-format.md): a
    font as tools/mkfont.c writes it from a TrueType font and
    lib/typeface.c reads it. A file is a header and then six arrays of
    fixed records, each directly after the one before, in the order
    below, with nothing after the last. Every integer is little-endian,
    the offsets below are byte offsets within the record they name, and
    the reader reads each field byte by byte, so no record needs
    alignment. Lengths and coordinates are in font units, y growing
    upward from the baseline, as in TrueType. */

#ifndef NOVA_FONT_FORMAT_H
#define NOVA_FONT_FORMAT_H

/** @name The header
    FONT_HEADER_SIZE bytes at the start of the file. The counts give the
    length of each array after it. The metrics are the source font's
    typographic ones (OpenType OS/2 sTypoAscender, sTypoDescender,
    sTypoLineGap, sxHeight, sCapHeight); the descender is negative below
    the baseline. */
/** @{ */
#define FONT_MAGIC "NOVAFONT"      /**< the file's first bytes */
#define FONT_MAGIC_SIZE 8          /**< bytes of FONT_MAGIC */
#define FONT_VERSION 1             /**< the version this header gives */
#define FONT_HEADER_MAGIC 0        /**< FONT_MAGIC */
#define FONT_HEADER_VERSION 8      /**< 32 bits: FONT_VERSION */
#define FONT_HEADER_UNITS 12       /**< 16 bits: font units to the em */
#define FONT_HEADER_ASCENDER 14    /**< 16 bits, signed */
#define FONT_HEADER_DESCENDER 16   /**< 16 bits, signed */
#define FONT_HEADER_LINE_GAP 18    /**< 16 bits, signed */
#define FONT_HEADER_X_HEIGHT 20    /**< 16 bits, signed */
#define FONT_HEADER_CAP_HEIGHT 22  /**< 16 bits, signed */
#define FONT_HEADER_GLYPHS 24      /**< 16 bits: glyph records, at least 1 */
#define FONT_HEADER_RANGES 26      /**< 16 bits: range records */
#define FONT_HEADER_LEFT_CLASSES 28  /**< 16 bits: kerning's first classes */
#define FONT_HEADER_RIGHT_CLASSES 30 /**< 16 bits: kerning's second classes */
#define FONT_HEADER_CONTOURS 32    /**< 32 bits: contour ends */
#define FONT_HEADER_POINTS 36      /**< 32 bits: points, and point flags */
#define FONT_HEADER_SIZE 40        /**< bytes of the header */
/** @} */

/** @name Ranges
    The character map: runs of consecutive code points, each mapped to
    consecutive glyphs from its first. Ranges are sorted by their first
    code point and do not overlap; a code point no range holds has no
    glyph of its own and is drawn as glyph 0, the missing-glyph shape. */
/** @{ */
#define FONT_RANGE_FIRST 0  /**< 32 bits: the first code point */
#define FONT_RANGE_COUNT 4  /**< 16 bits: how many, at least 1 */
#define FONT_RANGE_GLYPH 6  /**< 16 bits: the first code point's glyph */
#define FONT_RANGE_SIZE 8   /**< bytes of a range */
/** @} */

/** @name Glyphs
    One record per glyph, glyph 0 first. A glyph's contours are the
    contour ends from its first, contours of them, and every glyph's
    first is the sum of the counts of the glyphs before it. Its classes
    pick its row of the kerning matrix when it comes first in a pair and
    its column when it comes second. */
/** @{ */
#define FONT_GLYPH_ADVANCE 0       /**< 16 bits: the advance width */
#define FONT_GLYPH_LEFT_CLASS 2    /**< 16 bits: below the left classes */
#define FONT_GLYPH_RIGHT_CLASS 4   /**< 16 bits: below the right classes */
#define FONT_GLYPH_CONTOURS 6      /**< 16 bits: how many contours */
#define FONT_GLYPH_FIRST_CONTOUR 8 /**< 32 bits: the first contour end's index */
#define FONT_GLYPH_SIZE 12         /**< bytes of a glyph record */
/** @} */

/** @name Contour ends
    For each contour, 32 bits: the index one past its last point. Each
    contour holds at least one point, so the ends rise strictly, and the
    last is the point count. Contour c holds the points from the end of
    contour c - 1, or 0, up to its own end. */
/** @{ */
#define FONT_CONTOUR_SIZE 4 /**< bytes of a contour end */
/** @} */

/** @name Points
    Each point's x and y, 16 bits each and signed. A contour's points
    are TrueType's: a quadratic Bezier passes through each point on the
    curve and is pulled toward each point off it, and between two
    consecutive points off the curve lies one on it at their midpoint.
    The last point joins the first. */
/** @{ */
#define FONT_POINT_X 0    /**< 16 bits, signed */
#define FONT_POINT_Y 2    /**< 16 bits, signed */
#define FONT_POINT_SIZE 4 /**< bytes of a point */
/** @} */

/** @name Kerning
    The matrix of pair adjustments: 16 bits each, signed, left classes
    rows of right classes columns. A pair's adjustment, in font units,
    is added to its first glyph's advance. Class 0 of each side is the
    glyphs that kern with none, so its row and its column are zero. */
/** @{ */
#define FONT_KERN_SIZE 2 /**< bytes of an adjustment */
/** @} */

/** @name Point flags
    A byte per point, in the points' order: FONT_ON_CURVE, or 0 for a
    point off the curve. No other bit is set. */
/** @{ */
#define FONT_ON_CURVE 1   /**< the point lies on the curve */
#define FONT_FLAG_SIZE 1  /**< bytes of a point's flags */
/** @} */

#endif
