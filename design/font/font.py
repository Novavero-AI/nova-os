"""The console font's prototype: renders the glyphs gfx/glyphs.h defines
with the rasterizer gfx/raster.c ports, in the same integer arithmetic,
so gfx/font.c's masks equal these pixel for pixel at every cell width.

    python3 font.py atlas WIDTH OUT.pgm    every mask, as check-font.sh compares
    python3 font.py specimen OUT.png       every glyph and some text, for review
"""

import os
import re
import struct
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
GLYPHS = os.path.join(HERE, "..", "..", "gfx", "glyphs.h")

# raster.h
SUBPIXEL = 8
SAMPLES = 4
COVERAGE_MAX = SAMPLES * SAMPLES
SAMPLE_REACH = 5
CAPSULE, RING, ARCH, ARC = 0, 1, 2, 3

# font.h and font.c: a cell in font units, the strokes' radius, the masks'
# codes, printable ASCII and the caret in DEL's place, and the most shapes
# one glyph may become.
UNITS_WIDE = 480
UNITS_TALL = 960
STROKE_RADIUS = 30
FIRST = 0x20
CARET = 0x7F
GLYPH_SHAPES_MAX = 32
ATLAS_COLUMNS = 16

USAGE_ERROR = 2
PGM_MAXVAL = COVERAGE_MAX

STROKE = re.compile(
    r"\{\s*('(?:\\.|[^'\\])'|CARET)\s*,\s*(LINE|RING|ARC)\s*,\s*\{([^}]*)\}\s*\}"
)


def code_of(token):
    """A C character literal of glyphs.h, or CARET, as its code."""
    if token == "CARET":
        return CARET
    body = token[1:-1]
    return ord(body[1] if body.startswith("\\") else body)


def parse(path=GLYPHS):
    """Each code's strokes, in the order glyphs.h lists them."""
    strokes = {}
    with open(path) as f:
        for match in STROKE.finditer(f.read()):
            values = [int(v) for v in match.group(3).split(",") if v.strip()]
            strokes.setdefault(code_of(match.group(1)), []).append(
                (match.group(2), values)
            )
    return strokes


