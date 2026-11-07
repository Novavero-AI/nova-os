/** @file
    The store service, the namespace server and the file system service,
    which the first process starts (docs/spec/storage.md F16, F18): their
    bootstrap messages, the store service's own request, their limits on
    clients, and their exit codes. */

#ifndef NOVA_SERVICES_H
#define NOVA_SERVICES_H

#include "native_abi.h"

/** @name Limits on clients
    A service serves at most SERVICE_CLIENTS clients at once, each with at
    most SERVICE_CLIENT_CHANNELS channels open (storage.md F17): together
    the WAIT_MANY_MAX channels one object_wait_many watches, which is how
    a service waits for all of them. Eight channels hold a program's
    closure directory, a directory it lists and the files it keeps open,
    with room to spare.
    @{ */
#define SERVICE_CLIENTS 8 /**< the clients a service serves at once */
/** the channels each client may hold open */
#define SERVICE_CLIENT_CHANNELS (WAIT_MANY_MAX / SERVICE_CLIENTS)
/** @} */

_Static_assert(SERVICE_CLIENTS * SERVICE_CLIENT_CHANNELS == WAIT_MANY_MAX,
               "the clients' channels are every channel one wait watches");

/** @name The store service's bootstrap handles
    The store service's bootstrap message: these handles in this order,
    and a STORE_BOOTSTRAP. The image is a section it may only read; its
    control channel carries STORE_RIGHT_CLOSURE; the root directory lists
    every path the image holds, with IO_RIGHT_READ, _EXECUTE and _LIST.
    @{ */
#define STORE_BOOTSTRAP_DEBUG_LOG 0 /**< the debug log's resource */
#define STORE_BOOTSTRAP_SELF 1      /**< its process, with MAP */
#define STORE_BOOTSTRAP_IMAGE 2     /**< the store image's section */
#define STORE_BOOTSTRAP_CONTROL 3   /**< its end of the control channel */
#define STORE_BOOTSTRAP_ROOT 4      /**< its end of the root directory */
#define STORE_BOOTSTRAP_HANDLES 5   /**< how many handles it carries */
/** @} */

/** The store service's bootstrap message's bytes. */
typedef struct {
    UINT64 image_size; /**< the store image's size in bytes */
} STORE_BOOTSTRAP;

/** The rights of the store service's control channel (storage.md F12);
    this milestone has CLOSURE alone. An IO_OPEN of the empty path on a
    control channel, with rights of its own at most, opens another, so
    that its holder can give one to a program and keep its own; the
    open's attributes give IO_KIND_SERVICE, and the new channel belongs
    to the client of the one it was opened through (F17). */
#define STORE_RIGHT_CLOSURE 0x1

/** Open the closure directory of a store path (F5): on the control
    channel, an IO_REQUEST with this operation, the rights wanted, of
    IO_RIGHT_READ, _EXECUTE and _LIST, length the path's base name's
    bytes, which follow, and one handle, an endpoint, which the service
    answers and serves as an IO_OPEN's. The directory's only entries are
    the paths of the closure. Needs STORE_RIGHT_CLOSURE. */
#define STORE_OPEN_CLOSURE 0x100

/** @name The namespace server's bootstrap handles
    The namespace server's bootstrap message: the debug log's resource,
    then the server end of its root directory's channel, which carries
    IO_RIGHT_READ, _WRITE, _EXECUTE, _LIST and _PUBLISH: WRITE so that a
    writable directory published there opens writable through it.
    @{ */
#define NAMESPACE_BOOTSTRAP_DEBUG_LOG 0 /**< the debug log's resource */
#define NAMESPACE_BOOTSTRAP_ROOT 1      /**< its end of the root directory */
#define NAMESPACE_BOOTSTRAP_HANDLES 2   /**< how many handles it carries */
/** @} */

/** @name The file system service's bootstrap handles
    The file system service's bootstrap message (docs/spec/storage.md
    F18): these handles in this order and no bytes. The block channel is
    a block driver's client endpoint (block_protocol.h); the root
    directory is the server end of the pool's volume's root, which carries
    IO_RIGHT_READ, _WRITE and _LIST.
    @{ */
#define FS_BOOTSTRAP_DEBUG_LOG 0 /**< the debug log's resource */
#define FS_BOOTSTRAP_SELF 1      /**< its process, with MAP */
#define FS_BOOTSTRAP_BLOCK 2     /**< the block channel */
#define FS_BOOTSTRAP_ROOT 3      /**< its end of the root directory */
#define FS_BOOTSTRAP_HANDLES 4   /**< how many handles it carries */
/** @} */

/** @name Exit codes
    A service ends once no channel it serves is left open, with
    SERVICE_EXIT_DONE, nonzero so that its creator tells it from a
    default; with SERVICE_EXIT_FAILED if it could not start or could not
    go on serving.
    @{ */
#define SERVICE_EXIT_DONE 100 /**< no channel it serves is left open */
#define SERVICE_EXIT_FAILED 1 /**< it could not start or go on */
/** @} */

#endif
