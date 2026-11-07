/* The console, \nova\console.exe (docs/spec/console.md): the input
   drivers' client, which the platform driver starts and hands each
   input endpoint on its bootstrap channel (C2), and the server of the
   sessions the first process sends on its control channel (C4).
   Once the display comes there (C1) it draws the panel and, after every
   round of messages, the cells whose text changed and the caret.

   One thread waits on every channel at once with object_wait_many, with
   a deadline at the caret's next blink while a read waits on a drawn
   panel, and serves
   each channel that is ready; a session is served at most its queue's
   worth of messages a round, so a busy one cannot keep the keys from
   the others. */

#include "console_protocol.h"
#include "font.h"
#include "input_protocol.h"
#include "keys.h"
#include "log.h"
#include "mapping.h"
#include "native.h"
#include "panel.h"
#include "text.h"
#include "view.h"
#include "wipe.h"

/* It ends with EXIT_SUPERVISOR_GONE once the platform driver has closed
   its bootstrap channel, else with EXIT_FAILED. Both lie above the
   exception vectors a faulting process ends with (objects.md O10). */
#define EXIT_SUPERVISOR_GONE 100
#define EXIT_FAILED 101

/* Its own memory: the framebuffer's mapping, the backdrop and the masks,
   each mapped at the next free address from here, far above its image
   and below its stack. */
#define MAPPINGS_BASE 0x0000000100000000ULL

/* The caret shows for half a second, then hides for as long, while a
   read waits; a key shows it again at once. */
#define NS_PER_MS 1000000ULL
#define CARET_BLINK_NS (500 * NS_PER_MS)

#define TIMES_TEN_PER_MS 10

/* The objects one wait watches: the bootstrap channel, the control
   channel, the input endpoints and the sessions. */
#define ENTRY_BOOTSTRAP 0
#define ENTRY_CONTROL 1
#define ENTRY_INPUTS 2
#define ENTRY_SESSIONS (ENTRY_INPUTS + INPUT_DEVICES_MAX)
#define ENTRIES (ENTRY_SESSIONS + CONSOLE_SESSIONS)

#define WAKE_SIGNALS (CHANNEL_READABLE | CHANNEL_PEER_CLOSED)

/* The rights the console needs on its end of a session, and on the
   channels it only reads: the control channel and an input endpoint. */
#define SESSION_NEEDS (CHANNEL_RIGHT_READ | CHANNEL_RIGHT_WRITE | RIGHT_WAIT)
#define CHANNEL_NEEDS (CHANNEL_RIGHT_READ | RIGHT_WAIT)

/* A session: its channel, HANDLE_INVALID for a free slot, and the read
   that waits on it, if one does, numbered in the order reads came. */
typedef struct {
    UINT32 channel;
    UINT32 reading;
    UINT32 transaction;
    UINT32 limit;
    UINT32 echo;
    UINT64 order;
} SESSION;

/* An input driver's endpoint, HANDLE_INVALID for a free slot; the
   kind and absolute axes its description gave, the kind 0 until it has
   come; and whether a key event has come on it. */
typedef struct {
    UINT32 endpoint;
    UINT32 kind;
    UINT32 absolute;
    UINT32 keys;
} INPUT;

/* A key typed while no read waits, kept for the next. */
typedef struct {
    UINT8 edit;
    UINT8 character;
} TYPED;

/* channel_write needs a valid array even for no handles. */
static const HANDLE_TRANSFER no_handles[1];

static UINT32 bootstrap = HANDLE_INVALID;
static UINT32 control = HANDLE_INVALID;
static INPUT inputs[INPUT_DEVICES_MAX];
static SESSION sessions[CONSOLE_SESSIONS];
static UINT64 reads;
static int reader = -1;
static TEXT text;
static EDITOR editor;
static KEYBOARD modifiers;
static TYPED typed[CONSOLE_LINE_MAX];
static UINT32 typed_count;
static UINT64 back;

static UINT32 drawing;
static SCREEN screen;
static LAYOUT layout;
static UINT32 *backdrop;
static UINT8 *masks;
static INKS inks;
static PANEL panel = {&screen, &layout, 0, 0, &inks};
static PANEL_SHOWN shown;
static UINT32 caret_lit;
static UINT64 blink_at;
static MAPPING mapping = {HANDLE_INVALID, MAPPINGS_BASE};

