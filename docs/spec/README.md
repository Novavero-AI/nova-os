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
| [speculation.md](speculation.md) | The speculative-execution mitigations on entry from and exit to user mode and at a process switch, and how the kernel detects which apply |
| [replay.md](replay.md) | Every nondeterministic input a process can receive, and how a recording captures or excludes it |
| [compat.md](compat.md) | How a 32-bit Windows program runs: its process, ntdll, paths and the stand-in file protocol |
| [storage.md](storage.md) | The directory protocol and per-process views, the native file system, the store at `/store`, its builds and garbage collection, and the first milestone |
| [input.md](input.md) | The virtio input driver, the key events it sends a client, its report to the platform driver and how that supervises it |

## Firmware and platform

**Unified Extensible Firmware Interface (UEFI) Specification**, Release
2.11, November 21, 2024 (UEFI Forum).

- Document: <https://uefi.org/sites/default/files/resources/UEFI_Spec_Final_2.11.pdf>,
  also as HTML at <https://uefi.org/specs/UEFI/2.11/>
- Access: free download.
- sha256: `a64b8e442004b91becc3de9afaf8ca61b259a9a3b436accb6b3711ab5400cee9`
- Used by: nova-boot (`boot/efi.h`, `boot/nova-boot.c`), the boot
  protocol, including its random seeds (37.5), the disk layout.

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
  implementation, and the firmware's random number generators. The
  firmware QEMU bundles is an EDK II build.

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
  Local APIC, TSC, the interrupt flag); the system-call entry
  ([syscalls.md](syscalls.md)); page-table isolation
  ([speculation.md](speculation.md): SMEP and PCID enumeration, 5.1.4;
  PCIDs, 5.10.1 and 5.10.4.1).

**Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 1:
Basic Architecture**, order number 253665-093US, September 2026, the same
release as Volume 3.

- Document: <https://cdrdv2.intel.com/v1/dl/getContent/671436>
- Access: free download.
- sha256: `f6ffc430dfa2dada4f791b1ecb462810e59827befe9a9c23f9160941eea642f9`
- Used by: user-mode drivers' I/O-port access ([ipc.md](ipc.md), I6), for
  the I/O permission bitmap (20.5.2); the kernel's random number
  generator, for RDRAND and RDSEED and their retries (7.3.17) and CPUID's
  leaf range (21.1.3); the 32-bit lane, for the general registers' upper
  halves in compatibility mode (3.4.1.1).

**Intel 64 and IA-32 Architectures Software Developer's Manual, Volume 2
(2A, 2B, 2C and 2D): Instruction Set Reference, A-Z**, order number
325383-093US, September 2026, the same release.

- Document: <https://cdrdv2.intel.com/v1/dl/getContent/671110>
- Access: free download.
- sha256: `d137a7887787bbb06c1a8b8bafd51958d8d2e16b4dff987a03226c845cca1516`
- Used by: the kernel's encodings of RDRAND and RDSEED (Volume 2B), which
  TinyCC's assembler lacks.

**AMD64 Architecture Programmer's Manual, Volume 2: System Programming**,
publication 24593, revision 3.46, October 2026.

- Document: <https://docs.amd.com/api/khub/documents/LNBl5evreogi0r25VIFF2g/content>
- Access: free download.
- sha256: `e4130d5aa26227b6a116d75af9e0f6e5adb4c034ff0d49a3c0fe73c3f49794cf`
- Used by: nova-boot and the kernel on AMD processors, for the same areas
  as the Intel manual; IA32_EFER's address (3.1.7), which the Intel
  volume pinned here does not give; memory encryption (7.10); the
  speculation controls ([speculation.md](speculation.md): Automatic IBRS,
  3.1.7; SPEC_CTRL, PRED_CMD and the return address predictor, 3.2.9).

**AMD64 Architecture Programmer's Manual, Volume 3: General-Purpose and
System Instructions**, publication 24594, revision 3.39, October 2026.

- Document: <https://docs.amd.com/api/khub/documents/BqEoC2FadIAgbrUdEzDJIg/content>
- Access: free download. It states that it grants no licence to AMD's
  intellectual property and prohibits unauthorized copying or
  distribution; the specifications cite it and copy none of its text.
- sha256: `b598514b19f204d973040d9867a7ffc48c949009a8b3dc86b20c9188e721ee45`
- Used by: [speculation.md](speculation.md), for the CPUID bits that
  enumerate AMD's speculation controls and susceptibilities (E.3.2,
  E.4.7, E.4.19).

## Speculative execution

