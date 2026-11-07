/** @file
    Key events from an input driver to its client, and how the platform
    driver hands a client its endpoint (docs/spec/input.md). Every field
    is little-endian. */

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

/** What the platform driver sends a client on its bootstrap channel,
    which it keeps open while it supervises the client: messages of
    INPUT_CLIENT_MESSAGE_SIZE bytes and a transaction ID of 0. The first,
    the bootstrap message, carries the client's own handles, which
    console_protocol.h gives for the console; each after it one handle, an
    endpoint to read key events from, as the driver reported it, one once
    the driver has first reported and one after each restart of the
    driver. The client takes the next endpoint once the one it reads
    reports PEER_CLOSED, and ends once the platform driver closes the
    channel. */
#define INPUT_CLIENT_MESSAGE_SIZE 4

#endif
