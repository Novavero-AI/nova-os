/* The console's window (pane.h). */

#include "pane.h"
#include "font.h"
#include "log.h"
#include "native.h"

#define PIXEL_BYTES 4
#define PAGE_MASK (PAGE_SIZE - 1)

/* The window is the one thing the console asks the compositor for, so its
   request has one transaction ID, which the answer echoes. */
#define WINDOW_TRANSACTION 1

/* A message read, aligned to 8 bytes as the generated checks require
   (docs/spec/interface.md P14), whole, whatever its size: the largest of
   the connection's and the window's, neither of which carries a handle. */
static union {
    UINT64 alignment;
    UINT8 bytes[DISPLAY_WINDOW_TO_CLIENT_BYTES_MAX];
} incoming;
_Static_assert(DISPLAY_CONNECTION_TO_CLIENT_BYTES_MAX <=
                   DISPLAY_WINDOW_TO_CLIENT_BYTES_MAX,
               "a connection's message fits the buffer a window's does");

/* A message to send, the connection's or the window's, as long as the
   longest. */
static union {
    UINT64 alignment;
    UINT8 bytes[DISPLAY_WINDOW_TO_SERVER_BYTES_MAX];
} outgoing;
_Static_assert(DISPLAY_CONNECTION_TO_SERVER_BYTES_MAX <=
                   DISPLAY_WINDOW_TO_SERVER_BYTES_MAX,
               "a connection's request fits the buffer a window's does");

/* channel_write needs a valid array even for no handles. */
static const HANDLE_TRANSFER no_handles[1];

static void close_if_open(UINT32 *handle)
{
    if (*handle != HANDLE_INVALID)
        handle_close(*handle);
    *handle = HANDLE_INVALID;
}

/* A step that failed, in the form log.md T1 gives a failure, as the test
   client logs its own. */
static void say_failed(const char *step, UINT32 status)
{
    say("nova-console: window failed step=");
    say_value(step);
    say(" status=");
    say_number(status);
    say("\r\n");
}

static INT64 box_width(const PANE *pane)
{
    return pane->layout.box.right - pane->layout.box.left;
}

static INT64 box_height(const PANE *pane)
{
    return pane->layout.box.bottom - pane->layout.box.top;
}

/* ------------------------------------------------------------------------
   What a pane holds. */

void pane_init(PANE *pane, MAPPING *mapping)
{
    UINT32 i;

    pane->mapping = mapping;
    pane->connection = HANDLE_INVALID;
    pane->window = HANDLE_INVALID;
    pane->phase = PANE_IDLE;
    pane->masks = 0;
    for (i = 0; i < PANE_BUFFERS; i++)
        pane->buffers[i].pixels = 0;
    pane->presented = PANEL_NO_BUFFER;
    pane->owed = 0;
    pane->whole = 0;
    pane->wheel = 0;
    pane->serial = 0;
}

void pane_drop(PANE *pane)
{
    UINT32 i;

    close_if_open(&pane->window);
    close_if_open(&pane->connection);
    for (i = 0; i < PANE_BUFFERS; i++)
        if (pane->buffers[i].pixels) {
            mapping_unmap(pane->mapping, pane->buffers[i].pixels,
                          pane->buffers[i].bytes);
            pane->buffers[i].pixels = 0;
        }
    if (pane->masks) {
        mapping_unmap(pane->mapping, pane->masks, pane->masks_size);
        pane->masks = 0;
    }
    pane->phase = PANE_IDLE;
    pane->presented = PANEL_NO_BUFFER;
    pane->owed = 0;
    pane->whole = 0;
    pane->wheel = 0;
    pane->serial = 0;
}

void pane_adopt(PANE *pane, UINT32 connection)
{
    pane_drop(pane);
    pane->connection = connection;
    pane->phase = PANE_AWAITING_SCREEN;
}

int pane_laid_out(const PANE *pane)
{
    return pane->masks != 0;
}

/* Drop what the pane holds after a step failed. */
static void fail(PANE *pane, const char *step, UINT32 status)
{
    say_failed(step, status);
    pane_drop(pane);
}

/* The next message of channel, in incoming, with its size in *bytes.
   Returns OK, ERR_SHOULD_WAIT if none is queued, or the status that ends
   the channel, ERR_PEER_CLOSED if the other end is closed. Neither
   channel carries a handle, so a message that does is refused. */
