/* The ACPI tables. Each table is read through the direct map only after
   its header and then its whole length are found to lie in memory the
   memory map gives firmware: EfiACPIReclaimMemory and EfiACPIMemoryNVS,
   where firmware keeps the tables, and EfiReservedMemoryType and
   EfiRuntimeServicesData, which some firmware uses. The direct map maps
   every range the map describes but memory-mapped I/O, and the page
   allocator hands out none of these (docs/spec/boot-protocol.md,
   Memory). Every table's checksum
   must hold (ACPI 6.6 5.2.6), and every field read lies inside the
   table's checked length. */

#include "acpi.h"
#include "memory_layout.h"
#include "../mm/memory_map.h"
#include "../x86_64/paging.h"
#include "../x86_64/serial.h"

#define FIRMWARE_TABLE_TYPES                                              \
    (MEMORY_TYPE(EFI_ACPI_RECLAIM_MEMORY) |                               \
     MEMORY_TYPE(EFI_ACPI_MEMORY_NVS) |                                   \
     MEMORY_TYPE(EFI_RESERVED_MEMORY_TYPE) |                              \
     MEMORY_TYPE(EFI_RUNTIME_SERVICES_DATA))

/* The types the configuration space may share with the map: firmware
   reserves the range there, so that no OS takes it for RAM, or lists it
   as memory-mapped I/O. */
#define ECAM_TYPES                                                        \
    (MEMORY_TYPE(EFI_RESERVED_MEMORY_TYPE) |                              \
     MEMORY_TYPE(EFI_MEMORY_MAPPED_IO) |                                  \
     MEMORY_TYPE(EFI_MEMORY_MAPPED_IO_PORT_SPACE))

/* The RSDP (ACPI 6.6 5.2.5.3, Table 5.3): an ACPI 1.0 one is the first
   20 bytes, covered by its checksum; revision 2 and later add the length,
   the XSDT's address and an extended checksum over the length. */
#define RSDP_SIGNATURE "RSD PTR "
#define RSDP_SIGNATURE_SIZE 8
#define RSDP_V1_SIZE 20
#define RSDP_REVISION 15
#define RSDP_LENGTH 20
#define RSDP_XSDT 24
#define RSDP_V2_SIZE 36
#define RSDP_XSDT_REVISION 2

/* The description header (ACPI 6.6 5.2.6, Table 5.4). */
#define HEADER_SIZE 36
#define HEADER_LENGTH 4
#define SIGNATURE_SIZE 4

/* The XSDT's entries, 64-bit addresses after the header (5.2.8). */
#define XSDT_ENTRY_SIZE 8

/* The MCFG: the header, 8 reserved bytes, then 16-byte allocation
   structures, each a base address, segment group, start and end bus.
   ACPI 6.6 Table 5.6 only reserves the signature and defers the layout
   to the PCI Firmware Specification, which is not pinned
   (docs/spec/README.md, PCI Express configuration space). The base
   address is that of bus 0's configuration space, and bus b's starts
   1 MiB per bus above it, as the 12th Generation Intel Core datasheet's
   PCIEXBAR gives the address of a function's space. */
#define MCFG_ALLOCATIONS 44
#define MCFG_ALLOCATION_SIZE 16
#define MCFG_BASE 0
#define MCFG_SEGMENT 8
#define MCFG_START_BUS 10
#define MCFG_END_BUS 11
#define ECAM_BUS_SHIFT 20

/* The MADT (5.2.12, Table 5.19): the local APIC's 32-bit address, then
   interrupt controller structures, each with its type and length in its
   first two bytes. An I/O APIC structure (Table 5.24) gives its 32-bit
   address; a local APIC address override (Table 5.29) a 64-bit one. */
#define MADT_LOCAL_APIC 36
#define MADT_STRUCTURES 44
#define MADT_IO_APIC 1
#define MADT_IO_APIC_ADDRESS 4
#define MADT_LOCAL_APIC_OVERRIDE 5
#define MADT_OVERRIDE_ADDRESS 4
#define MADT_ENTRY_HEADER 2

/* The DMAR (VT-d 8.1): remapping structures from byte 48, each with a
   16-bit type and length. A DRHD (type 0, VT-d 8.3) holds its register
   set's size in the low 4 bits of byte 5, as a power of two of 4 KiB
   pages, and its base at byte 8. */
#define DMAR_STRUCTURES 48
#define DMAR_ENTRY_HEADER 4
#define DMAR_DRHD 0
#define DRHD_SIZE 5
#define DRHD_SIZE_MASK 0xf
#define DRHD_BASE 8
#define DRHD_MIN_LENGTH 16

