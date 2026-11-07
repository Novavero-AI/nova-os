"""Orbit: hairline orbits around an unseen star below the screen, one lit
segment echoing the boot spinner's arc.

One integer square root per pixel gives the radius rho in Q8 pixels; every
ring is then a subtraction and a coverage ramp (a hairline of width LINE,
anti-aliased over one pixel). Rings and the background glow are lit by the
star with an inverse-square falloff. The lit segment measures its angle
behind the head without atan2: with the head direction as a unit vector,
the cross product over rho is sin(angle), and a dot product picks the
correct half-plane.
"""

import math
import sys

from common import H, ONE, W, lorentz

CX = W * 80 // 100
CY = H * 118 // 100

RINGS = [H * 30 // 100, H * 48 // 100, H * 66 // 100, H * 110 // 100, H * 142 // 100]
RINGS_Q8 = [r << 8 for r in RINGS]
LINE = H * 256 // 1300  # hairline width, Q8 pixels (about 1.2 px)

STAR_FALL = (H * 256 * 80 // 100) ** 2  # Q8 pixels squared
GLOW_FALL = (H * 256 * 40 // 100) ** 2

RING = (58 * 256, 60 * 256, 64 * 256)  # at the star; dimmer outward
GLOW = (14 * 256, 16 * 256, 21 * 256)
BASE = (1 * 256, 1 * 256, 2 * 256)

# Lit segment: on ring 3, head pointing up and a little left of the star.
LIT_RING = 3
HEAD_ANGLE = math.radians(-112)  # screen angle, y down
HX = int(round(math.cos(HEAD_ANGLE) * ONE))
HY = int(round(math.sin(HEAD_ANGLE) * ONE))
TAIL_SIN = int(0.97 * ONE)  # tail spans to about 75 degrees behind the head
HEAD = (235 * 256, 238 * 256, 242 * 256)
HEAD_GLOW = (30 * 256, 34 * 256, 44 * 256)
HEAD_GLOW_FALL = (H * 256 // 70) ** 2
HEAD_POS = (CX * 256 + RINGS_Q8[LIT_RING] * HX // ONE, CY * 256 + RINGS_Q8[LIT_RING] * HY // ONE)
HEAD_TIP = 3 * 256  # head end softens over 3 px

# Grain for common.finish: peak amplitude in 1/256 of an 8-bit step.
GRAIN = 200


def coverage(dist):
    """Coverage of a hairline of width LINE at Q8 distance dist from its centre."""
    c = (LINE // 2 + 128 - abs(dist)) * ONE // 256
    if c <= 0:
        return 0
    return min(c, ONE)


def shade(x, y):
    dx = x - CX
    dy = y - CY
    rho = math.isqrt((dx * dx + dy * dy) << 16)  # Q8 pixels
    star = lorentz(rho, STAR_FALL)
    out = [BASE[k] + (GLOW[k] * lorentz(rho, GLOW_FALL) >> 16) for k in range(3)]
    for i, r in enumerate(RINGS_Q8):
        cov = coverage(rho - r)
        if cov == 0:
            continue
        if i == LIT_RING and rho:
            # sin of the angle behind the head; tail trails clockwise.
            behind = ((dy * HX - dx * HY) << 8) // rho
            ahead = (dx * HX + dy * HY) > 0
            if ahead and behind > -HEAD_TIP * ONE // r:
                t = max(0, behind) * ONE // TAIL_SIN
                tail = 0 if t >= ONE else (ONE - t) * (ONE - t) >> 16
                if behind < 0:
                    tail = tail * (ONE + behind * r // HEAD_TIP) >> 16
                lit = cov * tail >> 16
                for k in range(3):
                    out[k] += HEAD[k] * lit >> 16
        amt = cov * star >> 16
        for k in range(3):
            out[k] += RING[k] * amt >> 16
    hx = (x << 8) - HEAD_POS[0]
    hy = (y << 8) - HEAD_POS[1]
    glow = lorentz(math.isqrt(hx * hx + hy * hy), HEAD_GLOW_FALL)
    for k in range(3):
        out[k] += HEAD_GLOW[k] * glow >> 16
    return tuple(out)


if __name__ == "__main__":
    import common

    if sys.argv[1:] == ["preview"]:
        common.preview("orbit", common.OUT + "/check/preview-orbit.png", 2)
    else:
        common.main(4, "orbit", (1500, 900, 800, 500), GRAIN)