static union {
    CONSOLE_REQUEST request;
    INPUT_EVENT event;
    INPUT_DEVICE description;
    INPUT_POINTER report;
    CONSOLE_SETUP_DISPLAY_MESSAGE setup;
    UINT8 bytes[sizeof(CONSOLE_REQUEST) + CONSOLE_WRITE_MAX];
} message;

/* A read's reply, wiped once written, since a line read without echo is
   a secret. */
static struct {
    CONSOLE_REPLY reply;
    UINT8 line[CONSOLE_LINE_MAX];
} answer;

static UINT64 now(void)
{
    UINT64 nanoseconds = 0;

    clock_monotonic(&nanoseconds);
    return nanoseconds;
}

static void say_status(const char *what, UINT32 status)
{
    say("nova-console: ");
    say(what);
    say(", status ");
    say_number(status);
    say("\r\n");
}

static void close_if_open(UINT32 *handle)
{
    if (*handle != HANDLE_INVALID)
        handle_close(*handle);
    *handle = HANDLE_INVALID;
}

/* ------------------------------------------------------------------------
   Reads and the keys. */

/* Text that comes while a read waits goes before its line. */
static void output(const UINT8 *bytes, UINT64 count)
{
    if (reader >= 0)
        editor_hide(&editor, &text);
    text_write(&text, bytes, count);
    if (reader >= 0)
        editor_show(&editor, &text);
    back = 0;
}

/* Start the read that came first of those waiting, if one does. Returns
   whether one did. */
static int start_read(void)
{
    int i, first = -1;

    for (i = 0; i < CONSOLE_SESSIONS; i++)
        if (sessions[i].channel != HANDLE_INVALID && sessions[i].reading &&
            (first < 0 || sessions[i].order < sessions[first].order))
            first = i;
    if (first < 0)
        return 0;
    reader = first;
    editor_start(&editor, &text, sessions[first].limit, sessions[first].echo);
    caret_lit = 1;
    blink_at = now() + CARET_BLINK_NS;
    return 1;
}

static void end_session(int index)
{
    SESSION *session = &sessions[index];

    close_if_open(&session->channel);
    session->reading = 0;
    if (reader == index) {
        editor_hide(&editor, &text);
        wipe(editor.bytes, sizeof editor.bytes);
        reader = -1;
    }
}

/* Answer the reader with its line, which Enter has ended, and wipe it. A
   session the answer cannot reach ends. */
static void answer_read(void)
{
    SESSION *session = &sessions[reader];
    UINT32 i, status;

    answer.reply.transaction = session->transaction;
    answer.reply.length = editor.length;
    for (i = 0; i < editor.length; i++)
        answer.line[i] = editor.bytes[i];
    status = channel_write(session->channel, &answer,
                           (UINT32)sizeof answer.reply + editor.length,
                           no_handles, 0);
    wipe(&answer, sizeof answer);
    wipe(editor.bytes, sizeof editor.bytes);
    session->reading = 0;
    reader = -1;
    if (status != OK)
        end_session((int)(session - sessions));
}

static void apply(UINT32 edit, UINT8 character)
{
    if (editor_key(&editor, &text, edit, character))
        answer_read();
}

/* Start reads and give them the keys typed ahead, until no read waits
   or no key is left. A key given is wiped, since it may be part of a
   line read without echo. */
static void pump(void)
{
    UINT32 i;

    for (;;) {
        if (reader < 0 && !start_read())
            return;
        if (typed_count == 0)
            return;
        apply(typed[0].edit, typed[0].character);
        for (i = 1; i < typed_count; i++)
            typed[i - 1] = typed[i];
        typed_count--;
        wipe(&typed[typed_count], sizeof typed[typed_count]);
    }
}

/* The lines the view shows: the panel's, or before the display comes
   the most it can. */
static UINT32 view_rows(void)
{
    return drawing ? layout.rows : PANEL_ROWS;
}

/* Scroll the view lines back, or forward for a negative lines. */
static void scroll(INT64 lines)
{
    back = view_scroll(&text, view_rows(), back, lines);
}

