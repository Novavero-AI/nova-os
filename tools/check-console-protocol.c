/* Checks include/console_protocol.h, which tools/mkinterface.c makes of
   interfaces/console.desc (docs/spec/interface.md P17, P19), against the
   messages docs/spec/console.md C2 and C4 give: each message encoded with
   the values of their tables is the bytes a structure laid out by hand
   holds, with the handles' rights those tables give, and passes its own
   check and its channel's dispatcher; and each message C4 says ends a
   session, or that C2 refuses, fails its check. A read that comes while
   the session's earlier read waits is the console's own state, not a
   message's, and is outside this check. Prints each failure and a count,
   and exits nonzero if any check fails.

   cc -std=c11 -Wall -Wextra -pedantic -I include \
     -o check-console-protocol tools/check-console-protocol.c &&
     ./check-console-protocol */

#include <stdio.h>
#include <string.h>

#include "console_protocol.h"

/* The sizes console.md C2 and C4 give, and the one the generated limit
   holds: a whole write is a request and CONSOLE_WRITE_MAX bytes. */
_Static_assert(sizeof(CONSOLE_REQUEST) == 16, "C4: a request is 16 bytes");
_Static_assert(sizeof(CONSOLE_REPLY) == 8, "C4: a reply is 8 bytes");
_Static_assert(sizeof(CONSOLE_SETUP) == CONSOLE_SETUP_SIZE &&
                   CONSOLE_SETUP_SIZE == 8,
               "C2: a setup is 8 bytes");
_Static_assert(CONSOLE_SESSION_TO_SERVER_BYTES_MAX == 16 + 4096,
               "C4: a write is a request and at most 4,096 bytes");
_Static_assert(CONSOLE_BOOTSTRAP_DEBUG_LOG == 0 && CONSOLE_BOOTSTRAP_SELF == 1 &&
                   CONSOLE_BOOTSTRAP_CONTROL == 2 &&
                   CONSOLE_BOOTSTRAP_HANDLES == 3,
               "C2: the bootstrap message's handles in this order");
_Static_assert(CONSOLE_WRITE == 1 && CONSOLE_READ == 2 &&
                   CONSOLE_SETUP_DISPLAY == 1 && CONSOLE_SETUP_SESSION == 2 &&
                   CONSOLE_SETUP_BACKGROUND == 3 &&
                   CONSOLE_SETUP_DISPLAY_AFTER == 5 &&
                   CONSOLE_SETUP_OPTIONS == 6,
               "C2 and C4: the operations and the kinds");
_Static_assert(CONSOLE_OPTION_REDUCE_MOTION == 0x1 &&
                   CONSOLE_OPTIONS == CONSOLE_OPTION_REDUCE_MOTION,
               "C2: the options' one bit");
_Static_assert(CONSOLE_CONTROL_CAPACITY == CONSOLE_SESSIONS + 3,
               "C2: the control channel queues the options, the background "
               "or a connection, the display or the display after a "
               "compositor, and a session for each the console serves");

#define DISPLAY_RECORD_BYTES 24
#define BACKGROUND_LABEL_BYTES 16
#define BACKGROUND_MESSAGE_BYTES (8 + DISPLAY_RECORD_BYTES + BACKGROUND_LABEL_BYTES)

static unsigned failures;

static void check(int passed, const char *what)
{
    if (passed)
        return;
    printf("failed: %s\n", what);
    failures++;
}

/* The bytes of a message, aligned to 8 bytes as the generated functions
   require, with room for the longest message and a byte more. */
static union {
    UINT64 alignment;
    UINT8 bytes[CONSOLE_SESSION_TO_SERVER_BYTES_MAX + 16];
} buffer;

static void set32(UINT8 *at, UINT32 value)
{
    at[0] = (UINT8)value;
    at[1] = (UINT8)(value >> 8);
    at[2] = (UINT8)(value >> 16);
    at[3] = (UINT8)(value >> 24);
}

static void set64(UINT8 *at, UINT64 value)
{
    set32(at, (UINT32)value);
    set32(at + 4, (UINT32)(value >> 32));
}

/* The bytes of the display record console.md C2 gives: the physical
   address of pixel (0, 0), then the width, height, stride and format. */
static void display_record(UINT8 *at)
{
    set64(at, 0xFD00000000ull);
    set32(at + 8, 1280);
    set32(at + 12, 800);
    set32(at + 16, 1280);
    set32(at + 20, 1);
}

