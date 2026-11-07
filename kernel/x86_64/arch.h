/* What the kernel calls in its x86-64 layer. */

#ifndef NOVA_ARCH_H
#define NOVA_ARCH_H

#include "types.h"

/* The rate of the timer interrupt. */
#define TIMER_HZ 1000

/* Install the kernel's own descriptor tables and exception handlers, then
   take over nova-boot's page tables and remove their lower-half mapping.
   Returns with interrupts disabled. */
void arch_init(void);

/* Take the interrupt controllers over from the firmware and start the
   timer, calibrated with the timestamp counter's rate nova-boot measured.
   Needs the page allocator, for a table to map the Local APIC. Returns
   with interrupts enabled. */
void arch_start_timer(UINT64 tsc_per_ms);

/* Timer interrupts since arch_start_timer started the timer. */
UINT64 timer_ticks(void);

/* Halt until timer_ticks() reaches tick. */
void timer_sleep_until(UINT64 tick);

#endif
