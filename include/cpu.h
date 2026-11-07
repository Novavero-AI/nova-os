/** @file
    The x86-64 instructions that nova-boot's and the kernel's C code use and
    C cannot express. */

#ifndef NOVA_CPU_H
#define NOVA_CPU_H

#include "types.h"

/** Write value to the I/O port port (OUT). */
static inline void outb(UINT16 port, UINT8 value)
{
    __asm__ volatile("outb %0, %1" : : "a"(value), "d"(port));
}

/** Read a byte from the I/O port port (IN). */
static inline UINT8 inb(UINT16 port)
{
    UINT8 value;

    __asm__ volatile("inb %1, %0" : "=a"(value) : "d"(port));
    return value;
}

/** The timestamp counter (RDTSC). */
static inline UINT64 rdtsc(void)
{
    UINT32 lo, hi;

    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (UINT64)hi << 32 | lo;
}

/** The value of the model-specific register msr (RDMSR). */
static inline UINT64 rdmsr(UINT32 msr)
{
    UINT32 lo, hi;

    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (UINT64)hi << 32 | lo;
}

/** Write value to the model-specific register msr (WRMSR). */
static inline void wrmsr(UINT32 msr, UINT64 value)
{
    __asm__ volatile("wrmsr"
                     :
                     : "c"(msr), "a"((UINT32)value), "d"((UINT32)(value >> 32))
                     : "memory");
}

/** The registers CPUID returns for a leaf, with subleaf 0. The outputs go
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

/** The same for a subleaf other than 0. */
static inline void cpuid_subleaf(UINT32 leaf, UINT32 subleaf, UINT32 *a,
                                 UINT32 *b, UINT32 *c, UINT32 *d)
{
    UINT32 eax, ebx, ecx, edx;

    __asm__ volatile("cpuid"
                     : "=a"(eax), "=b"(ebx), "=c"(ecx), "=d"(edx)
                     : "a"(leaf), "c"(subleaf));
    *a = eax;
    *b = ebx;
    *c = ecx;
    *d = edx;
}

/** @name Bounds-check bypass
    Bounds-check bypass (docs/spec/speculation.md M4): a branch that checks
    a caller's value can be mispredicted, and the loads after it run with
    the value it refused (Intel HFB 3.2.1). After the check these compute
    its outcome again as data, with no branch to mispredict, so a load
    that uses their result stays in bounds. Each is one asm statement:
    TinyCC emits it as written, and its operands are the volatile
    registers RAX, RCX and RDX and the scratch R8 and R9, which a callee
    need not preserve.
    @{ */

/** index if it is below size, otherwise 0. CMP sets CF exactly when index
    is below size, SBB of a register from itself turns CF into all ones or
    zero, and AND applies that mask: bounds clipping, for which Intel lists
    SBB (Intel HFB 3.2.4.1). */
static inline UINT64 index_masked(UINT64 index, UINT64 size)
{
    UINT64 masked;

    __asm__ volatile("cmpq %2, %1\n\t"
                     "sbbq %0, %0\n\t"
                     "andq %1, %0"
                     : "=&a"(masked)
                     : "c"(index), "d"(size)
                     : "cc");
    return masked;
}

/** address if the size bytes from it lie in [base, top], ending at top at
    the latest, otherwise 0: each of the three conditions clears the result
    with a CMOV, "sufficient to mitigate bounds check bypass
    vulnerabilities even in cases where bounds clipping is not applicable"
    (Intel HFB 3.2.4.2; AMD STMS V1-2). When address is above top, top -
    address wraps, but the second CMOV has cleared the result already. */