static void key(const INPUT_EVENT *event)
{
    UINT8 character;
    UINT32 edit = keys_event(&modifiers, event->code, event->value,
                             &character);

    if (edit == EDIT_NONE)
        return;
    if (edit == EDIT_PAGE_UP || edit == EDIT_PAGE_DOWN) {
        scroll(edit == EDIT_PAGE_UP ? view_rows() / 2
                                    : -(INT64)(view_rows() / 2));
        return;
    }
    back = 0;
    caret_lit = 1;
    blink_at = now() + CARET_BLINK_NS;
    if (reader >= 0) {
        apply(edit, character);
    } else if (typed_count < CONSOLE_LINE_MAX) {
        typed[typed_count].edit = (UINT8)edit;
        typed[typed_count].character = character;
        typed_count++;
    }
}

/* ------------------------------------------------------------------------
   The pointer's lines (C6). */

/* A line of the log built whole, then written with one say, which the
   kernel writes in one piece, so that no other process's line lands
   among its pieces. A line longer than the buffer is cut short, before
   the LINE_END bytes its end and the NUL take. */
typedef struct {
    char text[DEBUG_WRITE_MAX_BYTES];
    UINT32 length;
} LINE;

#define LINE_END 3
#define DECIMAL_DIGITS_MAX 20

static void line_add(LINE *l, const char *piece)
{
    for (; *piece && l->length + LINE_END < sizeof l->text; piece++)
        l->text[l->length++] = *piece;
}

static void line_number(LINE *l, UINT64 n)
{
    char digits[DECIMAL_DIGITS_MAX + 1];
    UINT32 at = DECIMAL_DIGITS_MAX;

    digits[at] = 0;
    do
        digits[--at] = (char)('0' + n % 10);
    while (n /= 10);
    line_add(l, digits + at);
}

static void line_signed(LINE *l, INT32 n)
{
    if (n < 0)
        line_add(l, "-");
    line_number(l, n < 0 ? (UINT64)(-(INT64)n) : (UINT64)n);
}

static void line_say(LINE *l)
{
    l->text[l->length++] = '\r';
    l->text[l->length++] = '\n';
    l->text[l->length] = 0;
    say(l->text);
}

/* What a pointer line notes, as a kind and its detail: a motion,
   absolute or relative, with the axes it moves; a button pressed or
   released, with its code; the wheel turned up or down. */
#define NOTE_ABSOLUTE 0x10000
#define NOTE_RELATIVE 0x20000
#define NOTE_PRESSED 0x30000
#define NOTE_RELEASED 0x40000
#define NOTE_WHEEL 0x50000
#define NOTE_KIND 0xF0000
#define NOTE_DETAIL 0xFFFF
#define WHEEL_UP 0
#define WHEEL_DOWN 1
#define BOTH_AXES (INPUT_AXIS_X | INPUT_AXIS_Y)

/* evdev's BTN_MISC, below which every key code is a keyboard's key, and
   BTN_LEFT, with its names for that code and the seven after it, up to
   BTN_TASK. */
#define BTN_MISC 0x100
#define BTN_LEFT 0x110
static const char *const button_names[] = {
    "left", "right", "middle", "side", "extra", "forward", "back", "task"};
#define BUTTON_NAMES (sizeof button_names / sizeof button_names[0])

/* The pointer line noted last. */
static UINT32 pointer_noted;

static void line_axes(LINE *l, UINT32 axes)
{
    if (axes == BOTH_AXES)
        line_add(l, " x and y");
    else
        line_add(l, axes == INPUT_AXIS_X ? " x" : " y");
}

/* A button by its evdev name, a code past those by its number, and a
   keyboard's key by neither, since its code is what was typed. */
static void line_button(LINE *l, UINT32 code)
{
    if (code >= BTN_LEFT && code - BTN_LEFT < BUTTON_NAMES) {
        line_add(l, button_names[code - BTN_LEFT]);
        line_add(l, " button");
    } else if (code >= BTN_MISC) {
        line_add(l, "button ");
        line_number(l, code);
    } else {
        line_add(l, "key");
    }
}

/* Whether note says what the note before it did: the same motion's
   kind, whatever axes it moves, or the same button change or turn. */
