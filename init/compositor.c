/* init's part in the compositor (compositor.h). Each start of the
   compositor, the first and every restart, gets a control channel of its
   own; the end init keeps is the one the options, the background and the
   display go to, in that order and with no wait between them
   (docs/spec/display.md D19's phase 3 arrangement, rule 2), and then the
   console's connection, if a console runs, which a compositor started
   again gets as the first did (rule 9). The kernel's background goes
   with the first start alone: a compositor started again computes Limb
   (D2, rule 2). The answer to a connection is the one message init waits
   for, and it waits until a deadline: a compositor that has not answered
   by then is ended, and its supervisor reads that end as any other
   (D3). */

#include "compositor.h"
#include "console.h"
#include "display_protocol.h"
#include "platform.h"
#include "specimen_start.h"
#include "tests.h"

/* The bytes of each message sent to the compositor, aligned to 8 bytes as
   the generated functions require (docs/spec/interface.md P14), and as
   long as the control channel's longest message, which holds the
   bootstrap channel's too. */
static union {
    UINT64 alignment;
    UINT8 bytes[DISPLAY_CONTROL_TO_SERVER_BYTES_MAX];
} outgoing;
_Static_assert(DISPLAY_BOOTSTRAP_TO_CLIENT_BYTES_MAX <=
                   DISPLAY_CONTROL_TO_SERVER_BYTES_MAX,
               "the bootstrap message fits the outgoing buffer");

/* channel_write needs a valid array even for no handles. */
static const HANDLE_TRANSFER no_handles[1];

/* A request that has a reply carries a transaction other than 0
   (docs/spec/interface.md P7, rule 4). Init waits for no answer to the
   background, so it needs no particular one; a connection's, which it
   waits for, is told from other messages by its own, which rises. */
#define BACKGROUND_TRANSACTION 1
#define FIRST_CONNECT_TRANSACTION 2

/* COMPOSITOR_CONNECT_DEADLINE_SECONDS in the nanoseconds of a deadline. */
#define NS_PER_SECOND 1000000000ULL
#define CONNECT_DEADLINE_NS \
    (COMPOSITOR_CONNECT_DEADLINE_SECONDS * NS_PER_SECOND)

/* What the compositor sends init: its answers and its events, none of
   which carries a handle. */
static union {
    UINT64 alignment;
    UINT8 bytes[DISPLAY_CONTROL_TO_CLIENT_BYTES_MAX];
} incoming;

static UINT32 control = HANDLE_INVALID;
static BOOT_BACKGROUND held;
static UINT32 connect_transaction = FIRST_CONNECT_TRANSACTION;

int compositor_wanted(void)
{
    return !boot_file(SPECIMEN_MARKER);
}

void compositor_hold(BOOT_BACKGROUND *background)
{
    UINT32 i;

    held.label = background->label;
    held.count = background->count;
    for (i = 0; i < background->count; i++)
        held.sections[i] = background->sections[i];
    background->count = 0;
}

void compositor_given_up(void)
{
    boot_background_close(&held);
    if (control != HANDLE_INVALID) {
        handle_close(control);
        control = HANDLE_INVALID;
    }
}

void compositor_start(const PLATFORM *platform, UINT32 bootstrap)
{
    if (!display_released(bootstrap)) {
        boot_background_close(&held);
        return;
    }
    console_defer_display();
    platform_start_compositor(platform);
}

/* The options the compositor starts with (display_protocol.h), from the
   flag files a test disk holds: ui.reduce-motion for NOMOTION, leaving
   after the first frame that shows a window's present for COMPFAIL,
   answering no connection for NOANSWER, and logging what its frames cost
   for BUDGETS. WINDOW sets none. */
static UINT32 options(void)
{
    UINT32 bits = 0;

    if (tests_flag(TEST_FLAG_NO_MOTION))
        bits |= COMPOSITOR_OPTION_REDUCE_MOTION;
    if (tests_flag(TEST_FLAG_COMPOSITOR_FAILS))
        bits |= COMPOSITOR_OPTION_EXIT_TEST;
    if (tests_flag(TEST_FLAG_NO_ANSWER))
        bits |= COMPOSITOR_OPTION_NO_ANSWER_TEST;
    if (tests_flag(TEST_FLAG_BUDGETS))
        bits |= COMPOSITOR_OPTION_BUDGETS;
    return bits;
}

/* Write the message in outgoing, size bytes and the count handles in
   transfers, on the control channel. A compositor that has already
   ended has closed it, which its supervisor logs. Returns whether the
   write took the message. */
