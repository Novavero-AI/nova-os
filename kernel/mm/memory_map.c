/* The memory map. Descriptors are descriptor_size apart, which may exceed
   the structure (UEFI 2.11, 7.2.3). */

#include "memory_map.h"
#include "memory_layout.h"

/* One past the last page a descriptor may reach: no page of one starts
   above 0xfffffffffffff000 (UEFI 2.11, 7.2.3). */
#define MAP_END_PAGE 0x10000000000000ULL

static const BOOT_INFO *boot_info;

UINT64 memory_map_count(const BOOT_INFO *info)
{
    if (info->descriptor_size < sizeof(EFI_MEMORY_DESCRIPTOR))
        return 0;
    return info->memory_map_size / info->descriptor_size;
}

const EFI_MEMORY_DESCRIPTOR *memory_map_at(const BOOT_INFO *info,
                                           UINT64 index)
{
    return (const EFI_MEMORY_DESCRIPTOR *)(info->memory_map +
                                           index * info->descriptor_size);
}

/* A 64-bit start always lies below MAP_END_PAGE, so only the length needs
   bounding. */
int memory_map_well_formed(const EFI_MEMORY_DESCRIPTOR *d)
{
    return d->PhysicalStart % PAGE_SIZE == 0 && d->NumberOfPages != 0 &&
           d->NumberOfPages <= MAP_END_PAGE - d->PhysicalStart / PAGE_SIZE;
}

void memory_map_init(const BOOT_INFO *info)
{
    boot_info = info;
}

static int in_set(UINT32 type, UINT32 types)
{
    return type < MEMORY_TYPE_LIMIT && (types & MEMORY_TYPE(type));
}

/* The descriptor's end, which well_formed keeps from wrapping. */
static UINT64 end_of(const EFI_MEMORY_DESCRIPTOR *d)
{
    return d->PhysicalStart + d->NumberOfPages * PAGE_SIZE;
}

/* The callers check that base + size does not wrap. */
int memory_map_overlaps_other(UINT64 base, UINT64 size, UINT32 types)
{
    UINT64 i;

    for (i = 0; i < memory_map_count(boot_info); i++) {
        const EFI_MEMORY_DESCRIPTOR *d = memory_map_at(boot_info, i);

        if (memory_map_well_formed(d) && !in_set(d->Type, types) &&
            d->PhysicalStart < base + size && base < end_of(d))
            return 1;
    }
    return 0;
}

/* Each step finds a descriptor that holds the next uncovered byte and
   moves past its end, so it ends within one pass per descriptor. */
int memory_map_covers(UINT64 base, UINT64 size, UINT32 types)
{
    UINT64 next = base, end = base + size;
    UINT64 i, steps;

    for (steps = 0; next < end && steps < memory_map_count(boot_info);
         steps++) {
        for (i = 0; i < memory_map_count(boot_info); i++) {
            const EFI_MEMORY_DESCRIPTOR *d = memory_map_at(boot_info, i);

            if (memory_map_well_formed(d) && in_set(d->Type, types) &&
                d->PhysicalStart <= next && next < end_of(d)) {
                next = end_of(d);
                break;
            }
        }
        if (i == memory_map_count(boot_info))
            return 0;
    }
    return next >= end;
}