static int noted(UINT32 note)
{
    UINT32 kind = note & NOTE_KIND;

    if (kind == NOTE_ABSOLUTE || kind == NOTE_RELATIVE)
        return kind == (pointer_noted & NOTE_KIND);
    return note == pointer_noted;
}

/* Log note unless it says what the one before did, so that a run of
   motion of one kind, or of turns one way, is one line. */
static void pointer_line(UINT32 note)
{
    UINT32 detail = note & NOTE_DETAIL;
    LINE l;

    if (noted(note))
        return;
    pointer_noted = note;
    l.length = 0;
    line_add(&l, "nova-console: pointer ");
    switch (note & NOTE_KIND) {
    case NOTE_ABSOLUTE:
        line_add(&l, "moved, absolute");
        line_axes(&l, detail);
        break;
    case NOTE_RELATIVE:
        line_add(&l, "moved, relative");
        line_axes(&l, detail);
        break;
    case NOTE_PRESSED:
    case NOTE_RELEASED:
        line_button(&l, detail);
        line_add(&l, (note & NOTE_KIND) == NOTE_PRESSED ? " pressed"
                                                         : " released");
        break;
    default:
        line_add(&l, detail == WHEEL_UP ? "wheel turned up"
                                        : "wheel turned down");
        break;
    }
    line_say(&l);
}

/* Log what a pointer report does, and nothing of where the pointer is
   or how far it moved. A turn of the wheel also scrolls the view
   VIEW_WHEEL_LINES a detent (C3): a turn up, a positive wheel (C6), back
   through what the panel has shown, as Page Up does, and a turn down
   forward. */
static void pointer(const INPUT_POINTER *report)
{
    UINT32 i;

    if (report->absolute)
        pointer_line(NOTE_ABSOLUTE | report->absolute);
    if (report->dx || report->dy)
        pointer_line(NOTE_RELATIVE | (report->dx ? INPUT_AXIS_X : 0) |
                     (report->dy ? INPUT_AXIS_Y : 0));
    for (i = 0; i < report->buttons; i++)
        pointer_line((report->button[i].value == INPUT_KEY_PRESSED
                          ? NOTE_PRESSED
                          : NOTE_RELEASED) |
                     report->button[i].code);
    if (report->wheel) {
        pointer_line(NOTE_WHEEL |
                     (report->wheel > 0 ? WHEEL_UP : WHEEL_DOWN));
        scroll((INT64)VIEW_WHEEL_LINES * report->wheel);
    }
}

/* Whether a message of a pointer report's size is one from a pointer
   with the absolute axes axes: tagged as one, setting no other axis,
   and changing at most INPUT_REPORT_BUTTONS buttons, each pressed or
   released. */
static int is_report(const INPUT_POINTER *report, UINT32 axes)
{
    UINT32 i;

    if (report->transaction != 0 || report->type != INPUT_TYPE_SYN ||
        report->code != INPUT_SYN_REPORT || report->reserved != 0 ||
        (report->absolute & ~axes) != 0 ||
        report->buttons > INPUT_REPORT_BUTTONS)
        return 0;
    for (i = 0; i < report->buttons; i++)
        if (report->button[i].value != INPUT_KEY_PRESSED &&
            report->button[i].value != INPUT_KEY_RELEASED)
            return 0;
    return 1;
}

/* ------------------------------------------------------------------------
   An endpoint's description. */

/* Whether a message of a description's size is one: a keyboard's with
   no axis, or a pointer's with no axis but x and y, each with its
   maximum above its minimum. */
static int is_description(const INPUT_DEVICE *d)
{
    if (d->transaction != 0 || d->type != INPUT_TYPE_DEVICE ||
        d->reserved != 0 || (d->absolute & ~(UINT32)BOTH_AXES) != 0)
        return 0;
    if (d->kind == INPUT_DEVICE_KEYBOARD)
        return d->absolute == 0;
    return d->kind == INPUT_DEVICE_POINTER &&
           (!(d->absolute & INPUT_AXIS_X) || d->x.max > d->x.min) &&
           (!(d->absolute & INPUT_AXIS_Y) || d->y.max > d->y.min);
}

