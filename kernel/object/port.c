/* Ports. Every change to a queue runs with interrupts disabled, since an
   interrupt handler queues packets too. A port signals its internal
   WAITABLE while its queue is not empty; waiters block on that and
   re-check the queue when they wake, so each packet goes to exactly one
   of them (ipc.md I4). The waits are linked there, so closing the handle
   a waiter waits through ends its wait.

   A watch is a page of its own, allocated when it is registered: its
   wait item, linked on the watched object's list through the handle it
   was made with, so that the handle's close ends it (wait.h); its
   packet; and a reference to the watched object and one to the port,
   which keep both alive for as long as the watch or its packet is
   (docs/spec/objects.md O10). It ends in one of three ways, each with
   interrupts disabled: it fires, and its packet goes to the queue, the
   watch with it, until the packet is read or discarded; its handle
   closes, and it is freed with no packet; or the port's last handle
   closes, and it is freed with no packet. Each way takes it off both
   lists first, so no other way can reach it. Freeing a watch drops its
   two references. When its handle's close frees it, in the middle of a
   walk of the watched object's list, that handle still holds a
   reference of its own, so the object and its list outlive the walk;
   and a port with a watch registered has a handle, or is closing its
   last, whose reference goes only after the last-handle rule below has
   freed every watch, so the port outlives the watch too. */

#include <stddef.h>

#include "port.h"
#include "interrupt.h"
#include "cpu.h"
#include "../mm/page.h"
#include "../x86_64/user.h"

#define PORT_SIGNAL_PACKET 0x1

struct WATCH {
    WAIT_ITEM item;
    PACKET_NODE node;
    OBJECT *object;
    PORT *port;
    struct WATCH *next, *previous; /* on the port's list while registered */
};

typedef struct WATCH WATCH;

_Static_assert(sizeof(WATCH) <= PAGE_SIZE, "a watch fits in a page");

static WATCH *watch_of_item(WAIT_ITEM *item)
{
    return (WATCH *)((UINT8 *)item - offsetof(WATCH, item));
}

static WATCH *watch_of_node(PACKET_NODE *node)
{
    return (WATCH *)((UINT8 *)node - offsetof(WATCH, node));
}

static void free_watch(WATCH *watch)
{
    watch->port->watch_count--;
    object_release(watch->object);
    object_release(&watch->port->object);
    page_free_mapped(watch);
}

/* Take a registered watch off the watched object's list and the port's,
   with interrupts disabled. */
static void unregister(WATCH *watch)
{
    PORT *port = watch->port;

    waitable_unwatch(&watch->item);
    if (watch->previous)
        watch->previous->next = watch->next;
    else
        port->watches = watch->next;
    if (watch->next)
        watch->next->previous = watch->previous;
}

/* The watch's notify (wait.h): on a signal, queue the packet with the
   signals asked for and those active now; on its handle's close, free
   the watch with no packet. */
static void notify(WAIT_ITEM *item, STATUS status)
{
    WATCH *watch = watch_of_item(item);

    unregister(watch);
    if (status != OK) {
        free_watch(watch);
        return;
    }
    watch->node.packet.payload[0] =
        item->signals |
        (UINT64)item->object->signals << PACKET_SIGNAL_ACTIVE_SHIFT;
    port_enqueue(watch->port, &watch->node);
}

static void taken(PACKET_NODE *node)
{
    free_watch(watch_of_node(node));
}

/* Interrupt bindings and watches that name the port are cancelled, and
   queued packets discarded (docs/spec/objects.md O10). */
static void last_handle(OBJECT *object)
{
    PORT *port = (PORT *)object;
    UINT64 flags = save_and_disable_interrupts();
    WATCH *watch;

    while (port->bound)
        interrupt_unbind(port->bound);
    while ((watch = port->watches) != 0) {
        unregister(watch);
        free_watch(watch);
    }
    while (port->first)
        port_discard(port, port->first);
    restore_interrupts(flags);
}

static void destroy(OBJECT *object)
{
    page_free_mapped(object);
}

static WAITABLE *queue_of(OBJECT *object)
{
    return &((PORT *)object)->waitable;
}

static const OBJECT_TYPE port_type = {TYPE_PORT, last_handle, destroy, 0,
                                      queue_of};

