"""Shared integer helpers, the finishing pipeline and PNG output for the
nova-os background prototypes.

Every per-pixel function here uses integers only, in the widths a C port
needs: hashes wrap at 32 bits, fixed-point products fit in int64. Colour is
carried as "v16": an 8-bit channel value times 256, so a shade function can
express fractions of one 8-bit step and the finisher dithers them away.
"""

import math
import multiprocessing
import os
import struct
import sys
import zlib

W = 2560
H = 1600

M32 = 0xFFFFFFFF
ONE = 1 << 16  # Q16 one

# Renders are written to the current directory.
OUT = os.getcwd()

# R2 low-discrepancy sequence constants (1/g and 1/g^2, g the plastic number)
# scaled to 2^32. frac(x/g + y/g^2) is an ordered-dither threshold with a
# blue-noise-like spectrum, two multiplies per pixel.
R2X = 3242174889
R2Y = 2447445414


# ---------------------------------------------------------------------------
# Integer primitives


def hash32(x):
    """lowbias32 (Wellons): a 32-bit integer avalanche hash."""
    x &= M32
    x ^= x >> 16
    x = (x * 0x7FEB352D) & M32
    x ^= x >> 15
    x = (x * 0x846CA68B) & M32
    x ^= x >> 16
    return x


def hash3(a, b, c):
    return hash32((a * 0x8DA6B343) ^ (b * 0xD8163841) ^ (c * 0xCB1AB31F))


def smooth16(f):
    """Cubic smoothstep of a Q16 fraction in [0, ONE]."""
    return ((f * f) >> 16) * (3 * ONE - 2 * f) >> 16


def vnoise(px, py, seed):
    """Value noise at Q16 lattice coordinates; returns Q16 in [0, ONE)."""
    ix = px >> 16
    iy = py >> 16
    sx = smooth16(px & 0xFFFF)
    sy = smooth16(py & 0xFFFF)
    a = hash3(ix, iy, seed) >> 16
    b = hash3(ix + 1, iy, seed) >> 16
    c = hash3(ix, iy + 1, seed) >> 16
    d = hash3(ix + 1, iy + 1, seed) >> 16
    ab = a + (((b - a) * sx) >> 16)
    cd = c + (((d - c) * sx) >> 16)
    return ab + (((cd - ab) * sy) >> 16)


def vnoise_grad(px, py, seed):
    """Value noise slope at Q16 lattice coordinates: (d/dx, d/dy) in Q16
    per cell, exact for the cubic-smoothstep interpolant."""
    ix = px >> 16
    iy = py >> 16
    fx = px & 0xFFFF
    fy = py & 0xFFFF
    sx = smooth16(fx)
    sy = smooth16(fy)
    dsx = (6 * fx * (ONE - fx)) >> 16
    dsy = (6 * fy * (ONE - fy)) >> 16
    a = hash3(ix, iy, seed) >> 16
    b = hash3(ix + 1, iy, seed) >> 16
    c = hash3(ix, iy + 1, seed) >> 16
    d = hash3(ix + 1, iy + 1, seed) >> 16
    ab = a + (((b - a) * sx) >> 16)
    cd = c + (((d - c) * sx) >> 16)
    gx = (((b - a) * (ONE - sy) + (d - c) * sy) >> 16) * dsx >> 16
    gy = (cd - ab) * dsy >> 16
    return gx, gy