static void line_range(LINE *l, const char *axis, const INPUT_RANGE *r)
{
    line_add(l, axis);
    line_add(l, " from ");
    line_signed(l, r->min);
    line_add(l, " to ");
    line_signed(l, r->max);
}

/* Log what an endpoint's description says the device is, its axes'
   ranges included, which are the device's, not the user's. */
static void describe(const INPUT_DEVICE *d)
{
    LINE l;

    l.length = 0;
    line_add(&l, "nova-console: input from a ");
    if (d->kind == INPUT_DEVICE_KEYBOARD) {
        line_add(&l, "keyboard");
    } else {
        line_add(&l, "pointer");
        if (!d->absolute)
            line_add(&l, " with no absolute axis");
        if (d->absolute & INPUT_AXIS_X)
            line_range(&l, ", x", &d->x);
        if (d->absolute & INPUT_AXIS_Y)
            line_range(&l, ", y", &d->y);
    }
    line_say(&l);
}

/* ------------------------------------------------------------------------
   Sessions. */

/* Act on the request in message, byte_count bytes long, from session
   index. Returns 0 if it is not one the protocol has, which ends the
   session. */
static int request(int index, UINT32 byte_count)
{
    const CONSOLE_REQUEST *r = &message.request;
    SESSION *session = &sessions[index];

    if (byte_count < sizeof *r)
        return 0;
    if (r->operation == CONSOLE_WRITE) {
        if (r->flags != 0 || r->length > CONSOLE_WRITE_MAX ||
            byte_count != sizeof *r + r->length)
            return 0;
        output(message.bytes + sizeof *r, r->length);
        return 1;
    }
    if (r->operation != CONSOLE_READ || byte_count != sizeof *r ||
        (r->flags & ~(UINT32)CONSOLE_READ_NO_ECHO) || r->length == 0 ||
        r->length > CONSOLE_LINE_MAX || session->reading)
        return 0;
    session->reading = 1;
    session->transaction = r->transaction;
    session->limit = r->length;
    session->echo = !(r->flags & CONSOLE_READ_NO_ECHO);
    session->order = reads++;
    pump();
    return 1;
}

/* Serve up to the queue's worth of a session's messages. */
static void serve_session(int index)
{
    RECEIVED_HANDLE handles[1];
    UINT32 byte_count, handle_count, served, status;

    for (served = 0; served < CONSOLE_SESSION_CAPACITY; served++) {
        status = channel_read(sessions[index].channel, message.bytes,
                              sizeof message.bytes, handles, 0, &byte_count,
                              &handle_count);
        if (status == ERR_SHOULD_WAIT)
            return;
        if (status == OK && request(index, byte_count)) {
            if (sessions[index].channel == HANDLE_INVALID)
                return;
            continue;
        }
        if (status != ERR_PEER_CLOSED)
            say_status("a session sent what the protocol does not have; "
                       "it ends",
                       status);
        end_session(index);
        pump();
        return;
    }
}

static void add_session(const RECEIVED_HANDLE *handle)
{
    int i;

    if (handle->type == TYPE_CHANNEL &&
        (handle->rights & SESSION_NEEDS) == SESSION_NEEDS)
        for (i = 0; i < CONSOLE_SESSIONS; i++)
            if (sessions[i].channel == HANDLE_INVALID) {
                sessions[i].channel = handle->handle;
                sessions[i].reading = 0;
                return;
            }
    say("nova-console: a session is refused: no room, or not a channel "
        "the console can serve\r\n");
    handle_close(handle->handle);
}

/* ------------------------------------------------------------------------
   The display. */

static void say_ms(UINT64 nanoseconds)
{
    UINT64 tenths = nanoseconds * TIMES_TEN_PER_MS / NS_PER_MS;

    say_number(tenths / TIMES_TEN_PER_MS);
    say(".");
    say_number(tenths % TIMES_TEN_PER_MS);
    say(" ms");
}

/* Fit the text to the panel's width: its lines wrap at the narrower of
   the two from now on. */
static void fit_text(void)
{
    if (layout.columns >= text.columns)
        return;
    if (reader >= 0)
        editor_hide(&editor, &text);
    text.columns = layout.columns;
    if (text.column > text.columns)
        text.column = text.columns;
    if (reader >= 0)
        editor_show(&editor, &text);
}

