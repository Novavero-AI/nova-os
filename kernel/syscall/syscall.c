/* The calls this kernel implements, numbered as S8 numbers them
   (native_abi.h). Every other number, 0 among them, fails with
   ERR_NOT_SUPPORTED. An argument declared 32-bit is read from its low 32
   bits (S4). Each call that takes a handle checks it before anything
   else (docs/spec/objects.md O3) and holds a reference to its object
   while it uses it. A call that creates an object reserves the entry
   for its handle first, so a full table fails it before anything is
   made (S6). */

#include "syscall.h"
#include "cpu.h"
#include "../object/channel.h"
#include "../object/process.h"
#include "../object/resource.h"
#include "../x86_64/clock.h"
#include "../x86_64/serial.h"
#include "../x86_64/user.h"

#define PROCESS_OPTIONS (PROCESS_OPTION_32_BIT | PROCESS_OPTION_WRITE_EXECUTE)

typedef STATUS (*CALL)(const UINT64 *arguments, UINT64 *results);

/* A call, how many arguments it takes, and whether the 32-bit lane
   serves it: every result fits in 32 bits and a 32-bit process has a
   use for it (S11). */
typedef struct {
    CALL call;
    UINT32 arguments;
    UINT32 lane_32_bit;
} CALL_ENTRY;

static HANDLE_TABLE *caller_handles(void)
{
    return process_current()->handles;
}

/* Place object, which the caller holds with its creator's handle, in the
   entry reserved at index, and report the handle's value. */
static void give(UINT32 index, OBJECT *object, UINT32 rights, UINT64 *result)
{
    *result = handle_place(caller_handles(), index, object, rights);
}

static STATUS handle_close_call(const UINT64 *arguments, UINT64 *results)
{
    (void)results;
    return handle_close(caller_handles(), (UINT32)arguments[0]);
}

/* object_wait_one(handle, signals, deadline): observed signals. */
static STATUS object_wait_one_call(const UINT64 *arguments, UINT64 *results)
{
    OBJECT *object;
    WAITABLE *waitable;
    UINT32 observed;
    STATUS status = handle_get_waitable(caller_handles(), (UINT32)arguments[0],
                                        RIGHT_WAIT, &object, &waitable);

    if (status != OK)
        return status;
    status = wait_one(waitable, (UINT32)arguments[1], arguments[2], &observed);
    object_release(object);
    results[0] = observed;
    return status;
}

/* object_wait_many(items, count, deadline): writes each WAIT_ENTRY's
   observed signals, also when the deadline passes. */
static STATUS object_wait_many_call(const UINT64 *arguments, UINT64 *results)
{
    WAIT_ENTRY entries[WAIT_MANY_MAX];
    WAIT_ITEM items[WAIT_MANY_MAX];
    OBJECT *objects[WAIT_MANY_MAX];
    UINT64 address = arguments[0];
    UINT32 count = (UINT32)arguments[1];
    UINT32 taken, i;
    STATUS status;

    (void)results;
    if (count > WAIT_MANY_MAX)
        return ERR_OUT_OF_RANGE;
    status = copy_from_user(entries, address, count * sizeof(WAIT_ENTRY));
    for (taken = 0; status == OK && taken < count;) {
        status = handle_get_waitable(caller_handles(), entries[taken].handle,
                                     RIGHT_WAIT, &objects[taken],
                                     &items[taken].object);
        if (status == OK) {
            items[taken].signals = entries[taken].signals;
            taken++;
        }
    }
    if (status == OK) {
        status = wait_many(items, count, arguments[2]);
        for (i = 0; i < count; i++)
            entries[i].observed = items[i].observed;
        if (status == OK || status == ERR_TIMED_OUT) {
            STATUS copied =
                copy_to_user(address, entries, count * sizeof(WAIT_ENTRY));

            if (copied != OK)
                status = copied;
        }
    }
    for (i = 0; i < taken; i++)
        object_release(objects[i]);
    return status;
}

/* channel_create(capacity): two handles. */
static STATUS channel_create_call(const UINT64 *arguments, UINT64 *results)
{
    HANDLE_TABLE *table = caller_handles();
    ENDPOINT *first, *second;
    UINT32 index[2];
    STATUS status = handle_reserve(table, &index[0]);

    if (status != OK)
        return status;
    status = handle_reserve(table, &index[1]);
    if (status != OK) {
        handle_unreserve(table, index[0]);
        return status;
    }
    status = channel_create((UINT32)arguments[0], &first, &second);
    if (status != OK) {
        handle_unreserve(table, index[0]);
        handle_unreserve(table, index[1]);
        return status;
    }
    give(index[0], &first->object, CHANNEL_RIGHTS, &results[0]);
    give(index[1], &second->object, CHANNEL_RIGHTS, &results[1]);
    return OK;
}

