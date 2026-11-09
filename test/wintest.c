/* The window test client (wintest.h). It reads the screen from its
   connection, makes one window on the SYSTEM layer of the panel's
   surface, waits for the window's first configure, draws a buffer for it
   and presents it, then draws a second buffer, with a square changed, and
   presents that with the square as damage, waiting each time for the
   frame that shows the present. It logs each step and each key, entry,
   pointer event and leave its window is sent, those that come while it
   waits as those after, and holds the window until the compositor closes
   it or its connection (docs/spec/display.md D5 to D7 and D11, rules 2
   and 3). Any step that fails, and any event of the window that is not one
   of those, ends it WINTEST_EXIT_FAILED, logged. */

#include "display_protocol.h"
#include "log.h"
#include "mapping.h"
#include "native.h"
#include "timing.h"
#include "wintest.h"

/* Its own memory: the buffers, each mapped at the next free address from
   here, far above its image and below its stack. */
#define MAPPINGS_BASE 0x0000000100000000ULL

#define PIXEL_BYTES 4
#define PAGE_MASK (PAGE_SIZE - 1)
#define WINDOW_TRANSACTION 1
#define BUFFER_FIRST 0
#define BUFFER_SECOND 1
#define WAKE_SIGNALS (CHANNEL_READABLE | CHANNEL_PEER_CLOSED)

/* A message read, aligned to 8 bytes as the generated checks require
   (docs/spec/interface.md P14), whole, whatever its size. */
static union {
    UINT64 alignment;
    UINT8 bytes[CHANNEL_MAX_BYTES];
} incoming;

/* channel_write needs a valid array even for no handles. */
static const HANDLE_TRANSFER no_handles[1];

/* A message to send. */
static union {
    UINT64 alignment;
    UINT8 bytes[DISPLAY_WINDOW_TO_SERVER_BYTES_MAX];
} outgoing;

/* The names of a key event's modifier bits, in the order a line gives
   them. */
typedef struct {
    UINT32 bit;
    const char *name;
} MODIFIER_NAME;

static const MODIFIER_NAME modifier_names[] = {
    {DISPLAY_MODIFIER_SHIFT, "shift"},
    {DISPLAY_MODIFIER_CONTROL, "control"},
    {DISPLAY_MODIFIER_ALT, "alt"},
    {DISPLAY_MODIFIER_SUPER, "super"},
};

/* evdev's BTN_LEFT, and the names a pointer line gives it and the seven
   codes after it, as the console's lines name a button; a code past them
   is written as its number. */
#define BTN_LEFT 0x110
static const char *const button_names[] = {
    "left", "right", "middle", "side", "extra", "forward", "back", "task"};
#define BUTTON_NAMES (sizeof button_names / sizeof button_names[0])

static MAPPING mapping = {HANDLE_INVALID, MAPPINGS_BASE};
static UINT32 connection = HANDLE_INVALID;
static UINT32 window = HANDLE_INVALID;

static _Noreturn void fail(const char *step, UINT32 status)
{
    say("nova-wintest: failed step=");
    say_value(step);
    say(" status=");
    say_number(status);
    say("\r\n");
    process_exit(WINTEST_EXIT_FAILED);
}

/* The compositor closed the connection or the window: nothing to hold. */
static _Noreturn void closed(void)
{
    process_exit(WINTEST_EXIT_CLOSED);
}

/* The next message of channel, waited for, in incoming, with its size and
   no handle, since the compositor sends none. A closed channel ends the
   client, closed. */
static UINT32 next_message(UINT32 channel)
{
    RECEIVED_HANDLE handle;
    UINT32 bytes, count, observed, status;

    for (;;) {
        status = channel_read(channel, incoming.bytes, sizeof incoming.bytes,
                              &handle, 1, &bytes, &count);
        if (status == OK && count == 0)
            return bytes;
        if (status == OK)
            fail("a message carried a handle", count);
        if (status == ERR_PEER_CLOSED)
            closed();
        if (status != ERR_SHOULD_WAIT)
            fail("channel read", status);
        object_wait_one(channel, WAKE_SIGNALS, DEADLINE_INFINITE, &observed);
    }
}