/* Take the display through resource, which covers its framebuffer, and
   draw the panel. Any failure is logged, and the console goes on without
   drawing. */
static void display(UINT32 resource, const BOOTSTRAP_DISPLAY *d)
{
    UINT64 started = now();
    UINT32 status = drawing ? ERR_NOT_SUPPORTED
                            : mapping_display(&mapping, resource, d, &screen);

    handle_close(resource);
    if (status == ERR_NOT_SUPPORTED) {
        say("nova-console: a display it cannot draw on, or a second\r\n");
        return;
    }
    if (status != OK) {
        say_status("cannot map the framebuffer", status);
        return;
    }
    if (!panel_layout(&layout, screen.width, screen.height)) {
        say("nova-console: no room for a cell on the screen\r\n");
        return;
    }
    backdrop = mapping_memory(&mapping, panel_backdrop_size(&layout), &status);
    if (backdrop)
        masks = mapping_memory(&mapping, font_masks_size(layout.cell_width),
                               &status);
    if (!backdrop || !masks) {
        say_status("no memory for the panel", status);
        return;
    }
    if (!panel_backdrop(&layout, screen.width, screen.height, backdrop)) {
        say("nova-console: no background for a screen this size\r\n");
        return;
    }
    font_render(layout.cell_width, masks);
    panel_inks(&inks);
    panel.backdrop = backdrop;
    panel.masks = masks;
    panel_show(&screen, &layout, backdrop, &shown);
    fit_text();
    drawing = 1;
    say("nova-console: drawing on the ");
    say_number(screen.width);
    say("x");
    say_number(screen.height);
    say(" screen in cells of ");
    say_number(layout.cell_width);
    say("x");
    say_number(layout.cell_height);
    say(", ");
    say_number(layout.columns);
    say(" columns by ");
    say_number(layout.rows);
    say(" lines; panel drawn in ");
    say_ms(now() - started);
    say("\r\n");
}

/* ------------------------------------------------------------------------
   Drawing. */

/* Draw the view where it differs from what the screen shows, and make
   the frame's stores visible: the panel_show of a display that came
   this round, too. */
static void render(void)
{
    UINT64 top = view_top(&text, layout.rows, back);
    VIEW_CARET caret = view_caret(reader >= 0 && caret_lit ? &editor : 0,
                                  &text, layout.rows, top);

    panel_text(&panel, &shown, &text, top, &caret);
    gfx_flush();
}

/* ------------------------------------------------------------------------
   The channels. */

/* Take the bootstrap message: the debug log, the console's process and
   its end of the control channel (console_protocol.h). */
static void take_bootstrap(void)
{
    RECEIVED_HANDLE handles[CONSOLE_BOOTSTRAP_HANDLES];
    UINT32 transaction = 1, byte_count, handle_count, observed, i;
    UINT32 status;

    for (;;) {
        status = channel_read(bootstrap, &transaction, sizeof transaction,
                              handles, CONSOLE_BOOTSTRAP_HANDLES, &byte_count,
                              &handle_count);
        if (status != ERR_SHOULD_WAIT)
            break;
        object_wait_one(bootstrap, WAKE_SIGNALS, DEADLINE_INFINITE,
                        &observed);
    }
    if (status == OK && byte_count == INPUT_CLIENT_MESSAGE_SIZE &&
        transaction == 0 && handle_count == CONSOLE_BOOTSTRAP_HANDLES &&
        handles[CONSOLE_BOOTSTRAP_DEBUG_LOG].type == TYPE_RESOURCE &&
        handles[CONSOLE_BOOTSTRAP_SELF].type == TYPE_PROCESS &&
        (handles[CONSOLE_BOOTSTRAP_SELF].rights & PROCESS_RIGHT_MAP) &&
        handles[CONSOLE_BOOTSTRAP_CONTROL].type == TYPE_CHANNEL &&
        (handles[CONSOLE_BOOTSTRAP_CONTROL].rights & CHANNEL_NEEDS) ==
            CHANNEL_NEEDS) {
        say_open(handles[CONSOLE_BOOTSTRAP_DEBUG_LOG].handle);
        mapping.self = handles[CONSOLE_BOOTSTRAP_SELF].handle;
        control = handles[CONSOLE_BOOTSTRAP_CONTROL].handle;
        return;
    }
    for (i = 0; status == OK && i < handle_count; i++)
        handle_close(handles[i].handle);
    process_exit(EXIT_FAILED);
}

