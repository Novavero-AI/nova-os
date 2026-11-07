/* The ACPI tables the kernel reads at boot (ACPI 6.6 5.2), from the RSDP
   nova-boot found (BOOT_INFO.acpi_rsdp) through the XSDT: the MCFG, for
   the PCI Express configuration space it passes to the first process, and
   the MADT, DMAR and IVRS, for the registers of the interrupt controllers
   and IOMMUs that no section may cover (docs/spec/ipc.md I6). */

#ifndef NOVA_ACPI_H
#define NOVA_ACPI_H

#include "boot_info.h"
#include "../status.h"

/* Read the tables and log what they hold. Needs memory_map_init. A
   missing or malformed table is logged and left out; if it may have
   named registers the kernel keeps, every range counts as kept. */
void acpi_init(const BOOT_INFO *info);

UINT32 acpi_ecam_count(void);

/* The index-th configuration space range, index below acpi_ecam_count. */
const ECAM_RANGE *acpi_ecam(UINT32 index);

/* Whether the size bytes at base lie inside one configuration space
   range. */
int acpi_ecam_contains(UINT64 base, UINT64 size);

/* Whether the size bytes at base overlap the registers of an interrupt
   controller or IOMMU the tables name; always, if the tables could not
   all be read or recorded. */
int acpi_kernel_registers(UINT64 base, UINT64 size);

#endif
