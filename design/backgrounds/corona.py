"""Corona: a total eclipse high on the right, the only light in a black sky.

Radius rho comes from one integer square root (Q8 pixels). The corona is
inverse-square falloffs outward from the limb. Its streamers need the
angle around the disc but no atan2: value noise sampled at the unit
direction (dx, dy) / rho, scaled to a circle K cells across, varies with
angle only, so each octave draws radial filaments, finer as K grows. The
streamers taper in, growing stronger with distance from the limb, and are
weighted towards a tilted equator for the corona's natural flattening. A
hairline ring at the limb is the chromosphere.
"""

import math
import sys

from common import H, ONE, W, lorentz, vnoise

CX = W * 78 // 100
CY = H * 25 // 100
R = H * 88 // 1000  # disc radius, pixels
RQ = R << 8

RING_FALL = (H * 256 // 1100) ** 2  # chromosphere hairline
INNER_FALL = (RQ * 12 // 100) ** 2
MID_FALL = (RQ * 70 // 100) ** 2
# Outer corona width runs from OUTER_POLE at the poles to OUTER_POLE +
# OUTER_EQ on the equator. Mid and outer falloffs are squared (d^-4), as
# the real corona's steep decline, so both are gone well inside REACH.
OUTER_POLE = RQ * 40 // 100
OUTER_EQ = RQ * 170 // 100
SKY_FALL = (RQ * 5) ** 2

RING = (255 * 256, 250 * 256, 240 * 256)
INNER = (110 * 256, 112 * 256, 118 * 256)
MID = (32 * 256, 35 * 256, 42 * 256)
OUTER = (30 * 256, 33 * 256, 42 * 256)
SKY = (7 * 256, 9 * 256, 15 * 256)
BASE = (1 * 256, 1 * 256, 2 * 256)
DISC = (1 * 256, 1 * 256, 2 * 256)

# Streamer octaves: (cells around the circle, weight Q16, seed).
OCTAVES = [(4, 34000, 31), (9, 20000, 32), (23, 10000, 33), (60, 5000, 34)]
# Equator tilt as a Q16 unit vector; streamers favour this axis.
EQ = (int(round(math.cos(0.9) * ONE)), int(round(math.sin(0.9) * ONE)))
REACH = RQ * 6  # beyond this every corona term is under a quarter step

# Grain for common.finish: peak amplitude in 1/256 of an 8-bit step.
GRAIN = 220


def shade(x, y):
    dx = x - CX
    dy = y - CY
    rho = math.isqrt((dx * dx + dy * dy) << 16)
    sky = lorentz(rho, SKY_FALL)
    out = [BASE[k] + (SKY[k] * sky >> 16) for k in range(3)]
    d = rho - RQ
    if d < -256:
        return DISC
    ring = lorentz(d, RING_FALL)
    if d < 0:
        # Disc edge: coverage blend with the ring on top.
        cover = (d + 256) * ONE // 256
        out = [(DISC[k] * (ONE - cover) + out[k] * cover) >> 16 for k in range(3)]
        return tuple(out[k] + (RING[k] * ring * cover >> 32) for k in range(3))
    if rho < REACH:
        # Unit direction in Q16; noise on a circle of K cells.
        ux = (dx << 24) // rho
        uy = (dy << 24) // rho
        streak = 0
        for cells, weight, seed in OCTAVES:
            n = vnoise(ux * cells, uy * cells, seed) - 32768
            streak += n * weight >> 15
        # Equator weighting: |dot(u, EQ)| in Q16.
        eq = abs(ux * EQ[0] + uy * EQ[1]) >> 16
        eq2 = eq * eq >> 16
        taper = ONE - lorentz(d, INNER_FALL)
        mod = max(0, ONE + (streak * taper >> 16))
        inner = lorentz(d, INNER_FALL)
        mid = lorentz(d, MID_FALL)
        mid = (mid * mid >> 16) * mod >> 16
        width = OUTER_POLE + (OUTER_EQ * eq2 >> 16)
        outer = lorentz(d, width * width)
        outer = (outer * outer >> 16) * mod >> 16
        for k in range(3):
            out[k] += (INNER[k] * inner + MID[k] * mid + OUTER[k] * outer) >> 16
    return tuple(out[k] + (RING[k] * ring >> 16) for k in range(3))


if __name__ == "__main__":
    import common

    if sys.argv[1:] == ["preview"]:
        common.preview("corona", common.OUT + "/check/preview-corona.png", 2)
    else:
        common.main(3, "corona", (600, 650, 800, 500), GRAIN)
