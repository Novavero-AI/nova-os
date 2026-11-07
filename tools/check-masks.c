/* Checks the bounds-check-bypass masks of include/cpu.h,
   index_masked and range_selected (docs/spec/speculation.md M4), against
   the comparisons they stand in for: every combination of the values at
   and around each boundary, then pseudo-random values. The masks are
   x86-64 assembly, so the program must be built for x86-64; on an Apple
   silicon Mac, -arch x86_64 builds one that Rosetta runs.

   cc -std=c11 -Wall -Wextra -pedantic -I include -o check-masks \
     tools/check-masks.c && ./check-masks */

#include <stdio.h>

#include "cpu.h"

#define RANDOM_CASES 4000000

static UINT64 failures;

static UINT64 expected_index(UINT64 index, UINT64 size)
{
    return index < size ? index : 0;
}

static UINT64 expected_range(UINT64 address, UINT64 size, UINT64 base,
                             UINT64 top)
{
    return address >= base && address <= top && size <= top - address
               ? address
               : 0;
}

static void check_index(UINT64 index, UINT64 size)
{
    UINT64 got = index_masked(index, size);

    if (got != expected_index(index, size)) {
        if (failures++ < 10)
            printf("index_masked(%llx, %llx) = %llx\n", index, size, got);
    }
}

static void check_range(UINT64 address, UINT64 size, UINT64 base, UINT64 top)
{
    UINT64 got = range_selected(address, size, base, top);

    if (got != expected_range(address, size, base, top)) {
        if (failures++ < 10)
            printf("range_selected(%llx, %llx, %llx, %llx) = %llx\n",
                   address, size, base, top, got);
    }
}

/* xorshift64 (Marsaglia, "Xorshift RNGs", 2003), with a fixed seed so
   that every run checks the same values. */
static UINT64 next_random(UINT64 *state)
{
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    return *state;
}

/* A value from the bits of r: small, near a power of two, near the top
   of the range, or any, so that random cases still meet the edges. */
static UINT64 shaped(UINT64 r)
{
    UINT64 low = r >> 8 & 0xff;

    switch (r & 3) {
    case 0:
        return low;
    case 1:
        return (1ULL << (r >> 16 & 63)) + low - 0x80;
    case 2:
        return ~0ULL - low;
    default:
        return r;
    }
}

int main(void)
{
    /* The boundaries of the kernel's own uses (the user range of 4-level
       and 5-level paging, the call and handle tables' sizes) and of the
       arithmetic (0, the sign bit, the top). */
    static const UINT64 edges[] = {
        0,
        1,
        2,
        0x1f,
        0x20,
        0x21,
        0x1ff,
        0x200,
        0xffff,
        0x10000,
        0x10001,
        0x7ffffffff000ULL,
        0x7ffffffff001ULL,
        0x7fffffffffffULL,
        0x800000000000ULL,
        0xfffffffffffff000ULL,
        0x7fffffffffffffffULL,
        0x8000000000000000ULL,
        0xfffffffffffffffeULL,
        0xffffffffffffffffULL,
    };
    const UINT64 count = sizeof edges / sizeof edges[0];
    UINT64 a, s, b, t, i, state = 0x9e3779b97f4a7c15ULL, checks = 0;

    for (a = 0; a < count; a++)
        for (s = 0; s < count; s++) {
            check_index(edges[a], edges[s]);
            checks++;
            for (b = 0; b < count; b++)
                for (t = 0; t < count; t++) {
                    check_range(edges[a], edges[s], edges[b], edges[t]);
                    checks++;
                }
        }
    for (i = 0; i < RANDOM_CASES; i++) {
        UINT64 address = shaped(next_random(&state));
        UINT64 size = shaped(next_random(&state));
        UINT64 base = shaped(next_random(&state));
        UINT64 top = shaped(next_random(&state));

        check_index(address, size);
        check_range(address, size, base, top);
        checks += 2;
    }
    printf("%llu checks, %llu failed\n", checks, failures);
    return failures != 0;
}
