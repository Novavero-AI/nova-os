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
#include "../object/initiator.h"
#include "../object/interrupt.h"
#include "../object/process.h"
#include "../object/resource.h"
#include "../random/random.h"
#include "../random/wipe.h"
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

/* handle_duplicate(handle, rights): a handle. */
static STATUS handle_duplicate_call(const UINT64 *arguments, UINT64 *results)
{
    UINT32 duplicate;
    STATUS status = handle_duplicate(caller_handles(), (UINT32)arguments[0],
                                     (UINT32)arguments[1], &duplicate);

    if (status == OK)
        results[0] = duplicate;
    return status;
}

/* object_wait_one(handle, signals, deadline): observed signals, reported
   also when a closing handle cancels the wait (O9). */
static STATUS object_wait_one_call(const UINT64 *arguments, UINT64 *results)
{
    WAIT_ENTRY entry;
    STATUS status;

    entry.handle = (UINT32)arguments[0];
    entry.signals = (UINT32)arguments[1];
    entry.observed = 0;
    status = handle_wait(caller_handles(), &entry, 1, arguments[2]);
    results[0] = entry.observed;
    return status;
}

/* object_wait_many(items, count, deadline): writes each WAIT_ENTRY's
   observed signals, also when the deadline passes or a closing handle
   cancels the wait (O9). */
static STATUS object_wait_many_call(const UINT64 *arguments, UINT64 *results)
{
    WAIT_ENTRY entries[WAIT_MANY_MAX];
    UINT64 address = arguments[0];
    UINT32 count = (UINT32)arguments[1];
    STATUS status, copied;

    (void)results;
    if (count > WAIT_MANY_MAX)
        return ERR_OUT_OF_RANGE;
    status = copy_from_user(entries, address, count * sizeof(WAIT_ENTRY));
    if (status == OK)
        status = handle_wait(caller_handles(), entries, count, arguments[2]);
    if (status == OK || status == ERR_TIMED_OUT || status == ERR_CANCELED) {
        copied = copy_to_user(address, entries, count * sizeof(WAIT_ENTRY));
        if (copied != OK)
            status = copied;
    }
    return status;
}

/* object_wait_async(handle, port, key, signals). Interrupts stay disabled
   from the handles' checks through the watch's registration, so neither
   handle can leave the table in between, where its close would find no
   watch to end (O9). */
