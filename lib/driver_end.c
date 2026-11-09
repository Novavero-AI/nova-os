/* What follows an input driver's end (docs/spec/input.md K4). The
   platform driver reads one packet for a client's end and another for its
   driver's, in the order the kernel queued them, and the kernel closes
   the client's handles, which wakes the driver, before it queues the
   client's packet (docs/spec/objects.md O10). So a driver's end read
   with its client as it was does not say the end is the driver's own:
   it may be the client's, read first. The decision is of the facts of
   the driver's end alone, and is made again from them when the client's
   start or the client changes, which is when it can differ. */

#include "driver_end.h"

UINT32 driver_end_decide(const DRIVER_END *end)
{
    UINT32 client_as_it_was =
        end->client == end->served_client && end->start == end->served_start;

    if (end->client_given_up)
        return DRIVER_END_GIVEN_UP;
    if (!end->endpoint_closed || !end->served)
        return DRIVER_END_RESTART;
    if (client_as_it_was)
        return DRIVER_END_WAIT;
    return end->client == end->served_client ? DRIVER_END_CLIENT_RESTARTED
                                             : DRIVER_END_CLIENT_CHANGED;
}
