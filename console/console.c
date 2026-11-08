/* The console, \nova\console.exe (docs/spec/console.md): the input
   drivers' client, which the platform driver starts and hands each
   input endpoint on its bootstrap channel (C2), and the server of the
   sessions the first process sends on its control channel (C4). The
   kernel's background comes there before the display, and the console
   prepares its panel from it where it matches the display, and answers;
   otherwise it prepares the panel once the display comes (C5). With the
   display (C1) it draws the panel and, after every round of messages,
   the cells whose text changed and the caret.

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
   and the kernel's background while the backdrop is made from it, each
   mapped at the next free address from here, far above its image and
   below its stack. */
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

/* The rights the console needs on its end of a session and of the
   control channel, which it answers the background on, and on an input
   endpoint, which it only reads. */
#define SESSION_NEEDS (CHANNEL_RIGHT_READ | CHANNEL_RIGHT_WRITE | RIGHT_WAIT)
#define CONTROL_NEEDS SESSION_NEEDS
#define CHANNEL_NEEDS (CHANNEL_RIGHT_READ | RIGHT_WAIT)

/* Where the panel's backdrop came from, as the display's line says. */
#define BACKDROP_SECTION "section"
#define BACKDROP_COMPUTED "computed"

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

/* The panel, prepared for a screen of prepared_width x prepared_height
   once prepared is set: its layout, its backdrop and masks in memory of
   their sizes, its inks, where the backdrop came from and how long the
   preparation took. */
static UINT32 drawing;
static SCREEN screen;
static int prepared;
static INT64 prepared_width, prepared_height;
static const char *backdrop_source;
static UINT64 prepare_ns;
static LAYOUT layout;
static UINT32 *backdrop;
static UINT64 backdrop_size;
static UINT8 *masks;
static UINT64 masks_size;
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
    CONSOLE_SETUP_BACKGROUND_MESSAGE background;
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

/* A line for event, which ends in failed or refused, with status. */
static void say_status(const char *event, UINT32 status)
{
    say("nova-console: ");
    say(event);
    say(" status=");
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
    line_add(l, " axes=");
    if (axes == BOTH_AXES)
        line_add(l, "xy");
    else
        line_add(l, axes == INPUT_AXIS_X ? "x" : "y");
}

/* A button by its evdev name, a code past those by its number, and a
   keyboard's key by neither, since its code is what was typed. */