static const RECEIVED_HANDLE none[1] = {{0, 0, 0}};

#define CONTROL_RIGHTS (CHANNEL_RIGHT_READ | CHANNEL_RIGHT_WRITE | RIGHT_WAIT)

static int same(const void *got, const UINT8 *want, UINT32 count)
{
    return memcmp(got, want, count) == 0;
}

/* A request laid out as C4 gives it, with a transaction, an operation
   byte as the caller leaves it and the rest as given. */
static void request(UINT32 transaction, UINT32 operation, UINT32 length,
                    UINT32 flags)
{
    memset(buffer.bytes, 0, sizeof buffer.bytes);
    set32(buffer.bytes, transaction);
    set32(buffer.bytes + 4, operation);
    set32(buffer.bytes + 8, length);
    set32(buffer.bytes + 12, flags);
}

/* The dispatcher's verdict on the first count bytes of the buffer, with
   handle_count handles. */
static UINT32 session_verdict(UINT32 count, UINT32 handle_count,
                              UINT32 *which)
{
    return console_session_request_check(buffer.bytes, count, none,
                                         handle_count, which);
}

static void check_session_messages(void)
{
    CONSOLE_REQUEST body;
    CONSOLE_REPLY reply;
    UINT8 want[16 + 2];
    UINT32 size = 7, which = 0, status;

    /* A write of two bytes: transaction 0, operation 1, length 2, flags 0. */
    body.transaction = 0;
    body.operation = 0;
    body.length = 0;
    body.flags = 0;
    status = console_session_write_encode(&body, (const UINT8 *)"hi", 2,
                                          buffer.bytes, &size);
    memset(want, 0, sizeof want);
    set32(want + 4, 1);
    set32(want + 8, 2);
    want[16] = 'h';
    want[17] = 'i';
    check(status == OK && size == 18 && same(buffer.bytes, want, 18),
          "a write of two bytes is C4's 16 bytes and the two");
    check(session_verdict(18, 0, &which) == OK && which == CONSOLE_WRITE,
          "a write passes the dispatcher and is named a write");
    check(console_session_write_check(buffer.bytes, 18, none, 0) == OK,
          "a write passes its check");
    check(console_session_write_text(buffer.bytes) == buffer.bytes + 16,
          "a write's bytes start after its request");
    status = console_session_write_encode(&body, (const UINT8 *)"", 0,
                                          buffer.bytes, &size);
    check(status == OK && size == 16, "a write of no bytes is its request");
    status = console_session_write_encode(&body, buffer.bytes,
                                          CONSOLE_WRITE_MAX, buffer.bytes,
                                          &size);
    check(status == OK && size == 16 + CONSOLE_WRITE_MAX,
          "a write of CONSOLE_WRITE_MAX bytes is encoded");
    status = console_session_write_encode(&body, buffer.bytes,
                                          CONSOLE_WRITE_MAX + 1, buffer.bytes,
                                          &size);
    check(status == ERR_INVALID_ARGS && size == 0,
          "a write of more than CONSOLE_WRITE_MAX bytes is refused");
    body.flags = 1;
    status = console_session_write_encode(&body, (const UINT8 *)"hi", 2,
                                          buffer.bytes, &size);
    check(status == ERR_INVALID_ARGS && size == 0,
          "a write whose flags are not 0 is refused");

    /* A read: transaction 7, operation 2, length 80, flags 1. */
    body.transaction = 7;
    body.length = 80;
    body.flags = CONSOLE_READ_NO_ECHO;
    status = console_session_read_encode(&body, buffer.bytes, &size);
    memset(want, 0, sizeof want);
    set32(want, 7);
    set32(want + 4, 2);
    set32(want + 8, 80);
    set32(want + 12, 1);
    check(status == OK && size == 16 && same(buffer.bytes, want, 16),
          "a read is C4's 16 bytes");
    check(session_verdict(16, 0, &which) == OK && which == CONSOLE_READ,
          "a read passes the dispatcher and is named a read");
    body.length = 0;
    check(console_session_read_encode(&body, buffer.bytes, &size) ==
              ERR_INVALID_ARGS,
          "a read of no bytes is refused");
    body.length = CONSOLE_LINE_MAX + 1;
    check(console_session_read_encode(&body, buffer.bytes, &size) ==
              ERR_INVALID_ARGS,
          "a read of more than CONSOLE_LINE_MAX bytes is refused");
    body.length = CONSOLE_LINE_MAX;
    body.flags = 2;
    check(console_session_read_encode(&body, buffer.bytes, &size) ==
              ERR_INVALID_ARGS,
          "a read with another flag is refused");

    /* A read's reply: the request's transaction, the line's length and
       the line. */
    reply.transaction = 7;
    reply.length = 0;
    status = console_session_line_encode(&reply, (const UINT8 *)"abc", 3,
                                         buffer.bytes, &size);
    memset(want, 0, sizeof want);
    set32(want, 7);
    set32(want + 4, 3);
    want[8] = 'a';
    want[9] = 'b';
    want[10] = 'c';
    check(status == OK && size == 11 && same(buffer.bytes, want, 11),
          "a reply is C4's 8 bytes and the line");
    check(console_session_line_check(buffer.bytes, 11, none, 0) == OK,
          "a reply passes its check");
    check(console_session_line_check(buffer.bytes, 12, none, 0) ==
              ERR_INVALID_ARGS,
          "a reply with a byte more than its line is refused");
    check(console_session_line_check(buffer.bytes, 11, none, 1) ==
              ERR_INVALID_ARGS,
          "a reply that carries a handle is refused");
    check(console_session_line_line(buffer.bytes) == buffer.bytes + 8,
          "a reply's line starts after its length");
}