/* The IVRS (AMD IOMMU 5.2.2): IVHD and IVMD blocks from byte 48, each
   with an 8-bit type and a 16-bit length at byte 2. An IVHD block of
   type 10h, 11h or 40h gives the IOMMU's register base at byte 8
   (5.2.2.1). The register area is 16 KiB, or 512 KiB where the IOMMU has
   event counters (AMD IOMMU 3.2, Capability Offset 04h), so the kernel
   keeps 512 KiB. */
#define IVRS_BLOCKS 48
#define IVRS_ENTRY_HEADER 4
#define IVRS_LENGTH 2
#define IVHD_10 0x10
#define IVHD_11 0x11
#define IVHD_40 0x40
#define IVHD_BASE 8
#define IVHD_MIN_LENGTH 16
#define AMD_IOMMU_REGISTERS 0x80000

/* The register ranges kept from sections. More than fit, or tables the
   kernel could not read, make every range count as kept: a register the
   kernel does not know of is never left open (ipc.md I6). */
#define KEPT_RANGES_MAX 64

typedef struct {
    UINT64 base;
    UINT64 size;
} RANGE;

static ECAM_RANGE ecam[BOOTSTRAP_ECAM_RANGES_MAX];
static UINT32 ecam_count;
static RANGE kept[KEPT_RANGES_MAX];
static UINT32 kept_count;
static int kept_incomplete;

static UINT32 read16(const UINT8 *p) { return p[0] | p[1] << 8; }

static UINT32 read32(const UINT8 *p)
{
    return (UINT32)p[0] | (UINT32)p[1] << 8 | (UINT32)p[2] << 16 |
           (UINT32)p[3] << 24;
}

static UINT64 read64(const UINT8 *p)
{
    return read32(p) | (UINT64)read32(p + 4) << 32;
}

static int same_bytes(const UINT8 *a, const char *b, UINT32 count)
{
    UINT32 i;

    for (i = 0; i < count; i++)
        if (a[i] != (UINT8)b[i])
            return 0;
    return 1;
}

static int sums_to_zero(const UINT8 *bytes, UINT64 count)
{
    UINT8 sum = 0;
    UINT64 i;

    for (i = 0; i < count; i++)
        sum = (UINT8)(sum + bytes[i]);
    return sum == 0;
}

/* The direct-map address of size bytes at physical, if they lie in
   firmware table memory below the direct map's end; else 0. */
static const UINT8 *firmware_bytes(UINT64 physical, UINT64 size)
{
    if (physical >= DIRECT_MAP_SIZE || size > DIRECT_MAP_SIZE - physical ||
        !memory_map_covers(physical, size, FIRMWARE_TABLE_TYPES))
        return 0;
    return direct_map(physical);
}

/* The table at physical, its length checked to cover its header and to
   lie in firmware table memory, and its checksum to hold; or 0. *length
   receives the length. */
static const UINT8 *table_at(UINT64 physical, UINT32 *length)
{
    const UINT8 *table = firmware_bytes(physical, HEADER_SIZE);

    if (!table)
        return 0;
    *length = read32(table + HEADER_LENGTH);
    if (*length < HEADER_SIZE || !firmware_bytes(physical, *length) ||
        !sums_to_zero(table, *length))
        return 0;
    return table;
}

/* A range that wraps past the top of the address space counts as an
   overflow too. */
static void keep(UINT64 base, UINT64 size)
{
    UINT64 first = base & ~(UINT64)(PAGE_SIZE - 1);
    UINT64 length = size + (base - first);

    if (kept_count == KEPT_RANGES_MAX || length < size ||
        first + length < first) {
        kept_incomplete = 1;
        return;
    }
    kept[kept_count].base = first;
    kept[kept_count].size = length;
    kept_count++;
}

/* ------------------------------------------------------------------------
   The tables. */

/* A range is kept only if it is page aligned, as the direct map and
   sections need, if it overlaps no memory the map gives RAM or
   firmware, other than the reservation that firmware makes for it, and
   once the direct map, which covers that reservation with the type the
   MTRRs give it, no longer maps it: a section over configuration space
   then gives its pages their one memory type (docs/spec/ipc.md I6). */