static UINT32 take(UINT32 channel, UINT32 *bytes)
{
    RECEIVED_HANDLE none[1];
    UINT32 count;

    return channel_read(channel, incoming.bytes, sizeof incoming.bytes, none,
                        0, bytes, &count);
}

/* ------------------------------------------------------------------------
   The connection: the screen, and the window asked for. */

/* The screen is known: lay the panel out on it, render the font's masks,
   and ask for a window of the panel's box's size on the SYSTEM layer, the
   PANEL surface, with a channel of its own, whose compositor end goes
   with the request (docs/spec/display.md D6). */
static void ask_for_window(PANE *pane, const DISPLAY_SCREEN_EVENT *screen)
{
    DISPLAY_WINDOW_REQUEST request;
    HANDLE_TRANSFER transfer;
    UINT32 served = HANDLE_INVALID, size, status = OK;

    pane->screen_width = screen->width;
    pane->screen_height = screen->height;
    if (!panel_layout(&pane->layout, screen->width, screen->height)) {
        say("nova-console: window refused reason=\"no room for a cell\"\r\n");
        pane_drop(pane);
        return;
    }
    pane->masks_size = font_masks_size(pane->layout.cell_width);
    pane->masks = mapping_memory(pane->mapping, pane->masks_size, &status);
    if (!pane->masks) {
        fail(pane, "panel memory", status);
        return;
    }
    font_render(pane->layout.cell_width, pane->masks);
    request.header.transaction = WINDOW_TRANSACTION;
    request.header.kind = 0;
    request.width = (UINT32)box_width(pane);
    request.height = (UINT32)box_height(pane);
    request.layer = DISPLAY_LAYER_SYSTEM;
    request.surface = DISPLAY_SURFACE_PANEL;
    status = channel_create(WINDOW_CAPACITY, &pane->window, &served);
    if (status == OK)
        status = display_connection_window_encode(&request, &served, 1,
                                                  &transfer, outgoing.bytes,
                                                  &size);
    if (status == OK)
        status = channel_write(pane->connection, outgoing.bytes, size,
                               &transfer, 1);
    if (status != OK) {
        if (served != HANDLE_INVALID)
            handle_close(served);
        fail(pane, "window request", status);
        return;
    }
    pane->phase = PANE_AWAITING_WINDOW;
}

/* The answer to the request: the window is made, or the compositor refused
   it. */
static void window_answered(PANE *pane, const DISPLAY_WINDOW_REPLY *reply)
{
    if (pane->phase != PANE_AWAITING_WINDOW ||
        reply->transaction != WINDOW_TRANSACTION)
        fail(pane, "the answer is not the window's", reply->transaction);
    else if (reply->status != OK)
        fail(pane, "window refused", reply->status);
    else
        pane->phase = PANE_AWAITING_CONFIGURE;
}

void pane_serve_connection(PANE *pane)
{
    UINT32 served;

    for (served = 0; served < CONNECTION_CAPACITY &&
                     pane->connection != HANDLE_INVALID;
         served++) {
        UINT32 bytes, which = 0, status = take(pane->connection, &bytes);

        if (status == ERR_SHOULD_WAIT)
            return;
        if (status == ERR_PEER_CLOSED) {
            pane_drop(pane);
            return;
        }
        if (status != OK) {
            fail(pane, "connection read", status);
            return;
        }
        if (display_connection_event_check(incoming.bytes, bytes, 0, 0,
                                           &which) == OK) {
            /* A later screen leaves the window as it is: no phase 3 rule
               changes the screen's size. */
            if (pane->phase == PANE_AWAITING_SCREEN)
                ask_for_window(pane,
                               (const DISPLAY_SCREEN_EVENT *)incoming.bytes);
        } else if (display_connection_window_made_check(incoming.bytes, bytes,
                                                        0, 0) == OK) {
            window_answered(pane,
                            (const DISPLAY_WINDOW_REPLY *)incoming.bytes);
        } else {
            fail(pane, "a connection message is malformed", which);
        }
    }
}

/* ------------------------------------------------------------------------
   The window's events. */

/* The window's size and state, which the first configure gives and each
   later one may change: its size is the panel's box's, the one the pane
   asked for and the panel is drawn at, so another is refused. A present
   for an earlier serial is stale: the compositor ignores it, answers with
   no frame and releases its buffer, so after a configure the pane waits
   for no frame, presents the whole window for the new serial, and draws
   in whichever buffer the compositor has released. */
