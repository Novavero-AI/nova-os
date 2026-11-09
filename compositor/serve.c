/* The compositor's service of a connection and its window (compositor.h;
   docs/spec/display.md D5 to D7 and D19's phase 3 arrangement, rules 2
   to 5, 8 and 9). The first process issues a connection on the control
   channel, and the compositor serves one connection and one window on
   it, with the rules of lib/window.c, what waits for the window's
   channel in lib/backlog.c and the composition of lib/compose.c, which
   this file only calls.

   The input drivers' endpoints arrive on the bootstrap channel, are read
   with lib/endpoint.c and logged as the console logs them (D19, rule 8),
   and each key event goes through lib/keyroute.c: the secure attention
   key is logged and sent to no window, and every other key is queued for
   the window with the focus (D11, rule 1, D12 and D19's rule 3). Each
   pointer report goes through lib/route.c, which keeps the one pointer
   every pointing device moves and says what the window is sent, and what
   it says is queued for the window with the keys (D10, D11 rule 3 and
   D19's rule 6); where the pointer is, once it has a place, is drawn over
   every frame (D19, rule 7, lib/compose.c).

   Every wait is one object_wait_many on the channels and the time of the
   next frame or the channel's stall, so the compositor never waits for a
   client. Each channel's queue is read for at most its capacity in
   messages a time, so a client that fills one cannot keep the others
   waiting. A close of a window's or a connection's channel is read before
   the control channel is, since the first process issues a connection
   again only after the one before it is gone. A failed call closes the
   window or the connection it was for, with the status logged and a
   reason that is one of rule 8's. */

#include "compositor.h"
#include "backlog.h"
#include "display_protocol.h"
#include "endpoint.h"
#include "font.h"
#include "input_protocol.h"
#include "keyroute.h"
#include "log.h"
#include "native.h"
#include "outline.h"
#include "route.h"
#include "timing.h"
#include "window.h"

/* The signals every channel is waited on for. */
#define WAKE_SIGNALS (CHANNEL_READABLE | CHANNEL_PEER_CLOSED)

/* The channels waited on at once: the control channel, the bootstrap
   channel, a connection, a window and the input endpoints. */
#define WAITED_MAX (4 + INPUT_DEVICES_MAX)
_Static_assert(WAITED_MAX <= WAIT_MANY_MAX, "one wait holds every channel");

/* What the compositor's lines of the log begin with. */
#define LOG_COMPONENT "nova-compositor"

/* The most bytes of any message the compositor sends. */
#define OUTGOING_BYTES_MAX DISPLAY_WINDOW_TO_CLIENT_BYTES_MAX
_Static_assert(DISPLAY_CONNECTION_TO_CLIENT_BYTES_MAX <= OUTGOING_BYTES_MAX,
               "a connection's messages fit the outgoing buffer");
_Static_assert(DISPLAY_CONTROL_TO_CLIENT_BYTES_MAX <= OUTGOING_BYTES_MAX,
               "a control message fits the outgoing buffer");

/* The reasons a window or a connection is closed, as rule 8 gives them. */
#define REASON_MALFORMED "malformed"
#define REASON_BUFFER "buffer"
#define REASON_PRESENT "present"
#define REASON_STALLED "stalled"

/* channel_write needs a valid array even for no handles. */
static const HANDLE_TRANSFER no_handles[1];

/* A message read from any channel, whole: the most a channel carries,
   so that no read finds one too large for it and leaves it queued.
   Aligned to 8 bytes as the generated checks require
   (docs/spec/interface.md P14). */
static union {
    UINT64 alignment;
    UINT8 bytes[CHANNEL_MAX_BYTES];
} incoming;

/* A message to send, in the same alignment. */
static union {
    UINT64 alignment;
    UINT8 bytes[OUTGOING_BYTES_MAX];
} outgoing;

static const SERVE *host;

/* The connection served: its channel, HANDLE_INVALID while there is none,
   its number among those issued, its layers and rights, its label, and
   the windows it has made. */
typedef struct {
    UINT32 channel;
    UINT32 number;
    UINT32 layers;
    UINT32 rights;
    UINT32 label_length;
    UINT8 label[DISPLAY_LABEL_MAX];
    UINT32 windows;
} CONNECTION;

/* The window served: its channel, HANDLE_INVALID while there is none, its
   rules' record, what waits for the channel, and for each buffer given
   where it is mapped and how many bytes. frame_owed is nonzero from a
   present that is shown to the frame that shows it. */
typedef struct {
    UINT32 channel;
    WINDOW_RECORD record;
    BACKLOG backlog;
    UINT32 waiting;
    const UINT32 *pixels[WINDOW_BUFFERS_MAX];
    UINT64 mapped[WINDOW_BUFFERS_MAX];
    UINT32 frame_owed;
} WINDOW;

