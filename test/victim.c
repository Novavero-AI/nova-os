/* The program the logout disk's driver runs (driver.c, docs/spec/
   accounts.md A7): it is started from /store as login starts a program
   for the shell, with a view of its closure, and its one argument either
   is VICTIM_ONCE, which ends it at once, or names it, and then it stays
   running, logging a beat every BEAT_NS, until something kills it. A
   beat that follows the end of the session that ran it is the failure
   ci/boot.py's logout check looks for. */

#include "launch.h"
#include "log.h"
#include "logout.h"
#include "native.h"

#define BEAT_NS 100000000ULL

/* The launch message: its header, the largest view and the most
   arguments. */
static UINT8 launch[sizeof(LAUNCH_HEADER) +
                    LAUNCH_VIEW_MAX * sizeof(LAUNCH_VIEW_ENTRY) +
                    LAUNCH_ARGUMENTS_MAX];

static int is_once(const char *argument)
{
    const char *once = VICTIM_ONCE;
    UINT32 i;

    for (i = 0; once[i] && argument[i] == once[i]; i++)
        ;
    return once[i] == 0 && argument[i] == 0;
}

/* The argument of the launch message, which holds its debug log. */
static const char *read_launch(UINT32 bootstrap)
{
    RECEIVED_HANDLE handles[LAUNCH_FIRST_DIRECTORY + LAUNCH_VIEW_MAX];
    const LAUNCH_HEADER *header = (const LAUNCH_HEADER *)launch;
    const char *argument;
    UINT32 bytes, count;

    if (channel_read(bootstrap, launch, sizeof launch, handles,
                     LAUNCH_FIRST_DIRECTORY + LAUNCH_VIEW_MAX, &bytes,
                     &count) != OK ||
        count < LAUNCH_FIRST_DIRECTORY ||
        handles[LAUNCH_DEBUG_LOG].type != TYPE_RESOURCE ||
        bytes < sizeof *header || header->view_count > LAUNCH_VIEW_MAX ||
        header->argument_bytes == 0 ||
        bytes != sizeof *header +
                     header->view_count * sizeof(LAUNCH_VIEW_ENTRY) +
                     header->argument_bytes)
        process_exit(VICTIM_EXIT_FAILED);
    handle_close(bootstrap);
    say_open(handles[LAUNCH_DEBUG_LOG].handle);
    argument = (const char *)launch + sizeof *header +
               header->view_count * sizeof(LAUNCH_VIEW_ENTRY);
    if (argument[header->argument_bytes - 1] != 0)
        process_exit(VICTIM_EXIT_FAILED);
    return argument;
}

_Noreturn void victim_main(UINT32 bootstrap, UINT64 argument)
{
    static WAIT_ENTRY nothing;
    const char *id;
    UINT64 now, beats;

    (void)argument;
    id = read_launch(bootstrap);
    if (is_once(id))
        process_exit(VICTIM_EXIT_ENDED);
    for (beats = 1;; beats++) {
        say("nova-victim: beat id=");
        say_value(id);
        say(" n=");
        say_number(beats);
        say("\r\n");
        if (clock_monotonic(&now) != OK)
            process_exit(VICTIM_EXIT_FAILED);
        object_wait_many(&nothing, 0, now + BEAT_NS);
    }
}
