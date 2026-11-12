/** @file
    What a client of the input drivers does with an endpoint's messages
    (docs/spec/input.md K2 and K3, docs/spec/console.md C6): whether a
    message is a device's description, a key event or a pointer report
    the protocol allows, and the log lines that say what a description or
    a report is or does. The console and the compositor read the same
    endpoints and log the same lines (docs/spec/display.md D19, rule 8),
    so both use this library.

    Nothing here makes a system call or logs: a line is built whole
    (line.h) and handed to a function the caller gives, which writes
    it. */

#ifndef NOVA_ENDPOINT_H
#define NOVA_ENDPOINT_H

#include "input_protocol.h"
#include "line.h"

/** The pointer line logged last, which decides whether the next is left
    out (C6). Zero it before a client's first report; one is kept for all
    the pointers the client reads. */
typedef struct {
    UINT32 noted; /**< opaque: the last line's kind and detail, 0 for none */
} ENDPOINT_NOTES;

/** Whether the byte_count bytes of the message at d are a device's
    description (K3): exactly an INPUT_DEVICE, with a keyboard's no axis
    or a pointer's no axis but x and y, each with its maximum above its
    minimum. */
int endpoint_is_description(const INPUT_DEVICE *d, UINT32 byte_count);

/** Whether the byte_count bytes of the message at event are a key event
    (K2): exactly an INPUT_EVENT, of a key, with a value a key can have
    and its reserved field 0. */
int endpoint_is_key_event(const INPUT_EVENT *event, UINT32 byte_count);

/** Whether the byte_count bytes of the message at report are a pointer
    report (K2) from a pointer whose description gave the absolute axes
    axes: exactly an INPUT_POINTER, tagged as one, setting no other axis,
    and changing at most INPUT_REPORT_BUTTONS buttons, each pressed or
    released. */
int endpoint_is_report(const INPUT_POINTER *report, UINT32 byte_count,
                       UINT32 axes);

/** @name What endpoint_take makes of a message
    @{ */
#define ENDPOINT_REFUSED 0     /**< not a message the endpoint may send now */
#define ENDPOINT_DESCRIPTION 1 /**< the device's description */
#define ENDPOINT_KEY 2         /**< a key event of a keyboard */
#define ENDPOINT_REPORT 3      /**< a pointer report of a pointer */
/** @} */

/** Take the byte_count bytes of the next message of an input endpoint,
    aligned to 8 bytes, whose device is *device, with a kind of 0 before
    the endpoint's first message (K3): the first must be a description,
    and is kept in *device; each after it must be what the kind of device
    sends, a key event from a keyboard and a pointer report from a
    pointer, as endpoint_is_key_event and endpoint_is_report decide.
    Returns what the message is, or ENDPOINT_REFUSED for one that is not
    what the endpoint may send, which a client answers by closing the
    endpoint; *device is then as it was. */
UINT32 endpoint_take(INPUT_DEVICE *device, const void *message,
                     UINT32 byte_count);

/** Write the line that says what the description d (which
    endpoint_is_description accepted) gives: `COMPONENT: input
    kind=keyboard`, or `input kind=pointer` with each absolute axis's
    range, such as `x=0..32767 y=0..32767` (C6). */
void endpoint_describe(const char *component, const INPUT_DEVICE *d,
                       LINE_WRITE write);

/** Write the lines that say what report (which endpoint_is_report
    accepted) does (C6), one for each of: the absolute axes it sets, the
    relative motion, each button change, the wheel. Leave out a motion of
    the kind the line logged last noted, whatever its axes, and a line
    the same as that one, which updates notes. Nothing of where the
    pointer is or how far it moved is written. */
void endpoint_pointer(ENDPOINT_NOTES *notes, const char *component,
                      const INPUT_POINTER *report, LINE_WRITE write);

#endif