/* An input driver's endpoint, HANDLE_INVALID for a free slot: the
   device its description gave, whose kind is 0 until it has come, and
   whether a key event has come on it. */
typedef struct {
    UINT32 endpoint;
    INPUT_DEVICE device;
    UINT32 keys;
} INPUT;

static CONNECTION connection;
static WINDOW window;
static UINT32 issued;

/* What the keyboards have done, as one keyboard, the one pointer every
   pointing device moves, and the pointer line logged last, for all the
   pointers. */
static INPUT inputs[INPUT_DEVICES_MAX];
static KEYROUTE keyboards;
static ROUTE route;
static ENDPOINT_NOTES pointer_notes;

/* The area windows are placed in and the corners every window of this
   screen has: the radius of the panel's, rounded up to whole pixels
   being the reach. The material is the PANEL surface's, the one surface
   window_open serves. outline_work is the memory the corners are made
   in, which outline.h says is large. */
static WINDOW_AREA area;
static UINT32 scale;
static COMPOSE_CORNERS corners;
static COMPOSE_MATERIAL material;
static OUTLINE outline_work;

/* The pointer's image: the theme's arrow, its default (look.md V15), and
   the cursor frames draw it by, which has a place once both of the
   pointer's axes have been set, by the reports of one device or of
   several (D10, rule 4). */
static const INT32 pointer_body_points[] = THEME_POINTER_BODY_DEFAULT_POINTS;
static const UINT8 pointer_body_flags[] = THEME_POINTER_BODY_DEFAULT_FLAGS;
static const UINT32 pointer_body_ends[] = THEME_POINTER_BODY_DEFAULT_ENDS;
static const INT32 pointer_edge_points[] = THEME_POINTER_EDGE_DEFAULT_POINTS;
static const UINT8 pointer_edge_flags[] = THEME_POINTER_EDGE_DEFAULT_FLAGS;
static const UINT32 pointer_edge_ends[] = THEME_POINTER_EDGE_DEFAULT_ENDS;
static const COMPOSE_POINTER pointer_image = {
    {pointer_body_points, pointer_body_flags, pointer_body_ends,
     THEME_POINTER_BODY_DEFAULT_POINT_COUNT,
     THEME_POINTER_BODY_DEFAULT_CONTOURS},
    {pointer_edge_points, pointer_edge_flags, pointer_edge_ends,
     THEME_POINTER_EDGE_DEFAULT_POINT_COUNT,
     THEME_POINTER_EDGE_DEFAULT_CONTOURS},
    THEME_POINTER_BODY_INK_DEFAULT, THEME_POINTER_EDGE_INK_DEFAULT,
    THEME_POINTER_EDGE_OPACITY_DEFAULT};
static COMPOSE_CURSOR cursor;

/* What is to be composed at the next frame, and when that may be. */
static RECT damage;
static UINT64 next_frame;

static void zero(void *memory, UINT64 count)
{
    UINT8 *bytes = memory;
    UINT64 i;

    for (i = 0; i < count; i++)
        bytes[i] = 0;
}

static UINT64 earlier(UINT64 a, UINT64 b) { return a < b ? a : b; }

/* ------------------------------------------------------------------------
   The log. */

typedef struct {
    UINT32 bit;
    const char *name;
} FLAG_NAME;

static const FLAG_NAME layer_names[] = {
    {DISPLAY_LAYER_APP, "app"},
    {DISPLAY_LAYER_NOTIFICATION, "notification"},
    {DISPLAY_LAYER_OVERLAY, "overlay"},
    {DISPLAY_LAYER_TRUSTED, "trusted"},
    {DISPLAY_LAYER_SYSTEM, "system"},
};

static const FLAG_NAME right_names[] = {
    {DISPLAY_RIGHT_MINT, "mint"},
    {DISPLAY_RIGHT_ATTENTION, "attention"},
    {DISPLAY_RIGHT_CAPTURE, "capture"},
    {DISPLAY_RIGHT_ACTIVATE, "activate"},
};

/* The names of the bits of mask, comma-joined, or none. */
static void say_flags(UINT32 mask, const FLAG_NAME *names, UINT32 count)
{
    UINT32 i, written = 0;

    for (i = 0; i < count; i++) {
        if (!(mask & names[i].bit))
            continue;
        if (written++)
            say(",");
        say(names[i].name);
    }
    if (!written)
        say("none");
}

static void say_label(void)
{
    say_value_bytes(connection.label, connection.label_length);
}

/* ------------------------------------------------------------------------
   Sending. */