static STATUS object_wait_async_call(const UINT64 *arguments,
                                     UINT64 *results)
{
    HANDLE_TABLE *table = caller_handles();
    UINT32 value = (UINT32)arguments[0];
    OBJECT *object, *port;
    WAITABLE *waitable;
    UINT64 flags = save_and_disable_interrupts();
    STATUS status =
        handle_get_waitable(table, value, RIGHT_WAIT, &object, &waitable);

    (void)results;
    if (status == OK) {
        status = handle_get(table, (UINT32)arguments[1], TYPE_PORT,
                            PORT_RIGHT_QUEUE, &port);
        if (status == OK) {
            status = port_watch((PORT *)port, object, waitable, table, value,
                                arguments[2], (UINT32)arguments[3]);
            object_release(port);
        }
        object_release(object);
    }
    restore_interrupts(flags);
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

/* port_create(capacity): a handle. */
static STATUS port_create_call(const UINT64 *arguments, UINT64 *results)
{
    HANDLE_TABLE *table = caller_handles();
    PORT *port;
    UINT32 index;
    STATUS status = handle_reserve(table, &index);

    if (status != OK)
        return status;
    status = port_create((UINT32)arguments[0], &port);
    if (status == OK)
        give(index, &port->object, PORT_RIGHTS, &results[0]);
    else
        handle_unreserve(table, index);
    return status;
}

/* port_wait(port, deadline, packet). */
static STATUS port_wait_call(const UINT64 *arguments, UINT64 *results)
{
    (void)results;
    return port_wait(caller_handles(), (UINT32)arguments[0], arguments[1],
                     arguments[2]);
}

static STATUS clock_monotonic_call(const UINT64 *arguments, UINT64 *results)
{
    (void)arguments;
    results[0] = clock_monotonic();
    return OK;
}

/* job_create(parent, limits): a handle, which carries WRITE_EXECUTE only
   if the parent's handle does (O5, rule 1). The parent's rights are read
   with its check, interrupts disabled across both, so no other thread
   narrows them in between. */
static STATUS job_create_call(const UINT64 *arguments, UINT64 *results)
{
    HANDLE_TABLE *table = caller_handles();
    UINT32 value = (UINT32)arguments[0];
    JOB_LIMITS limits;
    OBJECT *parent, *same;
    JOB *job;
    UINT32 rights, index;
    UINT64 flags = save_and_disable_interrupts();
    STATUS status = handle_get(table, value, TYPE_JOB, JOB_RIGHT_CREATE_JOB,
                               &parent);

    if (status == OK)
        handle_peek(table, value, &same, &rights);
    restore_interrupts(flags);
    if (status != OK)
        return status;
    status = copy_from_user(&limits, arguments[1], sizeof limits);
    if (status == OK)
        status = handle_reserve(table, &index);
    if (status == OK) {
        status = job_create((JOB *)parent, &limits, &job);
        if (status == OK)
            give(index, &job->object,
                 (rights & JOB_RIGHT_WRITE_EXECUTE)
                     ? JOB_RIGHTS
                     : JOB_RIGHTS & ~JOB_RIGHT_WRITE_EXECUTE,
                 &results[0]);
        else
            handle_unreserve(table, index);
    }
    object_release(parent);
    return status;
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

/* thread_start(thread, entry, stack, argument 1, argument 2). */
static STATUS thread_start_call(const UINT64 *arguments, UINT64 *results)
{
    OBJECT *thread;
    STATUS status = handle_get(caller_handles(), (UINT32)arguments[0],
                               TYPE_THREAD, THREAD_RIGHT_START, &thread);

    (void)results;
    if (status != OK)
        return status;
    status = process_thread_start((USER_THREAD *)thread, arguments[1],
                                  arguments[2], arguments[3], arguments[4]);
    object_release(thread);
    return status;
}

static STATUS thread_exit_call(const UINT64 *arguments, UINT64 *results)
{
    (void)arguments;
    (void)results;
    process_thread_exit();
}

static void kill_process(OBJECT *process)
{
    process_terminate((PROCESS *)process, PROCESS_EXIT_KILLED);
}

static void kill_thread(OBJECT *thread)
{
    process_thread_kill((USER_THREAD *)thread);
}

static void kill_job(OBJECT *job)
{
    job_kill((JOB *)job);
}

/* What task_kill takes: each type, the right to end one, and how. */
static const struct {
    UINT32 type;
    UINT32 right;
    void (*kill)(OBJECT *object);
} killable[] = {
    {TYPE_PROCESS, PROCESS_RIGHT_TERMINATE, kill_process},
    {TYPE_THREAD, THREAD_RIGHT_TERMINATE, kill_thread},
    {TYPE_JOB, JOB_RIGHT_TERMINATE, kill_job},
};

/* task_kill(handle): a process, a thread or a job. A handle of another
   type is ERR_WRONG_TYPE, from the last type tried, so each type's
   attempt is O3's own check: the value, then the type, then the right.
   The target is only stopping when the call returns (O10). */
static STATUS task_kill_call(const UINT64 *arguments, UINT64 *results)
{
    UINT32 i;
    STATUS status = ERR_WRONG_TYPE;
    OBJECT *object;

    (void)results;
    for (i = 0; i < sizeof killable / sizeof killable[0]; i++) {
        status = handle_get(caller_handles(), (UINT32)arguments[0],
                            killable[i].type, killable[i].right, &object);
        if (status == OK) {
            killable[i].kill(object);
            object_release(object);
        }
        if (status != ERR_WRONG_TYPE)
            break;
    }
    return status;
}

/* section_create(size, options): a handle. No option is defined. */
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

/* process_unmap(process, address, size). */
static STATUS process_unmap_call(const UINT64 *arguments, UINT64 *results)
{
    OBJECT *process;
    STATUS status = handle_get(caller_handles(), (UINT32)arguments[0],
                               TYPE_PROCESS, PROCESS_RIGHT_MAP, &process);

    (void)results;
    if (status != OK)
        return status;
    status = process_unmap((PROCESS *)process, arguments[1], arguments[2]);
    object_release(process);
    return status;
}

/* ------------------------------------------------------------------------
   Device access (docs/spec/ipc.md I6 to I8). Each call that creates an
   object takes a resource or initiator handle first, then reserves the
   entry for the new handle. */

/* resource_create(parent, base, size): a handle. */
static STATUS resource_create_call(const UINT64 *arguments, UINT64 *results)
{
    HANDLE_TABLE *table = caller_handles();
    RESOURCE *child;
    OBJECT *parent;
    UINT32 index;
    STATUS status = handle_get(table, (UINT32)arguments[0], TYPE_RESOURCE,
                               RESOURCE_RIGHT_USE, &parent);

    if (status != OK)
        return status;
    status = handle_reserve(table, &index);
    if (status == OK) {
        status = resource_derive((RESOURCE *)parent, arguments[1],
                                 arguments[2], &child);
        if (status == OK)
            give(index, &child->object, RESOURCE_RIGHTS, &results[0]);
        else
            handle_unreserve(table, index);
    }
    object_release(parent);
    return status;
}

/* section_create_physical(resource, base, size, cache): a handle. */
static STATUS section_create_physical_call(const UINT64 *arguments,
                                           UINT64 *results)
{
    HANDLE_TABLE *table = caller_handles();
    SECTION *section;
    OBJECT *resource;
    UINT32 index;
    STATUS status = handle_get(table, (UINT32)arguments[0], TYPE_RESOURCE,
                               RESOURCE_RIGHT_USE, &resource);

    if (status != OK)
        return status;
    status = handle_reserve(table, &index);
    if (status == OK) {
        status = section_create_device((RESOURCE *)resource, arguments[1],
                                       arguments[2], (UINT32)arguments[3],
                                       &section);
        if (status == OK)
            give(index, &section->object, DEVICE_SECTION_RIGHTS,
                 &results[0]);
        else
            handle_unreserve(table, index);
    }
    object_release(resource);
    return status;
}

/* A device's interrupts are unconfined without interrupt remapping
   (ipc.md I7), which this kernel does not drive. */
static void log_unconfined(const INTERRUPT *interrupt)
{
    UINT64 flags = serial_lock();

    serial_write("nova: msi unconfined pci=");
    resource_log_requester(interrupt->source - INTERRUPT_SOURCE_MSI);
    serial_write(" vector=");
    serial_hex(interrupt->vector);
    serial_write("\r\n");
    serial_unlock(flags);
}

/* interrupt_create(resource, source, options): a handle, the MSI address
   and the MSI data. The vector is charged to the caller's job. */
static STATUS interrupt_create_call(const UINT64 *arguments, UINT64 *results)
{
    HANDLE_TABLE *table = caller_handles();
    INTERRUPT *interrupt;
    OBJECT *resource;
    UINT64 address;
    UINT32 data, index;
    STATUS status = handle_get(table, (UINT32)arguments[0], TYPE_RESOURCE,
                               RESOURCE_RIGHT_USE, &resource);

    if (status != OK)
        return status;
    status = handle_reserve(table, &index);
    if (status == OK) {
        status = interrupt_create((RESOURCE *)resource, arguments[1],
                                  (UINT32)arguments[2],
                                  process_current()->job, &interrupt,
                                  &address, &data);
        if (status == OK) {
            log_unconfined(interrupt);
            give(index, &interrupt->object, INTERRUPT_RIGHTS, &results[0]);
            results[1] = address;
            results[2] = data;
        } else {
            handle_unreserve(table, index);
        }
    }
    object_release(resource);
    return status;
}

/* interrupt_bind(interrupt, port, key). */
static STATUS interrupt_bind_call(const UINT64 *arguments, UINT64 *results)
{
    HANDLE_TABLE *table = caller_handles();
    OBJECT *interrupt, *port;
    STATUS status = handle_get(table, (UINT32)arguments[0], TYPE_INTERRUPT,
                               INTERRUPT_RIGHT_BIND, &interrupt);

    (void)results;
    if (status != OK)
        return status;
    status = handle_get(table, (UINT32)arguments[1], TYPE_PORT,
                        PORT_RIGHT_QUEUE, &port);
    if (status == OK) {
        status = interrupt_bind((INTERRUPT *)interrupt, (PORT *)port,
                                arguments[2]);
        object_release(port);
    }
    object_release(interrupt);
    return status;
}

/* interrupt_ack(interrupt). */
static STATUS interrupt_ack_call(const UINT64 *arguments, UINT64 *results)
{
    OBJECT *interrupt;
    STATUS status =
        handle_get(caller_handles(), (UINT32)arguments[0], TYPE_INTERRUPT,
                   INTERRUPT_RIGHT_ACK, &interrupt);

    (void)results;
    if (status != OK)
        return status;
    status = interrupt_ack((INTERRUPT *)interrupt);
    object_release(interrupt);
    return status;
}

/* initiator_create(resource, requester ID): a handle. */
static STATUS initiator_create_call(const UINT64 *arguments, UINT64 *results)
{
    HANDLE_TABLE *table = caller_handles();
    INITIATOR *initiator;
    OBJECT *resource;
    UINT32 index;
    STATUS status = handle_get(table, (UINT32)arguments[0], TYPE_RESOURCE,
                               RESOURCE_RIGHT_USE, &resource);

    if (status != OK)
        return status;
    status = handle_reserve(table, &index);
    if (status == OK) {
        status = initiator_create((RESOURCE *)resource, (UINT32)arguments[1],
                                  &initiator);
        if (status == OK)
            give(index, &initiator->object, INITIATOR_RIGHTS, &results[0]);
        else
            handle_unreserve(table, index);
    }
    object_release(resource);
    return status;
}

/* section_pin(initiator, section, offset, size, options, addresses): a
   handle. The section handle needs the rights the options do (ipc.md
   I8). */
static STATUS section_pin_call(const UINT64 *arguments, UINT64 *results)
{
    HANDLE_TABLE *table = caller_handles();
    UINT32 options = (UINT32)arguments[4];
    OBJECT *initiator, *section;
    PIN *pin;
    UINT32 index;
    STATUS status = handle_get(table, (UINT32)arguments[0], TYPE_INITIATOR,
                               INITIATOR_RIGHT_PIN, &initiator);

    if (status != OK)
        return status;
    status = handle_get(table, (UINT32)arguments[1], TYPE_SECTION,
                        initiator_pin_rights(options), &section);
    if (status == OK) {
        status = handle_reserve(table, &index);
        if (status == OK) {
            status = initiator_pin((INITIATOR *)initiator, (SECTION *)section,
                                   arguments[2], arguments[3], options,
                                   arguments[5], &pin);
            if (status == OK)
                give(index, &pin->object, PIN_RIGHTS, &results[0]);
            else
                handle_unreserve(table, index);
        }
        object_release(section);
    }
    object_release(initiator);
    return status;
}

/* pin_release(pin): the pin ends and its handle closes. The pin ends
   and the handle leaves the table with interrupts disabled across both,
   so no other thread can close the handle between them, which would
   quarantine the pin first; the handle, out of the table, then closes
   on a released pin. */
static STATUS pin_release_call(const UINT64 *arguments, UINT64 *results)
{
    HANDLE_TABLE *table = caller_handles();
    UINT32 value = (UINT32)arguments[0];
    OBJECT *pin;
    UINT32 rights;
    UINT64 flags = save_and_disable_interrupts();
    STATUS status = handle_peek(table, value, &pin, &rights);

    (void)results;
    if (status == OK && pin->type->number != TYPE_PIN)
        status = ERR_WRONG_TYPE;
    if (status == OK) {
        pin_release((PIN *)pin);
        handle_take(table, value);
    }
    restore_interrupts(flags);
    if (status == OK)
        object_close(pin);
    return status;
}

/* initiator_release_quarantine(initiator). */
static STATUS initiator_release_quarantine_call(const UINT64 *arguments,
                                                UINT64 *results)
{
    OBJECT *initiator;
    STATUS status =
        handle_get(caller_handles(), (UINT32)arguments[0], TYPE_INITIATOR,
                   INITIATOR_RIGHT_RELEASE, &initiator);

    (void)results;
    if (status != OK)
        return status;
    initiator_release_quarantine((INITIATOR *)initiator);
    object_release(initiator);
    return OK;
}

/* debug_write(resource, bytes, byte count). The bytes are copied whole
   before any is written, so a call that fails writes nothing (S7), and
   written under the port's lock, so they reach it in one piece. */
static STATUS debug_write_call(const UINT64 *arguments, UINT64 *results)
{
    UINT8 bytes[DEBUG_WRITE_MAX_BYTES];
    UINT64 count = arguments[2], flags;
    OBJECT *resource;
    STATUS status = handle_get(caller_handles(), (UINT32)arguments[0],
                               TYPE_RESOURCE, RESOURCE_RIGHT_USE, &resource);

    (void)results;
    if (status != OK)
        return status;
    if (!resource_covers((RESOURCE *)resource, RESOURCE_SYSTEM,
                         SYSTEM_PRIVILEGE_DEBUG_LOG, 1))
        status = ERR_ACCESS_DENIED;
    else if (count > DEBUG_WRITE_MAX_BYTES)
        status = ERR_OUT_OF_RANGE;
    else
        status = copy_from_user(bytes, arguments[1], count);
    object_release(resource);
    if (status == OK) {
        flags = serial_lock();
        serial_write_bytes(bytes, count);
        serial_unlock(flags);
    }
    return status;
}

/* random_read(bytes, byte count): the generator's next bytes, generated
   on the kernel stack, copied out and wiped there. ERR_BAD_STATE while
   no source of real entropy has seeded the generator, as random_init
   logs at boot. */
static STATUS random_read_call(const UINT64 *arguments, UINT64 *results)
{
    UINT8 bytes[RANDOM_READ_MAX_BYTES];
    UINT64 count = arguments[1];
    STATUS status;

    (void)results;
    if (count > RANDOM_READ_MAX_BYTES)
        return ERR_OUT_OF_RANGE;
    if (!random_read(bytes, count))
        return ERR_BAD_STATE;
    status = copy_to_user(arguments[0], bytes, count);
    wipe(bytes, sizeof bytes);
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
    [CALL_HANDLE_DUPLICATE] = {handle_duplicate_call, 2, 0},
    [CALL_OBJECT_WAIT_ONE] = {object_wait_one_call, 3, 1},
    [CALL_OBJECT_WAIT_MANY] = {object_wait_many_call, 3, 1},
    [CALL_OBJECT_WAIT_ASYNC] = {object_wait_async_call, 4, 0},
    [CALL_CHANNEL_CREATE] = {channel_create_call, 1, 1},
    [CALL_CHANNEL_WRITE] = {channel_write_call, 5, 1},
    [CALL_CHANNEL_READ] = {channel_read_call, 5, 1},
    [CALL_PORT_CREATE] = {port_create_call, 1, 0},
    [CALL_PORT_WAIT] = {port_wait_call, 3, 0},
    [CALL_CLOCK_MONOTONIC] = {clock_monotonic_call, 0, 0},
    [CALL_JOB_CREATE] = {job_create_call, 2, 0},
    [CALL_PROCESS_CREATE] = {process_create_call, 2, 0},
    [CALL_PROCESS_START] = {process_start_call, 6, 0},
    [CALL_PROCESS_EXIT] = {process_exit_call, 1, 1},
    [CALL_PROCESS_INFO] = {process_info_call, 1, 1},
    [CALL_THREAD_CREATE] = {thread_create_call, 1, 0},
    [CALL_THREAD_START] = {thread_start_call, 5, 0},
    [CALL_THREAD_EXIT] = {thread_exit_call, 0, 1},
    [CALL_TASK_KILL] = {task_kill_call, 1, 0},
    [CALL_SECTION_CREATE] = {section_create_call, 2, 0},
    [CALL_PROCESS_MAP] = {process_map_call, 6, 0},
    [CALL_PROCESS_UNMAP] = {process_unmap_call, 3, 0},
    [CALL_RESOURCE_CREATE] = {resource_create_call, 3, 0},
    [CALL_SECTION_CREATE_PHYSICAL] = {section_create_physical_call, 4, 0},
    [CALL_INTERRUPT_CREATE] = {interrupt_create_call, 3, 0},
    [CALL_INTERRUPT_BIND] = {interrupt_bind_call, 3, 0},
    [CALL_INTERRUPT_ACK] = {interrupt_ack_call, 1, 0},
    [CALL_INITIATOR_CREATE] = {initiator_create_call, 2, 0},
    [CALL_SECTION_PIN] = {section_pin_call, 6, 0},
    [CALL_PIN_RELEASE] = {pin_release_call, 1, 0},
    [CALL_INITIATOR_RELEASE_QUARANTINE] = {initiator_release_quarantine_call,
                                           1, 0},
    [CALL_DEBUG_WRITE] = {debug_write_call, 3, 1},
    [CALL_RANDOM_READ] = {random_read_call, 2, 0},
    [CALL_THREAD_SET_TLS_BASE] = {thread_set_tls_base_call, 1, 1},
};

/* The call number is a caller's index into calls, so each read of the
   table masks it after the range check (docs/spec/speculation.md M4). */
UINT32 syscall_argument_count(UINT64 number)
{
    if (number >= CALL_COUNT)
        return 0;
    return calls[index_masked(number, CALL_COUNT)].arguments;
}

int syscall_served_32_bit(UINT64 number)
{
    if (number >= CALL_COUNT)
        return 0;
    return calls[index_masked(number, CALL_COUNT)].lane_32_bit != 0;
}

int syscall_reports_results(UINT64 number, STATUS status)
{
    return status == ERR_BUFFER_TOO_SMALL ||
           (number == CALL_OBJECT_WAIT_ONE && status == ERR_CANCELED);
}

/* A mispredicted range check reads entry 0, whose call is 0, and a call
   through it would fetch from address 0, which no table maps (S6). */
STATUS syscall_invoke(UINT64 number, const UINT64 *arguments,
                      UINT64 *results)
{
    CALL call;

    if (number >= CALL_COUNT)
        return ERR_NOT_SUPPORTED;
    call = calls[index_masked(number, CALL_COUNT)].call;
    if (!call)
        return ERR_NOT_SUPPORTED;
    return call(arguments, results);
}
