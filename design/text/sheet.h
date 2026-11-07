/* The reference sheet, as render-text.c and compare-freetype.c read and
   draw it: the items sheet.py's header gives, over Limb at the sheet's
   size; and the fonts and Limb the specimen's render draws with. Built
   for the host. */

#ifndef NOVA_SHEET_H
#define NOVA_SHEET_H

#include "typeset.h"

/* A face the sheet names: its name, its nova-os font and the font's
   bytes, which it points into. */
typedef struct {
    const char *name;
    TYPEFACE face;
} SHEET_FACE;

/* Draw one line of text, length bytes of UTF-8, at size units to the
   em, its first glyph's origin at left, in units, on baseline, a pixel
   row, in ink. */
typedef void SHEET_TEXT(const CANVAS *canvas, const SHEET_FACE *face,
                        INT64 size, INT64 left, INT64 baseline,
                        const UINT8 *text, UINT64 length, UINT32 ink);

/* Render the sheet at path, with the fonts in directory fonts, into a
   canvas sheet_render allocates, drawing each line of text with text
   and every shape with lib/outline.c. Exits with a message naming the
   line on any error. */
void sheet_render(const char *path, const char *fonts, SHEET_TEXT *text,
                  CANVAS *canvas);

/* Read the font at path into face, whose bytes then last as long as
   the program. Exits with a message if it is not a nova-os font. */
void sheet_typeface(const char *path, TYPEFACE *face);

/* A new width x height canvas holding Limb. */
void sheet_limb(CANVAS *canvas, INT64 width, INT64 height);

/* The outline both programs fill with. */
extern OUTLINE sheet_outline;

/* Write canvas to standard output as a binary PPM; returns 0, or 1 if
   the write fails. */
int sheet_write(const CANVAS *canvas);

#endif
