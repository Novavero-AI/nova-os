/** @file
    How a pointing device's reports reach the window (docs/spec/display.md
    D10 rule 4, D11 rules 3 and 6, and D19's phase 3 arrangement, rules 4
    and 6): where the absolute axes put the pointer on the screen, whether
    it is over the window, and what the window is sent for a report, which
    is an entry, a pointer event, or a leave.

    The phase 3 arrangement serves one window, so there is no search among
    windows, no popup, no child and no change of focus here; the keyboard
    is the compositor's to send to the window with the focus (D11, rule
    1). A function decides over the decoded fields of a report and a ROUTE
    the caller keeps, makes no system call, keeps no state of its own and
    allocates nothing, and does its arithmetic in integers, so every build
    decides alike (tools/check-route.c).

    A window passed as 0 is no window: the call is made as if the window
    had closed, sends nothing, and only the pointer's place and the buttons
    held change, so no event ever carries a place on the screen.

    The window is told a pointer's place only relative to itself, in
    DISPLAY_POSITION_ONE units, and only while the pointer is over it or
    held to it by a button (D11, rule 6). The screen's place of the
    pointer is in the ROUTE for the compositor to draw it by. */

#ifndef NOVA_ROUTE_H
#define NOVA_ROUTE_H

#include "display_protocol.h"
#include "input_protocol.h"
#include "types.h"
#include "window.h"

/** The widest or highest screen route_axis maps onto, in pixels: with
    this many pixels a position in DISPLAY_POSITION_ONE units still fits
    a signed 32-bit integer. A greater extent is taken as this. */
#define ROUTE_EXTENT_MAX 0x7FFFFF

/** The most events one report, settling or source's going gives: an entry
    or a leave around a pointer event, and never an entry and a leave
    together. */
#define ROUTE_EVENTS_MAX 2

/** The most buttons held at once, over every source: as many as one
    report changes. A press past it is ignored, and so is its release. */
#define ROUTE_HELD_MAX INPUT_REPORT_BUTTONS

/** @name What holds the pointer while a button is held
    @{ */
#define ROUTE_GRAB_WINDOW 1  /**< the first button was pressed over the
                                  window */
#define ROUTE_GRAB_OUTSIDE 2 /**< it was pressed over no window, or the
                                  window has closed since */
/** @} */

/** An input event for the window, which a window's channel takes as the
    message its kind names: WINDOW_KEY, WINDOW_POINTER, WINDOW_ENTER or
    WINDOW_LEAVE. The header of a body is left 0, as the generated
    encoders take it and set the kind in; a WINDOW_LEAVE has no body. Every
    byte past the body in use is 0. */
typedef struct {
    UINT32 kind; /**< the message's kind */
    union {
        struct {
            WINDOW_KEY_EVENT event;        /**< the key, its text_length
                                                the bytes of text used */
            UINT8 text[WINDOW_KEY_TEXT_MAX]; /**< the key's text */
        } key;                             /**< WINDOW_KEY's */
        WINDOW_POINTER_EVENT pointer;      /**< WINDOW_POINTER's */
        WINDOW_ENTER_EVENT enter;          /**< WINDOW_ENTER's */
    } body; /**< what the kind says is in use */
} WINDOW_INPUT;

/** A button that is held: the source that reports it, which is the
    caller's number for a device, any value, and the evdev code of the
    button. Two sources may each have a button of one code. */
typedef struct {
    UINT32 source; /**< the caller's number for the device */
    UINT32 code;   /**< the button's evdev key code */
} ROUTE_BUTTON;

/** The pointer's state between reports, all zero before the first. Every
    pointing device the compositor reads moves one pointer, so it keeps
    one. */
typedef struct {
    UINT32 x;       /**< the pointer's place across the screen, in
                         DISPLAY_POSITION_ONE units, if INPUT_AXIS_X is in
                         known */
    UINT32 y;       /**< its place down the screen, if INPUT_AXIS_Y is in
                         known */
    UINT32 known;   /**< the axes a report has set: INPUT_AXIS_X and _Y */
    UINT32 held;    /**< how many of button are held */
    ROUTE_BUTTON button[ROUTE_HELD_MAX]; /**< the buttons held, in the order
                                              they were pressed */
    UINT32 grab;    /**< while held is not 0, what holds the pointer:
                         ROUTE_GRAB_WINDOW or ROUTE_GRAB_OUTSIDE; else 0 */
    UINT32 entered; /**< nonzero from the window's WINDOW_ENTER to its
                         WINDOW_LEAVE */
} ROUTE;

