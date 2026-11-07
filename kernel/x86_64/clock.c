/* The monotonic clock from the timestamp counter. RDTSC returns "a
   monotonically increasing unique value whenever executed, except for a
   64-bit counter wraparound", which Intel guarantees not to happen
   within 10 years of reset (SDM Vol. 3B 20.17). The counter keeps a
   constant rate only where it is invariant (SDM Vol. 3B 20.17.1), which
   neither nova-boot nor the kernel checks yet (docs/spec/boot-protocol.md,
   Undecided). */

#include "clock.h"
#include "cpu.h"

static UINT64 start;
static UINT64 per_ms;

int clock_init(UINT64 tsc_per_ms)
{
    if (!tsc_per_ms)
        return 0;
    per_ms = tsc_per_ms;
    start = rdtsc();
    return 1;
}

/* elapsed * NS_PER_MS / per_ms without overflowing 64 bits: whole
   milliseconds first, then the remainder, which is below per_ms. */
UINT64 clock_monotonic(void)
{
    UINT64 elapsed = rdtsc() - start;

    return elapsed / per_ms * NS_PER_MS +
           elapsed % per_ms * NS_PER_MS / per_ms;
}
