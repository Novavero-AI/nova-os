/** @file
    What the platform driver does with an input driver that has ended
    (docs/spec/input.md K4). An input driver ends with
    VIRTIO_INPUT_EXIT_DONE once its client has closed its endpoint, which
    is what a client's end does to every endpoint it holds. The kernel
    closes a process's handles before it asserts its TERMINATED
    (docs/spec/objects.md O10), so the driver's end can be read before the
    client's, and the decision here is of the facts alone, never of which
    was read first: a driver ended for its client's end waits until that
    end is read, and then follows from it. */

#ifndef NOVA_DRIVER_END_H
#define NOVA_DRIVER_END_H

#include "types.h"

/** @name What follows a driver's end
    The values driver_end_decide returns.
    @{ */
#define DRIVER_END_RESTART 0 /**< its own end: start it again as the
                                  restart policy allows, counting the
                                  restart */
#define DRIVER_END_WAIT 1    /**< its client closed its endpoint, and that
                                  client's end is not read yet: wait */
#define DRIVER_END_CLIENT_RESTARTED 2 /**< start it again, counting no
                                           restart: its client was
                                           started again */
#define DRIVER_END_CLIENT_CHANGED 3   /**< start it again, counting no
                                           restart: another client has
                                           taken its client's place */
#define DRIVER_END_GIVEN_UP 4         /**< leave it down: its client is
                                           given up, and nothing would
                                           read its endpoint */
/** @} */

/** The facts of an input driver's end. A client is named by a number
    that is its own among the clients, and a start of it by a number that
    differs from every other start of any client. */
typedef struct {
    UINT32 endpoint_closed; /**< it ended with VIRTIO_INPUT_EXIT_DONE: its
                                 client closed its endpoint */
    UINT32 served;          /**< its endpoint went to a client since the
                                 driver started */
    UINT32 served_client;   /**< that client, if served */
    UINT32 served_start;    /**< that client's start then, if served */
    UINT32 client;          /**< the client the input drivers have now */
    UINT32 start;           /**< that client's start now */
    UINT32 client_given_up; /**< that client is given up */
} DRIVER_END;

/** What follows the end of the driver whose facts are end: one of the
    DRIVER_END_ values, the first of these that holds. The client is
    given up: the driver is left down, whatever its end. The driver did
    not end for its client's closing its endpoint, or no client took its
    endpoint: it has ended of itself. Its client is another, or has been
    started again since: it is started again for that. Else the client is
    as it was, and the driver waits: its end is the client's, read first,
    or the client closed the endpoint and runs on. A driver that waits is
    decided again from the facts as they change. */
UINT32 driver_end_decide(const DRIVER_END *end);

#endif
