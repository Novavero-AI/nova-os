/* Channels. Each endpoint is an object of its own, and each points at its
   peer without holding a reference to it: an endpoint's last-handle rule
   unlinks the pair, and an endpoint is destroyed only after that rule
   has run, so neither link outlives the endpoint it names. A message
   lives in a page of its own, its bytes in a run of pages beside it.

   An endpoint's signals follow its state (ipc.md I1): READABLE while its
   queue holds a message, WRITABLE while its peer's queue has room, and
   PEER_CLOSED once its peer is gone. Every change of state recomputes
   them for both ends. */

#include "channel.h"
#include "cpu.h"
#include "../mm/page.h"
#include "../x86_64/user.h"

struct MESSAGE {
    MESSAGE *next;
    UINT8 *bytes; /* direct-map address of byte_pages pages, or 0 */
    UINT32 byte_count;
    UINT32 byte_pages;
    UINT32 handle_count;
    CARRIED_HANDLE handles[CHANNEL_MAX_HANDLES];
};

_Static_assert(sizeof(MESSAGE) <= PAGE_SIZE, "a message fits in a page");

#define CHANNEL_SIGNALS                                               \
    (CHANNEL_READABLE | CHANNEL_WRITABLE | CHANNEL_PEER_CLOSED)

/* With interrupts disabled. */
static void update_signals(ENDPOINT *endpoint)
{
    UINT32 signals = 0;

    if (endpoint->first)
        signals |= CHANNEL_READABLE;
    if (!endpoint->peer)
        signals |= CHANNEL_PEER_CLOSED;
    else if (endpoint->peer->queued < endpoint->peer->capacity)
        signals |= CHANNEL_WRITABLE;
    waitable_update(&endpoint->waitable, CHANNEL_SIGNALS, signals);
}

static void free_message(MESSAGE *message)
{
    UINT32 i;

    for (i = 0; i < message->byte_pages; i++)
        page_free((UINT64)message->bytes - DIRECT_MAP_BASE +
                  (UINT64)i * PAGE_SIZE);
    page_free_mapped(message);
}

/* A message with room for byte_count bytes, or 0 if memory runs out. */
static MESSAGE *alloc_message(UINT32 byte_count)
{
    MESSAGE *message = page_alloc_mapped();
    UINT32 pages = (byte_count + PAGE_SIZE - 1) / PAGE_SIZE;
    UINT64 run;

    if (!message || pages == 0)
        return message;
    run = page_alloc_run(pages);
    if (!run) {
        page_free_mapped(message);
        return 0;
    }
    message->bytes = direct_map(run);
    message->byte_pages = pages;
    return message;
}

/* A message destroyed unread closes the handles it carries (ipc.md I2). */
static void destroy_message(MESSAGE *message)
{
    UINT32 i;

    for (i = 0; i < message->handle_count; i++)
        object_close(message->handles[i].object);
    free_message(message);
}

/* The endpoint's queued messages are destroyed and its peer is closed
   (docs/spec/objects.md O10); messages already queued at the peer stay
   readable there (ipc.md I1). */
static void last_handle(OBJECT *object)
{
    ENDPOINT *endpoint = (ENDPOINT *)object;
    MESSAGE *message;

    while ((message = endpoint->first) != 0) {
        endpoint->first = message->next;
        destroy_message(message);
    }
    endpoint->last = 0;
    endpoint->queued = 0;
    if (endpoint->peer) {
        endpoint->peer->peer = 0;
        update_signals(endpoint->peer);
        endpoint->peer = 0;
    }
}

static void destroy(OBJECT *object)
{
    page_free_mapped(object);
}

static WAITABLE *waitable_of(OBJECT *object)
{
    return &((ENDPOINT *)object)->waitable;
}

static const OBJECT_TYPE channel_type = {TYPE_CHANNEL, last_handle, destroy,
                                         waitable_of, waitable_of};