/* Each message C4 says ends a session fails the dispatcher. */
static void check_session_refusals(void)
{
    UINT32 which = 0;

    request(1, CONSOLE_READ, 80, 0);
    check(session_verdict(15, 0, &which) == ERR_INVALID_ARGS,
          "a message shorter than a request is refused");
    check(session_verdict(7, 0, &which) == ERR_INVALID_ARGS &&
              which == CONSOLE_SESSION_UNNAMED,
          "a message too short to name its operation is unnamed");
    request(1, 0, 80, 0);
    check(session_verdict(16, 0, &which) == ERR_NOT_SUPPORTED,
          "an operation 0 is refused");
    request(1, 3, 80, 0);
    check(session_verdict(16, 0, &which) == ERR_NOT_SUPPORTED &&
              which == 3,
          "an unknown operation is refused and named as it is");
    request(1, CONSOLE_WRITE, CONSOLE_WRITE_MAX + 1, 0);
    check(session_verdict(16 + CONSOLE_WRITE_MAX + 1, 0, &which) ==
              ERR_INVALID_ARGS,
          "a write whose length is above CONSOLE_WRITE_MAX is refused");
    request(1, CONSOLE_WRITE, 5, 0);
    check(session_verdict(16 + 3, 0, &which) == ERR_INVALID_ARGS,
          "a write with fewer bytes than its length is refused");
    request(1, CONSOLE_WRITE, 3, 0);
    check(session_verdict(16 + 5, 0, &which) == ERR_INVALID_ARGS,
          "a write with more bytes than its length is refused");
    request(1, CONSOLE_WRITE, 3, 1);
    check(session_verdict(16 + 3, 0, &which) == ERR_INVALID_ARGS,
          "a write whose flags are not 0 is refused");
    request(1, CONSOLE_WRITE, 3, 0);
    check(session_verdict(16 + 3, 1, &which) == ERR_INVALID_ARGS,
          "a write that carries a handle is refused");
    request(1, CONSOLE_READ, 80, 0);
    check(session_verdict(17, 0, &which) == ERR_INVALID_ARGS,
          "a read with a byte after it is refused");
    request(1, CONSOLE_READ, 0, 0);
    check(session_verdict(16, 0, &which) == ERR_INVALID_ARGS,
          "a read whose length is 0 is refused");
    request(1, CONSOLE_READ, CONSOLE_LINE_MAX + 1, 0);
    check(session_verdict(16, 0, &which) == ERR_INVALID_ARGS,
          "a read whose length is above CONSOLE_LINE_MAX is refused");
    request(1, CONSOLE_READ, CONSOLE_LINE_MAX, 2);
    check(session_verdict(16, 0, &which) == ERR_INVALID_ARGS,
          "a read with another flag is refused");
    request(1, CONSOLE_READ, CONSOLE_LINE_MAX, CONSOLE_READ_NO_ECHO);
    check(session_verdict(16, 0, &which) == OK,
          "a read of CONSOLE_LINE_MAX bytes without echo passes");
    request(1, CONSOLE_READ, 1, 0);
    check(session_verdict(16, 1, &which) == ERR_INVALID_ARGS,
          "a read that carries a handle is refused");
}