def scale(v, width):
    """Font units to eighths of a pixel for cells width pixels wide,
    rounded half up, as font.c scales them."""
    return (v * width * SUBPIXEL + UNITS_WIDE // 2) // UNITS_WIDE


def shapes_of(strokes, width):
    """The SHAPEs a glyph's strokes become: a LINE a CAPSULE, a RING a
    RING, an ARC an ARC with a round cap, a CAPSULE of no length, at each
    end."""
    r = scale(STROKE_RADIUS, width)
    out = []
    for kind, v in strokes:
        s = [scale(x, width) for x in v]
        if kind == "LINE":
            out.append((CAPSULE, s[0], s[1], s[2], s[3], r, 0, 0, 0, 0))
        elif kind == "RING":
            out.append((RING, s[0], s[1], s[2], 0, r, 0, 0, 0, 0))
        else:
            cx, cy, radius, x0, y0, x1, y1 = s
            out.append((ARC, cx, cy, radius, 0, r, x0 - cx, y0 - cy, x1 - cx, y1 - cy))
            out.append((CAPSULE, x0, y0, x0, y0, r, 0, 0, 0, 0))
            out.append((CAPSULE, x1, y1, x1, y1, r, 0, 0, 0, 0))
    if len(out) > GLYPH_SHAPES_MAX:
        raise ValueError("a glyph of %d shapes, more than font.c draws" % len(out))
    return out


# ---------------------------------------------------------------------------
# raster.c's inside test and coverage


def cross(ax, ay, bx, by):
    return ax * by - ay * bx


def in_sweep(s, qx, qy):
    sx, sy, ex, ey = s[6], s[7], s[8], s[9]
    if cross(sx, sy, ex, ey) < 0:
        return not (cross(ex, ey, qx, qy) > 0 and cross(qx, qy, sx, sy) > 0)
    return cross(sx, sy, qx, qy) >= 0 and cross(qx, qy, ex, ey) >= 0


def inside(s, px, py, grow):
    kind, ax, ay, bx, by, radius = s[:6]
    qx, qy, r = px - ax, py - ay, radius + grow
    if r < 0:
        return False
    if kind == CAPSULE:
        dx, dy = bx - ax, by - ay
        dot, len2 = qx * dx + qy * dy, dx * dx + dy * dy
        if dot <= 0:
            return qx * qx + qy * qy <= r * r
        if dot >= len2:
            ex, ey = px - bx, py - by
            return ex * ex + ey * ey <= r * r
        c = qx * dy - qy * dx
        return c * c <= r * r * len2
    d2 = qx * qx + qy * qy
    lo, hi = bx - r, bx + r
    if kind == ARCH and py > ay + grow:
        return False
    if kind == ARC and (grow < 0 or (grow == 0 and not in_sweep(s, qx, qy))):
        return False
    lo = max(lo, 0)
    return lo * lo <= d2 <= hi * hi


def coverage(shapes, x, y):
    cx, cy = x * SUBPIXEL + SUBPIXEL // 2, y * SUBPIXEL + SUBPIXEL // 2
    near = False
    for s in shapes:
        if inside(s, cx, cy, -SAMPLE_REACH):
            return COVERAGE_MAX
        if inside(s, cx, cy, SAMPLE_REACH):
            near = True
    if not near:
        return 0
    hits = 0
    for sy in range(SAMPLES):
        for sx in range(SAMPLES):
            px = x * SUBPIXEL + sx * (SUBPIXEL // SAMPLES) + 1
            py = y * SUBPIXEL + sy * (SUBPIXEL // SAMPLES) + 1
            if any(inside(s, px, py, 0) for s in shapes):
                hits += 1
    return hits


def mask(strokes, width):
    """A glyph's coverage, row by row, for cells width pixels wide."""
    height = width * UNITS_TALL // UNITS_WIDE
    shapes = shapes_of(strokes, width)
    return bytes(coverage(shapes, x, y) for y in range(height) for x in range(width))


def masks(width, path=GLYPHS):
    strokes = parse(path)
    return [mask(strokes.get(code, []), width) for code in range(FIRST, CARET + 1)]


# ---------------------------------------------------------------------------
# Output


def atlas(width, out):
    """Every mask, ATLAS_COLUMNS to a row in code order, as a binary PGM
    whose values are coverage, 0..COVERAGE_MAX."""
    height = width * UNITS_TALL // UNITS_WIDE
    glyphs = masks(width)
    rows = (len(glyphs) + ATLAS_COLUMNS - 1) // ATLAS_COLUMNS
    image = bytearray(rows * height * ATLAS_COLUMNS * width)
    stride = ATLAS_COLUMNS * width
    for i, m in enumerate(glyphs):
        left, top = i % ATLAS_COLUMNS * width, i // ATLAS_COLUMNS * height
        for y in range(height):
            start = (top + y) * stride + left
            image[start : start + width] = m[y * width : (y + 1) * width]
    with open(out, "wb") as f:
        f.write(b"P5\n%d %d\n%d\n" % (stride, rows * height, PGM_MAXVAL))
        f.write(bytes(image))


def to_light(code):
    c = code / 255
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def to_code(light):
    c = light * 12.92 if light <= 0.0031308 else 1.055 * light ** (1 / 2.4) - 0.055
    return max(0, min(255, round(c * 255)))


INK = (242, 242, 246)
PAPER = (14, 16, 22)
SPECIMEN_TEXT = [
    "The quick brown fox jumps over the lazy dog.",
    "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG!",
    "0123456789 il1I| oO0 rn m ({[<>]}) @#$%^&*",
    "login: devon   password:   nova$ ls /store",
    "if (x != 0 && y <= 2) { return a[i] + b->c; }",
    "'single' \"double\" `tick` ~tilde_under; a:b, c?",
]


def write_png(path, w, h, rgb):
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw.extend(rgb[y * w * 3 : (y + 1) * w * 3])

    def chunk(tag, data):
        body = tag + data
        return struct.pack(">I", len(data)) + body + struct.pack(">I", zlib.crc32(body))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 9)))
        f.write(chunk(b"IEND", b""))


def specimen(out):
    """Every glyph at a large size, then the specimen text at the console's
    two default sizes, ink mixed with the paper in linear light."""
    blocks = []
    big = 48
    glyphs = masks(big)
    lines = [list(range(FIRST + i, min(FIRST + i + ATLAS_COLUMNS, CARET + 1)))
             for i in range(0, CARET + 1 - FIRST, ATLAS_COLUMNS)]
    blocks.append((big, glyphs, lines))
    for width in (16, 8):
        g = masks(width)
        blocks.append((width, g, [[ord(c) for c in t] for t in SPECIMEN_TEXT]))
    margin = 16
    w = margin * 2 + max(width * max(len(l) for l in lines) for width, _, lines in blocks)
    h = margin * (len(blocks) + 1) + sum(width * 2 * len(lines) for width, _, lines in blocks)
    image = bytearray(PAPER * (w * h))
    ink = [to_light(c) for c in INK]
    paper = [to_light(c) for c in PAPER]
    mixed = [[bytes(to_code(paper[k] + (ink[k] - paper[k]) * a / COVERAGE_MAX)
                    for k in range(3)) for a in range(COVERAGE_MAX + 1)]]
    top = margin
    for width, glyph_masks, lines in blocks:
        height = width * 2
        for row, line in enumerate(lines):
            for col, code in enumerate(line):
                m = glyph_masks[code - FIRST]
                for y in range(height):
                    for x in range(width):
                        a = m[y * width + x]
                        if a:
                            i = ((top + row * height + y) * w + margin + col * width + x) * 3
                            image[i : i + 3] = mixed[0][a]
        top += height * len(lines) + margin
    write_png(out, w, h, image)


def main():
    if len(sys.argv) == 4 and sys.argv[1] == "atlas":
        atlas(int(sys.argv[2]), sys.argv[3])
    elif len(sys.argv) == 3 and sys.argv[1] == "specimen":
        specimen(sys.argv[2])
    else:
        print(__doc__, file=sys.stderr)
        sys.exit(USAGE_ERROR)


if __name__ == "__main__":
    main()
