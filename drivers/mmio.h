/* Device registers and DMA memory as a user-mode driver reaches them.

   Each register access is one MOV of the register's width, written in
   assembly: VIRTIO 1.4 4.1.3.1 requires accesses of a field's own width,
   and TinyCC drops a volatile read whose value is unused, while some
   reads matter for their effect alone. A device section is mapped
   strong uncacheable, whose reads and writes reach the bus in program
   order and are never speculative (SDM Vol. 3A 11.2.5).

   Memory the device reads and writes by DMA is ordinary write-back RAM,
   where the processor-ordering model of SDM Vol. 3A 11.2.2 applies,
   which Intel does not promise future processors keep (11.2.5). So the
   driver orders its DMA memory accesses with fences rather than relying
   on it: SFENCE serializes every store before it ahead of the stores
   after it, LFENCE every load before it ahead of the loads after it, and
   MFENCE every load and store before it ahead of the loads and stores
   after it (11.2.5, and the ordering rules of 11.2.2). The "memory"
   clobber keeps the compiler from moving accesses across any of them. */

#ifndef NOVA_MMIO_H
#define NOVA_MMIO_H

#include "native_abi.h"

static inline UINT8 mmio_read8(const volatile void *address)
{
    UINT8 value;

    __asm__ volatile("movb (%1), %%al" : "=a"(value) : "r"(address)
                     : "memory");
    return value;
}

static inline UINT16 mmio_read16(const volatile void *address)
{
    UINT16 value;

    __asm__ volatile("movw (%1), %%ax" : "=a"(value) : "r"(address)
                     : "memory");
    return value;
}

static inline UINT32 mmio_read32(const volatile void *address)
{
    UINT32 value;

    __asm__ volatile("movl (%1), %%eax" : "=a"(value) : "r"(address)
                     : "memory");
    return value;
}

static inline void mmio_write8(volatile void *address, UINT8 value)
{
    __asm__ volatile("movb %%al, (%1)" : : "a"(value), "r"(address)
                     : "memory");
}

static inline void mmio_write16(volatile void *address, UINT16 value)
{
    __asm__ volatile("movw %%ax, (%1)" : : "a"(value), "r"(address)
                     : "memory");
}

static inline void mmio_write32(volatile void *address, UINT32 value)
{
    __asm__ volatile("movl %%eax, (%1)" : : "a"(value), "r"(address)
                     : "memory");
}

/* A 64-bit register as two 32-bit halves, low first (VIRTIO 1.4
   4.1.3). */
static inline void mmio_write64(volatile void *address, UINT64 value)
{
    mmio_write32(address, (UINT32)value);
    mmio_write32((volatile UINT8 *)address + 4, (UINT32)(value >> 32));
}

static inline void store_fence(void)
{
    __asm__ volatile("sfence" : : : "memory");
}

static inline void load_fence(void)
{
    __asm__ volatile("lfence" : : : "memory");
}

static inline void memory_fence(void)
{
    __asm__ volatile("mfence" : : : "memory");
}

#endif