/* channel_write(channel, bytes, byte count, handles, handle count). */
static STATUS channel_write_call(const UINT64 *arguments, UINT64 *results)
{
    OBJECT *channel;
    STATUS status = handle_get(caller_handles(), (UINT32)arguments[0],
                               TYPE_CHANNEL, CHANNEL_RIGHT_WRITE, &channel);

    (void)results;
    if (status != OK)
        return status;
    status = channel_write_message((ENDPOINT *)channel, caller_handles(),
                                   arguments[1], (UINT32)arguments[2],
                                   arguments[3], (UINT32)arguments[4]);
    object_release(channel);
    return status;
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

/* process_create(job, options): a handle. PROCESS_OPTION_WRITE_EXECUTE
   is among the rights the call needs: the job handle's WRITE_EXECUTE
   (O5). */
static STATUS process_create_call(const UINT64 *arguments, UINT64 *results)
{
    HANDLE_TABLE *table = caller_handles();
    UINT32 options = (UINT32)arguments[1];
    UINT32 needed = JOB_RIGHT_CREATE_PROCESS |
                    ((options & PROCESS_OPTION_WRITE_EXECUTE)
                         ? JOB_RIGHT_WRITE_EXECUTE
                         : 0);
    OBJECT *job;
    PROCESS *process;
    UINT32 index;
    STATUS status =
        handle_get(table, (UINT32)arguments[0], TYPE_JOB, needed, &job);

    if (status != OK)
        return status;
    if (options & ~PROCESS_OPTIONS)
        status = ERR_INVALID_ARGS;
    if (status == OK)
        status = handle_reserve(table, &index);
    if (status == OK) {
        status = process_create((JOB *)job, options, &process);
        if (status == OK)
            give(index, &process->object, PROCESS_RIGHTS, &results[0]);
        else
            handle_unreserve(table, index);
    }
    object_release(job);
    return status;
}

/* process_start(process, thread, entry, stack, bootstrap handle,
   argument). The bootstrap handle moves from the caller's table to the
   new process's with its rights; interrupts stay disabled from its check
   to its removal, so the move is whole or not at all. */
static STATUS process_start_call(const UINT64 *arguments, UINT64 *results)
{
    HANDLE_TABLE *table = caller_handles();
    UINT32 bootstrap = (UINT32)arguments[4];
    OBJECT *process, *thread, *object;
    UINT32 rights;
    UINT64 flags;
    STATUS status = handle_get(table, (UINT32)arguments[0], TYPE_PROCESS,
                               PROCESS_RIGHT_CREATE_THREAD, &process);

    (void)results;
    if (status != OK)
        return status;
    status = handle_get(table, (UINT32)arguments[1], TYPE_THREAD,
                        THREAD_RIGHT_START, &thread);
    if (status != OK) {
        object_release(process);
        return status;
    }
    flags = save_and_disable_interrupts();
    status = handle_peek(table, bootstrap, &object, &rights);
    if (status == OK && !(rights & RIGHT_TRANSFER))
        status = ERR_ACCESS_DENIED;
    if (status == OK)
        status = process_start((PROCESS *)process, (USER_THREAD *)thread,
                               arguments[2], arguments[3], object, rights,
                               arguments[5]);
    if (status == OK)
        handle_take(table, bootstrap);
    restore_interrupts(flags);
    object_release(thread);
    object_release(process);
    return status;
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

/* thread_create(process): a handle. */
static STATUS thread_create_call(const UINT64 *arguments, UINT64 *results)
{
    HANDLE_TABLE *table = caller_handles();
    OBJECT *process;
    USER_THREAD *thread;
    UINT32 index;
    STATUS status = handle_get(table, (UINT32)arguments[0], TYPE_PROCESS,
                               PROCESS_RIGHT_CREATE_THREAD, &process);

    if (status != OK)
        return status;
    status = handle_reserve(table, &index);
    if (status == OK) {
        status = process_thread_create((PROCESS *)process, &thread);
        if (status == OK)
            give(index, &thread->object, THREAD_RIGHTS, &results[0]);
        else
            handle_unreserve(table, index);
    }
    object_release(process);
    return status;
}

static STATUS thread_exit_call(const UINT64 *arguments, UINT64 *results)
{
    (void)arguments;
    (void)results;
    process_thread_exit();
}

/* section_create(size, options): a handle. No option is defined yet. */
static STATUS section_create_call(const UINT64 *arguments, UINT64 *results)
{
    HANDLE_TABLE *table = caller_handles();
    SECTION *section;
    UINT32 index;
    STATUS status;

    if (arguments[1] != 0)
        return ERR_INVALID_ARGS;
    status = handle_reserve(table, &index);
    if (status != OK)
        return status;
    status = section_create(arguments[0], &section);
    if (status == OK)
        give(index, &section->object, SECTION_RIGHTS, &results[0]);
    else
        handle_unreserve(table, index);
    return status;
}

/* process_map(process, section, offset, address, size, protection): the
   address. The section handle needs the rights the protection does
   (O5). */
static STATUS process_map_call(const UINT64 *arguments, UINT64 *results)
{
    HANDLE_TABLE *table = caller_handles();
    UINT32 protection = (UINT32)arguments[5];
    OBJECT *process, *section;
    STATUS status = handle_get(table, (UINT32)arguments[0], TYPE_PROCESS,
                               PROCESS_RIGHT_MAP, &process);

    if (status != OK)
        return status;
    status = handle_get(table, (UINT32)arguments[1], TYPE_SECTION,
                        process_map_rights(protection), &section);
    if (status == OK) {
        status = process_map((PROCESS *)process, (SECTION *)section,
                             arguments[2], arguments[3], arguments[4],
                             protection);
        object_release(section);
    }
    object_release(process);
    if (status == OK)
        results[0] = arguments[3];
    return status;
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

/* thread_set_tls_base(base): 0 or a user address (S10). */
static STATUS thread_set_tls_base_call(const UINT64 *arguments,
                                       UINT64 *results)
{
    UINT64 base = arguments[0];

    (void)results;
    if (base != 0 && (base < USER_BASE || base >= user_space_top()))
        return ERR_INVALID_ARGS;
    thread_set_tls_base(base);
    return OK;
}

#define CALL_COUNT (CALL_THREAD_SET_TLS_BASE + 1)

static const CALL_ENTRY calls[CALL_COUNT] = {
    [CALL_HANDLE_CLOSE] = {handle_close_call, 1, 1},
    [CALL_OBJECT_WAIT_ONE] = {object_wait_one_call, 3, 1},
    [CALL_OBJECT_WAIT_MANY] = {object_wait_many_call, 3, 1},
    [CALL_CHANNEL_CREATE] = {channel_create_call, 1, 1},
    [CALL_CHANNEL_WRITE] = {channel_write_call, 5, 1},
    [CALL_CHANNEL_READ] = {channel_read_call, 5, 1},
    [CALL_CLOCK_MONOTONIC] = {clock_monotonic_call, 0, 0},
    [CALL_PROCESS_CREATE] = {process_create_call, 2, 0},
    [CALL_PROCESS_START] = {process_start_call, 6, 0},
    [CALL_PROCESS_EXIT] = {process_exit_call, 1, 1},
    [CALL_PROCESS_INFO] = {process_info_call, 1, 1},
    [CALL_THREAD_CREATE] = {thread_create_call, 1, 0},
    [CALL_THREAD_EXIT] = {thread_exit_call, 0, 1},
    [CALL_SECTION_CREATE] = {section_create_call, 2, 0},
    [CALL_PROCESS_MAP] = {process_map_call, 6, 0},
    [CALL_DEBUG_WRITE] = {debug_write_call, 3, 1},
    [CALL_THREAD_SET_TLS_BASE] = {thread_set_tls_base_call, 1, 1},
};

UINT32 syscall_argument_count(UINT64 number)
{
    return number < CALL_COUNT ? calls[number].arguments : 0;
}

int syscall_served_32_bit(UINT64 number)
{
    return number < CALL_COUNT && calls[number].lane_32_bit;
}

STATUS syscall_invoke(UINT64 number, const UINT64 *arguments,
                      UINT64 *results)
{
    if (number >= CALL_COUNT || !calls[number].call)
        return ERR_NOT_SUPPORTED;
    return calls[number].call(arguments, results);
}
