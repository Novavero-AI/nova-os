# Boot protocol (x86-64)

nova-boot (`\EFI\BOOT\BOOTX64.EFI`) is the UEFI application that loads
the kernel (`\nova\kernel.exe`), builds its page tables, exits the
firmware's boot services and calls the kernel's entry with a `BOOT_INFO`
record. This document is the contract between the two as
`boot/nova-boot.c`, `include/boot_info.h`, `include/memory_layout.h` and
`kernel/main.c` implement it. Specifications are cited by the versions in
[README.md](README.md): "UEFI 7.4.6" is section 7.4.6 of UEFI 2.11, "SDM"
the Intel manual's Volume 3, "EDK II" the tag pinned there, "E-EDID" the
VESA standard listed there.

## Sequence

nova-boot, in order:

1. Disables the watchdog timer the boot manager arms for five minutes
   before starting it (UEFI 3.1.2, 7.5.1).
2. Takes the first handle `LocateHandleBuffer` returns for the Graphics
   Output Protocol (UEFI 7.3.15) and chooses among its modes whose pixel
   format is `PixelRedGreenBlueReserved8BitPerColor` (0) or
   `PixelBlueGreenRedReserved8BitPerColor` (1) (UEFI 12.9.2):
   - the display's native mode, one the size of the preferred timing in
     the EDID on that handle, from `EFI_EDID_ACTIVE_PROTOCOL` or, where
     the handle has none, `EFI_EDID_DISCOVERED_PROTOCOL` (UEFI 12.9.2.4,
     12.9.2.5). The timing is the first 18-byte descriptor of the EDID's
     128-byte base block, read only when `SizeOfEdid` covers the block,
     its header and checksum are valid and Feature Support bit 1 is set,
     which in EDID 1.4 marks the timing as the display's native format
     (E-EDID 3.3, 3.10.1, 3.11, Table 3.14);
   - failing that, the mode the firmware started in;
   - failing that, the mode with the most pixels.

   It calls `SetMode` only to change the mode, then draws the boot
   screen. It fails if no mode it can draw to is in use.
