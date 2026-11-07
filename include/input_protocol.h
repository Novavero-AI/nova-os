/* Key events from an input driver to its client, and how the platform
   driver hands a client its endpoint (docs/spec/input.md). Every field
   is little-endian. */

#ifndef NOVA_INPUT_PROTOCOL_H
#define NOVA_INPUT_PROTOCOL_H

#include "native_abi.h"

/* evdev's EV_KEY event type, and the values of a key event: released,
   pressed, and repeated while held (input.md cites the definitions). */
#define INPUT_TYPE_KEY 1
#define INPUT_KEY_RELEASED 0
#define INPUT_KEY_PRESSED 1
#define INPUT_KEY_REPEATED 2

/* One key event: a message of exactly these bytes and no handles. type,
   code and value are the evdev event's, as the device gave them; time is
   the monotonic clock, in nanoseconds, when the driver took the event
   from the device. */
typedef struct {
    UINT32 transaction; /* 0: an event answers no call (ipc.md I3) */
    UINT16 type;        /* INPUT_TYPE_KEY */
    UINT16 code;        /* the evdev key code */
    UINT32 value;       /* INPUT_KEY_RELEASED, _PRESSED or _REPEATED */
    UINT32 reserved;    /* 0 */
    UINT64 time;
} INPUT_EVENT;

_Static_assert(sizeof(INPUT_EVENT) == 24, "one layout on every target");

/* A client's bootstrap message from the platform driver: a transaction
   ID of 0 and two handles, the debug log and the endpoint it reads key
   events from. */
#define INPUT_CLIENT_HANDLE_DEBUG_LOG 0
#define INPUT_CLIENT_HANDLE_EVENTS 1
#define INPUT_CLIENT_HANDLES 2

#endif
