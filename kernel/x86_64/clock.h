/* The kernel's monotonic clock, in nanoseconds since clock_init, read
   from the timestamp counter at the rate nova-boot measured
   (BOOT_INFO.tsc_per_ms). Deadlines are times on this clock. */

#ifndef NOVA_CLOCK_H
#define NOVA_CLOCK_H

#include "types.h"

#define NS_PER_US 1000ULL
#define NS_PER_MS 1000000ULL
#define NS_PER_SECOND 1000000000ULL

/* Start the clock at 0. Returns 0 if tsc_per_ms is 0, which no counter
   that runs can measure. */
int clock_init(UINT64 tsc_per_ms);

UINT64 clock_monotonic(void);

#endif
