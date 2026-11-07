/* Starting the type specimen (specimen_start.h). init finds its store
   path by listing /store, as login_start.c finds the session's, opens
   the path's closure through the store service and starts the program
   from it with the closure as its view's /store, as store.c starts the
   test program. Its bootstrap channel holds two messages before it
   starts: the launch message, then the display. init keeps no handle to
   it: it draws once and exits, and the screen keeps what it drew. */

#include "specimen_start.h"
#include "console.h"
#include "launch.h"
#include "services.h"
#include "specimen.h"
#include "store_image.h"

#define STORE_DIRECTORY "/store/"
#define STORE_PATH "/store"

/* What the program's view grants: read and list its closure, and map
   its files executable, which a launcher does for it. */
#define CLOSURE_RIGHTS (IO_RIGHT_READ | IO_RIGHT_EXECUTE | IO_RIGHT_LIST)

/* Its bootstrap channel queues the launch message and the display. */
#define BOOTSTRAP_CAPACITY 2

static VIEW names;
static UINT8 base[STORE_BASE_NAME_MAX];
static UINT32 base_length;
static char path[sizeof STORE_DIRECTORY + STORE_BASE_NAME_MAX +
                 sizeof SPECIMEN_FILE];
static struct {
    LAUNCH_HEADER header;
    LAUNCH_VIEW_ENTRY entry;
} launch;

static UINT32 length_of(const char *text)
{
    UINT32 n = 0;

    while (text[n])
        n++;
    return n;
}

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

static void say_status(const char *what, UINT32 status)
{
    say("nova-init: ");
    say(what);
    say(", status ");
    say_number(status);
    say("\r\n");
}

/* Note the store path whose name is SPECIMEN_NAME. */
static void find_specimen(void *context, const UINT8 *name, UINT32 length,
                          UINT32 kind)
{
    UINT32 name_length = length_of(SPECIMEN_NAME);

    (void)context;
    if (kind == IO_KIND_DIRECTORY &&
        length == STORE_HASH_CHARS + 1 + name_length &&
        same(name + STORE_HASH_CHARS + 1, (const UINT8 *)SPECIMEN_NAME,
             name_length)) {
        copy(base, name, length);
        base_length = length;
    }
}

/* The program's file, laid out in a new process, and the handles of its
   launch message: the debug log, its process and its closure. */
static UINT32 load(const PLATFORM *platform, const STORE_SERVICES *services,
                   PROGRAM *program, HANDLE_TRANSFER *handles)
{
    IO_ATTRIBUTES attributes;
    UINT32 at = length_of(STORE_DIRECTORY), status;

    names.count = 0;
    base_length = 0;
    status = view_add(&names, "/", services->names);
    if (status == OK)
        status = view_list(&names, STORE_PATH, find_specimen, 0);
    if (status == OK && !base_length)
        status = ERR_NOT_FOUND;
    if (status != OK)
        return status;
    copy((UINT8 *)path, (const UINT8 *)STORE_DIRECTORY, at);
    copy((UINT8 *)path + at, base, base_length);
    at += base_length;
    path[at++] = '/';
    copy((UINT8 *)path + at, (const UINT8 *)SPECIMEN_FILE,
         length_of(SPECIMEN_FILE) + 1);
    status = io_open(services->control, STORE_OPEN_CLOSURE, base, base_length,
                     CLOSURE_RIGHTS, &handles[LAUNCH_FIRST_DIRECTORY].handle,
                     &attributes);
    if (status != OK)
        return status;
    status = load_from_store(platform->job, &names, path, program);
    if (status != OK) {
        handle_close(handles[LAUNCH_FIRST_DIRECTORY].handle);
        return status;
    }
    if (!launch_handles(platform->debug_log, program, handles)) {
        handle_close(handles[LAUNCH_FIRST_DIRECTORY].handle);
        handle_close(program->thread);
        handle_close(program->process);
        return ERR_NO_MEMORY;
    }
    return OK;
}

void specimen_start(const PLATFORM *platform, const STORE_SERVICES *services,
                    UINT32 bootstrap)
{
    HANDLE_TRANSFER handles[LAUNCH_FIRST_DIRECTORY + 1], framebuffer;
    CONSOLE_SETUP_DISPLAY_MESSAGE display;
    UINT32 mine = HANDLE_INVALID, theirs = HANDLE_INVALID, status, i;
    PROGRAM program;
    /* Whether each message is written: its handles have moved into the
       channel, which closes them when it is destroyed unread. */
    int launched = 0, sent = 0;

    if (!display_released(bootstrap))
        return;
    handles[LAUNCH_FIRST_DIRECTORY].rights = RIGHTS_SAME;
    status = load(platform, services, &program, handles);
    if (status != OK) {
        say_status("the specimen does not load from /store", status);
        return;
    }
    launch.header.view_count = 1;
    launch.header.argument_bytes = 0;
    launch.entry.length = length_of(STORE_PATH);
    for (i = 0; i < LAUNCH_PREFIX_MAX; i++)
        launch.entry.prefix[i] = 0;
    copy(launch.entry.prefix, (const UINT8 *)STORE_PATH, launch.entry.length);
    status = display_message(platform, &display, &framebuffer);
    if (status == OK)
        status = channel_create(BOOTSTRAP_CAPACITY, &mine, &theirs);
    if (status == OK)
        status = channel_write(mine, &launch, sizeof launch, handles,
                               LAUNCH_FIRST_DIRECTORY + 1);
    launched = status == OK;
    if (status == OK)
        status = channel_write(mine, &display, sizeof display, &framebuffer,
                               1);
    sent = status == OK;
    if (status == OK)
        status = program_start(&program, theirs);
    if (mine != HANDLE_INVALID)
        handle_close(mine);
    if (status != OK) {
        say_status("the specimen did not start", status);
        if (!launched)
            for (i = 0; i <= LAUNCH_FIRST_DIRECTORY; i++)
                handle_close(handles[i].handle);
        if (!sent && framebuffer.handle != HANDLE_INVALID)
            handle_close(framebuffer.handle);
        if (theirs != HANDLE_INVALID)
            handle_close(theirs);
        if (program.thread != HANDLE_INVALID)
            handle_close(program.thread);
        handle_close(program.process);
        return;
    }
    say("nova-init: the display sent to ");
    say(path);
    say(" in place of the console\r\n");
    handle_close(program.process);
}
