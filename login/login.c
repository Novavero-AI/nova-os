/* login (docs/spec/accounts.md A6, A7): the program on the console that
   creates the first account on a system with none, and otherwise asks
   for a name and a password. A login the accounts service accepts gives
   login the session's handles, the user's home and, for an
   administrator, an administrator channel; login starts the shell with
   those, a console session, the store's root to read and list, and a
   launcher channel to itself, and nothing else. While the shell runs,
   login serves it twice over. It relays the shell's console session to
   its own (console.md C4), so that its own outlives the shell's. And it
   is the shell's launcher: it starts each program the shell names from
   /store with a view of the program's closure, as the first process
   starts its test program (storage.md F5, F16). When the shell ends,
   the session ends, and login asks again.

   Each program the shell names runs in a job of its own, made in the
   sessions' job. Login keeps the job's handle until a later run needs
   the place of a program that has ended, or the session ends, and then
   ends every job it still holds, so that no program of one session runs
   on into the next (objects.md O7).

   Its console session and its channel to the accounts service come from
   the first process as setups on login's bootstrap channel, after each
   start of the console, of the service and of login (session.h). When
   either's peer closes, login takes the next, so a restart of the
   console or of the service leaves login, and a session in progress,
   running. While the service hashes a password, which takes seconds,
   login says so on the console.

   Three refused logins end an attempt: login says so and starts a new
   one, with the name asked again. The accounts service delays each
   refusal's reply (A5), which is what bounds guessing; the three
   refusals bound how long one attempt holds the console. Every password
   is wiped from login's memory once the service has it. */

#include "accounts_protocol.h"
#include "console.h"
#include "directory_protocol.h"
#include "launch.h"
#include "log.h"
#include "native.h"
#include "path.h"
#include "program.h"
#include "services.h"
#include "session.h"
#include "store_image.h"
#include "wipe.h"

#define EXIT_FAILED 1

/* An attempt ends after this many refused logins. */
#define ATTEMPT_FAILURES_MAX 3

/* A service that cannot count its accounts is asked again after this
   pause, which a setup from the first process cuts short: long enough
   that a fault that stays fills the log slowly, short enough that one
   that clears, as a file system's spent channels free, is noticed
   soon. */
#define COUNT_RETRY_SECONDS 10
#define NS_PER_SECOND 1000000000ULL

/* Where login maps a program's file while it reads it, and where it
   lays the program's image out (program.h): two windows of
   SECTION_MAX_SIZE, the largest file or image a section holds. */
#define FILE_WINDOW 0x0000000100000000ULL
#define LAYOUT_BASE 0x0000000200000000ULL
#define WINDOW_SIZE SECTION_MAX_SIZE

/* A launcher channel queues one request and its reply each way; so does
   a bootstrap channel its one message. */
#define LAUNCHER_CAPACITY 1
#define BOOTSTRAP_CAPACITY 1

/* The largest request on a console session (C4). */
#define CONSOLE_REQUEST_MAX (sizeof(CONSOLE_REQUEST) + CONSOLE_WRITE_MAX)

/* What a program login starts holds of itself: MAP, to map sections. */
#define SELF_RIGHTS (PROCESS_RIGHT_MAP | RIGHT_TRANSFER)

/* The store service serves SERVICE_CLIENTS clients at once, and its
   control channel and its root start one each; every other is the
   closure directory of a program running, one per program (storage.md
   F17). A session therefore runs at most the rest at once, and login
   refuses a run past them itself, with ERR_NO_RESOURCES, where the
   store service would refuse it with the same status. Login keeps a job
   for each, and lets go of the jobs of programs that have ended when a
   run needs the place, so a session runs any number of programs one
   after another. */
#define STORE_STARTING_CLIENTS 2
#define SESSION_PROGRAMS_MAX (SERVICE_CLIENTS - STORE_STARTING_CLIENTS)

_Static_assert(SERVICE_CLIENTS > STORE_STARTING_CLIENTS,
               "the store service serves a client for each program");

/* Login's own handle to a program's process, to tell when it has ended. */
#define WATCH_RIGHTS PROCESS_RIGHT_INSPECT

/* A program's closure: read and list it, and map its files executable. */
#define CLOSURE_RIGHTS (IO_RIGHT_READ | IO_RIGHT_EXECUTE | IO_RIGHT_LIST)

/* The shell's store: read and list, no more. It runs programs by asking
   login, never by mapping them itself. */
#define SHELL_STORE_RIGHTS (IO_RIGHT_READ | IO_RIGHT_LIST)

#define STORE_DIRECTORY "/store/"
#define STORE_DIRECTORY_LENGTH 7
#define STORE_PREFIX "/store"
#define STORE_PREFIX_LENGTH 6

/* The longest path to the shell's file the bootstrap message carries. */
#define SHELL_PATH_MAX (STORE_DIRECTORY_LENGTH + STORE_BASE_NAME_MAX + 64)

/* The largest request on the accounts service's login channel. */
#define CREDENTIALS_REQUEST_MAX                                           \
    (sizeof(IO_REQUEST) + sizeof(ACCOUNTS_CREDENTIALS) + ACCOUNT_NAME_MAX + \
     ACCOUNT_PASSWORD_MAX)

