/* Channels (docs/spec/ipc.md I1, I2): a pair of endpoints, each the other's
   peer, where a message of bytes and handles written to one is queued at
   the other. */

#ifndef NOVA_CHANNEL_H
#define NOVA_CHANNEL_H

#include "object.h"

/* A handle travelling in a message, with the rights it will carry. */
typedef struct {
    OBJECT *object;
    UINT32 rights;
} CARRIED_HANDLE;

typedef struct MESSAGE MESSAGE;

typedef struct ENDPOINT ENDPOINT;

struct ENDPOINT {
    OBJECT object;
    WAITABLE waitable;
    ENDPOINT *peer;           /* 0 once either end's last handle is gone */
    MESSAGE *first, *last;    /* the messages queued here, oldest first */
    UINT32 queued;
    UINT32 capacity;          /* the most messages queued here at once */
};

/* The rights a new endpoint's handle carries: every right a channel
   endpoint has, which never includes DUPLICATE (ipc.md I1). */
#define CHANNEL_RIGHTS                                                \
    (CHANNEL_RIGHT_READ | CHANNEL_RIGHT_WRITE | RIGHT_TRANSFER | RIGHT_WAIT)

/* A new channel's two endpoints, each queueing up to capacity messages,
   each with its creator's handle. Returns OK; ERR_INVALID_ARGS for a
   capacity of 0; ERR_OUT_OF_RANGE for one above CHANNEL_MAX_CAPACITY; or
   ERR_NO_MEMORY. */
STATUS channel_create(UINT32 capacity, ENDPOINT **first, ENDPOINT **second);

/* Queue a message the kernel writes at the peer of from: byte_count
   bytes of kernel memory at bytes, and count handles, which the caller
   holds and which pass to the message. Returns OK, ERR_OUT_OF_RANGE,
   ERR_PEER_CLOSED, ERR_SHOULD_WAIT or ERR_NO_MEMORY; on failure the
   caller keeps the handles. */
STATUS channel_write_kernel(ENDPOINT *from, const UINT8 *bytes,
                            UINT32 byte_count, const CARRIED_HANDLE *handles,
                            UINT32 count);

/* channel_write for the endpoint from the writer's table, with the user
   buffers the call names (docs/spec/syscalls.md S8): byte_count bytes
   at bytes, and handle_count HANDLE_TRANSFER entries at handles, whose
   handles leave table. All or nothing (I2). */
STATUS channel_write_message(ENDPOINT *endpoint, HANDLE_TABLE *table,
                             UINT64 bytes, UINT32 byte_count, UINT64 handles,
                             UINT32 handle_count);

/* channel_read for the endpoint into the reader's table, with the user
   buffers the call names (docs/spec/syscalls.md S8): copy the first
   queued message out and move its handles into table, writing a
   RECEIVED_HANDLE for each at handles. Reports the message's byte and
   handle counts in byte_count and handle_count, also when its buffers
   are too small. All or nothing: on failure the message stays queued
   and table is as it was. */
STATUS channel_read_message(ENDPOINT *endpoint, HANDLE_TABLE *table,
                            UINT64 bytes, UINT32 byte_capacity,
                            UINT64 handles, UINT32 handle_capacity,
                            UINT32 *byte_count, UINT32 *handle_count);

#endif
