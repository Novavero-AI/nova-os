"""The type specimen's screen as the prototype draws it: Limb at WIDTH x
HEIGHT and test/specimen_draw.c's text over it, in that file's
arithmetic, to a binary PPM. check-text.sh compares it with the host's
render, and ci/boot.py with the screen the specimen draws in nova-os.

    python3 -I specimen.py FONTS WIDTH HEIGHT OUT.ppm

FONTS is the directory of the nova-os fonts, each NAME.nvf.
"""

import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import outline  # noqa: E402
import sheet  # noqa: E402
import typeface  # noqa: E402

DESIGN_HEIGHT = 1800
LM = 320
HEADING_LEFT = LM - 6
HEADING_BASELINE = 560
HEADING_SIZE = 104
TOP = 680
ROW = 150
NAME_LEFT = LM + 136
NAME_SIZE = 64
CHOSEN = 1
BODY_BASELINE = 1240
BODY_SIZE = 26
BODY_WIDTH = 960

INK_TEXT = 0xF2F2F6
INK_SECONDARY = 0xA9B0BE
INK_UNCHOSEN = 0x717A8C

FILES = ["InterDisplay-Light.nvf", "InterDisplay-Regular.nvf", "Inter-Regular.nvf"]
NAMES = [b"alice", b"devon", b"ben"]
HEADING = b"Choose an account"
BODY = (
    b"nova-os sets this text in Inter, from outlines converted when the "
    b"system is built: each glyph placed to a 256th of a pixel, kerned by "
    b"the font's own pairs, and mixed into Limb in linear light, so that a "
    b"host build of the same code draws the same pixels."
)
USAGE_ERROR = 2


def draw(canvas, faces):
    """specimen_draw."""
    heading, names, body = faces
    one = outline.ONE

    def scaled(pixels):
        return outline.rounded(pixels * one * canvas.height, DESIGN_HEIGHT)

    def row_of(pixels):
        return outline.rounded(pixels * canvas.height, DESIGN_HEIGHT)

    full = outline.OPACITY_ONE
    typeface.draw(canvas, heading, scaled(HEADING_SIZE), scaled(HEADING_LEFT),
                  row_of(HEADING_BASELINE), HEADING, INK_TEXT, full)
    name_size = scaled(NAME_SIZE)
    cap = outline.rounded(names.cap_height * name_size, names.units)
    for i, name in enumerate(NAMES):
        centre = outline.rounded((2 * TOP + (2 * i + 1) * ROW) * one * canvas.height, 2 * DESIGN_HEIGHT)
        typeface.draw(canvas, names, name_size, scaled(NAME_LEFT),
                      outline.rounded(2 * centre + cap, 2 * one), name,
                      INK_TEXT if i == CHOSEN else INK_UNCHOSEN, full)
    size = scaled(BODY_SIZE)
    baseline = row_of(BODY_BASELINE)
    text = BODY
    while text:
        end, following = typeface.line_break(body, size, text, scaled(BODY_WIDTH))
        typeface.draw(canvas, body, size, scaled(LM), baseline, text[:end], INK_SECONDARY, full)
        baseline += typeface.line_height(body, size)
        text = text[following:]


def main():
    if len(sys.argv) != 5:
        print(__doc__, file=sys.stderr)
        sys.exit(USAGE_ERROR)
    fonts, width, height, out = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), sys.argv[4]
    faces = []
    for name in FILES:
        with open(os.path.join(fonts, name), "rb") as f:
            faces.append(typeface.Typeface(f.read()))
    canvas = outline.Canvas(width, height, sheet.limb(width, height, out))
    draw(canvas, faces)
    sheet.write(canvas, out)


if __name__ == "__main__":
    main()
