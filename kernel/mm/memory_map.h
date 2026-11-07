/* The UEFI memory map nova-boot hands over (docs/spec/boot-protocol.md,
   BOOT_INFO and Memory), read where nova-boot left it. It lies in loader
   data, which the page allocator never hands out, so it stays valid for
   the life of the system. */

#ifndef NOVA_MEMORY_MAP_H
#define NOVA_MEMORY_MAP_H

#include "boot_info.h"
#include "efi.h"

/* A set of memory types as a mask, one bit per EFI_MEMORY_TYPE value. No
   set holds a type of MEMORY_TYPE_LIMIT or above, among them the values
   UEFI leaves to OEMs and OS loaders (UEFI 2.11, 7.2.1). */
#define MEMORY_TYPE_LIMIT 32
#define MEMORY_TYPE(type) (1u << (type))

UINT64 memory_map_count(const BOOT_INFO *info);

const EFI_MEMORY_DESCRIPTOR *memory_map_at(const BOOT_INFO *info,
                                           UINT64 index);

/* A descriptor the kernel can trust: page-aligned, not empty, and within
   the address space UEFI allows. */
int memory_map_well_formed(const EFI_MEMORY_DESCRIPTOR *d);

/* Keep info's map for the queries below. */
void memory_map_init(const BOOT_INFO *info);

/* Whether a well-formed descriptor of a type outside types overlaps the
   size bytes at base. */
int memory_map_overlaps_other(UINT64 base, UINT64 size, UINT32 types);

/* Whether every byte of the size bytes at base lies in well-formed
   descriptors of types in types. */
int memory_map_covers(UINT64 base, UINT64 size, UINT32 types);

#endif
