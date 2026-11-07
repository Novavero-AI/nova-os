/* The kernel's random number generator, the source of the random_read
   system call (docs/spec/syscalls.md, call 42). */

#ifndef NOVA_RANDOM_H
#define NOVA_RANDOM_H

#include "boot_info.h"

/* Check the generator's primitives and construction, halting if a check
   fails, then seed it from the firmware's seeds in info, the processor's
   random-number instruction and the timing of timer interrupts, and log
   which sources it used. Wipes the seeds in info. Needs the timer
   running. */
void random_init(BOOT_INFO *info);

/* Write count random bytes to buffer, which must be kernel memory that
   cannot fault, since the bytes are copied with interrupts disabled.
   Returns 1, or 0 with buffer untouched if no source of real entropy has
   seeded the generator. Never waits. */
int random_read(VOID *buffer, UINT64 count);

/* Check ChaCha20, SHA-256 and the generator against published values.
   Returns 0 if every check passes, otherwise the one that failed. */
const char *random_self_test(void);

#endif
