# Specifications

The specifications nova-os is written against. A section number in
nova-os's documents or source refers to the version pinned here; the
sha256 identifies the exact copy the pin was taken from.

## nova-os

| Document | Defines |
|---|---|
| [boot-protocol.md](boot-protocol.md) | The kernel image, `BOOT_INFO` and the machine state nova-boot hands the kernel on x86-64 |
| [disk-layout.md](disk-layout.md) | The boot disk: GPT, the EFI System Partition, its FAT32 file system and file names |
| [objects.md](objects.md) | The kernel's object types, handles and access rights, the namespace, waiting and object lifetime |
| [ipc.md](ipc.md) | Channels and ports, how user-mode drivers reach interrupts, device memory and DMA, and asynchronous I/O requests |
| [syscalls.md](syscalls.md) | The x86-64 system-call entry, its register convention, error codes and the first calls |
| [replay.md](replay.md) | Every nondeterministic input a process can receive, and how a recording captures or excludes it |

## Firmware and platform

**Unified Extensible Firmware Interface (UEFI) Specification**, Release
2.11, November 21, 2024 (UEFI Forum).

- Document: <https://uefi.org/sites/default/files/resources/UEFI_Spec_Final_2.11.pdf>,
  also as HTML at <https://uefi.org/specs/UEFI/2.11/>
- Access: free download.
- sha256: `a64b8e442004b91becc3de9afaf8ca61b259a9a3b436accb6b3711ab5400cee9`
- Used by: nova-boot (`boot/efi.h`, `boot/nova-boot.c`), the boot
  protocol, the disk layout.

**Advanced Configuration and Power Interface (ACPI) Specification**,
Release 6.6, May 13, 2025 (UEFI Forum).

- Document: <https://uefi.org/sites/default/files/resources/ACPI_Spec_6.6.pdf>,
  also as HTML at <https://uefi.org/specs/ACPI/6.6/>
- Access: free download.
- sha256: `8c7542dd4de974ae47bba71bb0336637fe1e3838daad7692370ab4cf218efd35`
- Used by: the boot protocol (finding the RSDP, 5.2.5.2); the kernel's
  hardware discovery (MADT, 5.2.12); interrupt sources
  ([ipc.md](ipc.md), global system interrupts, 5.2.13).

**EDK II**, tag `edk2-stable202608`, commit
`2970e5699ba6267f3384ffab20f96647578aebc8` (TianoCore,
BSD-2-Clause-Patent).

- Source: <https://github.com/tianocore/edk2/tree/edk2-stable202608>
- Access: open source.
- Used by: the boot protocol, for firmware behaviour UEFI leaves to the
  implementation. The firmware QEMU bundles is an EDK II build.

**Should I create a generation 1 or 2 virtual machine in Hyper-V?**
(Microsoft Learn), dated June 18, 2025, updated February 16, 2026.

- Document: <https://learn.microsoft.com/en-us/windows-server/virtualization/hyper-v/plan/should-i-create-a-generation-1-or-2-virtual-machine-in-hyper-v>
- Access: free web page.
- sha256: `9dea282509000aac23281aad2ed2046f01d30b5c910bcdf30919fe5f51ccdd45`
- Used by: the first target. A generation 2 VM boots from a SCSI VHDX or
  an ISO, has UEFI firmware, no PIC, PIT or PCI bus, and an optional UART.

## Processors

**Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 3
(3A, 3B, 3C and 3D): System Programming Guide**, order number
325384-093US, September 2026.

- Document: <https://cdrdv2.intel.com/v1/dl/getContent/671447>
- Access: free download.
- sha256: `77919be320f35da2782771ef2cb65a868cf68e62c8f8279f2ca61199cb70d1f8`
- Used by: nova-boot (paging) and the kernel (paging, GDT, TSS, IDT,
  Local APIC, TSC); the system-call entry ([syscalls.md](syscalls.md)).

**Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 1:
Basic Architecture**, order number 253665-093US, September 2026, the same
release as Volume 3.

- Document: <https://cdrdv2.intel.com/v1/dl/getContent/671436>
- Access: free download.
- sha256: `f6ffc430dfa2dada4f791b1ecb462810e59827befe9a9c23f9160941eea642f9`
- Used by: user-mode drivers' I/O-port access ([ipc.md](ipc.md), I6), for
  the I/O permission bitmap (20.5.2).

