/* Interrupt objects. Each message-signalled interrupt owns a vector from
   MSI_VECTOR_FIRST on (x86_64.h), charged to its creator's job, until its
   last handle goes; from then a message to that vector finds no
   interrupt and is dropped after its EOI.
   Without interrupt remapping a device can write any vector (ipc.md I7),
   so an old device's message can reach the vector's next owner; drivers
   treat such a packet as a spurious notification. */

#include "interrupt.h"
#include "cpu.h"
#include "../mm/page.h"
#include "../x86_64/clock.h"
#include "../x86_64/x86_64.h"

/* The interrupt on each message-signalled vector, or 0. */
static INTERRUPT *vectors[MSI_VECTOR_COUNT];

static void last_handle(OBJECT *object)
{
    INTERRUPT *interrupt = (INTERRUPT *)object;
    UINT64 flags = save_and_disable_interrupts();

    vectors[interrupt->vector - MSI_VECTOR_FIRST] = 0;
    interrupt->vector = 0;
    if (interrupt->port)
        interrupt_unbind(interrupt);
    job_uncharge_vector(interrupt->job);
    restore_interrupts(flags);
    object_release(&interrupt->job->object);
}

static void destroy(OBJECT *object)
{
    page_free_mapped(object);
}

static const OBJECT_TYPE interrupt_type = {TYPE_INTERRUPT, last_handle,
                                           destroy, 0, 0};

STATUS interrupt_create(const RESOURCE *resource, UINT64 source,
                        UINT32 options, JOB *job, INTERRUPT **out,
                        UINT64 *address, UINT32 *data)
{
    INTERRUPT *interrupt;
    UINT64 flags;
    STATUS status;
    UINT32 i;

    if (!resource_covers(resource, RESOURCE_IRQ, source, 1))
        return ERR_OUT_OF_RANGE;
    if (source < INTERRUPT_SOURCE_MSI)
        return ERR_NOT_SUPPORTED;
    if (options != 0)
        return ERR_INVALID_ARGS;
    status = job_charge_vector(job);
    if (status != OK)
        return status;
    interrupt = page_alloc_mapped();
    if (!interrupt) {
        job_uncharge_vector(job);
        return ERR_NO_MEMORY;
    }
    flags = save_and_disable_interrupts();
    for (i = 0; i < MSI_VECTOR_COUNT && vectors[i]; i++)
        ;
    if (i < MSI_VECTOR_COUNT)
        vectors[i] = interrupt;
    restore_interrupts(flags);
    if (i == MSI_VECTOR_COUNT) {
        page_free_mapped(interrupt);
        job_uncharge_vector(job);
        return ERR_NO_RESOURCES;
    }
    object_init(&interrupt->object, &interrupt_type);
    object_retain(&job->object);
    interrupt->job = job;
    interrupt->source = source;
    interrupt->vector = MSI_VECTOR_FIRST + i;
    interrupt->node.packet.type = PACKET_INTERRUPT;
    apic_msi_message(interrupt->vector, address, data);
    *out = interrupt;
    return OK;
}

/* Queue the packet, stamped with the time, and mark it outstanding. With
   interrupts disabled and interrupt bound. */
static void queue_packet(INTERRUPT *interrupt)
{
    interrupt->outstanding = 1;
    interrupt->node.packet.payload[0] = clock_monotonic();
    port_enqueue(interrupt->port, &interrupt->node);
}

/* A message that arrived before the binding was remembered as pending,
   and is delivered at once. The binding holds the port (O10). A port
   whose last handle has gone takes no binding: its last-handle rule has
   cancelled the bindings it had, and no packet queued there would be
   read. */
STATUS interrupt_bind(INTERRUPT *interrupt, PORT *port, UINT64 key)
{
    UINT64 flags = save_and_disable_interrupts();
    STATUS status = OK;

    if (interrupt->port || !interrupt->vector || port->object.handles == 0) {
        status = ERR_BAD_STATE;
    } else {
        object_retain(&port->object);
        interrupt->port = port;
        interrupt->node.packet.key = key;
        interrupt->next_bound = port->bound;
        port->bound = interrupt;
        if (interrupt->pending) {
            interrupt->pending = 0;
            queue_packet(interrupt);
        }
    }
    restore_interrupts(flags);
    return status;
}

STATUS interrupt_ack(INTERRUPT *interrupt)
{
    UINT64 flags = save_and_disable_interrupts();
    STATUS status = OK;

    if (!interrupt->port) {
        status = ERR_BAD_STATE;
    } else {
        interrupt->outstanding = 0;
        if (interrupt->pending) {
            interrupt->pending = 0;
            queue_packet(interrupt);
        }
    }
    restore_interrupts(flags);
    return status;
}

/* The port reference goes through the work list, so this may run inside
   the port's own last-handle rule. */
void interrupt_unbind(INTERRUPT *interrupt)
{
    PORT *port = interrupt->port;
    INTERRUPT **link;

    port_remove(port, &interrupt->node);
    for (link = &port->bound; *link; link = &(*link)->next_bound)
        if (*link == interrupt) {
            *link = interrupt->next_bound;
            break;
        }
    interrupt->port = 0;
    interrupt->next_bound = 0;
    interrupt->outstanding = 0;
    interrupt->pending = 0;
    object_release(&port->object);
}

/* The EOI comes first, as I7 orders it, so the local APIC delivers every
   other vector meanwhile; interrupts stay disabled to the handler's
   end. */
void interrupt_fired(UINT32 vector)
{
    INTERRUPT *interrupt = vectors[vector - MSI_VECTOR_FIRST];

    apic_eoi();
    if (!interrupt)
        return;
    if (!interrupt->port || interrupt->outstanding)
        interrupt->pending = 1;
    else
        queue_packet(interrupt);
}
