/* The console, \nova\console.exe (docs/spec/console.md): the keyboard
   driver's client, which the platform driver starts and hands each
   keyboard endpoint on its bootstrap channel (C2), and the server of
   the sessions the first process sends on its control channel (C4).
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
#include "limb.h"
#include "log.h"
#include "native.h"
#include "panel.h"
#include "text.h"
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

#define PIXEL_BYTES 4
#define TIMES_TEN_PER_MS 10

/* The objects one wait watches: the bootstrap channel, the control
   channel, the keyboard's endpoint and the sessions. */
#define ENTRY_BOOTSTRAP 0
#define ENTRY_CONTROL 1
#define ENTRY_KEYBOARD 2
#define ENTRY_SESSIONS 3
#define ENTRIES (ENTRY_SESSIONS + CONSOLE_SESSIONS)

#define WAKE_SIGNALS (CHANNEL_READABLE | CHANNEL_PEER_CLOSED)

/* The rights the console needs on its end of a session, and on the
   channels it only reads: the control channel and a keyboard's
   endpoint. */
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

/* A key typed while no read waits, kept for the next. */
typedef struct {
    UINT8 edit;
    UINT8 character;
} TYPED;

/* Where the caret was last drawn, and whether it was. */
typedef struct {
    UINT64 row;
    UINT32 column;
    UINT32 lit;
} CARET;

/* channel_write needs a valid array even for no handles. */
static const HANDLE_TRANSFER no_handles[1];

static UINT32 self = HANDLE_INVALID;
static UINT32 bootstrap = HANDLE_INVALID;
static UINT32 control = HANDLE_INVALID;
static UINT32 keyboard = HANDLE_INVALID;
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
static UINT8 shown[PANEL_ROWS][TEXT_COLUMNS_MAX];
static CARET shown_caret;
static UINT32 caret_lit;
static UINT64 blink_at;
static UINT64 next_address = MAPPINGS_BASE;

