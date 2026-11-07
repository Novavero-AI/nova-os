/* What the type specimen draws (specimen.c): the approved login's
   account list and a paragraph in Inter over Limb. specimen_draw is
   shared with the host's render of the same screen
   (design/text/render-text.c), and design/text/specimen.py is its
   prototype. */

#ifndef NOVA_SPECIMEN_DRAW_H
#define NOVA_SPECIMEN_DRAW_H

#include "typeset.h"

/* The faces it draws with, each a file of the font path's share/fonts:
   the heading's, the names' and the paragraph's. */
#define SPECIMEN_HEADING 0
#define SPECIMEN_NAMES 1
#define SPECIMEN_BODY 2
#define SPECIMEN_FACES 3

/* The file name of face SPECIMEN_HEADING, _NAMES or _BODY. */
const char *specimen_face_file(int face);

/* Draw the specimen's text on canvas, which holds Limb at its size,
   with faces, in outline. The login design's sizes and places are for
   a screen 1800 pixels tall, and scale with the canvas's height, as the
   console's cell does. Returns 1, or 0 if a glyph does not fill. */
int specimen_draw(const CANVAS *canvas, const TYPEFACE *faces,
                  OUTLINE *outline);

#endif