static void check_control_messages(void)
{
    CONSOLE_SETUP_DISPLAY_MESSAGE display;
    CONSOLE_SETUP_BACKGROUND_MESSAGE background;
    CONSOLE_SETUP_OPTIONS_MESSAGE options;
    CONSOLE_SETUP setup;
    HANDLE_TRANSFER transfers[BOOTSTRAP_BACKGROUND_SECTIONS_MAX];
    UINT32 values[BOOTSTRAP_BACKGROUND_SECTIONS_MAX];
    RECEIVED_HANDLE received[BOOTSTRAP_BACKGROUND_SECTIONS_MAX + 1];
    UINT8 want[BACKGROUND_MESSAGE_BYTES];
    UINT32 size = 7, which = 0, status, i;

    /* The display: a setup of kind 1, the display record and a
       resource. */
    display.setup.transaction = 0;
    display.setup.kind = 0;
    display.display.base = 0xFD00000000ull;
    display.display.width = 1280;
    display.display.height = 800;
    display.display.stride = 1280;
    display.display.format = 1;
    values[0] = 5;
    status = console_control_display_encode(&display, values, 1, transfers,
                                            buffer.bytes, &size);
    memset(want, 0, sizeof want);
    set32(want + 4, CONSOLE_SETUP_DISPLAY);
    display_record(want + 8);
    check(status == OK && size == 32 && same(buffer.bytes, want, 32),
          "a display setup is a setup and the display record");
    check(transfers[0].handle == 5 && transfers[0].rights == RESOURCE_RIGHT_USE,
          "the display's resource travels with USE");
    received[0].handle = 5;
    received[0].type = TYPE_RESOURCE;
    received[0].rights = RESOURCE_RIGHT_USE;
    check(console_control_request_check(buffer.bytes, 32, received, 1,
                                        &which) == OK &&
              which == CONSOLE_SETUP_DISPLAY,
          "a display setup passes the dispatcher and is named the display");
    received[0].type = TYPE_SECTION;
    check(console_control_request_check(buffer.bytes, 32, received, 1,
                                        &which) == ERR_INVALID_ARGS,
          "a display setup whose handle is no resource is refused");
    received[0].type = TYPE_RESOURCE;
    check(console_control_request_check(buffer.bytes, 32, received, 0,
                                        &which) == ERR_INVALID_ARGS,
          "a display setup with no handle is refused");

    /* The display after a compositor: the display's message under kind
       5, with the same record and resource. */
    display.setup.kind = 0;
    values[0] = 5;
    status = console_control_display_after_encode(&display, values, 1,
                                                  transfers, buffer.bytes,
                                                  &size);
    memset(want, 0, sizeof want);
    set32(want + 4, CONSOLE_SETUP_DISPLAY_AFTER);
    display_record(want + 8);
    check(status == OK && size == 32 && same(buffer.bytes, want, 32),
          "a display after a compositor is a setup of kind 5 and the "
          "display record");
    check(transfers[0].handle == 5 && transfers[0].rights == RESOURCE_RIGHT_USE,
          "the resource of a display after a compositor travels with USE");
    received[0].handle = 5;
    received[0].type = TYPE_RESOURCE;
    received[0].rights = RESOURCE_RIGHT_USE;
    check(console_control_request_check(buffer.bytes, 32, received, 1,
                                        &which) == OK &&
              which == CONSOLE_SETUP_DISPLAY_AFTER,
          "a display after a compositor passes the dispatcher and is named "
          "so");
    received[0].type = TYPE_SECTION;
    check(console_control_request_check(buffer.bytes, 32, received, 1,
                                        &which) == ERR_INVALID_ARGS,
          "a display after a compositor whose handle is no resource is "
          "refused");
    received[0].type = TYPE_RESOURCE;
    check(console_control_request_check(buffer.bytes, 32, received, 0,
                                        &which) == ERR_INVALID_ARGS,
          "a display after a compositor with no handle is refused");

    /* A session: a setup of kind 2 and the console's end. */
    setup.transaction = 0;
    setup.kind = 0;
    values[0] = 6;
    status = console_control_session_encode(&setup, values, 1, transfers,
                                            buffer.bytes, &size);
    memset(want, 0, sizeof want);
    set32(want + 4, CONSOLE_SETUP_SESSION);
    check(status == OK && size == 8 && same(buffer.bytes, want, 8),
          "a session setup is a setup alone");
    check(transfers[0].handle == 6 && transfers[0].rights == CONTROL_RIGHTS,
          "the session's end travels with READ, WRITE and WAIT");
    received[0].handle = 6;
    received[0].type = TYPE_CHANNEL;
    received[0].rights = CONTROL_RIGHTS;
    check(console_control_request_check(buffer.bytes, 8, received, 1,
                                        &which) == OK &&
              which == CONSOLE_SETUP_SESSION,
          "a session setup passes and is named a session");
    received[0].rights = CONTROL_RIGHTS | RIGHT_TRANSFER;
    check(console_control_request_check(buffer.bytes, 8, received, 1,
                                        &which) == OK,
          "a session end with more rights than it needs passes");
    received[0].rights = CHANNEL_RIGHT_READ | CHANNEL_RIGHT_WRITE;
    check(console_control_request_check(buffer.bytes, 8, received, 1,
                                        &which) == ERR_INVALID_ARGS,
          "a session end without WAIT is refused");
    received[0].rights = CONTROL_RIGHTS;
    check(console_control_request_check(buffer.bytes, 9, received, 1,
                                        &which) == ERR_INVALID_ARGS,
          "a session setup with a byte more is refused");
    check(console_control_request_check(buffer.bytes, 8, received, 2,
                                        &which) == ERR_INVALID_ARGS,
          "a session setup with two handles is refused");
    set32(buffer.bytes, 1);
    check(console_control_request_check(buffer.bytes, 8, received, 1,
                                        &which) == ERR_INVALID_ARGS,
          "a setup whose transaction is not 0 is refused");
    set32(buffer.bytes, 0);
    set32(buffer.bytes + 4, 4);
    check(console_control_request_check(buffer.bytes, 8, received, 1,
                                        &which) == ERR_NOT_SUPPORTED &&
              which == 4,
          "a setup of a kind console.desc does not have yet is refused");
    check(console_control_request_check(buffer.bytes, 7, received, 1,
                                        &which) == ERR_INVALID_ARGS &&
              which == CONSOLE_CONTROL_UNNAMED,
          "a setup too short to hold its kind is unnamed");

    /* The options: a setup of kind 6 and the option bits, with no
       handle. */
    options.setup.transaction = 0;
    options.setup.kind = 0;
    options.options = CONSOLE_OPTION_REDUCE_MOTION;
    status = console_control_options_encode(&options, buffer.bytes, &size);
    memset(want, 0, sizeof want);
    set32(want + 4, CONSOLE_SETUP_OPTIONS);
    set32(want + 8, CONSOLE_OPTION_REDUCE_MOTION);
    check(status == OK && size == 12 && same(buffer.bytes, want, 12),
          "an options setup is a setup and the option bits");
    check(console_control_request_check(buffer.bytes, 12, none, 0, &which) ==
                  OK &&
              which == CONSOLE_SETUP_OPTIONS,
          "an options setup passes the dispatcher and is named the options");
    check(console_control_request_check(buffer.bytes, 12, received, 1,
                                        &which) == ERR_INVALID_ARGS,
          "an options setup that carries a handle is refused");
    check(console_control_request_check(buffer.bytes, 11, none, 0, &which) ==
              ERR_INVALID_ARGS,
          "an options setup of eleven bytes is refused");
    check(console_control_request_check(buffer.bytes, 13, none, 0, &which) ==
              ERR_INVALID_ARGS,
          "an options setup with a byte more is refused");
    set32(want + 8, 2);
    check(console_control_request_check(want, 12, none, 0, &which) ==
              ERR_INVALID_ARGS,
          "an options setup with a bit no option has is refused");
    set32(want + 8, 0);
    check(console_control_request_check(want, 12, none, 0, &which) == OK,
          "an options setup of no bits passes");
    set32(want, 1);
    check(console_control_request_check(want, 12, none, 0, &which) ==
              ERR_INVALID_ARGS,
          "an options setup whose transaction is not 0 is refused");
    options.options = 0;
    status = console_control_options_encode(&options, buffer.bytes, &size);
    check(status == OK && size == 12,
          "an options setup of no bits is encoded");
    options.options = CONSOLE_OPTIONS + 1;
    status = console_control_options_encode(&options, buffer.bytes, &size);
    check(status == ERR_INVALID_ARGS && size == 0,
          "an options setup with a bit no option has is not encoded");

    /* The background: a setup of kind 3, the display it is to be given,
       the label, and the kernel's sections with READ. */
    background.setup.transaction = 0;
    background.setup.kind = 0;
    background.display = display.display;
    background.background.background = 1;
    background.background.width = 1280;
    background.background.height = 800;
    background.background.format = 1;
    for (i = 0; i < 3; i++)
        values[i] = 10 + i;
    status = console_control_background_encode(&background, values, 3,
                                               transfers, buffer.bytes,
                                               &size);
    memset(want, 0, sizeof want);
    set32(want + 4, CONSOLE_SETUP_BACKGROUND);
    display_record(want + 8);
    set32(want + 32, 1);
    set32(want + 36, 1280);
    set32(want + 40, 800);
    set32(want + 44, 1);
    check(status == OK && size == 48 && same(buffer.bytes, want, 48),
          "a background setup is a setup, the display and the label");
    check(transfers[2].handle == 12 && transfers[2].rights == SECTION_RIGHT_READ,
          "the background's sections travel with READ");
    for (i = 0; i < 3; i++) {
        received[i].handle = 10 + i;
        received[i].type = TYPE_SECTION;
        received[i].rights = SECTION_RIGHT_READ;
    }
    check(console_control_request_check(buffer.bytes, 48, received, 3,
                                        &which) == OK &&
              which == CONSOLE_SETUP_BACKGROUND,
          "a background setup passes and is named the background");
    check(console_control_request_check(buffer.bytes, 48, received, 0,
                                        &which) == OK,
          "a background setup with no section passes");
    received[1].rights = 0;
    check(console_control_request_check(buffer.bytes, 48, received, 3,
                                        &which) == ERR_INVALID_ARGS,
          "a background section without READ is refused");
    check(console_control_background_encode(&background, values,
                                            BOOTSTRAP_BACKGROUND_SECTIONS_MAX +
                                                1,
                                            transfers, buffer.bytes,
                                            &size) == ERR_INVALID_ARGS,
          "a background of more than 64 sections is refused");

    /* The answer: a transaction of 0 and no handle. */
    status = console_control_prepared_encode(0, buffer.bytes, &size);
    memset(want, 0, sizeof want);
    check(status == OK && size == CONSOLE_PREPARED_SIZE &&
              same(buffer.bytes, want, 4),
          "the answer to a background is a transaction of 0");
    check(console_control_prepared_check(buffer.bytes, 4, none, 0) == OK,
          "the answer passes its check");
    check(console_control_prepared_check(buffer.bytes, 4, none, 1) ==
              ERR_INVALID_ARGS,
          "an answer that carries a handle is refused");
    check(console_control_prepared_check(buffer.bytes, 5, none, 0) ==
              ERR_INVALID_ARGS,
          "an answer of five bytes is refused");
    set32(buffer.bytes, 1);
    check(console_control_prepared_check(buffer.bytes, 4, none, 0) ==
              ERR_INVALID_ARGS,
          "an answer with a transaction other than 0 is refused");
    check(console_control_prepared_encode(1, buffer.bytes, &size) ==
                  ERR_INVALID_ARGS &&
              size == 0,
          "an answer with a transaction other than 0 is not encoded");
}

