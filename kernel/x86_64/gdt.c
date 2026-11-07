/* The kernel's global descriptor table: the null descriptor, 64-bit kernel
   code and kernel data. The firmware's table has its own layout (EDK2's
   CpuDxe puts 64-bit code at selector 0x38); with its own, the kernel
   names the selectors its interrupt gates use. */

#include "x86_64.h"

#define DESCRIPTOR_SIZE 8

/* Segment descriptor fields (SDM Vol. 3A 3.4.5, Figure 3-8, and Table 3-1
   for the type bits). */
#define DESC_LIMIT_FLAT (0xffffULL | 0xfULL << 48) /* limit bits 19:0 set */
#define DESC_READ_WRITE (1ULL << 41) /* code: readable; data: writable */
#define DESC_CODE (1ULL << 43)
#define DESC_CODE_OR_DATA (1ULL << 44) /* S: not a system descriptor */
#define DESC_PRESENT (1ULL << 47)
#define DESC_LONG (1ULL << 53)    /* L: 64-bit code */
#define DESC_DEFAULT_32 (1ULL << 54) /* D/B */
#define DESC_PAGE_GRANULAR (1ULL << 55) /* G: limit in 4 KiB units */

/* 64-bit mode ignores the base and limit (SDM Vol. 3A 3.2.4); both
   descriptors still describe a flat 4 GiB segment, as EDK2's do. */
#define KERNEL_CODE                                                         \
    (DESC_LIMIT_FLAT | DESC_READ_WRITE | DESC_CODE | DESC_CODE_OR_DATA |    \
     DESC_PRESENT | DESC_LONG | DESC_PAGE_GRANULAR)
#define KERNEL_DATA                                                         \
    (DESC_LIMIT_FLAT | DESC_READ_WRITE | DESC_CODE_OR_DATA | DESC_PRESENT | \
     DESC_DEFAULT_32 | DESC_PAGE_GRANULAR)

/* The encodings EDK2's CpuDxe loads for its LINEAR_CODE64_SEL and
   LINEAR_DATA64_SEL. */
_Static_assert(KERNEL_CODE == 0x00af9a000000ffffULL, "kernel code");
_Static_assert(KERNEL_DATA == 0x00cf92000000ffffULL, "kernel data");

/* Entry 0 is the null descriptor. Not const: loading a selector sets its
   descriptor's accessed bit (SDM Vol. 3A 3.4.5.1). */
static UINT64 gdt[] = {
    [KERNEL_CODE_SELECTOR / DESCRIPTOR_SIZE] = KERNEL_CODE,
    [KERNEL_DATA_SELECTOR / DESCRIPTOR_SIZE] = KERNEL_DATA,
};

/* 64-bit mode loads CS only through a far transfer. This one is a far
   return to the next instruction; its REX.W form pops an eight-byte RIP
   and CS, where the plain form pops four bytes of each (SDM Vol. 2B, RET),
   and TinyCC's assembler has no mnemonic for it. */
#define FAR_RETURN_64 ".byte 0x48, 0xcb\n"

static void reload_segments(void)
{
    __asm__ volatile("pushq %0\n\t"
                     "leaq 1f(%%rip), %%rax\n\t"
                     "pushq %%rax\n\t" FAR_RETURN_64 "1:\n\t"
                     "movw %w1, %%ds\n\t"
                     "movw %w1, %%es\n\t"
                     "movw %w1, %%fs\n\t"
                     "movw %w1, %%gs\n\t"
                     "movw %w1, %%ss"
                     :
                     : "i"(KERNEL_CODE_SELECTOR), "r"(KERNEL_DATA_SELECTOR)
                     : "rax", "memory");
}

void gdt_load(void)
{
    DESCRIPTOR_REGISTER gdtr;

    gdtr.limit = sizeof(gdt) - 1;
    gdtr.base = (UINT64)gdt;
    __asm__ volatile("lgdt %0" : : "m"(gdtr) : "memory");
    reload_segments();
}