def isin(a):
    """Sine of a Q16 angle in turns (65536 = one full turn), Q16 result.

    Bhaskara I's rational approximation, 16P / (5N^2 - 4P) with
    P = a(N - a) over a half turn N: one division, error under 0.002.
    """
    a &= 0xFFFF
    n = 32768
    if a < n:
        p = a * (n - a)
        return (16 * p << 16) // (5 * n * n - 4 * p)
    a -= n
    p = a * (n - a)
    return -((16 * p << 16) // (5 * n * n - 4 * p))


def icos(a):
    return isin(a + 16384)


def lorentz(d, w2):
    """w2 / (w2 + d^2) in Q16: inverse-square light falloff, w2 = width^2."""
    return (w2 << 16) // (w2 + d * d)


def clamp(v, lo, hi):
    if v < lo:
        return lo
    if v > hi:
        return hi
    return v


# ---------------------------------------------------------------------------
# Finishing: grain, ordered dither, quantise


def finish(x, y, rgb, grain):
    """Quantise v16 colour to 8 bits.

    grain is the peak amplitude of a triangular (TPDF) hash noise in 1/256
    of an 8-bit step, shared by all three channels so it adds no chroma
    speckle. The R2 threshold then rounds the remaining fraction.
    """
    t = ((x * R2X + y * R2Y) & M32) >> 24
    if grain:
        h = hash3(x, y, 0x6E6F7661)
        g = (((h & 0xFF) + ((h >> 8) & 0xFF) - 255) * grain) >> 8
    else:
        g = 0
    r, gg, b = rgb
    return (
        clamp((r + g + t) >> 8, 0, 255),
        clamp((gg + g + t) >> 8, 0, 255),
        clamp((b + g + t) >> 8, 0, 255),
    )


def quantise_plain(rgb):
    """Round to nearest with no dither: the banding control."""
    return tuple(clamp((c + 128) >> 8, 0, 255) for c in rgb)


# ---------------------------------------------------------------------------
# Rendering

_shade = None
_grain = 0


def _init(module_name, grain):
    global _shade, _grain
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    mod = __import__(module_name)
    _shade = mod.shade
    _grain = grain


def _rows(span):
    y0, y1 = span
    out = bytearray()
    shade = _shade
    grain = _grain
    for y in range(y0, y1):
        for x in range(W):
            out.extend(finish(x, y, shade(x, y), grain))
    return bytes(out)


def render(module_name, grain, workers=7):
    """Render the full frame with the module's shade(x, y); returns RGB rows."""
    step = 16
    spans = [(y, min(y + step, H)) for y in range(0, H, step)]
    with multiprocessing.get_context("spawn").Pool(
        workers, initializer=_init, initargs=(module_name, grain)
    ) as pool:
        chunks = pool.map(_rows, spans)
    return bytearray(b"".join(chunks))


def render_box(shade, box, dithered, grain):
    x0, y0, w, h = box
    out = bytearray()
    for y in range(y0, y0 + h):
        for x in range(x0, x0 + w):
            rgb = shade(x, y)
            out.extend(finish(x, y, rgb, grain) if dithered else quantise_plain(rgb))
    return out


# ---------------------------------------------------------------------------
# PNG


def write_png(path, w, h, rgb):
    raw = bytearray()
    stride = w * 3
    for y in range(h):
        raw.append(0)
        raw.extend(rgb[y * stride : (y + 1) * stride])

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body) & M32)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def crop(img, w, box):
    x0, y0, cw, ch = box
    out = bytearray()
    for y in range(y0, y0 + ch):
        start = (y * w + x0) * 3
        out.extend(img[start : start + cw * 3])
    return out


