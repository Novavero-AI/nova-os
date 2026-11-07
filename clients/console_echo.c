/* The console's demo client, \nova\conecho.exe, which the first process
   starts on the console at the end of startup (docs/spec/console.md C2):
   it writes a greeting, then reads lines and writes each back, logging
   each line's length but never its bytes, and reads a line without echo
   when asked to. It ends once the console closes its session. */

#include "console_protocol.h"
#include "log.h"
#include "native.h"

/* It ends with EXIT_SESSION_CLOSED once the console has closed its
   session, else with EXIT_FAILED. Both lie above the exception vectors
   a faulting process ends with (docs/spec/objects.md O10). */
#define EXIT_SESSION_CLOSED 100
#define EXIT_FAILED 101

#define WAKE_SIGNALS (CHANNEL_READABLE | CHANNEL_PEER_CLOSED)

/* The line that asks for a read without echo. */
#define SECRET_WORD "secret"

static const char greeting[] =
    "nova-os\n"
    "\n"
    "This is the console's demo client. Type a line and press Enter, and\n"
    "it comes back. Type " SECRET_WORD " to type a line the console does "
    "not show.\n"
    "\n";

/* channel_write needs a valid array even for no handles. */
static const HANDLE_TRANSFER no_handles[1];

static UINT32 session = HANDLE_INVALID;

static struct {
    CONSOLE_REQUEST request;
    UINT8 bytes[CONSOLE_WRITE_MAX];
} out;

static struct {
    CONSOLE_REPLY reply;
    UINT8 line[CONSOLE_LINE_MAX];
} in;

static void end(UINT32 status)
{
    if (status == ERR_PEER_CLOSED)
        process_exit(EXIT_SESSION_CLOSED);
    say("nova-console-echo: the session failed, status ");
    say_number(status);
    say("\r\n");
    process_exit(EXIT_FAILED);
}

/* Write the request in out with length bytes after it, waiting while the
   console's queue is full. */
static void send(UINT32 length)
{
    UINT32 status, observed;

    for (;;) {
        status = channel_write(session, &out,
                               (UINT32)sizeof out.request + length,
                               no_handles, 0);
        if (status != ERR_SHOULD_WAIT)
            break;
        status = object_wait_one(session,
                                 CHANNEL_WRITABLE | CHANNEL_PEER_CLOSED,
                                 DEADLINE_INFINITE, &observed);
        if (status != OK)
            break;
    }
    if (status != OK)
        end(status);
}

static void write_bytes(const UINT8 *bytes, UINT32 count)
{
    UINT32 i;

    while (count) {
        UINT32 piece = count < CONSOLE_WRITE_MAX ? count : CONSOLE_WRITE_MAX;

        out.request.transaction = 0;
        out.request.operation = CONSOLE_WRITE;
        out.request.length = piece;
        out.request.flags = 0;
        for (i = 0; i < piece; i++)
            out.bytes[i] = bytes[i];
        send(piece);
        bytes += piece;
        count -= piece;
    }
}

static void write_text(const char *text)
{
    UINT32 count = 0;

    while (text[count])
        count++;
    write_bytes((const UINT8 *)text, count);
}

/* Read a line into in, with flags; returns its length. */
static UINT32 read_line(UINT32 flags)
{
    UINT32 byte_count, handle_count, observed, status;
    RECEIVED_HANDLE handles[1];

    out.request.transaction = 1;
    out.request.operation = CONSOLE_READ;
    out.request.length = CONSOLE_LINE_MAX;
    out.request.flags = flags;
    send(0);
    for (;;) {
        status = channel_read(session, &in, sizeof in, handles, 0,
                              &byte_count, &handle_count);
        if (status != ERR_SHOULD_WAIT)
            break;
        status = object_wait_one(session, WAKE_SIGNALS, DEADLINE_INFINITE,
                                 &observed);
        if (status != OK)
            end(status);
    }
    if (status != OK)
        end(status);
    if (byte_count < sizeof in.reply || in.reply.transaction != 1 ||
        in.reply.length > CONSOLE_LINE_MAX ||
        byte_count != sizeof in.reply + in.reply.length)
        end(ERR_BAD_STATE);
    return in.reply.length;
}

static int is_secret_word(UINT32 length)
{
    static const char word[] = SECRET_WORD;
    UINT32 i;

    if (length != sizeof word - 1)
        return 0;
    for (i = 0; i < length; i++)
        if (in.line[i] != (UINT8)word[i])
            return 0;
    return 1;
}

/* Read a line without echo, and wipe it unseen. */
static void secret(void)
{
    UINT32 length, i;

    write_text("Type a line; it stays off the screen: ");
    length = read_line(CONSOLE_READ_NO_ECHO);
    for (i = 0; i < length; i++)
        in.line[i] = 0;
    write_text("\nThe console read the line and showed none of it.\n");
}

_Noreturn void console_echo_main(UINT32 bootstrap, UINT64 argument)
{
    RECEIVED_HANDLE handles[CONSOLE_PROGRAM_HANDLES];
    UINT32 transaction = 1, byte_count, handle_count, observed, length;
    UINT32 status;

    (void)argument;
    for (;;) {
        status = channel_read(bootstrap, &transaction, sizeof transaction,
                              handles, CONSOLE_PROGRAM_HANDLES, &byte_count,
                              &handle_count);
        if (status != ERR_SHOULD_WAIT)
            break;
        object_wait_one(bootstrap, WAKE_SIGNALS, DEADLINE_INFINITE,
                        &observed);
    }
    if (status != OK || byte_count != CONSOLE_PROGRAM_MESSAGE_SIZE ||
        transaction != 0 || handle_count != CONSOLE_PROGRAM_HANDLES ||
        handles[CONSOLE_PROGRAM_DEBUG_LOG].type != TYPE_RESOURCE ||
        handles[CONSOLE_PROGRAM_SESSION].type != TYPE_CHANNEL)
        process_exit(EXIT_FAILED);
    handle_close(bootstrap);
    say_open(handles[CONSOLE_PROGRAM_DEBUG_LOG].handle);
    session = handles[CONSOLE_PROGRAM_SESSION].handle;
    say("nova-console-echo: writing the greeting\r\n");
    write_text(greeting);
    for (;;) {
        write_text("> ");
        length = read_line(0);
        if (is_secret_word(length)) {
            secret();
            continue;
        }
        write_bytes(in.line, length);
        write_text("\n");
        say("nova-console-echo: echoed a line of ");
        say_number(length);
        say(" bytes\r\n");
    }
}
