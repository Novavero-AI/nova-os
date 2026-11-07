/* Compares the reference sheet's text as lib/ draws it with FreeType's
   rendering of the same glyphs, from the TrueType fonts the nova-os
   fonts were converted from: a sanity check of the converter and the
   rasterizer, since the two cannot agree exactly. FreeType loads each
   glyph unhinted (FT_LOAD_NO_HINTING) at the sheet's size and renders
   its anti-aliased coverage (FT_RENDER_MODE_NORMAL) at the same origin,
   to a 64th of a pixel, that lib/typeset.c places it at; the coverage
   is mixed over the same Limb in linear light as lib/outline.c mixes.
   For each line it prints the pixels either drawing changes, the mean
   and the largest difference of a pixel's channels, in sRGB codes, and
   the share of the pixels within TOLERANCE codes, and it fails if a
   line's share is below SHARE_MIN percent or its mean above MEAN_MAX
   hundredths of a code. Built for the host with FreeType.

   usage: compare-freetype SHEET FONTS TTFS

   TTFS holds each face's TrueType font, NAME.ttf. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include "raster.h"
#include "sheet.h"

/* The bounds a line must meet. */
#define TOLERANCE 16
#define SHARE_MIN 98
#define MEAN_MAX 200

#define FACES_MAX 16
#define PATH_MAX_BYTES 4096
#define CHANNEL_BITS 8
#define CHANNEL_MASK 0xFFu
#define CHANNELS 3
#define HUNDREDTHS 100
#define PERCENT 100

/* FreeType's 26.6 fixed point: units of a 64th of a pixel, a quarter of
   OUTLINE_ONE's. */
#define FT_ONE 64
#define UNITS_PER_FT (OUTLINE_ONE / FT_ONE)
#define DPI 72

static const char *ttfs;
static FT_Library library;
static FT_Face ft_faces[FACES_MAX];
static const SHEET_FACE *ft_owners[FACES_MAX];
static int ft_count;
static int failed;
static UINT32 *scratch[2];
static UINT64 scratch_size;

static void die(const char *what, const char *detail)
{
    fprintf(stderr, "compare-freetype: %s%s%s\n", what, detail ? ": " : "",
            detail ? detail : "");
    exit(1);
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

/* The FreeType face of the sheet's face, opened once. */
static FT_Face ft_face(const SHEET_FACE *face)
{
    char path[PATH_MAX_BYTES];
    int i;

    for (i = 0; i < ft_count; i++)
        if (ft_owners[i] == face)
            return ft_faces[i];
    if (ft_count == FACES_MAX)
        die("too many faces", face->name);
    snprintf(path, sizeof path, "%s/%s.ttf", ttfs, face->name);
    if (FT_New_Face(library, path, 0, &ft_faces[ft_count]) != 0)
        die("FreeType cannot open", path);
    ft_owners[ft_count] = face;
    return ft_faces[ft_count++];
}

/* The code point at *at in well-formed UTF-8, moving *at past it. */
static UINT32 next_code(const UINT8 *text, UINT64 *at)
{
    UINT32 b = text[*at], code;
    int more = b < 0x80 ? 0 : b < 0xE0 ? 1 : b < 0xF0 ? 2 : 3;

    code = more == 0 ? b : b & (0x3F >> more);
    (*at)++;
    while (more-- > 0)
        code = code << 6 | (text[(*at)++] & 0x3F);
    return code;
}

static UINT32 mix(UINT32 pixel, UINT32 ink, INT64 coverage)
{
    UINT32 out = 0;
    int shift;

    for (shift = 2 * CHANNEL_BITS; shift >= 0; shift -= CHANNEL_BITS)
        out |= (UINT32)gfx_cross_fade(pixel >> shift & CHANNEL_MASK,
                                      ink >> shift & CHANNEL_MASK, coverage,
                                      OUTLINE_COVERAGE_MAX)
               << shift;
    return out;
}

/* Draw the line with FreeType's glyphs at lib/typeset.c's places. */
static void draw_freetype(const CANVAS *canvas, const SHEET_FACE *face,
                          INT64 size, INT64 left, INT64 baseline,
                          const UINT8 *text, UINT64 length, UINT32 ink)
{
    const TYPEFACE *t = &face->face;
    FT_Face ft = ft_face(face);
    UINT64 at = 0;
    UINT32 previous = 0;
    INT64 pen = 0;
    int started = 0;

    if (FT_Set_Char_Size(ft, 0, size / UNITS_PER_FT, DPI, DPI) != 0)
        die("FreeType takes no size", face->name);
    while (at < length) {
        UINT32 code = next_code(text, &at), glyph = typeface_glyph(t, code);
        INT64 x, origin, fraction, row, column;
        FT_GlyphSlot slot = ft->glyph;

        if (started)
            pen += typeface_advance(t, previous) +
                   typeface_kerning(t, previous, glyph);
        previous = glyph;
        started = 1;
        x = rounded(left + rounded(pen * size, t->units), UNITS_PER_FT);
        origin = floor_div(x, FT_ONE);
        fraction = x - origin * FT_ONE;
        if (FT_Load_Glyph(ft, FT_Get_Char_Index(ft, code),
                          FT_LOAD_NO_HINTING | FT_LOAD_NO_BITMAP) != 0)
            die("FreeType cannot load a glyph", face->name);
        FT_Outline_Translate(&slot->outline, (FT_Pos)fraction, 0);
        if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL) != 0)
            die("FreeType cannot render a glyph", face->name);
        for (row = 0; row < slot->bitmap.rows; row++)
            for (column = 0; column < slot->bitmap.width; column++) {
                INT64 px = origin + slot->bitmap_left + column;
                INT64 py = baseline - slot->bitmap_top + row;
                INT64 coverage =
                    slot->bitmap.buffer[row * slot->bitmap.pitch + column];
                UINT32 *p;

                if (coverage == 0 || px < 0 || py < 0 ||
                    px >= canvas->width || py >= canvas->height)
                    continue;
                p = &canvas->pixels[py * canvas->stride + px];
                *p = mix(*p, ink, coverage);
            }
    }
}

