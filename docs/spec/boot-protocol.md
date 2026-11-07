# Boot protocol (x86-64)

nova-boot (`\EFI\BOOT\BOOTX64.EFI`) is the UEFI application that loads
the kernel (`\nova\kernel.exe`), exits the firmware's boot services and
calls the kernel's entry with a `BOOT_INFO` record. This document is the
contract between the two as `boot/nova-boot.c`, `include/boot_info.h` and
`kernel/main.c` implement it. Specifications are cited by the versions in
[README.md](README.md): "UEFI 7.4.6" is section 7.4.6 of UEFI 2.11, "SDM"
the Intel manual's Volume 3, "EDK II" the tag pinned there.

## Sequence

nova-boot, in order:

1. Disables the watchdog timer the boot manager arms for five minutes
   before starting it (UEFI 3.1.2, 7.5.1).
2. Locates the first Graphics Output Protocol instance (UEFI 7.3.16). Of
   its modes no larger than 1920 x 1080 whose pixel format is
   `PixelRedGreenBlueReserved8BitPerColor` (0) or
   `PixelBlueGreenRedReserved8BitPerColor` (1) (UEFI 12.9.2), it switches
   to the one with the most pixels if that is more than the current mode
   has, then draws the boot screen. It fails if the mode in use has
   another format.
