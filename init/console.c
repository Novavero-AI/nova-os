/* init's part in the console. The console is the platform driver's
   input client (platform.c), which starts it and asks this file for its
   bootstrap message, so each start of the console, the first and every
   restart, gets a control channel of its own; the end init keeps is the
   one the next display and sessions go to. The display goes only after
   the kernel has released it, so a console started before the release
   gets it from console_start, and one started after from
   console_started, which also gives login a session on it
   (login_start.c); login's first session comes with its own start. */

#include "console.h"
#include "console_protocol.h"
#include "init.h"
#include "input_protocol.h"
#include "login_start.h"

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

static UINT32 control = HANDLE_INVALID;
static int released;

static void close_handles(HANDLE_TRANSFER *handles, UINT32 count)
{
    UINT32 i;

    for (i = 0; i < count; i++)
        if (handles[i].handle != HANDLE_INVALID) {
            handle_close(handles[i].handle);
            handles[i].handle = HANDLE_INVALID;
        }
}

UINT32 display_message(const PLATFORM *platform,
                       CONSOLE_SETUP_DISPLAY_MESSAGE *message,
                       HANDLE_TRANSFER *framebuffer)
{
    const BOOTSTRAP_DISPLAY *d = &platform->boot->display;
    UINT64 first = d->base - d->base % PAGE_SIZE;
    UINT64 end = (d->base + (UINT64)d->stride * d->height * PIXEL_BYTES +
                  PAGE_SIZE - 1) /
                 PAGE_SIZE * PAGE_SIZE;

    framebuffer->handle = HANDLE_INVALID;
    framebuffer->rights = FRAMEBUFFER_RIGHTS;
    message->setup.transaction = 0;
    message->setup.kind = CONSOLE_SETUP_DISPLAY;
    message->display = *d;
    return resource_create(platform->mmio, first, end - first,
                           &framebuffer->handle);
}

/* Send the display and a resource covering exactly the framebuffer's
   pages to the console. */
static void send_display(const PLATFORM *platform)
{
    CONSOLE_SETUP_DISPLAY_MESSAGE message;
    HANDLE_TRANSFER framebuffer;
    UINT32 status = display_message(platform, &message, &framebuffer);

    if (status == OK)
        status = channel_write(control, &message, sizeof message,
                               &framebuffer, 1);
    if (status != OK) {
        close_handles(&framebuffer, 1);
        say_status("display send failed", status);
    }
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
    say_status("console session failed", status);
    return HANDLE_INVALID;
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
    login_console_started();
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

void console_start(const PLATFORM *platform, UINT32 bootstrap)
{
    if (!display_released(bootstrap))
        return;
    released = 1;
    if (control == HANDLE_INVALID) {
        say("nova-init: display unused reason=\"no console\"\r\n");
        return;
    }
    send_display(platform);
}
