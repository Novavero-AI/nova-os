/* What the console draws (docs/spec/console.md C2, C5): one dark
   translucent panel with rounded corners over the desktop background,
   and in it a grid of cells, each a glyph of the console font and, where
   the caret stands, the caret, which show the lines of the text view.h
   places in view. It writes pixels and nothing else, so that
   ci/screens.c draws the same panel on the host: the screen's, where the
   console holds the framebuffer, or, in a window, a buffer's, which carry
   only the ink and its cover, the panel's material and corners being the
   compositor's. The console makes the stores to the screen visible once
   its frame is drawn (gfx_flush). */

#ifndef NOVA_PANEL_H
#define NOVA_PANEL_H

#include "gfx.h"
#include "view.h"

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

/* What drawing the panel's cells needs: the screen, the panel's layout
   on it, its backdrop, the console font's masks at the layout's cell
   width, and the inks. */
typedef struct {
    const SCREEN *screen;
    const LAYOUT *layout;
    const UINT32 *backdrop;
    const UINT8 *masks;
    const INKS *inks;
} PANEL;

/* What the screen shows in the panel's cells: each one's character, and
   the caret. */
typedef struct {
    UINT8 cells[PANEL_ROWS][TEXT_COLUMNS_MAX];
    VIEW_CARET caret;
} PANEL_SHOWN;

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

/* Draw Limb over the whole screen, computed for its size, as the kernel's
   last fade frame leaves it (docs/spec/console.md C1), so that nothing a
   writer before the console drew stays. Returns 0, drawing nothing, if
   Limb has no size for the screen (limb.h). */
int panel_limb(const SCREEN *screen);

/* Draw the backdrop on screen, and note in shown what that leaves in
   the cells: every one blank, and no caret. */
void panel_show(const SCREEN *screen, const LAYOUT *layout,
                const UINT32 *backdrop, PANEL_SHOWN *shown);

/* Draw text's view from line top, with caret, in the cells where shown
   says the screen differs: each cell whose character changed, and the
   cells the caret leaves and enters. Then shown says what the screen
   shows. */
void panel_text(const PANEL *panel, PANEL_SHOWN *shown, const TEXT *text,
                UINT64 top, const VIEW_CARET *caret);

/* ------------------------------------------------------------------------
   A window's pixels (docs/spec/console.md C2, Window mode). */

/* The pixel of a window with the glyph's mask coverage and the caret's at
   it, each from 0 to COVERAGE_MAX: 0x00000000 where both are 0, the
   panel's material alone. Else the pixel's cover, bits 24 to 31, is
   (coverage * 255 + 8) / 16 for the ink alone, and g + c - g * c / 255
   for a glyph's g and a caret's c over each other, and its colour, the
   sRGB codes of 0x00RRGGBB, is the ink's, or the caret's ink mixed over
   the glyph's by c over that cover in linear light (gfx_cross_fade). */
UINT32 panel_window_pixel(UINT8 glyph, UINT8 caret);

/* Make the cells view.h puts in view, the lines of text from line top
   and caret, into *want. */
void panel_view(const LAYOUT *layout, const TEXT *text, UINT64 top,
                const VIEW_CARET *caret, PANEL_SHOWN *want);

/* Note in *shown that a window's buffer holds nothing drawn, so that the
   next panel_window_draw draws every cell. */
void panel_window_forget(const LAYOUT *layout, PANEL_SHOWN *shown);

/* The smallest rectangle of the window, in its pixels, that holds every
   cell where the frames *was and *want differ, a character or the
   caret's presence. Returns 0, setting nothing, if no cell differs. */
int panel_window_damage(const LAYOUT *layout, const PANEL_SHOWN *was,
                        const PANEL_SHOWN *want, RECT *damage);

/* The result of panel_window_buffer when no buffer is free, and the value
   of a buffer number that names none. */
#define PANEL_NO_BUFFER (-1)

/* The buffer of count, numbered from 0, that the next frame is drawn in:
   the first for which available[i] is nonzero, or PANEL_NO_BUFFER. A
   buffer is available while the compositor holds no claim on it: one it
   has not been given, or one it has released (display.md D7). It depends
   on nothing else, in particular not on which buffer was presented last,
   since a present the compositor ignores leaves that one released and
   the one it reads held. */
int panel_window_buffer(const int *available, UINT32 count);

/* Draw into pixels, a window's buffer of rows stride pixels apart whose
   left and top are the panel's box's, each cell where *shown, what the
   buffer shows, differs from *want, with masks, the console font's at the
   layout's cell width. Then *shown is *want. */
void panel_window_draw(const LAYOUT *layout, const UINT8 *masks,
                       UINT32 *pixels, UINT32 stride, PANEL_SHOWN *shown,
                       const PANEL_SHOWN *want);

#endif