**AMD64 Architecture Programmer's Manual, Volume 2: System Programming**,
publication 24593, revision 3.46, October 2026.

- Document: <https://docs.amd.com/api/khub/documents/LNBl5evreogi0r25VIFF2g/content>
- Access: free download.
- sha256: `e4130d5aa26227b6a116d75af9e0f6e5adb4c034ff0d49a3c0fe73c3f49794cf`
- Used by: nova-boot and the kernel on AMD processors, for the same areas
  as the Intel manual; IA32_EFER's address (3.1.7), which the Intel
  volume pinned here does not give; memory encryption (7.10).

## IOMMUs

**Intel Virtualization Technology for Directed I/O Architecture
Specification**, revision 5.20, April 2026, order number D51397-019.

- Document: <https://cdrdv2.intel.com/v1/dl/getContent/671081>
- Access: free download. It states that it grants no licence to Intel's
  intellectual property except to publish an unmodified copy.
- sha256: `44179074934b5d28f49e0302805d69e74bd4e7ad323525ef127d6e99e0c0a2fa`
- Used by: DMA confinement on Intel platforms ([ipc.md](ipc.md), I8): the
  remapping tables (3), invalidation (6.5) and the DMAR table (8); the
  IOMMU registers kept out of `RESOURCE_MMIO` (I6).

**AMD I/O Virtualization Technology (IOMMU) Specification**, publication
48882-PUB, revision 3.11, April 2026.

- Document: <https://docs.amd.com/api/khub/documents/2oKZlc87KNCKiT3B~xJjHg/content>
- Access: free download under the Specification Agreement printed in it,
  which permits review as a reference for designing a product that
  interfaces with an AMD product, and forbids duplicating any part of it
  or giving it to anyone else. The specifications cite it by section and
  copy none of its text.
- sha256: `f7c375a15db5ed63de760356867211063d164a2ed59f2d38613daec95894ce22`
- Used by: DMA confinement on AMD platforms ([ipc.md](ipc.md), I8): the
  Device Table and I/O page tables (2.2), commands (2.4) and the IVRS
  table (5.2); the IOMMU registers kept out of `RESOURCE_MMIO` (I6).

## Executable format and calling convention

**PE Format** (Microsoft Learn), updated September 10, 2026.

- Document: <https://learn.microsoft.com/en-us/windows/win32/debug/pe-format>
- Access: free web page.
- sha256: `ed7487e49f94c1ad7a4ff99e5859aafcf83c822c23196eb8a88d2db161e7357b`
- Used by: nova-boot's kernel loader; the kernel image format in the boot
  protocol; the images TinyCC writes.

**x64 calling convention** (Microsoft Learn), dated March 19, 2025,
updated May 21, 2026, with **x64 stack usage**, dated December 17, 2018,
updated November 5, 2025.

- Documents: <https://learn.microsoft.com/en-us/cpp/build/x64-calling-convention>,
  <https://learn.microsoft.com/en-us/cpp/build/stack-usage>
- Access: free web pages.
- sha256: `963a36365ace6d4148e5c2d63688b1c89663479c3fa9c951606e9a7861ad1317`
  and `d22c63e02a6b73c9ecb0c073489e0f2e964bda13ce50fe9a6d6f321a3d4cef37`
