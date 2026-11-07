/* The kernel's virtual address space on x86-64, which nova-boot builds and
   the kernel inherits (docs/spec/boot-protocol.md, Paging). The kernel
   lives in the upper half; once it runs, the lower half maps nothing and
   is left for user processes. Every address here has bits 63:47 set, so
   it is canonical and in the upper half with 4-level and 5-level paging
   alike (SDM Vol. 3A 5.1.1). */

#ifndef NOVA_MEMORY_LAYOUT_H
#define NOVA_MEMORY_LAYOUT_H

#include "native_abi.h"
#include "types.h"

/* PAGE_SIZE (native_abi.h) is the x86-64 base page and the unit of the
   UEFI memory map (UEFI 2.11, 7.2.3). */

/* Physical address p is mapped at DIRECT_MAP_BASE + p, for every range
   the UEFI memory map describes other than memory-mapped I/O, and the
   framebuffer, read-write and not executable. The window is the lower 64
   TiB of 4-level paging's upper half; nova-boot refuses memory above
   it. */
#define DIRECT_MAP_BASE 0xFFFF800000000000ULL
#define DIRECT_MAP_SIZE 0x0000400000000000ULL

/* The kernel image, mapped section by section from its first byte, in the
   top 2 GiB of the address space. */
#define KERNEL_IMAGE_BASE 0xFFFFFFFF80000000ULL
#define KERNEL_IMAGE_SPACE 0x80000000ULL

_Static_assert(KERNEL_IMAGE_BASE + KERNEL_IMAGE_SPACE == 0,
               "the image space ends at the top of the address space");
_Static_assert(DIRECT_MAP_BASE + DIRECT_MAP_SIZE <= KERNEL_IMAGE_BASE,
               "the direct map lies below the kernel image");

/* The kernel's first stack, which nova-boot allocates and the kernel
   starts on. The page at KERNEL_STACK_TOP and the page below
   KERNEL_STACK_BASE are left unmapped, so running off either end faults;
   the one below is the guard an overflow reaches. */
#define KERNEL_STACK_SIZE (16 * PAGE_SIZE)
#define KERNEL_STACK_TOP (KERNEL_IMAGE_BASE - PAGE_SIZE)
#define KERNEL_STACK_BASE (KERNEL_STACK_TOP - KERNEL_STACK_SIZE)

/* Where the direct map puts a physical address. */
static inline VOID *direct_map(UINT64 physical)
{
    return (VOID *)(DIRECT_MAP_BASE + physical);
}

#endif
