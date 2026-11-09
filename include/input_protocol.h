/** @file
    Key events and pointer reports from the input drivers to their
    client, and how the platform driver hands the client their endpoints
    (docs/spec/input.md). Every field is little-endian. */

#ifndef NOVA_INPUT_PROTOCOL_H
#define NOVA_INPUT_PROTOCOL_H

#include "native_abi.h"

/** @name Key events
    evdev's EV_KEY event type, and the values of a key event: released,
    pressed, and repeated while held (input.md cites the definitions).
    @{ */
#define INPUT_TYPE_KEY 1     /**< evdev's EV_KEY */
#define INPUT_KEY_RELEASED 0 /**< released */
#define INPUT_KEY_PRESSED 1  /**< pressed */
#define INPUT_KEY_REPEATED 2 /**< repeated while held */
/** @} */

/** One key event: a message of exactly these bytes and no handles. type,
    code and value are the evdev event's, as the device gave them; time is
    the monotonic clock, in nanoseconds, when the driver took the event
    from the device. */
typedef struct {
    UINT32 transaction; /**< 0: an event answers no call (ipc.md I3) */
    UINT16 type;        /**< INPUT_TYPE_KEY */
    UINT16 code;        /**< the evdev key code */
    UINT32 value;       /**< INPUT_KEY_RELEASED, _PRESSED or _REPEATED */
    UINT32 reserved;    /**< 0 */
    UINT64 time;        /**< when the driver took it, in nanoseconds */
} INPUT_EVENT;

_Static_assert(sizeof(INPUT_EVENT) == 24, "one layout on every target");

/** @name A device's description
    The first message on every endpoint, before any key event or pointer
    report, says what the device is. Its type lies above evdev's EV_MAX
    (0x1f), so it is no event's type.
    @{ */
#define INPUT_TYPE_DEVICE 0x100 /**< a description's type */
#define INPUT_DEVICE_KEYBOARD 1 /**< key events follow */
#define INPUT_DEVICE_POINTER 2  /**< pointer reports follow */
/** @} */

/** @name The absolute axes of a description or a report
    @{ */
#define INPUT_AXIS_X 0x1 /**< evdev's ABS_X */
#define INPUT_AXIS_Y 0x2 /**< evdev's ABS_Y */
/** @} */

/** An absolute axis's range, as the device gives it with
    VIRTIO_INPUT_CFG_ABS_INFO: its values run from min to max, and max
    lies above min. */
typedef struct {
    INT32 min; /**< the axis's least value */
    INT32 max; /**< its greatest */
} INPUT_RANGE;

/** A device's description: a message of exactly these bytes and no
    handles, the first on its endpoint. A pointer's absolute axes are
    those it has with a range; a client maps an axis's value onto the
    screen from it. */
typedef struct {
    UINT32 transaction; /**< 0: a description answers no call */
    UINT16 type;        /**< INPUT_TYPE_DEVICE */
    UINT16 kind;        /**< INPUT_DEVICE_KEYBOARD or _POINTER */
    UINT32 absolute;    /**< a pointer's INPUT_AXIS_X and _Y, else 0 */
    UINT32 reserved;    /**< 0 */
    INPUT_RANGE x;      /**< ABS_X's range if absolute holds it, else 0s */
    INPUT_RANGE y;      /**< ABS_Y's range if absolute holds it, else 0s */
} INPUT_DEVICE;

_Static_assert(sizeof(INPUT_DEVICE) == 32, "one layout on every target");

/** @name Pointer reports
    evdev's EV_SYN event type and SYN_REPORT code tag a pointer report,
    which changes at most INPUT_REPORT_BUTTONS buttons.
    @{ */
#define INPUT_TYPE_SYN 0       /**< evdev's EV_SYN */
#define INPUT_SYN_REPORT 0     /**< evdev's SYN_REPORT */
#define INPUT_REPORT_BUTTONS 8 /**< the most button changes a report holds */
/** @} */

/** A button a pointer report presses or releases. */
typedef struct {
    UINT16 code;  /**< the evdev key code, one the device declared */
    UINT16 value; /**< INPUT_KEY_PRESSED or INPUT_KEY_RELEASED */
} INPUT_BUTTON;

/** One pointer report: a message of exactly these bytes and no handles,
    for the events a pointer sent up to an EV_SYN SYN_REPORT, or up to a
    change of a button that the report would change twice or that would
    not fit, which then starts the next report. Values are
    the device's own: an absolute axis's lies in the description's range
    unless the device strays from it, and one the report does not set
    reads 0 here and keeps, for the client, the value it last set. dx, dy
    and wheel are the report's own motion, 0 if none, each sum held
    within a signed 32-bit integer rather than wrapped. button holds, in
    the order the device made them, the changes of each button's state,
    each button at most once: a press of one the client knows released,
    or a release of one it knows pressed. time is the monotonic clock,
    in nanoseconds, when the driver took the event that ended the report
    from the device. */
typedef struct {
    UINT32 transaction; /**< 0: a report answers no call */
    UINT16 type;        /**< INPUT_TYPE_SYN */
    UINT16 code;        /**< INPUT_SYN_REPORT */
    UINT32 absolute;    /**< INPUT_AXIS_X and _Y: the axes it sets */
    UINT32 buttons;     /**< how many of button it holds */
    INT32 x;            /**< ABS_X if set, else 0 */
    INT32 y;            /**< ABS_Y if set, else 0 */
    INT32 dx;           /**< evdev's REL_X, summed */
    INT32 dy;           /**< evdev's REL_Y, summed */
    INT32 wheel;        /**< evdev's REL_WHEEL, in detents, summed */
    UINT32 reserved;    /**< 0 */
    UINT64 time;        /**< when the driver took its end */
    INPUT_BUTTON button[INPUT_REPORT_BUTTONS]; /**< the buttons changed */
} INPUT_POINTER;

_Static_assert(sizeof(INPUT_POINTER) == 80, "one layout on every target");

/** The most messages an input driver's endpoint queues for its client
    before the driver waits for it to read (docs/spec/input.md K2): each
    holds kernel memory until it is read. The compositor, which serves
    other channels as well, reads at most this many from one endpoint
    before it serves them. */
#define INPUT_ENDPOINT_CAPACITY 32

/** The most input devices the platform driver runs a driver for, and so
    the most endpoints a client reads at once, since each driver serves
    one: a keyboard, a tablet and a mouse, and one more. */
#define INPUT_DEVICES_MAX 4

/** What the platform driver sends a client on its bootstrap channel,
    which it keeps open while it supervises the client: messages of
    INPUT_CLIENT_MESSAGE_SIZE bytes and a transaction ID of 0. The first,
    the bootstrap message, carries the client's own handles, which
    console_protocol.h gives for the console; each after it one handle, an
    endpoint to read a description and then key events or pointer reports
    from, as a driver reported it, one once each driver has first reported
    and one after each restart of a driver. The client reads every
    endpoint it holds, closes one once it reports PEER_CLOSED, and ends
    once the platform driver closes the channel. */
#define INPUT_CLIENT_MESSAGE_SIZE 4

#endif