/* Write the message in outgoing, size bytes, on channel. */
static UINT32 send_outgoing(UINT32 channel, UINT32 size)
{
    return channel_write(channel, outgoing.bytes, size, no_handles, 0);
}

/* A message a backlog gives the window's channel (backlog.h): what the
   channel took, refused as full or found closed. A message that its
   encoder refuses was made by the compositor, which has failed, and not
   by the window: the compositor ends. */
static UINT32 send_to_window(void *context, const BACKLOG_MESSAGE *message)
{
    UINT32 size = 0, status = backlog_encode(message, outgoing.bytes, &size);

    (void)context;
    if (status != OK) {
        compositor_say_status("window message encode failed", status);
        process_exit(COMPOSITOR_EXIT_FAILED);
    }
    status = send_outgoing(window.channel, size);
    if (status == OK)
        return BACKLOG_SEND_TAKEN;
    return status == ERR_PEER_CLOSED ? BACKLOG_SEND_CLOSED
                                     : BACKLOG_SEND_FULL;
}

/* The first process is told of what happened to the connection issued
   from this channel, as the closed event gives: the number its answer
   gave. */
static void tell_closed(UINT32 number)
{
    COMPOSITOR_CLOSED_EVENT event;
    UINT32 size, status;

    event.setup.transaction = 0;
    event.setup.kind = 0;
    event.connection = number;
    status = display_control_closed_encode(&event, outgoing.bytes, &size);
    if (status == OK)
        status = send_outgoing(host->control, size);
    if (status != OK && status != ERR_PEER_CLOSED)
        compositor_say_status("closed event send failed", status);
}

/* ------------------------------------------------------------------------
   The pointer. */

/* The window the pointer may be over, as lib/route.c takes it: 0 where
   there is none. */
static const WINDOW_RECORD *route_window(void)
{
    return window.channel != HANDLE_INVALID ? &window.record : 0;
}

/* Queue what lib/route.c decided the window is sent. It decides nothing
   where there is no window, so no event waits for one. */
static void send_routed(const ROUTED *routed)
{
    UINT32 i;

    for (i = 0; i < routed->count; i++)
        backlog_push(&window.backlog, &routed->event[i]);
}

/* Move the pointer's image to the pixel the pointer's position rounds
   down to, once the pointer has a place, and damage what that changes
   (D19, rule 7). */
static void move_cursor(void)
{
    RECT boxes[2];
    UINT32 count, i;

    if (!route_placed(&route))
        return;
    count = compose_cursor_move(&cursor, host->screen,
                                route.x / DISPLAY_POSITION_ONE,
                                route.y / DISPLAY_POSITION_ONE, boxes);
    for (i = 0; i < count; i++)
        window_damage_join(&damage, &boxes[i]);
}

/* A pointer report of input's endpoint: what the window is sent, and the
   pointer's image moved (D19, rule 6). Each endpoint is a source of
   buttons by its place, which a later endpoint takes over only after the
   close that lets go its buttons (close_input). */
static void take_report(const INPUT *input, const INPUT_POINTER *report)
{
    ROUTED routed;

    route_report(&route, (UINT32)(input - inputs), &input->device,
                 (UINT32)host->screen->width, (UINT32)host->screen->height,
                 route_window(), report, &routed);
    send_routed(&routed);
    move_cursor();
}

/* The pointing device of input's endpoint has gone: the window is sent the
   releases of the buttons it held (D11, rule 3). */
static void pointer_gone(const INPUT *input)
{
    ROUTED routed;

    route_source_gone(&route, (UINT32)(input - inputs), route_window(),
                      timing_now(), &routed);
    send_routed(&routed);
}

/* A present of the window is shown for the first time under a pointer that
   has not moved since: the window has the pointer if the pointer is over
   it (D11, rule 3). */
static void settle_pointer(void)
{
    ROUTED routed;

    route_settle(&route, route_window(), timing_now(), &routed);
    send_routed(&routed);
}

/* ------------------------------------------------------------------------
   The window. */

/* The window's rectangle on the screen, widened to the blocks it writes. */
static RECT window_rectangle(void)
{
    RECT rect;

    rect.left = window.record.x;
    rect.top = window.record.y;
    rect.right = (INT64)window.record.x + window.record.width;
    rect.bottom = (INT64)window.record.y + window.record.height;
    return compose_blocks(host->screen, &rect);
}

static void unmap_buffer(UINT32 number)
{
    if (window.pixels[number]) {
        mapping_unmap(host->mapping, window.pixels[number],
                      window.mapped[number]);
        window.pixels[number] = 0;
        window.mapped[number] = 0;
    }
}

