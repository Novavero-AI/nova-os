/* The session's program on the logout disk, which stands in for the shell
   (docs/spec/accounts.md A7, ci/boot.py logout). The shell waits for each
   program it runs, so a program it started never outlives it, and what
   login does with the programs left at logout cannot be seen with it.
   The driver asks the session's launcher for programs as the shell does,
   and ends without waiting for them.

   It runs VICTIM_ONCE programs, each waited for, more of them than a
   process has handles, so that a handle login keeps for each program
   past its end runs login out of handles before they are done. It then
   runs victims, which stay running, until the launcher refuses one, the
   count that refusal comes at being login's limit on a session's
   programs; leaves them running for HOLD_NS, which is long for each to
   beat; and ends. Login then ends every program of the session, which
   is the check: it must say how many, and no victim may beat again. */

#include "accounts_protocol.h"
#include "directory_protocol.h"
#include "log.h"
#include "logout.h"
#include "native.h"
#include "path.h"
#include "session.h"

/* More runs than the 256 handles a process holds at most. */
#define CHURN_RUNS 260

/* The most victims the driver tries; the launcher refuses one first. */
#define VICTIMS_MAX 32

/* A victim's identifier is its number in decimal, of at most two
   digits. */
_Static_assert(VICTIMS_MAX <= 100, "an identifier has two digits");

#define NS_PER_SECOND 1000000000ULL
#define HOLD_NS (2 * NS_PER_SECOND)

/* A program that has been told to end ends within this. */
#define END_LIMIT_NS (5 * NS_PER_SECOND)

#define ARGUMENT_MAX 16

/* The victim's file in /store, a path recorded when this program is
   built. */
static const char victim_path[] = VICTIM_PATH;

static ACCOUNTS_USER user;
static UINT32 launcher = HANDLE_INVALID;

static void copy(UINT8 *to, const char *from, UINT32 count)
{
    UINT32 i;

    for (i = 0; i < count; i++)
        to[i] = (UINT8)from[i];
}

static UINT32 length_of(const char *text)
{
    UINT32 n = 0;

    while (text[n])
        n++;
    return n;
}

static void read_bootstrap(UINT32 bootstrap)
{
    RECEIVED_HANDLE handles[SESSION_HANDLES_MAX];
    UINT32 bytes, count;

    if (channel_read(bootstrap, &user, sizeof user, handles,
                     SESSION_HANDLES_MAX, &bytes, &count) != OK ||
        bytes != sizeof user || count < SESSION_ADMINISTRATOR ||
        handles[SESSION_DEBUG_LOG].type != TYPE_RESOURCE ||
        handles[SESSION_LAUNCHER].type != TYPE_CHANNEL ||
        user.name_length == 0 || user.name_length > ACCOUNT_NAME_MAX)
        process_exit(DRIVER_EXIT_FAILED);
    handle_close(bootstrap);
    say_open(handles[SESSION_DEBUG_LOG].handle);
    launcher = handles[SESSION_LAUNCHER].handle;
}

/* Ask the launcher to run the victim with argument, as the shell asks
   (shell.c, run). Returns the launcher's status, with the program's
   process in *process if that is OK. */
static UINT32 run(const char *argument, UINT32 *process)
{
    static UINT8 buffer[sizeof(IO_REQUEST) + sizeof(LAUNCHER_RUN_REQUEST) +
                        sizeof victim_path + ARGUMENT_MAX];
    IO_REQUEST *request = (IO_REQUEST *)buffer;
    LAUNCHER_RUN_REQUEST *details =
        (LAUNCHER_RUN_REQUEST *)(buffer + sizeof *request);
    UINT8 *data = buffer + sizeof *request + sizeof *details;
    UINT32 path_length = length_of(victim_path);
    UINT32 argument_bytes = length_of(argument) + 1, reply_length, status;
    UINT8 reply[sizeof(IO_REPLY)];
    RECEIVED_HANDLE received;

    copy(data, victim_path, path_length);
    copy(data + path_length, argument, argument_bytes);
    request->operation = LAUNCHER_RUN;
    request->offset = 0;
    request->length = sizeof *details + path_length + argument_bytes;
    request->rights = 0;
    request->flags = 0;
    request->reserved = 0;
    details->path_length = path_length;
    details->argument_bytes = argument_bytes;
    received.handle = HANDLE_INVALID;
    status = io_call(launcher, request, sizeof *request + request->length,
                     reply, sizeof reply, &reply_length, &received);
    if (status == OK && received.type != TYPE_PROCESS) {
        handle_close(received.handle);
        return ERR_BAD_STATE;
    }
    *process = status == OK ? received.handle : HANDLE_INVALID;
    return status;
}

/* Log the failure of what the driver did, and end. */
static _Noreturn void fail(const char *event, UINT32 status)
{
    say("nova-driver: ");
    say(event);
    say(" status=");
    say_number(status);
    say("\r\n");
    process_exit(DRIVER_EXIT_FAILED);
}

/* Run VICTIM_ONCE programs, each waited for until it has ended. */
static void churn(void)
{
    UINT32 process, observed, status, i;
    UINT64 now;

    for (i = 0; i < CHURN_RUNS; i++) {
        status = run(VICTIM_ONCE, &process);
        if (status != OK)
            fail("churn failed", status);
        status = clock_monotonic(&now);
        if (status == OK)
            status = object_wait_one(process, PROCESS_TERMINATED,
                                     now + END_LIMIT_NS, &observed);
        handle_close(process);
        if (status != OK)
            fail("churn failed", status);
    }
    say("nova-driver: churned runs=");
    say_number(CHURN_RUNS);
    say("\r\n");
}

static void name_victim(UINT32 id, char *name)
{
    UINT32 n = 0;

    if (id >= 10)
        name[n++] = (char)('0' + id / 10);
    name[n++] = (char)('0' + id % 10);
    name[n] = 0;
}

/* Run victims, named 0, 1, and on, until one is refused, and leave them
   running. */
static void start_victims(void)
{
    char name[ARGUMENT_MAX];
    UINT32 process, status, id;

    for (id = 0; id < VICTIMS_MAX; id++) {
        name_victim(id, name);
        status = run(name, &process);
        say("nova-driver: run id=");
        say_number(id);
        say(" status=");
        say_number(status);
        say("\r\n");
        if (status != OK)
            return;
        handle_close(process);
    }
    fail("run limit failed", OK);
}

_Noreturn void driver_main(UINT32 bootstrap, UINT64 argument)
{
    static WAIT_ENTRY nothing;
    UINT64 now;

    (void)argument;
    read_bootstrap(bootstrap);
    say("nova-driver: session name=");
    say_value_bytes(user.name, user.name_length);
    say("\r\n");
    churn();
    start_victims();
    if (clock_monotonic(&now) != OK)
        process_exit(DRIVER_EXIT_FAILED);
    object_wait_many(&nothing, 0, now + HOLD_NS);
    process_exit(DRIVER_EXIT_ENDED);
}
