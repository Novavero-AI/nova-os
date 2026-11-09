/* The window test client's start (window_test.h). Each start, the first and
   every restart, is a new process in the root job that gets a connection of
   its own, as each start of the compositor's client must (docs/spec/display.md
   D5, D19's phase 3 arrangement, rule 9). */

#include "window_test.h"
#include "compositor.h"
#include "display_protocol.h"
#include "tests.h"
#include "wintest.h"

/* The messages its bootstrap channel queues: the one. */
#define BOOTSTRAP_CAPACITY 1

static void close_transfers(const HANDLE_TRANSFER *handles, UINT32 count)
{
    UINT32 i;

    for (i = 0; i < count; i++)
        if (handles[i].handle != HANDLE_INVALID)
            handle_close(handles[i].handle);
}

/* Lay out wintest.exe with its bootstrap message written (wintest.h):
   the transaction ID of 0 and the debug log, its process and a connection
   the compositor has issued. */
static UINT32 load_client(const PLATFORM *platform, PROGRAM *program,
                          UINT32 *bootstrap)
{
    static const UINT32 transaction = 0;
    HANDLE_TRANSFER handles[WINTEST_BOOTSTRAP_HANDLES];
    UINT32 mine = HANDLE_INVALID, theirs = HANDLE_INVALID, status;
    UINT32 connection = HANDLE_INVALID, i;

    status = load_boot_file(platform->job, boot_file(WINTEST_FILE), program);
    if (status != OK)
        return status;
    for (i = 0; i < WINTEST_BOOTSTRAP_HANDLES; i++) {
        handles[i].handle = HANDLE_INVALID;
        handles[i].rights = RIGHTS_SAME;
    }
    handles[WINTEST_BOOTSTRAP_CONNECTION].rights = DISPLAY_CONNECTION_RIGHTS;
    if (!launch_handles(platform->debug_log, program, handles))
        status = ERR_NO_MEMORY;
    if (status == OK)
        status = compositor_connect(WINTEST_LABEL, DISPLAY_LAYER_SYSTEM, 0,
                                    &connection);
    if (status == OK) {
        handles[WINTEST_BOOTSTRAP_CONNECTION].handle = connection;
        status = channel_create(BOOTSTRAP_CAPACITY, &mine, &theirs);
    }
    if (status == OK)
        status = channel_write(mine, &transaction, WINTEST_BOOTSTRAP_BYTES,
                               handles, WINTEST_BOOTSTRAP_HANDLES);
    if (mine != HANDLE_INVALID)
        handle_close(mine);
    /* A write that fails moves no handle (ipc.md I2), so each one made is
       still init's to close; after a write that succeeds none is. */
    if (status != OK) {
        close_transfers(handles, WINTEST_BOOTSTRAP_HANDLES);
        if (theirs != HANDLE_INVALID)
            handle_close(theirs);
        handle_close(program->thread);
        handle_close(program->process);
        return status;
    }
    *bootstrap = theirs;
    return OK;
}

int window_test_start(const PLATFORM *platform)
{
    if (!tests_flag(TEST_FLAG_WINDOW) || !compositor_wanted())
        return 1;
    if (!boot_file(WINTEST_FILE)) {
        say("nova-init: window test unavailable reason=\"no " WINTEST_FILE
            "\"\r\n");
        return 0;
    }
    return platform_keep(platform, "wintest", load_client);
}