/* Close the window: its channel, and its buffers' mappings, the record of
   the keys and buttons it was sent, and the pointer's entry and button
   hold. What it showed is composed over at the next frame. A window closed
   for reason, one of rule 8's, is logged; one whose holder closed it, with
   reason 0, is not. */
static void close_window(const char *reason)
{
    UINT32 i;

    if (window.channel == HANDLE_INVALID)
        return;
    if (reason) {
        say("nova-compositor: window closed label=");
        say_label();
        say(" reason=");
        say(reason);
        say("\r\n");
    }
    if (window.record.shown != WINDOW_NO_BUFFER) {
        RECT rect = window_rectangle();

        window_damage_join(&damage, &rect);
    }
    handle_close(window.channel);
    for (i = 0; i < WINDOW_BUFFERS_MAX; i++)
        unmap_buffer(i);
    zero(&window, sizeof window);
    window.channel = HANDLE_INVALID;
    keyroute_window_gone(&keyboards);
    route_window_closed(&route);
}

/* Close the connection and, with it, its window, which are logged as
   rule 8 gives, and tell the first process. */
static void close_connection(const char *reason)
{
    UINT32 number = connection.number;

    if (connection.channel == HANDLE_INVALID)
        return;
    if (reason) {
        say("nova-compositor: connection closed number=");
        say_number(number);
        say(" label=");
        say_label();
        say(" reason=");
        say(reason);
        say("\r\n");
    }
    close_window(reason);
    handle_close(connection.channel);
    connection.channel = HANDLE_INVALID;
    tell_closed(number);
}

/* Show the window its configure, serial and all (D7, rule 4). */
static void send_configure(void)
{
    WINDOW_CONFIGURE_EVENT configure;

    configure.header.transaction = 0;
    configure.header.kind = 0;
    configure.serial = window_configure(&window.record);
    configure.width = window.record.width;
    configure.height = window.record.height;
    configure.scale = scale;
    configure.state = window_state(window.record.layer);
    backlog_configure(&window.backlog, &configure);
}

/* A buffer given to the window, with its sections in handles, which are
   closed whatever the result. Returns the reason the window is to be
   closed for, or 0. The buffer is mapped only once the rules accept it,
   by the sizes its sections hold. */
static const char *take_buffer(const RECEIVED_HANDLE *handles, UINT32 count)
{
    const WINDOW_BUFFER_REQUEST *request =
        (const WINDOW_BUFFER_REQUEST *)incoming.bytes;
    UINT64 sizes[CHANNEL_MAX_HANDLES];
    UINT32 number = request->buffer, status = OK, i;
    const char *reason = REASON_BUFFER;

    for (i = 0; i < count && status == OK; i++)
        sizes[i] = mapping_section_bytes(host->mapping, handles[i].handle,
                                         SECTION_MAX_SIZE, &status);
    if (status != OK) {
        compositor_say_status("buffer size read failed", status);
    } else if (window_give_buffer(&window.record, request, sizes, count) ==
               OK) {
        UINT64 bytes = window_buffer_size(request->stride, request->height);
        UINT64 mapped = 0;
        const void *pixels = mapping_image(host->mapping, handles, count,
                                           bytes, &mapped, &status);

        if (pixels) {
            unmap_buffer(number);
            window.pixels[number] = pixels;
            window.mapped[number] = mapped;
            reason = 0;
        } else {
            compositor_say_status("buffer map failed", status);
        }
    }
    compositor_close_received(handles, count);
    return reason;
}

/* A frame presented on the window. Returns the reason the window is to
   be closed for, or 0. */
static const char *take_present(void)
{
    const WINDOW_PRESENT_REQUEST *request =
        (const WINDOW_PRESENT_REQUEST *)incoming.bytes;
    WINDOW_PRESENTED presented;
    UINT32 hidden = window.record.shown == WINDOW_NO_BUFFER;
    UINT32 i;

    if (window_present(&window.record, host->screen, request,
                       display_window_present_damage(incoming.bytes),
                       &presented) != OK)
        return REASON_PRESENT;
    backlog_release(&window.backlog, presented.released);
    if (presented.shown) {
        window.frame_owed = 1;
        for (i = 0; i < presented.count; i++)
            window_damage_join(&damage, &presented.damage[i]);
        if (hidden)
            settle_pointer();
    }
    return 0;
}

/* A message the window's channel carried, bytes long with count handles
   in handles: a malformed one, or one the rules refuse, closes the
   window. */
static void take_window_message(UINT32 bytes, const RECEIVED_HANDLE *handles,
                                UINT32 count)
{
    UINT32 which = 0;
    UINT32 status = display_window_request_check(incoming.bytes, bytes,
                                                 handles, count, &which);
    const char *reason = REASON_MALFORMED;

    if (status == OK && which == WINDOW_BUFFER)
        reason = take_buffer(handles, count);
    else if (status == OK && which == WINDOW_PRESENT)
        reason = take_present();
    else
        compositor_close_received(handles, count);
    if (reason)
        close_window(reason);
}