static void configured(PANE *pane, const WINDOW_CONFIGURE_EVENT *event)
{
    int first = pane->phase == PANE_AWAITING_CONFIGURE;

    if (pane->phase < PANE_AWAITING_CONFIGURE)
        return;
    if (event->width != box_width(pane) || event->height != box_height(pane)) {
        say("nova-console: window refused reason=\"size\"\r\n");
        pane_drop(pane);
        return;
    }
    pane->serial = event->serial;
    pane->owed = 0;
    pane->whole = 1;
    pane->phase = PANE_SHOWING;
    if (!first)
        return;
    say("nova-console: window screen=");
    say_number(pane->screen_width);
    say("x");
    say_number(pane->screen_height);
    say(" size=");
    say_number(event->width);
    say("x");
    say_number(event->height);
    say("\r\n");
}

/* A key: the edit it makes, as keys_event would make it. */
static void key_taken(PANE_INPUT *input, const WINDOW_KEY_EVENT *event)
{
    input->edit = keys_window_edit(
        event->code, event->value, display_window_key_characters(incoming.bytes),
        event->text_length, &input->character);
}

int pane_input(PANE *pane, PANE_INPUT *input)
{
    UINT32 bytes, which = 0, status, i;

    input->edit = EDIT_NONE;
    input->character = 0;
    input->lines = 0;
    if (pane->window == HANDLE_INVALID)
        return 0;
    status = take(pane->window, &bytes);
    if (status == ERR_SHOULD_WAIT)
        return 0;
    if (status == ERR_PEER_CLOSED) {
        pane_drop(pane);
        return 0;
    }
    if (status != OK) {
        fail(pane, "window read", status);
        return 0;
    }
    if (display_window_event_check(incoming.bytes, bytes, 0, 0, &which) !=
        OK) {
        fail(pane, "a window event is malformed", which);
        return 0;
    }
    switch (which) {
    case WINDOW_CONFIGURE:
        configured(pane, (const WINDOW_CONFIGURE_EVENT *)incoming.bytes);
        break;
    case WINDOW_RELEASED:
        for (i = 0; i < PANE_BUFFERS; i++)
            if (((const WINDOW_RELEASED_EVENT *)incoming.bytes)->buffers >> i &
                1)
                pane->buffers[i].released = 1;
        break;
    case WINDOW_FRAME:
        pane->owed = 0;
        break;
    case WINDOW_KEY:
        key_taken(input, (const WINDOW_KEY_EVENT *)incoming.bytes);
        break;
    case WINDOW_POINTER:
        input->lines = view_wheel(
            &pane->wheel, ((const WINDOW_POINTER_EVENT *)incoming.bytes)->wheel);
        break;
    default:
        break;
    }
    return 1;
}

/* ------------------------------------------------------------------------
   Drawing and presenting. */

/* The sections a buffer of bytes bytes is held in, each SECTION_MAX_SIZE
   but the last (docs/spec/display.md D6). */
static UINT32 sections_of(UINT64 bytes)
{
    return (UINT32)((bytes + SECTION_MAX_SIZE - 1) / SECTION_MAX_SIZE);
}

/* Make buffer number's memory, the window's size, a row after row with no
   gap, in sections of SECTION_MAX_SIZE bytes but the last, mapped one
   after another, and give it to the window (docs/spec/display.md D6,
   Buffers). It holds no pixel yet, so it shows what a new buffer of
   nothing drawn does, and the compositor may read it at once. Returns OK,
   or the status of the call that failed with nothing of it left mapped or
   open. */