def stretch(img):
    """Map the crop's 0.5th..99.5th percentile to 0..255, clipping outside,
    so one-step bands in the gradient become obvious even when a bright
    line or highlight shares the crop."""
    ordered = sorted(img)
    lo = ordered[len(ordered) // 200]
    hi = ordered[len(ordered) - 1 - len(ordered) // 200]
    span = max(1, hi - lo)
    return bytearray(clamp((v - lo) * 255 // span, 0, 255) for v in img), lo, hi


def mean_run(img, w, h):
    """Mean length of runs of identical RGB along rows: a band-width proxy."""
    runs = 0
    for y in range(h):
        prev = None
        for x in range(w):
            i = (y * w + x) * 3
            px = bytes(img[i : i + 3])
            if px != prev:
                runs += 1
                prev = px
    return (w * h) / runs


# ---------------------------------------------------------------------------
# Wordmark overlay (mockup only, so plain float geometry)


def _seg_dist(px, py, ax, ay, bx, by):
    vx, vy = bx - ax, by - ay
    t = ((px - ax) * vx + (py - ay) * vy) / (vx * vx + vy * vy)
    t = max(0.0, min(1.0, t))
    return math.hypot(px - ax - t * vx, py - ay - t * vy)


def _wordmark_strokes(w, h):
    xh = w / 22.0
    sw = xh / 14.0
    yc = 0.44 * h
    top, bot = yc - xh / 2, yc + xh / 2
    gap = 0.22 * xh
    widths = [0.78 * xh, xh, 0.86 * xh, xh]
    total = sum(widths) + 3 * gap
    x = w / 2 - total / 2
    half = sw / 2
    segs, arcs = [], []  # arcs: (cx, cy, r, upper_only)
    # n: left stem, upper semicircle, right stem
    wn = widths[0]
    r = (wn - sw) / 2
    cxn = x + wn / 2
    segs.append((x + half, top + half, x + half, bot))
    segs.append((x + wn - half, top + r + half, x + wn - half, bot))
    arcs.append((cxn, top + r + half, r, True))
    x += wn + gap
    # o
    arcs.append((x + xh / 2, yc, (xh - sw) / 2, False))
    x += xh + gap
    # v
    wv = widths[2]
    segs.append((x + half, top + half, x + wv / 2, bot - half))
    segs.append((x + wv - half, top + half, x + wv / 2, bot - half))
    x += wv + gap
    # a: single-storey bowl and right stem
    arcs.append((x + xh / 2 - half, yc, (xh - sw) / 2 - half, False))
    segs.append((x + xh - half, top + half, x + xh - half, bot))
    box = (int(w / 2 - total / 2) - 4, int(top) - 4, int(total) + 8, int(xh) + 8)
    return segs, arcs, sw, box


def overlay_wordmark(img, w, h, opacity=0.5):
    out = bytearray(img)
    segs, arcs, sw, (bx, by, bw, bh) = _wordmark_strokes(w, h)
    for y in range(by, by + bh):
        for x in range(bx, bx + bw):
            px, py = x + 0.5, y + 0.5
            d = 1e9
            for s in segs:
                d = min(d, _seg_dist(px, py, *s))
            for cx, cy, r, upper in arcs:
                if upper and py > cy:
                    continue
                d = min(d, abs(math.hypot(px - cx, py - cy) - r))
            a = max(0.0, min(1.0, sw / 2 - d + 0.5)) * opacity
            if a > 0:
                i = (y * w + x) * 3
                for k in range(3):
                    out[i + k] = int(out[i + k] + (255 - out[i + k]) * a + 0.5)
    return out


# ---------------------------------------------------------------------------
# Driver


def preview(module_name, path, step=4):
    """Point-sample every step-th pixel for fast tuning."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    mod = __import__(module_name)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    pw, ph = W // step, H // step
    out = bytearray()
    for y in range(ph):
        for x in range(pw):
            out.extend(finish(x * step, y * step, mod.shade(x * step, y * step), 0))
    write_png(path, pw, ph, out)


def main(n, module_name, crop_box, grain):
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    mod = __import__(module_name)
    img = render(module_name, grain)
    base = os.path.join(OUT, "bg-%d" % n)
    write_png(base + ".png", W, H, img)
    c = crop(img, W, crop_box)
    write_png(base + "-crop.png", crop_box[2], crop_box[3], c)
    write_png(base + "-with-mark.png", W, H, overlay_wordmark(img, W, H))
    control = render_box(mod.shade, crop_box, False, 0)
    s_d, lo_d, hi_d = stretch(c)
    s_c, lo_c, hi_c = stretch(control)
    os.makedirs(os.path.join(OUT, "check"), exist_ok=True)
    chk = os.path.join(OUT, "check", "bg-%d" % n)
    write_png(chk + "-crop-stretched.png", crop_box[2], crop_box[3], s_d)
    write_png(chk + "-control-stretched.png", crop_box[2], crop_box[3], s_c)
    print(
        "bg-%d crop %s: dithered levels %d..%d mean run %.2f px; "
        "undithered control levels %d..%d mean run %.2f px"
        % (
            n,
            crop_box,
            lo_d,
            hi_d,
            mean_run(c, crop_box[2], crop_box[3]),
            lo_c,
            hi_c,
            mean_run(control, crop_box[2], crop_box[3]),
        )
    )