STATUS channel_create(UINT32 capacity, ENDPOINT **first, ENDPOINT **second)
{
    ENDPOINT *a, *b;

    if (capacity == 0)
        return ERR_INVALID_ARGS;
    if (capacity > CHANNEL_MAX_CAPACITY)
        return ERR_OUT_OF_RANGE;
    a = page_alloc_mapped();
    b = a ? page_alloc_mapped() : 0;
    if (!b) {
        if (a)
            page_free_mapped(a);
        return ERR_NO_MEMORY;
    }
    object_init(&a->object, &channel_type);
    object_init(&b->object, &channel_type);
    waitable_init(&a->waitable);
    waitable_init(&b->waitable);
    a->capacity = capacity;
    b->capacity = capacity;
    a->peer = b;
    b->peer = a;
    a->waitable.signals = CHANNEL_WRITABLE;
    b->waitable.signals = CHANNEL_WRITABLE;
    *first = a;
    *second = b;
    return OK;
}

/* Queue message at from's peer, with interrupts disabled. */
static STATUS enqueue(ENDPOINT *from, MESSAGE *message)
{
    ENDPOINT *to = from->peer;

    if (!to)
        return ERR_PEER_CLOSED;
    if (to->queued >= to->capacity)
        return ERR_SHOULD_WAIT;
    message->next = 0;
    if (to->last)
        to->last->next = message;
    else
        to->first = message;
    to->last = message;
    to->queued++;
    update_signals(to);
    update_signals(from);
    return OK;
}

STATUS channel_write_kernel(ENDPOINT *from, const UINT8 *bytes,
                            UINT32 byte_count, const CARRIED_HANDLE *handles,
                            UINT32 count)
{
    MESSAGE *message;
    UINT64 flags;
    STATUS status;
    UINT32 i;

    if (byte_count > CHANNEL_MAX_BYTES || count > CHANNEL_MAX_HANDLES)
        return ERR_OUT_OF_RANGE;
    message = alloc_message(byte_count);
    if (!message)
        return ERR_NO_MEMORY;
    message->byte_count = byte_count;
    for (i = 0; i < byte_count; i++)
        message->bytes[i] = bytes[i];
    message->handle_count = count;
    for (i = 0; i < count; i++)
        message->handles[i] = handles[i];
    flags = save_and_disable_interrupts();
    status = enqueue(from, message);
    restore_interrupts(flags);
    if (status != OK)
        free_message(message);
    return status;
}

/* Check every entry of transfers against table and the endpoint being
   written, in the order docs/spec/objects.md O3 gives, and fill the
   message's handles; nothing moves yet. With interrupts disabled. */
static STATUS check_transfers(ENDPOINT *endpoint, HANDLE_TABLE *table,
                              const HANDLE_TRANSFER *transfers, UINT32 count,
                              MESSAGE *message)
{
    UINT32 i, j;

    for (i = 0; i < count; i++) {
        OBJECT *object;
        UINT32 rights, wanted = transfers[i].rights;
        STATUS status = handle_peek(table, transfers[i].handle, &object,
                                    &rights);

        if (status != OK)
            return status;
        if (!(rights & RIGHT_TRANSFER) ||
            (wanted != RIGHTS_SAME && (wanted & rights) != wanted))
            return ERR_ACCESS_DENIED;
        if (object == &endpoint->object ||
            (endpoint->peer && object == &endpoint->peer->object))
            return ERR_NOT_SUPPORTED;
        for (j = 0; j < i; j++)
            if (transfers[j].handle == transfers[i].handle)
                return ERR_NOT_SUPPORTED;
        message->handles[i].object = object;
        message->handles[i].rights = wanted == RIGHTS_SAME ? rights : wanted;
    }
    return OK;
}

