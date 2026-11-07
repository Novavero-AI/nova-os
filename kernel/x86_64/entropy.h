/* The processor's sources for the kernel's random number generator
   (kernel/random): its random-number instructions, RDSEED and RDRAND
   (SDM Vol. 1 7.3.17), and the timestamp counter. */

#ifndef NOVA_ENTROPY_H
#define NOVA_ENTROPY_H

#include "types.h"

typedef enum {
    CPU_RANDOM_NONE,
    CPU_RANDOM_RDSEED,
    CPU_RANDOM_RDRAND
} CPU_RANDOM;

/* Whether the processor has the instruction. */
int cpu_random_supported(CPU_RANDOM instruction);

/* Fill count words with values of the instruction, which the processor
   must have. Returns 1, or 0 if a value did not come within the retry
   limit. */
int cpu_random_read(CPU_RANDOM instruction, UINT64 *words, UINT64 count);

/* "RDSEED", "RDRAND" or "none", for the log. */
const char *cpu_random_name(CPU_RANDOM instruction);

/* The timestamp counter. */
UINT64 cycle_counter(void);

#endif
