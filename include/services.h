/* The store service and the namespace server, which the first process
   starts (docs/spec/storage.md F16): their bootstrap messages, the store
   service's own request, their limits on clients, and their exit codes. */

#ifndef NOVA_SERVICES_H
#define NOVA_SERVICES_H

#include "native_abi.h"

/* A service serves at most SERVICE_CLIENTS clients at once, each with at
   most SERVICE_CLIENT_CHANNELS channels open (storage.md F17): together
   the WAIT_MANY_MAX channels one object_wait_many watches, which is how
   a service waits for all of them. Eight channels hold a program's
   closure directory, a directory it lists and the files it keeps open,
   with room to spare. */
#define SERVICE_CLIENTS 8
#define SERVICE_CLIENT_CHANNELS (WAIT_MANY_MAX / SERVICE_CLIENTS)

_Static_assert(SERVICE_CLIENTS * SERVICE_CLIENT_CHANNELS == WAIT_MANY_MAX,
               "the clients' channels are every channel one wait watches");

/* The store service's bootstrap message: these handles in this order,
   and a STORE_BOOTSTRAP. The image is a section it may only read; its
   control channel carries STORE_RIGHT_CLOSURE; the root directory lists
   every path the image holds, with IO_RIGHT_READ, _EXECUTE and _LIST. */
#define STORE_BOOTSTRAP_DEBUG_LOG 0
#define STORE_BOOTSTRAP_SELF 1 /* its process, with MAP */
#define STORE_BOOTSTRAP_IMAGE 2
#define STORE_BOOTSTRAP_CONTROL 3
#define STORE_BOOTSTRAP_ROOT 4
#define STORE_BOOTSTRAP_HANDLES 5

typedef struct {
    UINT64 image_size;
} STORE_BOOTSTRAP;

/* The rights of the store service's control channel (storage.md F12);
   this milestone has CLOSURE alone. */
#define STORE_RIGHT_CLOSURE 0x1

/* Open the closure directory of a store path (F5): on the control
   channel, an IO_REQUEST with this operation, the rights wanted, of
   IO_RIGHT_READ, _EXECUTE and _LIST, length the path's base name's
   bytes, which follow, and one handle, an endpoint, which the service
   answers and serves as an IO_OPEN's. The directory's only entries are
   the paths of the closure. Needs STORE_RIGHT_CLOSURE. */
#define STORE_OPEN_CLOSURE 0x100

/* The namespace server's bootstrap message: the debug log's resource,
   then the server end of its root directory's channel, which carries
   IO_RIGHT_READ, _EXECUTE, _LIST and _PUBLISH. */
#define NAMESPACE_BOOTSTRAP_DEBUG_LOG 0
#define NAMESPACE_BOOTSTRAP_ROOT 1
#define NAMESPACE_BOOTSTRAP_HANDLES 2

/* A service ends once no channel it serves is left open, with
   SERVICE_EXIT_DONE, nonzero so that its creator tells it from a
   default; with SERVICE_EXIT_FAILED if it could not start. */
#define SERVICE_EXIT_DONE 100
#define SERVICE_EXIT_FAILED 1

#endif