/* Close input endpoint index; a key held on it as its driver ended is
   released. */
static void close_input(int index)
{
    close_if_open(&inputs[index].endpoint);
    if (inputs[index].keys)
        modifiers.held = 0;
    inputs[index].kind = 0;
    inputs[index].absolute = 0;
    inputs[index].keys = 0;
}

/* Take one message of byte_count bytes, in message, from input endpoint
   index: first the device's description, then key events from a
   keyboard, each a key typed (C3), or pointer reports from a pointer, as
   C6 gives. Returns 0 if it is none of those. */
static int take_input(int index, UINT32 byte_count)
{
    INPUT *input = &inputs[index];
    const INPUT_EVENT *event = &message.event;

    if (!input->kind) {
        if (byte_count != sizeof message.description ||
            !is_description(&message.description))
            return 0;
        input->kind = message.description.kind;
        input->absolute = message.description.absolute;
        describe(&message.description);
        return 1;
    }
    if (input->kind == INPUT_DEVICE_KEYBOARD) {
        if (byte_count != sizeof *event || event->transaction != 0 ||
            event->type != INPUT_TYPE_KEY ||
            event->value > INPUT_KEY_REPEATED)
            return 0;
        input->keys = 1;
        key(event);
        return 1;
    }
    if (byte_count != sizeof message.report ||
        !is_report(&message.report, input->absolute))
        return 0;
    pointer(&message.report);
    return 1;
}

/* Take the messages queued on input endpoint index. A message out of
   place ends the endpoint, as one the driver closed does. */
static void serve_input(int index)
{
    RECEIVED_HANDLE handles[1];
    UINT32 byte_count, handle_count, status;

    for (;;) {
        status = channel_read(inputs[index].endpoint, message.bytes,
                              sizeof message.bytes, handles, 0, &byte_count,
                              &handle_count);
        if (status == ERR_SHOULD_WAIT)
            return;
        if (status == OK && take_input(index, byte_count))
            continue;
        if (status != ERR_PEER_CLOSED)
            say_status("an input message out of place: not the device's "
                       "description first, then its key events or pointer "
                       "reports",
                       status);
        close_input(index);
        return;
    }
}

/* Take endpoint, from the platform driver, among the input endpoints.
   Those held are served first, so that one whose driver has ended gives
   its queued messages and then its place. */
static void add_input(UINT32 endpoint)
{
    int i;

    for (i = 0; i < INPUT_DEVICES_MAX; i++)
        if (inputs[i].endpoint != HANDLE_INVALID)
            serve_input(i);
    for (i = 0; i < INPUT_DEVICES_MAX; i++)
        if (inputs[i].endpoint == HANDLE_INVALID) {
            inputs[i].endpoint = endpoint;
            return;
        }
    say("nova-console: an input endpoint past the ones it reads is "
        "closed\r\n");
    handle_close(endpoint);
}

/* An input endpoint from the platform driver. The platform driver's
   close of the channel ends the console. */
static void serve_bootstrap(void)
{
    RECEIVED_HANDLE handle;
    UINT32 transaction = 1, byte_count, handle_count;
    UINT32 status = channel_read(bootstrap, &transaction, sizeof transaction,
                                 &handle, 1, &byte_count, &handle_count);

    if (status == ERR_SHOULD_WAIT)
        return;
    if (status == ERR_PEER_CLOSED) {
        say("nova-console: the platform driver closed its channel\r\n");
        process_exit(EXIT_SUPERVISOR_GONE);
    }
    if (status == OK && byte_count == INPUT_CLIENT_MESSAGE_SIZE &&
        transaction == 0 && handle_count == 1 &&
        handle.type == TYPE_CHANNEL &&
        (handle.rights & CHANNEL_NEEDS) == CHANNEL_NEEDS) {
        add_input(handle.handle);
        return;
    }
    if (status == OK && handle_count)
        handle_close(handle.handle);
    say_status("a message from the platform driver that is not an input "
               "endpoint",
               status);
    process_exit(EXIT_FAILED);
}