/* The bootstrap message: the debug log, the process and the connection. */
static void take_bootstrap(UINT32 bootstrap)
{
    RECEIVED_HANDLE handles[WINTEST_BOOTSTRAP_HANDLES];
    UINT32 bytes, count, observed, status;

    for (;;) {
        status = channel_read(bootstrap, incoming.bytes, sizeof incoming.bytes,
                              handles, WINTEST_BOOTSTRAP_HANDLES, &bytes,
                              &count);
        if (status != ERR_SHOULD_WAIT)
            break;
        object_wait_one(bootstrap, WAKE_SIGNALS, DEADLINE_INFINITE,
                        &observed);
    }
    if (status != OK || bytes != WINTEST_BOOTSTRAP_BYTES ||
        count != WINTEST_BOOTSTRAP_HANDLES ||
        handles[WINTEST_BOOTSTRAP_DEBUG_LOG].type != TYPE_RESOURCE ||
        handles[WINTEST_BOOTSTRAP_SELF].type != TYPE_PROCESS ||
        handles[WINTEST_BOOTSTRAP_CONNECTION].type != TYPE_CHANNEL)
        process_exit(WINTEST_EXIT_FAILED);
    say_open(handles[WINTEST_BOOTSTRAP_DEBUG_LOG].handle);
    mapping.self = handles[WINTEST_BOOTSTRAP_SELF].handle;
    connection = handles[WINTEST_BOOTSTRAP_CONNECTION].handle;
}

/* The screen's size from the connection's first message (D6). */
static void take_screen(UINT32 *width, UINT32 *height)
{
    const DISPLAY_SCREEN_EVENT *event =
        (const DISPLAY_SCREEN_EVENT *)incoming.bytes;
    UINT32 bytes = next_message(connection), which = 0;

    if (display_connection_event_check(incoming.bytes, bytes, 0, 0,
                                       &which) != OK ||
        which != DISPLAY_SCREEN)
        fail("the connection's first message is not the screen", which);
    *width = event->width;
    *height = event->height;
}

/* Ask for a window of the size wanted on the SYSTEM layer, the panel's
   surface, with a channel of its own, whose compositor end goes with the
   request, and wait for the answer. */
static void make_window(UINT32 wanted_width, UINT32 wanted_height)
{
    DISPLAY_WINDOW_REQUEST request;
    const DISPLAY_WINDOW_REPLY *reply =
        (const DISPLAY_WINDOW_REPLY *)incoming.bytes;
    HANDLE_TRANSFER transfer;
    UINT32 served = HANDLE_INVALID, size, bytes, status;

    request.header.transaction = WINDOW_TRANSACTION;
    request.header.kind = 0;
    request.width = wanted_width;
    request.height = wanted_height;
    request.layer = DISPLAY_LAYER_SYSTEM;
    request.surface = DISPLAY_SURFACE_PANEL;
    status = channel_create(WINDOW_CAPACITY, &window, &served);
    if (status != OK)
        fail("window channel", status);
    status = display_connection_window_encode(&request, &served, 1, &transfer,
                                              outgoing.bytes, &size);
    if (status == OK)
        status = channel_write(connection, outgoing.bytes, size, &transfer, 1);
    if (status != OK)
        fail("window request", status);
    bytes = next_message(connection);
    if (display_connection_window_made_check(incoming.bytes, bytes, 0, 0) !=
            OK ||
        reply->transaction != WINDOW_TRANSACTION)
        fail("the answer is not the window's", reply->transaction);
    if (reply->status != OK)
        fail("window refused", reply->status);
}

/* The window's next event, checked, in incoming; returns its kind. */
static UINT32 next_event(void)
{
    UINT32 bytes = next_message(window), which = 0;

    if (display_window_event_check(incoming.bytes, bytes, 0, 0, &which) != OK)
        fail("a window event is malformed", which);
    return which;
}

/* Draw frame of a width x height window into a new buffer, its rows
   WINTEST_STRIDE_PAD pixels longer than the window is wide, in whole
   pages and in sections of SECTION_MAX_SIZE bytes but the last, mapped one
   after another, and give it to the window as number (display.md D6,
   Buffers). The extra pixels of a row are WINTEST_PAD_PIXEL. */