/* A line read for a name or a password holds one byte more than either
   may have, so that a line too long is told from one that fits. */
#define NAME_LINE (ACCOUNT_NAME_MAX + 1)
#define PASSWORD_LINE (ACCOUNT_PASSWORD_MAX + 1)

_Static_assert(PASSWORD_LINE <= CONSOLE_LINE_MAX,
               "the console reads a whole password and one byte more");

typedef struct {
    UINT8 bytes[PASSWORD_LINE];
    UINT32 length;
} SECRET;

static UINT32 debug_log = HANDLE_INVALID, self = HANDLE_INVALID;
static UINT32 job = HANDLE_INVALID, console = HANDLE_INVALID;
static UINT32 accounts = HANDLE_INVALID, store = HANDLE_INVALID;
static UINT32 store_control = HANDLE_INVALID, setups = HANDLE_INVALID;
static VIEW store_view;
static LAYOUT_WINDOW layout;
static char shell_path[SHELL_PATH_MAX + 1];

/* A program the session started: login's handle to its job, which
   carries TERMINATE, and to its process. A place is free while its job
   is HANDLE_INVALID, which is the zero of static storage. */
typedef struct {
    UINT32 job;
    UINT32 process;
} SESSION_PROGRAM;

static SESSION_PROGRAM session_programs[SESSION_PROGRAMS_MAX];

/* Programs take no interrupt vectors: none drives a device. */
static const JOB_LIMITS no_vectors;

static UINT8 message[CHANNEL_MAX_BYTES];
static UINT8 request_bytes[CREDENTIALS_REQUEST_MAX];
static UINT8 relayed[CONSOLE_REQUEST_MAX];
static UINT8 relayed_line[sizeof(CONSOLE_REPLY) + CONSOLE_LINE_MAX];
static RECEIVED_HANDLE received[CHANNEL_MAX_HANDLES];
static SECRET password, again;
static UINT8 name[NAME_LINE];
static UINT32 name_length;

/* A message with no handles still names an array in user memory, which
   channel_write checks whatever the count (syscalls.md S6). */
static const HANDLE_TRANSFER no_handles[1];

static void copy(UINT8 *to, const UINT8 *from, UINT32 count)
{
    UINT32 i;

    for (i = 0; i < count; i++)
        to[i] = from[i];
}

