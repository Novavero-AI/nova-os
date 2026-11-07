/* init's part in the console. The console is the platform driver's
   input client (platform.c), which starts it and asks this file for its
   bootstrap message, so each start of the console, the first and every
   restart, gets a control channel of its own; the end init keeps is the
   one the next display and sessions go to. The display goes only after
   the kernel has released it, so a console started before the release
   gets it from console_start, and one started after from
   console_started. */

#include "console.h"
#include "console_protocol.h"
#include "init.h"
#include "input_protocol.h"
#include "loader.h"

#define DEMO_FILE "conecho.exe"
#define PIXEL_BYTES 4

/* The rights each handle the console gets travels with: the debug log
   to write to it, its process to map memory, its control channel to read
   from, its end of a session to serve it, and the framebuffer's resource
   to make sections over it; none to pass on. */
#define CONSOLE_LOG_RIGHTS RESOURCE_RIGHT_USE
#define CONSOLE_SELF_RIGHTS PROCESS_RIGHT_MAP
#define CONSOLE_CONTROL_RIGHTS (CHANNEL_RIGHT_READ | RIGHT_WAIT)
#define CONSOLE_END_RIGHTS                                                \
    (CHANNEL_RIGHT_READ | CHANNEL_RIGHT_WRITE | RIGHT_WAIT)
#define FRAMEBUFFER_RIGHTS RESOURCE_RIGHT_USE

/* A program's bootstrap channel queues the one message it is sent. */
#define PROGRAM_CHANNEL_CAPACITY 1

static UINT32 control = HANDLE_INVALID;
static int released;

