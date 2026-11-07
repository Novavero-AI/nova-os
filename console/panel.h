/* What the console draws (docs/spec/console.md C5): one dark
   translucent panel with rounded corners over the desktop background,
   and in it a grid of cells, each a glyph of the console font and, where
   the caret stands, the caret. */

#ifndef NOVA_PANEL_H
#define NOVA_PANEL_H

#include "gfx.h"

/* The lines the panel shows. */
#define PANEL_ROWS 30

/* The ink the text and the caret are drawn in. */
#define PANEL_TEXT 0
#define PANEL_CARET 1
#define PANEL_INKS 2

#define PANEL_CHANNELS 3
#define PANEL_CODES 256

/* Where everything goes on a screen: the cells' size, how many the
   panel holds, the panel and its corners' radius, and the top-left
   corner of cell (0, 0). */
typedef struct {
    INT64 cell_width, cell_height;
    UINT32 columns, rows;
    RECT box;
    INT64 radius;
    INT64 text_left, text_top;
} LAYOUT;

/* For each ink, channel and coverage, the code a backdrop code becomes
   under the ink at that coverage, mixed in linear light. */
typedef struct {
    UINT8 mix[PANEL_INKS][PANEL_CHANNELS][COVERAGE_MAX + 1][PANEL_CODES];
} INKS;

/* Lay the panel out on a width x height screen. Returns 0 if not one
   cell fits. */
int panel_layout(LAYOUT *layout, INT64 width, INT64 height);

/* The bytes panel_backdrop writes for layout. */
UINT64 panel_backdrop_size(const LAYOUT *layout);

/* The panel's pixels without text, row by row as 0x00RRGGBB: Limb, as
   the kernel drew it on the width x height screen, under the panel's
   tint. Returns 0, writing nothing, if Limb has no size for the screen
   (limb.h). */
int panel_backdrop(const LAYOUT *layout, INT64 width, INT64 height,
                   UINT32 *backdrop);

void panel_inks(INKS *inks);

/* Draw the backdrop on screen. */
void panel_show(const SCREEN *screen, const LAYOUT *layout,
                const UINT32 *backdrop);

/* Draw the cell at row and column, column up to layout->columns, where
   the cell lies in the panel's padding, with glyph, a mask of the
   console font, and caret's mask over it unless caret is 0. */
void panel_cell(const SCREEN *screen, const LAYOUT *layout,
                const UINT32 *backdrop, const INKS *inks,
                const UINT8 *glyph, const UINT8 *caret, UINT32 row,
                UINT32 column);

#endif
