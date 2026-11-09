/* The console in a window (docs/spec/console.md C2, Window mode;
   docs/spec/display.md D4, D6, D7 and D19's phase 3 arrangement): the
   console's end of a display connection, the one window it makes on the
   panel's surface, two buffers it draws the panel's pixels in by turns
   (panel_window_draw), and the keys and wheel turns the window is sent.
   What the pixels are and what a frame damages, and which buffer a frame
   is drawn in, are panel.c's, and which keys edit and how far a wheel
   scrolls are keymap.c's and view.c's, so that each is checked on the
   host; this file is the channels and the memory.

   A pane takes a connection, waits for its screen, asks for a window the
   size of the panel's box on that screen, and draws and presents once the
   window is configured. It presents a frame at a time: the next waits for
   the compositor's frame event for the last, and draws into a buffer the
   compositor has released (panel_window_buffer). A connection or a window that closes leaves
   the pane holding nothing, ready for the next connection. */

#ifndef NOVA_PANE_H
#define NOVA_PANE_H

#include "display_protocol.h"
#include "keymap.h"
#include "mapping.h"
#include "panel.h"
#include "view.h"

/* The buffers a window is drawn in by turns: one the compositor reads
   while the other is drawn. */
#define PANE_BUFFERS 2

/* Where a pane stands. */
#define PANE_IDLE 0              /* holds no connection */
#define PANE_AWAITING_SCREEN 1   /* holds one, which has not said its screen */
#define PANE_AWAITING_WINDOW 2   /* has asked for the window */
#define PANE_AWAITING_CONFIGURE 3 /* the window is made, not yet sized */
#define PANE_SHOWING 4           /* the window is configured: it is drawn */

/* One of the window's buffers: the pixels, mapped, 0 until it is given
   to the window, the bytes mapped; whether the compositor reads it not;
   and what it shows, which is what was last drawn in it. */
typedef struct {
    UINT32 *pixels;
    UINT64 bytes;
    int released;
    PANEL_SHOWN shown;
} PANE_BUFFER;

typedef struct {
    MAPPING *mapping;
    UINT32 connection, window;
    UINT32 phase;
    UINT32 serial; /* the last configure's, 0 before the first */
    UINT32 screen_width, screen_height;
    LAYOUT layout; /* the panel on the screen; its box is the window */
    UINT8 *masks;  /* the font's masks at the layout's cell width, or 0 */
    UINT64 masks_size;
    PANE_BUFFER buffers[PANE_BUFFERS];
    int presented; /* the buffer the compositor was last given, or -1 */
    int owed;      /* a present's frame has not come */
    int whole;     /* the next present damages the whole window */
    INT64 wheel;   /* the part of a detent turned and not scrolled */
    PANEL_SHOWN want;
} PANE;

/* What a message of the window asks of the console: an edit, with its
   character for EDIT_INSERT, which is EDIT_NONE for none, and the lines
   to scroll back, negative for forward, 0 for none. */
typedef struct {
    UINT32 edit;
    UINT8 character;
    INT64 lines;
} PANE_INPUT;

/* A pane that holds nothing, in mapping. */
void pane_init(PANE *pane, MAPPING *mapping);

/* Take connection, the console's end of a display connection, in place of
   any the pane held. */
void pane_adopt(PANE *pane, UINT32 connection);

/* Close what the pane holds, its window, its connection and its memory. */
void pane_drop(PANE *pane);

/* Whether the panel is laid out, from the screen's first message until
   the pane drops what it holds: its layout and masks stand. */
int pane_laid_out(const PANE *pane);

/* Read the connection's messages that are queued: the screen, which has
   the pane ask for its window, and the answer to the request. A closed
   connection, a message out of place and a refused window drop the pane,
   logged unless the compositor closed it. */
void pane_serve_connection(PANE *pane);

/* Read the window's next message, if one is queued, and act on it:
   configure, release and frame are the pane's own, a key or a wheel turn
   is returned in *input. Returns 0, with *input empty, if none is queued
   or the pane dropped what it held; 1 otherwise, so that it is called
   again until it returns 0. */
int pane_input(PANE *pane, PANE_INPUT *input);

/* Draw the view of text from line top, with caret, and present it, if the
   window is shown, its last present has had its frame and a buffer is
   released, and the view differs from what the compositor has been given.
   Returns having done nothing otherwise. */
void pane_show(PANE *pane, const TEXT *text, UINT64 top,
               const VIEW_CARET *caret);

#endif