static void read_mcfg(const UINT8 *table, UINT32 length)
{
    UINT32 at;

    for (at = MCFG_ALLOCATIONS; at + MCFG_ALLOCATION_SIZE <= length;
         at += MCFG_ALLOCATION_SIZE) {
        const UINT8 *entry = table + at;
        UINT64 base = read64(entry + MCFG_BASE);
        UINT32 start = entry[MCFG_START_BUS], end = entry[MCFG_END_BUS];
        UINT64 first, size;

        if (end < start)
            continue;
        first = base + ((UINT64)start << ECAM_BUS_SHIFT);
        size = (UINT64)(end - start + 1) << ECAM_BUS_SHIFT;
        if (first % PAGE_SIZE != 0) {
            serial_write("nova: acpi: mcfg range at ");
            serial_hex(first);
            serial_write(" is not page aligned, left out\r\n");
            continue;
        }
        if (first < base || first + size < first ||
            memory_map_overlaps_other(first, size, ECAM_TYPES)) {
            serial_write("nova: acpi: mcfg range at ");
            serial_hex(first);
            serial_write(" overlaps memory, left out\r\n");
            continue;
        }
        if (ecam_count == BOOTSTRAP_ECAM_RANGES_MAX)
            break;
        if (!paging_unmap_direct_range(first, size)) {
            serial_write("nova: acpi: mcfg range at ");
            serial_hex(first);
            serial_write(" left out: no memory to unmap it from the direct "
                         "map\r\n");
            continue;
        }
        ecam[ecam_count].base = first;
        ecam[ecam_count].segment = (UINT16)read16(entry + MCFG_SEGMENT);
        ecam[ecam_count].start_bus = (UINT8)start;
        ecam[ecam_count].end_bus = (UINT8)end;
        ecam[ecam_count].reserved = 0;
        ecam_count++;
        serial_write("nova: acpi: pci express configuration space, segment ");
        serial_number(read16(entry + MCFG_SEGMENT));
        serial_write(" buses ");
        serial_number(start);
        serial_write(" to ");
        serial_number(end);
        serial_write(", at ");
        serial_hex(first);
        serial_write("\r\n");
    }
}

/* A MADT too short for its fixed fields names no register the kernel
   can keep, so the tables count as incomplete. */
static UINT32 read_madt(const UINT8 *table, UINT32 length)
{
    UINT32 at, io_apics = 0;

    if (length < MADT_STRUCTURES) {
        kept_incomplete = 1;
        return 0;
    }
    keep(read32(table + MADT_LOCAL_APIC), PAGE_SIZE);
    for (at = MADT_STRUCTURES; at + MADT_ENTRY_HEADER <= length;) {
        const UINT8 *entry = table + at;
        UINT32 entry_length = entry[1];

        if (entry_length < MADT_ENTRY_HEADER || entry_length > length - at)
            break;
        if (entry[0] == MADT_IO_APIC &&
            entry_length >= MADT_IO_APIC_ADDRESS + 4) {
            keep(read32(entry + MADT_IO_APIC_ADDRESS), PAGE_SIZE);
            io_apics++;
        } else if (entry[0] == MADT_LOCAL_APIC_OVERRIDE &&
                   entry_length >= MADT_OVERRIDE_ADDRESS + 8) {
            keep(read64(entry + MADT_OVERRIDE_ADDRESS), PAGE_SIZE);
        }
        at += entry_length;
    }
    return io_apics;
}

static UINT32 read_dmar(const UINT8 *table, UINT32 length)
{
    UINT32 at, units = 0;

    for (at = DMAR_STRUCTURES; at + DMAR_ENTRY_HEADER <= length;) {
        const UINT8 *entry = table + at;
        UINT32 entry_length = read16(entry + 2);

        if (entry_length < DMAR_ENTRY_HEADER || entry_length > length - at)
            break;
        if (read16(entry) == DMAR_DRHD && entry_length >= DRHD_MIN_LENGTH) {
            keep(read64(entry + DRHD_BASE),
                 (UINT64)PAGE_SIZE << (entry[DRHD_SIZE] & DRHD_SIZE_MASK));
            units++;
        }
        at += entry_length;
    }
    return units;
}

static UINT32 read_ivrs(const UINT8 *table, UINT32 length)
{
    UINT32 at, units = 0;

    for (at = IVRS_BLOCKS; at + IVRS_ENTRY_HEADER <= length;) {
        const UINT8 *entry = table + at;
        UINT32 entry_length = read16(entry + IVRS_LENGTH);

        if (entry_length < IVRS_ENTRY_HEADER || entry_length > length - at)
            break;
        if ((entry[0] == IVHD_10 || entry[0] == IVHD_11 ||
             entry[0] == IVHD_40) &&
            entry_length >= IVHD_MIN_LENGTH) {
            keep(read64(entry + IVHD_BASE), AMD_IOMMU_REGISTERS);
            units++;
        }
        at += entry_length;
    }
    return units;
}