static void check_bootstrap_messages(void)
{
    HANDLE_TRANSFER transfers[CONSOLE_BOOTSTRAP_HANDLES];
    UINT32 values[CONSOLE_BOOTSTRAP_HANDLES] = {1, 2, 3};
    RECEIVED_HANDLE received[CONSOLE_BOOTSTRAP_HANDLES];
    UINT8 want[4] = {0, 0, 0, 0};
    UINT32 size = 7, status;

    /* The first message: a transaction of 0 and the debug log, the
       console's process and its end of the control channel. */
    status = console_bootstrap_start_encode(0, values,
                                            CONSOLE_BOOTSTRAP_HANDLES,
                                            transfers, buffer.bytes, &size);
    check(status == OK && size == 4 && same(buffer.bytes, want, 4),
          "the bootstrap message is a transaction of 0");
    check(transfers[CONSOLE_BOOTSTRAP_DEBUG_LOG].rights ==
                  RESOURCE_RIGHT_USE &&
              transfers[CONSOLE_BOOTSTRAP_SELF].rights == PROCESS_RIGHT_MAP &&
              transfers[CONSOLE_BOOTSTRAP_CONTROL].rights == CONTROL_RIGHTS &&
              transfers[CONSOLE_BOOTSTRAP_CONTROL].handle == 3,
          "the bootstrap message's handles travel with C2's rights");
    received[CONSOLE_BOOTSTRAP_DEBUG_LOG].type = TYPE_RESOURCE;
    received[CONSOLE_BOOTSTRAP_DEBUG_LOG].rights = RESOURCE_RIGHT_USE;
    received[CONSOLE_BOOTSTRAP_SELF].type = TYPE_PROCESS;
    received[CONSOLE_BOOTSTRAP_SELF].rights = PROCESS_RIGHT_MAP | RIGHT_TRANSFER;
    received[CONSOLE_BOOTSTRAP_CONTROL].type = TYPE_CHANNEL;
    received[CONSOLE_BOOTSTRAP_CONTROL].rights = CONTROL_RIGHTS;
    check(console_bootstrap_start_check(buffer.bytes, 4, received,
                                        CONSOLE_BOOTSTRAP_HANDLES) == OK,
          "the bootstrap message passes its check");
    check(console_bootstrap_start_check(buffer.bytes, 4, received, 2) ==
              ERR_INVALID_ARGS,
          "a bootstrap message with two handles is refused");
    received[CONSOLE_BOOTSTRAP_SELF].rights = RIGHT_TRANSFER;
    check(console_bootstrap_start_check(buffer.bytes, 4, received,
                                        CONSOLE_BOOTSTRAP_HANDLES) ==
              ERR_INVALID_ARGS,
          "a bootstrap message whose process lacks MAP is refused");
    received[CONSOLE_BOOTSTRAP_SELF].rights = PROCESS_RIGHT_MAP;
    received[CONSOLE_BOOTSTRAP_DEBUG_LOG].type = TYPE_CHANNEL;
    check(console_bootstrap_start_check(buffer.bytes, 4, received,
                                        CONSOLE_BOOTSTRAP_HANDLES) ==
              ERR_INVALID_ARGS,
          "a bootstrap message whose log is no resource is refused");
    received[CONSOLE_BOOTSTRAP_DEBUG_LOG].type = TYPE_RESOURCE;
    set32(buffer.bytes, 1);
    check(console_bootstrap_start_check(buffer.bytes, 4, received,
                                        CONSOLE_BOOTSTRAP_HANDLES) ==
              ERR_INVALID_ARGS,
          "a bootstrap message whose transaction is not 0 is refused");

    /* Each message after it: a transaction of 0 and an endpoint. */
    status = console_bootstrap_endpoint_encode(0, values, 1, transfers,
                                               buffer.bytes, &size);
    check(status == OK && size == 4 && same(buffer.bytes, want, 4) &&
              transfers[0].rights == (CHANNEL_RIGHT_READ | RIGHT_WAIT),
          "an endpoint message is a transaction of 0 and a handle with "
          "READ and WAIT");
    received[0].type = TYPE_CHANNEL;
    received[0].rights = CHANNEL_RIGHT_READ | RIGHT_WAIT;
    check(console_bootstrap_endpoint_check(buffer.bytes, 4, received, 1) == OK,
          "an endpoint message passes its check");
    received[0].rights = CHANNEL_RIGHT_READ;
    check(console_bootstrap_endpoint_check(buffer.bytes, 4, received, 1) ==
              ERR_INVALID_ARGS,
          "an endpoint without WAIT is refused");
    received[0].rights = CHANNEL_RIGHT_READ | RIGHT_WAIT;
    check(console_bootstrap_endpoint_check(buffer.bytes, 4, received, 0) ==
              ERR_INVALID_ARGS,
          "an endpoint message with no handle is refused");
}

int main(void)
{
    check_session_messages();
    check_session_refusals();
    check_control_messages();
    check_bootstrap_messages();
    printf("%u failure%s\n", failures, failures == 1 ? "" : "s");
    return failures != 0;
}
