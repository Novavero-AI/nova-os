/* Renders the reference sheet, sheet.txt, with lib/'s outlines and text
   over gfx/limb.c's Limb, to standard output as a binary PPM, for
   check-text.sh to compare with the prototype's, sheet.py, whose header
   gives the sheet's items. Built for the host.

   usage: render-text SHEET FONTS > sheet.ppm */

#include <stdio.h>
#include <stdlib.h>

#include "sheet.h"

static void draw(const CANVAS *canvas, const SHEET_FACE *face, INT64 size,
                 INT64 left, INT64 baseline, const UINT8 *text,
                 UINT64 length, UINT32 ink)
{
    if (!typeset_draw(canvas, &face->face, size, left, baseline, text, length,
                      ink, OUTLINE_OPACITY_ONE, &sheet_outline)) {
        fprintf(stderr, "render-text: a glyph that does not fill\n");
        exit(1);
    }
}

int main(int argc, char **argv)
{
    CANVAS canvas;

    if (argc != 3) {
        fprintf(stderr, "usage: %s SHEET FONTS > sheet.ppm\n", argv[0]);
        return 2;
    }
    sheet_render(argv[1], argv[2], draw, &canvas);
    if (sheet_write(&canvas)) {
        perror(argv[0]);
        return 1;
    }
    return 0;
}
