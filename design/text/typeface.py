"""The prototype of lib/typeface.c and lib/typeset.c: a nova-os font
(include/font_format.h) read, its glyphs' outlines placed, and text laid
out left to right with advances and pair kerning, in the C's arithmetic.

Sizes, positions and widths are in 256ths of a pixel (outline.ONE); a
glyph's advance and kerning add up in font units along a line, and each
point is placed from that exact sum, so a line has no rounding drift.
Baselines are whole pixels.
"""

import struct

from outline import ONE, Outline, fill, rounded

MAGIC = b"NOVAFONT"
VERSION = 1
HEADER_SIZE = 40
RANGE_SIZE = 8
GLYPH_SIZE = 12
CONTOUR_SIZE = 4
POINT_SIZE = 4
KERN_SIZE = 2
ON_CURVE = 1

SPACE = 0x20
REPLACEMENT = 0xFFFD


class FontError(Exception):
    pass


class Typeface:
    """A font file's records, checked whole when read, as
    typeface_load checks them."""

    def __init__(self, data):
        if len(data) < HEADER_SIZE or data[:8] != MAGIC:
            raise FontError("not a nova-os font")
        (version, self.units, self.ascender, self.descender, self.line_gap,
         self.x_height, self.cap_height, glyphs, ranges, lefts, rights,
         contours, points) = struct.unpack_from("<IHhhhhhHHHHII", data, 8)
        if version != VERSION or self.units == 0 or glyphs == 0 or lefts == 0 or rights == 0:
            raise FontError("a header this reader does not take")
        at = HEADER_SIZE
        self.ranges = [struct.unpack_from("<IHH", data, at + i * RANGE_SIZE) for i in range(ranges)]
        at += ranges * RANGE_SIZE
        self.glyphs = [struct.unpack_from("<HHHHI", data, at + i * GLYPH_SIZE) for i in range(glyphs)]
        at += glyphs * GLYPH_SIZE
        self.ends = list(struct.unpack_from("<%dI" % contours, data, at))
        at += contours * CONTOUR_SIZE
        flat = struct.unpack_from("<%dh" % (2 * points), data, at)
        self.points = list(zip(flat[0::2], flat[1::2]))
        at += points * POINT_SIZE
        self.rights = rights
        self.kerning = list(struct.unpack_from("<%dh" % (lefts * rights), data, at))
        at += lefts * rights * KERN_SIZE
        self.flags = data[at : at + points]
        at += points
        if at != len(data):
            raise FontError("a file whose size is not its records'")
        last = 0
        for first, count, glyph in self.ranges:
            if first < last or count == 0 or glyph + count > glyphs:
                raise FontError("ranges out of order or past the glyphs")
            last = first + count
        running = 0
        for advance, left, right, count, first in self.glyphs:
            if left >= lefts or right >= rights or first != running:
                raise FontError("a glyph record out of range")
            running += count
        if running != contours:
            raise FontError("the glyphs' contours are not the contour count")
        previous = 0
        for end in self.ends:
            if end <= previous:
                raise FontError("contour ends that do not rise")
            previous = end
        if previous != points or any(f & ~ON_CURVE for f in self.flags):
            raise FontError("contour ends or flags out of range")

    def glyph(self, code):
        lo, hi = 0, len(self.ranges)
        while lo < hi:
            mid = (lo + hi) // 2
            first, count, glyph = self.ranges[mid]
            if code < first:
                hi = mid
            elif code >= first + count:
                lo = mid + 1
            else:
                return glyph + code - first
        return 0

    def _record(self, glyph):
        """A glyph's record; one past the font's is glyph 0's."""
        return self.glyphs[glyph if glyph < len(self.glyphs) else 0]

    def advance(self, glyph):
        return self._record(glyph)[0]

    def kern(self, left, right):
        return self.kerning[self._record(left)[1] * self.rights + self._record(right)[2]]

    def outline(self, glyph, outline, left, pen, baseline, size):
        """Add glyph's contours to outline: its origin pen font units
        right of left, on the baseline, a whole pixel, at size units to
        the em."""
        _, _, _, count, first = self._record(glyph)
        top = baseline * ONE
        start = self.ends[first - 1] if first else 0
        for c in range(first, first + count):
            end = self.ends[c]
            pts = [
                (left + rounded((pen + x) * size, self.units), top - rounded(y * size, self.units),
                 self.flags[i] & ON_CURVE)
                for i, (x, y) in enumerate(self.points[start:end], start)
            ]
            _contour(outline, pts)
            start = end