static void say_status(const char *what, UINT32 status)
{
    say("nova-init: ");
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

static void close_handles(HANDLE_TRANSFER *handles, UINT32 count)
{
    UINT32 i;

    for (i = 0; i < count; i++)
        if (handles[i].handle != HANDLE_INVALID) {
            handle_close(handles[i].handle);
            handles[i].handle = HANDLE_INVALID;
        }
}

/* Send the display and a resource covering exactly the framebuffer's
   pages to the console. */
static void send_display(const PLATFORM *platform)
{
    const BOOTSTRAP_DISPLAY *d = &platform->boot->display;
    UINT64 first = d->base - d->base % PAGE_SIZE;
    UINT64 end = (d->base + (UINT64)d->stride * d->height * PIXEL_BYTES +
                  PAGE_SIZE - 1) /
                 PAGE_SIZE * PAGE_SIZE;
    CONSOLE_SETUP_DISPLAY_MESSAGE message;
    HANDLE_TRANSFER framebuffer;
    UINT32 status;

    framebuffer.handle = HANDLE_INVALID;
    framebuffer.rights = FRAMEBUFFER_RIGHTS;
    message.setup.transaction = 0;
    message.setup.kind = CONSOLE_SETUP_DISPLAY;
    message.display = *d;
    status = resource_create(platform->mmio, first, end - first,
                             &framebuffer.handle);
    if (status == OK)
        status = channel_write(control, &message, sizeof message,
                               &framebuffer, 1);
    if (status != OK) {
        close_handles(&framebuffer, 1);
        say_status("the display did not reach the console", status);
        return;
    }
    say("nova-init: the display, ");
    say_number(d->width);
    say("x");
    say_number(d->height);
    say(", sent to the console with a resource over ");
    say_hex(first);
    say(" to ");
    say_hex(end);
    say("\r\n");
}

UINT32 console_session(void)
{
    CONSOLE_SETUP setup;
    HANDLE_TRANSFER end;
    UINT32 client = HANDLE_INVALID, status = ERR_BAD_STATE;

    end.handle = HANDLE_INVALID;
    end.rights = CONSOLE_END_RIGHTS;
    setup.transaction = 0;
    setup.kind = CONSOLE_SETUP_SESSION;
    if (control != HANDLE_INVALID)
        status = channel_create(CONSOLE_SESSION_CAPACITY, &end.handle,
                                &client);
    if (status == OK)
        status = channel_write(control, &setup, sizeof setup, &end, 1);
    if (status == OK)
        return client;
    close_handles(&end, 1);
    if (client != HANDLE_INVALID)
        handle_close(client);
    say_status("no session on the console", status);
    return HANDLE_INVALID;
}

/* Start the demo client in the root job with a session; it runs
   unsupervised, and ends when its session does. */
static void start_demo(const PLATFORM *platform)
{
    const BOOT_FILE_VIEW *file = boot_file(DEMO_FILE);
    HANDLE_TRANSFER handles[CONSOLE_PROGRAM_HANDLES];
    PROGRAM program;
    UINT32 mine = HANDLE_INVALID, theirs = HANDLE_INVALID, transaction = 0;
    UINT32 status;

    if (!file) {
        say("nova-init: no \\nova\\" DEMO_FILE ", the console's demo "
            "client\r\n");
        return;
    }
    program.process = HANDLE_INVALID;
    program.thread = HANDLE_INVALID;
    handles[CONSOLE_PROGRAM_DEBUG_LOG].handle = HANDLE_INVALID;
    handles[CONSOLE_PROGRAM_DEBUG_LOG].rights = CONSOLE_LOG_RIGHTS;
    handles[CONSOLE_PROGRAM_SESSION].handle = console_session();
    handles[CONSOLE_PROGRAM_SESSION].rights = CONSOLE_SESSION_RIGHTS;
    status = handles[CONSOLE_PROGRAM_SESSION].handle == HANDLE_INVALID
                 ? ERR_BAD_STATE
                 : loader_load(platform->job, file, &program);
    if (status == OK)
        status = resource_create(platform->debug_log,
                                 SYSTEM_PRIVILEGE_DEBUG_LOG, 1,
                                 &handles[CONSOLE_PROGRAM_DEBUG_LOG].handle);
    if (status == OK)
        status = channel_create(PROGRAM_CHANNEL_CAPACITY, &mine, &theirs);
    if (status == OK)
        status = channel_write(mine, &transaction,
                               CONSOLE_PROGRAM_MESSAGE_SIZE, handles,
                               CONSOLE_PROGRAM_HANDLES);
    if (status != OK)
        close_handles(handles, CONSOLE_PROGRAM_HANDLES);
    if (status == OK)
        status = loader_start(&program, theirs);
    if (status == OK)
        theirs = HANDLE_INVALID;
    close_if_open(&theirs);
    close_if_open(&mine);
    close_if_open(&program.thread);
    close_if_open(&program.process);
    if (status != OK) {
        say_status("the console's demo client did not start", status);
        return;
    }
    say("nova-init: started \\nova\\" DEMO_FILE " on the console\r\n");
}

UINT32 console_bootstrap(const PLATFORM *platform, UINT32 process,
                         UINT32 channel)
{
    HANDLE_TRANSFER handles[CONSOLE_BOOTSTRAP_HANDLES];
    UINT32 mine = HANDLE_INVALID, transaction = 0, i;
    UINT32 status;

    for (i = 0; i < CONSOLE_BOOTSTRAP_HANDLES; i++)
        handles[i].handle = HANDLE_INVALID;
    handles[CONSOLE_BOOTSTRAP_DEBUG_LOG].rights = CONSOLE_LOG_RIGHTS;
    handles[CONSOLE_BOOTSTRAP_SELF].rights = CONSOLE_SELF_RIGHTS;
    handles[CONSOLE_BOOTSTRAP_CONTROL].rights = CONSOLE_CONTROL_RIGHTS;
    status = resource_create(platform->debug_log, SYSTEM_PRIVILEGE_DEBUG_LOG,
                             1, &handles[CONSOLE_BOOTSTRAP_DEBUG_LOG].handle);
    if (status == OK)
        status = handle_duplicate(process, PROCESS_RIGHT_MAP | RIGHT_TRANSFER,
                                  &handles[CONSOLE_BOOTSTRAP_SELF].handle);
    if (status == OK)
        status = channel_create(CONSOLE_CONTROL_CAPACITY, &mine,
                                &handles[CONSOLE_BOOTSTRAP_CONTROL].handle);
    if (status == OK)
        status = channel_write(channel, &transaction,
                               INPUT_CLIENT_MESSAGE_SIZE, handles,
                               CONSOLE_BOOTSTRAP_HANDLES);
    if (status != OK) {
        close_handles(handles, CONSOLE_BOOTSTRAP_HANDLES);
        if (mine != HANDLE_INVALID)
            handle_close(mine);
        return status;
    }
    if (control != HANDLE_INVALID)
        handle_close(control);
    control = mine;
    return OK;
}

void console_started(const PLATFORM *platform)
{
    if (!released)
        return;
    send_display(platform);
    start_demo(platform);
}

void console_start(const PLATFORM *platform, UINT32 bootstrap)
{
    UINT32 observed;
    UINT32 status = object_wait_one(bootstrap, CHANNEL_PEER_CLOSED,
                                    DEADLINE_INFINITE, &observed);

    handle_close(bootstrap);
    if (status != OK) {
        say_status("cannot wait for the display's release", status);
        return;
    }
    released = 1;
    say("nova-init: the kernel released the display\r\n");
    if (control == HANDLE_INVALID) {
        say("nova-init: no console to draw on it\r\n");
        return;
    }
    send_display(platform);
    start_demo(platform);
}