/* Read the window's channel for at most its capacity in messages. */
static void serve_window(void)
{
    RECEIVED_HANDLE handles[CHANNEL_MAX_HANDLES];
    UINT32 n;

    for (n = 0; n < WINDOW_CAPACITY && window.channel != HANDLE_INVALID; n++) {
        UINT32 bytes, count = 0;
        UINT32 status = channel_read(window.channel, incoming.bytes,
                                     sizeof incoming.bytes, handles,
                                     CHANNEL_MAX_HANDLES, &bytes, &count);

        if (status == OK) {
            take_window_message(bytes, handles, count);
        } else if (status == ERR_PEER_CLOSED) {
            close_window(0);
        } else if (status == ERR_SHOULD_WAIT) {
            return;
        } else {
            compositor_say_status("window read failed", status);
            close_window(REASON_MALFORMED);
        }
    }
}

/* ------------------------------------------------------------------------
   The connection. */

static void reply_window_made(UINT32 transaction, UINT32 status,
                              UINT32 number)
{
    DISPLAY_WINDOW_REPLY reply;
    UINT32 size;

    reply.transaction = transaction;
    reply.status = status;
    reply.window = number;
    if (display_connection_window_made_encode(&reply, outgoing.bytes,
                                              &size) == OK)
        status = send_outgoing(connection.channel, size);
    else
        status = ERR_INVALID_ARGS;
    if (status != OK && status != ERR_PEER_CLOSED)
        close_connection(REASON_STALLED);
}

/* A window asked for on the connection, handle being the compositor's
   end of its channel, which is the window's or is closed. Answers it,
   and sends the first configure (rule 5). */
static void open_window(const DISPLAY_WINDOW_REQUEST *request, UINT32 handle)
{
    UINT32 reach = (UINT32)corners.reach;
    UINT32 status = window_open(&area, reach, connection.layers,
                                window.channel != HANDLE_INVALID,
                                request, &window.record);

    if (status != OK) {
        handle_close(handle);
        reply_window_made(request->header.transaction, status, 0);
        return;
    }
    window.channel = handle;
    connection.windows++;
    reply_window_made(request->header.transaction, OK, connection.windows);
    if (window.channel != HANDLE_INVALID)
        send_configure();
}

/* Read the connection's channel for at most its capacity in messages. */
static void serve_connection(void)
{
    RECEIVED_HANDLE handles[CHANNEL_MAX_HANDLES];
    UINT32 n;

    for (n = 0;
         n < CONNECTION_CAPACITY && connection.channel != HANDLE_INVALID;
         n++) {
        UINT32 bytes, count = 0, which = 0;
        UINT32 status = channel_read(connection.channel, incoming.bytes,
                                     sizeof incoming.bytes, handles,
                                     CHANNEL_MAX_HANDLES, &bytes, &count);

        if (status == ERR_SHOULD_WAIT)
            return;
        if (status == ERR_PEER_CLOSED) {
            close_connection(0);
            return;
        }
        if (status != OK) {
            compositor_say_status("connection read failed", status);
            close_connection(REASON_MALFORMED);
            return;
        }
        status = display_connection_request_check(incoming.bytes, bytes,
                                                  handles, count, &which);
        if (status == OK && which == DISPLAY_WINDOW) {
            open_window((const DISPLAY_WINDOW_REQUEST *)incoming.bytes,
                        handles[0].handle);
        } else {
            compositor_close_received(handles, count);
            close_connection(REASON_MALFORMED);
        }
    }
}

/* ------------------------------------------------------------------------
   The control channel. */

/* Send the connection's first message, the screen (D6). */
static UINT32 send_screen(UINT32 channel)
{
    DISPLAY_SCREEN_EVENT event;
    UINT32 size;
    UINT32 status;

    event.header.transaction = 0;
    event.header.kind = 0;
    event.width = host->screen->width;
    event.height = host->screen->height;
    event.scale = scale;
    status = display_connection_screen_encode(&event, outgoing.bytes, &size);
    return status == OK ? send_outgoing(channel, size) : status;
}

/* Answer the connection issued as transaction. */
static void reply_connected(UINT32 transaction, UINT32 status, UINT32 number)
{
    COMPOSITOR_CONNECTED_REPLY reply;
    UINT32 size;

    reply.transaction = transaction;
    reply.status = status;
    reply.connection = number;
    status = display_control_connected_encode(&reply, outgoing.bytes, &size);
    if (status == OK)
        status = send_outgoing(host->control, size);
    if (status != OK && status != ERR_PEER_CLOSED)
        compositor_say_status("connected reply send failed", status);
}

