"""The outline rasterizer's prototype: lib/outline.c's arithmetic in
Python, so the C port's pixels equal these exactly.

A path is contours of lines, quadratic and cubic Bezier curves in 256ths
of a pixel. Curves are cut into lines, and each pixel row's coverage is
the exact area, under the nonzero winding rule, of the part of the row
the path fills: the row is split at every line's ends and every crossing
of two lines, and in each slice the lines are ordered across it and the
winding number counted from the left, so that only the edges where it
turns zero or nonzero bound what is filled. A pixel's coverage, 0 to
COVERAGE_MAX, mixes the ink with what is under it in linear light, as
gfx/raster.c mixes, with the same sRGB tables.

Python's // and >> round toward minus infinity; the C rounds through
floor_div to match.
"""

ONE = 256  # OUTLINE_ONE: units to a pixel
FULL = 2 * ONE * ONE  # twice a pixel's area, the accumulator's whole pixel
COVERAGE_MAX = 255
OPACITY_ONE = 65536

FLATNESS = ONE // 16  # the most a line may stray from its curve
SEGMENTS_MAX = 64  # the most lines one curve is cut into
EDGES_MAX = 4096  # OUTLINE_EDGES_MAX
WIDTH_MAX = 16384  # OUTLINE_WIDTH_MAX
COORDINATE_MAX = 1 << 24  # OUTLINE_COORDINATE_MAX

# A circle's quarter as a cubic: control points KAPPA of the radius from
# its ends along their tangents, 4(sqrt 2 - 1)/3, as lib/outline.c gives.
KAPPA_NUM = 55228475
KAPPA_DEN = 100000000


class OutlineError(Exception):
    pass


def rounded(n, d):
    """n / d rounded to the nearest integer, halves up, for d > 0."""
    return (2 * n + d) // (2 * d)


# ---------------------------------------------------------------------------
# sRGB, as gfx/raster.c decodes and encodes it


CODE_MAX = 255
HALF_CODES = 2 * CODE_MAX
LIGHT_BITS = 30
LIGHT_ONE = 1 << LIGHT_BITS
KNEE_NUM, KNEE_DEN = 4045, 100000
SLOPE_NUM, SLOPE_DEN = 1292, 100
OFFSET_NUM, SCALE_NUM, OFFSET_DEN = 55, 1055, 1000


def _times(a, b):
    return a * b >> LIGHT_BITS


def _fifth_root(a):
    root = 0
    bit = LIGHT_ONE
    while bit:
        nxt = root + bit
        if nxt <= LIGHT_ONE and _times(_times(_times(_times(nxt, nxt), nxt), nxt), nxt) <= a:
            root = nxt
        bit >>= 1
    return root


def _decode_half(j):
    if j * KNEE_DEN <= KNEE_NUM * HALF_CODES:
        return j * SLOPE_DEN * LIGHT_ONE // (HALF_CODES * SLOPE_NUM)
    t = (j * OFFSET_DEN + OFFSET_NUM * HALF_CODES) * LIGHT_ONE // (SCALE_NUM * HALF_CODES)
    t2 = _times(t, t)
    return _times(t2, _fifth_root(t2))


LIGHT = [_decode_half(j) for j in range(HALF_CODES + 1)]


def _encode(value):
    low, high = 0, CODE_MAX
    while low < high:
        mid = (low + high + 1) // 2
        if LIGHT[2 * mid - 1] <= value:
            low = mid
        else:
            high = mid - 1
    return low


