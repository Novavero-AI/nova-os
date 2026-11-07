# Font format

A nova-os font is one face of a typeface in the form programs draw text
with: glyph outlines, advance widths, a character map and pair kerning,
in fixed records that a program reads in place. `tools/mkfont.c` writes
it from a TrueType font when the system is built, `lib/typeface.c`
reads it, and `include/font_format.h` gives its offsets and constants.
The interface's typeface, Inter, reaches programs this way, from its
store path under `/store` ([build.sh](../../build.sh)).

Sources are pinned in [README.md](README.md) and cited by these names:
"OpenType", the OpenType specification 1.9.1, by table or page;
"FreeType", FreeType's Glyph Conventions; "OFL", the SIL Open Font
License 1.1, by condition.

## Conventions

- Integers are little-endian and unaligned; a reader reads each field
  byte by byte.
- Lengths and coordinates are in font units, the header giving the
  units to the em, with y growing upward from the baseline, as in
  TrueType (OpenType, `glyf`).
- The file is the header and then six arrays, each directly after the
  one before, in the order of the sections below, with nothing after
  the last. The header's counts give each array's length, so no offset
  is stored and a file's size follows from its header.

## Header

At offset 0, 40 bytes:

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | `magic`, `NOVAFONT` |
| 8 | 4 | `version`, 1 |
| 12 | 2 | Units to the em |
| 14 | 2 | Ascender, signed |
| 16 | 2 | Descender, signed, negative below the baseline |
| 18 | 2 | Line gap, signed |
| 20 | 2 | x-height, signed |
| 22 | 2 | Cap height, signed |
| 24 | 2 | Glyphs, at least 1 |
| 26 | 2 | Ranges |
| 28 | 2 | Left kerning classes, at least 1 |
| 30 | 2 | Right kerning classes, at least 1 |
| 32 | 4 | Contours |
| 36 | 4 | Points |

The metrics are the source font's typographic ones: `OS/2`'s
`sTypoAscender`, `sTypoDescender`, `sTypoLineGap`, `sxHeight` and
`sCapHeight` (OpenType, `OS/2`). A line's height is the ascender less
the descender plus the line gap.

## Ranges

The character map, one 8-byte record per range:

| Offset | Size | Field |
|---|---|---|
| 0 | 4 | The first code point |
| 4 | 2 | How many, at least 1 |
| 6 | 2 | The first code point's glyph |

Code point `first + i`, for `i` below the count, maps to glyph
`glyph + i`. Ranges are sorted by first code point and do not overlap,
and each maps only to glyphs the file holds. A code point no range
holds is drawn with glyph 0, the missing-glyph shape, as in a font's
own map: "character codes that do not correspond to any glyph in the
font should be mapped to glyph index 0" (OpenType, `cmap`).

## Glyphs

One 12-byte record per glyph, glyph 0 first:

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | The advance width |
| 2 | 2 | Left kerning class, below the left classes |
| 4 | 2 | Right kerning class, below the right classes |
| 6 | 2 | Contours |
| 8 | 4 | The index of its first contour end |

A glyph's contours follow the previous glyph's: its first index is the
sum of the counts of the glyphs before it, and the counts add up to the
header's contours. A glyph with none, a space, draws nothing.

## Contour ends

One 4-byte index per contour: the index one past its last point.
Contour `c` holds the points from contour `c - 1`'s end, or 0, up to its
own. Each contour holds at least one point, so the ends rise strictly,
and the last is the header's points.

## Points

One 4-byte record per point, its x then its y, each signed. A contour's
points are TrueType's: its curves are "second-order Bezier-splines",
and "Straight lines are defined by two consecutive on curve points"
(OpenType, TrueType fundamentals); "Two successive conic 'off' points
force the rasterizer to create ... a virtual 'on' point inbetween, at
their exact middle" (FreeType, VI). The last point joins the first. A
reader drawing a contour starts at its first point on the curve, or its
last if the first is off it, or midway between them if both are.

## Kerning

The matrix of pair adjustments: one signed 2-byte value per pair of
classes, left classes rows of right classes columns, row by row. The
adjustment of glyph `a` followed by glyph `b` is the value at `a`'s
left class's row and `b`'s right class's column, in font units, and is
added to `a`'s advance. Class 0 of each side holds the glyphs that kern
with no glyph, so its row and its column are zero.

## Point flags

One byte per point, in the points' order: 1 for a point on the curve, 0
for one off it.

## Reading