/* The XSDT's address from a valid RSDP of revision 2 or later, or 0. */
static UINT64 xsdt_address(UINT64 rsdp_physical)
{
    const UINT8 *rsdp = firmware_bytes(rsdp_physical, RSDP_V1_SIZE);
    UINT32 length;

    if (!rsdp || !same_bytes(rsdp, RSDP_SIGNATURE, RSDP_SIGNATURE_SIZE) ||
        !sums_to_zero(rsdp, RSDP_V1_SIZE)) {
        serial_write("nova: acpi: no valid rsdp at ");
        serial_hex(rsdp_physical);
        serial_write(", tables not read\r\n");
        return 0;
    }
    if (rsdp[RSDP_REVISION] < RSDP_XSDT_REVISION) {
        serial_write("nova: acpi: rsdp revision ");
        serial_number(rsdp[RSDP_REVISION]);
        serial_write(" has no xsdt, tables not read\r\n");
        return 0;
    }
    length = read32(rsdp + RSDP_LENGTH);
    if (length < RSDP_V2_SIZE || !firmware_bytes(rsdp_physical, length) ||
        !sums_to_zero(rsdp, length)) {
        serial_write("nova: acpi: rsdp extended checksum fails, tables not "
                     "read\r\n");
        return 0;
    }
    return read64(rsdp + RSDP_XSDT);
}

/* Read every table the XSDT lists that the kernel uses. A listed table
   that fails its checks may be a MADT, DMAR or IVRS, so it leaves the
   tables incomplete; so does the lack of a MADT. */
static void read_tables(const UINT8 *xsdt, UINT32 length)
{
    const UINT8 *table;
    UINT64 address;
    UINT32 table_length, at, madts = 0, io_apics = 0, iommus = 0;

    for (at = HEADER_SIZE; at + XSDT_ENTRY_SIZE <= length;
         at += XSDT_ENTRY_SIZE) {
        address = read64(xsdt + at);
        table = table_at(address, &table_length);
        if (!table) {
            serial_write("nova: acpi: the table at ");
            serial_hex(address);
            serial_write(" lies outside firmware memory or fails its "
                         "checksum\r\n");
            kept_incomplete = 1;
        } else if (same_bytes(table, "MCFG", SIGNATURE_SIZE)) {
            read_mcfg(table, table_length);
        } else if (same_bytes(table, "APIC", SIGNATURE_SIZE)) {
            io_apics += read_madt(table, table_length);
            madts++;
        } else if (same_bytes(table, "DMAR", SIGNATURE_SIZE)) {
            iommus += read_dmar(table, table_length);
        } else if (same_bytes(table, "IVRS", SIGNATURE_SIZE)) {
            iommus += read_ivrs(table, table_length);
        }
    }
    if (!madts)
        kept_incomplete = 1;
    serial_write("nova: acpi: i/o apics ");
    serial_number(io_apics);
    serial_write(", iommus ");
    serial_number(iommus);
    serial_write("; the kernel drives no iommu, so device dma and msi are "
                 "unconfined\r\n");
}

void acpi_init(const BOOT_INFO *info)
{
    const UINT8 *xsdt = 0;
    UINT64 address;
    UINT32 length = 0;

    if (!info->acpi_rsdp) {
        serial_write("nova: acpi: no rsdp from the firmware, tables not "
                     "read\r\n");
    } else if ((address = xsdt_address(info->acpi_rsdp)) != 0) {
        xsdt = table_at(address, &length);
        if (!xsdt || !same_bytes(xsdt, "XSDT", SIGNATURE_SIZE)) {
            serial_write("nova: acpi: no valid xsdt at ");
            serial_hex(address);
            serial_write(", tables not read\r\n");
            xsdt = 0;
        }
    }
    if (!xsdt) {
        kept_incomplete = 1;
        serial_write("nova: acpi: the registers the kernel keeps are "
                     "unknown; no section covers device memory\r\n");
        return;
    }
    serial_write("nova: acpi: rsdp at ");
    serial_hex(info->acpi_rsdp);
    serial_write(", xsdt lists ");
    serial_number((length - HEADER_SIZE) / XSDT_ENTRY_SIZE);
    serial_write(" tables\r\n");
    read_tables(xsdt, length);
    if (kept_incomplete)
        serial_write("nova: acpi: the tables name registers the kernel "
                     "could not record; no section covers device "
                     "memory\r\n");
}

UINT32 acpi_ecam_count(void)
{
    return ecam_count;
}

const ECAM_RANGE *acpi_ecam(UINT32 index)
{
    return &ecam[index];
}

static UINT64 ecam_size(const ECAM_RANGE *range)
{
    return (UINT64)(range->end_bus - range->start_bus + 1) << ECAM_BUS_SHIFT;
}

int acpi_ecam_contains(UINT64 base, UINT64 size)
{
    UINT32 i;

    for (i = 0; i < ecam_count; i++)
        if (base >= ecam[i].base && size <= ecam_size(&ecam[i]) &&
            base - ecam[i].base <= ecam_size(&ecam[i]) - size)
            return 1;
    return 0;
}

int acpi_kernel_registers(UINT64 base, UINT64 size)
{
    UINT32 i;

    if (kept_incomplete)
        return 1;
    for (i = 0; i < kept_count; i++)
        if (kept[i].base < base + size && base < kept[i].base + kept[i].size)
            return 1;
    return 0;
}