/* A display or a session from the first process. */
static void serve_control(void)
{
    RECEIVED_HANDLE handle;
    const CONSOLE_SETUP *setup = &message.setup.setup;
    UINT32 byte_count, handle_count;
    UINT32 status = channel_read(control, message.bytes, sizeof message.bytes,
                                 &handle, 1, &byte_count, &handle_count);

    if (status == ERR_SHOULD_WAIT)
        return;
    if (status == OK && handle_count == 1 &&
        byte_count >= CONSOLE_SETUP_SIZE && setup->transaction == 0) {
        if (setup->kind == CONSOLE_SETUP_SESSION &&
            byte_count == CONSOLE_SETUP_SIZE) {
            add_session(&handle);
            return;
        }
        if (setup->kind == CONSOLE_SETUP_DISPLAY &&
            byte_count == sizeof message.setup &&
            handle.type == TYPE_RESOURCE) {
            display(handle.handle, &message.setup.display);
            return;
        }
    }
    if (status == OK && handle_count)
        handle_close(handle.handle);
    if (status != ERR_PEER_CLOSED)
        say_status("a control message the console does not take", status);
    if (status != OK)
        close_if_open(&control);
}

/* Wait for any channel to be ready, or for the caret's blink, and serve
   what is ready. */
static void serve(void)
{
    WAIT_ENTRY entries[ENTRIES];
    UINT32 which[ENTRIES], count = 0, i, status;
    UINT64 deadline = drawing && reader >= 0 ? blink_at : DEADLINE_INFINITE;

    entries[count].handle = bootstrap;
    which[count++] = ENTRY_BOOTSTRAP;
    if (control != HANDLE_INVALID) {
        entries[count].handle = control;
        which[count++] = ENTRY_CONTROL;
    }
    for (i = 0; i < INPUT_DEVICES_MAX; i++)
        if (inputs[i].endpoint != HANDLE_INVALID) {
            entries[count].handle = inputs[i].endpoint;
            which[count++] = ENTRY_INPUTS + i;
        }
    for (i = 0; i < CONSOLE_SESSIONS; i++)
        if (sessions[i].channel != HANDLE_INVALID) {
            entries[count].handle = sessions[i].channel;
            which[count++] = ENTRY_SESSIONS + i;
        }
    for (i = 0; i < count; i++) {
        entries[i].signals = WAKE_SIGNALS;
        entries[i].observed = 0;
    }
    status = object_wait_many(entries, count, deadline);
    if (status != OK && status != ERR_TIMED_OUT) {
        say_status("cannot wait for its channels", status);
        process_exit(EXIT_FAILED);
    }
    for (i = 0; i < count; i++) {
        if (!entries[i].observed)
            continue;
        if (which[i] == ENTRY_BOOTSTRAP)
            serve_bootstrap();
        else if (which[i] == ENTRY_CONTROL)
            serve_control();
        else if (which[i] < ENTRY_SESSIONS &&
                 inputs[which[i] - ENTRY_INPUTS].endpoint == entries[i].handle)
            serve_input((int)(which[i] - ENTRY_INPUTS));
        else if (which[i] >= ENTRY_SESSIONS &&
                 sessions[which[i] - ENTRY_SESSIONS].channel ==
                     entries[i].handle)
            serve_session((int)(which[i] - ENTRY_SESSIONS));
    }
    pump();
    if (reader >= 0 && now() >= blink_at) {
        caret_lit = !caret_lit;
        blink_at = now() + CARET_BLINK_NS;
    }
    if (drawing)
        render();
}

_Noreturn void console_main(UINT32 bootstrap_channel, UINT64 argument)
{
    int i;

    (void)argument;
    bootstrap = bootstrap_channel;
    for (i = 0; i < CONSOLE_SESSIONS; i++)
        sessions[i].channel = HANDLE_INVALID;
    for (i = 0; i < INPUT_DEVICES_MAX; i++)
        inputs[i].endpoint = HANDLE_INVALID;
    take_bootstrap();
    text_init(&text, TEXT_COLUMNS_MAX);
    say("nova-console: running, waiting for the display and sessions\r\n");
    for (;;)
        serve();
}
