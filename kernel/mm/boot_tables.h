/* The page tables the firmware left loaded in CR3, which the kernel runs
   on. UEFI 2.11, 2.3.4, has them identity-map all
   memory the memory map describes, so a physical address is also the
   address they translate.

   They sit in memory the map hands to the OS. EDK2 takes them from a
   pool of boot-services data, maps the whole pool read-only in 2 MiB
   units and sets CR0.WP (AllocatePageTableMemory, AllocateAlignedPages
   and EnablePageTableProtection, edk2-stable202608), so a kernel write
   to any page of the pool faults, used for a table or not. */

#ifndef NOVA_BOOT_TABLES_H
#define NOVA_BOOT_TABLES_H

#include "types.h"

/* Whether a kernel write to address succeeds through the tables. */
int boot_tables_writable(UINT64 address);

/* Call visit with the address of each page that holds part of the
   tables. */
void boot_tables_each_page(void (*visit)(UINT64 page));

#endif