static int write_control(const char *what, UINT32 size,
                         const HANDLE_TRANSFER *transfers, UINT32 count)
{
    UINT32 status = channel_write(control, outgoing.bytes, size, transfers,
                                  count);

    if (status != OK && status != ERR_PEER_CLOSED)
        say_status(what, status);
    return status == OK;
}

/* The compositor's options: the first message on its control channel. */
static void send_options(void)
{
    COMPOSITOR_SETUP_OPTIONS_MESSAGE message;
    UINT32 size;
    UINT32 status;

    message.setup.transaction = 0;
    message.setup.kind = 0;
    message.options = options();
    status = display_control_options_encode(&message, outgoing.bytes, &size);
    if (status == OK)
        write_control("compositor options send failed", size, no_handles, 0);
    else
        say_status("compositor options send failed", status);
}

/* The kernel's background, if init still holds it, with the display it is
   to be given. Its sections leave init whether or not they are sent. */
static void send_background(const PLATFORM *platform)
{
    COMPOSITOR_SETUP_BACKGROUND_MESSAGE message;
    HANDLE_TRANSFER sections[BOOTSTRAP_BACKGROUND_SECTIONS_MAX];
    UINT32 size, status;

    if (!held.count)
        return;
    message.setup.transaction = BACKGROUND_TRANSACTION;
    message.setup.kind = 0;
    display_for_background(platform, &message.display);
    message.background = held.label;
    status = display_control_background_encode(&message, held.sections,
                                               held.count, sections,
                                               outgoing.bytes, &size);
    if (status == OK && write_control("compositor background send failed",
                                      size, sections, held.count)) {
        held.count = 0;
        return;
    }
    if (status != OK)
        say_status("compositor background send failed", status);
    boot_background_close(&held);
}

/* The display, with a resource covering exactly the framebuffer's pages. */
static void send_display(const PLATFORM *platform)
{
    COMPOSITOR_SETUP_DISPLAY_MESSAGE message;
    HANDLE_TRANSFER framebuffer;
    UINT32 status = display_resource(platform, &framebuffer);
    UINT32 resource = framebuffer.handle, size;

    message.setup.transaction = 0;
    message.setup.kind = 0;
    message.display = platform->boot->display;
    if (status == OK)
        status = display_control_display_encode(&message, &resource, 1,
                                                &framebuffer, outgoing.bytes,
                                                &size);
    if (status == OK &&
        write_control("display send failed", size, &framebuffer, 1))
        return;
    if (status != OK)
        say_status("display send failed", status);
    if (framebuffer.handle != HANDLE_INVALID)
        handle_close(framebuffer.handle);
}

/* Read the compositor's messages until its answer to the connection
   issued as transaction, whose status and connection number are
   *answer's, or until CONNECT_DEADLINE_NS after the call. The answers
   and events that came before, a background's prepared and a
   connection's closed, are not waited for and are passed over; the
   deadline is one time on the clock, so none of them moves it, and it is
   checked after each, so a compositor that keeps the channel full of
   them is not waited for beyond it either. A wait that ends for any
   reason but the deadline goes back to the read, which says what the
   channel holds. Returns OK; ERR_TIMED_OUT for a compositor that has not
   answered by the deadline; ERR_PEER_CLOSED for one that has closed its
   end; or the status of the read or the clock that failed. */
static UINT32 await_connected(UINT32 transaction,
                              COMPOSITOR_CONNECTED_REPLY *answer)
{
    const COMPOSITOR_CONNECTED_REPLY *reply =
        (const COMPOSITOR_CONNECTED_REPLY *)incoming.bytes;
    RECEIVED_HANDLE handles[CHANNEL_MAX_HANDLES];
    UINT32 byte_count, handle_count, observed, i;
    UINT64 now, deadline;
    UINT32 status = clock_monotonic(&now);
    int found;

    if (status != OK)
        return status;
    deadline = now + CONNECT_DEADLINE_NS;
    for (;;) {
        status = channel_read(control, incoming.bytes, sizeof incoming.bytes,
                              handles, CHANNEL_MAX_HANDLES, &byte_count,
                              &handle_count);
        if (status == ERR_SHOULD_WAIT) {
            if (object_wait_one(control,
                                CHANNEL_READABLE | CHANNEL_PEER_CLOSED,
                                deadline, &observed) == ERR_TIMED_OUT)
                return ERR_TIMED_OUT;
            continue;
        }
        if (status != OK)
            return status;
        found = display_control_connected_check(incoming.bytes, byte_count,
                                                handles, handle_count) == OK &&
                reply->transaction == transaction;
        for (i = 0; i < handle_count; i++)
            handle_close(handles[i].handle);
        if (found) {
            answer->transaction = reply->transaction;
            answer->status = reply->status;
            answer->connection = reply->connection;
            return OK;
        }
        status = clock_monotonic(&now);
        if (status != OK)
            return status;
        if (now >= deadline)
            return ERR_TIMED_OUT;
    }
}

