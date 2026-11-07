/* The specimen's screen (specimen_draw.h): the approved login's
   "Choose an account" with three accounts, the second chosen, and a
   paragraph of body text, at the design's sizes and places for a screen
   1800 pixels tall (the login design's direction C, Horizon: LM, the
   left margin, and the account rows from TOP, ROW apart). */

#include "specimen_draw.h"

#define DESIGN_HEIGHT 1800
#define LM 320
#define HEADING_LEFT (LM - 6)
#define HEADING_BASELINE 560
#define HEADING_SIZE 104
#define TOP 680
#define ROW 150
#define NAME_LEFT (LM + 136)
#define NAME_SIZE 64
#define CHOSEN 1
#define BODY_BASELINE 1240
#define BODY_SIZE 26
#define BODY_WIDTH 960

/* The login design's inks: text, secondary, and the accounts not
   chosen. */
#define INK_TEXT 0xF2F2F6u
#define INK_SECONDARY 0xA9B0BEu
#define INK_UNCHOSEN 0x717A8Cu

static const char *const files[SPECIMEN_FACES] = {
    "InterDisplay-Light.nvf", "InterDisplay-Regular.nvf",
    "Inter-Regular.nvf"};

static const char *const names[] = {"alice", "devon", "ben"};

#define NAMES (sizeof names / sizeof names[0])

static const char heading[] = "Choose an account";
static const char body[] =
    "nova-os sets this text in Inter, from outlines converted when the "
    "system is built: each glyph placed to a 256th of a pixel, kerned by "
    "the font's own pairs, and mixed into Limb in linear light, so that a "
    "host build of the same code draws the same pixels.";

const char *specimen_face_file(int face)
{
    return files[face];
}

static INT64 floor_div(INT64 a, INT64 b)
{
    INT64 q = a / b;

    return q * b > a ? q - 1 : q;
}

static INT64 rounded(INT64 n, INT64 d)
{
    return floor_div(2 * n + d, 2 * d);
}

static UINT64 length_of(const char *text)
{
    UINT64 n = 0;

    while (text[n])
        n++;
    return n;
}

/* A length of the design, in pixels, on the canvas, in units. */
static INT64 scaled(const CANVAS *canvas, INT64 pixels)
{
    return rounded(pixels * OUTLINE_ONE * canvas->height, DESIGN_HEIGHT);
}

/* A pixel row of the design on the canvas. */
static INT64 row_of(const CANVAS *canvas, INT64 pixels)
{
    return rounded(pixels * canvas->height, DESIGN_HEIGHT);
}

static int line(const CANVAS *canvas, const TYPEFACE *face, INT64 size,
                INT64 left, INT64 baseline, const char *text, UINT64 length,
                UINT32 ink, OUTLINE *outline)
{
    return typeset_draw(canvas, face, size, left, baseline,
                        (const UINT8 *)text, length, ink, OUTLINE_OPACITY_ONE,
                        outline);
}

int specimen_draw(const CANVAS *canvas, const TYPEFACE *faces,
                  OUTLINE *outline)
{
    const TYPEFACE *names_face = &faces[SPECIMEN_NAMES];
    const TYPEFACE *body_face = &faces[SPECIMEN_BODY];
    INT64 name_size = scaled(canvas, NAME_SIZE);
    INT64 body_size = scaled(canvas, BODY_SIZE);
    INT64 cap = rounded(names_face->cap_height * name_size, names_face->units);
    INT64 baseline = row_of(canvas, BODY_BASELINE);
    UINT64 start = 0, count = length_of(body), i;

    if (!line(canvas, &faces[SPECIMEN_HEADING],
              scaled(canvas, HEADING_SIZE), scaled(canvas, HEADING_LEFT),
              row_of(canvas, HEADING_BASELINE), heading, length_of(heading),
              INK_TEXT, outline))
        return 0;
    /* Each name's cap height centres on its row. */
    for (i = 0; i < NAMES; i++) {
        INT64 centre = rounded((2 * TOP + (2 * (INT64)i + 1) * ROW) *
                                   OUTLINE_ONE * canvas->height,
                               2 * DESIGN_HEIGHT);

        if (!line(canvas, names_face, name_size, scaled(canvas, NAME_LEFT),
                  rounded(2 * centre + cap, 2 * OUTLINE_ONE), names[i],
                  length_of(names[i]), i == CHOSEN ? INK_TEXT : INK_UNCHOSEN,
                  outline))
            return 0;
    }
    while (start < count) {
        UINT64 next, end = typeset_break(body_face, body_size,
                                         (const UINT8 *)body + start,
                                         count - start,
                                         scaled(canvas, BODY_WIDTH), &next);

        if (!line(canvas, body_face, body_size, scaled(canvas, LM), baseline,
                  body + start, end, INK_SECONDARY, outline))
            return 0;
        baseline += typeset_line_height(body_face, body_size);
        start += next;
    }
    return 1;
}