static void line_button(LINE *l, UINT32 code)
{
    line_add(l, " button=");
    if (code >= BTN_LEFT && code - BTN_LEFT < BUTTON_NAMES)
        line_add(l, button_names[code - BTN_LEFT]);
    else if (code >= BTN_MISC)
        line_number(l, code);
    else
        line_add(l, "key");
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
        line_add(&l, "move mode=absolute");
        line_axes(&l, detail);
        break;
    case NOTE_RELATIVE:
        line_add(&l, "move mode=relative");
        line_axes(&l, detail);
        break;
    case NOTE_PRESSED:
        line_add(&l, "press");
        line_button(&l, detail);
        break;
    case NOTE_RELEASED:
        line_add(&l, "release");
        line_button(&l, detail);
        break;
    default:
        line_add(&l, detail == WHEEL_UP ? "wheel direction=up"
                                        : "wheel direction=down");
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

static void line_range(LINE *l, const char *key, const INPUT_RANGE *r)
{
    line_add(l, key);
    line_signed(l, r->min);
    line_add(l, "..");
    line_signed(l, r->max);
}

/* Log what an endpoint's description says the device is, its axes'
   ranges included, which are the device's, not the user's. */
static void describe(const INPUT_DEVICE *d)
{
    LINE l;

    l.length = 0;
    line_add(&l, "nova-console: input kind=");
    if (d->kind == INPUT_DEVICE_KEYBOARD) {
        line_add(&l, "keyboard");
    } else {
        line_add(&l, "pointer");
        if (d->absolute & INPUT_AXIS_X)
            line_range(&l, " x=", &d->x);
        if (d->absolute & INPUT_AXIS_Y)
            line_range(&l, " y=", &d->y);
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
            say_status("session message refused", status);
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
    say("nova-console: session refused\r\n");
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

/* Give back the panel's memory, if it holds any. */
static void release_panel(void)
{
    if (backdrop)
        mapping_unmap(&mapping, backdrop, backdrop_size);
    if (masks)
        mapping_unmap(&mapping, masks, masks_size);
    backdrop = 0;
    masks = 0;
    prepared = 0;
}

/* Lay the panel out for a width x height screen, with new memory for its
   backdrop and masks in place of any it held. Returns OK;
   ERR_NOT_SUPPORTED, changing nothing, if not one cell fits; or the
   status of the call that failed, with the panel holding no memory. */
static UINT32 lay_out(INT64 width, INT64 height)
{
    UINT32 status = OK;
    LAYOUT next;

    if (!panel_layout(&next, width, height))
        return ERR_NOT_SUPPORTED;
    release_panel();
    layout = next;
    backdrop_size = panel_backdrop_size(&layout);
    masks_size = font_masks_size(layout.cell_width);
    backdrop = mapping_memory(&mapping, backdrop_size, &status);
    if (backdrop)
        masks = mapping_memory(&mapping, masks_size, &status);
    if (!masks)
        release_panel();
    return status;
}

/* With the backdrop made, render the masks and the inks, and note the
   panel prepared for a width x height screen from source, since
   started. */
static void finish_panel(INT64 width, INT64 height, const char *source,
                         UINT64 started)
{
    font_render(layout.cell_width, masks);
    panel_inks(&inks);
    panel.backdrop = backdrop;
    panel.masks = masks;
    prepared = 1;
    prepared_width = width;
    prepared_height = height;
    backdrop_source = source;
    prepare_ns = now() - started;
}

/* The bytes of band index of the kernel's background, an image of bytes
   bytes (native_abi.h). */
static UINT64 band_size(UINT64 bytes, UINT32 index)
{
    UINT64 rest = bytes - (UINT64)index * SECTION_MAX_SIZE;

    return rest < SECTION_MAX_SIZE ? rest : SECTION_MAX_SIZE;
}

/* Map the count sections of the kernel's background, an image of bytes
   bytes, to read, one after another, and return where the image starts,
   with what was mapped in *mapped; or 0, with nothing left mapped and
   the status of the call that failed in *status. */
static const UINT32 *map_image(const RECEIVED_HANDLE *handles, UINT32 count,
                               UINT64 bytes, UINT64 *mapped, UINT32 *status)
{
    const UINT8 *start = 0;
    UINT32 i;

    *mapped = 0;
    for (i = 0; i < count; i++) {
        const UINT8 *band = mapping_section(&mapping, handles[i].handle,
                                            band_size(bytes, i), MAP_READ,
                                            status);

        if (!band) {
            if (start)
                mapping_unmap(&mapping, start, *mapped);
            return 0;
        }
        if (!start)
            start = band;
        *mapped = (UINT64)(mapping.next - (UINT64)start);
    }
    return (const UINT32 *)start;
}

/* Prepare the panel's backdrop from the kernel's background, whose
   count sections are at handles, once message has shown it matches the
   display the console is to be given. Logged, with the panel left
   unprepared, if its memory or the background's mapping fails. */
static void prepare_from(const CONSOLE_SETUP_BACKGROUND_MESSAGE *m,
                        const RECEIVED_HANDLE *handles, UINT32 count,
                        UINT64 started)
{
    const BOOTSTRAP_BACKGROUND *b = &m->background;
    UINT64 bytes = (UINT64)b->width * b->height *
                   BOOTSTRAP_BACKGROUND_PIXEL_BYTES,
           mapped;
    const UINT32 *image;
    UINT32 status = lay_out(b->width, b->height);

    if (status != OK) {
        say_status("panel memory failed", status);
        return;
    }
    image = map_image(handles, count, bytes, &mapped, &status);
    if (!image) {
        release_panel();
        say_status("background map failed", status);
        return;
    }
    panel_backdrop_image(&layout, image, b->width, b->format, backdrop);
    mapping_unmap(&mapping, image, mapped);
    finish_panel(b->width, b->height, BACKDROP_SECTION, started);
}

/* Whether the count handles a background message carried are the
   sections its label's image takes, each with READ. */
static int are_sections(const BOOTSTRAP_BACKGROUND *b,
                        const RECEIVED_HANDLE *handles, UINT32 count)
{
    UINT32 i;

    if (count != bootstrap_background_sections(b->width, b->height))
        return 0;
    for (i = 0; i < count; i++)
        if (handles[i].type != TYPE_SECTION ||
            !(handles[i].rights & SECTION_RIGHT_READ))
            return 0;
    return 1;
}

/* The kernel's background, offered before the display in a message of
   byte_count bytes at m (C5): prepare the panel from it where its label
   matches the display the console is to be given, close its sections,
   and answer the first process, which waits for the answer before it
   reports its startup done. A background that does not match, or a
   message that is not one, is refused, logged, and answered too, and
   the panel is prepared once the display comes. */
static void take_background(const CONSOLE_SETUP_BACKGROUND_MESSAGE *m,
                            UINT32 byte_count, const RECEIVED_HANDLE *handles,
                            UINT32 count)
{
    UINT64 started = now();
    const char *refusal = 0;
    UINT32 transaction = 0, status, i;

    if (byte_count != sizeof *m || m->setup.transaction != 0)
        refusal = "malformed";
    else if (drawing)
        refusal = "after the display";
    else
        refusal = panel_background_refusal(&m->background, &m->display);
    if (!refusal && !are_sections(&m->background, handles, count))
        refusal = "wrong sections";
    if (refusal) {
        say("nova-console: background refused reason=");
        say_value(refusal);
        say("\r\n");
    } else {
        prepare_from(m, handles, count, started);
    }
    for (i = 0; i < count; i++)
        handle_close(handles[i].handle);
    status = channel_write(control, &transaction, CONSOLE_PREPARED_SIZE,
                           no_handles, 0);
    if (status != OK)
        say_status("background answer failed", status);
}

/* Take the display through resource, which covers its framebuffer, and
   draw the panel, prepared now if it was not prepared for a screen this
   size already. Any failure is logged, and the console goes on without
   drawing. */
static void display(UINT32 resource, const BOOTSTRAP_DISPLAY *d)
{
    UINT64 started = now();
    UINT32 status = drawing ? ERR_NOT_SUPPORTED
                            : mapping_display(&mapping, resource, d, &screen);

    handle_close(resource);
    if (status == ERR_NOT_SUPPORTED) {
        say(drawing
                ? "nova-console: display refused reason=\"second display\"\r\n"
                : "nova-console: display refused reason=unsupported\r\n");
        return;
    }
    if (status != OK) {
        say_status("framebuffer map failed", status);
        return;
    }
    if (!prepared || prepared_width != screen.width ||
        prepared_height != screen.height) {
        UINT64 preparing = now();

        status = lay_out(screen.width, screen.height);
        if (status == ERR_NOT_SUPPORTED) {
            say("nova-console: display refused reason=\"no room for a "
                "cell\"\r\n");
            return;
        }
        if (status != OK) {
            say_status("panel memory failed", status);
            return;
        }
        if (!panel_backdrop(&layout, screen.width, screen.height, backdrop)) {
            release_panel();
            say("nova-console: display refused reason=\"no background\"\r\n");
            return;
        }
        finish_panel(screen.width, screen.height, BACKDROP_COMPUTED,
                     preparing);
    }
    panel_show(&screen, &layout, backdrop, &shown);
    fit_text();
    drawing = 1;
    say("nova-console: display screen=");
    say_number(screen.width);
    say("x");
    say_number(screen.height);
    say(" cell=");
    say_number(layout.cell_width);
    say("x");
    say_number(layout.cell_height);
    say(" grid=");
    say_number(layout.columns);
    say("x");
    say_number(layout.rows);
    say(" backdrop=");
    say(backdrop_source);
    say(" prepare_ms=");
    say_ms(prepare_ns);
    say(" draw_ms=");
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
        (handles[CONSOLE_BOOTSTRAP_CONTROL].rights & CONTROL_NEEDS) ==
            CONTROL_NEEDS) {
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
            say_status("input message refused", status);
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
    say("nova-console: input refused reason=\"no room\"\r\n");
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
        say("nova-console: stopped\r\n");
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
    say_status("platform message failed", status);
    process_exit(EXIT_FAILED);
}

/* A display, a session or the kernel's background from the first
   process. */
static void serve_control(void)
{
    RECEIVED_HANDLE handles[BOOTSTRAP_BACKGROUND_SECTIONS_MAX];
    const CONSOLE_SETUP *setup = &message.setup.setup;
    UINT32 byte_count, handle_count = 0, i;
    UINT32 status = channel_read(control, message.bytes, sizeof message.bytes,
                                 handles, BOOTSTRAP_BACKGROUND_SECTIONS_MAX,
                                 &byte_count, &handle_count);

    if (status == ERR_SHOULD_WAIT)
        return;
    if (status == OK && byte_count >= CONSOLE_SETUP_SIZE &&
        setup->kind == CONSOLE_SETUP_BACKGROUND) {
        take_background(&message.background, byte_count, handles,
                        handle_count);
        return;
    }
    if (status == OK && byte_count >= CONSOLE_SETUP_SIZE &&
        setup->transaction == 0) {
        if (setup->kind == CONSOLE_SETUP_SESSION &&
            byte_count == CONSOLE_SETUP_SIZE && handle_count == 1) {
            add_session(&handles[0]);
            return;
        }
        if (setup->kind == CONSOLE_SETUP_DISPLAY &&
            byte_count == sizeof message.setup && handle_count == 1 &&
            handles[0].type == TYPE_RESOURCE) {
            display(handles[0].handle, &message.setup.display);
            return;
        }
    }
    for (i = 0; status == OK && i < handle_count; i++)
        handle_close(handles[i].handle);
    if (status != ERR_PEER_CLOSED)
        say_status("control message refused", status);
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
        say_status("wait failed", status);
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
    for (;;)
        serve();
}