/* A connection the first process issues, whose compositor end is
   handle: it is the connection served, or closed with the status
   answered. The first message on it is the screen. */
static UINT32 begin_connection(const COMPOSITOR_CONNECT_REQUEST *request,
                               UINT32 handle)
{
    const UINT8 *label = display_control_connect_label(incoming.bytes);
    UINT32 status = send_screen(handle), i;

    if (status != OK)
        return status;
    connection.channel = handle;
    connection.number = ++issued;
    connection.layers = request->layers;
    connection.rights = request->rights;
    connection.label_length = request->label_length;
    for (i = 0; i < request->label_length; i++)
        connection.label[i] = label[i];
    connection.windows = 0;
    say("nova-compositor: connection issued number=");
    say_number(connection.number);
    say(" label=");
    say_label();
    say(request->mark ? " mark=yes" : " mark=no");
    say(" layers=");
    say_flags(request->layers, layer_names,
              sizeof layer_names / sizeof layer_names[0]);
    say(" rights=");
    say_flags(request->rights, right_names,
              sizeof right_names / sizeof right_names[0]);
    say("\r\n");
    return OK;
}

static void issue_connection(const COMPOSITOR_CONNECT_REQUEST *request,
                             UINT32 handle)
{
    UINT32 status = ERR_NO_RESOURCES;

    if (connection.channel == HANDLE_INVALID)
        status = begin_connection(request, handle);
    if (status != OK)
        handle_close(handle);
    reply_connected(request->setup.transaction, status,
                    status == OK ? connection.number : 0);
}

/* Read the control channel for at most its capacity in messages. The first
   process's close of it ends the compositor. */
static void serve_control(void)
{
    RECEIVED_HANDLE handles[CHANNEL_MAX_HANDLES];
    UINT32 n;

    for (n = 0; n < COMPOSITOR_CONTROL_CAPACITY; n++) {
        UINT32 bytes, count = 0, which = 0;
        UINT32 status = channel_read(host->control, incoming.bytes,
                                     sizeof incoming.bytes, handles,
                                     CHANNEL_MAX_HANDLES, &bytes, &count);

        if (status == ERR_SHOULD_WAIT)
            return;
        if (status == ERR_PEER_CLOSED)
            process_exit(COMPOSITOR_EXIT_DONE);
        if (status != OK) {
            compositor_say_status("control read failed", status);
            process_exit(COMPOSITOR_EXIT_FAILED);
        }
        status = display_control_request_check(incoming.bytes, bytes, handles,
                                               count, &which);
        if (status == OK && which == COMPOSITOR_CONNECT)
            issue_connection(
                (const COMPOSITOR_CONNECT_REQUEST *)incoming.bytes,
                handles[0].handle);
        else
            compositor_refuse(status, handles, count);
    }
}

/* ------------------------------------------------------------------------
   Input. */

/* Whether a window has the focus: the window served, if it is on the
   SYSTEM layer, from its being made, when its first configure says so
   (D19, rules 5 and 6), until it closes. */
static UINT32 window_has_focus(void)
{
    return window.channel != HANDLE_INVALID &&
           (window_state(window.record.layer) & WINDOW_STATE_FOCUSED) != 0;
}

/* A key event of a keyboard: the secure attention key is logged, once for
   each press, and sent to no window; any other press is queued for the
   window with the focus, or dropped where none has it; and a repeat or a
   release is queued only if its press was (lib/keyroute.c). */
static void take_key(INPUT *input, const INPUT_EVENT *event)
{
    WINDOW_INPUT routed;

    input->keys = 1;
    switch (keyroute_event(&keyboards, event, window_has_focus(), &routed)) {
    case KEYROUTE_WINDOW:
        backlog_push(&window.backlog, &routed);
        break;
    case KEYROUTE_ATTENTION:
        say(LOG_COMPONENT ": attention at_ms=");
        timing_say_ms(event->time);
        say("\r\n");
        break;
    default:
        break;
    }
}

/* Take the byte_count bytes in incoming as the next message of input's
   endpoint (input.md K3). Returns 0 if the endpoint may not send it. A
   pointer report is logged as C6 gives, and then routed. */