3. Reads and places the kernel image ([Kernel image](#kernel-image)).
4. Measures the timestamp counter against `Stall(50000)`.
5. Allocates a buffer for the memory map, takes the map and calls
   `ExitBootServices` with its key, retaking the map and retrying up to
   three times in all (UEFI 7.4.6).
6. Calls the kernel's entry. If the kernel returns, nova-boot halts.

On a failure nova-boot returns an error `EFI_STATUS` to the firmware.

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
- optional-header `Magic` 0x20B (PE32+);
- a base relocation table (data directory 5) holding only
  `IMAGE_REL_BASED_DIR64` (10) and `IMAGE_REL_BASED_ABSOLUTE` (0) entries,
  or no base relocation table when the image contains no absolute
  addresses;
- no imports: nova-boot resolves none;
- `SectionAlignment` of 4096 or less, since nova-boot places the image at a
  4 KiB boundary.

The README's build links the kernel with `-subsystem=native`
(`IMAGE_SUBSYSTEM_NATIVE`, 1) and `-e=kernel_main`. nova-boot does not read
`Subsystem`.

### Placement

nova-boot allocates `SizeOfImage` rounded up to 4 KiB pages anywhere in
memory as `EfiLoaderCode` (UEFI 7.2.1), zeroes them, copies the first
`SizeOfHeaders` bytes of the file, then copies each section's
`min(SizeOfRawData, VirtualSize)` bytes from `PointerToRawData` to
`VirtualAddress`. It adds the difference between the load address and
`ImageBase` to every `DIR64` location. Physical and virtual addresses are
equal ([Paging](#paging)), so the kernel runs at its physical load
address. nova-boot applies no per-section page protection.

## Entry

```c
VOID kernel_main(BOOT_INFO *info);
```

nova-boot calls the image's `AddressOfEntryPoint` with the Microsoft x64
calling convention (UEFI 2.3.4.2; x64 calling convention): `info` in RCX,
RSP 16-byte aligned at the call with the caller's 32-byte home area above
the return address. The kernel does not return.

## BOOT_INFO

`include/boot_info.h`; offsets as TinyCC and clang lay the record out for
x86-64 PE.

| Offset | Field | Type | Meaning |
|---|---|---|---|
| 0 | `screen.pixels` | `UINT32 *` | `FrameBufferBase` of the graphics mode in use (UEFI 12.9.2) |
| 8 | `screen.width` | `INT64` | `HorizontalResolution` |
| 16 | `screen.height` | `INT64` | `VerticalResolution` |
| 24 | `screen.stride` | `INT64` | `PixelsPerScanLine`; pixel (x, y) is `pixels[y * stride + x]` |
| 32 | `screen.format` | `UINT32` | `EFI_GRAPHICS_PIXEL_FORMAT`: 0 (byte 0 red) or 1 (byte 0 blue) |
| 40 | `spinner` | `SPINNER` | The boot screen spinner's centre, size and head angle (`gfx/gfx.h`), for the kernel to keep it turning |
| 80 | `tsc_per_ms` | `UINT64` | Timestamp-counter ticks per millisecond |
| 88 | `memory_map` | `UINT8 *` | The memory map `ExitBootServices` accepted |
| 96 | `memory_map_size` | `UINTN` | Size of the map in bytes |
| 104 | `descriptor_size` | `UINTN` | Distance between descriptors in bytes |

The record is 112 bytes and lives in nova-boot's `.data` section.

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

## Machine state at entry

UEFI defines the processor state during boot services (UEFI 2.3.4) and what
`ExitBootServices` changes (UEFI 7.4.6). nova-boot changes nothing between
`ExitBootServices` returning and the call, so the kernel starts in the
boot-services state with the changes below.

### Processor mode

Long mode, 64-bit mode, on the bootstrap processor only (UEFI 2.3.4).

### Paging

Paging is on, with the firmware's page tables in CR3. Every range in the
memory map is identity-mapped; some ranges may lack read, write or execute
permission; mappings outside the map are undefined (UEFI 2.3.4). The map
need not describe the framebuffer, so UEFI does not promise it a mapping.
nova-boot draws to it before `ExitBootServices` through the same tables,
and EDK II's `CoreExitBootServices`
(`MdeModulePkg/Core/Dxe/DxeMain/DxeMain.c`) does not load CR3.

The firmware chooses 4-level or 5-level paging (CR4.LA57, bit 12, SDM 2.5).
CR4.LA57 cannot change while paging is on in IA-32e mode (SDM 5.1.2), so
the kernel's own page tables use the mode it finds.

### Segments and descriptor tables

Segment selectors are flat (UEFI 2.3.4). The GDT and IDT are the
firmware's. On EDK II the GDT is in `EfiRuntimeServicesData` below 4 GiB
(`UefiCpuPkg/CpuDxe/CpuGdt.c`) and the IDT in firmware memory, in a
boot-services pool once `CpuDxe` extends it to 256 vectors
(`UefiCpuPkg/CpuDxe/CpuDxe.c`).

### Interrupts

UEFI enables interrupts during boot services (UEFI 2.3.4) and stops the
firmware timer in `ExitBootServices` (UEFI 7.4.6); it does not state the
interrupt flag afterwards. EDK II clears it at the end of a successful
`ExitBootServices` (`CoreExitBootServices`). `kernel_main` executes `cli`
first.

### Flags and floating point

RFLAGS.DF is clear; other flags are undefined. The x87 control word is
0x037F, MXCSR 0x1F80, CR0.EM and CR0.TS clear (UEFI 2.3.4). The kernel
uses no floating point.

### Stack

The kernel runs on the stack the firmware started nova-boot on: at least
128 KiB at nova-boot's entry (UEFI 2.3.4), less nova-boot's frames. UEFI
does not say what memory holds it. On EDK II it is the DXE stack, which
`DxeIpl` allocates as `EfiBootServicesData`
(`MdeModulePkg/Core/DxeIplPeim/X64/DxeLoadFunc.c`); `StartImage` calls an
image on the caller's stack.

### Other processors

The application processors stay where the firmware left them. At
`ExitBootServices` EDK II moves them into a loop whose code it places in
`EfiReservedMemoryType` pages (`UefiCpuPkg/Library/MpInitLib/DxeMpLib.c`,
`AllocateApLoopCodeBuffer` and `MpInitChangeApLoopCallback`).

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
| `EfiLoaderCode`, `EfiLoaderData` | 1, 2 | The kernel's to use; they hold the kernel image, nova-boot and `BOOT_INFO`, and the memory map |
| `EfiPersistentMemory` | 14 | Usable like conventional memory, contents non-volatile |
| `EfiACPIReclaimMemory` | 9 | Preserve until the ACPI tables are read |
| `EfiRuntimeServicesCode`, `EfiRuntimeServicesData` | 5, 6 | Preserve |
| `EfiACPIMemoryNVS` | 10 | Preserve |
| `EfiPalCode` | 13 | Preserve |
| `EfiReservedMemoryType`, `EfiUnusableMemory` | 0, 8 | Do not use |
| `EfiMemoryMappedIO`, `EfiMemoryMappedIOPortSpace` | 11, 12 | Not used by the OS; device addresses come from ACPI |
| `EfiUnacceptedMemoryType` | 15 | Accept before use |

nova-boot's allocations are `EfiLoaderCode` (the kernel image) and
`EfiLoaderData` (the memory map). `BOOT_INFO` is in nova-boot's own image,
which UEFI types `EfiLoaderCode` for code and `EfiLoaderData` for data
(UEFI 2.1.1, Table 2.1); EDK II allocates an application's whole image as
`EfiLoaderCode` (`MdeModulePkg/Core/Dxe/Image/Image.c`, `CoreLoadPeImage`).

The firmware's stack, page tables and IDT live in memory UEFI does not
name; on EDK II they are `EfiBootServicesData`, and its page-table pools
too (`DxeIpl` and `UefiCpuPkg/CpuDxe/CpuPageTable.c`). The kernel switches
to its own stack, GDT, IDT and page tables before it allocates from
`EfiBootServicesCode` or `EfiBootServicesData`, and copies what it needs
from `BOOT_INFO` and the map before it reuses `EfiLoaderCode` or
`EfiLoaderData`.

## Where main departs from the specifications

Each entry is behaviour in main that a specification contradicts, with
the change that brings it in line.

1. **The map's descriptor size before the first map.** `efi_main` sizes the
   map buffer from the `descriptor_size` its first `GetMemoryMap` call,
   made with an empty buffer, leaves behind, and does not check that
   call's status. That call fails with `EFI_BUFFER_TOO_SMALL`, and UEFI
   7.2.3 does not say `DescriptorSize` is returned on that error (EDK II
   returns it). Initialise it to `sizeof(EFI_MEMORY_DESCRIPTOR)` and
   require `EFI_BUFFER_TOO_SMALL` from the first call.
2. **The descriptor version.** The kernel reads version 1 descriptors
   without checking. Fail the boot unless `DescriptorVersion` is
   `EFI_MEMORY_DESCRIPTOR_VERSION` (1, UEFI 7.2.3).
3. **Returning after `ExitBootServices` was called.** On a failed map or
   after three refused attempts, `efi_main` returns to the firmware. After
   the first call a loader may use only the memory allocation services
   (UEFI 7.4.6), and EDK II has already stopped its timer by then. Once
   `ExitBootServices` has been called, retry with a freshly sized buffer
   and halt on failure instead of returning.
4. **Resources on failure.** UEFI 7.2 requires an image to free what it
   allocated before returning to the firmware. `place_kernel` returns
   without freeing the kernel's pages when it rejects a relocation, and
   `efi_main` returns without freeing them when the map buffer cannot be
   allocated. Free the kernel's pages on every failure before
   `ExitBootServices`.
5. **Probing the data directories.** `place_kernel` reads data directory 5
   without checking `SizeOfOptionalHeader` or `NumberOfRvaAndSizes`; the
   PE Format says the first must, and the second should, bound every
   data-directory probe. It also copies sections and applies relocations
   without checking them against the file size and `SizeOfImage`. Check
   `NumberOfRvaAndSizes > 5`, that the optional header and section table
   lie inside the file, and that every section, relocation block and
   relocated location lies inside the file or the image.
6. **Images without relocations.** An image with
   `IMAGE_FILE_RELOCS_STRIPPED` must load at its `ImageBase` (PE Format,
   Characteristics). `place_kernel` places every image anywhere. Reject an
   image with that flag set.
7. **Section alignment.** `place_kernel` does not read `SectionAlignment`.
   Reject an image whose `SectionAlignment` exceeds 4096.
8. **The kernel's PE header.** For `-subsystem=native` TinyCC lays sections
   out on 32-byte boundaries but writes `SectionAlignment` 0x1000 and
   `FileAlignment` 0x200 into the header (`tccpe.c`, lines 633-634 and
   2046-2048 at the pinned commit). The resulting `kernel.exe` places
   `.text` at RVA 0x20, inside its 0x240 bytes of headers. Its section
   addresses, its `SizeOfImage` (0x1B20) and two of its raw data offsets
   (0x240, 0x1A80) are not multiples of the declared alignments, as the PE
   Format requires them to be. Linking with
   `-Wl,-section-alignment=1000 -Wl,-file-alignment=200` (hexadecimal)
   gives an image whose layout matches its header.
9. **Interrupts at entry.** The interrupt flag at entry depends on the
   firmware (see [Interrupts](#interrupts)). Execute `cli` in nova-boot
   after `ExitBootServices` succeeds, so that interrupts are disabled at
   entry on any firmware.
10. **Comments on memory.** `include/boot_info.h` says the memory map marks
    nova-boot's image as loader data; EDK II marks it loader code. State
    both, or place `BOOT_INFO` in an `EfiLoaderData` allocation.
    `kernel/main.c` says the kernel runs on the loader's stack; it runs on
    the firmware's ([Stack](#stack)). It also says UEFI's note on memory
    after `ExitBootServices` lists boot-services code and data and
    conventional memory; the note lists `EfiLoaderCode` and
    `EfiLoaderData` as well.
11. **Opening the kernel.** `read_kernel` ignores the status of
    `SetPosition` and `GetPosition`; if either fails, for example because
    the path names a directory (UEFI 13.5.11, 13.5.12), the size it reads
    is uninitialised. Check both statuses, or take the size and the
    directory attribute from `GetInfo` with `EFI_FILE_INFO` (UEFI 13.5.16).

## Undecided

This version leaves open:

- **A version field.** `BOOT_INFO` carries no magic, version or size, so a
  kernel cannot tell which loader built it.
- **ACPI.** ACPI 6.6 5.2.5.2 has the OS loader take the RSDP pointer from
  the configuration table (`EFI_ACPI_20_TABLE_GUID`, UEFI 4.6) before
  `ExitBootServices` and hand it to the OS. `BOOT_INFO` carries no RSDP
  pointer, and no SMBIOS pointer either.
- **Runtime services.** Whether the kernel uses UEFI runtime services, and
  so whether `BOOT_INFO` carries `RuntimeServices` and whether the loader
  or kernel calls `SetVirtualAddressMap`.
- **The kernel's address space.** Whether the kernel stays identity-mapped
  at its load address or moves to a fixed higher-half address, and whether
  nova-boot or the kernel builds the first page tables.
- **The kernel stack.** Whether nova-boot allocates the kernel's first
  stack as `EfiLoaderData` and switches to it before the call, so that the
  kernel never runs on boot-services memory.
- **Marking nova-os memory.** UEFI reserves memory types 0x80000000 to
  0xFFFFFFFF for OS loaders (UEFI 7.2.1). Typing the kernel image,
  `BOOT_INFO` and the map with them would let the kernel find them in the
  map.
- **Multiple processors.** How the kernel brings up the application
  processors (the MADT, ACPI 6.6 5.2.12, and INIT and startup IPIs), and
  what it must preserve until then.
- **The framebuffer.** `FrameBufferSize` is not passed; `PixelBitMask`
  and `PixelBltOnly` modes fail the boot; only the first Graphics Output
  instance is considered.
- **Timekeeping.** Whether nova-boot checks for an invariant TSC and what
  the kernel falls back to without one.
- **The serial console.** Which port the kernel uses and who programs it;
  ACPI's SPCR and DBG2 tables describe debug ports.
- **Load options and further files.** A command line, or files beyond the
  kernel.
- **ARM64.** The AArch64 entry state (UEFI 2.3.6), the image
  `\EFI\BOOT\BOOTAA64.EFI` and `Machine` 0xAA64.
- **Measured and Secure Boot.** What nova-boot measures and how.
