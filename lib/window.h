/** @file
    The rules of a window the compositor enforces (docs/spec/display.md D6
    and D7, and D19's phase 3 arrangement, rules 4 and 5): whether a
    window may be made, where and how large it is, whether a buffer given
    to it or a frame presented on it is valid, and what a frame shows
    changed.

    Every function is a decision over decoded message fields and a
    WINDOW_RECORD the caller keeps and passes in. A function makes no
    system call, keeps no state of its own and allocates nothing, so the
    compositor keeps one record for its window and a host build drives
    many (tools/check-window.c). A refused request leaves the record as
    it was. Integer arithmetic only, so every build decides alike.

    A buffer of another size than its window is anchored at the window's
    top left corner: the buffer's pixel (0, 0) is the window's, a buffer
    larger than the window is cut at the window's edges and a smaller one
    leaves the rest of the window to its material (D7, rule 4).

    The routing of input, the pointer's hold and the backlog of events are
    not here, nor are the cascade of further windows, popups and child
    windows, which later phases add to the arrangement. */

#ifndef NOVA_WINDOW_H
#define NOVA_WINDOW_H

#include "display_protocol.h"
#include "gfx.h"
#include "types.h"

/** The windows the phase 3 arrangement serves at once, on every
    connection together (D19, rule 4). */
#define WINDOW_SERVED_MAX 1

/** WINDOW_RECORD.shown while no frame has been shown. */
#define WINDOW_NO_BUFFER WINDOW_BUFFERS_MAX

/** The part of the screen windows are placed in: the screen's size, and
    the depth of the strip along its top, which is 0 while none is drawn
    (D6, Placement). */
typedef struct {
    UINT32 screen_width;  /**< the screen's width in pixels */
    UINT32 screen_height; /**< the screen's height in pixels */
    UINT32 strip;         /**< the strip's depth in pixels */
} WINDOW_AREA;

/** What the compositor holds of a buffer given to a window: its shape,
    which a present is cut to. */
typedef struct {
    UINT32 given;  /**< nonzero once the buffer has been given */
    UINT32 width;  /**< its width in pixels */
    UINT32 height; /**< its height in pixels */
    UINT32 stride; /**< its pixels from one row to the next, which the
                        compositor reads its rows by; no rule here reads
                        it */
} WINDOW_HELD_BUFFER;

/** A window: its place, its layer and surface, the serial of the latest
    configure sent, the buffer being read, and the buffers given. */
typedef struct {
    UINT32 x;       /**< its left edge on the screen */
    UINT32 y;       /**< its top edge on the screen */
    UINT32 width;   /**< its width in pixels */
    UINT32 height;  /**< its height in pixels */
    UINT32 layer;   /**< the DISPLAY_LAYER_ it is on */
    UINT32 surface; /**< the DISPLAY_SURFACE_ it is drawn on */
    UINT32 serial;  /**< the latest configure's, 0 before the first */
    UINT32 shown;   /**< the buffer being read, or WINDOW_NO_BUFFER */
    UINT32 owed;    /**< nonzero after a present was ignored: the next
                         frame shown damages the whole window */
    WINDOW_HELD_BUFFER buffers[WINDOW_BUFFERS_MAX]; /**< those given */
} WINDOW_RECORD;

/** What presenting a frame did. */
typedef struct {
    UINT32 shown;    /**< nonzero if the frame is now shown, in which
                          case the window is sent a WINDOW_FRAME at the
                          next frame boundary, whatever count is: a frame
                          that changes no pixel is shown and answered
                          (D7, rule 3) */
    UINT32 released; /**< the buffer the compositor does not read after
                          this present, as a bit, or 0 where there is
                          none, which a WINDOW_RELEASED tells the client
                          of */
    UINT32 count;    /**< the rectangles in damage; 0 for a frame that
                          changes no pixel, or that is not shown */
    RECT damage[WINDOW_DAMAGE_MAX]; /**< what to compose, in screen pixels,
                          each widened to whole blocks of the screen's
                          framebuffer by compose_blocks */
} WINDOW_PRESENTED;