static inline UINT64 range_selected(UINT64 address, UINT64 size, UINT64 base,
                                    UINT64 top)
{
    UINT64 selected;

    __asm__ volatile("xorl %%r8d, %%r8d\n\t"
                     "movq %1, %0\n\t"
                     "cmpq %3, %1\n\t"
                     "cmovbq %%r8, %0\n\t"
                     "cmpq %4, %1\n\t"
                     "cmovaq %%r8, %0\n\t"
                     "movq %4, %%r9\n\t"
                     "subq %1, %%r9\n\t"
                     "cmpq %%r9, %2\n\t"
                     "cmovaq %%r8, %0"
                     : "=&a"(selected)
                     : "c"(address), "d"(size), "m"(base), "m"(top)
                     : "r8", "r9", "cc");
    return selected;
}
/** @} */

/** @name Control registers
    @{ */

/** CR0's value. */
static inline UINT64 read_cr0(void)
{
    UINT64 value;

    __asm__ volatile("mov %%cr0, %0" : "=r"(value));
    return value;
}

/** Write value to CR0. */
static inline void write_cr0(UINT64 value)
{
    __asm__ volatile("mov %0, %%cr0" : : "r"(value) : "memory");
}

/** The linear address a page fault was raised for. */
static inline UINT64 read_cr2(void)
{
    UINT64 value;

    __asm__ volatile("mov %%cr2, %0" : "=r"(value));
    return value;
}

/** CR3's value, which holds the top paging table's address. */
static inline UINT64 read_cr3(void)
{
    UINT64 value;

    __asm__ volatile("mov %%cr3, %0" : "=r"(value));
    return value;
}

/** Write value to CR3. */
static inline void write_cr3(UINT64 value)
{
    __asm__ volatile("mov %0, %%cr3" : : "r"(value) : "memory");
}

/** CR4's value. */
static inline UINT64 read_cr4(void)
{
    UINT64 value;

    __asm__ volatile("mov %%cr4, %0" : "=r"(value));
    return value;
}

/** Write value to CR4. */
static inline void write_cr4(UINT64 value)
{
    __asm__ volatile("mov %0, %%cr4" : : "r"(value) : "memory");
}
/** @} */

/** Write back every modified cache line and invalidate the caches (SDM
    Vol.\ 2D, WBINVD). */
static inline void write_back_and_invalidate_caches(void)
{
    __asm__ volatile("wbinvd" : : : "memory");
}

/** Raise a breakpoint exception (INT3). */
static inline void breakpoint(void)
{
    __asm__ volatile("int3");
}

/** Let maskable interrupts in (STI). */
static inline void enable_interrupts(void)
{
    __asm__ volatile("sti" : : : "memory");
}

/** Keep maskable interrupts out (CLI). */
static inline void disable_interrupts(void)
{
    __asm__ volatile("cli" : : : "memory");
}

/** RFLAGS.IF, which lets maskable interrupts in (SDM Vol.\ 3A 2.3). */
#define RFLAGS_INTERRUPT_ENABLE (1ULL << 9)

/** Disable interrupts and return the RFLAGS they were disabled from, for
    restore_interrupts. With one processor, a section between the two runs
    without being interrupted or preempted. */
static inline UINT64 save_and_disable_interrupts(void)
{
    UINT64 flags;

    __asm__ volatile("pushfq\n\tpopq %0\n\tcli" : "=r"(flags) : : "memory");
    return flags;
}

/** Enable interrupts again if they were enabled in flags, the RFLAGS
    save_and_disable_interrupts returned. */
static inline void restore_interrupts(UINT64 flags)
{
    if (flags & RFLAGS_INTERRUPT_ENABLE)
        enable_interrupts();
}

/** Halt until an interrupt has been handled; called and returning with
    interrupts disabled. Interrupts stay inhibited on the instruction
    boundary after STI (SDM Vol. 2B, STI), so one that became pending after
    the caller's last check wakes the HLT instead of being taken before it. */
static inline void wait_for_interrupt(void)
{
    __asm__ volatile("sti\n\thlt\n\tcli" : : : "memory");
}

/** Stop for good. An NMI still wakes HLT, hence the loop. */
static inline _Noreturn void halt_forever(void)
{
    disable_interrupts();
    for (;;)
        __asm__ volatile("hlt");
}

#endif
