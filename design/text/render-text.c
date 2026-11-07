/* Renders, for check-text.sh to compare with the prototype's renders,
   with lib/'s outlines and text over gfx/limb.c's Limb, to standard
   output as a binary PPM: the reference sheet, whose items sheet.py's
   header gives, or the type specimen's screen at WIDTH x HEIGHT, as
   test/specimen_draw.c draws it in nova-os. Built for the host.

   usage: render-text sheet SHEET FONTS > sheet.ppm
          render-text specimen FONTS WIDTH HEIGHT > specimen.ppm */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sheet.h"
#include "specimen_draw.h"

#define PATH_MAX_BYTES 4096

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

static void specimen(const char *fonts, INT64 width, INT64 height,
                     CANVAS *canvas)
{
    TYPEFACE faces[SPECIMEN_FACES];
    char path[PATH_MAX_BYTES];
    int i;

    for (i = 0; i < SPECIMEN_FACES; i++) {
        snprintf(path, sizeof path, "%s/%s", fonts, specimen_face_file(i));
        sheet_typeface(path, &faces[i]);
    }
    sheet_limb(canvas, width, height);
    if (!specimen_draw(canvas, faces, &sheet_outline)) {
        fprintf(stderr, "render-text: a glyph that does not fill\n");
        exit(1);
    }
}

int main(int argc, char **argv)
{
    CANVAS canvas;

    if (argc == 4 && strcmp(argv[1], "sheet") == 0) {
        sheet_render(argv[2], argv[3], draw, &canvas);
    } else if (argc == 5 && strcmp(argv[1], "specimen") == 0) {
        specimen(argv[2], atol(argv[3]), atol(argv[4]), &canvas);
    } else {
        fprintf(stderr, "usage: %s sheet SHEET FONTS > sheet.ppm\n"
                        "       %s specimen FONTS WIDTH HEIGHT > "
                        "specimen.ppm\n",
                argv[0], argv[0]);
        return 2;
    }
    if (sheet_write(&canvas)) {
        perror(argv[0]);
        return 1;
    }
    return 0;
}
