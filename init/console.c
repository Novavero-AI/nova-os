/* init's part in the console. The console is the platform driver's
   input client where no compositor runs (platform.c), which starts it and
   asks this file for its bootstrap message, so each start of the console,
   the first and every restart, gets a control channel of its own; the end
   init keeps is the one the options, the connection, the next display and
   sessions go to, the options first. Where a compositor draws, the
   kernel's release leaves the display with it (compositor.c) and the
   console, started beside it, is given login's session and a connection
   to it, on which it makes its window (console_connect), and no display,
   until the compositor is given up: then it is sent the display after a
   compositor (console_display_after), and a console started later the
   plain display, which console_started also gives login a session on
   (login_start.c); login's first session comes with its own start. The
   type specimen's disk runs no compositor and gives the console no
   display. */

#include "console.h"
#include "compositor.h"
#include "console_protocol.h"
#include "display_protocol.h"
#include "init.h"
#include "login_start.h"
#include "tests.h"

#define PIXEL_BYTES 4

/* The label of the console's connection to the compositor (display.md
   D5, D19 rule 2). */
#define CONSOLE_CONNECTION_LABEL "console"

/* The rights the framebuffer's resource travels with: to make sections
   over it, and none to pass on. The other handles the console gets
   travel with the rights console.desc gives them. */
#define FRAMEBUFFER_RIGHTS RESOURCE_RIGHT_USE

/* On a test disk the receiver of the kernel's background is told of a
   display MISMATCH_ROWS taller than the one it is given, so that the
   background does not match it and the receiver computes its Limb at
   the display, as a restarted one does (console.md C5, display.md D19);
   ci/boot.py checks that every test disk's boot takes that path and
   draws the screen the system disk's boot draws from the background. */
#define MISMATCH_ROWS 1

/* The bytes of each message sent to the console, aligned to 8 bytes as
   the generated functions require (docs/spec/interface.md P14) and as
   long as the control channel's longest message, which holds the
   bootstrap channel's too. */
static union {
    UINT64 alignment;
    UINT8 bytes[CONSOLE_CONTROL_TO_SERVER_BYTES_MAX];
} outgoing;
_Static_assert(CONSOLE_BOOTSTRAP_TO_CLIENT_BYTES_MAX <=
                   CONSOLE_CONTROL_TO_SERVER_BYTES_MAX,
               "the bootstrap message fits the outgoing buffer");

/* channel_write needs a valid array even for no handles. */
static const HANDLE_TRANSFER no_handles[1];

static UINT32 control = HANDLE_INVALID;

/* Who the display is for once the kernel has released it: nobody yet,
   the console, or a compositor. */
typedef enum { DISPLAY_UNRELEASED, DISPLAY_CONSOLE, DISPLAY_COMPOSITOR }
    DISPLAY_HOLDER;
static DISPLAY_HOLDER holder = DISPLAY_UNRELEASED;

static void close_handles(HANDLE_TRANSFER *handles, UINT32 count)
{
    UINT32 i;

    for (i = 0; i < count; i++)
        if (handles[i].handle != HANDLE_INVALID) {
            handle_close(handles[i].handle);
            handles[i].handle = HANDLE_INVALID;
        }
}

UINT32 display_resource(const PLATFORM *platform,
                        HANDLE_TRANSFER *framebuffer)
{
    const BOOTSTRAP_DISPLAY *d = &platform->boot->display;
    UINT64 first = d->base - d->base % PAGE_SIZE;
    UINT64 end = (d->base + (UINT64)d->stride * d->height * PIXEL_BYTES +
                  PAGE_SIZE - 1) /
                 PAGE_SIZE * PAGE_SIZE;

    framebuffer->handle = HANDLE_INVALID;
    framebuffer->rights = FRAMEBUFFER_RIGHTS;
    return resource_create(platform->mmio, first, end - first,
                           &framebuffer->handle);
}

UINT32 display_message(const PLATFORM *platform,
                       CONSOLE_SETUP_DISPLAY_MESSAGE *message,
                       HANDLE_TRANSFER *framebuffer)
{
    message->setup.transaction = 0;
    message->setup.kind = CONSOLE_SETUP_DISPLAY;
    message->display = platform->boot->display;
    return display_resource(platform, framebuffer);
}

void display_for_background(const PLATFORM *platform,
                            BOOTSTRAP_DISPLAY *display)
{
    *display = platform->boot->display;
    if (tests_enabled())
        display->height += MISMATCH_ROWS;
}

/* How the display goes to the console: as the first one after the
   kernel's, or after a compositor, whose last frame the screen still
   shows (console.md C2). */
typedef enum { DISPLAY_FROM_KERNEL, DISPLAY_FROM_COMPOSITOR } DISPLAY_FROM;

/* Send the display and a resource covering exactly the framebuffer's
   pages to the console. */
static void send_display(const PLATFORM *platform, DISPLAY_FROM from)
{
    CONSOLE_SETUP_DISPLAY_MESSAGE message;
    HANDLE_TRANSFER framebuffer;
    UINT32 status = display_message(platform, &message, &framebuffer);
    UINT32 resource = framebuffer.handle, size;

    if (status == OK && from == DISPLAY_FROM_COMPOSITOR)
        status = console_control_display_after_encode(
            &message, &resource, 1, &framebuffer, outgoing.bytes, &size);
    else if (status == OK)
        status = console_control_display_encode(&message, &resource, 1,
                                                &framebuffer,
                                                outgoing.bytes, &size);
    if (status == OK)
        status = channel_write(control, outgoing.bytes, size, &framebuffer,
                               1);
    if (status != OK) {
        close_handles(&framebuffer, 1);
        say_status("display send failed", status);
    }
}