/** What a report gives the window, in the order to send it. */
typedef struct {
    UINT32 count;                            /**< how many of event */
    WINDOW_INPUT event[ROUTE_EVENTS_MAX];    /**< the events, in order */
} ROUTED;

/** The place on an axis of extent pixels that value, on an axis ranging
    from range->min to range->max, is at (D10, rule 4): the value's
    distance above the least, over the range's size, times the extent, in
    DISPLAY_POSITION_ONE units rounded down; a value outside the range is
    taken as the nearest end of it. So the place is within the screen,
    below extent pixels, and the pixel it lies in is the spec's
    (value - min) * extent / (max - min + 1). A range whose maximum is
    below its minimum, which a description never has, or an extent of 0,
    gives 0. */
UINT32 route_axis(INT32 value, const INPUT_RANGE *range, UINT32 extent);

/** Whether both axes have been set, which is when the pointer has a
    place: until then it is over nothing (D10, rule 4) and is not drawn
    (D19, rule 7). */
UINT32 route_placed(const ROUTE *route);

/** Take the pointer report of the source the caller numbers source, whose
    description is device, on a screen of width x height pixels, with the
    window as it is: window is the window the pointer may be over, or 0
    for none. A window that has shown no frame is over no pixel (D19, rule
    5).

    Sets *routed to what the window is sent. The pointer is over the
    window when the pixel its place lies in is one of the window's, the
    left and top edges' pixels in and the right and bottom edges' out. The
    window has the pointer while it is over the window and no button is
    held, or while a button is held that was pressed over the window; a
    button pressed over no window holds the pointer to none until every
    button is released, so that the window is never sent a release it was
    sent no press of (D11, rule 3).

    A button is a source's and a code. A press of a button held, a release
    of one not held, and a press while ROUTE_HELD_MAX are held, whose
    release is then one not held, change nothing. The window is sent a
    button by its code, whatever the source: a press when no source held
    the code, a release when the last source that held it lets go.

    The window is sent, in order: a WINDOW_ENTER with the place, where it
    did not have the pointer and now does; a WINDOW_POINTER, with the
    place, the wheel's turn in DISPLAY_WHEEL_DETENT units (saturated) and
    the button changes that went to the window, in the report's order; and
    a WINDOW_LEAVE, where it had the pointer and now does not, after the
    pointer event that carries the last button's release. Whether the
    window has the pointer is judged when the report is done, buttons and
    all, and decides the wheel and the entry or leave alike. The pointer
    event is sent if a button change went to the window, or if the window
    has the pointer and the wheel turned or, with the window having had it
    before, the pointer moved; so no event is sent for a report that
    changes nothing the window sees, and the move that enters the window
    is its WINDOW_ENTER alone. A held pointer is sent places outside the
    window, which are negative or beyond its size. Relative motion moves
    no pointer (D10, rule 6). */
void route_report(ROUTE *route, UINT32 source, const INPUT_DEVICE *device,
                  UINT32 width, UINT32 height, const WINDOW_RECORD *window,
                  const INPUT_POINTER *report, ROUTED *routed);

/** Decide again, with the pointer where it is, for a window that has
    changed under a pointer that has not moved since, which no report
    says: one whose first present is shown, or that has moved, or changed
    its size. Sets *routed to the WINDOW_ENTER, stamped time, or the
    WINDOW_LEAVE, that the window's now having or not having the pointer
    gives, or to none. The same rules as route_report's decide it, so a
    held pointer gives none. */
void route_settle(ROUTE *route, const WINDOW_RECORD *window, UINT64 time,
                  ROUTED *routed);

/** An input endpoint has closed, as a driver's failure closes it: the
    buttons of the source the caller numbers source are let go, so that a
    release the driver lost does not hold the pointer for ever. Each
    code that thereby goes up is released to a window the pointer is held
    to, in the order the buttons were pressed, in one WINDOW_POINTER at the
    pointer's place stamped time; and the window then has the pointer or
    has not as after a report, so a leave follows where the hold ends
    outside the window. Sets *routed to what the window is sent. */
void route_source_gone(ROUTE *route, UINT32 source, const WINDOW_RECORD *window,
                       UINT64 time, ROUTED *routed);

/** The window has closed: it has no pointer, is told nothing, and a
    button held to it is held to none, so the next window is sent no
    release of a press it never saw. The pointer's place stays. */
void route_window_closed(ROUTE *route);

#endif