static union {
    CONSOLE_REQUEST request;
    INPUT_EVENT event;
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

/* The lines the view can scroll back: to the oldest kept, at most. */
static UINT64 back_limit(void)
{
    UINT64 rows = drawing ? layout.rows : PANEL_ROWS;
    UINT64 end = text.lines > rows ? text.lines - rows : 0;

    return end > text_first(&text) ? end - text_first(&text) : 0;
}

static void scroll(UINT32 edit)
{
    UINT64 step = (drawing ? layout.rows : PANEL_ROWS) / 2;

    if (edit == EDIT_PAGE_UP)
        back = back + step < back_limit() ? back + step : back_limit();
    else
        back = back > step ? back - step : 0;
}

static void key(const INPUT_EVENT *event)
{
    UINT8 character;
    UINT32 edit = keys_event(&modifiers, event->code, event->value,
                             &character);

    if (edit == EDIT_NONE)
        return;
    if (edit == EDIT_PAGE_UP || edit == EDIT_PAGE_DOWN) {
        scroll(edit);
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

/* size bytes of new zeroed memory, mapped read-write at the next free
   address, in sections of at most SECTION_MAX_SIZE; 0 if memory runs
   out. A failure leaves what it mapped, which the console then never
   uses. */
static void *allocate(UINT64 size)
{
    UINT64 start = next_address, offset, piece;
    UINT32 section, status;

    for (offset = 0; offset < size; offset += piece) {
        piece = size - offset < SECTION_MAX_SIZE ? size - offset
                                                 : SECTION_MAX_SIZE;
        piece = (piece + PAGE_SIZE - 1) / PAGE_SIZE * PAGE_SIZE;
        status = section_create(piece, 0, &section);
        if (status == OK) {
            status = process_map(self, section, 0, next_address, piece,
                                 MAP_READ | MAP_WRITE);
            handle_close(section);
        }
        if (status != OK) {
            say_status("no memory for the panel", status);
            return 0;
        }
        next_address += piece;
    }
    return (void *)start;
}

/* Map the framebuffer's pages, from first up to end, write-combining,
   through resource; returns where first lies, or 0. */
static UINT8 *map_framebuffer(UINT32 resource, UINT64 first, UINT64 end)
{
    UINT64 start = next_address, offset, piece;
    UINT32 section, status;

    for (offset = first; offset < end; offset += piece) {
        piece = end - offset < SECTION_MAX_SIZE ? end - offset
                                                : SECTION_MAX_SIZE;
        status = section_create_physical(resource, offset, piece,
                                         CACHE_WRITE_COMBINING, &section);
        if (status == OK) {
            status = process_map(self, section, 0, next_address, piece,
                                 MAP_READ | MAP_WRITE);
            handle_close(section);
        }
        if (status != OK) {
            say_status("cannot map the framebuffer", status);
            return 0;
        }
        next_address += piece;
    }
    return (UINT8 *)start;
}

static void say_ms(UINT64 nanoseconds)
{
    UINT64 tenths = nanoseconds * TIMES_TEN_PER_MS / NS_PER_MS;

    say_number(tenths / TIMES_TEN_PER_MS);
    say(".");
    say_number(tenths % TIMES_TEN_PER_MS);
    say(" ms");
}

/* Whether the display record describes a screen the console draws on:
   one with a pixel format it knows, a side Limb draws, rows no shorter
   than the screen is wide, and every pixel within the address space. */
static int drawable(const BOOTSTRAP_DISPLAY *d)
{
    return (d->format == PIXEL_RGB || d->format == PIXEL_BGR) &&
           d->width >= LIMB_MIN_SIDE && d->height >= LIMB_MIN_SIDE &&
           d->width <= LIMB_MAX_SIDE && d->height <= LIMB_MAX_SIDE &&
           d->stride >= d->width && d->stride <= LIMB_MAX_SIDE &&
           d->base % PIXEL_BYTES == 0 &&
           d->base <= ~0ULL - (UINT64)d->stride * d->height * PIXEL_BYTES -
                          PAGE_SIZE;
}

/* The panel as panel_show leaves it: every cell blank, and no caret. */
static void blank_shown(void)
{
    UINT32 row, column;

    for (row = 0; row < layout.rows; row++)
        for (column = 0; column < layout.columns; column++)
            shown[row][column] = ' ';
    shown_caret.lit = 0;
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
    UINT64 first = d->base - d->base % PAGE_SIZE;
    UINT64 end = (d->base + (UINT64)d->stride * d->height * PIXEL_BYTES +
                  PAGE_SIZE - 1) /
                 PAGE_SIZE * PAGE_SIZE;
    UINT64 started = now();
    UINT8 *mapped;

    if (drawing || !drawable(d)) {
        say("nova-console: a display it cannot draw on, or a second\r\n");
        handle_close(resource);
        return;
    }
    mapped = map_framebuffer(resource, first, end);
    handle_close(resource);
    if (!mapped)
        return;
    screen.pixels = (UINT32 *)(mapped + (d->base - first));
    screen.width = d->width;
    screen.height = d->height;
    screen.stride = d->stride;
    screen.format = d->format;
    if (!panel_layout(&layout, screen.width, screen.height)) {
        say("nova-console: no room for a cell on the screen\r\n");
        return;
    }
    backdrop = allocate(panel_backdrop_size(&layout));
    masks = allocate(font_masks_size(layout.cell_width));
    if (!backdrop || !masks)
        return;
    if (!panel_backdrop(&layout, screen.width, screen.height, backdrop)) {
        say("nova-console: no background for a screen this size\r\n");
        return;
    }
    font_render(layout.cell_width, masks);
    panel_inks(&inks);
    panel_show(&screen, &layout, backdrop);
    blank_shown();
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

/* The first line the view shows. */
static UINT64 view_top(void)
{
    UINT64 end = text.lines > layout.rows ? text.lines - layout.rows : 0;

    return end - (back < back_limit() ? back : back_limit());
}

/* Where the caret is to be drawn, if it is: while a read waits and the
   blink shows it, at its place in the view. */
static CARET caret_wanted(UINT64 top)
{
    CARET c;
    UINT64 line;

    c.row = 0;
    c.column = 0;
    c.lit = 0;
    if (reader < 0 || !caret_lit)
        return c;
    editor_caret(&editor, &text, &line, &c.column);
    if (line < top || line - top >= layout.rows)
        return c;
    c.row = line - top;
    c.lit = 1;
    return c;
}

static int at(const CARET *c, UINT64 row, UINT32 column)
{
    return c->lit && c->row == row && c->column == column;
}

/* The cell at row and column, the column up to layout.columns, which
   lies in the padding and holds no text. */
static void draw(UINT64 row, UINT32 column, UINT8 character, int caret)
{
    panel_cell(&screen, &layout, backdrop, &inks,
               font_mask(masks, layout.cell_width, character),
               caret ? font_mask(masks, layout.cell_width, FONT_CARET) : 0,
               (UINT32)row, column);
}

/* Draw every cell whose text differs from what the screen shows, and
   the cells the caret leaves and enters. */
static void render(void)
{
    UINT64 top = view_top(), row;
    CARET want = caret_wanted(top);
    int moved = want.lit != shown_caret.lit || want.row != shown_caret.row ||
                want.column != shown_caret.column;
    UINT32 column;

    for (row = 0; row < layout.rows; row++) {
        UINT64 line = top + row;
        const UINT8 *cells = line < text.lines ? text_line(&text, line) : 0;

        for (column = 0; column < layout.columns; column++) {
            UINT8 character = cells ? cells[column] : (UINT8)' ';

            if (character != shown[row][column] ||
                (moved && (at(&want, row, column) ||
                           at(&shown_caret, row, column))))
                draw(row, column, character, at(&want, row, column));
            shown[row][column] = character;
        }
    }
    if (moved && at(&shown_caret, shown_caret.row, layout.columns))
        draw(shown_caret.row, layout.columns, ' ', 0);
    if (moved && at(&want, want.row, layout.columns))
        draw(want.row, layout.columns, ' ', 1);
    shown_caret = want;
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
        self = handles[CONSOLE_BOOTSTRAP_SELF].handle;
        control = handles[CONSOLE_BOOTSTRAP_CONTROL].handle;
        return;
    }
    for (i = 0; status == OK && i < handle_count; i++)
        handle_close(handles[i].handle);
    process_exit(EXIT_FAILED);
}

/* Take the key events queued on the keyboard's endpoint. A message that
   is not one ends the endpoint, as one the driver closed does. */
static void serve_keyboard(void)
{
    RECEIVED_HANDLE handles[1];
    const INPUT_EVENT *event = &message.event;
    UINT32 byte_count, handle_count, status;

    for (;;) {
        status = channel_read(keyboard, message.bytes, sizeof message.bytes,
                              handles, 0, &byte_count, &handle_count);
        if (status == ERR_SHOULD_WAIT)
            return;
        if (status == OK && byte_count == sizeof *event &&
            event->transaction == 0 && event->type == INPUT_TYPE_KEY &&
            event->value <= INPUT_KEY_REPEATED) {
            key(event);
            continue;
        }
        if (status != ERR_PEER_CLOSED)
            say_status("a keyboard message that is not a key event", status);
        close_if_open(&keyboard);
        return;
    }
}

/* A keyboard endpoint from the platform driver, in place of the one
   before, whose queued keys are taken first; a key held as the old
   driver ended is released. The platform driver's close of the channel
   ends the console. */
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
        if (keyboard != HANDLE_INVALID)
            serve_keyboard();
        close_if_open(&keyboard);
        keyboard = handle.handle;
        modifiers.held = 0;
        return;
    }
    if (status == OK && handle_count)
        handle_close(handle.handle);
    say_status("a message from the platform driver that is not a keyboard "
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
    if (keyboard != HANDLE_INVALID) {
        entries[count].handle = keyboard;
        which[count++] = ENTRY_KEYBOARD;
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
        else if (which[i] == ENTRY_KEYBOARD)
            serve_keyboard();
        else if (sessions[which[i] - ENTRY_SESSIONS].channel ==
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
    take_bootstrap();
    text_init(&text, TEXT_COLUMNS_MAX);
    say("nova-console: running, waiting for the display and sessions\r\n");
    for (;;)
        serve();
}
