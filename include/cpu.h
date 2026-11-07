/* The x86-64 instructions that nova-boot's and the kernel's C code use and
   C cannot express. */

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

static inline void wrmsr(UINT32 msr, UINT64 value)
{
    __asm__ volatile("wrmsr"
                     :
                     : "c"(msr), "a"((UINT32)value), "d"((UINT32)(value >> 32))
                     : "memory");
}

/* The registers CPUID returns for a leaf, with subleaf 0. The outputs go
   to locals first: given the pointers as operands, TinyCC stores through
   RSI, which the Microsoft x64 convention has the callee preserve, and
   does not save it. */
static inline void cpuid(UINT32 leaf, UINT32 *a, UINT32 *b, UINT32 *c,
                         UINT32 *d)
{
    UINT32 eax, ebx, ecx, edx;

    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(leaf), "c"(0));
    *a = eax;
    *b = ebx;
    *c = ecx;
    *d = edx;
}

static inline UINT64 read_cr0(void)
{
    UINT64 value;

    __asm__ volatile("mov %%cr0, %0" : "=r"(value));
    return value;
}

static inline void write_cr0(UINT64 value)
{
    __asm__ volatile("mov %0, %%cr0" : : "r"(value) : "memory");
}

/* The linear address a page fault was raised for. */
static inline UINT64 read_cr2(void)
{
    UINT64 value;

    __asm__ volatile("mov %%cr2, %0" : "=r"(value));
    return value;
}

static inline UINT64 read_cr3(void)
{
    UINT64 value;

    __asm__ volatile("mov %%cr3, %0" : "=r"(value));
    return value;
}

static inline void write_cr3(UINT64 value)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(value) : "memory");
}

static inline UINT64 read_cr4(void)
{
    UINT64 value;

    __asm__ volatile("mov %%cr4, %0" : "=r"(value));
    return value;
}

static inline void write_cr4(UINT64 value)
{
    __asm__ volatile("mov %0, %%cr4" : : "r"(value) : "memory");
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
