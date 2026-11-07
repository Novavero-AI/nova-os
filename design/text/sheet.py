"""Renders the reference sheet, sheet.txt, as the prototype draws it,
over Limb at the sheet's size, to a binary PPM; check-text.sh compares
it with render-text.c's, which draws with lib/.

    python3 -I sheet.py SHEET FONTS OUT.ppm

FONTS is the directory of the nova-os fonts the sheet names, each
NAME.nvf. Each line of SHEET is an item, its fields separated by single
spaces; a line starting with '#' and an empty line are skipped:

    sheet WIDTH HEIGHT                 the sheet's size, in pixels, first
    at LEFT TOP                        the next text's left end, and the
                                       pixel row above its first line
    text FACE SIZE INK TEXT            one line, its baseline a line's
                                       height below the last
    wrap FACE SIZE WIDTH INK TEXT      lines of at most WIDTH, broken at
                                       spaces, each as text
    rect LEFT TOP RIGHT BOTTOM RADIUS INK OPACITY
    circle X Y RADIUS INK OPACITY

Lengths are pixels with at most two decimals, sizes pixels to the em,
inks six hexadecimal digits, opacities percent with at most two
decimals. TEXT is the rest of the line, with \\uXXXX for a code point
outside ASCII.
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import outline  # noqa: E402
import typeface  # noqa: E402

RENDER = os.path.join(HERE, "..", "backgrounds", "render.py")
HUNDREDTHS = 100
PERCENT_HUNDREDTHS = 10000
USAGE_ERROR = 2


class SheetError(Exception):
    pass


def hundredths(text):
    """A decimal of at most two places, in hundredths."""
    whole, point, fraction = text.partition(".")
    if not whole.isdigit() or (point and not fraction.isdigit()) or len(fraction) > 2:
        raise SheetError("not a decimal of at most two places: " + text)
    return int(whole) * HUNDREDTHS + int((fraction + "00")[:2])


def length(text):
    return outline.rounded(hundredths(text) * outline.ONE, HUNDREDTHS)


def opacity(text):
    percent = hundredths(text)
    if percent > PERCENT_HUNDREDTHS:
        raise SheetError("an opacity past 100 percent: " + text)
    return outline.rounded(percent * outline.OPACITY_ONE, PERCENT_HUNDREDTHS)


def ink(text):
    if len(text) != 6:
        raise SheetError("not an ink: " + text)
    return int(text, 16)


def utf8(text):
    """TEXT's bytes, each \\uXXXX as its code point's UTF-8."""
    out = bytearray()
    i = 0
    while i < len(text):
        if text.startswith("\\u", i):
            out.extend(chr(int(text[i + 2 : i + 6], 16)).encode("utf-8"))
            i += 6
        else:
            out.extend(text[i].encode("ascii"))
            i += 1
    return bytes(out)


def fields(line, count):
    """The first count fields of line and the rest of it."""
    parts = line.split(" ", count)
    if len(parts) != count + 1:
        raise SheetError("too few fields: " + line)
    return parts


def limb(width, height, path):
    subprocess.run([sys.executable, "-I", RENDER, "limb", str(width), str(height), path], check=True)
    with open(path, "rb") as f:
        data = f.read()
    header = b"P6\n%d %d\n255\n" % (width, height)
    if not data.startswith(header) or len(data) != len(header) + 3 * width * height:
        raise SheetError("render.py wrote an unexpected PPM")
    body = data[len(header) :]
    return [body[i] << 16 | body[i + 1] << 8 | body[i + 2] for i in range(0, len(body), 3)]


def render(sheet, fonts, out):
    with open(sheet, encoding="ascii") as f:
        lines = [l.rstrip("\n") for l in f if l.strip() and not l.startswith("#")]
    kind, width, height = lines[0].split(" ")
    if kind != "sheet":
        raise SheetError("the first item is not the sheet's size")
    width, height = int(width), int(height)
    canvas = outline.Canvas(width, height, limb(width, height, out))
    faces = {}

    def face(name):
        if name not in faces:
            with open(os.path.join(fonts, name + ".nvf"), "rb") as f:
                faces[name] = typeface.Typeface(f.read())
        return faces[name]

    left, cursor = 0, 0
    for line in lines[1:]:
        kind = line.split(" ", 1)[0]
        if kind == "at":
            _, x, top = line.split(" ")
            left, cursor = length(x), int(top)
        elif kind in ("text", "wrap"):
            if kind == "text":
                _, name, size, colour, text = fields(line, 4)
                limit = None
            else:
                _, name, size, limit, colour, text = fields(line, 5)
                limit = length(limit)
            f, size, text = face(name), length(size), utf8(text)
            while True:
                end, following = (len(text), len(text)) if limit is None else typeface.line_break(f, size, text, limit)
                cursor += typeface.line_height(f, size)
                typeface.draw(canvas, f, size, left, cursor, text[:end], ink(colour), outline.OPACITY_ONE)
                text = text[following:]
                if not text:
                    break
        elif kind == "rect":
            _, x0, y0, x1, y1, r, colour, alpha = line.split(" ")
            o = outline.Outline()
            outline.rounded_rect(o, length(x0), length(y0), length(x1), length(y1), length(r))
            outline.fill(o, canvas, ink(colour), opacity(alpha))
        elif kind == "circle":
            _, x, y, r, colour, alpha = line.split(" ")
            o = outline.Outline()
            outline.circle(o, length(x), length(y), length(r))
            outline.fill(o, canvas, ink(colour), opacity(alpha))
        else:
            raise SheetError("an unknown item: " + line)
    write(canvas, out)


def write(canvas, out):
    """canvas as a binary PPM at out."""
    with open(out, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (canvas.width, canvas.height))
        f.write(bytes(v for p in canvas.pixels for v in ((p >> 16) & 0xFF, (p >> 8) & 0xFF, p & 0xFF)))


def main():
    if len(sys.argv) != 4:
        print(__doc__, file=sys.stderr)
        sys.exit(USAGE_ERROR)
    render(*sys.argv[1:])


if __name__ == "__main__":
    main()