def cross_fade(frm, to, weight, total):
    """gfx_cross_fade: the code whose light mixes frm's and to's, to's
    weighted weight of total."""
    return _encode((LIGHT[2 * frm] * (total - weight) + LIGHT[2 * to] * weight) // total)


# ---------------------------------------------------------------------------
# Paths


class Outline:
    """Lines in 256ths of a pixel, each with its two ends, y growing
    down; a horizontal line bounds nothing and is left out."""

    def __init__(self):
        self.edges = []
        self.start = None
        self.at = None

    def _check(self, *coordinates):
        if self.at is None:
            raise OutlineError("a line with no contour open")
        if any(abs(v) > COORDINATE_MAX for v in coordinates):
            raise OutlineError("a coordinate past OUTLINE_COORDINATE_MAX")

    def _line(self, x, y):
        self._check(x, y)
        x0, y0 = self.at
        if y != y0:
            if len(self.edges) == EDGES_MAX:
                raise OutlineError("more lines than OUTLINE_EDGES_MAX")
            self.edges.append((x0, y0, x, y))
        self.at = (x, y)

    def close(self):
        if self.at is not None and self.at != self.start:
            self._line(*self.start)
        self.at = None

    def move(self, x, y):
        self.close()
        if abs(x) > COORDINATE_MAX or abs(y) > COORDINATE_MAX:
            raise OutlineError("a coordinate past OUTLINE_COORDINATE_MAX")
        self.start = (x, y)
        self.at = (x, y)

    def line(self, x, y):
        self._line(x, y)

    def quad(self, cx, cy, x, y):
        self._check(cx, cy, x, y)
        x0, y0 = self.at
        ddx, ddy = x0 - 2 * cx + x, y0 - 2 * cy + y
        n = _segments(ddx * ddx + ddy * ddy, 1)
        nn = n * n
        for i in range(1, n + 1):
            a, b, c = (n - i) * (n - i), 2 * i * (n - i), i * i
            self._line(rounded(a * x0 + b * cx + c * x, nn), rounded(a * y0 + b * cy + c * y, nn))

    def cubic(self, c1x, c1y, c2x, c2y, x, y):
        self._check(c1x, c1y, c2x, c2y, x, y)
        x0, y0 = self.at
        ax, ay = x0 - 2 * c1x + c2x, y0 - 2 * c1y + c2y
        bx, by = c1x - 2 * c2x + x, c1y - 2 * c2y + y
        n = _segments(max(ax * ax + ay * ay, bx * bx + by * by), 9)
        nnn = n * n * n
        for i in range(1, n + 1):
            j = n - i
            a, b, c, d = j * j * j, 3 * i * j * j, 3 * i * i * j, i * i * i
            self._line(
                rounded(a * x0 + b * c1x + c * c2x + d * x, nnn),
                rounded(a * y0 + b * c1y + c * c2y + d * y, nnn),
            )


def _segments(second2, factor):
    """The fewest lines, at most SEGMENTS_MAX, that keep a curve whose
    second difference has squared length second2 within FLATNESS: a
    quadratic's lines stray at most |d| / 4n^2 from it, so 16 F^2 n^4 >=
    |d|^2; a cubic's at most 3 max|d| / 4n^2, so factor 9."""
    n = 1
    while n < SEGMENTS_MAX and 16 * FLATNESS * FLATNESS * n ** 4 < factor * second2:
        n += 1
    return n


def rounded_rect(outline, x0, y0, x1, y1, r):
    """A rectangle from (x0, y0) to (x1, y1) with corners of radius r,
    clockwise on the screen, as a contour of its own."""
    if any(abs(v) > COORDINATE_MAX for v in (x0, y0, x1, y1, r)):
        raise OutlineError("a coordinate past OUTLINE_COORDINATE_MAX")
    k = rounded(r * KAPPA_NUM, KAPPA_DEN)
    outline.move(x0 + r, y0)
    outline.line(x1 - r, y0)
    outline.cubic(x1 - r + k, y0, x1, y0 + r - k, x1, y0 + r)
    outline.line(x1, y1 - r)
    outline.cubic(x1, y1 - r + k, x1 - r + k, y1, x1 - r, y1)
    outline.line(x0 + r, y1)
    outline.cubic(x0 + r - k, y1, x0, y1 - r + k, x0, y1 - r)
    outline.line(x0, y0 + r)
    outline.cubic(x0, y0 + r - k, x0 + r - k, y0, x0 + r, y0)
    outline.close()


def circle(outline, cx, cy, r):
    if any(abs(v) > COORDINATE_MAX for v in (cx, cy, r)):
        raise OutlineError("a coordinate past OUTLINE_COORDINATE_MAX")
    rounded_rect(outline, cx - r, cy - r, cx + r, cy + r, r)


# ---------------------------------------------------------------------------
# Coverage


def _x_at(edge, y):
    """The x of edge at y, between its ends' y: (top x, top y, bottom x,
    bottom y, direction)."""
    tx, ty, bx, by, _ = edge
    return tx + rounded((y - ty) * (bx - tx), by - ty)


def _cell(acc, width, xp, yp, xq, yq, sign):
    """Add the part of a slice's bounding edge that lies in one column,
    from (xp, yp) to (xq, yq): twice the area to its right within the
    column to that column, the rest to the next, so that the running sum
    across the row is each pixel's area."""
    dy = abs(yq - yp)
    if dy == 0:
        return
    c = (xp + xq) // (2 * ONE)
    if c < 0:
        acc[0] += sign * dy * 2 * ONE
    elif c < width:
        f = xp + xq - 2 * c * ONE
        acc[c] += sign * dy * (2 * ONE - f)
        acc[c + 1] += sign * dy * f


def _deposit(acc, width, xa, ya, xb, yb, sign):
    """Add the edge from (xa, ya) to (xb, yb), row-local, cut at each
    column it crosses. What lies left of the canvas covers every column
    from 0 and goes to column 0 whole; what lies right of it covers
    none."""
    if xa == xb:
        _cell(acc, width, xa, ya, xb, yb, sign)
        return
    if xa > xb:
        xa, ya, xb, yb = xb, yb, xa, ya
    limit = width * ONE
    if xb <= 0:
        acc[0] += sign * abs(yb - ya) * 2 * ONE
        return
    if xa >= limit:
        return
    x, y = xa, ya
    if x < 0:
        y0 = ya + rounded(-xa * (yb - ya), xb - xa)
        acc[0] += sign * abs(y0 - ya) * 2 * ONE
        x, y = 0, y0
    while True:
        boundary = (x // ONE + 1) * ONE
        if boundary >= xb:
            _cell(acc, width, x, y, xb, yb, sign)
            return
        ny = ya + rounded((boundary - xa) * (yb - ya), xb - xa)
        _cell(acc, width, x, y, boundary, ny, sign)
        x, y = boundary, ny
        if x >= limit:
            return


def _row(edges, top, width, acc):
    """Accumulate row top's coverage, top in units, into acc."""
    bottom = top + ONE
    pieces = []
    for edge in edges:
        if edge[1] < bottom and edge[3] > top:
            pieces.append((edge, max(edge[1], top), min(edge[3], bottom)))
    if not pieces:
        return False
    splits = set()
    for _, s, t in pieces:
        splits.add(s)
        splits.add(t)
    for i in range(len(pieces)):
        p, ps, pt = pieces[i]
        for j in range(i + 1, len(pieces)):
            q, qs, qt = pieces[j]
            lo, hi = max(ps, qs), min(pt, qt)
            if lo >= hi:
                continue
            dl = _x_at(p, lo) - _x_at(q, lo)
            dh = _x_at(p, hi) - _x_at(q, hi)
            if (dl < 0 < dh) or (dh < 0 < dl):
                num, den = (hi - lo) * dl, dl - dh
                if den < 0:
                    num, den = -num, -den
                y = lo + num // den
                if y > lo:
                    splits.add(y)
    splits = sorted(splits)
    for k in range(len(splits) - 1):
        ya, yb = splits[k], splits[k + 1]
        across = []
        for edge, s, t in pieces:
            if s <= ya and t >= yb:
                xa, xb = _x_at(edge, ya), _x_at(edge, yb)
                across.append((xa + xb, xa, xb, edge[4]))
        across.sort(key=lambda a: a[0])
        winding = 0
        for _, xa, xb, direction in across:
            before = winding
            winding += direction
            if before == 0 and winding != 0:
                _deposit(acc, width, xa, ya - top, xb, yb - top, 1)
            elif before != 0 and winding == 0:
                _deposit(acc, width, xa, ya - top, xb, yb - top, -1)
    return True


def fill(outline, canvas, ink, opacity):
    """Mix ink, 0xRRGGBB, at opacity of OPACITY_ONE, into canvas, a
    Canvas, where the outline covers it."""
    outline.close()
    if canvas.width > WIDTH_MAX:
        raise OutlineError("a canvas wider than OUTLINE_WIDTH_MAX")
    if opacity > OPACITY_ONE:
        raise OutlineError("an opacity past OPACITY_ONE")
    if not outline.edges:
        return
    edges = []
    for x0, y0, x1, y1 in outline.edges:
        if y0 < y1:
            edges.append((x0, y0, x1, y1, 1))
        else:
            edges.append((x1, y1, x0, y0, -1))
    ymin = min(e[1] for e in edges)
    ymax = max(e[3] for e in edges)
    xmin = min(min(e[0], e[2]) for e in edges)
    xmax = max(max(e[0], e[2]) for e in edges)
    first = max(0, ymin // ONE)
    last = min(canvas.height, (ymax - 1) // ONE + 1)
    left = max(0, xmin // ONE)
    right = min(canvas.width, xmax // ONE + 1)
    if first >= last or left >= right:
        return
    total = COVERAGE_MAX * OPACITY_ONE
    inks = [(ink >> 16) & 0xFF, (ink >> 8) & 0xFF, ink & 0xFF]
    for row in range(first, last):
        acc = [0] * (canvas.width + 1)
        if not _row(edges, row * ONE, canvas.width, acc):
            continue
        area = sum(acc[:left])
        base = row * canvas.width
        for x in range(left, right):
            area += acc[x]
            coverage = min(max(rounded(area * COVERAGE_MAX, FULL), 0), COVERAGE_MAX)
            if coverage == 0:
                continue
            weight = coverage * opacity
            if weight == total:
                canvas.pixels[base + x] = ink
                continue
            p = canvas.pixels[base + x]
            under = [(p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF]
            canvas.pixels[base + x] = (
                cross_fade(under[0], inks[0], weight, total) << 16
                | cross_fade(under[1], inks[1], weight, total) << 8
                | cross_fade(under[2], inks[2], weight, total)
            )


class Canvas:
    """width x height pixels, row by row, each 0xRRGGBB in sRGB codes."""

    def __init__(self, width, height, pixels):
        self.width = width
        self.height = height
        self.pixels = pixels
