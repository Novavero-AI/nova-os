# Console font

The console's typeface: monospace, printable ASCII, every glyph a few
strokes of one width in the geometry of the wordmark the boot screen
draws, capsules, rings and arcs, with round ends. The glyphs are data,
`gfx/glyphs.h`; `gfx/font.c` renders them with `gfx/raster.c`, and
`font.py` is the prototype that renders them in the same integer
arithmetic.

## Metrics

A cell is 480 font units wide and 960 tall. Each stroke's outer edge
lies on the metric, so its centre line lies 30 units, the stroke's
radius, inside it:

| Metric | Units | 8-pixel cell | 16-pixel cell |
|---|---|---|---|
| Advance | 480 | 8 | 16 |
| Ascender, above the baseline | 600 | 10 | 20 |
| Cap height | 540 | 9 | 18 |
| x-height | 360 | 6 | 12 |
| Descender, below the baseline | 180 | 3 | 6 |
| Line gap, 120 above the ascender and 60 below the descender | 180 | 3 | 6 |
| Stroke width | 60 | 1 | 2 |

The baseline lies 720 units below the cell's top. A pixel is 60 units in
an 8-pixel cell and 30 in a 16-pixel one, so a horizontal or vertical
stroke whose centre line lies on an odd multiple of 30 covers whole
pixels at both sizes; the strokes keep to those lines wherever a glyph
allows. Round lowercase letters are circles the x-height tall, as the
wordmark's are; capitals and figures are as wide, so their rounds are
stadiums, two half circles joined by straight sides.

Arcs (`ARC` in `gfx/raster.h`) are the one shape the font adds to the
wordmark's: `c`, `e`, `s`, `f`, `t`, the capitals' stadiums, the curved
figures, the brackets and `?`, `&` and `~` need a circle cut at angles
other than the horizontal of the wordmark's `n`.

## Rendering

The prototype needs Python 3 and its standard library:

```sh
python3 design/font/font.py specimen specimen.png  # every glyph, then text at 16 and 8 pixels
python3 design/font/font.py atlas 16 atlas.pgm     # every mask, as the check compares
```

## Checking the port

```sh
design/font/check-font.sh          # 8- and 16-pixel cells
design/font/check-font.sh 10 24    # other widths
```

`check-font.sh` builds `render-font.c` with `gfx/font.c` and
`gfx/raster.c` for the host, with `$CC` or else `cc`, renders every mask
with it and with `font.py` for each cell width, and compares the two
atlases byte for byte. It prints `identical` or how many bytes differ
for each width, and exits nonzero if any differ.