STATUS channel_write_message(ENDPOINT *endpoint, HANDLE_TABLE *table,
                             UINT64 bytes, UINT32 byte_count, UINT64 handles,
                             UINT32 handle_count)
{
    HANDLE_TRANSFER transfers[CHANNEL_MAX_HANDLES];
    MESSAGE *message;
    UINT64 flags;
    STATUS status;
    UINT32 i;

    if (byte_count > CHANNEL_MAX_BYTES || handle_count > CHANNEL_MAX_HANDLES)
        return ERR_OUT_OF_RANGE;
    status = copy_from_user(transfers, handles,
                            (UINT64)handle_count * sizeof(HANDLE_TRANSFER));
    if (status != OK)
        return status;
    message = alloc_message(byte_count);
    if (!message)
        return ERR_NO_MEMORY;
    message->byte_count = byte_count;
    message->handle_count = handle_count;
    status = copy_from_user(message->bytes, bytes, byte_count);
    if (status == OK) {
        flags = save_and_disable_interrupts();
        status = check_transfers(endpoint, table, transfers, handle_count,
                                 message);
        if (status == OK)
            status = enqueue(endpoint, message);
        if (status == OK)
            for (i = 0; i < handle_count; i++)
                handle_take(table, transfers[i].handle);
        restore_interrupts(flags);
    }
    if (status != OK)
        free_message(message);
    return status;
}

/* Move message's handles into table and report them at handles, reserving
   every entry before anything moves so that a full table or a fault on
   the user buffer leaves both as they were (docs/spec/syscalls.md S6). */
static STATUS deliver(MESSAGE *message, HANDLE_TABLE *table, UINT64 handles)
{
    RECEIVED_HANDLE received[CHANNEL_MAX_HANDLES];
    UINT32 index[CHANNEL_MAX_HANDLES];
    UINT32 count = message->handle_count;
    UINT32 reserved, i;
    STATUS status = OK;

    for (reserved = 0; status == OK && reserved < count; reserved++)
        status = handle_reserve(table, &index[reserved]);
    if (status != OK)
        reserved--;
    for (i = 0; status == OK && i < count; i++) {
        received[i].handle = handle_value(table, index[i]);
        received[i].type = message->handles[i].object->type->number;
        received[i].rights = message->handles[i].rights;
    }
    if (status == OK)
        status = copy_to_user(handles, received,
                              (UINT64)count * sizeof(RECEIVED_HANDLE));
    if (status != OK) {
        for (i = 0; i < reserved; i++)
            handle_unreserve(table, index[i]);
        return status;
    }
    for (i = 0; i < count; i++)
        handle_place(table, index[i], message->handles[i].object,
                     message->handles[i].rights);
    return OK;
}

/* The bytes are copied before the handles move, so a fault on either
   buffer leaves the message queued; the bytes buffer is then partly
   written, which S6 allows. */
STATUS channel_read_message(ENDPOINT *endpoint, HANDLE_TABLE *table,
                            UINT64 bytes, UINT32 byte_capacity,
                            UINT64 handles, UINT32 handle_capacity,
                            UINT32 *byte_count, UINT32 *handle_count)
{
    UINT64 flags = save_and_disable_interrupts();
    MESSAGE *message = endpoint->first;
    STATUS status;

    *byte_count = 0;
    *handle_count = 0;
    if (!user_range_valid(bytes, byte_capacity) ||
        !user_range_valid(handles, (UINT64)handle_capacity *
                                       sizeof(RECEIVED_HANDLE))) {
        status = ERR_BAD_ADDRESS;
    } else if (!message) {
        status = endpoint->peer ? ERR_SHOULD_WAIT : ERR_PEER_CLOSED;
    } else {
        *byte_count = message->byte_count;
        *handle_count = message->handle_count;
        if (message->byte_count > byte_capacity ||
            message->handle_count > handle_capacity)
            status = ERR_BUFFER_TOO_SMALL;
        else
            status = copy_to_user(bytes, message->bytes, message->byte_count);
        if (status == OK)
            status = deliver(message, table, handles);
    }
    if (status == OK) {
        endpoint->first = message->next;
        if (!endpoint->first)
            endpoint->last = 0;
        endpoint->queued--;
        update_signals(endpoint);
        if (endpoint->peer)
            update_signals(endpoint->peer);
        free_message(message);
    }
    restore_interrupts(flags);
    return status;
}
