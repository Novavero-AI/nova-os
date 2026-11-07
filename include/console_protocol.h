/** @file
    The console's protocols (docs/spec/console.md): a client's session,
    over which it writes text and reads lines typed at the console (C4),
    and the messages with which the first process starts the console and
    hands it the display and its sessions (C2). Every field is
    little-endian. */

#ifndef NOVA_CONSOLE_PROTOCOL_H
#define NOVA_CONSOLE_PROTOCOL_H

#include "native_abi.h"

/** @name Sessions
    Sessions (C4). A client writes CONSOLE_REQUESTs on its end of a
    session channel; the console answers each read with a CONSOLE_REPLY
    and the line. A message that is not one of the requests below ends the
    session: the console closes its end without a reply.
    @{ */

/** Show the length bytes that follow the request. No reply. */
#define CONSOLE_WRITE 1

/** Read a line of at most length bytes, edited at the console and ended
    by Enter. The reply carries the request's transaction ID, the line's
    length and the line, printable ASCII without its end. A session has
    at most one read waiting. */
#define CONSOLE_READ 2

/** A read's flag: the line is not shown as it is typed, nor is its end. */
#define CONSOLE_READ_NO_ECHO 0x1

/** The most bytes one write shows. */
#define CONSOLE_WRITE_MAX 4096
/** The most bytes one line holds. */
#define CONSOLE_LINE_MAX 256

/** The messages the console's end of a session queues (ipc.md I1); a
    client that fills it waits for CHANNEL_WRITABLE. */
#define CONSOLE_SESSION_CAPACITY 16

/** The sessions the console serves at once. */
#define CONSOLE_SESSIONS 8
/** @} */

/** A request on a session. */
typedef struct {
    UINT32 transaction; /**< echoed in a read's reply (ipc.md I3) */
    UINT32 operation;   /**< CONSOLE_WRITE or CONSOLE_READ */
    UINT32 length;      /**< a write's bytes, or a read's most */
    UINT32 flags;       /**< 0, or CONSOLE_READ_NO_ECHO on a read */
} CONSOLE_REQUEST;

/** The reply to a read, which the line follows. */
typedef struct {
    UINT32 transaction; /**< the read's */
    UINT32 length;      /**< the line's bytes, which follow */
} CONSOLE_REPLY;

_Static_assert(sizeof(CONSOLE_REQUEST) == 16, "one layout on every target");
_Static_assert(sizeof(CONSOLE_REPLY) == 8, "one layout on every target");
_Static_assert(sizeof(CONSOLE_REQUEST) + CONSOLE_WRITE_MAX <=
                   CHANNEL_MAX_BYTES,
               "a whole write fits one message");

/** @name Starting the console
    Starting the console (C2). Its bootstrap channel is the one the
    platform driver keeps to the input drivers' client (input.md K3).
    The first message there is INPUT_CLIENT_MESSAGE_SIZE bytes, a
    transaction ID of 0, and these handles in this order; each after it
    carries an input endpoint, as input_protocol.h gives.
    @{ */
#define CONSOLE_BOOTSTRAP_DEBUG_LOG 0 /**< the debug log's resource */
#define CONSOLE_BOOTSTRAP_SELF 1      /**< its process, with MAP */
#define CONSOLE_BOOTSTRAP_CONTROL 2   /**< its end of the control channel */
#define CONSOLE_BOOTSTRAP_HANDLES 3   /**< how many handles it carries */
/** @} */

/** @name The control channel
    Messages on the control channel, from the first process: a
    CONSOLE_SETUP of CONSOLE_SETUP_SIZE bytes, its kind saying what the one
    handle it carries is. CONSOLE_SETUP_DISPLAY carries an MMIO resource
    covering the framebuffer's pages, followed by the display's
    BOOTSTRAP_DISPLAY; CONSOLE_SETUP_SESSION carries the console's end of a
    session, with CHANNEL_RIGHT_READ, CHANNEL_RIGHT_WRITE and RIGHT_WAIT.
    The channel queues the display and a session for each the console
    serves.
    @{ */
#define CONSOLE_SETUP_DISPLAY 1 /**< the display */
#define CONSOLE_SETUP_SESSION 2 /**< a session */
#define CONSOLE_SETUP_SIZE 8    /**< the bytes of a setup */
/** the messages the control channel queues */
#define CONSOLE_CONTROL_CAPACITY (CONSOLE_SESSIONS + 1)
/** @} */

/** A message on the control channel. */
typedef struct {
    UINT32 transaction; /**< 0 */
    UINT32 kind;        /**< CONSOLE_SETUP_DISPLAY or CONSOLE_SETUP_SESSION */
} CONSOLE_SETUP;

/** The setup that hands the console the display. */
typedef struct {
    CONSOLE_SETUP setup;       /**< CONSOLE_SETUP_DISPLAY */
    BOOTSTRAP_DISPLAY display; /**< the display, as nova-boot drew on it */
} CONSOLE_SETUP_DISPLAY_MESSAGE;

_Static_assert(sizeof(CONSOLE_SETUP) == CONSOLE_SETUP_SIZE,
               "one layout on every target");

/** A client's end of a session: it writes requests and reads replies,
    and the end may move to the program that is to use it. */
#define CONSOLE_SESSION_RIGHTS                                            \
    (CHANNEL_RIGHT_READ | CHANNEL_RIGHT_WRITE | RIGHT_WAIT | RIGHT_TRANSFER)

#endif