static int same(const UINT8 *a, const UINT8 *b, UINT32 count)
{
    UINT32 i;

    for (i = 0; i < count; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}

static UINT32 length_of(const char *text)
{
    UINT32 n = 0;

    while (text[n])
        n++;
    return n;
}

static void forget(SECRET *secret)
{
    wipe(secret, sizeof *secret);
}

/* A line for event, which ends in failed or refused, with status. */
static void say_status(const char *event, UINT32 status)
{
    say("nova-login: ");
    say(event);
    say(" status=");
    say_number(status);
    say("\r\n");
}

static void close_received(UINT32 count)
{
    UINT32 i;

    for (i = 0; i < count; i++)
        handle_close(received[i].handle);
}

/* ------------------------------------------------------------------------
   Setups. */

/* Take the next setup from the first process (session.h), waiting for
   one if wait, and put its handle in place of the console's or the
   accounts service's, whichever it is. Returns 0 if none was there to
   take without waiting. A setup that is not one is closed; a bootstrap
   channel that fails ends login, which has nothing to wait for then. */
static int take_setup(int wait)
{
    LOGIN_SETUP setup;
    RECEIVED_HANDLE handle;
    UINT32 bytes = 0, count = 0, observed, status;
    UINT32 *slot;

    if (wait &&
        object_wait_one(setups, CHANNEL_READABLE | CHANNEL_PEER_CLOSED,
                        DEADLINE_INFINITE, &observed) != OK)
        process_exit(EXIT_FAILED);
    status = channel_read(setups, &setup, sizeof setup, &handle, 1, &bytes,
                          &count);
    if (status == ERR_SHOULD_WAIT && !wait)
        return 0;
    if (status != OK) {
        say_status("setup read failed", status);
        process_exit(EXIT_FAILED);
    }
    slot = setup.kind == LOGIN_SETUP_CONSOLE    ? &console
           : setup.kind == LOGIN_SETUP_ACCOUNTS ? &accounts
                                                : 0;
    if (bytes != sizeof setup || setup.transaction != 0 || count != 1 ||
        handle.type != TYPE_CHANNEL || !slot) {
        if (count)
            handle_close(handle.handle);
        say("nova-login: setup refused reason=malformed\r\n");
        return 1;
    }
    if (*slot != HANDLE_INVALID)
        handle_close(*slot);
    *slot = handle.handle;
    return 1;
}

/* Take every setup already queued, then wait for a console session and
   a channel to the accounts service while either is missing. */
static void take_setups(void)
{
    while (take_setup(0))
        ;
    while (console == HANDLE_INVALID || accounts == HANDLE_INVALID)
        take_setup(1);
}

/* Wait COUNT_RETRY_SECONDS, or until the first process sends a setup. */
static void pause_before_retry(void)
{
    UINT64 now = 0;
    UINT32 observed;

    clock_monotonic(&now);
    object_wait_one(setups, CHANNEL_READABLE,
                    now + COUNT_RETRY_SECONDS * NS_PER_SECOND, &observed);
}

/* Drop *channel, whose peer has closed; the next setup replaces it. */
static void lose(UINT32 *channel)
{
    if (*channel != HANDLE_INVALID)
        handle_close(*channel);
    *channel = HANDLE_INVALID;
}

/* ------------------------------------------------------------------------
   The console. A session that fails is dropped, and whatever was in
   progress on it given up; the next session starts again from the
   prompt. */

static void show(const char *text)
{
    if (console != HANDLE_INVALID && console_say(console, text) != OK)
        lose(&console);
}

/* Read a line into line, of capacity bytes, at *length. Returns 0 if
   the console session failed. */
static int ask(const char *prompt, CONSOLE_ECHO_MODE echo, UINT8 *line,
               UINT32 capacity, UINT32 *length)
{
    show(prompt);
    if (console == HANDLE_INVALID ||
        console_read_line(console, echo, line, capacity, length) != OK) {
        lose(&console);
        return 0;
    }
    if (echo == CONSOLE_NO_ECHO)
        show("\n");
    return 1;
}

static int ask_name(const char *prompt)
{
    return ask(prompt, CONSOLE_ECHO, name, sizeof name, &name_length);
}

static int ask_password(const char *prompt, SECRET *secret)
{
    return ask(prompt, CONSOLE_NO_ECHO, secret->bytes, sizeof secret->bytes,
               &secret->length);
}

/* ------------------------------------------------------------------------
   The accounts service. */

/* Send operation with credentials, then wipe the request, which holds
   the password, and read the reply into message with its handles in
   received. Returns the reply's status, with *count its handles, every
   one closed unless the status is OK. A channel whose peer has closed,
   a service that ended, is dropped for the next setup's. */
static UINT32 accounts_call(UINT32 operation, const SECRET *secret,
                            UINT32 *reply_length, UINT32 *count)
{
    IO_REQUEST *request = (IO_REQUEST *)request_bytes;
    ACCOUNTS_CREDENTIALS *c =
        (ACCOUNTS_CREDENTIALS *)(request_bytes + sizeof *request);
    UINT8 *data = request_bytes + sizeof *request + sizeof *c;
    UINT32 status, observed, transaction;

    *count = 0;
    if (name_length > ACCOUNT_NAME_MAX ||
        (secret && secret->length > ACCOUNT_PASSWORD_MAX))
        return ERR_INVALID_ARGS;
    request->operation = operation;
    request->offset = 0;
    request->rights = 0;
    request->flags = 0;
    request->reserved = 0;
    if (secret) {
        c->name_length = name_length;
        c->password_length = secret->length;
        copy(data, name, name_length);
        copy(data + name_length, secret->bytes, secret->length);
        request->length = sizeof *c + name_length + secret->length;
    } else {
        request->length = 0;
    }
    status = io_send(accounts, request, sizeof *request + request->length,
                     0, 0);
    transaction = request->transaction;
    wipe(request_bytes, sizeof request_bytes);
    if (status == OK)
        status = object_wait_one(accounts,
                                 CHANNEL_READABLE | CHANNEL_PEER_CLOSED,
                                 DEADLINE_INFINITE, &observed);
    if (status == OK)
        status = channel_read(accounts, message, sizeof message, received,
                              CHANNEL_MAX_HANDLES, reply_length, count);
    if (status != OK) {
        *count = 0;
        if (status == ERR_PEER_CLOSED)
            lose(&accounts);
        return status;
    }
    if (*reply_length < sizeof(IO_REPLY) ||
        ((const IO_REPLY *)message)->transaction != transaction)
        status = ERR_BAD_STATE;
    else
        status = ((const IO_REPLY *)message)->status;
    if (status != OK) {
        close_received(*count);
        *count = 0;
    }
    return status;
}

/* ------------------------------------------------------------------------
   The session's programs. */

static int ended(UINT32 process)
{
    UINT32 state, exit_code;

    return process_info(process, &state, &exit_code) == OK &&
           state == PROCESS_STATE_TERMINATED;
}

static void let_go(SESSION_PROGRAM *program)
{
    handle_close(program->process);
    handle_close(program->job);
    program->process = HANDLE_INVALID;
    program->job = HANDLE_INVALID;
}

/* A place for a program, once the places of programs that have ended are
   free again; 0 if every program of the session still runs. */
static SESSION_PROGRAM *vacant_program(void)
{
    SESSION_PROGRAM *vacant = 0;
    UINT32 i;

    for (i = 0; i < SESSION_PROGRAMS_MAX; i++) {
        SESSION_PROGRAM *program = &session_programs[i];

        if (program->job != HANDLE_INVALID && ended(program->process))
            let_go(program);
        if (program->job == HANDLE_INVALID && !vacant)
            vacant = program;
    }
    return vacant;
}

/* End each program the session started and let go of its job. A program
   still running is stopped when task_kill returns, and finishes ending,
   closing its handles, as its last thread leaves the kernel (O10). A job
   whose kill fails stays held: login's handle is the only one that can
   end it, so the next logout tries again, and the program keeps its
   place meanwhile. Returns how many programs were running and are now
   stopped. */
static UINT32 end_programs(void)
{
    UINT32 stopped = 0, status, i;

    for (i = 0; i < SESSION_PROGRAMS_MAX; i++) {
        SESSION_PROGRAM *program = &session_programs[i];

        if (program->job == HANDLE_INVALID)
            continue;
        if (ended(program->process)) {
            let_go(program);
            continue;
        }
        status = task_kill(program->job);
        if (status != OK) {
            say_status("program kill failed", status);
            continue;
        }
        stopped++;
        let_go(program);
    }
    return stopped;
}

/* ------------------------------------------------------------------------
   Starting programs. */

static UINT32 debug_log_for_program(UINT32 *resource)
{
    return resource_create(debug_log, SYSTEM_PRIVILEGE_DEBUG_LOG, 1, resource);
}

/* Load the program whose file is at path in login's view of /store into
   a new process in into, a job login holds. */
static UINT32 load(const char *path, UINT32 into, PROGRAM *program)
{
    IO_ATTRIBUTES attributes;
    RECEIVED_HANDLE section;
    UINT32 channel, status, unmapped;
    UINT64 size = 0, mapped_size;

    program->process = HANDLE_INVALID;
    program->thread = HANDLE_INVALID;
    status = view_open(&store_view, path, IO_RIGHT_READ, &channel,
                       &attributes);
    if (status != OK)
        return status;
    if (attributes.kind != IO_KIND_FILE)
        status = ERR_NOT_SUPPORTED;
    if (status == OK)
        status = io_get_section(channel, IO_RIGHT_READ, &section, &size);
    handle_close(channel);
    if (status != OK)
        return status;
    mapped_size = (size + PAGE_SIZE - 1) / PAGE_SIZE * PAGE_SIZE;
    if (size == 0 || mapped_size > WINDOW_SIZE)
        status = ERR_INVALID_ARGS;
    if (status == OK)
        status = process_map(self, section.handle, 0, FILE_WINDOW,
                             mapped_size, MAP_READ);
    handle_close(section.handle);
    if (status != OK)
        return status;
    status = program_load(&layout, into, (const UINT8 *)FILE_WINDOW, size,
                          program);
    unmapped = process_unmap(self, FILE_WINDOW, mapped_size);
    if (status == OK && unmapped != OK) {
        handle_close(program->thread);
        handle_close(program->process);
        program->process = HANDLE_INVALID;
        status = unmapped;
    }
    return status;
}

/* Start program with a bootstrap message of byte_count bytes and count
   handles, which move into it. On failure the handles are closed, and
   the process too. */
static UINT32 start(PROGRAM *program, const void *bytes,
                    UINT32 byte_count, const HANDLE_TRANSFER *handles,
                    UINT32 count)
{
    UINT32 mine, theirs, i;
    UINT32 status = channel_create(BOOTSTRAP_CAPACITY, &mine, &theirs);
    int written = 0;

    if (status == OK) {
        status = channel_write(mine, bytes, byte_count, handles, count);
        written = status == OK;
        handle_close(mine);
        if (status == OK)
            status = program_start(program, theirs);
        if (status != OK)
            handle_close(theirs);
    }
    if (status != OK) {
        if (!written)
            for (i = 0; i < count; i++)
                if (handles[i].handle != HANDLE_INVALID)
                    handle_close(handles[i].handle);
        if (program->thread != HANDLE_INVALID)
            handle_close(program->thread);
        handle_close(program->process);
        program->process = HANDLE_INVALID;
    }
    return status;
}

/* The base name of the store path a clean absolute path below /store
   names: no empty, "." or ".." component, and a component after
   /store/. Returns 0 if path is not one. */
static int store_base(const UINT8 *path, UINT32 length, const UINT8 **base,
                      UINT32 *base_length)
{
    UINT32 at = STORE_DIRECTORY_LENGTH, start, n;

    if (length <= STORE_DIRECTORY_LENGTH ||
        !same(path, (const UINT8 *)STORE_DIRECTORY, STORE_DIRECTORY_LENGTH))
        return 0;
    *base = path + at;
    *base_length = 0;
    while (at < length) {
        start = at;
        while (at < length && path[at] != '/' && path[at] != 0)
            at++;
        n = at - start;
        if (at < length && path[at] == 0)
            return 0;
        if (n == 0 || (n == 1 && path[start] == '.') ||
            (n == 2 && path[start] == '.' && path[start + 1] == '.'))
            return 0;
        if (!*base_length)
            *base_length = n;
        if (at < length)
            at++;
        if (at == length && path[length - 1] == '/')
            return 0;
    }
    return *base_length > 0 && *base_length <= STORE_BASE_NAME_MAX;
}

/* Start the program at the path of path_length bytes in a job of its own,
   with a view of its closure under /store and nothing else, and
   argument_bytes of arguments (launch.h). The job is kept in a place of
   the session's until the session ends or the program has ended and
   another run needs the place; with none vacant the run is refused with
   ERR_NO_RESOURCES and *at_limit set, which is the session's own limit,
   not a failure. */
static UINT32 run_program(const UINT8 *path, UINT32 path_length,
                          const UINT8 *arguments, UINT32 argument_bytes,
                          UINT32 *process, int *at_limit)
{
    static char text[IO_PATH_MAX + 1];
    static UINT8 launch[sizeof(LAUNCH_HEADER) + sizeof(LAUNCH_VIEW_ENTRY) +
                        LAUNCH_ARGUMENTS_MAX];
    HANDLE_TRANSFER handles[LAUNCH_FIRST_DIRECTORY + 1];
    LAUNCH_HEADER *header = (LAUNCH_HEADER *)launch;
    LAUNCH_VIEW_ENTRY *entry = (LAUNCH_VIEW_ENTRY *)(launch + sizeof *header);
    IO_ATTRIBUTES attributes;
    SESSION_PROGRAM *place;
    PROGRAM program;
    const UINT8 *base;
    UINT32 base_length, closure, program_job = HANDLE_INVALID;
    UINT32 watch = HANDLE_INVALID, status, i;

    *process = HANDLE_INVALID;
    if (path_length > IO_PATH_MAX || argument_bytes > LAUNCH_ARGUMENTS_MAX ||
        !store_base(path, path_length, &base, &base_length))
        return ERR_INVALID_ARGS;
    place = vacant_program();
    if (!place) {
        *at_limit = 1;
        return ERR_NO_RESOURCES;
    }
    copy((UINT8 *)text, path, path_length);
    text[path_length] = 0;
    status = io_open(store_control, STORE_OPEN_CLOSURE, base, base_length,
                     CLOSURE_RIGHTS, &closure, &attributes);
    if (status != OK)
        return status;
    status = job_create(job, &no_vectors, &program_job);
    if (status == OK)
        status = load(text, program_job, &program);
    if (status != OK) {
        if (program_job != HANDLE_INVALID)
            handle_close(program_job);
        handle_close(closure);
        return status;
    }
    for (i = 0; i <= LAUNCH_FIRST_DIRECTORY; i++) {
        handles[i].handle = HANDLE_INVALID;
        handles[i].rights = RIGHTS_SAME;
    }
    handles[LAUNCH_FIRST_DIRECTORY].handle = closure;
    status = debug_log_for_program(&handles[LAUNCH_DEBUG_LOG].handle);
    if (status == OK)
        status = handle_duplicate(program.process, SELF_RIGHTS,
                                  &handles[LAUNCH_SELF].handle);
    if (status == OK)
        status = handle_duplicate(program.process, WATCH_RIGHTS, &watch);
    if (status != OK) {
        for (i = 0; i <= LAUNCH_FIRST_DIRECTORY; i++)
            if (handles[i].handle != HANDLE_INVALID)
                handle_close(handles[i].handle);
        handle_close(program.thread);
        handle_close(program.process);
        handle_close(program_job);
        return status;
    }
    header->view_count = 1;
    header->argument_bytes = argument_bytes;
    entry->length = STORE_PREFIX_LENGTH;
    for (i = 0; i < LAUNCH_PREFIX_MAX; i++)
        entry->prefix[i] = 0;
    copy(entry->prefix, (const UINT8 *)STORE_PREFIX, STORE_PREFIX_LENGTH);
    copy(launch + sizeof *header + sizeof *entry, arguments, argument_bytes);
    status = start(&program, launch,
                   sizeof *header + sizeof *entry + argument_bytes, handles,
                   LAUNCH_FIRST_DIRECTORY + 1);
    if (status != OK) {
        handle_close(watch);
        handle_close(program_job);
        return status;
    }
    place->job = program_job;
    place->process = watch;
    *process = program.process;
    return OK;
}

/* Serve one request on the launcher channel. Returns 0 if the channel
   ends: a message that cannot be read, or one too short to be a
   request. */
static int serve_launcher(UINT32 launcher)
{
    const IO_REQUEST *request = (const IO_REQUEST *)message;
    const LAUNCHER_RUN_REQUEST *run =
        (const LAUNCHER_RUN_REQUEST *)(message + sizeof *request);
    const UINT8 *path = message + sizeof *request + sizeof *run;
    IO_REPLY reply;
    HANDLE_TRANSFER transfer;
    UINT32 bytes, count, status, process = HANDLE_INVALID;
    int at_limit = 0;

    if (channel_read(launcher, message, sizeof message, received,
                     CHANNEL_MAX_HANDLES, &bytes, &count) != OK)
        return 0;
    close_received(count);
    if (bytes < sizeof *request)
        return 0;
    if (request->operation != LAUNCHER_RUN)
        status = ERR_NOT_SUPPORTED;
    else if (bytes < sizeof *request + sizeof *run ||
             request->length != bytes - sizeof *request ||
             run->path_length > IO_PATH_MAX ||
             run->argument_bytes > LAUNCH_ARGUMENTS_MAX ||
             request->length !=
                 sizeof *run + run->path_length + run->argument_bytes ||
             (run->argument_bytes &&
              path[run->path_length + run->argument_bytes - 1] != 0))
        status = ERR_INVALID_ARGS;
    else
        status = run_program(path, run->path_length, path + run->path_length,
                             run->argument_bytes, &process, &at_limit);
    reply.transaction = request->transaction;
    reply.status = status;
    reply.information = 0;
    transfer.handle = process;
    transfer.rights = LAUNCHER_PROCESS_RIGHTS;
    if (channel_write(launcher, &reply, sizeof reply,
                      status == OK ? &transfer : no_handles,
                      status == OK) != OK &&
        status == OK)
        handle_close(process);
    if (status == OK) {
        say("nova-login: program started path=");
        say_value_bytes(path, run->path_length);
        say("\r\n");
    } else if (at_limit) {
        say("nova-login: program start refused reason=\"session limit\" "
            "limit=");
        say_number(SESSION_PROGRAMS_MAX);
        say("\r\n");
    } else {
        say_status("program start failed", status);
    }
    return 1;
}

/* ------------------------------------------------------------------------
   The shell's console. */

/* Login's console session for a relay: its own, or once that has
   failed, the next the first process sends, waited for while the shell
   waits on its request. */
static UINT32 relay_console(void)
{
    while (console == HANDLE_INVALID)
        take_setup(1);
    return console;
}

/* Serve one request on the shell's console session, as the console
   serves one (C4): a write is shown on login's own session, and a read
   is read there and answered with the shell's transaction, each again
   on the next session if login's fails. A message the console would end
   a session for ends the shell's, so that nothing the shell sends can
   end login's. Returns 0 if the session ends. */
static int relay(UINT32 session)
{
    const CONSOLE_REQUEST *request = (const CONSOLE_REQUEST *)relayed;
    CONSOLE_REPLY *reply = (CONSOLE_REPLY *)relayed_line;
    CONSOLE_ECHO_MODE echo;
    UINT32 bytes, count, length = 0, status;

    if (channel_read(session, relayed, sizeof relayed, received,
                     CHANNEL_MAX_HANDLES, &bytes, &count) != OK)
        return 0;
    if (count) {
        close_received(count);
        return 0;
    }
    if (bytes < sizeof *request)
        return 0;
    if (request->operation == CONSOLE_WRITE) {
        if (request->flags || request->length > CONSOLE_WRITE_MAX ||
            bytes != sizeof *request + request->length)
            return 0;
        while (console_write(relay_console(), relayed + sizeof *request,
                             request->length) != OK)
            lose(&console);
        return 1;
    }
    if (request->operation != CONSOLE_READ || bytes != sizeof *request ||
        request->length == 0 || request->length > CONSOLE_LINE_MAX ||
        (request->flags & ~(UINT32)CONSOLE_READ_NO_ECHO))
        return 0;
    echo = request->flags & CONSOLE_READ_NO_ECHO ? CONSOLE_NO_ECHO
                                                 : CONSOLE_ECHO;
    while (console_read_line(relay_console(), echo,
                             relayed_line + sizeof *reply, request->length,
                             &length) != OK)
        lose(&console);
    reply->transaction = request->transaction;
    reply->length = length;
    status = channel_write(session, relayed_line, sizeof *reply + length,
                           no_handles, 0);
    /* The line may be a password the shell asked for without echo. */
    wipe(relayed_line, sizeof relayed_line);
    return status == OK;
}

/* ------------------------------------------------------------------------
   Sessions. */

/* Start the shell for user, with the session's handles from the
   accounts service, home and, if administrator is not HANDLE_INVALID,
   the administrator channel, which move into it. Returns the shell's
   process, with the launcher channel's end at *launcher; or
   HANDLE_INVALID with every handle closed. */
static UINT32 start_shell(const ACCOUNTS_USER *user, UINT32 home,
                          UINT32 administrator, UINT32 *launcher,
                          UINT32 *session)
{
    HANDLE_TRANSFER handles[SESSION_HANDLES_MAX];
    IO_ATTRIBUTES attributes;
    PROGRAM program;
    UINT32 count = SESSION_ADMINISTRATOR, i, status;

    for (i = 0; i < SESSION_HANDLES_MAX; i++) {
        handles[i].handle = HANDLE_INVALID;
        handles[i].rights = RIGHTS_SAME;
    }
    handles[SESSION_HOME].handle = home;
    if (administrator != HANDLE_INVALID) {
        handles[SESSION_ADMINISTRATOR].handle = administrator;
        count = SESSION_ADMINISTRATOR + 1;
    }
    *launcher = HANDLE_INVALID;
    *session = HANDLE_INVALID;
    status = load(shell_path, job, &program);
    if (status == OK)
        status = debug_log_for_program(&handles[SESSION_DEBUG_LOG].handle);
    if (status == OK) {
        status = channel_create(CONSOLE_SESSION_CAPACITY, session,
                                &handles[SESSION_CONSOLE].handle);
        handles[SESSION_CONSOLE].rights = CONSOLE_SESSION_RIGHTS;
    }
    if (status == OK)
        status = channel_create(LAUNCHER_CAPACITY, launcher,
                                &handles[SESSION_LAUNCHER].handle);
    if (status == OK)
        status = io_open(store, IO_OPEN, 0, 0, SHELL_STORE_RIGHTS,
                         &handles[SESSION_STORE].handle, &attributes);
    if (status == OK) {
        status = start(&program, user, sizeof *user, handles, count);
        if (status == OK)
            return program.process;
        handle_close(*launcher);
        handle_close(*session);
        *launcher = HANDLE_INVALID;
        *session = HANDLE_INVALID;
        say_status("shell start failed", status);
        return HANDLE_INVALID;
    }
    say_status("shell start failed", status);
    for (i = 0; i < SESSION_HANDLES_MAX; i++)
        if (handles[i].handle != HANDLE_INVALID)
            handle_close(handles[i].handle);
    if (*launcher != HANDLE_INVALID)
        handle_close(*launcher);
    if (*session != HANDLE_INVALID)
        handle_close(*session);
    *launcher = HANDLE_INVALID;
    *session = HANDLE_INVALID;
    if (program.process != HANDLE_INVALID) {
        handle_close(program.thread);
        handle_close(program.process);
    }
    return HANDLE_INVALID;
}

/* A channel login serves for the shell, and what serves one message on
   it, returning 0 if the channel ends. */
typedef struct {
    UINT32 handle;
    int (*serve)(UINT32 channel);
} SERVED;

#define SERVED_LAUNCHER 0
#define SERVED_CONSOLE 1
#define SERVED_COUNT 2

/* A session's wait: the shell, the first process's channel, then the
   channels served. */
#define WAIT_SHELL 0
#define WAIT_SETUPS 1
#define SERVED_FIRST_WAIT 2

/* Run the session the accounts service's reply in message opened, with
   its count handles in received, until the shell ends. The wait lists
   the shell, the first process's channel, whose setups login takes as
   they come so that none waits for the session's end, then each
   channel still open; one whose peer has closed, or whose message ends
   it, is closed and leaves the list. */
static void run_session(UINT32 reply_length, UINT32 count)
{
    ACCOUNTS_USER user;
    SERVED served[SERVED_COUNT];
    WAIT_ENTRY items[SERVED_FIRST_WAIT + SERVED_COUNT];
    UINT32 slot[SERVED_FIRST_WAIT + SERVED_COUNT];
    UINT32 shell, waits, administrator = HANDLE_INVALID, i, observed, killed;
    const UINT8 *bytes = message + sizeof(IO_REPLY);
    int keep;

    if (reply_length != sizeof(IO_REPLY) + sizeof user || count == 0 ||
        count > ACCOUNTS_REPLY_ADMINISTRATOR + 1 ||
        received[ACCOUNTS_REPLY_HOME].type != TYPE_CHANNEL ||
        (count > ACCOUNTS_REPLY_ADMINISTRATOR &&
         received[ACCOUNTS_REPLY_ADMINISTRATOR].type != TYPE_CHANNEL)) {
        close_received(count);
        show("The accounts service's answer is not a session.\n");
        return;
    }
    for (i = 0; i < sizeof user; i++)
        ((UINT8 *)&user)[i] = bytes[i];
    if (user.name_length > ACCOUNT_NAME_MAX) {
        close_received(count);
        return;
    }
    if (count > ACCOUNTS_REPLY_ADMINISTRATOR)
        administrator = received[ACCOUNTS_REPLY_ADMINISTRATOR].handle;
    served[SERVED_LAUNCHER].serve = serve_launcher;
    served[SERVED_CONSOLE].serve = relay;
    shell = start_shell(&user, received[ACCOUNTS_REPLY_HOME].handle,
                        administrator, &served[SERVED_LAUNCHER].handle,
                        &served[SERVED_CONSOLE].handle);
    if (shell == HANDLE_INVALID) {
        show("The shell did not start.\n");
        return;
    }
    say("nova-login: session started name=");
    say_value_bytes(user.name, user.name_length);
    say("\r\n");
    for (;;) {
        items[WAIT_SHELL].handle = shell;
        items[WAIT_SHELL].signals = PROCESS_TERMINATED;
        items[WAIT_SETUPS].handle = setups;
        items[WAIT_SETUPS].signals = CHANNEL_READABLE;
        waits = SERVED_FIRST_WAIT;
        for (i = 0; i < SERVED_COUNT; i++) {
            if (served[i].handle == HANDLE_INVALID)
                continue;
            slot[waits] = i;
            items[waits].handle = served[i].handle;
            items[waits].signals = CHANNEL_READABLE | CHANNEL_PEER_CLOSED;
            waits++;
        }
        if (object_wait_many(items, waits, DEADLINE_INFINITE) != OK ||
            (items[WAIT_SHELL].observed & PROCESS_TERMINATED))
            break;
        if (items[WAIT_SETUPS].observed & CHANNEL_READABLE)
            while (take_setup(0))
                ;
        for (i = SERVED_FIRST_WAIT; i < waits; i++) {
            SERVED *c = &served[slot[i]];

            observed = items[i].observed;
            keep = observed & CHANNEL_READABLE
                       ? c->serve(c->handle)
                       : !(observed & CHANNEL_PEER_CLOSED);
            if (!keep) {
                handle_close(c->handle);
                c->handle = HANDLE_INVALID;
            }
        }
    }
    for (i = 0; i < SERVED_COUNT; i++)
        if (served[i].handle != HANDLE_INVALID)
            handle_close(served[i].handle);
    handle_close(shell);
    killed = end_programs();
    say("nova-login: session ended name=");
    say_value_bytes(user.name, user.name_length);
    say(" killed=");
    say_number(killed);
    say("\r\n");
    show("Logged out.\n\n");
}

/* ------------------------------------------------------------------------
   Attempts. */

/* The first account, an administrator's, then its session. Returns once
   the session ends, once another login has made an account, or once the
   console session or the service's channel has gone. */
static void first_boot(void)
{
    UINT32 status, reply_length, count;

    show("nova-os has no accounts yet. Create the administrator's "
         "account.\n");
    for (;;) {
        if (!ask_name("name: ") || !ask_password("password: ", &password) ||
            !ask_password("password again: ", &again)) {
            forget(&password);
            forget(&again);
            return;
        }
        if (password.length != again.length ||
            !same(password.bytes, again.bytes, password.length)) {
            forget(&password);
            forget(&again);
            show("The passwords differ. Try again.\n");
            continue;
        }
        forget(&again);
        if (password.length < ACCOUNT_PASSWORD_MIN ||
            password.length > ACCOUNT_PASSWORD_MAX) {
            forget(&password);
            show("A password has 8 to 128 characters. Try again.\n");
            continue;
        }
        show("Creating the account...\n");
        status = accounts_call(ACCOUNTS_CREATE_FIRST, &password,
                               &reply_length, &count);
        forget(&password);
        if (status == OK) {
            show("Created. Welcome, administrator.\n");
            run_session(reply_length, count);
            return;
        }
        if (status == ERR_ALREADY_EXISTS)
            return;
        if (status == ERR_PEER_CLOSED) {
            show("The accounts service restarted. Try again.\n");
            return;
        }
        if (status == ERR_INVALID_ARGS) {
            show("A name is 1 to 32 lowercase letters, digits and '-', "
                 "starting with a letter. Try again.\n");
            say_status("account create refused", status);
        } else {
            show("The account was not created. Try again.\n");
            say_status("account create failed", status);
        }
    }
}

/* Ask for a name and a password until a login succeeds, then run its
   session, or until ATTEMPT_FAILURES_MAX logins are refused, or the
   console session or the service's channel goes. */
static void attempt(void)
{
    UINT32 failures, status, reply_length, count;

    for (failures = 0; failures < ATTEMPT_FAILURES_MAX; failures++) {
        if (!ask_name("login: ") || !ask_password("password: ", &password)) {
            forget(&password);
            return;
        }
        show("Checking...\n");
        status = accounts_call(ACCOUNTS_LOGIN, &password, &reply_length,
                               &count);
        forget(&password);
        if (status == OK) {
            run_session(reply_length, count);
            return;
        }
        if (status == ERR_PEER_CLOSED) {
            show("The accounts service restarted. Try again.\n");
            return;
        }
        show("Login incorrect.\n");
        if (status == ERR_INVALID_ARGS)
            say_status("login refused", status);
        else if (status != ERR_ACCESS_DENIED)
            say_status("login check failed", status);
    }
    show("Three refused logins. This attempt has ended.\n\n");
    say("nova-login: attempt ended refusals=");
    say_number(ATTEMPT_FAILURES_MAX);
    say("\r\n");
}

/* ------------------------------------------------------------------------
   The program. */

/* The bootstrap message, from bootstrap, which login keeps for the
   first process's setups. */
static void read_bootstrap(UINT32 bootstrap)
{
    RECEIVED_HANDLE handles[LOGIN_HANDLES];
    UINT32 bytes, count;

    if (channel_read(bootstrap, message, sizeof message, handles,
                     LOGIN_HANDLES, &bytes, &count) != OK ||
        count != LOGIN_HANDLES ||
        handles[LOGIN_DEBUG_LOG].type != TYPE_RESOURCE ||
        handles[LOGIN_SELF].type != TYPE_PROCESS ||
        handles[LOGIN_JOB].type != TYPE_JOB ||
        handles[LOGIN_STORE].type != TYPE_CHANNEL ||
        handles[LOGIN_STORE_CONTROL].type != TYPE_CHANNEL ||
        bytes == 0 || bytes > SHELL_PATH_MAX + 1 || message[bytes - 1] != 0 ||
        length_of((const char *)message) != bytes - 1)
        process_exit(EXIT_FAILED);
    setups = bootstrap;
    debug_log = handles[LOGIN_DEBUG_LOG].handle;
    say_open(debug_log);
    self = handles[LOGIN_SELF].handle;
    job = handles[LOGIN_JOB].handle;
    store = handles[LOGIN_STORE].handle;
    store_control = handles[LOGIN_STORE_CONTROL].handle;
    copy((UINT8 *)shell_path, message, bytes);
    layout.self = self;
    layout.window = LAYOUT_BASE;
    layout.window_size = WINDOW_SIZE;
    if (view_add(&store_view, STORE_PREFIX, store) != OK)
        process_exit(EXIT_FAILED);
}

/* Each attempt starts with a console session and a channel to the
   service. A service that cannot count its accounts is reported on the
   console and asked again after a pause. */
_Noreturn void login_main(UINT32 bootstrap, UINT64 argument)
{
    UINT32 status, reply_length, count;

    (void)argument;
    read_bootstrap(bootstrap);
    say("nova-login: ready session=console shell=");
    say_value(shell_path);
    say("\r\n");
    for (;;) {
        take_setups();
        status = accounts_call(ACCOUNTS_COUNT, 0, &reply_length, &count);
        if (status == ERR_PEER_CLOSED)
            continue;
        if (status != OK) {
            say_status("account count failed", status);
            show("The accounts service cannot count its accounts; trying "
                 "again.\n");
            pause_before_retry();
            continue;
        }
        if (((const IO_REPLY *)message)->information == 0)
            first_boot();
        else
            attempt();
    }
}
