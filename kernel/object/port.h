/* Ports (docs/spec/ipc.md I4): queues of packets that threads wait on
   with port_wait. This kernel queues interrupt packets, from interrupts
   bound to the port (interrupt.h); port_queue and watches are not
   implemented yet. */

#ifndef NOVA_PORT_H
#define NOVA_PORT_H

#include "object.h"
#include "../sched/wait.h"

/* The rights of a new port's handle: every right a port has. */
#define PORT_RIGHTS                                                      \
    (PORT_RIGHT_QUEUE | PORT_RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER)

/* A packet as it waits in a port's queue. Its source allocates it when
   it registers (ipc.md I4), so queueing it never needs memory. */
typedef struct PACKET_NODE PACKET_NODE;

struct PACKET_NODE {
    PACKET_NODE *next;
    int queued;
    PORT_PACKET packet;
};

struct INTERRUPT;

typedef struct {
    OBJECT object;
    WAITABLE waitable; /* PORT_SIGNAL_PACKET while a packet is queued */
    PACKET_NODE *first, *last;
    struct INTERRUPT *bound; /* the interrupts bound to the port */
    UINT32 capacity;
} PORT;

/* A port that holds up to capacity user packets, with its creator's
   handle. Returns OK, ERR_INVALID_ARGS for a capacity of 0,
   ERR_OUT_OF_RANGE above PORT_MAX_CAPACITY, or ERR_NO_MEMORY. */
STATUS port_create(UINT32 capacity, PORT **port);

/* Queue node at the end of port's queue, if it is not queued already. */
void port_enqueue(PORT *port, PACKET_NODE *node);

/* Take node out of port's queue, if it is queued. */
void port_remove(PORT *port, PACKET_NODE *node);

/* port_wait: copy the first queued packet to the PORT_PACKET at user
   address packet and dequeue it, waiting until the deadline for one if
   none is queued. Returns OK, ERR_TIMED_OUT, or ERR_BAD_ADDRESS, which
   leaves the packet queued (docs/spec/syscalls.md S6). */
STATUS port_wait(PORT *port, UINT64 deadline, UINT64 packet);

#endif
