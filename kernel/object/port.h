/* Ports (docs/spec/ipc.md I4): queues of packets that threads wait on
   with port_wait. This kernel queues interrupt packets, from interrupts
   bound to the port (interrupt.h), and signal packets, from watches
   object_wait_async registers; port_queue is not implemented. */

#ifndef NOVA_PORT_H
#define NOVA_PORT_H

#include "object.h"
#include "../sched/wait.h"

/* The most watches a port takes at once, registered or with a packet
   queued: one for each handle a process can hold. The kernel charges no
   job for the page each watch takes (docs/spec/objects.md O6), and this
   bound keeps the pages one process's watches hold to what the handles
   it can hold bound already. */
#define PORT_WATCHES_MAX HANDLE_TABLE_CAPACITY

/* The rights of a new port's handle: every right a port has. */
#define PORT_RIGHTS                                                      \
    (PORT_RIGHT_QUEUE | PORT_RIGHT_READ | RIGHT_DUPLICATE | RIGHT_TRANSFER)

/* A packet as it waits in a port's queue. Its source allocates it when
   it registers (ipc.md I4), so queueing it never needs memory. taken, if
   set, runs once the packet has left the queue, read or discarded: a
   watch's packet is the last of the watch, which taken frees. */
typedef struct PACKET_NODE PACKET_NODE;

struct PACKET_NODE {
    PACKET_NODE *next;
    int queued;
    void (*taken)(PACKET_NODE *node);
    PORT_PACKET packet;
};

struct INTERRUPT;
struct WATCH;

typedef struct {
    OBJECT object;
    WAITABLE waitable; /* PORT_SIGNAL_PACKET while a packet is queued */
    PACKET_NODE *first, *last;
    struct INTERRUPT *bound; /* the interrupts bound to the port */
    struct WATCH *watches;   /* the watches registered to queue here */
    UINT32 watch_count;      /* registered or with a packet queued */
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

/* port_remove, then run node's taken if it was queued. */
void port_discard(PORT *port, PACKET_NODE *node);

/* object_wait_async (docs/spec/objects.md O9, ipc.md I4): a one-shot
   watch of object, whose signals are at waitable, made through the
   handle value handle in table, that queues one PACKET_SIGNAL carrying
   key on port once any of signals is active, at once if one is already.
   Its packet is allocated now. The watch holds object and port until
   its packet leaves the port; it ends with no packet if the handle
   leaves table first, or once port's last handle closes. The caller has
   checked both handles and keeps interrupts disabled from those checks
   through this call, so neither handle can close in between unseen.
   Returns OK, or ERR_NO_MEMORY if no page is free or port has
   PORT_WATCHES_MAX watches already. */
STATUS port_watch(PORT *port, OBJECT *object, WAITABLE *waitable,
                  const HANDLE_TABLE *table, UINT32 handle, UINT64 key,
                  UINT32 signals);

/* port_wait through the handle value in table, which needs
   PORT_RIGHT_READ: copy the first queued packet to the PORT_PACKET at
   user address packet and dequeue it, waiting until the deadline for one
   if none is queued. Returns OK; the handle's check (handle_get);
   ERR_BAD_ADDRESS, which leaves the packet queued (docs/spec/syscalls.md
   S6); ERR_TIMED_OUT; or ERR_CANCELED if the handle leaves table
   meanwhile (docs/spec/objects.md O9). */
STATUS port_wait(HANDLE_TABLE *table, UINT32 value, UINT64 deadline,
                 UINT64 packet);

#endif
