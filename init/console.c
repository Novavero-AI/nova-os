/* init's part in the console. The console is the platform driver's
   input client (platform.c), which starts it and asks this file for its
   bootstrap message, so each start of the console, the first and every
   restart, gets a control channel of its own; the end init keeps is the
   one the background, the next display and sessions go to. The kernel's
   background goes to a console started before the release, which
   prepares its panel from it and answers before init reports its
   startup done (console_prepare). The display goes only after the
   kernel has released it, so a console started before the release gets
   it from console_start, and one started after from console_started,
   which also gives login a session on it (login_start.c); login's first
   session comes with its own start. */

#include "console.h"
#include "console_protocol.h"
#include "init.h"
#include "input_protocol.h"
#include "login_start.h"
#include "tests.h"

#define PIXEL_BYTES 4

/* The rights each handle the console gets travels with: the debug log
   to write to it, its process to map memory, its control channel to read
   from and answer the background on, its end of a session to serve it,
   the framebuffer's resource to make sections over it, and the
   background's sections to map them to read; none to pass on. */
#define CONSOLE_LOG_RIGHTS RESOURCE_RIGHT_USE
#define CONSOLE_SELF_RIGHTS PROCESS_RIGHT_MAP
#define CONSOLE_CONTROL_RIGHTS                                            \
    (CHANNEL_RIGHT_READ | CHANNEL_RIGHT_WRITE | RIGHT_WAIT)
#define CONSOLE_END_RIGHTS                                                \
    (CHANNEL_RIGHT_READ | CHANNEL_RIGHT_WRITE | RIGHT_WAIT)
#define FRAMEBUFFER_RIGHTS RESOURCE_RIGHT_USE
#define BACKGROUND_RIGHTS SECTION_RIGHT_READ

/* On a test disk the console is told of a display MISMATCH_ROWS taller
   than the one it is given, so that the kernel's background does not
   match it and the console computes its panel's Limb at the display, as
   a restarted console does (console.md C5); ci/boot.py checks that every
   test disk's boot takes that path and draws the screen the system
   disk's boot draws from the background. */
#define MISMATCH_ROWS 1

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

void console_prepare(const PLATFORM *platform, BOOT_BACKGROUND *background)
{
    CONSOLE_SETUP_BACKGROUND_MESSAGE message;
    HANDLE_TRANSFER sections[BOOTSTRAP_BACKGROUND_SECTIONS_MAX];
    RECEIVED_HANDLE handle;
    UINT32 answer = 0, byte_count = 0, handle_count, observed, i, status;

    if (control == HANDLE_INVALID) {
        boot_background_close(background);
        return;
    }
    message.setup.transaction = 0;
    message.setup.kind = CONSOLE_SETUP_BACKGROUND;
    message.display = platform->boot->display;
    if (tests_enabled())
        message.display.height += MISMATCH_ROWS;
    message.background = background->label;
    for (i = 0; i < background->count; i++) {
        sections[i].handle = background->sections[i];
        sections[i].rights = BACKGROUND_RIGHTS;
    }
    status = channel_write(control, &message, sizeof message, sections,
                           background->count);
    if (status != OK) {
        boot_background_close(background);
        if (status != ERR_PEER_CLOSED)
            say_status("background send failed", status);
        return;
    }
    background->count = 0;
    status = object_wait_one(control, CHANNEL_READABLE | CHANNEL_PEER_CLOSED,
                             DEADLINE_INFINITE, &observed);
    if (status == OK)
        status = channel_read(control, &answer, sizeof answer, &handle, 0,
                              &byte_count, &handle_count);
    if (status != OK && status != ERR_PEER_CLOSED)
        say_status("console answer failed", status);
    else if (status == OK &&
             (byte_count != CONSOLE_PREPARED_SIZE || answer != 0))
        say("nova-init: console answer refused reason=malformed\r\n");
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