/* Send the console its options, the first message on its control channel
   (console.md C2): ui.reduce-motion where the boot disk is a test disk
   with the flag file, and no bit otherwise. A console that has already
   ended has closed the channel, which its supervisor logs. */
static void send_options(void)
{
    CONSOLE_SETUP_OPTIONS_MESSAGE message;
    UINT32 size, status;

    message.setup.transaction = 0;
    message.setup.kind = 0;
    message.options = tests_flag(TEST_FLAG_NO_MOTION)
                          ? CONSOLE_OPTION_REDUCE_MOTION
                          : 0;
    status = console_control_options_encode(&message, outgoing.bytes, &size);
    if (status == OK)
        status = channel_write(control, outgoing.bytes, size, no_handles, 0);
    if (status != OK && status != ERR_PEER_CLOSED)
        say_status("console options send failed", status);
}

UINT32 console_session(void)
{
    CONSOLE_SETUP setup;
    HANDLE_TRANSFER end;
    UINT32 mine = HANDLE_INVALID, client = HANDLE_INVALID;
    UINT32 status = ERR_BAD_STATE, size;

    setup.transaction = 0;
    setup.kind = 0;
    if (control != HANDLE_INVALID)
        status = channel_create(CONSOLE_SESSION_CAPACITY, &mine, &client);
    if (status == OK)
        status = console_control_session_encode(&setup, &mine, 1, &end,
                                                outgoing.bytes, &size);
    if (status == OK)
        status = channel_write(control, outgoing.bytes, size, &end, 1);
    if (status == OK)
        return client;
    if (mine != HANDLE_INVALID)
        handle_close(mine);
    if (client != HANDLE_INVALID)
        handle_close(client);
    say_status("console session failed", status);
    return HANDLE_INVALID;
}

void console_connect(void)
{
    CONSOLE_SETUP setup;
    HANDLE_TRANSFER end;
    UINT32 client, size, status;

    if (control == HANDLE_INVALID || holder != DISPLAY_COMPOSITOR ||
        tests_flag(TEST_FLAG_WINDOW))
        return;
    status = compositor_connect(CONSOLE_CONNECTION_LABEL, DISPLAY_LAYER_SYSTEM,
                                0, &client);
    if (status != OK)
        return;
    setup.transaction = 0;
    setup.kind = 0;
    status = console_control_connection_encode(&setup, &client, 1, &end,
                                               outgoing.bytes, &size);
    if (status == OK)
        status = channel_write(control, outgoing.bytes, size, &end, 1);
    if (status == OK)
        return;
    handle_close(client);
    if (status != ERR_PEER_CLOSED)
        say_status("console connection send failed", status);
}

UINT32 console_bootstrap(const PLATFORM *platform, UINT32 process,
                         UINT32 channel)
{
    HANDLE_TRANSFER transfers[CONSOLE_BOOTSTRAP_HANDLES];
    UINT32 handles[CONSOLE_BOOTSTRAP_HANDLES];
    UINT32 mine = HANDLE_INVALID, size, i;
    UINT32 status;

    for (i = 0; i < CONSOLE_BOOTSTRAP_HANDLES; i++)
        handles[i] = HANDLE_INVALID;
    status = resource_create(platform->debug_log, SYSTEM_PRIVILEGE_DEBUG_LOG,
                             1, &handles[CONSOLE_BOOTSTRAP_DEBUG_LOG]);
    if (status == OK)
        status = handle_duplicate(process, PROCESS_RIGHT_MAP | RIGHT_TRANSFER,
                                  &handles[CONSOLE_BOOTSTRAP_SELF]);
    if (status == OK)
        status = channel_create(CONSOLE_CONTROL_CAPACITY, &mine,
                                &handles[CONSOLE_BOOTSTRAP_CONTROL]);
    if (status == OK)
        status = console_bootstrap_start_encode(0, handles,
                                                CONSOLE_BOOTSTRAP_HANDLES,
                                                transfers, outgoing.bytes,
                                                &size);
    if (status == OK)
        status = channel_write(channel, outgoing.bytes, size, transfers,
                               CONSOLE_BOOTSTRAP_HANDLES);
    if (status != OK) {
        for (i = 0; i < CONSOLE_BOOTSTRAP_HANDLES; i++)
            if (handles[i] != HANDLE_INVALID)
                handle_close(handles[i]);
        if (mine != HANDLE_INVALID)
            handle_close(mine);
        return status;
    }
    if (control != HANDLE_INVALID)
        handle_close(control);
    control = mine;
    send_options();
    return OK;
}

void console_started(const PLATFORM *platform)
{
    if (holder == DISPLAY_UNRELEASED)
        return;
    if (holder == DISPLAY_CONSOLE)
        send_display(platform, DISPLAY_FROM_KERNEL);
    login_console_started();
    console_connect();
}

int display_released(UINT32 bootstrap)
{
    UINT32 observed;
    UINT32 status = object_wait_one(bootstrap, CHANNEL_PEER_CLOSED,
                                    DEADLINE_INFINITE, &observed);

    handle_close(bootstrap);
    if (status != OK) {
        say_status("display release wait failed", status);
        return 0;
    }
    return 1;
}

void console_defer_display(void)
{
    holder = DISPLAY_COMPOSITOR;
}

void console_display_after(const PLATFORM *platform)
{
    holder = DISPLAY_CONSOLE;
    if (control != HANDLE_INVALID)
        send_display(platform, DISPLAY_FROM_COMPOSITOR);
}
