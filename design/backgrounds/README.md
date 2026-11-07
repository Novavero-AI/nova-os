# Desktop backgrounds

Prototypes of the desktop background candidates, in Python. Every pixel is
computed in integers of the widths C has, so a port draws the same
pixels; `gfx/limb.c` is the port of Limb, the default.

| Script | N | Background |
|---|---|---|
| `limb.py` | 1 | Limb: the night side of a planet whose edge catches the first light |
| `tide.py` | 2 | Tide: a horizon over dark water, lit by a glow just below it |
| `corona.py` | 3 | Corona: a total eclipse high on the right, the only light in a black sky |
| `orbit.py` | 4 | Orbit: hairline orbits around an unseen star, one lit segment echoing the boot spinner |

`common.py` holds the integer helpers they share, the finish every pixel
goes through (grain and an ordered dither, then rounding to 8 bits) and
the PNG writer.

## Rendering

The scripts need Python 3 and its standard library. Each renders at
2560 x 1600 (`W` and `H` in `common.py`) into the directory it is run
from:

```sh
python3 path/to/design/backgrounds/limb.py           # the full render
python3 path/to/design/backgrounds/limb.py preview   # a quick, smaller one
```

A full render writes `bg-N.png`, a 1:1 crop `bg-N-crop.png` and
`bg-N-with-mark.png` with the wordmark over it, and in `check/` the crop
and an undithered control with their contrast stretched; it prints the
mean run of equal pixels in each, a measure of banding. A preview samples
every second or fourth pixel into `check/preview-NAME.png`.

`render.py NAME WIDTH HEIGHT OUT.ppm` renders one candidate at another
size as a binary PPM. A size too small for one of a candidate's glows to
have a width fails with Python's error.

## Checking the port

```sh
design/backgrounds/check-limb.sh                # 2560x1600 and 1920x1080
design/backgrounds/check-limb.sh 1280x800 ...   # other sizes
```

`check-limb.sh` builds `render-limb.c` with `gfx/limb.c` for the host,
with `$CC` or else `cc`, renders Limb with it and with `limb.py` at each
size, and compares the two images byte for byte. It prints `identical` or
how many bytes differ for each size, and exits nonzero if any differ.
