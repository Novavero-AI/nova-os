/* RDSEED and RDRAND. RDSEED reads the processor's non-deterministic
   random bit generator, RDRAND a deterministic generator the processor
   reseeds from it (SDM Vol. 1 7.3.17.1, 7.3.17.2). Either reports in CF
   whether its destination holds a value; without one it writes zero
   (SDM Vol. 2B, RDRAND and RDSEED, Operation). AMD reports both
   instructions with the same CPUID bits (Processor Programming Reference
   for AMD Family 1Ah Model 02h, CPUID_Fn00000001_ECX[30] and
   CPUID_Fn00000007_EBX_x00[18]). */

#include "entropy.h"
#include "cpu.h"

/* CPUID.01H:ECX.RDRAND[30] and CPUID.07H.00H:EBX.RDSEED[18] (SDM Vol. 1
   7.3.17.1, 7.3.17.2). Leaf 7 is read only where leaf 0 reports it, since
   for a leaf above the highest basic leaf CPUID returns the highest basic
   leaf's data (SDM Vol. 1 21.1.3). */
#define CPUID_HIGHEST_LEAF 0
#define CPUID_FEATURES 1
#define CPUID_EXTENDED_FEATURES 7
#define RDRAND_BIT (1u << 30)
#define RDSEED_BIT (1u << 18)

/* SDM Vol. 1 7.3.17 recommends retrying while CF is 0, for a limited
   number of times so that a failed generator does not hang the caller.
   For RDRAND its example stops after 10 tries. For RDSEED it gives no
   number, and it does not call RDSEED's underflow unlikely as it does
   RDRAND's, so RDSEED gets ten times as many; a draw is a few words, at
   boot and at each reseed, so even a hundred failed tries cost nothing
   that matters. */
#define RDRAND_RETRIES 10
#define RDSEED_RETRIES 100

/* RDSEED RAX and RDRAND RAX, which TinyCC's assembler does not know:
   REX.W 0F C7 /7 and REX.W 0F C7 /6 (SDM Vol. 2B), with ModRM 0xF8 and
   0xF0 naming RAX. SETC then copies CF out. */
static int rdseed(UINT64 *value)
{
    UINT64 v;
    UINT32 ok;

    __asm__ volatile(".byte 0x48, 0x0f, 0xc7, 0xf8\n\t"
                     "setc %%dl\n\t"
                     "movzbl %%dl, %%edx"
                     : "=a"(v), "=d"(ok)
                     :
                     : "cc");
    *value = v;
    return (int)ok;
}

static int rdrand(UINT64 *value)
{
    UINT64 v;
    UINT32 ok;

    __asm__ volatile(".byte 0x48, 0x0f, 0xc7, 0xf0\n\t"
                     "setc %%dl\n\t"
                     "movzbl %%dl, %%edx"
                     : "=a"(v), "=d"(ok)
                     :
                     : "cc");
    *value = v;
    return (int)ok;
}

int cpu_random_supported(CPU_RANDOM instruction)
{
    UINT32 highest, a, b, c, d;

    cpuid(CPUID_HIGHEST_LEAF, &highest, &b, &c, &d);
    switch (instruction) {
    case CPU_RANDOM_RDSEED:
        if (highest < CPUID_EXTENDED_FEATURES)
            return 0;
        cpuid(CPUID_EXTENDED_FEATURES, &a, &b, &c, &d);
        return (b & RDSEED_BIT) != 0;
    case CPU_RANDOM_RDRAND:
        cpuid(CPUID_FEATURES, &a, &b, &c, &d);
        return (c & RDRAND_BIT) != 0;
    case CPU_RANDOM_NONE:
        break;
    }
    return 0;
}

/* One value from step, tried up to retries times. */
static int retry(int (*step)(UINT64 *), int retries, UINT64 *value)
{
    int tries;

    for (tries = 0; tries < retries; tries++)
        if (step(value))
            return 1;
    return 0;
}

int cpu_random_read(CPU_RANDOM instruction, UINT64 *words, UINT64 count)
{
    UINT64 i;

    for (i = 0; i < count; i++) {
        int ok = 0;

        switch (instruction) {
        case CPU_RANDOM_RDSEED:
            ok = retry(rdseed, RDSEED_RETRIES, &words[i]);
            break;
        case CPU_RANDOM_RDRAND:
            ok = retry(rdrand, RDRAND_RETRIES, &words[i]);
            break;
        case CPU_RANDOM_NONE:
            break;
        }
        if (!ok)
            return 0;
    }
    return 1;
}

const char *cpu_random_name(CPU_RANDOM instruction)
{
    switch (instruction) {
    case CPU_RANDOM_RDSEED:
        return "RDSEED";
    case CPU_RANDOM_RDRAND:
        return "RDRAND";
    case CPU_RANDOM_NONE:
        break;
    }
    return "none";
}

UINT64 cycle_counter(void)
{
    return rdtsc();
}
