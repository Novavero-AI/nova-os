/* What the compositor keeps for a window whose channel cannot take what it
   has to send (docs/spec/display.md D11, rule 7). See backlog.h. */

#include "backlog.h"

#define UINT64_GREATEST 0xFFFFFFFFFFFFFFFFull

/* What owed gives the message of. */
#define OWED_NONE 0
#define OWED_CONFIGURE 1
#define OWED_RELEASED 2
#define OWED_FRAME 3
#define OWED_DROPPED 4
#define OWED_EVENT 5

/* Bytes, not a structure assignment or initializer: TinyCC turns those
   into calls to memmove and memset, and no program here links a C library
   (docs/spec/interface.md P2). */
static void copy_bytes(void *to, const void *from, UINT32 size)
{
    UINT8 *destination = (UINT8 *)to;
    const UINT8 *source = (const UINT8 *)from;
    UINT32 i;

    for (i = 0; i < size; i++)
        destination[i] = source[i];
}

static void clear_bytes(void *to, UINT32 size)
{
    UINT8 *destination = (UINT8 *)to;
    UINT32 i;

    for (i = 0; i < size; i++)
        destination[i] = 0;
}

/* ------------------------------------------------------------------------
   The events. */

/* A pointer event that changes no button and turns no wheel. */
static UINT32 is_motion(const WINDOW_INPUT *event)
{
    return event->kind == WINDOW_POINTER && event->body.pointer.buttons == 0 &&
           event->body.pointer.wheel == 0;
}

/* The place in the ring of the event that is index from the oldest. */
static UINT32 place(const BACKLOG *backlog, UINT32 index)
{
    return (backlog->head + index) % WINDOW_BACKLOG;
}

void backlog_push(BACKLOG *backlog, const WINDOW_INPUT *event)
{
    if (backlog->count != 0 && is_motion(event) &&
        is_motion(&backlog->queue[place(backlog, backlog->count - 1)])) {
        copy_bytes(&backlog->queue[place(backlog, backlog->count - 1)], event,
                   sizeof *event);
        return;
    }
    if (backlog->count == WINDOW_BACKLOG) {
        backlog->head = place(backlog, 1);
        backlog->count--;
        backlog->dropped = 1;
    }
    copy_bytes(&backlog->queue[place(backlog, backlog->count)], event,
               sizeof *event);
    backlog->count++;
}

/* ------------------------------------------------------------------------
   The state. */

void backlog_configure(BACKLOG *backlog,
                       const WINDOW_CONFIGURE_EVENT *configure)
{
    copy_bytes(&backlog->configure, configure, sizeof *configure);
    backlog->configured = 1;
}

void backlog_frame(BACKLOG *backlog, UINT64 time)
{
    backlog->frame_time = time;
    backlog->framed = 1;
}

void backlog_release(BACKLOG *backlog, UINT32 buffers)
{
    backlog->released |= buffers & WINDOW_BUFFERS_ALL;
}

/* ------------------------------------------------------------------------
   Sending. */

/* Set *message to the one owed first, in the order backlog.h gives, and
   return what it is of, or OWED_NONE where none is owed. What it is of,
   and not its kind, says what taken clears, so an event of a kind that is
   no input event's is sent and gone, not sent again. */
static UINT32 owed(const BACKLOG *backlog, BACKLOG_MESSAGE *message)
{
    clear_bytes(message, sizeof *message);
    if (backlog->configured) {
        message->kind = WINDOW_CONFIGURE;
        copy_bytes(&message->body.configure, &backlog->configure,
                   sizeof backlog->configure);
        return OWED_CONFIGURE;
    }
    if (backlog->released != 0) {
        message->kind = WINDOW_RELEASED;
        message->body.released.buffers = backlog->released;
        return OWED_RELEASED;
    }
    if (backlog->framed) {
        message->kind = WINDOW_FRAME;
        message->body.frame.time = backlog->frame_time;
        return OWED_FRAME;
    }
    if (backlog->dropped) {
        message->kind = WINDOW_DROPPED;
        return OWED_DROPPED;
    }
    if (backlog->count == 0)
        return OWED_NONE;
    copy_bytes(&message->body.input, &backlog->queue[place(backlog, 0)],
               sizeof message->body.input);
    message->kind = message->body.input.kind;
    return OWED_EVENT;
}

/* The message of what owed gave has gone. */
static void taken(BACKLOG *backlog, UINT32 source)
{
    switch (source) {
    case OWED_CONFIGURE:
        backlog->configured = 0;
        break;
    case OWED_RELEASED:
        backlog->released = 0;
        break;
    case OWED_FRAME:
        backlog->framed = 0;
        break;
    case OWED_DROPPED:
        backlog->dropped = 0;
        break;
    default:
        backlog->head = place(backlog, 1);
        backlog->count--;
        break;
    }
}

static UINT32 stalled(const BACKLOG *backlog, UINT64 now)
{
    return now >= backlog->full_since &&
           now - backlog->full_since >= WINDOW_STALL_NS;
}

UINT32 backlog_flush(BACKLOG *backlog, BACKLOG_SEND send, void *context,
                     UINT64 now)
{
    BACKLOG_MESSAGE message;
    UINT32 source;

    while ((source = owed(backlog, &message)) != OWED_NONE) {
        UINT32 result = send(context, &message);

        if (result == BACKLOG_SEND_CLOSED)
            return BACKLOG_CLOSED;
        if (result != BACKLOG_SEND_TAKEN) {
            if (!backlog->full) {
                backlog->full = 1;
                backlog->full_since = now;
            }
            return stalled(backlog, now) ? BACKLOG_STALLED : BACKLOG_WAITING;
        }
        taken(backlog, source);
        backlog->full = 0;
    }
    backlog->full = 0;
    return BACKLOG_IDLE;
}

UINT64 backlog_deadline(const BACKLOG *backlog)
{
    /* A deadline past the clock's range is no deadline: the monotonic clock
       does not reach it. */
    if (!backlog->full ||
        backlog->full_since > UINT64_GREATEST - WINDOW_STALL_NS)
        return BACKLOG_NO_DEADLINE;
    return backlog->full_since + WINDOW_STALL_NS;
}
