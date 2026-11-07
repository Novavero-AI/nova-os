/* The x86-64 layer's start, each step logged to the serial port. */

#include "arch.h"
#include "x86_64.h"
#include "cpu.h"
#include "paging.h"
#include "serial.h"
#include "speculation.h"

void arch_init(void)
{
    gdt_load();
    serial_write("nova: gdt and tss loaded\r\n");
    idt_load();
    serial_write("nova: idt loaded\r\n");
    /* The handler logs this and returns, which shows the IDT works before
       anything depends on it. */
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
    serial_write("nova: timer interrupts arriving\r\n");
}
