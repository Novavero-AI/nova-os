/** @file
    What the compositor's frames cost, as its budget lines say it
    (docs/spec/display.md D9 and D19's phase 3 arrangement, rule 8): the
    change a frame is counted under, the pointer's motion or a window's
    present, what the frames of each kind have cost since the kind's last
    line, and the line that says so once BUDGET_FRAMES of them have been
    composed.

    Nothing here makes a system call or logs: a line is built whole
    (line.h) and handed to a function the caller gives, which writes it,
    so a host build counts and writes what nova-os does
    (tools/check-budget.c). */

#ifndef NOVA_BUDGET_H
#define NOVA_BUDGET_H

#include "line.h"
#include "types.h"

/** The frames of one kind a budget line counts: 16, which display.md
    D19's rule 8 gives. A line is written once this many of its kind have
    been composed since the kind's last. */
#define BUDGET_FRAMES 16

/** @name The change a frame is counted under
    The values budget_change returns (display.md D19, rule 8).
    @{ */
#define BUDGET_CHANGE_NONE 0    /**< neither: the frame is counted nowhere */
#define BUDGET_CHANGE_POINTER 1 /**< the pointer's motion alone damaged it */
#define BUDGET_CHANGE_PRESENT 2 /**< a window's present damaged it */
/** @} */

/** What the frames of one kind have cost since the kind's last line. All
    zero, none has been counted. */
typedef struct {
    UINT32 frames;   /**< how many are counted, fewer than BUDGET_FRAMES */
    UINT64 pixels;   /**< the most pixels one of them composed */
    UINT64 total_ns; /**< their times together, in nanoseconds */
    UINT64 max_ns;   /**< the longest of their times, in nanoseconds */
} BUDGET_TALLY;

/** The tally of each kind of change. All zero, no frame has been counted;
    the compositor starts with one so. */
typedef struct {
    BUDGET_TALLY pointer; /**< frames the pointer's motion alone damaged */
    BUDGET_TALLY present; /**< frames a window's present damaged */
} BUDGET;

/** The change a frame is counted under, one of the BUDGET_CHANGE_ values,
    from what damaged it: presented, nonzero where a window's present put
    a pixel in the frame's one rectangle; rectangle, nonzero where that
    rectangle holds a pixel, a present's or another's, as a closing
    window's; and pointer, nonzero where the pointer's motion left a
    rectangle to compose. It is BUDGET_CHANGE_PRESENT where a present
    damaged the frame, whatever else did; BUDGET_CHANGE_POINTER where the
    pointer's motion did and the frame's rectangle holds no pixel, so that
    the motion alone damaged it; and BUDGET_CHANGE_NONE for every other
    frame, one that composes nothing or that something else damaged. */
UINT32 budget_change(UINT32 presented, UINT32 rectangle, UINT32 pointer);

/** Count a frame of change, one of the BUDGET_CHANGE_ values, that
    composed pixels pixels in nanoseconds nanoseconds, from the frame's
    start to its SFENCE. Once BUDGET_FRAMES frames of its kind are
    counted, write the kind's line (display.md D19, rule 8) and start its
    tally again:

        nova-compositor: budget change=pointer frames=16 pixels=4992
        total_ms=10.0 max_ms=0.6

    as one line, with change=present for a present's frames: frames is
    BUDGET_FRAMES, pixels the most one of them composed, total_ms their
    times together and max_ms the longest, each in milliseconds to the
    tenth, rounded down (docs/spec/log.md T1). The longest line there is,
    with the greatest pixels and times, is 128 bytes, within the LINE it
    is built in. A frame of BUDGET_CHANGE_NONE, or of no BUDGET_CHANGE_
    value, is counted nowhere and writes nothing. */
void budget_frame(BUDGET *budget, UINT32 change, UINT64 pixels,
                  UINT64 nanoseconds, LINE_WRITE write);

#endif
