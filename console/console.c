/* The console, \nova\console.exe (docs/spec/console.md): the server of
   the sessions the first process sends on its control channel (C4), in
   one of two modes. In window mode, while a compositor runs, the first
   process sends it a display connection, on which it makes a window and
   draws its panel's pixels (pane.c); it takes its keys and the wheel from
   the window and holds no input endpoint (C2). In direct mode it is the
   input drivers' client, which the platform driver starts and hands each
   input endpoint on its bootstrap channel (C2): the kernel's background
   comes on the control channel before the display, and the console
   prepares its panel from it where it matches the display, and answers;
   otherwise it prepares the panel once the display comes (C5). With the
   display (C1) it draws the panel and, after every round of messages,
   the cells whose text changed and the caret. The display after a
   compositor, which ends window mode, comes the same way.

   One thread waits on every channel at once with object_wait_many, with
   a deadline at the caret's next blink while a read waits on a drawn
   panel and the options do not hold it still, and serves each channel
   that is ready; a session is served at most its queue's worth of
   messages a round, so a busy one cannot keep the keys from the
   others. */

#include "console_protocol.h"
#include "endpoint.h"
#include "font.h"
#include "input_protocol.h"
#include "keymap.h"
#include "log.h"
#include "mapping.h"
#include "native.h"
#include "pane.h"
#include "panel.h"
#include "text.h"
#include "timing.h"
#include "view.h"
#include "wipe.h"

/* It ends with EXIT_SUPERVISOR_GONE once the platform driver has closed
   its bootstrap channel, else with EXIT_FAILED. Both lie above the
   exception vectors a faulting process ends with (objects.md O10). */
#define EXIT_SUPERVISOR_GONE 100
#define EXIT_FAILED 101

/* The component the input and pointer lines begin with (log.md T1). */
#define LOG_COMPONENT "nova-console"

/* Its own memory: the framebuffer's mapping, the backdrop and the masks,
   and the kernel's background while the backdrop is made from it, each
   mapped at the next free address from here, far above its image and
   below its stack. */
#define MAPPINGS_BASE 0x0000000100000000ULL

/* The caret shows for half a second, then hides for as long, while a
   read waits; a key shows it again at once. With the option
   CONSOLE_OPTION_REDUCE_MOTION it stays shown. */
#define CARET_BLINK_NS (500 * TIMING_NS_PER_MS)

/* The objects one wait watches: the bootstrap channel, the control
   channel, the window's connection and the window, the input endpoints
   and the sessions. */
#define ENTRY_BOOTSTRAP 0
#define ENTRY_CONTROL 1
#define ENTRY_CONNECTION 2
#define ENTRY_WINDOW 3
#define ENTRY_INPUTS 4
#define ENTRY_SESSIONS (ENTRY_INPUTS + INPUT_DEVICES_MAX)
#define ENTRIES (ENTRY_SESSIONS + CONSOLE_SESSIONS)

#define WAKE_SIGNALS (CHANNEL_READABLE | CHANNEL_PEER_CLOSED)

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

/* An input driver's endpoint, HANDLE_INVALID for a free slot; the device
   its description gave, whose kind is 0 until it has come; and whether a
   key event has come on it. */
