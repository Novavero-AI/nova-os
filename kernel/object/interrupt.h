/* Interrupt objects (docs/spec/ipc.md I7): one interrupt source, delivered
   as packets on the port it is bound to. This kernel delivers
   message-signalled interrupts, each on a vector of its own; global
   system interrupts through an I/O APIC are not implemented. */

#ifndef NOVA_INTERRUPT_H
#define NOVA_INTERRUPT_H

#include "job.h"
#include "port.h"
#include "resource.h"

/* The rights of a new interrupt's handle: every right an interrupt has. */
#define INTERRUPT_RIGHTS                                                 \
    (INTERRUPT_RIGHT_BIND | INTERRUPT_RIGHT_ACK | RIGHT_DUPLICATE |      \
     RIGHT_TRANSFER)

typedef struct INTERRUPT INTERRUPT;

struct INTERRUPT {
    OBJECT object;
    UINT64 source;
    UINT32 vector;      /* 0 once the last handle has gone */
    JOB *job;           /* charged for the vector, held while it is */
    PORT *port;         /* while bound, holding a reference */
    INTERRUPT *next_bound;
    PACKET_NODE node;   /* the one packet it queues */
    int outstanding;    /* a packet queued and not yet acknowledged */
    int pending;        /* it fired again meanwhile */
};

/* interrupt_create: an interrupt for source, which resource, an IRQ
   resource, must cover, with options, which must be 0 for a
   message-signalled source. Allocates a vector, charged to job, the
   creating process's (docs/spec/objects.md O6), until the interrupt's
   last handle goes, and reports the message address and data the device
   is to write. Returns OK; ERR_OUT_OF_RANGE if resource does not cover
   source; ERR_NOT_SUPPORTED for a global system interrupt;
   ERR_INVALID_ARGS for options other than 0; ERR_NO_RESOURCES if job
   holds its quota of vectors or no vector is free; or ERR_NO_MEMORY. */
STATUS interrupt_create(const RESOURCE *resource, UINT64 source,
                        UINT32 options, JOB *job, INTERRUPT **interrupt,
                        UINT64 *address, UINT32 *data);

/* interrupt_bind: deliver interrupt's packets, carrying key, to port.
   Returns OK, or ERR_BAD_STATE if it is bound already, or its last handle
   or the port's has gone. */
STATUS interrupt_bind(INTERRUPT *interrupt, PORT *port, UINT64 key);

/* interrupt_ack: the driver has serviced the device; queue the next
   packet now if the interrupt fired meanwhile. Returns OK, or
   ERR_BAD_STATE if it is not bound. */
STATUS interrupt_ack(INTERRUPT *interrupt);

/* Unbind interrupt from its port, taking back a queued packet. With
   interrupts disabled. */
void interrupt_unbind(INTERRUPT *interrupt);

/* The interrupt handler's part, for a message-signalled vector: write the
   local APIC's EOI and queue a packet, or remember the interrupt as
   pending while one is outstanding. */
void interrupt_fired(UINT32 vector);

#endif
