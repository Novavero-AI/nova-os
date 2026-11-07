/* Ports. Every change to a queue runs with interrupts disabled, since an
   interrupt handler queues packets too. A port signals its internal
   WAITABLE while its queue is not empty; waiters block on that and
   re-check the queue when they wake, so each packet goes to exactly one
   of them (ipc.md I4). */

#include "port.h"
#include "interrupt.h"
#include "cpu.h"
#include "../mm/page.h"
#include "../x86_64/user.h"

#define PORT_SIGNAL_PACKET 0x1

/* Interrupt bindings that name the port are cancelled and queued packets
   discarded (docs/spec/objects.md O10). */
static void last_handle(OBJECT *object)
{
    PORT *port = (PORT *)object;
    UINT64 flags = save_and_disable_interrupts();

    while (port->bound)
        interrupt_unbind(port->bound);
    while (port->first)
        port_remove(port, port->first);
    restore_interrupts(flags);
}

static void destroy(OBJECT *object)
{
    page_free_mapped(object);
}

static const OBJECT_TYPE port_type = {TYPE_PORT, last_handle, destroy, 0};

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

/* The copy runs with interrupts disabled, so the packet it copies is the
   one it dequeues; a fault in it fails the copy and leaves the packet
   queued. */
STATUS port_wait(PORT *port, UINT64 deadline, UINT64 packet)
{
    UINT64 flags;
    UINT32 observed;
    STATUS status;

    if (!user_range_valid(packet, sizeof(PORT_PACKET)))
        return ERR_BAD_ADDRESS;
    for (;;) {
        flags = save_and_disable_interrupts();
        if (port->first) {
            PACKET_NODE *node = port->first;

            status = copy_to_user(packet, &node->packet, sizeof(PORT_PACKET));
            if (status == OK)
                port_remove(port, node);
            restore_interrupts(flags);
            return status;
        }
        restore_interrupts(flags);
        if (wait_one(&port->waitable, PORT_SIGNAL_PACKET, deadline,
                     &observed) == ERR_TIMED_OUT)
            return ERR_TIMED_OUT;
    }
}
