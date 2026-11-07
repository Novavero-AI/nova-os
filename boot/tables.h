/* The page tables nova-boot builds for the kernel before it exits boot
   services, from pages it allocates as EfiLoaderData. The firmware's
   tables are loaded while these are built and identity-map every page
   nova-boot allocates (UEFI 2.11, 2.3.4), so a table's physical address
   is also where nova-boot writes it. */

#ifndef NOVA_BOOT_TABLES_H
#define NOVA_BOOT_TABLES_H

#include "efi.h"

typedef struct {
    EFI_BOOT_SERVICES *boot;
    UINT64 top;        /* the top table's physical address, for CR3 */
    int levels;        /* 4 or 5, as the firmware runs */
    int largest_level; /* the highest level whose entries map pages */
    UINT64 no_execute; /* PTE_NO_EXECUTE if the processor has NX, else 0 */
} PAGE_TABLES;

/* Allocate an empty top table for the paging depth in use and note the
   page sizes and the execute-disable bit the processor supports. */
EFI_STATUS tables_create(EFI_BOOT_SERVICES *boot, PAGE_TABLES *tables);

/* Map size bytes at linear to physical, each a multiple of 4 KiB, with
   the entry bits in flags (PTE_WRITABLE, PTE_NO_EXECUTE), in the largest
   pages their alignment and size allow, up to those an entry at
   largest_level maps and the processor supports. PTE_NO_EXECUTE is left
   out where the processor has no NX. Fails if a table cannot be allocated
   or the range overlaps one already mapped. */
EFI_STATUS tables_map(PAGE_TABLES *tables, UINT64 linear, UINT64 physical,
                      UINT64 size, UINT64 flags, int largest_level);

/* Free every table. */
void tables_free(PAGE_TABLES *tables);

#endif
