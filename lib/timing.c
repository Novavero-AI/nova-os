/* The time a program logs (timing.h). */

#include "timing.h"
#include "log.h"
#include "native.h"

#define TENTHS_PER_MS 10

UINT64 timing_now(void)
{
    UINT64 nanoseconds = 0;

    clock_monotonic(&nanoseconds);
    return nanoseconds;
}

void timing_say_ms(UINT64 nanoseconds)
{
    UINT64 tenths = nanoseconds * TENTHS_PER_MS / TIMING_NS_PER_MS;

    say_number(tenths / TENTHS_PER_MS);
    say(".");
    say_number(tenths % TENTHS_PER_MS);
}