static void draw_lib(const CANVAS *canvas, const SHEET_FACE *face, INT64 size,
                     INT64 left, INT64 baseline, const UINT8 *text,
                     UINT64 length, UINT32 ink)
{
    if (!typeset_draw(canvas, &face->face, size, left, baseline, text, length,
                      ink, OUTLINE_OPACITY_ONE, &sheet_outline))
        die("a glyph that does not fill", face->name);
}

static INT64 channel_difference(UINT32 a, UINT32 b)
{
    INT64 most = 0;
    int k;

    for (k = 0; k < CHANNELS; k++) {
        INT64 d = (INT64)(a >> k * CHANNEL_BITS & CHANNEL_MASK) -
                  (INT64)(b >> k * CHANNEL_BITS & CHANNEL_MASK);

        d = d < 0 ? -d : d;
        most = d > most ? d : most;
    }
    return most;
}

/* Draw the line both ways over copies of the canvas, compare, report,
   then draw it on the canvas with lib/ for the lines after it. */
static void compare(const CANVAS *canvas, const SHEET_FACE *face, INT64 size,
                    INT64 left, INT64 baseline, const UINT8 *text,
                    UINT64 length, UINT32 ink)
{
    UINT64 pixels = (UINT64)(canvas->stride * canvas->height), i;
    UINT64 changed = 0, within = 0, total = 0;
    INT64 most = 0;
    CANVAS copy[2];
    int k;

    if (pixels > scratch_size) {
        for (k = 0; k < 2; k++) {
            free(scratch[k]);
            scratch[k] = malloc(pixels * sizeof *scratch[k]);
            if (!scratch[k])
                die("out of memory", 0);
        }
        scratch_size = pixels;
    }
    for (k = 0; k < 2; k++) {
        copy[k] = *canvas;
        copy[k].pixels = scratch[k];
        memcpy(scratch[k], canvas->pixels, pixels * sizeof *scratch[k]);
    }
    draw_lib(&copy[0], face, size, left, baseline, text, length, ink);
    draw_freetype(&copy[1], face, size, left, baseline, text, length, ink);
    for (i = 0; i < pixels; i++) {
        INT64 d;

        if (scratch[0][i] == canvas->pixels[i] &&
            scratch[1][i] == canvas->pixels[i])
            continue;
        d = channel_difference(scratch[0][i], scratch[1][i]);
        changed++;
        total += (UINT64)d;
        within += d <= TOLERANCE;
        most = d > most ? d : most;
    }
    printf("%-22s %6ld.%02ld px  %6lu pixels  mean %ld.%02ld  max %3ld  "
           "within %d: %ld.%02ld%%\n",
           face->name, (long)(size / OUTLINE_ONE),
           (long)rounded(size % OUTLINE_ONE * HUNDREDTHS, OUTLINE_ONE),
           (unsigned long)changed,
           (long)(changed ? total / changed : 0),
           (long)(changed ? total * HUNDREDTHS / changed % HUNDREDTHS : 0),
           (long)most, TOLERANCE,
           (long)(changed ? within * PERCENT / changed : PERCENT),
           (long)(changed ? within * PERCENT * HUNDREDTHS / changed %
                                HUNDREDTHS
                          : 0));
    if (changed && (within * PERCENT < SHARE_MIN * changed ||
                    total * HUNDREDTHS > (UINT64)MEAN_MAX * changed))
        failed = 1;
    draw_lib(canvas, face, size, left, baseline, text, length, ink);
}

int main(int argc, char **argv)
{
    CANVAS canvas;

    if (argc != 4) {
        fprintf(stderr, "usage: %s SHEET FONTS TTFS\n", argv[0]);
        return 2;
    }
    ttfs = argv[3];
    if (FT_Init_FreeType(&library) != 0)
        die("FreeType does not start", 0);
    sheet_render(argv[1], argv[2], compare, &canvas);
    printf(failed ? "a line differs from FreeType's past the bounds\n"
                  : "every line within the bounds\n");
    return failed;
}
