/* PCI configuration space as the platform driver and PCI device drivers
   read it, and the bootstrap message the platform driver sends a device
   driver.

   The PCI Local Bus and PCI Express Base specifications define the
   configuration header and capabilities; PCI-SIG's documents are not
   pinned (docs/spec/README.md). The offsets and bits below match the
   12th Generation Intel Core datasheet's register descriptions pinned
   there: vendor and device IDs at 0h and 2h; Device Command at 4h, with
   I/O Space Enable in bit 0, Memory Space Enable in bit 1, Bus Master
   Enable in bit 2 and Interrupt Disable in bit 10; the header type at
   Eh; the BARs from 10h; a bridge's secondary bus number at 19h (Bus
   Numbers, 18h, bits 15:8); the capabilities pointer at 34h; and the
   MSI-X table's 16-byte entries, with the vector control's mask in bit
   0. The MSI-X capability's own layout (ID 11h; message control with
   the table size in bits 10:0, function mask in bit 14 and enable in bit
   15; the table offset and BAR indicator) is in none of the pinned
   documents; QEMU's devices were observed to follow it. */

#ifndef NOVA_PCI_H
#define NOVA_PCI_H

#include "native_abi.h"

/* A function's configuration space: 4 KiB, at the ECAM range's base plus
   1 MiB per bus, 32 KiB per device and 4 KiB per function (the
   datasheet's PCIEXBAR). */
#define PCI_CONFIG_SIZE 0x1000
#define PCI_BUS_SHIFT 20
#define PCI_DEVICE_SHIFT 15
#define PCI_FUNCTION_SHIFT 12
#define PCI_DEVICES 32
#define PCI_FUNCTIONS 8

#define PCI_VENDOR_ID 0x00
#define PCI_DEVICE_ID 0x02
#define PCI_COMMAND 0x04
#define PCI_STATUS 0x06
#define PCI_HEADER_TYPE 0x0e
#define PCI_BARS_OFFSET 0x10
#define PCI_SECONDARY_BUS 0x19
#define PCI_CAPABILITIES 0x34

#define PCI_VENDOR_ABSENT 0xffff
#define PCI_COMMAND_IO 0x0001
#define PCI_COMMAND_MEMORY 0x0002
#define PCI_COMMAND_BUS_MASTER 0x0004
#define PCI_COMMAND_INTX_DISABLE 0x0400
#define PCI_STATUS_CAPABILITIES 0x0010
#define PCI_HEADER_MULTIFUNCTION 0x80
#define PCI_HEADER_LAYOUT 0x7f
#define PCI_HEADER_ENDPOINT 0x00
#define PCI_HEADER_BRIDGE 0x01

/* A BAR: bit 0 set for I/O space; for memory, bits 2:1 give its width,
   00b for 32 bits and 10b for 64, and the address is the rest above bit
   3. */
#define PCI_BARS 6
#define PCI_BAR_SIZE 4
#define PCI_BAR_IO 0x1
#define PCI_BAR_WIDTH 0x6
#define PCI_BAR_32 0x0
#define PCI_BAR_64 0x4
#define PCI_BAR_ADDRESS (~0xfULL)

/* A capability: its ID in byte 0 and the next one's offset in byte 1, 0
   at the end of the list. Offsets lie in the first 256 bytes, and the
   low 2 bits of a pointer are reserved. */
#define PCI_CAP_ID 0
#define PCI_CAP_NEXT 1
#define PCI_CAP_POINTER_MASK 0xfc
#define PCI_CAPS_MAX 48
#define PCI_CAP_VENDOR 0x09
#define PCI_CAP_MSIX 0x11

#define MSIX_CONTROL 2
#define MSIX_TABLE 4
#define MSIX_CONTROL_TABLE_SIZE 0x07ff
#define MSIX_CONTROL_MASK_ALL 0x4000
#define MSIX_CONTROL_ENABLE 0x8000
#define MSIX_TABLE_BIR 0x7
#define MSIX_TABLE_OFFSET (~0x7u)
#define MSIX_ENTRY_SIZE 16
#define MSIX_ENTRY_ADDRESS 0
#define MSIX_ENTRY_DATA 8
#define MSIX_ENTRY_CONTROL 12
#define MSIX_ENTRY_MASKED 0x1

/* A function's bus requester ID (ipc.md I8): the segment group in bits
   31:16, then bus, device and function in the low 16 bits as the
   configuration address orders them. */
#define PCI_SEGMENT_SHIFT 16
#define PCI_REQUESTER_BUS_SHIFT 8
#define PCI_REQUESTER_DEVICE_SHIFT 3
#define PCI_REQUESTER(segment, bus, device, function)                    \
    ((UINT32)(segment) << PCI_SEGMENT_SHIFT |                            \
     (UINT32)(bus) << PCI_REQUESTER_BUS_SHIFT |                          \
     (UINT32)(device) << PCI_REQUESTER_DEVICE_SHIFT | (UINT32)(function))

/* Log requester as PCI writes a function's address,
   segment:bus:device.function in hexadecimal (pci.c). */
void pci_say_function(UINT32 requester);

/* The bytes of a PCI device driver's bootstrap message: the function's
   requester ID, the physical address of its configuration space, and
   each BAR's memory range, size 0 where the BAR decodes no memory. */
typedef struct {
    UINT32 requester;
    UINT32 reserved;
    UINT64 config;
    UINT64 bar_base[PCI_BARS];
    UINT64 bar_size[PCI_BARS];
} PCI_DEVICE_BOOTSTRAP;

/* Its handles, in order: the debug log; the driver's own process, with
   MAP; the function's initiator, with PIN; an IRQ resource covering the
   function's message-signalled interrupts; an MMIO resource covering its
   configuration space; then an MMIO resource for each BAR with a
   nonzero size, in BAR order. */
#define PCI_HANDLE_DEBUG_LOG 0
#define PCI_HANDLE_SELF 1
#define PCI_HANDLE_INITIATOR 2
#define PCI_HANDLE_IRQ 3
#define PCI_HANDLE_CONFIG 4
#define PCI_HANDLE_BARS 5
#define PCI_HANDLES_MAX (PCI_HANDLE_BARS + PCI_BARS)

#endif
