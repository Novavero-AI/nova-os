# Text and filled outlines

The prototypes of `lib/outline.c`, which fills paths of lines and
Bezier curves, and of `lib/typeface.c` and `lib/typeset.c`, which read
a nova-os font ([docs/spec/font-format.md](../../docs/spec/font-format.md))
and set text in it: `outline.py` and `typeface.py`, in the same integer
arithmetic, so the C draws the same pixels. `sheet.txt` is a reference
sheet of the approved designs' faces and sizes and of their rounded
rectangles and circles over Limb.

## The rasterizer

A curve is cut into lines that stray from it by at most a sixteenth of
a pixel. Each pixel row is then cut into slices at every line's ends
and at every crossing of two lines within the row. Within a slice the
lines keep their order across it, so counting their directions from the
left gives the winding number between each two, and the lines where it
turns from zero to nonzero, or back, bound what the slice fills. Each
of those adds the area to its right within each pixel it passes, and
the running sum across the row is the area of each pixel inside the
outline under the nonzero rule, "A point is considered to be an
interior point of a glyph if it has a non-zero winding number"
(OpenType, TrueType fundamentals), exactly but for rounding to a 256th
of a pixel where lines are cut, at a row, a column or a crossing.
Coverage scales light, not codes, with
`gfx/raster.c`'s sRGB tables, which the boot screen, the kernel's fade
and the console use.

- **Why exact area:** Inter's static faces hold contours that overlap
  in the same direction in five of Latin-1's glyphs, U+00C7, U+00D0,
  U+00D8, U+00E7 and U+00F8 (C and c with cedilla, eth, O and o with
  stroke), where a cedilla, a bar or a stroke overlies the letter: the
  winding number there is 2. The rule must hold at every point, not
  only away from such overlaps.
- **Cost:** a row's crossings are found by testing each pair of its
  lines, and each slice's lines are sorted, so a row of k lines costs
  k squared tests; each glyph is filled on its own, which keeps k to a
  glyph's lines across a row, a dozen or two.
- **Not taken:** the signed area of every line summed into each pixel,
  with the sum's magnitude clamped, as font-rs, stb_truetype's second
  rasterizer and FreeType's smooth rasterizer compute coverage. It
  needs no sort and no crossing, and it is exact while no two contours
  overlap in the same direction; where they do, a pixel that both
  cover in part counts the area of each. Summed from the same lines at
  26 pixels, those five glyphs' overlapping pixels come out covered by
  up to 59 of 255 more, in each of the six faces shipped, while every
  other Latin-1 glyph differs by at most 1 of 255, its rounding.

## Placing text

Glyphs are placed to a 256th of a pixel across, and on whole pixel rows
down: a line's advances and kerning add up in font units, and each
point of each glyph is scaled from that exact sum, so a line drifts by
no rounding however long it is, and every line's baseline lands alike
on the pixel grid.

- **Why across:** the outlines are drawn unhinted, so nothing in them is
  fitted to whole pixels for a whole-pixel position to keep; rounding
  each glyph's position to a pixel would move it by up to half a
  pixel, a twentieth of the advance of a letter in the designs' 22 to
  26 pixel text, and spaces between letters would vary by twice that.
- **Why down:** a baseline on a pixel row puts the bottoms of letters
  that sit on it on that row's edge, the same in every line.
- **Cost:** a glyph's coverage depends on its position's fraction, so a
  cache of glyph masks would need one mask per fraction, or positions
  rounded to a few fractions; nothing caches masks yet, and each glyph
  is filled where it stands.

`lib/typeset.c` decodes UTF-8, a byte that does not start a well-formed
sequence being U+FFFD, maps each code point to its glyph, advances by
each glyph's width plus its pair's kerning, and breaks lines at spaces:
a first word wider than the line stays on it alone. It does no other
shaping: no `GSUB` substitution, so no ligatures, contextual alternates
or tabular figures; no mark attachment; left to right only, with no
bidirectional text; no hyphenation. Where neighbours overlap, each is
mixed in turn.

## Rendering

The prototypes need Python 3 and its standard library, and nova-os
fonts, as `tools/mkfont.c` writes them:

```sh
python3 -I design/text/sheet.py design/text/sheet.txt FONTS sheet.ppm
```

## Checking the port

```sh
design/text/check-text.sh FONTS
design/text/compare-freetype.sh FONTS
```

`check-text.sh` builds `render-text.c` with `lib/` for the host, with
`$CC` or else `cc`, renders the sheet with it and with the prototype,
and compares the two images byte for byte. It prints `identical` or how
many bytes differ, and exits nonzero if they differ.

`compare-freetype.sh` builds `compare-freetype.c` with FreeType and
draws each of the sheet's lines both with `lib/` and with FreeType's
unhinted coverage of the same glyphs from the TrueType fonts beside the
nova-os ones, at the same positions and mixed the same way. The two
cannot agree exactly, since their curves are cut differently and
FreeType places a glyph to a 64th of a pixel; for each line it prints
the mean and largest difference of a pixel's channels, in sRGB codes,
and fails unless the mean is at most 2 and 98 percent of the pixels
differ by at most 16.
