/* The calls this kernel implements, numbered as S8 numbers them
   (native_abi.h). Every other number, 0 among them, fails with
   ERR_NOT_SUPPORTED. An argument declared 32-bit is read from its low 32
   bits (S4). Each call that takes a handle checks it before anything
   else (docs/spec/objects.md O3) and holds a reference to its object
   while it uses it. */

#include "syscall.h"
#include "../object/channel.h"
#include "../object/process.h"
#include "../object/resource.h"
#include "../x86_64/clock.h"
#include "../x86_64/serial.h"
#include "../x86_64/user.h"

typedef STATUS (*CALL)(const UINT64 *arguments, UINT64 *results);

typedef struct {
    CALL call;
    UINT32 arguments;
} CALL_ENTRY;

static HANDLE_TABLE *caller_handles(void)
{
    return process_current()->handles;
}

static STATUS handle_close_call(const UINT64 *arguments, UINT64 *results)
{
    (void)results;
    return handle_close(caller_handles(), (UINT32)arguments[0]);
}

/* channel_read(channel, bytes, byte capacity, handles, handle capacity):
   byte count and handle count. */
static STATUS channel_read_call(const UINT64 *arguments, UINT64 *results)
{
    OBJECT *channel;
    UINT32 byte_count, handle_count;
    STATUS status = handle_get(caller_handles(), (UINT32)arguments[0],
                               TYPE_CHANNEL, CHANNEL_RIGHT_READ, &channel);

    if (status != OK)
        return status;
    status = channel_read_message((ENDPOINT *)channel, caller_handles(),
                                  arguments[1], (UINT32)arguments[2],
                                  arguments[3], (UINT32)arguments[4],
                                  &byte_count, &handle_count);
    object_release(channel);
    results[0] = byte_count;
    results[1] = handle_count;
    return status;
}

static STATUS clock_monotonic_call(const UINT64 *arguments, UINT64 *results)
{
    (void)arguments;
    results[0] = clock_monotonic();
    return OK;
}

static STATUS process_exit_call(const UINT64 *arguments, UINT64 *results)
{
    (void)results;
    process_exit((UINT32)arguments[0]);
}

/* process_info(process): state and exit code. */
static STATUS process_info_call(const UINT64 *arguments, UINT64 *results)
{
    OBJECT *process;
    UINT32 state, exit_code;
    STATUS status = handle_get(caller_handles(), (UINT32)arguments[0],
                               TYPE_PROCESS, PROCESS_RIGHT_INSPECT, &process);

    if (status != OK)
        return status;
    process_info((PROCESS *)process, &state, &exit_code);
    object_release(process);
    results[0] = state;
    results[1] = exit_code;
    return OK;
}

static STATUS thread_exit_call(const UINT64 *arguments, UINT64 *results)
{
    (void)arguments;
    (void)results;
    process_thread_exit();
}

/* debug_write(resource, bytes, byte count). The bytes are copied whole
   before any is written, so a call that fails writes nothing (S7). */
static STATUS debug_write_call(const UINT64 *arguments, UINT64 *results)
{
    UINT8 bytes[DEBUG_WRITE_MAX_BYTES];
    UINT64 count = arguments[2];
    OBJECT *resource;
    STATUS status = handle_get(caller_handles(), (UINT32)arguments[0],
                               TYPE_RESOURCE, RESOURCE_RIGHT_USE, &resource);

    (void)results;
    if (status != OK)
        return status;
    if (!resource_covers((RESOURCE *)resource, SYSTEM_PRIVILEGE_DEBUG_LOG))
        status = ERR_ACCESS_DENIED;
    else if (count > DEBUG_WRITE_MAX_BYTES)
        status = ERR_OUT_OF_RANGE;
    else
        status = copy_from_user(bytes, arguments[1], count);
    object_release(resource);
    if (status == OK)
        serial_write_bytes(bytes, count);
    return status;
}

#define CALL_COUNT (CALL_DEBUG_WRITE + 1)

static const CALL_ENTRY calls[CALL_COUNT] = {
    [CALL_HANDLE_CLOSE] = {handle_close_call, 1},
    [CALL_CHANNEL_READ] = {channel_read_call, 5},
    [CALL_CLOCK_MONOTONIC] = {clock_monotonic_call, 0},
    [CALL_PROCESS_EXIT] = {process_exit_call, 1},
    [CALL_PROCESS_INFO] = {process_info_call, 1},
    [CALL_THREAD_EXIT] = {thread_exit_call, 0},
    [CALL_DEBUG_WRITE] = {debug_write_call, 3},
};

UINT32 syscall_argument_count(UINT64 number)
{
    return number < CALL_COUNT ? calls[number].arguments : 0;
}

STATUS syscall_invoke(UINT64 number, const UINT64 *arguments,
                      UINT64 *results)
{
    if (number >= CALL_COUNT || !calls[number].call)
        return ERR_NOT_SUPPORTED;
    return calls[number].call(arguments, results);
}