static void give_buffer(UINT32 number, UINT32 width, UINT32 height,
                        UINT32 frame)
{
    UINT32 stride = width + WINTEST_STRIDE_PAD;
    UINT64 bytes =
        ((UINT64)stride * height * PIXEL_BYTES + PAGE_MASK) & ~(UINT64)PAGE_MASK;
    UINT32 sections = (UINT32)((bytes + SECTION_MAX_SIZE - 1) / SECTION_MAX_SIZE);
    UINT32 created[CHANNEL_MAX_HANDLES], sent[CHANNEL_MAX_HANDLES];
    HANDLE_TRANSFER transfers[CHANNEL_MAX_HANDLES];
    WINDOW_BUFFER_REQUEST request;
    UINT32 *pixels = 0, size, status = OK, i, x, y;

    for (i = 0; i < sections && status == OK; i++) {
        UINT64 rest = bytes - (UINT64)i * SECTION_MAX_SIZE;
        UINT64 piece = rest < SECTION_MAX_SIZE ? rest : SECTION_MAX_SIZE;
        void *at;

        status = section_create(piece, 0, &created[i]);
        if (status != OK)
            break;
        at = mapping_section(&mapping, created[i], piece,
                             MAP_READ | MAP_WRITE, &status);
        if (!pixels)
            pixels = at;
        if (status == OK)
            status = handle_duplicate(created[i],
                                      SECTION_RIGHT_READ | RIGHT_TRANSFER,
                                      &sent[i]);
    }
    if (status != OK)
        fail("buffer sections", status);
    for (y = 0; y < height; y++)
        for (x = 0; x < stride; x++)
            pixels[(UINT64)y * stride + x] =
                x < width ? wintest_pixel(width, height, x, y, frame)
                          : WINTEST_PAD_PIXEL;
    request.header.transaction = 0;
    request.header.kind = 0;
    request.buffer = number;
    request.width = width;
    request.height = height;
    request.stride = stride;
    status = display_window_buffer_encode(&request, sent, sections, transfers,
                                          outgoing.bytes, &size);
    if (status == OK)
        status = channel_write(window, outgoing.bytes, size, transfers,
                               sections);
    if (status != OK)
        fail("buffer message", status);
    for (i = 0; i < sections; i++)
        handle_close(created[i]);
}

/* Present buffer number for serial, with rectangle as its damage unless
   it is 0. */
static void present(UINT32 number, UINT32 serial, const WINDOW_RECT *rectangle)
{
    WINDOW_PRESENT_REQUEST request;
    UINT32 size, status;

    request.header.transaction = 0;
    request.header.kind = 0;
    request.buffer = number;
    request.serial = serial;
    request.rectangles = rectangle ? 1 : 0;
    status = display_window_present_encode(&request, rectangle,
                                           request.rectangles, outgoing.bytes,
                                           &size);
    if (status == OK)
        status = channel_write(window, outgoing.bytes, size, no_handles, 0);
    if (status != OK)
        fail("present", status);
}

static const char *value_name(UINT32 value)
{
    switch (value) {
    case INPUT_KEY_RELEASED:
        return "released";
    case INPUT_KEY_PRESSED:
        return "pressed";
    default:
        return "repeated";
    }
}

/* Log the key event in incoming, the fields of a WINDOW_KEY as it
   carries them: its code, value, the modifiers held as a comma-joined
   list of names in lower case or none, the text as a value, which is
   empty for a key that gives none, and the time the driver took the key,
   in milliseconds to the tenth. */
static void log_key(void)
{
    const WINDOW_KEY_EVENT *event = (const WINDOW_KEY_EVENT *)incoming.bytes;
    UINT32 i, written = 0;

    say("nova-wintest: key code=");
    say_number(event->code);
    say(" value=");
    say(value_name(event->value));
    say(" modifiers=");
    for (i = 0; i < sizeof modifier_names / sizeof modifier_names[0]; i++)
        if (event->modifiers & modifier_names[i].bit) {
            if (written++)
                say(",");
            say(modifier_names[i].name);
        }
    if (!written)
        say("none");
    say(" text=");
    say_value_bytes(display_window_key_characters(incoming.bytes),
                    event->text_length);
    say(" time_ms=");
    timing_say_ms(event->time);
    say("\r\n");
}

/* n in decimal, with a minus sign where it is below 0. */
static void say_signed(INT32 n)
{
    if (n < 0)
        say("-");
    say_number(n < 0 ? (UINT64)(-(INT64)n) : (UINT64)n);
}

static void say_button(UINT32 code)
{
    if (code >= BTN_LEFT && code - BTN_LEFT < BUTTON_NAMES)
        say(button_names[code - BTN_LEFT]);
    else
        say_number(code);
}

/* Log the pointer event in incoming, the fields of a WINDOW_POINTER as it
   carries them: the pointer's place across and down the window and the
   wheel's turn, each a signed whole number of the unit its key names (256ths
   of a pixel and of 120ths of a detent), the buttons changed, each its name
   and its new state, comma-joined, or none, and the time the driver took
   the report's end. */