Vendor guidance on speculative-execution attacks.
[speculation.md](speculation.md) cites each document by the name in its
first column or heading.

**Hardware Features and Behavior Related to Speculative Execution**
("Intel HFB"), technical paper, document 758913, revision 2.1, October
2026 (Intel).

- Document: <https://cdrdv2.intel.com/v1/dl/getContent/758913>, which
  serves the revision's PDF; Intel publishes it as a web page, ID 823144,
  at <https://www.intel.com/content/www/us/en/developer/articles/technical/software-security-guidance/technical-documentation/hardware-behavior-related-to-speculative-execution.html>
- Access: free download. It grants no licence to Intel's intellectual
  property, but "the information reported herein is available for use in
  connection with the mitigation of the security vulnerabilities
  described". Every page is footed "Intel Confidential", although Intel
  serves the document publicly and its cover gives the public URL.
- sha256: `41a35c8591054419eb295e1afe370ede0d1c4d36eb9ea5c154063e969957c823`
- Used by: [speculation.md](speculation.md).

**Intel software security guidance**, web pages (Intel). Each page gives
an ID, the date it was last updated and sometimes a version; the
affected-processors table gives none, and its date is its page
metadata's last-modified time.

| Name | Page | ID, version, updated | sha256 |
|---|---|---|---|
| Intel CPUID | [CPUID Enumeration and Architectural MSRs](https://www.intel.com/content/www/us/en/developer/articles/technical/software-security-guidance/technical-documentation/cpuid-enumeration-and-architectural-msrs.html) | 758381, version 8.0, October 22, 2026 | `a36940e07d7fe03cca1e80eaad1d4e46f8b3913b3f3c5fb60a5e337984c9608e` |
| Intel retpoline | [Retpoline: A Branch Target Injection Mitigation](https://www.intel.com/content/www/us/en/developer/articles/technical/software-security-guidance/technical-documentation/retpoline-branch-target-injection-mitigation.html) | 758357, version 2.1, August 22, 2022 | `835c5bd131a2da86305f9c91ee4cc2bec2af49a65eec05e902dd2cb00f058481` |
| Intel BHI | [Branch History Injection and Intra-mode Branch Target Injection](https://www.intel.com/content/www/us/en/developer/articles/technical/software-security-guidance/technical-documentation/branch-history-injection.html), technical documentation | 824191, version 2.6, May 12, 2025 | `5fc47309a092a3d9d4e54aadaa21ba185c9c1632bc31428c8f560985dfa8e1c4` |
| Intel RSBU | [Return Stack Buffer Underflow](https://www.intel.com/content/www/us/en/developer/articles/technical/software-security-guidance/advisory-guidance/return-stack-buffer-underflow.html) | 737306, July 12, 2022 | `e2701b33eb2f034eac49f4fc311dac5e86a42a3cd752fa4f947b732cd07aac6e` |
| Intel PBRSB | [Post-barrier Return Stack Buffer Predictions](https://www.intel.com/content/www/us/en/developer/articles/technical/software-security-guidance/advisory-guidance/post-barrier-return-stack-buffer-predictions.html) | 739507, August 9, 2022 | `8850920b538dd086f3ef4527550a7a3ddd0453fac55796d5b5f283be6c014402` |
| Intel RDCL | [Rogue Data Cache Load](https://www.intel.com/content/www/us/en/developer/articles/technical/software-security-guidance/advisory-guidance/rogue-data-cache-load.html) | 660223, January 3, 2018 | `ff0e336051ab455dc19929414898e9374c12e169cce0906efb1caf52a55c7846` |
| Intel L1TF | [L1 Terminal Fault](https://www.intel.com/content/www/us/en/developer/articles/technical/software-security-guidance/technical-documentation/intel-analysis-l1-terminal-fault.html), technical documentation | 758376, August 14, 2018 | `2a35669ff18d34277058456d16f088e4b3b861bafb8baced40f9cd02eca90534` |
| Intel MDS | [Microarchitectural Data Sampling](https://www.intel.com/content/www/us/en/developer/articles/technical/software-security-guidance/technical-documentation/intel-analysis-microarchitectural-data-sampling.html), technical documentation | 758366, version 3.0, March 11, 2021 | `8b7455d51254b7f4af5ab4b04e49cd963fc3922a6d843a98562e3073fb396d98` |
| Intel TAA | [Intel TSX Asynchronous Abort](https://www.intel.com/content/www/us/en/developer/articles/technical/software-security-guidance/advisory-guidance/intel-tsx-asynchronous-abort.html) | 660217, November 12, 2019 | `5a69896041f7e73fe1c9f42e8de1af2bf78a8e3b7608bc55ebd43f72383d9b68` |
| Intel MMIO | [Processor MMIO Stale Data Vulnerabilities](https://www.intel.com/content/www/us/en/developer/articles/technical/software-security-guidance/technical-documentation/processor-mmio-stale-data-vulnerabilities.html), technical documentation | 758360, June 14, 2022 | `0ae873f6b06d994cf83701a826283b30fd95eadc31f78afda45326e22fbc7387` |
| Intel GDS | [Gather Data Sampling](https://www.intel.com/content/www/us/en/developer/articles/technical/software-security-guidance/technical-documentation/gather-data-sampling.html), technical documentation | 785676, version 1.0, December 5, 2024 | `9a6849881dbe8e69c529b14e716119cc820032c0616b1a65a8bfac44d945286f` |
| Intel RFDS | [Register File Data Sampling](https://www.intel.com/content/www/us/en/developer/articles/technical/software-security-guidance/advisory-guidance/register-file-data-sampling.html) | 817587, version 1.0, July 22, 2025 | `f60639698afbc9aa90a6b04e3d45f67a1bfb877122ffefcbae815cad275a7a35` |
| Intel SWAPGS | [Speculative Behavior of SWAPGS and Segment Registers](https://www.intel.com/content/www/us/en/developer/articles/technical/software-security-guidance/technical-documentation/speculative-behavior-swapgs-and-segment-registers.html), technical documentation | 758371, August 6, 2019 | `bf0e5297926eae5354adf2e1c01f061713a9ce7b3e913d91376a89f99bf1ad90` |
| Intel ITS | [Indirect Target Selection](https://www.intel.com/content/www/us/en/developer/articles/technical/software-security-guidance/advisory-guidance/indirect-target-selection.html) | 854667, version 1.0, May 13, 2025 | `94c0c2bc0a39196e79e254476512c4a608e0dcd46f3c09e8b8c4e11b2d445474` |
| Intel IBPDU | [Indirect Branch Predictor Delayed Updates](https://www.intel.com/content/www/us/en/developer/articles/technical/software-security-guidance/advisory-guidance/indirect-branch-predictor-delayed-updates.html) | 855452, version 1.0, May 21, 2025 | `4b6bfbe6056f796a7e1d82f5ab1c07b1571d4732f74f8c54f82c55cf7f28391a` |
| Intel table | [Affected Processors: Guidance for Security Issues on Intel Processors](https://www.intel.com/content/www/us/en/developer/topic-technology/software-security-guidance/processors-affected-consolidated-product-cpu-model.html) | November 2, 2026 | `54abc61a2eca7911263db3d98338f7549b50cba992eb78619b06898ee9a9b949` |

- Access: free web pages.
- Used by: [speculation.md](speculation.md).

**Software Techniques for Managing Speculation on AMD Processors** ("AMD
STMS"), white paper, revision 5.09.23, dated May 9, 2023 in its revision
history (AMD). docs.amd.com lists the same file as documents 70586 and
70628.

- Document: <https://docs.amd.com/api/khub/documents/vbhMWnxkkda5Mz6LirjB_Q/content>
- Access: free download.
- sha256: `d86742c1bf84c7c9a543e014a42152d525bd3cda05eb7f67b453fe5df72036a6`
- Used by: [speculation.md](speculation.md).

**Speculative Return Stack Overflow Mitigation Options and Considerations
for Public Cloud Users** ("AMD SRSO cloud"), white paper, first edition,
April 2024 (AMD), docs.amd.com document 70608.

- Document: <https://docs.amd.com/api/khub/documents/qvUciCbuQfXCBkXbklCwAw/content>
- Access: free download. It states that it grants no licence to AMD's
  intellectual property.
- sha256: `117d0ca1381c4861c7a594b4aecbd0a953ecd2146f69482642bccc1ee006a282`
- Used by: [speculation.md](speculation.md).

**AMD white papers, from a third-party mirror.** AMD publishes these on
www.amd.com, which reset or timed out every request made to pin them, and
docs.amd.com does not carry them. The copies pinned here come from a
third-party mirror and have not been compared with AMD's.

| Name | Document | Revision, date | sha256 |
|---|---|---|---|
| AMD SRSO | [Technical Update Regarding Speculative Return Stack Overflow](https://kib.kiev.ua/x86docs/AMD/Spectre/AMD-SB-7005-r2.0.pdf) | 2.0, February 25, 2024 | `c86e06dd6dc420f48a13ec30f79619101c66dd9ee26511f5887d9b6057bb9ba6` |
| AMD BTC | [Technical Guidance for Mitigating Branch Type Confusion](https://kib.kiev.ua/x86docs/AMD/Spectre/AMD-SB-1037-r3.0.pdf) | 3.0, November 8, 2022 | `13a4af325a18a94b353a418327ab377cd536cb91ba1de9f2965832885173256c` |
| AMD TSA | [Technical Guidance for Mitigating Transient Scheduler Attacks](https://kib.kiev.ua/x86docs/AMD/Spectre/technical-guidance-for-mitigating-transient-scheduler-attacks.pdf) | 2.0, July 2025 | `a1360e941db1f2c37fd05113dce8feb7ee6f5a431cf9189302b53ced6640d07b` |

- Access: free download from the mirror. Each states that it grants no
  licence to AMD's intellectual property.
- Used by: [speculation.md](speculation.md).

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

## Cryptography

**ChaCha20 and Poly1305 for IETF Protocols**, RFC 8439, June 2018
(IRTF).

- Document: <https://www.rfc-editor.org/rfc/rfc8439.txt>
- Access: free.
- sha256: `25bef70fbf7a07ff45c2fe4cb7c6ce954eac687413d8610603268b4e4415324c`
- Used by: the kernel's ChaCha20 block function (2.3) and its boot-time
  test (2.3.2, A.1).

**Secure Hash Standard (SHS)**, FIPS PUB 180-4, August 2015 (NIST), and
**US Secure Hash Algorithms (SHA and SHA-based HMAC and HKDF)**, RFC 6234,
May 2011 (IETF).

- Documents: <https://nvlpubs.nist.gov/nistpubs/FIPS/NIST.FIPS.180-4.pdf>,
  <https://www.rfc-editor.org/rfc/rfc6234.txt>
- Access: free.
- sha256: `0455b406d89648d20cbde375561e19c245b9815e894164c2670772e3d54deb82`
  (FIPS 180-4) and
  `8f39f02a57bfd1da15634706724a585766e6223f77ecdaf243cad16cd3a1aa1b`
  (RFC 6234)
- Used by: the kernel's SHA-256 (FIPS 180-4) and its boot-time test
  (RFC 6234, 8.5); the checksums and digests of
  [storage.md](storage.md).

**Fast-key-erasure random-number generators**, D. J. Bernstein, the
cr.yp.to blog, July 23, 2017.

- Document: <https://blog.cr.yp.to/20170723-random.html>
- Access: free web page. The page lists the blog's newer posts, so a
  later copy has another sha256.
- sha256: `5e20540f869e04a3b776aa4bb54a73ec00a948e67d932ad1341956890ebaadf3`
- Used by: the kernel's random number generator
  (`kernel/random/generator.c`).

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

## Windows compatibility

Microsoft Learn pages and open specifications, each pinned as fetched on
November 7, 2026 with the date it shows as its last update. Used by:
[compat.md](compat.md), `include/nt32.h`, ntdll (`ntdll/ntdll.c`) and the
32-bit lane ([syscalls.md S11](syscalls.md#s11-the-32-bit-lane)). Access:
free web pages; MS-ERREF, MS-SMB2 and MS-FSCC under Microsoft's Open
Specifications terms.

| Page | Updated | sha256 |
|---|---|---|
| [TEB (winternl.h)][ms-teb] | 2024-05-29 | `6bceaadb1721e142725195a4a32694cf91e6ca60495251669e20ffbda902368b` |
| [PEB (winternl.h)][ms-peb] | 2022-09-01 | `44690a16f009a01093425492edf60d26535a91892cc5b42c0d92ca392b2fb2b0` |
| [PEB_LDR_DATA (winternl.h)][ms-ldr] | 2024-02-22 | `1d4e819dbfde0c2992ef4d90e60dfe20cde1928260f07f64d8638ebbeca36fca` |
| [RTL_USER_PROCESS_PARAMETERS (winternl.h)][ms-params] | 2024-02-22 | `e211deb26a9a92bcbb1b56641e56468c50cebd09d814aadb0318aae71b622536` |
| [_UNICODE_STRING (ntdef.h)][ms-unicode] | 2024-02-22 | `3be29607c409759a7b93e68e5269ec936f810cdbea0ed0bc803c761989fafbf1` |
| [_OBJECT_ATTRIBUTES (ntdef.h)][ms-oa] | 2023-08-09 | `3bf9ea1a7624efeb2bc1e4f30678f1bc2fb3d14f34469aed45d5dd79738b173b` |
| [LIST_ENTRY (ntdef.h)][ms-list] | 2021-04-02 | `f7caf67826f0ffab8f34eea4156e89d950c69497a3e5242fea42b2ab3b7adbca` |
| [_IO_STATUS_BLOCK (wdm.h)][ms-iosb] | 2022-02-25 | `1fb045a3827def44428bbcf8553476cddeb1074b9bf4bc6c9d8265cc97987612` |
| [NtCreateFile function (winternl.h)][ms-create] | 2021-10-13 | `5bc4f3dc9991d0bbc77c8b0af71690869c988740c26cd2fd6bf1b5f01243e062` |
| [NtReadFile function (ntifs.h)][ms-read] | 2023-07-06 | `1cee672a2a9cba820c9568d401a483121f8cf92011aa156a66b81af6d43a02c6` |
| [NtWriteFile function (ntifs.h)][ms-write] | 2023-07-06 | `406dffc655626287d381e946956756217593ee79d1db8092145f54990a855c41` |
| [NtClose function (ntifs.h)][ms-close] | 2024-02-22 | `0735bb7902046fe6aa405c8552af3b924e34946678fd8b3f3b5d42be45604b4b` |
| [NtSetInformationFile function (ntifs.h)][ms-setinfo] | 2023-07-06 | `25929a5d6fda0017fc5f3aa8d0c645816cb9ab9eb3ae5c811bafeaa4bccdd6b8` |
| [NtQueryInformationFile function (ntifs.h)][ms-queryinfo] | 2023-07-06 | `3f88b37c07618a71cfdff6d77d49345441ca96f1aacae689b5dac5249d2ae490` |
| [FILE_POSITION_INFORMATION structure (wdm.h)][ms-position] | 2024-02-22 | `4a84f3a2f7b929efa192dc4c6753e80de048599e678146314d65511d64187a6c` |
| [FILE_STANDARD_INFORMATION structure (wdm.h)][ms-standard] | 2024-02-22 | `0dd6048f159af807a5f1172bdaf41905f471d15eaafec728a60e1c241714e160` |
| [ZwTerminateProcess function (ntddk.h)][ms-terminate] | 2023-02-28 | `b03ea4d9c659497864209dffb643a9ecbcc9289b833601502b4a3f9ae88cd694` |
| [ZwCurrentProcess Macro][ms-current] | 2023-03-01 | `3713699fdcb313d8125425e35a87ff871e4c0f79eee90e5d2693458c0858c6a2` |
| [Naming Files, Paths, and Namespaces][ms-naming] | 2024-08-28 | `f1f1b4f6b41241f58742eb6050ba4dd9409720f62a4d1b1ff131417193b00691` |
| [Environment Variables][ms-environment] | 2025-07-14 | `d95ef68e21a81ffd7720ae5d7441766101ae45ac93a2427d477c1f92030ea6d8` |
| [__stdcall][ms-stdcall] | 2025-02-10 | `95d0de9ae535b11699e4244020017a8cfa919eb9607454c4d20eb666b55211e9` |
| [Argument Passing and Naming Conventions][ms-arguments] | 2025-10-03 | `da72f0dd7febe8a67a2d6265af9d8107a9efe36c642751f2c9d22434232722e1` |
| [MS-ERREF 2.3.1 NTSTATUS Values][ms-erref-values] | 2021-11-16 | `eb8b93ae6e40f6851e3b9172b8db2c18f6e0fc7150a636dbc1d22fb1a4152a57` |
| [MS-SMB2 2.2.13 SMB2 CREATE Request][ms-smb2-create] | 2023-02-28 | `85535072834d14fed56ba4d86d67d2a7c2f6e8241ae9f398dfc00b0f83579f19` |
| [MS-SMB2 2.2.13.1.1 File_Pipe_Printer_Access_Mask][ms-smb2-access] | 2023-02-28 | `dbf068fe3ac25cda41f31a761dbc4b9e85bf55be0d5e53a8e879c48dbacea4e0` |
| [MS-SMB2 2.2.14 SMB2 CREATE Response][ms-smb2-response] | 2025-04-07 | `dae9e65c1bf41681c5390670a97c468659a16ad1967466867782d5997f13b05d` |
| [MS-FSCC 2.4.40 FilePositionInformation][ms-fscc-position] | 2025-04-07 | `7cee0eeeac9a93917d1f43cb11e65ec1ed957c74c2273a8d32dc2966cb9021d2` |
| [MS-FSCC 2.4.47 FileStandardInformation][ms-fscc-standard] | 2021-05-28 | `cfca0d77cda89200d560b1cca83c6bcbd8e261a8f369e737a0c129cf2352e889` |

[ms-teb]: https://learn.microsoft.com/en-us/windows/win32/api/winternl/ns-winternl-teb
[ms-peb]: https://learn.microsoft.com/en-us/windows/win32/api/winternl/ns-winternl-peb
[ms-ldr]: https://learn.microsoft.com/en-us/windows/win32/api/winternl/ns-winternl-peb_ldr_data
[ms-params]: https://learn.microsoft.com/en-us/windows/win32/api/winternl/ns-winternl-rtl_user_process_parameters
[ms-unicode]: https://learn.microsoft.com/en-us/windows/win32/api/ntdef/ns-ntdef-_unicode_string
[ms-oa]: https://learn.microsoft.com/en-us/windows/win32/api/ntdef/ns-ntdef-_object_attributes
[ms-list]: https://learn.microsoft.com/en-us/windows/win32/api/ntdef/ns-ntdef-list_entry
[ms-iosb]: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/ns-wdm-_io_status_block
[ms-create]: https://learn.microsoft.com/en-us/windows/win32/api/winternl/nf-winternl-ntcreatefile
[ms-read]: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-ntreadfile
[ms-write]: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-ntwritefile
[ms-close]: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-ntclose
[ms-setinfo]: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-ntsetinformationfile
[ms-queryinfo]: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntifs/nf-ntifs-ntqueryinformationfile
[ms-position]: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/ns-wdm-_file_position_information
[ms-standard]: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/wdm/ns-wdm-_file_standard_information
[ms-terminate]: https://learn.microsoft.com/en-us/windows-hardware/drivers/ddi/ntddk/nf-ntddk-zwterminateprocess
[ms-current]: https://learn.microsoft.com/en-us/windows-hardware/drivers/kernel/zwcurrentprocess
[ms-naming]: https://learn.microsoft.com/en-us/windows/win32/fileio/naming-a-file
[ms-environment]: https://learn.microsoft.com/en-us/windows/win32/procthread/environment-variables
[ms-stdcall]: https://learn.microsoft.com/en-us/cpp/cpp/stdcall
[ms-arguments]: https://learn.microsoft.com/en-us/cpp/cpp/argument-passing-and-naming-conventions
[ms-erref-values]: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-erref/596a1078-e883-4972-9bbc-49e60bebca55
[ms-smb2-create]: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-smb2/e8fb45c1-a03d-44ca-b7ae-47385cfd7997
[ms-smb2-access]: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-smb2/77b36d0f-6016-458a-a7a0-0f4a72ae1534
[ms-smb2-response]: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-smb2/d166aa9e-0b53-410e-b35e-3933d8131927
[ms-fscc-position]: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/e3ce4a39-327e-495c-99b6-6b61606b6f16
[ms-fscc-standard]: https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-fscc/5afa7f66-619c-48f3-955f-68c4ece704ae

**stage0-pe32**, commit `ff1c2ad86a91d3d7d086722ffed49dfe18dd50d0`, the
revision nova-nix builds its chain from, and the trees it builds from at
the commits it records for them: **M2libc**
`13e57eb52bd30a6b0296f1cef1c54fc40065f07b`, **M2-Planet**
`ee0bf91eb02390c2d40bd85379113e524f0c752b` and **mescc-tools**
`578ac99623d291e2b9eacf03cacc390cfe512661` (all GPL-3.0). The seed,
`bootstrap-seeds/PE32/i386/hex0-seed.exe`, and the files its chain reads
up to M1 are test inputs, fetched by hash for the test disk
([README](../../README.md#disk-image)); the sources were read only to
learn which routines, fields and values the chain's programs use, and
nothing of them is copied ([compat.md](compat.md)).

## Storage and file systems

**Microsoft Extensible Firmware Initiative FAT32 File System
Specification, FAT: General Overview of On-Disk Format**, version 1.03,
December 6, 2000; legal agreement updated March 30, 2011.

- Document: <https://download.microsoft.com/download/0/8/4/084c452b-b772-4fe5-89bb-a0cbf082286a/fatgen103.doc>
- Access: free download under the agreement printed in the document,
  whose licence covers products used in the boot process (clause 1(e)).
- sha256: `07db52dfd7c81d712dfaaa844eb64db5ce6cd439a435f0d2a414539d07e23c1d`
- Used by: the disk layout and the image tool's FAT32 writer;
  [storage.md](storage.md), for the licence's limit to the boot process.
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
- Used by: an NVMe storage driver for real hardware;
  [storage.md](storage.md), for the Flush command (7.2).

## Devices

**Virtual I/O Device (VIRTIO)**, version 1.4, Committee Specification 01,
April 8, 2026 (OASIS).

- Document: <https://docs.oasis-open.org/virtio/virtio/v1.4/cs01/virtio-v1.4-cs01.pdf>
- Access: free download, under the Non-Assertion Mode of the OASIS IPR
  Policy.
- sha256: `497b7b989b5161444e87866c264a46be90911b8f4738eadf4dd4f52eb37c8727`
- Used by: drivers for QEMU's virtio devices, for the PCI transport
  (4.1) and split virtqueues (2.7) (`drivers/virtio_driver.c`): the
  virtio block driver (`drivers/virtio_blk.c`), for the block device
  (5.2), and the virtio input driver (`drivers/virtio_input.c`,
  [input.md](input.md)), for the input device (5.8);
  [storage.md](storage.md), for when a block write is stable (5.2.6.2).

**Linux input subsystem**, tag `v7.3`, commit
`c03c1a14507dfaf8b856c2608f9ac216021fc222`: the event codes header,
`include/uapi/linux/input-event-codes.h`, and the event codes
documentation, `Documentation/input/event-codes.rst`. VIRTIO 1.4 5.8
defers to them for the meaning of an input event.

- Documents: <https://raw.githubusercontent.com/torvalds/linux/v7.3/include/uapi/linux/input-event-codes.h>,
  <https://raw.githubusercontent.com/torvalds/linux/v7.3/Documentation/input/event-codes.rst>
- Access: free. The header is GPL-2.0-only with the Linux syscall note,
  the documentation GPL-2.0; [input.md](input.md) cites their values
  and copies none of their text.
- sha256: `c51a642306313045e83b853c729902eadf50856c3af5e260bbfc762e54e798ae`
  (header) and
  `09e7dab4c380a6554133553114fcf805c3c3be0df4bf2ea991ebe729fcee5d36`
  (documentation)
- Used by: [input.md](input.md): the event type `EV_KEY` (1) and the key
  codes (the header); a key event's values, 0 released, 1 pressed and 2
  repeated (the documentation, `EV_KEY`).

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
  `MmioCfgBaseAddr` (MSRC001_0058) gives the base and bus range;
  `CPUID_Fn00000001_ECX` and `CPUID_Fn00000007_EBX_x00` give AMD's
  RDRAND and RDSEED bits, which `kernel/x86_64/entropy.c` reads;
  `DE_CFG` (MSRC001_1029), which makes LFENCE dispatch serializing, and
  `PATCH_LEVEL` (MSR0000_008B), the loaded microcode's revision, which
  `kernel/x86_64/speculation.c` sets and reads
  ([speculation.md](speculation.md) M4, M13).
  <https://docs.amd.com/api/khub/documents/X_EXZ3uavMHFQe8qvjfHcw/content>,
  a zip archive, sha256
  `e91d9fe3ae32df3d6c88012cdac261440cc06e089a0d83d191247a06c8c91e99`.
- **12th Generation Intel Core Processors Datasheet, Volume 2 of 2**,
  document 655259, revision 003, April 2022: "PCI Express Configuration
  Address Space" and the `PCIEXBAR` register, which give a function's
  configuration space address; its register descriptions, for the
  configuration header's fields, the Device Command bits and a bridge's
  bus numbers; and the Volume Management Device's MSI-X table (8.2.2 to
  8.2.4), for a table entry's layout.
  <https://cdrdv2.intel.com/v1/dl/getContent/655259>, sha256
  `8b13b3db5530561e3e252910a17b7f088b5ad04164d5fdca750124e93e868189`.

The MCFG's allocation structures and the MSI-X capability's layout (its
ID, message control and table offset) are in none of these; the kernel's
MCFG reader (`kernel/acpi/acpi.c`) and `drivers/pci.h` follow the layout
QEMU's firmware and devices were observed to use.

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

## Storage design references

Papers and reference documents [storage.md](storage.md) compares nova-os
with. Its page numbers are the ones each copy pinned here prints.

**The Purely Functional Software Deployment Model**, Eelco Dolstra, PhD
thesis, Utrecht University, defended January 18, 2006.

- Document: <https://edolstra.github.io/pubs/phd-thesis.pdf>, the
  author's copy
- Access: free download; no terms stated.
- sha256: `f2bd78a82fb615ce2f3145d5418733bc65e09121147ecb814eba54eaf59875e4`
- Used by: [storage.md](storage.md). The PDF's page is the printed page
  plus 8.

**The Design and Implementation of a Log-Structured File System**, Mendel
Rosenblum and John K. Ousterhout, the preprint dated July 24, 1991, of the
paper in the 13th ACM Symposium on Operating Systems Principles and ACM
Transactions on Computer Systems 10(1), February 1992.

- Document: <https://people.eecs.berkeley.edu/~brewer/cs262/LFS.pdf>, a
  course copy
- Access: free download; no terms stated. Its page numbers are the
  preprint's, not the journal's.
- sha256: `cc6a7d7e52cd9182368f42024188bfbcf899f3fc47ce8b87c33a1aabffa286b7`
- Used by: [storage.md](storage.md).

**The Zettabyte File System**, Jeff Bonwick, Matt Ahrens, Val Henson, Mark
Maybee and Mark Shellenbaum, Sun Microsystems, 2003; the paper names no
venue.

- Document: <https://web.mit.edu/6.033/2019/wwwdocs/papers/zfs.pdf>, a
  course copy
- Access: free download; no terms stated.
- sha256: `27beda48b1e8bdcc6018b9858b1e55e5b5c596e25d9ff61115be3509a05f5615`
- Used by: [storage.md](storage.md).

**ZFS On-Disk Specification, Draft**, Sun Microsystems, 2006.

- Document: <https://people.cs.umass.edu/~liberato/courses/2020-spring-compsci590k/files/ondiskformat0822.pdf>,
  a course copy; no copy from Sun or Oracle was found.
- Access: free download. It states that it is distributed under licences
  restricting its use, copying and distribution; storage.md cites it by
  section and copies none of its text.
- sha256: `9c67ac514b02fe46ba1958322e9b3870995bda64ac73150bc82c9eb19e341ce0`
- Used by: [storage.md](storage.md).

**File System Design for an NFS File Server Appliance**, Dave Hitz, James
Lau and Michael Malcolm, USENIX Winter 1994 Technical Conference, as
Network Appliance Technical Report 3002, Rev. C 3/95.

- Document: <https://rcs.uwaterloo.ca/~ali/cs350-f19/papers/wafl.pdf>, a
  course copy of the report, which carries USENIX's permission to
  reproduce it. USENIX serves the paper as text, without page numbers, at
  <https://static.usenix.org/publications/library/proceedings/sf94/full_papers/hitz.a>.
- Access: free download.
- sha256: `e254a24f5d540bc68bdabe235405520961ddc9b2e9cf4ebeb304965c27e656d2`
- Used by: [storage.md](storage.md).

**Apple File System Reference**, Apple Inc., June 22, 2020.

- Document: <https://developer.apple.com/support/downloads/Apple-File-System-Reference.pdf>
- Access: free download. Its notice permits storing it for personal use
  and grants no licence to the technology it describes; storage.md cites
  it by page and copies none of its text.
- sha256: `74c3db584971aea2b11549ca25799ead824e1bd8994e94cd3f8665ce5aea79cc`
- Used by: [storage.md](storage.md).

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
- **AMD-SB-7005**, AMD's "Return Address Predictor Security Bulletin",
  at <https://www.amd.com/en/resources/product-security/bulletin/amd-sb-7005.html>,
  the address AMD SRSO gives. AMD SRSO 5 refers to it for the microcode
  revisions from which "Zen3" and "Zen4" processors clear branch types on
  IBPB.
  www.amd.com reset or timed out every request made to pin it, as for the
  white papers under [Speculative execution](#speculative-execution).
  Used by: [speculation.md](speculation.md) M10, which logs SRSO as
  unconfirmed on those processors until the list is pinned.
- **BTRFS: The Linux B-Tree Filesystem**, O. Rodeh and others, ACM
  Transactions on Storage, August 2013, as the Btrfs documentation cites
  it. The ACM Digital Library answered every request with a bot check,
  and no open copy was found. [storage.md](storage.md) cites the Btrfs
  documentation instead.
- **The Linux implementation of a log-structured file system**, Ryusuke
  Konishi and others, ACM SIGOPS Operating Systems Review 40(3), pages
  102 to 107, 2006, as the NILFS project's links page cites it. The ACM
  Digital Library answered with a bot check. [storage.md](storage.md)
  cites the NILFS project's documentation instead.
