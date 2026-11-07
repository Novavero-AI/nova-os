/* The console client (console.h): CONSOLE_REQUESTs on a session, as
   console_protocol.h lays them out. A write has no reply; a read's reply
   carries its transaction. */

#include "console.h"
#include "native.h"
#include "wipe.h"

/* Transactions a program assigns itself have bit 31 clear (ipc.md I3). */
#define TRANSACTION_MASK 0x7fffffffu

static UINT8 request_bytes[sizeof(CONSOLE_REQUEST) + CONSOLE_WRITE_MAX];
static UINT8 reply_bytes[sizeof(CONSOLE_REPLY) + CONSOLE_LINE_MAX];
static UINT32 last_transaction;

/* A message with no handles still names an array in user memory, which
   channel_write checks whatever the count (syscalls.md S6). */
static const HANDLE_TRANSFER no_handles[1];

static UINT32 next_transaction(void)
{
    last_transaction = (last_transaction + 1) & TRANSACTION_MASK;
    return last_transaction;
}

/* Write a message of length bytes on session, waiting for room while
   the console's queue is full (C4). */
static UINT32 send(UINT32 session, const void *bytes, UINT32 length)
{
    UINT32 status, observed;

    for (;;) {
        status = channel_write(session, bytes, length, no_handles, 0);
        if (status != ERR_SHOULD_WAIT)
            return status;
        status = object_wait_one(session,
                                 CHANNEL_WRITABLE | CHANNEL_PEER_CLOSED,
                                 DEADLINE_INFINITE, &observed);
        if (status != OK)
            return status;
        if (!(observed & CHANNEL_WRITABLE))
            return ERR_PEER_CLOSED;
    }
}

UINT32 console_write(UINT32 session, const UINT8 *text, UINT32 length)
{
    CONSOLE_REQUEST *request = (CONSOLE_REQUEST *)request_bytes;
    UINT32 piece, i, status = OK;

    while (status == OK && length > 0) {
        piece = length < CONSOLE_WRITE_MAX ? length : CONSOLE_WRITE_MAX;
        request->transaction = 0;
        request->operation = CONSOLE_WRITE;
        request->length = piece;
        request->flags = 0;
        for (i = 0; i < piece; i++)
            request_bytes[sizeof *request + i] = text[i];
        status = send(session, request_bytes, sizeof *request + piece);
        text += piece;
        length -= piece;
    }
    return status;
}

UINT32 console_say(UINT32 session, const char *text)
{
    UINT32 length = 0;

    while (text[length])
        length++;
    return console_write(session, (const UINT8 *)text, length);
}

UINT32 console_read_line(UINT32 session, CONSOLE_ECHO_MODE echo, UINT8 *line,
                         UINT32 capacity, UINT32 *length)
{
    static RECEIVED_HANDLE handles[CHANNEL_MAX_HANDLES];
    const CONSOLE_REPLY *reply = (const CONSOLE_REPLY *)reply_bytes;
    CONSOLE_REQUEST request;
    UINT32 status, observed, bytes = 0, count = 0, i;

    *length = 0;
    if (capacity == 0 || capacity > CONSOLE_LINE_MAX)
        return ERR_INVALID_ARGS;
    request.transaction = next_transaction();
    request.operation = CONSOLE_READ;
    request.length = capacity;
    request.flags = echo == CONSOLE_NO_ECHO ? CONSOLE_READ_NO_ECHO : 0;
    status = send(session, &request, sizeof request);
    if (status == OK)
        status = object_wait_one(session,
                                 CHANNEL_READABLE | CHANNEL_PEER_CLOSED,
                                 DEADLINE_INFINITE, &observed);
    if (status == OK)
        status = channel_read(session, reply_bytes, sizeof reply_bytes,
                              handles, CHANNEL_MAX_HANDLES, &bytes, &count);
    for (i = 0; status == OK && i < count; i++)
        handle_close(handles[i].handle);
    if (status == OK &&
        (count != 0 || bytes < sizeof *reply ||
         reply->transaction != request.transaction ||
         reply->length > capacity || bytes != sizeof *reply + reply->length))
        status = ERR_BAD_STATE;
    if (status == OK) {
        for (i = 0; i < reply->length; i++)
            line[i] = reply_bytes[sizeof *reply + i];
        *length = reply->length;
    }
    /* A line read without echo is a password: no copy of it stays. */
    wipe(reply_bytes, sizeof reply_bytes);
    return status;
}
