/* Channels (docs/spec/ipc.md I1, I2): a pair of endpoints, each the other's
   peer, where a message written to one is queued at the other. This
   kernel carries messages the kernel itself writes, which hold handles
   and no bytes, such as a new process's bootstrap message (I6), and lets
   user code read them with channel_read. */

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
    ENDPOINT *peer;           /* 0 once either end's last handle is gone */
    MESSAGE *first, *last;    /* the messages queued here, oldest first */
};

/* The rights a new endpoint's handle carries: every right a channel
   endpoint has, which never includes DUPLICATE (ipc.md I1). */
#define CHANNEL_RIGHTS                                                \
    (CHANNEL_RIGHT_READ | CHANNEL_RIGHT_WRITE | RIGHT_TRANSFER | RIGHT_WAIT)

/* A new channel's two endpoints, each with its creator's handle. Returns
   OK or ERR_NO_MEMORY. */
STATUS channel_create(ENDPOINT **first, ENDPOINT **second);

/* Queue a message of count handles, which the caller holds, at the peer
   of from. The handles pass to the message. Returns OK, ERR_PEER_CLOSED
   or ERR_NO_MEMORY; on failure the caller keeps the handles. */
STATUS channel_write_handles(ENDPOINT *from, const CARRIED_HANDLE *handles,
                             UINT32 count);

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