3. Reads and places the kernel image ([Kernel image](#kernel-image)).
4. Allocates the kernel's stack and builds the kernel's page tables
   ([Paging](#paging)).
5. Measures the timestamp counter against `Stall(50000)`.
6. Asks up to four `EFI_RNG_PROTOCOL` instances for a seed each
   ([Random seeds](#random-seeds)).
7. Takes the memory map into a buffer it allocates with room for four
   more descriptors, growing the buffer if the map outgrows it, and calls
   `ExitBootServices` with the map's key. A refused key means the map
   changed after it was taken; nova-boot retakes it and tries again, three
   calls in all (UEFI 7.4.6).
8. Clears the interrupt flag, completes `BOOT_INFO`, sets IA32_EFER.NXE
   if the processor has NX and sets CR0.WP. Then `boot_switch`
   (`boot/switch.S`) loads CR3 with the kernel's tables, moves RSP to the
   kernel's stack and calls the kernel's entry.

Until it first calls `ExitBootServices`, nova-boot meets a failure by
freeing what it allocated (UEFI 7.2), the kernel's tables and stack
included, and returning an error `EFI_STATUS` to the firmware. After that
call only the memory allocation services remain (UEFI 7.4.6), so a
failure halts the processor.

## Kernel image

### Location

The kernel is the file `\nova\kernel.exe` on the volume nova-boot was
loaded from: nova-boot opens the `DeviceHandle` of its own
`EFI_LOADED_IMAGE_PROTOCOL` (UEFI 9.1.1) through
`EFI_SIMPLE_FILE_SYSTEM_PROTOCOL` (UEFI 13.4.1) and opens the path from
the volume's root (UEFI 13.5.2). FAT matches names without regard to case,
so the on-disk name may be `\NOVA\KERNEL.EXE`;
[disk-layout.md](disk-layout.md) gives the names the image tool writes.

### Format

A PE32+ image (PE Format):

- `MZ` at offset 0, the offset of the PE signature at 0x3C, the signature
  `PE\0\0`;
- COFF `Machine` `IMAGE_FILE_MACHINE_AMD64` (0x8664);
- `IMAGE_FILE_RELOCS_STRIPPED` clear, since nova-boot places the image
  wherever it gets pages;
- optional-header `Magic` 0x20B (PE32+);
- a nonzero `AddressOfEntryPoint`;
- a base relocation table (data directory 5) holding only
  `IMAGE_REL_BASED_DIR64` (10) and `IMAGE_REL_BASED_ABSOLUTE` (0) entries,
  or no base relocation table when the image contains no absolute
  addresses;
- no imports: nova-boot resolves none;
- `SectionAlignment` of 4096 or less, since nova-boot places the image at a
  4 KiB boundary;
- `SizeOfImage` of at most 2 GiB, the space from `KERNEL_IMAGE_BASE` to
  the top of the address space;
- a section table within `SizeOfHeaders`, which PE Format defines to
  cover it;
- no 4 KiB page of the image that both a section with
  `IMAGE_SCN_MEM_WRITE` and one with `IMAGE_SCN_MEM_EXECUTE` reach (PE
  Format, Section Flags).

nova-boot rejects an image whose headers, section table, sections,
relocation blocks or relocated locations reach outside the file or
`SizeOfImage`. An image whose `NumberOfRvaAndSizes` gives no slot for the
base relocation directory has no relocations; one whose
`SizeOfOptionalHeader` does not cover a slot `NumberOfRvaAndSizes` claims
is rejected (PE Format, Optional Header).

The README's build links the kernel with `-subsystem=native`
(`IMAGE_SUBSYSTEM_NATIVE`, 1), `-e=kernel_main`, and section and file
alignments of 4096 and 512 bytes. nova-boot does not read `Subsystem`.

### Placement

nova-boot allocates `SizeOfImage` rounded up to 4 KiB pages anywhere in
memory as `EfiLoaderCode` (UEFI 7.2.1), zeroes them, copies the first
`SizeOfHeaders` bytes of the file, then copies each section's
`min(SizeOfRawData, VirtualSize)` bytes from `PointerToRawData` to
`VirtualAddress`. It adds the difference between `KERNEL_IMAGE_BASE` and
`ImageBase` to every `DIR64` location, so the kernel runs at
`KERNEL_IMAGE_BASE` wherever its pages are, and the absolute addresses it
holds stay valid once the lower half is unmapped. The kernel's tables map
the image page by page with the rights of its sections
([Paging](#paging)).

## Entry

```c
VOID kernel_main(BOOT_INFO *info);
```

nova-boot calls `KERNEL_IMAGE_BASE` plus the image's
`AddressOfEntryPoint` with the Microsoft x64 calling convention (UEFI
2.3.4.2; x64 calling convention): `info` in RCX, a direct-map address;
RSP 16-byte aligned at the call, with the caller's 32-byte home area
above the return address, so RSP is `KERNEL_STACK_TOP` - 40 at the first
instruction; RBP 0. The return address is in `boot_switch`, whose page
the kernel unmaps with the rest of the lower half. The kernel does not
return.

## BOOT_INFO

`include/boot_info.h`; offsets as TinyCC and clang lay the record out for
x86-64 PE.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 0 | `magic` | `UINT64` | `BOOT_INFO_MAGIC`, 0x544F4F4241564F4E: "NOVABOOT" in memory order |
| 8 | `version` | `UINT64` | `BOOT_INFO_VERSION`, 2 for the record and entry state this document describes |
| 16 | `screen.pixels` | `UINT32 *` | The direct-map address of `FrameBufferBase` of the graphics mode in use (UEFI 12.9.2) |
| 24 | `screen.width` | `INT64` | `HorizontalResolution` |
| 32 | `screen.height` | `INT64` | `VerticalResolution` |
| 40 | `screen.stride` | `INT64` | `PixelsPerScanLine`; pixel (x, y) is `pixels[y * stride + x]` |
| 48 | `screen.format` | `UINT32` | `EFI_GRAPHICS_PIXEL_FORMAT`: 0 (byte 0 red) or 1 (byte 0 blue) |
| 56 | `spinner` | `SPINNER` | The boot screen spinner's centre, size and head angle (`gfx/gfx.h`), from which the kernel finds the part of the screen the boot screen covers |
| 96 | `tsc_per_ms` | `UINT64` | Timestamp-counter ticks per millisecond |
| 104 | `memory_map` | `UINT8 *` | The direct-map address of the memory map `ExitBootServices` accepted |
| 112 | `memory_map_size` | `UINTN` | Size of the map in bytes |
| 120 | `descriptor_size` | `UINTN` | Distance between descriptors in bytes |
| 128 | `random_seed_count` | `UINT64` | Entries of `random_seeds` filled in, at most `BOOT_RANDOM_SEEDS` (4) |
| 136 + 40i | `random_seeds[i].status` | `UINT64` | The `EFI_STATUS` of asking instance i for a seed; the seed is valid only if it is 0 (`EFI_SUCCESS`) |
| 144 + 40i | `random_seeds[i].bytes` | `UINT8[32]` | The seed |

The record is 296 bytes and lives in nova-boot's `.data` section; `info`
is its direct-map address. The kernel reads `magic` and `version` before
any other field and halts, logging both, on a value it was not built
for. Another layout, or another machine state at entry, takes another
version.

`tsc_per_ms` is the counter's advance across `Stall(50000)` divided by 50.
`Stall` waits at least the time asked (UEFI 7.5.2), so the value can
overstate the counter's rate but understates it only by the rounding to
whole ticks. The rate is constant only on a processor with an invariant
TSC (SDM 20.17.1, CPUID.80000007H:EDX[8]; AMD APM Volume 2,
`8000_0007h_EDX[TscInvariant]`); nova-boot does not check for one.

The memory map is an array of `EFI_MEMORY_DESCRIPTOR` (UEFI 7.2.3) of
`memory_map_size / descriptor_size` entries. The kernel steps through it by
`descriptor_size`, never by `sizeof(EFI_MEMORY_DESCRIPTOR)`: EDK II reports
48 bytes for the 40-byte structure (`MdeModulePkg/Core/Dxe/Mem/Page.c`,
`CoreGetMemoryMap`). Addresses in it are physical.

### Random seeds

nova-boot finds every handle with `EFI_RNG_PROTOCOL` with
`LocateHandleBuffer(ByProtocol)` (UEFI 7.3.15) and asks the first four,
in the order returned, for 32 bytes from the default algorithm,
`GetRNG` with a null `RNGAlgorithm` (UEFI 37.5.3). It asks each
instance it finds, not only the one `LocateProtocol` returns, since
that one can be the processor's RDRAND, which the kernel reads itself. A failure leaves
its status in the entry; no seed, or no instance, does not stop the
boot. nova-boot does not judge the seeds. The kernel hashes the valid
ones with its own sources, then zeroes every entry.

QEMU 11.1.3 builds its `edk2-x86_64-code.fd` from
`OvmfPkg/OvmfPkgX64.dsc` (`roms/edk2-build.config`) at EDK II commit
`4dfdca63a93497203f197ec98ba20e2327e4afe4`, which its `roms/edk2` pins.
That build has two kinds of instance, from
`OvmfPkg/Include/Dsc/OvmfRngComponents.dsc.inc`; the sources below are
the same at the tag pinned in [README.md](README.md), apart from
comments and a TPL raise in `VirtioRngGetRNG`:

| Driver | Present when | Algorithms, default first |
|---|---|---|
| `SecurityPkg/RandomNumberGenerator/RngDxe` | The processor has RDRAND: its entry installs nothing unless `BaseRngLib` reads a value (`Rand/RngDxe.c`, `GetAvailableAlgorithms`) | `EFI_RNG_ALGORITHM_SP800_90_CTR_256_GUID`, RDRAND's output; `EFI_RNG_ALGORITHM_RAW` |
| `OvmfPkg/VirtioRngDxe` | A virtio-rng device is present, one instance per device | `EFI_RNG_ALGORITHM_RAW` |

Booted in QEMU with `-machine q35`, the observed instances were: none with
QEMU's default processor, which has neither RDRAND nor RDSEED; one,
VirtioRngDxe's, with `-device virtio-rng-pci`; one, RngDxe's, with
`-cpu max`; two with both, RngDxe's first, which is also the one
`LocateProtocol` returns. Each virtio-rng device added one more. So with
QEMU's default processor the firmware offers a seed only with
`-device virtio-rng-pci`.

The drivers leave copies of what they return in boot-services memory:
`VirtioRngGetRNG` reads into a pool buffer and frees it without
clearing it, and RngDxe's `RngGetBytes` passes its values through a
stack array. The kernel's page allocator zeroes every free page when it
starts, before the kernel reads the seeds.

## Machine state at entry

UEFI defines the processor state during boot services (UEFI 2.3.4) and what
`ExitBootServices` changes (UEFI 7.4.6). Between `ExitBootServices`
returning and the call nova-boot changes the interrupt flag,
IA32_EFER.NXE, CR0.WP, CR3, RSP and RBP, so the kernel starts in the
boot-services state with the changes below.

### Processor mode

Long mode, 64-bit mode, on the bootstrap processor only (UEFI 2.3.4).

### Paging

Paging is on with nova-boot's tables in CR3. They have the depth the
firmware ran with, 4-level or 5-level (CR4.LA57, bit 12, SDM 2.5), since
CR4.LA57 cannot change while paging is on in IA-32e mode (SDM 5.1.2);
nova-boot builds either. They map, with the constants of
`include/memory_layout.h`:

| Linear addresses | Map | Rights |
|---|---|---|
| The lower half | `boot_switch`'s pages, at their physical addresses, and nothing else | Read, execute |
| `DIRECT_MAP_BASE` (0xFFFF800000000000) + p, for p below 64 TiB | Physical address p, for every range the memory map describes and the framebuffer (`FrameBufferBase` and `FrameBufferSize`, UEFI 12.9.2) | Read, write |
| `KERNEL_STACK_BASE` to `KERNEL_STACK_TOP` (0xFFFFFFFF7FFEF000 to 0xFFFFFFFF7FFFF000) | The kernel's stack ([Stack](#stack)) | Read, write |
| `KERNEL_IMAGE_BASE` (0xFFFFFFFF80000000) + RVA, below `SizeOfImage` | The kernel image | A page is executable if a section with `IMAGE_SCN_MEM_EXECUTE` reaches it, writable if one with `IMAGE_SCN_MEM_WRITE` does, and otherwise read-only, as the headers are |

Nothing else is mapped: not the page below `KERNEL_STACK_BASE`, which is
the stack's guard, nor the page at `KERNEL_STACK_TOP`.

- **Execute disable.** Where CPUID.80000001H:EDX[20] reports NX (SDM
  5.1.4), nova-boot sets IA32_EFER.NXE (SDM 2.2.1) and the XD bit in
  every entry that maps a page not marked executable above. Without NX,
  XD is reserved (SDM 5.5.4) and every page is executable.
- **Write protection.** CR0.WP is set, so read-only pages bind the
  kernel too (SDM 5.1.3).
- **Entries.** An entry that points at a table grants write and execute
  and leaves the restrictions to the entry that maps the page, since a
  translation's rights are the most restrictive of all the entries it
  uses (SDM 5.6.1). No entry is user-accessible or global.
- **Page sizes.** Below 4 MiB the direct map uses 4 KiB pages: the
  fixed-range MTRRs give the first MiB memory types 4 KiB at a time (SDM
  14.11.2.2), and SDM 14.11.9 advises large pages only from 4 MiB up.
  From there it uses 1 GiB pages where CPUID.80000001H:EDX[26] reports
  them (SDM 5.1.4) and 2 MiB pages, each where a range's alignment and
  length allow. The kernel image, the stack and `boot_switch`'s pages are
  4 KiB pages.
- **Memory types.** No entry sets PAT, PCD or PWT, so a page gets the
  memory type that PAT entry 0 and the MTRRs select together (SDM 5.9.2,
  Table 14-7), provided the MTRRs give a large page one type throughout;
  a large page over several types is undefined (SDM 14.11.9), and
  nova-boot does not read the MTRRs to rule it out. EDK II's identity
  map, of 2 MiB and 1 GiB pages, sets none of the three bits either
  (`MdeModulePkg/Core/DxeIplPeim/X64/VirtualMemory.c`) and sets memory
  types through the MTRRs (`UefiCpuPkg/CpuDxe/CpuDxe.c`,
  `CpuSetMemoryAttributes`).
- **The direct map's source.** nova-boot takes the memory map it maps
  before the final one. UEFI has the map describe all memory present,
  however it is used (UEFI 7.2.3), so the allocations after it change
  descriptor types, not the memory described. EDK II lists memory it has
  not tested as `EfiReservedMemoryType` and makes it conventional memory
  only when an allocation needs it (`MdeModulePkg/Core/Dxe/Mem/Page.c`,
  `CoreGetMemoryMap` and `PromoteMemoryResource`). A descriptor UEFI
  7.2.3 does not allow, unaligned or empty, is skipped, as the kernel's
  page allocator skips it; a range reaching past 64 TiB fails the boot
  with `EFI_UNSUPPORTED`.
- **The tables' memory.** Every table is an `EfiLoaderData` page
  nova-boot allocates before `ExitBootServices`.
- **Cached translations.** The TLBs may still hold translations from the
  firmware's tables, global ones among them if the firmware set CR4.PGE.
  The kernel clears the top table's lower-half entries, then invalidates
  every translation: by changing CR4.PGE if it is set, and otherwise by
  loading CR3 (SDM 5.10.4.1).

### Segments and descriptor tables

Segment selectors are flat (UEFI 2.3.4). The GDT and IDT are the
firmware's, at addresses the kernel's tables do not map, so until the
kernel loads its own, delivering an exception or NMI faults again and
ends in a shutdown (SDM 7.15, Event 8). The kernel loads its GDT and IDT
before it does more than check `BOOT_INFO` and write to the serial port.
On EDK II the GDT is in `EfiRuntimeServicesData` below 4 GiB
(`UefiCpuPkg/CpuDxe/CpuGdt.c`) and the IDT in firmware memory, in a
boot-services pool once `CpuDxe` extends it to 256 vectors
(`UefiCpuPkg/CpuDxe/CpuDxe.c`).

### Interrupts

UEFI enables interrupts during boot services (UEFI 2.3.4) and stops the
firmware timer in `ExitBootServices` (UEFI 7.4.6); it does not state the
interrupt flag afterwards. EDK II clears it at the end of a successful
`ExitBootServices` (`CoreExitBootServices`). nova-boot clears it before it
calls the kernel, so interrupts are disabled at entry on any firmware.

### Flags and floating point

RFLAGS.DF is clear; other flags are undefined. The x87 control word is
0x037F, MXCSR 0x1F80, CR0.EM and CR0.TS clear (UEFI 2.3.4). The kernel
uses no floating point.

### Stack

The kernel runs on a 64 KiB stack that nova-boot allocates as
`EfiLoaderData` and maps from `KERNEL_STACK_BASE` to `KERNEL_STACK_TOP`.
The pages below and above it are unmapped, so an overflow faults on the
guard page below instead of writing past the stack. The firmware's
stack, which nova-boot ran on, is mapped only in the direct map.

### Other processors

The application processors stay where the firmware left them. At
`ExitBootServices` EDK II moves them into a loop whose code it places in
`EfiReservedMemoryType` pages (`UefiCpuPkg/Library/MpInitLib/DxeMpLib.c`,
`AllocateApLoopCodeBuffer` and `MpInitChangeApLoopCallback`). Without AMD
SEV-ES they run it on a page table of its own, also in
`EfiReservedMemoryType` (`MpLib.c`, `PrepareApLoopCode`;
`X64/CreatePageTable.c`), which `AsmRelocateApLoopGeneric`
(`X64/MpFuncs.nasm`) loads into CR3, so they translate through none of
the firmware's boot-services tables.

### Firmware services

Boot services are gone. Of the system table only `Hdr`, `FirmwareVendor`,
`FirmwareRevision`, `RuntimeServices`, `NumberOfTableEntries` and
`ConfigurationTable` stay valid (UEFI 4.3), and nova-boot passes none of
them. The watchdog is disabled (UEFI 7.5.1).

### Serial port

`kernel_main` writes to COM1 (I/O port 0x3F8) as the firmware left it
configured; nova-boot does not set it up. A Hyper-V generation 2 VM has a
UART only when one is configured.

## Memory

After `ExitBootServices` the kernel owns all memory the map describes,
except what the types below reserve (UEFI 7.2, Table 7.10 and its note):

| Type | Value | After ExitBootServices |
|---|---|---|
| `EfiConventionalMemory` | 7 | Free |
| `EfiBootServicesCode`, `EfiBootServicesData` | 3, 4 | Free, once the kernel has moved off them (below) |
| `EfiLoaderCode`, `EfiLoaderData` | 1, 2 | The kernel's to use; they hold the kernel image, nova-boot and `BOOT_INFO`, the memory map, and the kernel's page tables and stack |
| `EfiPersistentMemory` | 14 | Usable like conventional memory, contents non-volatile |
| `EfiACPIReclaimMemory` | 9 | Preserve until the ACPI tables are read |
| `EfiRuntimeServicesCode`, `EfiRuntimeServicesData` | 5, 6 | Preserve |
| `EfiACPIMemoryNVS` | 10 | Preserve |
| `EfiPalCode` | 13 | Preserve |
| `EfiReservedMemoryType`, `EfiUnusableMemory` | 0, 8 | Do not use |
| `EfiMemoryMappedIO`, `EfiMemoryMappedIOPortSpace` | 11, 12 | Not used by the OS; device addresses come from ACPI |
| `EfiUnacceptedMemoryType` | 15 | Accept before use |

nova-boot's allocations are `EfiLoaderCode` (the kernel image) and
`EfiLoaderData` (the memory map and the kernel's page tables and stack).
`BOOT_INFO` is in nova-boot's own image,
which UEFI types `EfiLoaderCode` for code and `EfiLoaderData` for data
(UEFI 2.1.1, Table 2.1); EDK II allocates an application's whole image as
`EfiLoaderCode` (`MdeModulePkg/Core/Dxe/Image/Image.c`, `CoreLoadPeImage`).

The firmware's stack, page tables and IDT live in memory UEFI does not
name; on EDK II they are `EfiBootServicesData`, and its page-table pools
too (`DxeIpl` and `UefiCpuPkg/CpuDxe/CpuPageTable.c`). The kernel runs on
nova-boot's stack and tables from its first instruction, and it loads
its own GDT and IDT before it allocates from `EfiBootServicesCode` or
`EfiBootServicesData`, so none of that memory is in use by then. The
kernel reaches physical memory through the direct map. It copies what it
needs from `BOOT_INFO` and the map before it reuses `EfiLoaderCode` or
`EfiLoaderData`.

## Undecided

This version leaves open:

- **ACPI.** ACPI 6.6 5.2.5.2 has the OS loader take the RSDP pointer from
  the configuration table (`EFI_ACPI_20_TABLE_GUID`, UEFI 4.6) before
  `ExitBootServices` and hand it to the OS. `BOOT_INFO` carries no RSDP
  pointer, and no SMBIOS pointer either.
- **Runtime services.** Whether the kernel uses UEFI runtime services, and
  so whether `BOOT_INFO` carries `RuntimeServices` and whether the loader
  or kernel calls `SetVirtualAddressMap`.
- **Marking nova-os memory.** UEFI reserves memory types 0x80000000 to
  0xFFFFFFFF for OS loaders (UEFI 7.2.1). Typing the kernel image,
  `BOOT_INFO` and the map with them would let the kernel find them in the
  map.
- **Multiple processors.** How the kernel brings up the application
  processors (the MADT, ACPI 6.6 5.2.12, and INIT and startup IPIs), and
  what it must preserve until then.
- **The framebuffer.** `FrameBufferSize` is not passed; `PixelBitMask`
  and `PixelBltOnly` modes fail the boot; only the first Graphics Output
  instance is considered. The native mode comes only from the EDID base
  block. A display whose native timing does not fit a base-block
  descriptor (4095 x 4095 pixels at up to 655.35 MHz, E-EDID Table 3.21)
  gives it in an extension block, which nova-boot does not read; it
  keeps the firmware's mode unless the base block still marks its first
  descriptor native, in which case nova-boot switches to that smaller
  mode where it is offered. A Graphics Output handle that drives several
  displays as one carries no EDID (UEFI 12.10) and keeps the firmware's
  mode.
- **Timekeeping.** Whether nova-boot checks for an invariant TSC and what
  the kernel falls back to without one.
- **The serial console.** Which port the kernel uses and who programs it;
  ACPI's SPCR and DBG2 tables describe debug ports.
- **Load options and further files.** A command line, or files beyond the
  kernel.
- **ARM64.** The AArch64 entry state (UEFI 2.3.6), the image
  `\EFI\BOOT\BOOTAA64.EFI` and `Machine` 0xAA64.
- **Measured and Secure Boot.** What nova-boot measures and how.
- **Memory encryption.** No entry of nova-boot's tables sets the C-bit
  that marks a page encrypted (AMD APM Volume 2, 7.10.4), so the tables
  do not suit a machine or guest whose memory AMD SME or SEV encrypts.