static UINT32 give_buffer(PANE *pane, UINT32 number)
{
    PANE_BUFFER *buffer = &pane->buffers[number];
    UINT32 width = (UINT32)box_width(pane), height = (UINT32)box_height(pane);
    UINT64 bytes = ((UINT64)width * height * PIXEL_BYTES + PAGE_MASK) &
                   ~(UINT64)PAGE_MASK;
    UINT32 sections = sections_of(bytes);
    UINT32 created[CHANNEL_MAX_HANDLES], sent[CHANNEL_MAX_HANDLES];
    HANDLE_TRANSFER transfers[CHANNEL_MAX_HANDLES];
    WINDOW_BUFFER_REQUEST request;
    UINT8 *start = 0;
    UINT32 made = 0, duplicated = 0, size, status = OK, i;

    if (sections > CHANNEL_MAX_HANDLES)
        return ERR_INVALID_ARGS;
    for (i = 0; i < sections && status == OK; i++) {
        UINT64 rest = bytes - (UINT64)i * SECTION_MAX_SIZE;
        UINT64 piece = rest < SECTION_MAX_SIZE ? rest : SECTION_MAX_SIZE;
        void *at;

        status = section_create(piece, 0, &created[i]);
        if (status != OK)
            break;
        made++;
        at = mapping_section(pane->mapping, created[i], piece,
                             MAP_READ | MAP_WRITE, &status);
        if (!start)
            start = at;
        if (status == OK)
            status = handle_duplicate(created[i],
                                      SECTION_RIGHT_READ | RIGHT_TRANSFER,
                                      &sent[i]);
        if (status == OK)
            duplicated++;
    }
    request.header.transaction = 0;
    request.header.kind = 0;
    request.buffer = number;
    request.width = width;
    request.height = height;
    request.stride = width;
    if (status == OK)
        status = display_window_buffer_encode(&request, sent, sections,
                                              transfers, outgoing.bytes,
                                              &size);
    if (status == OK)
        status = channel_write(pane->window, outgoing.bytes, size, transfers,
                               sections);
    for (i = 0; i < made; i++)
        handle_close(created[i]);
    if (status != OK) {
        for (i = 0; i < duplicated; i++)
            handle_close(sent[i]);
        if (start)
            mapping_unmap(pane->mapping, start,
                          pane->mapping->next - (UINT64)start);
        return status;
    }
    buffer->pixels = (UINT32 *)start;
    buffer->bytes = bytes;
    buffer->released = 1;
    panel_window_forget(&pane->layout, &buffer->shown);
    return OK;
}

/* Present buffer number for the configure's serial, with rectangle as its
   damage unless it is 0 (docs/spec/display.md D7). Returns OK, or the
   status of the call that failed. */
static UINT32 present(PANE *pane, UINT32 number, const RECT *damage)
{
    WINDOW_PRESENT_REQUEST request;
    WINDOW_RECT rectangle;
    UINT32 size, status;

    request.header.transaction = 0;
    request.header.kind = 0;
    request.buffer = number;
    request.serial = pane->serial;
    request.rectangles = damage ? 1 : 0;
    if (damage) {
        rectangle.x = (UINT32)damage->left;
        rectangle.y = (UINT32)damage->top;
        rectangle.width = (UINT32)(damage->right - damage->left);
        rectangle.height = (UINT32)(damage->bottom - damage->top);
    }
    status = display_window_present_encode(&request, damage ? &rectangle : 0,
                                           request.rectangles, outgoing.bytes,
                                           &size);
    if (status == OK)
        status = channel_write(pane->window, outgoing.bytes, size, no_handles,
                               0);
    return status;
}

void pane_show(PANE *pane, const TEXT *text, UINT64 top,
               const VIEW_CARET *caret)
{
    int available[PANE_BUFFERS], chosen;
    UINT32 target, i, status;
    PANE_BUFFER *buffer;
    RECT damage;
    int whole = pane->whole || pane->presented == PANEL_NO_BUFFER;

    if (pane->phase != PANE_SHOWING || pane->owed)
        return;
    for (i = 0; i < PANE_BUFFERS; i++)
        available[i] = !pane->buffers[i].pixels || pane->buffers[i].released;
    chosen = panel_window_buffer(available, PANE_BUFFERS);
    if (chosen == PANEL_NO_BUFFER)
        return;
    target = (UINT32)chosen;
    buffer = &pane->buffers[target];
    panel_view(&pane->layout, text, top, caret, &pane->want);
    if (!whole && !panel_window_damage(&pane->layout,
                                       &pane->buffers[pane->presented].shown,
                                       &pane->want, &damage))
        return;
    if (!buffer->pixels) {
        status = give_buffer(pane, target);
        if (status != OK) {
            fail(pane, "window buffer", status);
            return;
        }
    }
    panel_window_draw(&pane->layout, pane->masks, buffer->pixels,
                      (UINT32)box_width(pane), &buffer->shown, &pane->want);
    status = present(pane, target, whole ? 0 : &damage);
    if (status != OK) {
        fail(pane, "present", status);
        return;
    }
    buffer->released = 0;
    pane->presented = (int)target;
    pane->owed = 1;
    pane->whole = 0;
}
