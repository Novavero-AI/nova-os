/* The window test client (wintest.c), which the first process starts on a
   disk that carries the flag file WINDOW and gives a display connection
   (init/window_test.c), so that the compositor serves a window before the
   console is its client (docs/spec/display.md D19's phase 3 arrangement,
   rule 11): its boot file and label, the message it is started with, the
   codes it ends with, and what it draws, which ci/screens.c draws again
   on the host to compare the screen with.

   It makes one window on the SYSTEM layer, as the console's will be, gives
   two buffers, presents the first whole and the second with one damage
   rectangle, logs each key its window is sent, and holds the window until
   the compositor closes it. */

#ifndef NOVA_WINTEST_H
#define NOVA_WINTEST_H

#include "types.h"

/* The boot file it is loaded from, and the label of its connection. */
#define WINTEST_FILE "wintest.exe"
#define WINTEST_LABEL "wintest"

/* Its bootstrap message: a transaction ID of 0, and these handles in this
   order: the debug log's resource, its process with MAP, and its end of
   its connection with DISPLAY_CONNECTION_RIGHTS. */
#define WINTEST_BOOTSTRAP_BYTES 4
#define WINTEST_BOOTSTRAP_DEBUG_LOG 0
#define WINTEST_BOOTSTRAP_SELF 1
#define WINTEST_BOOTSTRAP_CONNECTION 2
#define WINTEST_BOOTSTRAP_HANDLES 3

/* How it ends: its connection or its window closed, or a step failed. */
#define WINTEST_EXIT_CLOSED 100
#define WINTEST_EXIT_FAILED 101

/* The frames it presents: the first draws the whole window, the second
   changes a square of it. */
#define WINTEST_FRAMES 2

/* A buffer's rows are this many pixels longer than the window is wide, so
   that a compositor that reads a row at the window's width and not the
   stride shows it. The extra pixels are PAD_PIXEL, a colour no window
   shows. */
#define WINTEST_STRIDE_PAD 16
#define WINTEST_PAD_PIXEL 0xFFFF00FFu

/* The pixel's cover is its top byte (display.md D6). */
#define WINTEST_COVER_SHIFT 24
#define WINTEST_OPAQUE 0xFF000000u

/* What a window is drawn as. Its top eighth has cover 0, so the window's
   material shows there, over a colour the material must hide. The next
   eighth has half cover, the colour mixed over the material. The lower
   left quarter holds a checkerboard of squares of WINTEST_SQUARE pixels,
   black and white. The rest is a gradient, red along the row and green
   down the column, over a blue of one level, at full cover. The second
   frame changes a square at the window's centre. */
#define WINTEST_BANDS 8
#define WINTEST_HIDDEN_COLOUR 0x00FF0000u
#define WINTEST_HALF_COVER 0x80u
#define WINTEST_HALF_COLOUR 0x00E0A030u
#define WINTEST_SQUARE 8
#define WINTEST_BLUE 0x80u
#define WINTEST_CHANNEL_MAX 255u
#define WINTEST_MARKER_DIVISOR 8
#define WINTEST_MARKER_COLOUR 0xFF33CC66u

/* The size it asks for on a screen of screen_width x screen_height pixels:
   half in each dimension. */
static inline void wintest_want(UINT32 screen_width, UINT32 screen_height,
                                UINT32 *width, UINT32 *height)
{
    *width = screen_width / 2;
    *height = screen_height / 2;
}

/* The square the second frame changes, in a window of width x height:
   centred, a WINTEST_MARKER_DIVISOR-th of the shorter side across. */
static inline void wintest_marker(UINT32 width, UINT32 height, UINT32 *left,
                                  UINT32 *top, UINT32 *side)
{
    UINT32 shorter = width < height ? width : height;

    *side = shorter / WINTEST_MARKER_DIVISOR;
    *left = (width - *side) / 2;
    *top = (height - *side) / 2;
}

/* The pixel at column x and row y of frame, 1 or 2, in a window of
   width x height. */
static inline UINT32 wintest_pixel(UINT32 width, UINT32 height, UINT32 x,
                                   UINT32 y, UINT32 frame)
{
    UINT32 band = height / WINTEST_BANDS;
    UINT32 left, top, side;

    wintest_marker(width, height, &left, &top, &side);
    if (frame >= 2 && x >= left && x < left + side && y >= top &&
        y < top + side)
        return WINTEST_MARKER_COLOUR;
    if (y < band)
        return WINTEST_HIDDEN_COLOUR;
    if (y < 2 * band)
        return WINTEST_HALF_COVER << WINTEST_COVER_SHIFT |
               WINTEST_HALF_COLOUR;
    if (x < width / 4 && y >= height - height / 4)
        return (x / WINTEST_SQUARE + y / WINTEST_SQUARE) % 2
                   ? 0xFFFFFFFFu
                   : WINTEST_OPAQUE;
    return WINTEST_OPAQUE |
           (width > 1 ? x * WINTEST_CHANNEL_MAX / (width - 1) : 0) << 16 |
           (height > 1 ? y * WINTEST_CHANNEL_MAX / (height - 1) : 0) << 8 |
           WINTEST_BLUE;
}

#endif