/* The compositor has not answered a connection by its deadline, which
   counts as its end (display.md D3; D19's phase 3 arrangement, rule 2,
   step 4): log it and end the compositor. The supervisor then reads the
   end as any other of the compositor's, and starts it again or gives it
   up as the restart policy allows (platform.c). */
static void end_unanswering(void)
{
    UINT32 status;

    say_status("connection wait failed", ERR_TIMED_OUT);
    status = platform_end_compositor();
    if (status != OK)
        say_status("compositor end failed", status);
}

UINT32 compositor_connect(const char *label, UINT32 layers, UINT32 rights,
                          UINT32 *client)
{
    COMPOSITOR_CONNECT_REQUEST request;
    COMPOSITOR_CONNECTED_REPLY answer;
    HANDLE_TRANSFER transfer;
    UINT32 mine = HANDLE_INVALID, theirs = HANDLE_INVALID, size, status;
    UINT32 transaction = connect_transaction;
    UINT32 length = 0;

    *client = HANDLE_INVALID;
    if (control == HANDLE_INVALID)
        return ERR_BAD_STATE;
    while (label[length])
        length++;
    request.setup.transaction = transaction;
    request.setup.kind = 0;
    request.mark = 0;
    request.layers = layers;
    request.rights = rights;
    request.label_length = length;
    status = channel_create(CONNECTION_CAPACITY, &mine, &theirs);
    if (status == OK)
        status = display_control_connect_encode(&request, (const UINT8 *)label,
                                                length, &mine, 1, &transfer,
                                                outgoing.bytes, &size);
    if (status == OK)
        status = channel_write(control, outgoing.bytes, size, &transfer, 1);
    if (status != OK) {
        if (mine != HANDLE_INVALID)
            handle_close(mine);
        if (theirs != HANDLE_INVALID)
            handle_close(theirs);
        if (status != ERR_PEER_CLOSED)
            say_status("connection send failed", status);
        return status;
    }
    connect_transaction++;
    status = await_connected(transaction, &answer);
    if (status == ERR_TIMED_OUT)
        end_unanswering();
    if (status == OK && answer.status != OK) {
        say_status("connection refused", answer.status);
        status = answer.status;
    }
    if (status != OK) {
        handle_close(theirs);
        return status;
    }
    *client = theirs;
    return OK;
}

void compositor_started(const PLATFORM *platform)
{
    send_background(platform);
    send_display(platform);
    console_connect();
}

UINT32 compositor_bootstrap(const PLATFORM *platform, UINT32 process,
                            UINT32 channel)
{
    HANDLE_TRANSFER transfers[COMPOSITOR_BOOTSTRAP_HANDLES];
    UINT32 handles[COMPOSITOR_BOOTSTRAP_HANDLES];
    UINT32 mine = HANDLE_INVALID, size, i;
    UINT32 status;

    for (i = 0; i < COMPOSITOR_BOOTSTRAP_HANDLES; i++)
        handles[i] = HANDLE_INVALID;
    status = resource_create(platform->debug_log, SYSTEM_PRIVILEGE_DEBUG_LOG,
                             1, &handles[COMPOSITOR_BOOTSTRAP_DEBUG_LOG]);
    if (status == OK)
        status = handle_duplicate(process, PROCESS_RIGHT_MAP | RIGHT_TRANSFER,
                                  &handles[COMPOSITOR_BOOTSTRAP_SELF]);
    if (status == OK)
        status = channel_create(COMPOSITOR_CONTROL_CAPACITY, &mine,
                                &handles[COMPOSITOR_BOOTSTRAP_CONTROL]);
    if (status == OK)
        status = display_bootstrap_start_encode(
            0, handles, COMPOSITOR_BOOTSTRAP_HANDLES, transfers,
            outgoing.bytes, &size);
    if (status == OK)
        status = channel_write(channel, outgoing.bytes, size, transfers,
                               COMPOSITOR_BOOTSTRAP_HANDLES);
    if (status != OK) {
        for (i = 0; i < COMPOSITOR_BOOTSTRAP_HANDLES; i++)
            if (handles[i] != HANDLE_INVALID)
                handle_close(handles[i]);
        if (mine != HANDLE_INVALID)
            handle_close(mine);
        return status;
    }
    if (control != HANDLE_INVALID)
        handle_close(control);
    control = mine;
    send_options();
    return OK;
}
