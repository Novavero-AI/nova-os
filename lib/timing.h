/** @file
    The time a program logs: the monotonic clock in nanoseconds, and a
    span of it as the milliseconds a log line gives
    (docs/spec/log.md T1). */

#ifndef NOVA_TIMING_H
#define NOVA_TIMING_H

#include "native_abi.h"

/** Nanoseconds in a millisecond. */
#define TIMING_NS_PER_MS 1000000ULL

/** The monotonic clock's time in nanoseconds; 0 if the call fails. */
UINT64 timing_now(void);

/** Write nanoseconds to the log as milliseconds with one decimal,
    rounded down, as 12.3. */
void timing_say_ms(UINT64 nanoseconds);

#endif
