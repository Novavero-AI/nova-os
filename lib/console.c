/* The console client (console.h): a session's requests laid out by
   console_protocol.h's generated functions, which refuse what the
   description does not allow, and a read's reply accepted by its check
   (docs/spec/console.md C4, docs/spec/interface.md P14). A write has no
   reply; a read's reply carries its transaction. */

#include "console.h"
#include "native.h"
#include "wipe.h"

/* Transactions a program assigns itself have bit 31 clear (ipc.md I3). */
#define TRANSACTION_MASK 0x7fffffffu

/* The bytes of a request and of a reply, each aligned to 8 bytes and as
   long as the longest message of its direction, as the generated
   functions require (interface.md P14). */
static union {
    UINT64 alignment;
    UINT8 bytes[CONSOLE_SESSION_TO_SERVER_BYTES_MAX];
} request_buffer;
static union {
    UINT64 alignment;
    UINT8 bytes[CONSOLE_SESSION_TO_CLIENT_BYTES_MAX];
} reply_buffer;
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
    CONSOLE_REQUEST request;
    UINT32 piece, size, status = OK;

    request.transaction = 0;
    request.operation = 0;
    request.length = 0;
    request.flags = 0;
    while (status == OK && length > 0) {
        piece = length < CONSOLE_WRITE_MAX ? length : CONSOLE_WRITE_MAX;
        status = console_session_write_encode(&request, text, piece,
                                              request_buffer.bytes, &size);
        if (status == OK)
            status = send(session, request_buffer.bytes, size);
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
    const CONSOLE_REPLY *reply = (const CONSOLE_REPLY *)reply_buffer.bytes;
    CONSOLE_REQUEST request;
    const UINT8 *text;
    UINT32 status, observed, bytes = 0, count = 0, size, i;

    *length = 0;
    request.transaction = next_transaction();
    request.operation = 0;
    request.length = capacity;
    request.flags = echo == CONSOLE_NO_ECHO ? CONSOLE_READ_NO_ECHO : 0;
    status = console_session_read_encode(&request, request_buffer.bytes,
                                         &size);
    if (status == OK)
        status = send(session, request_buffer.bytes, size);
    if (status == OK)
        status = object_wait_one(session,
                                 CHANNEL_READABLE | CHANNEL_PEER_CLOSED,
                                 DEADLINE_INFINITE, &observed);
    if (status == OK)
        status = channel_read(session, reply_buffer.bytes,
                              sizeof reply_buffer.bytes, handles,
                              CHANNEL_MAX_HANDLES, &bytes, &count);
    for (i = 0; status == OK && i < count; i++)
        handle_close(handles[i].handle);
    if (status == OK &&
        (console_session_line_check(reply_buffer.bytes, bytes, handles,
                                    count) != OK ||
         reply->transaction != request.transaction ||
         reply->length > capacity))
        status = ERR_BAD_STATE;
    if (status == OK) {
        text = console_session_line_line(reply_buffer.bytes);
        for (i = 0; i < reply->length; i++)
            line[i] = text[i];
        *length = reply->length;
    }
    /* A line read without echo is a password: no copy of it stays. */
    wipe(reply_buffer.bytes, sizeof reply_buffer.bytes);
    return status;
}