typedef struct {
    UINT32 endpoint;
    INPUT_DEVICE device;
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
static UINT32 reduce_motion;
static MAPPING mapping = {HANDLE_INVALID, MAPPINGS_BASE};
static PANE pane;

static union {
    CONSOLE_REQUEST request;
    INPUT_EVENT event;
    INPUT_POINTER report;
    CONSOLE_SETUP_DISPLAY_MESSAGE setup;
    CONSOLE_SETUP_BACKGROUND_MESSAGE background;
    CONSOLE_SETUP_OPTIONS_MESSAGE options;
    UINT8 bytes[sizeof(CONSOLE_REQUEST) + CONSOLE_WRITE_MAX];
} message;

/* A read's reply, wiped once written, since a line read without echo is
   a secret. It is the buffer the generated encoding writes, aligned to 8
   bytes (docs/spec/interface.md P14). */
static union {
    UINT64 alignment;
    CONSOLE_REPLY reply;
    UINT8 bytes[CONSOLE_SESSION_TO_CLIENT_BYTES_MAX];
} answer;

/* The answer to a background, aligned the same way. */
static union {
    UINT64 alignment;
    UINT8 bytes[CONSOLE_CONTROL_TO_CLIENT_BYTES_MAX];
} background_answer;

/* Show the caret for CARET_BLINK_NS more. */
static void caret_show(void)
{
    caret_lit = 1;
    blink_at = timing_now() + CARET_BLINK_NS;
}

/* Whether the caret is on a timer: it blinks while a read waits, unless
   the options hold it still. */
static int caret_blinks(void)
{
    return reader >= 0 && !reduce_motion;
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
    caret_show();
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
    UINT32 size, status;

    answer.reply.transaction = session->transaction;
    answer.reply.length = 0;
    status = console_session_line_encode(&answer.reply, editor.bytes,
                                         editor.length, answer.bytes, &size);
    if (status == OK)
        status = channel_write(session->channel, answer.bytes, size,
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

/* The lines the view shows: the panel's, in a window or drawn directly,
   or before either comes the most it can. */
static UINT32 view_rows(void)
{
    if (pane_laid_out(&pane))
        return pane.layout.rows;
    return drawing ? layout.rows : PANEL_ROWS;
}

/* Scroll the view lines back, or forward for a negative lines. */
static void scroll(INT64 lines)
{
    back = view_scroll(&text, view_rows(), back, lines);
}

/* Act on an edit and its character, as the keys of either mode make them
   (keymap.h): scroll, or edit the line a read waits for, or keep the key
   for the next read. */
static void edit_key(UINT32 edit, UINT8 character)
{
    if (edit == EDIT_NONE)
        return;
    if (edit == EDIT_PAGE_UP || edit == EDIT_PAGE_DOWN) {
        scroll(edit == EDIT_PAGE_UP ? view_rows() / 2
                                    : -(INT64)(view_rows() / 2));
        return;
    }
    back = 0;
    caret_show();
    if (reader >= 0) {
        apply(edit, character);
    } else if (typed_count < CONSOLE_LINE_MAX) {
        typed[typed_count].edit = (UINT8)edit;
        typed[typed_count].character = character;
        typed_count++;
    }
}

static void key(const INPUT_EVENT *event)
{
    UINT8 character;
    UINT32 edit = keys_event(&modifiers, event->code, event->value,
                             &character);

    edit_key(edit, character);
}

/* ------------------------------------------------------------------------
   The pointer's lines (C6). */

/* The pointer line logged last. */
static ENDPOINT_NOTES pointer_notes;

/* Log what a pointer report does, and nothing of where the pointer is
   or how far it moved. A turn of the wheel also scrolls the view
   VIEW_WHEEL_LINES a detent (C3): a turn up, a positive wheel (C6), back
   through what the panel has shown, as Page Up does, and a turn down
   forward. */
static void pointer(const INPUT_POINTER *report)
{
    endpoint_pointer(&pointer_notes, LOG_COMPONENT, report, say);
    if (report->wheel)
        scroll((INT64)VIEW_WHEEL_LINES * report->wheel);
}

/* ------------------------------------------------------------------------
   Sessions. */

/* Act on the request in message, byte_count bytes long and carrying
   handle_count handles, from session index. Returns 0 if it is not one
   the protocol allows, or is a read while the session's earlier read
   waits, which ends the session (C4). */
static int request(int index, UINT32 byte_count, const RECEIVED_HANDLE *handles,
                   UINT32 handle_count)
{
    const CONSOLE_REQUEST *r = &message.request;
    SESSION *session = &sessions[index];
    UINT32 which;

    if (console_session_request_check(message.bytes, byte_count, handles,
                                      handle_count, &which) != OK)
        return 0;
    if (which == CONSOLE_WRITE) {
        output(console_session_write_text(message.bytes), r->length);
        return 1;
    }
    if (session->reading)
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
        if (status == OK &&
            request(index, byte_count, handles, handle_count)) {
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

/* Serve the session whose end handle is, a handle the control channel's
   check accepted; refused, logged and closed if all CONSOLE_SESSIONS
   places are taken. */
static void add_session(const RECEIVED_HANDLE *handle)
{
    int i;

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

/* Fit the text to the width of the panel laid out as target: its lines
   wrap at the narrower of the two from now on. */
static void fit_text(const LAYOUT *target)
{
    if (target->columns >= text.columns)
        return;
    if (reader >= 0)
        editor_hide(&editor, &text);
    text.columns = target->columns;
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
    prepare_ns = timing_now() - started;
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
    image = mapping_image(&mapping, handles, count, bytes, &mapped, &status);
    if (!image) {
        release_panel();
        say_status("background map failed", status);
        return;
    }
    panel_backdrop_image(&layout, image, b->width, b->format, backdrop);
    mapping_unmap(&mapping, image, mapped);
    finish_panel(b->width, b->height, BACKDROP_SECTION, started);
}

/* Answer the first process's background, which carried transaction,
   with the control channel's reply: the panel is prepared or it is
   refused. */
static void answer_prepared(UINT32 transaction)
{
    UINT32 size, status = console_control_prepared_encode(
                    transaction, background_answer.bytes, &size);

    if (status == OK)
        status = channel_write(control, background_answer.bytes, size,
                               no_handles, 0);
    if (status != OK)
        say_status("background answer failed", status);
}

/* The kernel's background, offered before the display in the message at m
   (C5), whose check passed: prepare the panel from it where its label
   matches the display the console is to be given, close its sections,
   and answer the first process, which waits for the answer before it
   reports its startup done. A background that does not match, or that
   holds other sections than its label's image takes, is refused,
   logged, and answered too, and the panel is prepared once the display
   comes. */
static void take_background(const CONSOLE_SETUP_BACKGROUND_MESSAGE *m,
                            const RECEIVED_HANDLE *handles, UINT32 count)
{
    UINT64 started = timing_now();
    const char *refusal = 0;
    UINT32 i;

    if (drawing)
        refusal = "after the display";
    else
        refusal = panel_background_refusal(&m->background, &m->display);
    if (!refusal &&
        count != bootstrap_background_sections(m->background.width,
                                               m->background.height))
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
    answer_prepared(m->setup.transaction);
}

/* Who held the display before the console and what the screen shows: the
   kernel, which left Limb, under which the panel is all there is to
   draw, or a compositor, whose last frame stays until Limb is drawn over
   it (C1, C2). */
typedef enum { HANDOFF_FROM_KERNEL, HANDOFF_FROM_COMPOSITOR } HANDOFF;

/* Take the display through resource, which covers its framebuffer, and
   draw the panel, prepared now if it was not prepared for a screen this
   size already, after Limb over the whole screen where the compositor
   drew before. Any failure is logged, and the console goes on without
   drawing. */
static void display(UINT32 resource, const BOOTSTRAP_DISPLAY *d,
                    HANDOFF handoff)
{
    UINT64 started = timing_now();
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
        UINT64 preparing = timing_now();

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
    if (handoff == HANDOFF_FROM_COMPOSITOR && !panel_limb(&screen)) {
        say("nova-console: display refused reason=\"no background\"\r\n");
        return;
    }
    panel_show(&screen, &layout, backdrop, &shown);
    fit_text(&layout);
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
    timing_say_ms(prepare_ns);
    say(" draw_ms=");
    timing_say_ms(timing_now() - started);
    if (handoff == HANDOFF_FROM_COMPOSITOR)
        say(" after=compositor");
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

/* Draw the view in the window, which presents it when the compositor is
   ready for a frame. */
static void render_window(void)
{
    UINT64 top = view_top(&text, pane.layout.rows, back);
    VIEW_CARET caret = view_caret(reader >= 0 && caret_lit ? &editor : 0,
                                  &text, pane.layout.rows, top);

    pane_show(&pane, &text, top, &caret);
}

/* ------------------------------------------------------------------------
   The channels. */

/* Take the bootstrap message: the debug log, the console's process and
   its end of the control channel (console_protocol.h). */
static void take_bootstrap(void)
{
    RECEIVED_HANDLE handles[CONSOLE_BOOTSTRAP_HANDLES];
    UINT32 byte_count, handle_count, observed, i;
    UINT32 status;

    for (;;) {
        status = channel_read(bootstrap, message.bytes, sizeof message.bytes,
                              handles, CONSOLE_BOOTSTRAP_HANDLES, &byte_count,
                              &handle_count);
        if (status != ERR_SHOULD_WAIT)
            break;
        object_wait_one(bootstrap, WAKE_SIGNALS, DEADLINE_INFINITE,
                        &observed);
    }
    if (status == OK &&
        console_bootstrap_start_check(message.bytes, byte_count, handles,
                                      handle_count) == OK) {
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
    inputs[index].device.kind = 0;
    inputs[index].keys = 0;
}

/* Take one message of byte_count bytes, in message, from input endpoint
   index: first the device's description, then key events from a
   keyboard, each a key typed (C3), or pointer reports from a pointer, as
   C6 gives. Returns 0 if it is none of those. */
static int take_input(int index, UINT32 byte_count)
{
    INPUT *input = &inputs[index];

    switch (endpoint_take(&input->device, message.bytes, byte_count)) {
    case ENDPOINT_DESCRIPTION:
        endpoint_describe(LOG_COMPONENT, &input->device, say);
        return 1;
    case ENDPOINT_KEY:
        input->keys = 1;
        key(&message.event);
        return 1;
    case ENDPOINT_REPORT:
        pointer(&message.report);
        return 1;
    default:
        return 0;
    }
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
    UINT32 byte_count, handle_count;
    UINT32 status = channel_read(bootstrap, message.bytes,
                                 sizeof message.bytes, &handle, 1,
                                 &byte_count, &handle_count);

    if (status == ERR_SHOULD_WAIT)
        return;
    if (status == ERR_PEER_CLOSED) {
        say("nova-console: stopped\r\n");
        process_exit(EXIT_SUPERVISOR_GONE);
    }
    if (status == OK &&
        console_bootstrap_endpoint_check(message.bytes, byte_count, &handle,
                                         handle_count) == OK) {
        add_input(handle.handle);
        return;
    }
    if (status == OK && handle_count)
        handle_close(handle.handle);
    say_status("platform message failed", status);
    process_exit(EXIT_FAILED);
}

/* Take the options the first process sends (console.md C2): with
   CONSOLE_OPTION_REDUCE_MOTION the caret stays lit. */
static void set_options(UINT32 options)
{
    reduce_motion = (options & CONSOLE_OPTION_REDUCE_MOTION) != 0;
    caret_show();
}

/* Take a display connection, on which the console makes its window while
   a compositor runs (C2), in place of the one it held. A console that
   holds the display itself has no use for one. */
static void take_connection(const RECEIVED_HANDLE *handle)
{
    if (drawing) {
        say("nova-console: connection refused reason=\"display held\"\r\n");
        handle_close(handle->handle);
        return;
    }
    pane_adopt(&pane, handle->handle);
}

/* The display after a compositor: the console leaves window mode, which
   the compositor's end has made of no use, and draws the panel itself. */
static void take_display_after(const RECEIVED_HANDLE *resource,
                               const BOOTSTRAP_DISPLAY *d)
{
    pane_drop(&pane);
    display(resource->handle, d, HANDOFF_FROM_COMPOSITOR);
}

/* The options, a display, the display after a compositor, a connection, a
   session or the kernel's background from the first process. A message the
   description does not allow is dropped, with the handles it carried
   closed; a background that has a reply is answered with the request's
   transaction alone, so the first process does not wait for it
   (docs/spec/interface.md P10). */
static void serve_control(void)
{
    RECEIVED_HANDLE handles[BOOTSTRAP_BACKGROUND_SECTIONS_MAX];
    UINT32 byte_count, handle_count = 0, which = CONSOLE_CONTROL_UNNAMED, i;
    UINT32 read_status = channel_read(control, message.bytes,
                                      sizeof message.bytes, handles,
                                      BOOTSTRAP_BACKGROUND_SECTIONS_MAX,
                                      &byte_count, &handle_count);
    UINT32 status = read_status;

    if (read_status == ERR_SHOULD_WAIT)
        return;
    if (read_status == OK)
        status = console_control_request_check(message.bytes, byte_count,
                                               handles, handle_count, &which);
    if (status == OK) {
        if (which == CONSOLE_SETUP_BACKGROUND)
            take_background(&message.background, handles, handle_count);
        else if (which == CONSOLE_SETUP_SESSION)
            add_session(&handles[0]);
        else if (which == CONSOLE_SETUP_OPTIONS)
            set_options(message.options.options);
        else if (which == CONSOLE_SETUP_CONNECTION)
            take_connection(&handles[0]);
        else if (which == CONSOLE_SETUP_DISPLAY_AFTER)
            take_display_after(&handles[0], &message.setup.display);
        else
            display(handles[0].handle, &message.setup.display,
                    HANDOFF_FROM_KERNEL);
        return;
    }
    if (which == CONSOLE_SETUP_BACKGROUND) {
        say("nova-console: background refused reason=malformed\r\n");
        answer_prepared(message.setup.setup.transaction);
    }
    for (i = 0; read_status == OK && i < handle_count; i++)
        handle_close(handles[i].handle);
    if (read_status != ERR_PEER_CLOSED)
        say_status("control message refused", status);
    if (read_status != OK)
        close_if_open(&control);
}

/* Serve the window's connection, then the window: the answer to its
   request comes on the one before the first configure on the other, so
   the connection is read first. Each message of the window is a key or a
   turn of the wheel to act on, or one the pane has acted on; at most the
   window's queue's worth are served a round. */
static void serve_pane(void)
{
    PANE_INPUT input;
    UINT32 served;

    pane_serve_connection(&pane);
    for (served = 0; served < WINDOW_CAPACITY && pane_input(&pane, &input);
         served++) {
        edit_key(input.edit, input.character);
        if (input.lines)
            scroll(input.lines);
    }
    if (pane_laid_out(&pane))
        fit_text(&pane.layout);
}

/* Wait for any channel to be ready, or for the caret's blink, and serve
   what is ready. */
static void serve(void)
{
    WAIT_ENTRY entries[ENTRIES];
    UINT32 which[ENTRIES], count = 0, i, status;
    UINT64 deadline = (drawing || pane_laid_out(&pane)) && caret_blinks()
                          ? blink_at
                          : DEADLINE_INFINITE;

    entries[count].handle = bootstrap;
    which[count++] = ENTRY_BOOTSTRAP;
    if (control != HANDLE_INVALID) {
        entries[count].handle = control;
        which[count++] = ENTRY_CONTROL;
    }
    if (pane.connection != HANDLE_INVALID) {
        entries[count].handle = pane.connection;
        which[count++] = ENTRY_CONNECTION;
    }
    if (pane.window != HANDLE_INVALID) {
        entries[count].handle = pane.window;
        which[count++] = ENTRY_WINDOW;
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
        else if (which[i] == ENTRY_CONNECTION || which[i] == ENTRY_WINDOW)
            serve_pane();
        else if (which[i] >= ENTRY_INPUTS && which[i] < ENTRY_SESSIONS &&
                 inputs[which[i] - ENTRY_INPUTS].endpoint == entries[i].handle)
            serve_input((int)(which[i] - ENTRY_INPUTS));
        else if (which[i] >= ENTRY_SESSIONS &&
                 sessions[which[i] - ENTRY_SESSIONS].channel ==
                     entries[i].handle)
            serve_session((int)(which[i] - ENTRY_SESSIONS));
    }
    pump();
    if (caret_blinks() && timing_now() >= blink_at) {
        caret_lit = !caret_lit;
        blink_at = timing_now() + CARET_BLINK_NS;
    }
    if (drawing)
        render();
    else if (pane.phase == PANE_SHOWING)
        render_window();
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
    pane_init(&pane, &mapping);
    take_bootstrap();
    text_init(&text, TEXT_COLUMNS_MAX);
    for (;;)
        serve();
}