- Used by: every nova-os image (TinyCC's x86_64-win32 target), firmware
  calls (UEFI 2.3.4.2), the kernel entry and the system-call convention
  ([syscalls.md](syscalls.md)).

## Storage and file systems

**Microsoft Extensible Firmware Initiative FAT32 File System
Specification, FAT: General Overview of On-Disk Format**, version 1.03,
December 6, 2000; legal agreement updated March 30, 2011.

- Document: <https://download.microsoft.com/download/0/8/4/084c452b-b772-4fe5-89bb-a0cbf082286a/fatgen103.doc>
- Access: free download under the agreement printed in the document,
  whose licence covers products used in the boot process (clause 1(e)).
- sha256: `07db52dfd7c81d712dfaaa844eb64db5ce6cd439a435f0d2a414539d07e23c1d`
- Used by: the disk layout and the image tool's FAT32 writer.
- Microsoft also serves an earlier save with the same technical text at
  <https://download.microsoft.com/download/1/6/1/161ba512-40e2-4cc9-843a-923143f3456c/fatgen103.doc>,
  sha256 `b17d66c796d9cd3070adf4ccbc00add5a8b6b6f5491fe8ea949df778fce60172`.

**[MS-VHDX]: Virtual Hard Disk v2 (VHDX) File Format**, protocol revision
8.0, April 23, 2024.

- Document: <https://winprotocoldocs-bhdugrdyduf5h2e4.b02.azurefd.net/MS-VHDX/%5bMS-VHDX%5d.pdf>,
  listed at <https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-vhdx/83e061f8-f6e2-4de1-91bd-5d518a43d477>
- Access: free download under Microsoft's Open Specifications terms.
- sha256: `1419e02e8dd2f6f223b44230e88518c90f6e6af09f902b17f86576fb548d46f4`
- Used by: the image tool's VHDX container.

**NVM Express Base Specification**, revision 2.4, July 31, 2026, and
**NVMe over PCIe Transport Specification**, revision 1.4, July 31, 2026.

- Documents: <https://nvmexpress.org/wp-content/uploads/NVM-Express-Base-Specification-Revision-2.4-Ratified-2026.07.31.pdf>,
  <https://nvmexpress.org/wp-content/uploads/NVM-Express-NVMe-over-PCIe-Transport-Specification-Revision-1.4-Ratified-2026.07.31.pdf>
- Access: free download. The notice in each gives non-members the right
  to review and cite the document and grants no licence to implement it.
- sha256: `fe7647532c06859f2952d9db1f90165aa3e9375343d588d5444253f7dbb2c9bd`
  and `2f6dc41d529c03e0596219fb2a734221ba3261837e14bab61168cde5a3226c30`
- Used by: an NVMe storage driver for real hardware.

## Devices

**Virtual I/O Device (VIRTIO)**, version 1.4, Committee Specification 01,
April 8, 2026 (OASIS).

- Document: <https://docs.oasis-open.org/virtio/virtio/v1.4/cs01/virtio-v1.4-cs01.pdf>
- Access: free download, under the Non-Assertion Mode of the OASIS IPR
  Policy.
- sha256: `497b7b989b5161444e87866c264a46be90911b8f4738eadf4dd4f52eb37c8727`
- Used by: drivers for QEMU's virtio devices.

**eXtensible Host Controller Interface for Universal Serial Bus (xHCI)
Requirements Specification**, revision 2.0, October 2025, document
868296 (Intel). Revision 1.2c, October 2025, document 868295, is the
current revision of the 1.x line.

- Documents: <https://cdrdv2.intel.com/v1/dl/getContent/868296>,
  <https://cdrdv2.intel.com/v1/dl/getContent/868295>
- Access: free download. Each states that it grants no licence to
  Intel's intellectual property.
- sha256: `6b671c4cf9f396a70a5fb993316cbf1cea9e1fd55ce464b5dea0ab6fcd6e4cf0`
  (2.0) and `0b06318005c3e0c8b896f2a002c2a3c78426b5fdacac4ad1cc02ffec15835190`
  (1.2c)
- Used by: a USB host-controller driver (QEMU's `qemu-xhci`).

## Graphics

**How to interpret the sRGB color space (specified in IEC 61966-2-1) for
ICC profiles** (International Color Consortium), April 2015.

- Document: <https://registry.color.org/rgb-registry/files/sRGB.pdf>,
  listed at <https://registry.color.org/rgb-registry/srgb>
- Access: free download.
- sha256: `ceed855c29808217a472954abb04cdc60a5bd3200387a93f5c5ffcef8ddf9f1f`
- Used by: `gfx/gfx.c`, for sRGB's transfer function: the encoding in
  A.8 and the decoding in B.1, whose red line is the decoding; its green
  and blue lines repeat the red channel's variable, and its linear blue
  line misprints 12.92 as 12.02.
- IEC 61966-2-1, which defines the transfer function, is sold by the IEC
  and not pinned.

## PCI Express configuration space

The PCI Express Base Specification and the PCI Firmware Specification
(which defines the MCFG table that ACPI 6.6 reserves in Table 5.6) are
published by PCI-SIG and are not pinned: their current revisions and
access terms are unverified. These public documents describe
memory-mapped configuration space (ECAM):

- **Processor Programming Reference for AMD Family 1Ah Model 02h,
  Revision C1**, publication 57238, revision 0.49, March 6, 2026.
  `MmioCfgBaseAddr` (MSRC001_0058) gives the base and bus range.
  <https://docs.amd.com/api/khub/documents/X_EXZ3uavMHFQe8qvjfHcw/content>,
  a zip archive, sha256
  `e91d9fe3ae32df3d6c88012cdac261440cc06e089a0d83d191247a06c8c91e99`.
- **12th Generation Intel Core Processors Datasheet, Volume 2 of 2**,
  document 655259, revision 003, April 2022: "PCI Express Configuration
  Address Space" and the `PCIEXBAR` register.
  <https://cdrdv2.intel.com/v1/dl/getContent/655259>, sha256
  `8b13b3db5530561e3e252910a17b7f088b5ad04164d5fdca750124e93e868189`.

## Design references

Systems and papers the object, IPC and system-call specifications compare
nova-os with. Their section numbers refer to the copies pinned here.

**seL4 Reference Manual**, version 17.0.0, 30 October 2026 (seL4
Foundation).

- Document: <https://sel4.systems/Info/Docs/seL4-manual-latest.pdf>; the
  URL serves the latest version, so the sha256 identifies 17.0.0.
- Access: free download, licensed GPL-2.0-only. The specifications cite
  and paraphrase it and copy none of its text.
- sha256: `597ff73b57209dee574ecb3ad294ae9e53389399c51e6642deb96d5ad8eb8205`
- Used by: [objects.md](objects.md), [ipc.md](ipc.md),
  [syscalls.md](syscalls.md).

**Improving IPC by Kernel Design**, Jochen Liedtke, 14th ACM Symposium on
Operating Systems Principles (SOSP '93), December 1993, pages 175 to 188,
DOI 10.1145/168619.168633.

- Document: <https://os.inf.tu-dresden.de/papers_ps/jochen/Ipcsosp.ps>,
  the author's PostScript, linked from TU Dresden's L4 page
  <https://os.inf.tu-dresden.de/L4/doc.html>
- Access: free download.
- sha256: `9328d45b96f2d9ea1cc077628b5405ffdabccb8df7a97e474f9b650697716350`
- Used by: [ipc.md](ipc.md), [syscalls.md](syscalls.md).

**On Micro-Kernel Construction**, Jochen Liedtke, 15th ACM Symposium on
Operating Systems Principles (SOSP '95), December 1995, pages 237 to 250,
DOI 10.1145/224056.224075.

- Document: <https://os.inf.tu-dresden.de/papers_ps/jochen/Mikern.ps>,
  the author's PostScript, linked from the same page
- Access: free download.
- sha256: `6f6f600fe4fabe1315770a835c140535fd990fcd31da85dc6160289a4e0b4edf`
- Used by: [objects.md](objects.md), [ipc.md](ipc.md).

**L4 Microkernels: The Lessons from 20 Years of Research and
Deployment**, Gernot Heiser and Kevin Elphinstone, ACM Transactions on
Computer Systems 34(1), April 2016, DOI 10.1145/2893177.

- Document: <https://trustworthy.systems/publications/nicta_full_text/8988.pdf>,
  from the authors' research group
- Access: free download.
- sha256: `4375964e2eac58ba2f185c04412b8eeba749ebe700550f1ed519e7034819a635`
- Used by: [ipc.md](ipc.md).

## Not pinned

- **VESA Enhanced Extended Display Identification Data Standard**
  (E-EDID), Release A, Revision 2, September 25, 2006, which defines
  EDID 1.4. VESA lists it among its free standards but serves those only
  after a registration form, so no copy is pinned. Used by: nova-boot's
  choice of the display's native mode (the base block's header, 3.3; its
  checksum, 3.11; the preferred timing, 3.10.1, Tables 3.21 and 3.22).
- **16550 UART.** `kernel/main.c` drives COM1 through the 16550 register
  interface (line status register at offset 5, transmitter-empty bit
  0x20). The register reference, a 16550 datasheet, is not pinned.