static void log_pointer(void)
{
    const WINDOW_POINTER_EVENT *event =
        (const WINDOW_POINTER_EVENT *)incoming.bytes;
    UINT32 i;

    say("nova-wintest: pointer x_256ths=");
    say_signed(event->x);
    say(" y_256ths=");
    say_signed(event->y);
    say(" wheel_120ths=");
    say_signed(event->wheel);
    say(" buttons=");
    for (i = 0; i < event->buttons; i++) {
        if (i)
            say(",");
        say_button(event->button[i].code);
        say(event->button[i].value == INPUT_KEY_PRESSED ? ":pressed"
                                                        : ":released");
    }
    if (!event->buttons)
        say("none");
    say(" time_ms=");
    timing_say_ms(event->time);
    say("\r\n");
}

/* Log the entry in incoming: the pointer's place in the window and the
   time, as log_pointer does. */
static void log_enter(void)
{
    const WINDOW_ENTER_EVENT *event =
        (const WINDOW_ENTER_EVENT *)incoming.bytes;

    say("nova-wintest: enter x_256ths=");
    say_signed(event->x);
    say(" y_256ths=");
    say_signed(event->y);
    say(" time_ms=");
    timing_say_ms(event->time);
    say("\r\n");
}

/* Log the input event of kind which in incoming, and return 1; for any
   other kind, return 0. */
static int log_input(UINT32 which)
{
    switch (which) {
    case WINDOW_KEY:
        log_key();
        return 1;
    case WINDOW_POINTER:
        log_pointer();
        return 1;
    case WINDOW_ENTER:
        log_enter();
        return 1;
    case WINDOW_LEAVE:
        say("nova-wintest: leave\r\n");
        return 1;
    default:
        return 0;
    }
}

/* The window's next event that is not input, in incoming; returns its
   kind. A window is sent the pointer's entry and moves once a present of
   it is shown, which is before its frame message, so input can come among
   the answers to a present: it is logged as the end of the run logs it. */
static UINT32 next_answer(void)
{
    UINT32 which = next_event();

    while (log_input(which))
        which = next_event();
    return which;
}

/* The serial and size of the window's first configure. */
static UINT32 take_configure(UINT32 *width, UINT32 *height)
{
    const WINDOW_CONFIGURE_EVENT *event =
        (const WINDOW_CONFIGURE_EVENT *)incoming.bytes;
    UINT32 which = next_answer();

    if (which != WINDOW_CONFIGURE)
        fail("the first event is not a configure", which);
    *width = event->width;
    *height = event->height;
    return event->serial;
}

/* Read the events that answer a present of buffer number: the release of
   the buffers in released, a mask, if it is not 0, and then the frame that
   shows the present. Each is logged, and any input between them. */
static void await_frame(UINT32 number, UINT32 released)
{
    const WINDOW_RELEASED_EVENT *event =
        (const WINDOW_RELEASED_EVENT *)incoming.bytes;
    UINT32 which = next_answer();

    if (released) {
        if (which != WINDOW_RELEASED || event->buffers != released)
            fail("the release is not of the buffers expected", which);
        say("nova-wintest: released buffers=");
        say_number(event->buffers);
        say("\r\n");
        which = next_answer();
    }
    if (which != WINDOW_FRAME)
        fail("the event is not the frame that shows the present", which);
    say("nova-wintest: shown frame=");
    say_number(number);
    say("\r\n");
}

_Noreturn void wintest_main(UINT32 bootstrap, UINT64 argument)
{
    UINT32 screen_width, screen_height, wanted_width, wanted_height;
    UINT32 width, height, serial, left, top, side;
    WINDOW_RECT marker;

    (void)argument;
    take_bootstrap(bootstrap);
    take_screen(&screen_width, &screen_height);
    wintest_want(screen_width, screen_height, &wanted_width, &wanted_height);
    make_window(wanted_width, wanted_height);
    serial = take_configure(&width, &height);
    say("nova-wintest: window screen=");
    say_number(screen_width);
    say("x");
    say_number(screen_height);
    say(" size=");
    say_number(width);
    say("x");
    say_number(height);
    say("\r\n");
    give_buffer(BUFFER_FIRST, width, height, 1);
    present(BUFFER_FIRST, serial, 0);
    await_frame(1, 0);
    give_buffer(BUFFER_SECOND, width, height, 2);
    wintest_marker(width, height, &left, &top, &side);
    marker.x = left;
    marker.y = top;
    marker.width = side;
    marker.height = side;
    present(BUFFER_SECOND, serial, side ? &marker : 0);
    await_frame(2, 1U << BUFFER_FIRST);
    for (;;) {
        UINT32 which = next_event();

        if (!log_input(which))
            fail("an event that is not input", which);
    }
}