/** The size a window asking for width x height gets (D6, Placement): at
    most the area's size and WINDOW_WIDTH_MAX x WINDOW_HEIGHT_MAX, and at
    least twice reach, the corners' radius rounded up to a whole pixel
    (COMPOSE_CORNERS), except that where the area is smaller than that it
    is the area's size. A request outside the bounds gets the nearest size
    inside them. area is nonempty: its width and its height below the
    strip are not 0. */
void window_size(const WINDOW_AREA *area, UINT32 reach, UINT32 width,
                 UINT32 height, UINT32 *fitted_width, UINT32 *fitted_height);

/** The place of a width x height window in area (D6, Placement): its
    left is (W - width) / 2 and its top is
    strip + (H - strip) * 40 / 100 - height / 2, raised to at least the
    strip, in integers rounded down. D6's lowering of the top to at most
    H - height is not made, since a window no larger than the area, which
    this is for, never needs it. area is nonempty, as for window_size. */
void window_place(const WINDOW_AREA *area, UINT32 width, UINT32 height,
                  UINT32 *x, UINT32 *y);

/** Make a window for request on a connection that may use the layers
    given, a set of DISPLAY_LAYER_ values, when open windows are made
    already (D19, rule 4). Returns OK with *window set: placed, sized by
    window_size, no configure sent, no buffer given. Else the first
    refusal of ERR_ACCESS_DENIED for a layer that is not exactly one of
    layers, ERR_NOT_SUPPORTED for a surface other than PANEL, and
    ERR_NO_RESOURCES for WINDOW_SERVED_MAX windows already or an empty
    area, with *window as it was. */
UINT32 window_open(const WINDOW_AREA *area, UINT32 reach, UINT32 layers,
                   UINT32 open, const DISPLAY_WINDOW_REQUEST *request,
                   WINDOW_RECORD *window);

/** The serial of a configure about to be sent to window: one more than
    the last. It stays at the greatest serial, after 2^32 - 1 configures,
    rather than return to 0, which names no configure. */
UINT32 window_configure(WINDOW_RECORD *window);

/** The bytes of a buffer of stride x height pixels: 4 bytes a pixel, in
    whole pages (D6, Buffers); the greatest UINT64 where they do not fit
    in one. */
UINT64 window_buffer_size(UINT32 stride, UINT32 height);

/** The sections that hold size bytes: SECTION_MAX_SIZE each but the last. */
UINT64 window_buffer_sections(UINT64 size);

/** Record the buffer request describes, whose sections are the count
    sections of sizes section_sizes, in order (D6, Buffers; D19, rule 5).
    Returns OK, or ERR_INVALID_ARGS, for which the compositor closes the
    window, for a stride below the width, a size the sections of a
    message cannot hold, sections that are not those D6 bands the buffer
    into, and a buffer given under the number of the one being read; then
    *window is as it was. The ranges of the message's other fields are
    the generated check's; here a number beyond WINDOW_BUFFER_LAST and a
    width or height of 0 are refused only so that no index or count is
    out of range. */
UINT32 window_give_buffer(WINDOW_RECORD *window,
                          const WINDOW_BUFFER_REQUEST *request,
                          const UINT64 *section_sizes, UINT32 count);

/** Show the frame request names, with the request->rectangles rectangles
    rects that follow it, on window, which is on screen (D7; D19, rule
    5). Returns OK with *presented set, or ERR_INVALID_ARGS, for which
    the compositor closes the window, for a buffer never given and too
    many rectangles; then *window and *presented are as they were.

    A present that names the serial of the latest configure is shown: the
    compositor reads its buffer from now, and the buffer read before it is
    released. One that names another serial, or 0, is ignored: nothing is
    shown, its buffer is not read and is released at once, unless it is
    the buffer being read, and the next present that is shown damages the
    whole window, since the client's rectangles are relative to the frame
    it presented, which the screen does not show.

    The damage of a frame shown is its rectangles cut to the buffer and to
    the window, or, with no rectangles, the buffer cut to the window; it
    is the whole window where nothing was shown before, where the
    buffer's size is not the one shown's, and after an ignored present.
    Each is then widened to blocks. */
UINT32 window_present(WINDOW_RECORD *window, const SCREEN *screen,
                      const WINDOW_PRESENT_REQUEST *request,
                      const WINDOW_RECT *rects,
                      WINDOW_PRESENTED *presented);

#endif