def _midpoint(a, b):
    return ((a[0] + b[0]) >> 1, (a[1] + b[1]) >> 1)


def _contour(outline, pts):
    """A TrueType contour: on-curve points joined by lines, or by a
    quadratic through an off-curve point, with an on-curve point implied
    midway between two off-curve ones."""
    if pts[0][2]:
        start, rest = pts[0][:2], pts[1:]
    elif pts[-1][2]:
        start, rest = pts[-1][:2], pts[:-1]
    else:
        start, rest = _midpoint(pts[-1], pts[0]), pts
    outline.move(*start)
    control = None
    for x, y, on in rest:
        if on:
            if control:
                outline.quad(control[0], control[1], x, y)
            else:
                outline.line(x, y)
            control = None
        else:
            if control:
                mid = _midpoint(control, (x, y))
                outline.quad(control[0], control[1], mid[0], mid[1])
            control = (x, y)
    if control:
        outline.quad(control[0], control[1], start[0], start[1])
    outline.close()


# ---------------------------------------------------------------------------
# Layout


def decode(text):
    """UTF-8 to code points, each byte that does not start a well-formed
    sequence (RFC 3629, 4) as U+FFFD."""
    out = []
    i = 0
    while i < len(text):
        b = text[i]
        if b < 0x80:
            out.append(b)
            i += 1
            continue
        if 0xC2 <= b <= 0xDF:
            n, code, low = 1, b & 0x1F, 0x80
        elif 0xE0 <= b <= 0xEF:
            n, code, low = 2, b & 0x0F, 0x800
        elif 0xF0 <= b <= 0xF4:
            n, code, low = 3, b & 0x07, 0x10000
        else:
            out.append(REPLACEMENT)
            i += 1
            continue
        if i + n >= len(text):
            out.append(REPLACEMENT)
            i += 1
            continue
        good = True
        for k in range(1, n + 1):
            c = text[i + k]
            if c & 0xC0 != 0x80:
                good = False
                break
            code = code << 6 | (c & 0x3F)
        if not good or code < low or code > 0x10FFFF or 0xD800 <= code <= 0xDFFF:
            out.append(REPLACEMENT)
            i += 1
            continue
        out.append(code)
        i += n + 1
    return out


def _glyphs(face, text):
    return [face.glyph(c) for c in decode(text)]


def _pens(face, glyphs):
    """Each glyph's origin in font units from the line's start, and the
    line's width: the advances, each with its pair's kerning but the
    last's."""
    pens = []
    pen = 0
    for i, g in enumerate(glyphs):
        pens.append(pen)
        pen += face.advance(g)
        if i + 1 < len(glyphs):
            pen += face.kern(g, glyphs[i + 1])
    return pens, pen


def width(face, size, text):
    """The width of text, one line, in units."""
    return rounded(_pens(face, _glyphs(face, text))[1] * size, face.units)


def line_break(face, size, text, limit):
    """The first line of text at most limit units wide, broken at a space:
    its length in bytes, and where the next line starts, past the spaces
    the break takes. A first word wider than limit, with any spaces
    before it, is a line alone."""
    end = None
    i = 0
    while i < len(text) and text[i] == SPACE:
        i += 1
    while i < len(text):
        while i < len(text) and text[i] != SPACE:
            i += 1
        if end is not None and width(face, size, text[:i]) > limit:
            break
        end = i
        while i < len(text) and text[i] == SPACE:
            i += 1
    if end is None:
        end = 0
    following = end
    while following < len(text) and text[following] == SPACE:
        following += 1
    return end, following


def draw(canvas, face, size, left, baseline, text, ink, opacity):
    """Draw text, one line, at size units to the em: its first glyph's
    origin at left, in units, on baseline, a pixel row; each glyph filled
    alone."""
    glyphs = _glyphs(face, text)
    pens, _ = _pens(face, glyphs)
    for g, pen in zip(glyphs, pens):
        outline = Outline()
        face.outline(g, outline, left, pen, baseline, size)
        fill(outline, canvas, ink, opacity)


def line_height(face, size):
    """The distance from one baseline to the next, in whole pixels."""
    return rounded((face.ascender - face.descender + face.line_gap) * size, face.units * ONE)