`typeface_load` takes a file only whole and exact: the magic and
version, units to the em and the glyph and class counts not zero, a
size equal to the one the header's counts give, ranges in order and
within the glyphs, each glyph's classes within the counts and its first
contour the running sum, contour ends rising to the point count, and no
flag but bit 0. Once loaded, a glyph is read with no further check.

## Conversion

`mkfont FONT OUT RANGE...` reads a font with TrueType outlines,
`sfntVersion` 0x00010000 (OpenType, "Organization of an OpenType
font"), and writes glyph 0 and the glyph of each code point in the
ranges given that the font maps, in that order:

- **Map:** `cmap`'s format 4 subtable for platform 3, encoding 1, the
  Unicode Basic Multilingual Plane (OpenType, `cmap`), so code points
  run up to U+FFFF.
- **Outlines:** `glyf` and `loca`. A composite glyph's components are
  resolved into their contours, each moved by its offset; a component
  placed by matching points, or scaled, makes the tool fail, since the
  format holds only simple outlines. Instructions, hinting's programs,
  are left out.
- **Advances:** `hmtx`.
- **Kerning:** the lookups of `GPOS`'s `kern` feature in the default
  language system of the `latn` script, or else `DFLT`, or none if
  neither has one or there is no `GPOS`, applied as a
  layout engine applies them, "in the order the lookup definitions
  occur in the LookupList", the first subtable of a lookup that matches
  ending the lookup: "processing of the lookup is finished -- no further
  subtables are processed for that position" (OpenType, common table
  formats). A pair adjustment subtable
  (lookup type 2, or 9 wrapping it) applies to a pair when its coverage
  holds the first glyph and, in format 1, its pair set holds the second;
  in format 2 the two glyphs' classes pick the adjustment (OpenType,
  `GPOS`). Only the first glyph's `XAdvance` is read; a subtable whose
  values hold anything else, a kern lookup of another type, or one that
  skips base glyphs or ligatures, makes the tool fail. So does a code
  point given whose glyph `GDEF` classes as a mark, since nothing here
  attaches marks. The converter resolves every ordered pair of the
  glyphs kept, then gives two glyphs one left class when they kern alike
  before every glyph, and one right class when they kern alike after
  every glyph: the matrix holds each pair exactly.

`GSUB` and every `GPOS` feature but `kern` are left out: no ligatures,
contextual alternates or tabular figures.

## What nova-os ships

[build.sh](../../build.sh) converts Inter 4.1, fetched from its release
and checked by the SHA-256 `SHA256SUMS` gives, as follows.

**Faces.** The six static faces the approved login and desktop designs
set type in, and no other: Inter Regular, Medium and SemiBold for text
up to 40 pixels, Inter Display Light, Regular and SemiBold for display
sizes from 44 (the designs' type scale: field values and body text,
buttons and the bar, titles and labels; the login's names and titles,
the account rows, headings). The static faces, not Inter's variable
font, because a weight between them is never drawn and a variable
outline needs `gvar`'s deltas resolved at a weight before it is drawn.

**Characters.** The Unicode blocks [ui.md](ui.md) U3 names, whole:
Latin, U+0020 to U+007E and U+00A0 to U+024F; Greek and Coptic,
U+0370 to U+03FF; Cyrillic, U+0400 to U+04FF, but for its combining
marks, U+0483 to U+0489, which this format cannot place and the tool
refuses; General Punctuation and Superscripts and Subscripts, U+2000 to
U+209F; and Currency Symbols, U+20A0 to U+20CF; with the four arrows
from U+2190 to U+2193 that the designs' key hints show. Inter maps
1,022 code points among them; one it does not map, such as U+00AD, the
soft hyphen, is drawn as glyph 0. Each face keeps 1,023 glyphs, holds
104,069 to 115,404 nonzero kerning pairs in 149 to 160 classes a side,
and is 218,790 to 223,690 bytes, against Inter's 408,972 to 419,744
for every glyph with hinting; the six together are 1,327,296.

**Licence.** Inter is licensed under the OFL. A font converted to
another format, or with glyphs removed, is a Modified Version: one
"made by adding to, deleting, or substituting -- in part or in whole
-- any of the components of the Original Version, by changing formats
or by porting the Font Software to a new environment" (OFL,
definitions). A Modified Version may not carry a Reserved Font Name
without permission (condition 3); Inter's licence names none after its
copyright statement, so the converted faces keep the name Inter. They
are redistributed with the copyright notice and the licence, which "can
be included ... as stand-alone text files" (condition 2), and under the
OFL alone (condition 5): the fonts' store path holds Inter's
`LICENSE.txt`, its copyright notice and the OFL's text, in the same
directory as the faces.
