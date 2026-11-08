/* The x86-64 layer's start. */

#include "arch.h"
#include "x86_64.h"
#include "cpu.h"
#include "paging.h"
#include "speculation.h"

void arch_init(void)
{
    gdt_load();
    idt_load();
    /* The handler returns, which shows the IDT works before anything
       depends on it; with a broken IDT the breakpoint faults instead. */
    breakpoint();
    paging_init();
    speculation_init();
    user_mode_init();
}

void arch_start_timer(UINT64 tsc_per_ms)
{
    apic_timer_start(tsc_per_ms);
    enable_interrupts();
    timer_sleep_until(timer_ticks() + 1);
}
