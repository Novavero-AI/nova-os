/* The x86-64 instructions the kernel's C code uses and C cannot express. */

#ifndef NOVA_CPU_H
#define NOVA_CPU_H

#include "types.h"

static inline void outb(UINT16 port, UINT8 value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "d"(port));
}

static inline UINT8 inb(UINT16 port)
{
    UINT8 value;

    __asm__ volatile("inb %1, %0" : "=a"(value) : "d"(port));
    return value;
}

static inline UINT64 rdtsc(void)
{
    UINT32 lo, hi;

    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (UINT64)hi << 32 | lo;
}

static inline UINT64 rdmsr(UINT32 msr)
{
    UINT32 lo, hi;

    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (UINT64)hi << 32 | lo;
}

/* The linear address a page fault was raised for. */
static inline UINT64 read_cr2(void)
{
    UINT64 value;

    __asm__ volatile("mov %%cr2, %0" : "=r"(value));
    return value;
}

static inline void breakpoint(void)
{
    __asm__ volatile("int3");
}

static inline void enable_interrupts(void)
{
    __asm__ volatile("sti" : : : "memory");
}

static inline void disable_interrupts(void)
{
    __asm__ volatile("cli" : : : "memory");
}

/* Halt until an interrupt has been handled; called and returning with
   interrupts disabled. Interrupts stay inhibited on the instruction
   boundary after STI (SDM Vol. 2B, STI), so one that became pending after
   the caller's last check wakes the HLT instead of being taken before it. */
static inline void wait_for_interrupt(void)
{
    __asm__ volatile("sti\n\thlt\n\tcli" : : : "memory");
}

/* Stop for good. An NMI still wakes HLT, hence the loop. */
static inline void halt_forever(void)
{
    disable_interrupts();
    for (;;)
        __asm__ volatile("hlt");
}

#endif
