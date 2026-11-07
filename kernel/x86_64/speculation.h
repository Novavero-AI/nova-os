/* The speculative-execution measures (docs/spec/speculation.md): which
   apply on this processor, found once at boot, and the parts of them
   that run outside the entry and exit code. */

#ifndef NOVA_SPECULATION_H
#define NOVA_SPECULATION_H

#include "types.h"

/* Read the enumeration (M2), decide the measures (M3), set those that are
   set once at boot, enable the entry and exit's measure sites, start
   page-table isolation where it applies (M11), and log one line naming
   each class, what it gets and why. After paging_init and gdt_load, with
   interrupts disabled, and before any entry from user mode. */
void speculation_init(void);

/* The process switch's barrier (M3): IBPB (M6), then on AMD processors
   with neither IBPB_RET nor ERAPS the 32-CALL overwrite (M7). Called
   before the CR3 load of a switch to a thread of another process than the
   last user thread this processor ran. */
void speculation_process_switch(void);

/* wait_for_interrupt, with VERW before the HLT where M13 applies. */
void speculation_wait_for_interrupt(void);

#endif
