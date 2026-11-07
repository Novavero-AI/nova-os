"""Limb: the night side of a planet whose edge catches the first light.

A circle far larger than the screen, its crest low and right of centre.
Signed distance to the edge uses the algebraic form (q - R^2) / 2R, which
needs no square root and is exact at the edge, where precision matters.
Every glow is an inverse-square (Lorentzian) falloff across the edge times
another along it, centred on the hidden sun. Distances are Q8 pixels.
"""

import sys

from common import H, W, lorentz

R = 4 * H
CREST_Y = H * 78 // 100
CX = W * 68 // 100
CY = CREST_Y + R
TWO_R = 2 * R
SUN_X = W * 57 // 100

# Grain for common.finish: peak amplitude in 1/256 of an 8-bit step.
GRAIN = 160


def q8sq(px_num, px_den):
    """(width in Q8 pixels)^2 for a width of H * num / den pixels."""
    w = H * 256 * px_num // px_den
    return w * w


# Widths across the edge, sky side and planet side.
RIM_OUT = q8sq(1, 900)
RIM_IN = q8sq(1, 2000)
BAND_OUT = q8sq(1, 38)
BAND_IN = q8sq(1, 400)
WARM_OUT = q8sq(1, 120)
WARM_IN = q8sq(1, 1200)
HAZE_OUT = q8sq(1, 4)
NIGHT_IN = q8sq(1, 30)

# Widths along the edge (pixels, squared), all centred on the sun.
ALONG_RIM = (W * 20 // 100) ** 2
ALONG_BAND = (W * 34 // 100) ** 2
ALONG_WARM = (W * 9 // 100) ** 2
ALONG_HAZE = (W * 70 // 100) ** 2

# Peak colours, v16 (8-bit value * 256).
RIM = (240 * 256, 232 * 256, 218 * 256)
BAND = (34 * 256, 78 * 256, 140 * 256)
WARM = (205 * 256, 148 * 256, 72 * 256)
HAZE = (6 * 256, 14 * 256, 30 * 256)
NIGHT = (10 * 256, 20 * 256, 36 * 256)
GROUND = (2 * 256, 3 * 256, 5 * 256)

# Along-edge weights depend only on the column.
_COLS = []
for _x in range(W):
    _t = _x - SUN_X
    _COLS.append(
        (
            lorentz(_t, ALONG_RIM),
            lorentz(_t, ALONG_BAND),
            lorentz(_t, ALONG_WARM),
            lorentz(_t, ALONG_HAZE),
        )
    )


def shade(x, y):
    dx = x - CX
    dy = y - CY
    d = ((dx * dx + dy * dy - R * R) << 8) // TWO_R
    a_rim, a_band, a_warm, a_haze = _COLS[x]
    if d >= 0:
        rim = lorentz(d, RIM_OUT) * a_rim >> 16
        band = lorentz(d, BAND_OUT) * a_band >> 16
        warm = lorentz(d, WARM_OUT) * a_warm >> 16
        haze = lorentz(d, HAZE_OUT) * a_haze >> 16
        return tuple(
            (RIM[k] * rim + BAND[k] * band + WARM[k] * warm + HAZE[k] * haze) >> 16
            for k in range(3)
        )
    rim = lorentz(d, RIM_IN) * a_rim >> 16
    band = lorentz(d, BAND_IN) * a_band >> 16
    warm = lorentz(d, WARM_IN) * a_warm >> 16
    night = lorentz(d, NIGHT_IN) * a_haze >> 16
    return tuple(
        GROUND[k]
        + ((RIM[k] * rim + BAND[k] * band + WARM[k] * warm + NIGHT[k] * night) >> 16)
        for k in range(3)
    )


if __name__ == "__main__":
    import common

    if sys.argv[1:] == ["preview"]:
        common.preview("limb", common.OUT + "/check/preview-limb.png")
    else:
        # Crop: sky above the limb, left of the sun, where the haze fades.
        common.main(1, "limb", (200, 500, 800, 500), GRAIN)
