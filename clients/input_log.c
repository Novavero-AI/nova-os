/* The input driver's client in the first set, \nova\inputlog.exe: it
   logs each key event the driver sends (docs/spec/input.md) until the
   driver closes the endpoint. The platform driver starts it with the
   endpoint the driver reported. */

#include "input_protocol.h"
#include "log.h"
#include "native.h"

/* It ends with EXIT_DRIVER_GONE once the driver has closed the endpoint
   and every event queued there is logged, else with EXIT_FAILED. Both lie
   above the exception vectors a faulting process ends with
   (docs/spec/objects.md O10). */
#define EXIT_DRIVER_GONE 100
#define EXIT_FAILED 101

/* A message as large as a channel carries, so that one which is not a
   key event can still be read, and so leaves the queue. */
static union {
    INPUT_EVENT event;
    UINT8 bytes[CHANNEL_MAX_BYTES];
} message;

static RECEIVED_HANDLE handles[CHANNEL_MAX_HANDLES];

/* The endpoint, from the bootstrap message, once the log is open. */
static UINT32 read_bootstrap(UINT32 bootstrap)
{
    UINT32 byte_count, handle_count, transaction;
    RECEIVED_HANDLE received[INPUT_CLIENT_HANDLES];

    if (channel_read(bootstrap, &transaction, sizeof transaction, received,
                     INPUT_CLIENT_HANDLES, &byte_count, &handle_count) != OK ||
        handle_count != INPUT_CLIENT_HANDLES ||
        received[INPUT_CLIENT_HANDLE_EVENTS].type != TYPE_CHANNEL)
        process_exit(EXIT_FAILED);
    say_open(received[INPUT_CLIENT_HANDLE_DEBUG_LOG].handle);
    handle_close(bootstrap);
    return received[INPUT_CLIENT_HANDLE_EVENTS].handle;
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

_Noreturn void input_log_main(UINT32 bootstrap, UINT64 argument)
{
    UINT32 events = read_bootstrap(bootstrap);
    UINT32 status, observed;

    (void)argument;
    for (;;) {
        status = take(events);
        if (status == ERR_SHOULD_WAIT)
            status = object_wait_one(events,
                                     CHANNEL_READABLE | CHANNEL_PEER_CLOSED,
                                     DEADLINE_INFINITE, &observed);
        if (status == ERR_PEER_CLOSED) {
            say("nova-input-log: the input driver closed the endpoint\r\n");
            process_exit(EXIT_DRIVER_GONE);
        }
        if (status != OK) {
            say("nova-input-log: cannot read a key event, status ");
            say_number(status);
            say("\r\n");
            process_exit(EXIT_FAILED);
        }
    }
}
