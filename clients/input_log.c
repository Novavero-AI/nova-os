/* The input driver's client in the first set, \nova\inputlog.exe: it
   logs each key event the driver sends (docs/spec/input.md), on each
   endpoint the platform driver hands it in turn on its bootstrap
   channel, and ends once the platform driver closes that channel. */

#include "input_protocol.h"
#include "log.h"
#include "native.h"

/* It ends with EXIT_SUPERVISOR_GONE once the platform driver has closed
   its bootstrap channel, else with EXIT_FAILED. Both lie above the
   exception vectors a faulting process ends with (docs/spec/objects.md
   O10). */
#define EXIT_SUPERVISOR_GONE 100
#define EXIT_FAILED 101

/* A message as large as a channel carries, so that one which is not a
   key event can still be read, and so leaves the queue. */
static union {
    INPUT_EVENT event;
    UINT8 bytes[CHANNEL_MAX_BYTES];
} message;

static RECEIVED_HANDLE handles[CHANNEL_MAX_HANDLES];

/* The handle the next message on the bootstrap channel carries, waiting
   for one, which must be of type; HANDLE_INVALID once the platform
   driver has closed the channel and none is left. A message of another
   shape ends the client. */
static UINT32 take_handle(UINT32 bootstrap, UINT32 type)
{
    RECEIVED_HANDLE handle;
    UINT32 transaction, byte_count, handle_count, observed, status;

    for (;;) {
        status = channel_read(bootstrap, &transaction, sizeof transaction,
                              &handle, 1, &byte_count, &handle_count);
        if (status != ERR_SHOULD_WAIT)
            break;
        status = object_wait_one(bootstrap,
                                 CHANNEL_READABLE | CHANNEL_PEER_CLOSED,
                                 DEADLINE_INFINITE, &observed);
        if (status != OK)
            break;
    }
    if (status == ERR_PEER_CLOSED)
        return HANDLE_INVALID;
    if (status == OK && byte_count == INPUT_CLIENT_MESSAGE_SIZE &&
        transaction == 0 && handle_count == 1 && handle.type == type)
        return handle.handle;
    say("nova-input-log: a message from the platform driver that is not "
        "the one expected, status ");
    say_number(status);
    say("\r\n");
    process_exit(EXIT_FAILED);
}

static void say_event(const INPUT_EVENT *event)
{
    say("nova-input-log: key ");
    say_number(event->code);
    say(event->value == INPUT_KEY_PRESSED    ? " pressed"
        : event->value == INPUT_KEY_RELEASED ? " released"
                                             : " repeated");
    say(" at ");
    say_number(event->time);
    say(" ns\r\n");
}

/* Log the next message on events; a message that is not a key event is
   logged as such, and any handles it carries are closed. Returns the
   status of the read. */
static UINT32 take(UINT32 events)
{
    const INPUT_EVENT *event = &message.event;
    UINT32 byte_count, handle_count, i;
    UINT32 status = channel_read(events, message.bytes, sizeof message.bytes,
                                 handles, CHANNEL_MAX_HANDLES, &byte_count,
                                 &handle_count);

    if (status != OK)
        return status;
    for (i = 0; i < handle_count; i++)
        handle_close(handles[i].handle);
    if (byte_count == sizeof *event && !handle_count &&
        event->transaction == 0 && event->type == INPUT_TYPE_KEY &&
        event->value <= INPUT_KEY_REPEATED && event->reserved == 0) {
        say_event(event);
    } else {
        say("nova-input-log: a message of ");
        say_number(byte_count);
        say(" bytes and ");
        say_number(handle_count);
        say(" handles that is not a key event\r\n");
    }
    return OK;
}

/* Log the key events on events until the driver closes it. */
static void log_events(UINT32 events)
{
    UINT32 status, observed;

    for (;;) {
        status = take(events);
        if (status == ERR_SHOULD_WAIT)
            status = object_wait_one(events,
                                     CHANNEL_READABLE | CHANNEL_PEER_CLOSED,
                                     DEADLINE_INFINITE, &observed);
        if (status == ERR_PEER_CLOSED) {
            say("nova-input-log: the input driver closed the endpoint\r\n");
            return;
        }
        if (status != OK) {
            say("nova-input-log: cannot read a key event, status ");
            say_number(status);
            say("\r\n");
            process_exit(EXIT_FAILED);
        }
    }
}

_Noreturn void input_log_main(UINT32 bootstrap, UINT64 argument)
{
    UINT32 log = take_handle(bootstrap, TYPE_RESOURCE);
    UINT32 events;

    (void)argument;
    if (log == HANDLE_INVALID)
        process_exit(EXIT_FAILED);
    say_open(log);
    while ((events = take_handle(bootstrap, TYPE_CHANNEL)) !=
           HANDLE_INVALID) {
        log_events(events);
        handle_close(events);
    }
    say("nova-input-log: the platform driver closed its channel\r\n");
    process_exit(EXIT_SUPERVISOR_GONE);
}
