/* Channels. Each endpoint is an object of its own, and each points at its
   peer without holding a reference to it: an endpoint's last-handle rule
   unlinks the pair, and an endpoint is destroyed only after that rule
   has run, so neither link outlives the endpoint it names. A message
   lives in a page of its own. */

#include "channel.h"
#include "cpu.h"
#include "../mm/page.h"
#include "../x86_64/user.h"

struct MESSAGE {
    MESSAGE *next;
    UINT32 handle_count;
    CARRIED_HANDLE handles[CHANNEL_MAX_HANDLES];
};

_Static_assert(sizeof(MESSAGE) <= PAGE_SIZE, "a message fits in a page");

/* A message destroyed unread closes the handles it carries (ipc.md I2). */
static void destroy_message(MESSAGE *message)
{
    UINT32 i;

    for (i = 0; i < message->handle_count; i++)
        object_close(message->handles[i].object);
    page_free_mapped(message);
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
    if (endpoint->peer) {
        endpoint->peer->peer = 0;
        endpoint->peer = 0;
    }
}

static void destroy(OBJECT *object)
{
    page_free_mapped(object);
}

static const OBJECT_TYPE channel_type = {TYPE_CHANNEL, last_handle, destroy};

STATUS channel_create(ENDPOINT **first, ENDPOINT **second)
{
    ENDPOINT *a = page_alloc_mapped();
    ENDPOINT *b = a ? page_alloc_mapped() : 0;

    if (!b) {
        if (a)
            page_free_mapped(a);
        return ERR_NO_MEMORY;
    }
    object_init(&a->object, &channel_type);
    object_init(&b->object, &channel_type);
    a->peer = b;
    b->peer = a;
    *first = a;
    *second = b;
    return OK;
}

STATUS channel_write_handles(ENDPOINT *from, const CARRIED_HANDLE *handles,
                             UINT32 count)
{
    MESSAGE *message;
    UINT64 flags;
    STATUS status = OK;
    UINT32 i;

    if (count > CHANNEL_MAX_HANDLES)
        return ERR_OUT_OF_RANGE;
    message = page_alloc_mapped();
    if (!message)
        return ERR_NO_MEMORY;
    message->handle_count = count;
    for (i = 0; i < count; i++) {
        message->handles[i].object = handles[i].object;
        message->handles[i].rights = handles[i].rights;
    }
    flags = save_and_disable_interrupts();
    if (!from->peer) {
        status = ERR_PEER_CLOSED;
    } else if (from->peer->last) {
        from->peer->last->next = message;
        from->peer->last = message;
    } else {
        from->peer->first = message;
        from->peer->last = message;
    }
    restore_interrupts(flags);
    if (status != OK)
        page_free_mapped(message);
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

/* Messages carry no bytes yet, so the byte buffer is checked but never
   written. */
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
        *handle_count = message->handle_count;
        status = message->handle_count > handle_capacity
                     ? ERR_BUFFER_TOO_SMALL
                     : deliver(message, table, handles);
    }
    if (status == OK) {
        endpoint->first = message->next;
        if (!endpoint->first)
            endpoint->last = 0;
        page_free_mapped(message);
    }
    restore_interrupts(flags);
    return status;
}
