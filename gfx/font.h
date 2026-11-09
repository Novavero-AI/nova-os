/* The console font: a monospace typeface for printable ASCII built from
   the wordmark's strokes (design/font/README.md). Each glyph is a few
   capsules, rings and arcs of one width (glyphs.h), rendered once per
   cell width into a mask of each pixel's coverage with the rasterizer's
   sampling (raster.h), so that drawing text copies masks. */

#ifndef NOVA_FONT_H
#define NOVA_FONT_H

#include "raster.h"

/* The masks' codes: printable ASCII, from the space to the tilde, then
   the caret in the place of DEL, which has no glyph. */
#define FONT_FIRST 0x20
#define FONT_LAST 0x7E
#define FONT_CARET 0x7F
#define FONT_MASKS (FONT_CARET - FONT_FIRST + 1)

/* A cell in font units, and every stroke's radius. A cell is twice as
   tall as it is wide, and a pixel width scales the units by that width
   over FONT_UNITS_WIDE. */
#define FONT_UNITS_WIDE 480
#define FONT_UNITS_TALL 960
#define FONT_STROKE_RADIUS 30

/* The narrowest and widest cells font_render draws, in pixels: below
   the least a stroke is under half a pixel wide, and the most is the
   height of the tallest screen Limb draws (limb.h) over
   CELLS_PER_HEIGHT. */
#define FONT_WIDTH_MIN 4
#define FONT_WIDTH_MAX 164

/* A cell is a hundredth of the screen's height wide, so the console's 30
   lines take three fifths of the height: 8 x 16 pixels at 1280 x 800,
   16 x 32 at 2560 x 1600 (console.md C5). */
#define CELLS_PER_HEIGHT 100

/* The width in pixels of a cell on a screen height pixels tall: the
   height over CELLS_PER_HEIGHT, within FONT_WIDTH_MIN and
   FONT_WIDTH_MAX. */
static inline INT64 font_cell_width(INT64 height)
{
    INT64 width = height / CELLS_PER_HEIGHT;

    if (width < FONT_WIDTH_MIN)
        return FONT_WIDTH_MIN;
    return width > FONT_WIDTH_MAX ? FONT_WIDTH_MAX : width;
}

/* A cell's quarters: the unit the panel's padding and its corners' radius
   are given in (console.md C5). */
#define CELL_QUARTERS 4

/* The corners' radius of the panel, and of a window drawn on the panel's
   surface, in quarters of a cell's width (display.md D19, rule 4). */
#define PANEL_RADIUS_QUARTERS 5

/* The radius in whole pixels of the panel's corners for cells cell pixels
   wide: five quarters of a cell, rounded down. */
static inline INT64 font_panel_radius(INT64 cell)
{
    return cell * PANEL_RADIUS_QUARTERS / CELL_QUARTERS;
}

/* The height in pixels of a cell width pixels wide. */
INT64 font_height(INT64 width);

/* The bytes font_render writes for cells width pixels wide. */
UINT64 font_masks_size(INT64 width);

/* Render every mask for cells width pixels wide, from FONT_WIDTH_MIN to
   FONT_WIDTH_MAX, into masks: code c's at font_mask(masks, width, c),
   row by row, a byte a pixel holding its coverage, 0 to COVERAGE_MAX. */
void font_render(INT64 width, UINT8 *masks);

/* Code's mask among masks font_render wrote for width, code from
   FONT_FIRST to FONT_CARET. */
const UINT8 *font_mask(const UINT8 *masks, INT64 width, UINT32 code);

#endif
