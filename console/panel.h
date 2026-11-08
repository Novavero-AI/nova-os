/* What the console draws (docs/spec/console.md C5): one dark
   translucent panel with rounded corners over the desktop background,
   and in it a grid of cells, each a glyph of the console font and, where
   the caret stands, the caret, which show the lines of the text view.h
   places in view. It writes the screen's pixels and nothing else, so
   that ci/screens.c draws the same panel on the host; the console makes
   the stores visible once its frame is drawn (gfx_flush). */

#ifndef NOVA_PANEL_H
#define NOVA_PANEL_H

#include "gfx.h"
#include "native_abi.h"
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
   the kernel drew it on the width x height screen, computed, under the
   panel's tint. Returns 0, writing nothing, if Limb has no size for the
   screen (limb.h). */
int panel_backdrop(const LAYOUT *layout, INT64 width, INT64 height,
                   UINT32 *backdrop);

/* Why the kernel's background, as background labels it, cannot give the
   panel its backdrop on display, a short phrase for the log; or 0 if it
   can: Limb, in a pixel format the panel reads, at the display's size,
   which Limb and the panel's layout take (console.md C5). */
const char *panel_background_refusal(const BOOTSTRAP_BACKGROUND *background,
                                     const BOOTSTRAP_DISPLAY *display);

/* panel_backdrop's pixels, read from image, the kernel's background on a
   screen width pixels wide in format, PIXEL_RGB or PIXEL_BGR, row after
   row with no gap between rows, for which layout was made. */
void panel_backdrop_image(const LAYOUT *layout, const UINT32 *image,
                          INT64 width, UINT32 format, UINT32 *backdrop);

void panel_inks(INKS *inks);

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

#endif