static int take_input(INPUT *input, UINT32 byte_count)
{
    switch (endpoint_take(&input->device, incoming.bytes, byte_count)) {
    case ENDPOINT_DESCRIPTION:
        endpoint_describe(LOG_COMPONENT, &input->device, say);
        return 1;
    case ENDPOINT_KEY:
        take_key(input, (const INPUT_EVENT *)incoming.bytes);
        return 1;
    case ENDPOINT_REPORT:
        endpoint_pointer(&pointer_notes, LOG_COMPONENT,
                         (const INPUT_POINTER *)incoming.bytes, say);
        take_report(input, (const INPUT_POINTER *)incoming.bytes);
        return 1;
    default:
        return 0;
    }
}

/* Close input's endpoint and free its place. A keyboard that sent keys
   leaves no modifier held and no press waiting for its release
   (keyroute_gone), and a pointing device no button held (pointer_gone). */
static void close_input(INPUT *input)
{
    handle_close(input->endpoint);
    if (input->keys)
        keyroute_gone(&keyboards);
    if (input->device.kind == INPUT_DEVICE_POINTER)
        pointer_gone(input);
    zero(input, sizeof *input);
    input->endpoint = HANDLE_INVALID;
}

/* Take the messages queued on input's endpoint, at most its capacity. A
   message out of place ends the endpoint, as one the driver closed does. */
static void serve_input(INPUT *input)
{
    RECEIVED_HANDLE handles[1];
    UINT32 n;

    for (n = 0; n < INPUT_ENDPOINT_CAPACITY; n++) {
        UINT32 bytes, count;
        UINT32 status = channel_read(input->endpoint, incoming.bytes,
                                     sizeof incoming.bytes, handles, 0, &bytes,
                                     &count);

        if (status == ERR_SHOULD_WAIT)
            return;
        if (status == OK && take_input(input, bytes))
            continue;
        if (status != ERR_PEER_CLOSED)
            compositor_say_status("input message refused", status);
        close_input(input);
        return;
    }
}

static void serve_inputs(void)
{
    UINT32 i;

    for (i = 0; i < INPUT_DEVICES_MAX; i++)
        if (inputs[i].endpoint != HANDLE_INVALID)
            serve_input(&inputs[i]);
}

/* A place for an input endpoint, or 0 where every place holds one. */
static INPUT *free_input(void)
{
    UINT32 i;

    for (i = 0; i < INPUT_DEVICES_MAX; i++)
        if (inputs[i].endpoint == HANDLE_INVALID)
            return &inputs[i];
    return 0;
}

/* An input endpoint from the platform driver, one a time, and only while
   a place is free, so that the endpoint a driver's restart gives waits in
   the channel until its predecessor has been read to its close and given
   up its place, however many messages that took, and none is refused.
   Its close of the channel ends the compositor (D19, rule 2) once the
   channel is read, and a message that is not an endpoint is its failure. */
static void serve_bootstrap(void)
{
    INPUT *place = free_input();
    RECEIVED_HANDLE handle;
    UINT32 bytes, count = 0, status;

    if (place == 0)
        return;
    status = channel_read(host->bootstrap, incoming.bytes,
                          sizeof incoming.bytes, &handle, 1, &bytes, &count);
    if (status == ERR_SHOULD_WAIT)
        return;
    if (status == ERR_PEER_CLOSED)
        process_exit(COMPOSITOR_EXIT_DONE);
    if (status == OK &&
        display_bootstrap_endpoint_check(incoming.bytes, bytes, &handle,
                                         count) == OK) {
        place->endpoint = handle.handle;
        return;
    }
    if (status == OK)
        compositor_close_received(&handle, count);
    compositor_say_status("bootstrap message failed", status);
    process_exit(COMPOSITOR_EXIT_FAILED);
}

/* ------------------------------------------------------------------------
   Frames. */

/* Whether a frame has something to do: damage to compose, or a frame to
   tell the window of. */
static int frame_wanted(void)
{
    return damage.left < damage.right || window.frame_owed;
}

/* Compose the damage if a frame is due, and tell the window it showed its
   present (D7, rule 3): at most once each FRAME_NS, and only when
   something is damaged or owed. */
static void frame(void)
{
    COMPOSE_WINDOW shown;
    UINT32 count = 0;
    UINT64 begun = timing_now(), time;

    if (!frame_wanted() || begun < next_frame)
        return;
    if (window.channel != HANDLE_INVALID &&
        window.record.shown != WINDOW_NO_BUFFER) {
        const WINDOW_HELD_BUFFER *buffer =
            &window.record.buffers[window.record.shown];

        shown.pixels = window.pixels[window.record.shown];
        shown.stride = buffer->stride;
        shown.columns = buffer->width;
        shown.rows = buffer->height;
        shown.place.left = window.record.x;
        shown.place.top = window.record.y;
        shown.place.right = (INT64)window.record.x + window.record.width;
        shown.place.bottom = (INT64)window.record.y + window.record.height;
        shown.material = &material;
        shown.corners = &corners;
        count = 1;
    }
    compose_frame(host->screen, host->wallpaper, &shown, count, &damage,
                  host->band, TILE_ROWS, &cursor);
    gfx_flush();
    time = timing_now();
    next_frame = begun + FRAME_NS;
    damage.left = damage.top = damage.right = damage.bottom = 0;
    if (window.frame_owed) {
        backlog_frame(&window.backlog, time);
        window.frame_owed = 0;
    }
}

