/* The x86-64 layer's start, each step logged to the serial port. */

#include "arch.h"
#include "x86_64.h"
#include "cpu.h"
#include "serial.h"

void arch_init(UINT64 tsc_per_ms)
{
    gdt_load();
    serial_write("nova: gdt loaded\r\n");
    idt_load();
    serial_write("nova: idt loaded\r\n");
    /* The handler logs this and returns, which shows the IDT works before
       any device interrupt depends on it. */
    breakpoint();
    apic_timer_start(tsc_per_ms);
    enable_interrupts();
    timer_sleep_until(timer_ticks() + 1);
    serial_write("nova: timer interrupts arriving\r\n");
}
