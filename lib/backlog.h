/** @file
    What the compositor keeps for a window whose channel cannot take what
    it has to send (docs/spec/display.md D11, rule 7): the input events
    that wait, the latest configure and frame, the buffers released since
    the window was last told, and how long the channel has stayed full.
    The compositor never waits for a client, so a window's messages go
    through a BACKLOG and the channel takes what it will.

    A function decides over the BACKLOG the caller keeps and makes no
    system call, keeps no state of its own and allocates nothing; the
    channel is a function the caller gives backlog_flush, so a host build
    drives it with a channel of any capacity (tools/check-backlog.c). The
    BACKLOG for a new window is all zero.

    What a window is sent, in this order, whatever order it was owed in:
    its latest configure, the buffers released, the latest frame, a
    WINDOW_DROPPED if events were dropped, and then the events, oldest
    first. The state messages go first because the events after them are
    read in the state they give: a key that follows a configure that gives
    the focus is the focus's, and a frame follows the configure its client
    draws for. A WINDOW_DROPPED goes just before the events, which are the
    ones that came after what it says was dropped. */

#ifndef NOVA_BACKLOG_H
#define NOVA_BACKLOG_H

#include "display_protocol.h"
#include "route.h"
#include "types.h"

/** @name What backlog_flush says of the channel
    @{ */
#define BACKLOG_IDLE 0    /**< nothing is owed: all went, or none was */
#define BACKLOG_WAITING 1 /**< the channel refused one: it waits, and
                               backlog_flush is called again when the
                               channel has room, or at
                               backlog_deadline */
#define BACKLOG_STALLED 2 /**< the channel has stayed full for
                               WINDOW_STALL_NS: close the window and log
                               it (D11, rule 7) */
#define BACKLOG_CLOSED 3  /**< the channel is closed, which its holder
                               did: close the window, and log nothing
                               (D19, rule 8) */
/** @} */

/** @name What the channel says of a message it is given
    Any other value is read as BACKLOG_SEND_FULL.
    @{ */
#define BACKLOG_SEND_TAKEN 1  /**< the channel took the message */
#define BACKLOG_SEND_FULL 2   /**< it is full: the message stays owed */
#define BACKLOG_SEND_CLOSED 3 /**< its other end has closed */
/** @} */

/** backlog_deadline's value while the channel is not known to be full. */
#define BACKLOG_NO_DEADLINE 0xFFFFFFFFFFFFFFFFull

/** A message the backlog gives the channel. */
typedef struct {
    UINT32 kind; /**< WINDOW_CONFIGURE, WINDOW_RELEASED, WINDOW_FRAME or
                      WINDOW_DROPPED, or the kind of an input event */
    union {
        WINDOW_CONFIGURE_EVENT configure; /**< WINDOW_CONFIGURE's */
        WINDOW_RELEASED_EVENT released;   /**< WINDOW_RELEASED's */
        WINDOW_FRAME_EVENT frame;         /**< WINDOW_FRAME's */
        WINDOW_INPUT input;               /**< an input event's */
    } body; /**< what the kind says is in use: none for WINDOW_DROPPED */
} BACKLOG_MESSAGE;

/** The window's channel, as the backlog is given it: send message on it,
    and return BACKLOG_SEND_TAKEN, BACKLOG_SEND_FULL or
    BACKLOG_SEND_CLOSED. context is what backlog_flush was given. The
    function does not call any function of the BACKLOG backlog_flush is
    working on: that changes the queue the flush is about to take its
    message from. */
typedef UINT32 (*BACKLOG_SEND)(void *context, const BACKLOG_MESSAGE *message);

/** What waits for a window. */
typedef struct {
    WINDOW_INPUT queue[WINDOW_BACKLOG]; /**< the events, a ring from head */
    UINT32 head;       /**< where the oldest is */
    UINT32 count;      /**< how many wait, at most WINDOW_BACKLOG */
    UINT32 dropped;    /**< nonzero: a WINDOW_DROPPED is owed */
    UINT32 configured; /**< nonzero: configure is owed */
    WINDOW_CONFIGURE_EVENT configure; /**< the latest configure */
    UINT32 framed;     /**< nonzero: a WINDOW_FRAME is owed */
    UINT64 frame_time; /**< the latest frame's time */
    UINT32 released;   /**< the buffers released since the window was told,
                            bit n for buffer n; 0 for none */
    UINT32 full;       /**< nonzero while the channel has refused since
                            full_since, with nothing taken */
    UINT64 full_since; /**< when it first refused, if full */
} BACKLOG;

/** Queue event for the window, to wait behind those that wait (D11, rule
    7). A motion, a pointer event with no button change and no wheel, is
    merged into the newest event that waits if that is a motion: it takes
    its place and time, and nothing more waits. Otherwise the event waits
    at the end, and if WINDOW_BACKLOG events wait already the oldest is
    dropped for it and a WINDOW_DROPPED is owed, once however many are
    dropped before it is sent. The event dropped may be a release or a
    leave: the window is told to treat its key and button state as
    unknown and the pointer as outside until its next entry. An event is
    queued even where the channel has room: backlog_flush then sends it,
    so none overtakes another. */
void backlog_push(BACKLOG *backlog, const WINDOW_INPUT *event);

/** Lay message out as the message the window's channel carries, by the
    generated encoder of its kind (display.desc), in bytes, which is
    aligned to 8 bytes and holds at least
    DISPLAY_WINDOW_TO_CLIENT_BYTES_MAX. Returns OK with *size set to the
    message's size, or the encoder's status, ERR_INVALID_ARGS, with *size 0
    if the message breaks its description or is of a kind the window's
    channel does not carry: backlog_flush's send function calls it for each
    message it is given. */
UINT32 backlog_encode(const BACKLOG_MESSAGE *message, void *bytes,
                      UINT32 *size);

/** Keep configure as the window's latest, in place of one not yet sent. */
void backlog_configure(BACKLOG *backlog,
                       const WINDOW_CONFIGURE_EVENT *configure);

/** Keep a frame that showed the window's present at time, in place of
    one not yet sent. */
void backlog_frame(BACKLOG *backlog, UINT64 time);

/** Note the buffers, a mask of WINDOW_BUFFERS_ALL's bits, that the
    compositor has stopped reading: the next WINDOW_RELEASED names every
    buffer so noted since the last. Bits outside WINDOW_BUFFERS_ALL are
    not noted. */
void backlog_release(BACKLOG *backlog, UINT32 buffers);

/** Send what is owed, in the order above, with send on context, until it
    has all gone or the channel refuses one, which stays owed. now is the
    time on clock_monotonic, in nanoseconds. Returns BACKLOG_IDLE where
    nothing is owed after it; BACKLOG_CLOSED where the channel is closed,
    which is no stall and starts and stops no clock; BACKLOG_STALLED where
    the channel refused and has refused, with nothing taken since, for
    WINDOW_STALL_NS; else BACKLOG_WAITING. A channel that takes a message
    after a refusal is not full, however long it was. */
UINT32 backlog_flush(BACKLOG *backlog, BACKLOG_SEND send, void *context,
                     UINT64 now);

/** The time at which a channel that has stayed full, from when it first
    refused, is stalled, so the compositor calls backlog_flush then even
    if the channel has not said it has room; BACKLOG_NO_DEADLINE where it
    is not known full. */
UINT64 backlog_deadline(const BACKLOG *backlog);

#endif