STATUS port_create(UINT32 capacity, PORT **out)
{
    PORT *port;

    if (capacity == 0)
        return ERR_INVALID_ARGS;
    if (capacity > PORT_MAX_CAPACITY)
        return ERR_OUT_OF_RANGE;
    port = page_alloc_mapped();
    if (!port)
        return ERR_NO_MEMORY;
    object_init(&port->object, &port_type);
    waitable_init(&port->waitable);
    port->capacity = capacity;
    *out = port;
    return OK;
}

void port_enqueue(PORT *port, PACKET_NODE *node)
{
    UINT64 flags = save_and_disable_interrupts();

    if (!node->queued) {
        node->next = 0;
        node->queued = 1;
        if (port->last)
            port->last->next = node;
        else
            port->first = node;
        port->last = node;
        waitable_update(&port->waitable, 0, PORT_SIGNAL_PACKET);
    }
    restore_interrupts(flags);
}

void port_remove(PORT *port, PACKET_NODE *node)
{
    UINT64 flags = save_and_disable_interrupts();
    PACKET_NODE **link;

    for (link = &port->first; node->queued && *link; link = &(*link)->next)
        if (*link == node) {
            *link = node->next;
            if (port->last == node) {
                PACKET_NODE *last = port->first;

                while (last && last->next)
                    last = last->next;
                port->last = last;
            }
            node->queued = 0;
            node->next = 0;
            break;
        }
    if (!port->first)
        waitable_update(&port->waitable, PORT_SIGNAL_PACKET, 0);
    restore_interrupts(flags);
}

void port_discard(PORT *port, PACKET_NODE *node)
{
    UINT64 flags = save_and_disable_interrupts();
    int queued = node->queued;

    port_remove(port, node);
    if (queued && node->taken)
        node->taken(node);
    restore_interrupts(flags);
}

STATUS port_watch(PORT *port, OBJECT *object, WAITABLE *waitable,
                  const HANDLE_TABLE *table, UINT32 handle, UINT64 key,
                  UINT32 signals)
{
    UINT64 flags = save_and_disable_interrupts();
    WATCH *watch =
        port->watch_count < PORT_WATCHES_MAX ? page_alloc_mapped() : 0;

    if (!watch) {
        restore_interrupts(flags);
        return ERR_NO_MEMORY;
    }
    port->watch_count++;
    object_retain(object);
    object_retain(&port->object);
    watch->object = object;
    watch->port = port;
    watch->item.object = waitable;
    watch->item.signals = signals;
    watch->item.handle_table = table;
    watch->item.handle = handle;
    watch->item.end.notify = notify;
    watch->node.taken = taken;
    watch->node.packet.key = key;
    watch->node.packet.type = PACKET_SIGNAL;
    watch->next = port->watches;
    if (port->watches)
        port->watches->previous = watch;
    port->watches = watch;
    waitable_watch(&watch->item);
    if (waitable->signals & signals)
        notify(&watch->item, OK);
    restore_interrupts(flags);
    return OK;
}

/* Interrupts stay disabled from the handle's check to the end, except
   while the thread is blocked, so a close of the handle always finds the
   wait it ends, and the packet copied is the one dequeued; a fault in
   the copy fails it and leaves the packet queued. A waiter woken for a
   packet another thread took first waits again. */
STATUS port_wait(HANDLE_TABLE *table, UINT32 value, UINT64 deadline,
                 UINT64 packet)
{
    UINT64 flags = save_and_disable_interrupts();
    OBJECT *object;
    PORT *port;
    WAIT_ITEM item;
    STATUS status = handle_get(table, value, TYPE_PORT, PORT_RIGHT_READ,
                               &object);

    if (status != OK) {
        restore_interrupts(flags);
        return status;
    }
    port = (PORT *)object;
    item.object = &port->waitable;
    item.signals = PORT_SIGNAL_PACKET;
    item.handle_table = table;
    item.handle = value;
    if (!user_range_valid(packet, sizeof(PORT_PACKET)))
        status = ERR_BAD_ADDRESS;
    while (status == OK && !port->first)
        status = wait_many(&item, 1, deadline);
    if (status == OK) {
        PACKET_NODE *node = port->first;

        status = copy_to_user(packet, &node->packet, sizeof(PORT_PACKET));
        if (status == OK)
            port_discard(port, node);
    }
    object_release(object);
    restore_interrupts(flags);
    return status;
}