/* Send the window what waits for it, and close it if its channel is gone
   or has stayed full. */
static void flush_window(void)
{
    UINT32 status;

    if (window.channel == HANDLE_INVALID)
        return;
    status = backlog_flush(&window.backlog, send_to_window, 0, timing_now());
    window.waiting = status == BACKLOG_WAITING;
    if (status == BACKLOG_STALLED)
        close_window(REASON_STALLED);
    else if (status == BACKLOG_CLOSED)
        close_window(0);
}

/* ------------------------------------------------------------------------
   The wait. */

/* Wait until a channel has something to read or has closed, the window's
   has room for what waits, or the next frame or the window's stall is
   due. Ends the compositor, FAILED, if the wait itself fails, which
   would otherwise return at once for ever. */
static void wait_for_work(void)
{
    WAIT_ENTRY items[WAITED_MAX];
    UINT32 count = 0, status, i;
    UINT64 deadline = DEADLINE_INFINITE;

    items[count].handle = host->control;
    items[count].signals = WAKE_SIGNALS;
    count++;
    if (free_input()) {
        items[count].handle = host->bootstrap;
        items[count].signals = WAKE_SIGNALS;
        count++;
    }
    for (i = 0; i < INPUT_DEVICES_MAX; i++)
        if (inputs[i].endpoint != HANDLE_INVALID) {
            items[count].handle = inputs[i].endpoint;
            items[count].signals = WAKE_SIGNALS;
            count++;
        }
    if (connection.channel != HANDLE_INVALID) {
        items[count].handle = connection.channel;
        items[count].signals = WAKE_SIGNALS;
        count++;
    }
    if (window.channel != HANDLE_INVALID) {
        UINT64 stall = backlog_deadline(&window.backlog);

        items[count].handle = window.channel;
        items[count].signals =
            WAKE_SIGNALS | (window.waiting ? CHANNEL_WRITABLE : 0);
        count++;
        deadline = earlier(deadline, stall);
    }
    if (frame_wanted())
        deadline = earlier(deadline, next_frame);
    status = object_wait_many(items, count, deadline);
    if (status != OK && status != ERR_TIMED_OUT) {
        compositor_say_status("wait failed", status);
        process_exit(COMPOSITOR_EXIT_FAILED);
    }
}

/* ------------------------------------------------------------------------
   Starting. */

/* Make what every window of this screen shares: the area, the scale, the
   PANEL surface's material and the corners of the panel's radius
   (D19's phase 3 arrangement, rule 4), and the pointer's cursor at the
   screen's scale (rule 7). Ends the compositor, FAILED, if the corners
   cannot be made, which only a radius past COMPOSE_REACH_MAX causes. The
   arrow is traced at a scale far inside the outline rasterizer's reach,
   for the screens mapping_display accepts, so that compose_cursor_move
   never refuses it (tools/check-compose.c checks the greatest scale such a
   screen gives). */
static void prepare_windows(void)
{
    INT64 radius = font_panel_radius(font_cell_width(host->screen->height));

    area.screen_width = host->screen->width;
    area.screen_height = host->screen->height;
    area.strip = 0;
    scale = window_scale(host->screen->height);
    compose_material(&material, THEME_MATERIAL_PANEL_DEFAULT_TINT,
                     THEME_MATERIAL_PANEL_DEFAULT_AMOUNT);
    if (!compose_corners(&outline_work, &corners, radius * OUTLINE_ONE)) {
        say("nova-compositor: corners failed reason=\"radius past the reach\""
            "\r\n");
        process_exit(COMPOSITOR_EXIT_FAILED);
    }
    cursor.image = &pointer_image;
    cursor.quarters = scale;
    cursor.work = &outline_work;
}

_Noreturn void serve(const SERVE *context)
{
    UINT32 i;

    host = context;
    connection.channel = HANDLE_INVALID;
    window.channel = HANDLE_INVALID;
    for (i = 0; i < INPUT_DEVICES_MAX; i++)
        inputs[i].endpoint = HANDLE_INVALID;
    prepare_windows();
    for (;;) {
        wait_for_work();
        serve_window();
        serve_connection();
        serve_control();
        serve_bootstrap();
        serve_inputs();
        frame();
        flush_window();
    }
}
