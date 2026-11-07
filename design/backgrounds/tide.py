"""Tide: a horizon over dark water, lit by a glow just below the horizon.

The sky is inverse-square falloffs above the horizon line. The sea is a
mirror of that sky: a row dz pixels below the horizon reflects the sky row
dz above it, shifted by the local wave slope (a facet tilted by theta moves
the reflection by about 2 * theta * focal length, the same number of pixels
at any distance). Wave slope is the analytic gradient of value noise laid
on the water plane: depth Z = K / dz, lateral X = x / dz. Each octave fades
as its on-screen frequency nears the pixel pitch (K / dz^2 cells per row),
the integer form of mip filtering, so distant water settles into a smooth
mirror instead of aliasing. The glitter path is not drawn: it emerges where
tilted facets catch the glow. A Fresnel term dims the near water.
"""

import sys

from common import H, W, clamp, lorentz, vnoise_grad

HORIZON = H * 60 // 100
LIGHT_X = W * 63 // 100

SKY_FALL = (H // 7) ** 2
GLOW_FALL = (H // 16) ** 2
LINE_FALL = (H // 260) ** 2
GLOW_ALONG = (W * 22 // 100) ** 2
LINE_ALONG = (W * 60 // 100) ** 2
FRESNEL_FALL = (H // 7) ** 2

SKY = (28 * 256, 32 * 256, 38 * 256)
GLOW = (78 * 256, 74 * 256, 68 * 256)
LINE = (30 * 256, 30 * 256, 30 * 256)
DEEP = (2 * 256, 3 * 256, 4 * 256)

# Reflectance, Q16: grazing water near the horizon against steep near water.
FRESNEL_FAR = 52000
FRESNEL_NEAR = 12000

# Ripple lattice: vertical cells per row is RIPPLE_Z / dz^2, so the nearest
# row (dz = 0.4H) sees ripple cells about 28 px tall.
RIPPLE_Z = (H * 4 // 10) ** 2 // 28
RIPPLE_X = 131072 * H // 1600  # 2.0 in Q16: cells stretch along the horizon
RATE_HALF = 65536 * 35 // 100  # octave fades to half at 0.35 cells per row
# Reflection shift per unit of noise slope (Q16 cells), vertical and lateral.
SLOPE_Y = H * 2 // 7
SLOPE_X = W // 25

_GLOW_COL = [lorentz(x - LIGHT_X, GLOW_ALONG) for x in range(W)]
_LINE_COL = [lorentz(x - LIGHT_X, LINE_ALONG) for x in range(W)]

# Grain for common.finish: peak amplitude in 1/256 of an 8-bit step.
GRAIN = 200


def fade(rate):
    """Octave weight: squared inverse-square in the cell rate, so an octave
    is gone well before its cells shrink below a pixel."""
    w = lorentz(rate, RATE_HALF * RATE_HALF)
    return w * w >> 16


def sky(x, dy):
    s = lorentz(dy, SKY_FALL)
    glow = lorentz(dy, GLOW_FALL) * _GLOW_COL[x] >> 16
    line = lorentz(dy, LINE_FALL) * _LINE_COL[x] >> 16
    return tuple((SKY[k] * s + GLOW[k] * glow + LINE[k] * line) >> 16 for k in range(3))


def shade(x, y):
    if y < HORIZON:
        return sky(x, HORIZON - y)
    dz = y - HORIZON + 1
    zq = (RIPPLE_Z << 16) // dz
    xq = (x - LIGHT_X) * RIPPLE_X // dz
    rate = (RIPPLE_Z << 16) // (dz * dz)
    w0 = fade(rate >> 1)
    w1 = fade(rate)
    w2 = fade(rate << 1)
    g0x, g0y = vnoise_grad(xq >> 1, zq >> 1, 10)
    g1x, g1y = vnoise_grad(xq + 0x3C6EF3, zq + 0x5A827A, 11)
    g2x, g2y = vnoise_grad(2 * xq + 0x9E3779, 2 * zq + 0x7F4A7C, 12)
    # Each octave doubles the frequency and halves the height, so all three
    # carry the same slope weight.
    gy = (g0y * w0 + g1y * w1 + g2y * w2) >> 16
    gx = (g0x * w0 + g1x * w1 + g2x * w2) >> 16
    # A facet steep enough to aim below the horizon sees the far water, which
    # mirrors the horizon itself, so the lookup clamps there.
    dy = max(0, dz + (SLOPE_Y * gy >> 16))
    xr = clamp(x + (SLOPE_X * gx >> 16), 0, W - 1)
    fresnel = FRESNEL_NEAR + ((FRESNEL_FAR - FRESNEL_NEAR) * lorentz(dz, FRESNEL_FALL) >> 16)
    mirror = sky(xr, dy)
    return tuple(DEEP[k] + (mirror[k] * fresnel >> 16) for k in range(3))


if __name__ == "__main__":
    import common

    if sys.argv[1:] == ["preview"]:
        common.preview("tide", common.OUT + "/check/preview-tide.png", 2)
    else:
        # Crop: upper-left sky, the darkest stretch of the gradient.
        common.main(2, "tide", (100, 300, 800, 500), GRAIN)
