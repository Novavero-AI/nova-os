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
| [log.md](log.md) | The debug log: the form of its lines, how tests report on it in KTAP, and which self-tests run on which boot |
| [compat.md](compat.md) | How a 32-bit Windows program runs: its process, ntdll, paths and the stand-in file protocol |
| [storage.md](storage.md) | The directory protocol and per-process views, the native file system, the store at `/store`, its builds and garbage collection, and the first milestone |
| [input.md](input.md) | The virtio input driver, the description, key events and pointer reports it sends a client, its report to the platform driver and how that supervises it |
| [console.md](console.md) | The console: how it takes the display from the kernel, how the first process starts it, its keys, the session protocol programs write text and read lines with, and its panel and font |
| [store-image.md](store-image.md) | The store image the first milestone boots from: its header, registrations, nodes, blobs and their hash trees |
| [pool-format.md](pool-format.md) | A data disk's pool: its partition, superblock rings, block pointers, node table and directories, and its commit |
| [accounts.md](accounts.md) | Users' accounts and records, password hashing, login, the session a login opens, the shell and the first boot |
| [shell.md](shell.md) | nvo, the shell's language: its grammar, values, pipelines and errors, how a command is found and run from `/store`, the handles the shell grants a command, interactive behaviour, replay and settings |
| [display.md](display.md) | Who holds the display from boot to the desktop, the compositor, windows and their buffers, composition, the pointer, routing input, the secure attention key, capture, the trusted interface, replay, the GPU path, the build plan and notifications |
| [ui.md](ui.md) | The toolkit: the rasterizer's outlines, the font and its format, text layout, the interface as data and its renderer, widgets, layout, themes, accessibility, the HUD and the first programs |
| [look.md](look.md) | The visual design, Horizon: how sizes follow the scale, the theme's colours, surfaces and type, controls, the focus marks and motion, and the layout of the bar, login, windows, the HUD, notifications and the system's requests |
| [network.md](network.md) | The virtio network driver and its frame protocol, the network service's TCP/IP stack and the resolver, grants and sockets as channels, replay, the threat model and limits, and the order of work with its CI checks |
| [font-format.md](font-format.md) | The nova-os font: its header, character map, glyphs, outlines and kerning, and its conversion from TrueType |

## Firmware and platform

**Unified Extensible Firmware Interface (UEFI) Specification**, Release
2.11, November 21, 2024 (UEFI Forum).

- Document: <https://uefi.org/sites/default/files/resources/UEFI_Spec_Final_2.11.pdf>,
  also as HTML at <https://uefi.org/specs/UEFI/2.11/>
- Access: free download.
- sha256: `a64b8e442004b91becc3de9afaf8ca61b259a9a3b436accb6b3711ab5400cee9`
- Used by: nova-boot (`boot/efi.h`, `boot/nova-boot.c`), the boot
  protocol, including its random seeds (37.5), the disk layout, and the
  data disk's GPT ([pool-format.md](pool-format.md), `tools/mkdata.c`,
  `fs/fs.c`).

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
  firmware QEMU bundles is an EDK II build. [shell.md](shell.md), for
  the UEFI Shell's script extension
  (`ShellPkg/Application/Shell/Shell.c`).

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
  PCIDs, 5.10.1 and 5.10.4.1); [display.md](display.md), for the
  write-combining type's properties (14.3, Table 14-2) and its buffers
  (14.3.1).

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
  [storage.md](storage.md), [store-image.md](store-image.md) and
  [pool-format.md](pool-format.md).

**Argon2 Memory-Hard Function for Password Hashing and Proof-of-Work
Applications**, RFC 9106, September 2021 (IRTF).

- Document: <https://www.rfc-editor.org/rfc/rfc9106.txt>
- Access: free.
- sha256: `855c06f060379e34285e83a217e9069b5c72e161a1e54df9af5cd88dbb231f31`
- Used by: the accounts service's Argon2id (`accounts/argon2.c`: H_0,
  H', indexing, G and P, 3.2 to 3.6), its parameters (section 4) and
  its check at start against the Argon2id test vector (5.3)
  ([accounts.md](accounts.md) A4).

**The BLAKE2 Cryptographic Hash and Message Authentication Code (MAC)**,
RFC 7693, November 2015 (Independent Submission).

- Document: <https://www.rfc-editor.org/rfc/rfc7693.txt>
- Access: free.
- sha256: `c943754888364fe29bbd0bb3c71c6658ef392371497eaa4a9d9dde015b0721e5`
- Used by: BLAKE2b (`accounts/blake2b.c`: 2 and 3), Argon2's H, and its
  check at start against BLAKE2b-512("abc") (Appendix A) and the
  self-test's hash of hashes (Appendix E); keyed BLAKE2b (3.3) for the
  network service's initial sequence numbers and ephemeral ports
  ([network.md](network.md) N9).

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
up to M1 are test inputs, fetched by hash for the chain disk
([build.sh](../../build.sh)); the sources were read only to
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

**UTF-8, a transformation format of ISO 10646**, RFC 3629, November 2003
(IETF).

- Document: <https://www.rfc-editor.org/rfc/rfc3629.txt>
- Access: free.
- sha256: `a2a3a39457d30420c812f87a38cc2b9194832f6982ef17e619de7d816879d6a6`
- Used by: names in the namespace, the store and a pool's directories
  ([store-image.md](store-image.md), [pool-format.md](pool-format.md);
  `lib/name.c` and `tools/mkstore.c`, section 4); text layout
  ([ui.md](ui.md) U4).

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
  [storage.md](storage.md), for when a block write is stable (5.2.6.2);
  [display.md](display.md), for a pointing device's reports and its
  axes' ranges (5.8.4, 5.8.6.2), and the GPU device in 2D (5.7);
  [network.md](network.md), for the network device (5.1), message
  framing (2.7.4) and the used ring (2.7.8).

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
- Used by: [input.md](input.md): the event types `EV_SYN` (0),
  `EV_KEY` (1), `EV_REL` (2) and `EV_ABS` (3) and the last, `EV_MAX`
  (0x1f), the codes `SYN_REPORT` and `SYN_DROPPED`, `ABS_X`, `ABS_Y`,
  `REL_X`, `REL_Y` and `REL_WHEEL`, and the key codes (the header); a
  key event's values, 0 released, 1 pressed and 2 repeated (the
  documentation, `EV_KEY`), what a report and a dropped report are
  (`EV_SYN`), and the wheel's detents (`EV_REL`).
  [console.md](console.md): the names `BTN_LEFT` to `BTN_TASK` (the
  header), and a report ending each motion (`EV_SYN`).
  [display.md](display.md): the
  types `EV_SYN` (0), `EV_REL` (2) and `EV_ABS` (3), `SYN_REPORT` (0)
  and `SYN_DROPPED` (3), `ABS_X` and `ABS_Y`, `REL_X`, `REL_Y` and
  `REL_WHEEL`, the buttons from `BTN_LEFT` (0x110) and the keys of the
  secure attention key (the header); what a report and a dropped report
  are (`EV_SYN`), and the high-resolution wheel's 120ths of a detent
  (`EV_REL`) (the documentation).

**QEMU**, release 11.1.3, tag `v11.1.3`, commit
`005c61c0ef6f6ae615283440b4bb7be51b2c6ddd`, and release 8.2.2, tag
`v8.2.2`, commit `11aa0b1ff115b86160c4d37e7c37e6a6b13b77ea`, the
upstream release of the QEMU that CI installs from Ubuntu 24.04
(`1:8.2.2+ds-0ubuntu1.18`, whose own patches were not read).

| File | Tag | sha256 |
|---|---|---|
| [`hw/input/virtio-input-hid.c`][qemu-hid] | `v11.1.3` | `aae90a22fda46a0a61939cf3f4e56f93bf751215dffb3d2ba643b7f09a3693f9` |
| [`include/ui/input.h`][qemu-input] | `v11.1.3` | `7ebddaca72c431b3fd91fd4b96fb5c23d2bdd99b416da655625339b8d515426f` |
| [`ui/input.c`][qemu-input-c] | `v11.1.3` | `619089241c78af002e296ce54627ae2a30cb3421dc8684e8d879471e3af43b83` |
| [`qapi/ui.json`][qemu-qapi] | `v11.1.3` | `87ac10949caa0da383eaba3d0676f078915bed9ea96025a6c7fb1c08135548ba` |
| [`hw/input/virtio-input-hid.c`][qemu-hid-8] | `v8.2.2` | `ed8537556bba0b21814c010bd6b73b1b9338d807bd3592e6a37576b53b9b14ee` |
| [`hw/input/virtio-input.c`][qemu-virtio-input] | `v11.1.3` | `0c3cecb7be4b5a115358643ca95788fa111d17efe41ecb3c0124bf73c7d4aad5` |
| [`hw/input/virtio-input.c`][qemu-virtio-input-8] | `v8.2.2` | `ccb9cb16a9e180a9edede42b534dca80131dcdea317cb04d9db6e555ee4355d4` |
| [`ui/input.c`][qemu-input-c-8] | `v8.2.2` | `e67241a5d54df6b19397f31172b0143ea0d5590676c6d6bd5e11e458f7d5e1f4` |
| [`include/ui/input.h`][qemu-input-8] | `v8.2.2` | `69efe9f954bf6937994eead227ce21b9ead6dd90b0ea610eace70849e8b3c4f4` |
| [`ui/input-legacy.c`][qemu-input-legacy-8] | `v8.2.2` | `9b1e7e74ffe473d07d25079edcf9668c7c85e61f34b853d4a06fdd307171fd56` |

[qemu-hid]: https://gitlab.com/qemu-project/qemu/-/raw/v11.1.3/hw/input/virtio-input-hid.c
[qemu-input]: https://gitlab.com/qemu-project/qemu/-/raw/v11.1.3/include/ui/input.h
[qemu-input-c]: https://gitlab.com/qemu-project/qemu/-/raw/v11.1.3/ui/input.c
[qemu-qapi]: https://gitlab.com/qemu-project/qemu/-/raw/v11.1.3/qapi/ui.json
[qemu-hid-8]: https://gitlab.com/qemu-project/qemu/-/raw/v8.2.2/hw/input/virtio-input-hid.c
[qemu-virtio-input]: https://gitlab.com/qemu-project/qemu/-/raw/v11.1.3/hw/input/virtio-input.c
[qemu-virtio-input-8]: https://gitlab.com/qemu-project/qemu/-/raw/v8.2.2/hw/input/virtio-input.c
[qemu-input-c-8]: https://gitlab.com/qemu-project/qemu/-/raw/v8.2.2/ui/input.c
[qemu-input-8]: https://gitlab.com/qemu-project/qemu/-/raw/v8.2.2/include/ui/input.h
[qemu-input-legacy-8]: https://gitlab.com/qemu-project/qemu/-/raw/v8.2.2/ui/input-legacy.c

- Access: free. `virtio-input-hid.c` is under the GPL, version 2 or
  later; [display.md](display.md), [console.md](console.md) and
  `ci/boot.py` cite what the files declare and copy none of their code.
- Used by: [display.md](display.md): the virtio tablet's axes, their
  range of 0 to `0x7FFF`, its buttons and wheel, and its reports
  (`virtio-input-hid.c`, `input.h`), the same in 8.2.2 while the
  tablet's `wheel-axis` property is on, its default; when QEMU counts
  the pointer absolute (`input.c`); QMP's `screendump` and
  `input-send-event` (`ui.json`). [console.md](console.md): the wheel
  turned up sent as `REL_WHEEL` 1 (`virtio-input-hid.c`). `ci/boot.py`:
  the tablet's range (`input.h`), the one `SYN_REPORT` that ends each
  `input-send-event` and the device a button is sent to (`input.c`),
  the one whose driver set `DRIVER_OK` last (`virtio-input.c`), the
  wheel's turn sent as `REL_WHEEL` 1 or -1 (`virtio-input-hid.c`), and
  the 1,024 key events, syncs and delays `send-key` queues, past which
  it drops a key (`input.c`), each the same in 8.2.2, whose `send-key`
  is in `input-legacy.c`.

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

## Networking

**Requests for Comments** (RFC Editor), each the plain-text copy at
`https://www.rfc-editor.org/rfc/rfcN.txt`, fetched on November 7, 2026.

| RFC | Title | Date | sha256 |
|---|---|---|---|
| 768 | User Datagram Protocol | August 1980 | `7dc8880e1ecef9c3f9da0db4b876a16e96bfa4f0953cc9d977d414f8f680c2f0` |
| 791 | Internet Protocol | September 1981 | `6cfb387fcecfc1b72f2f69343c5b6951b5d263d708162e2b5c66ab8f394f6265` |
| 792 | Internet Control Message Protocol | September 1981 | `58714393ded142bacf188d7e8977eef98f4110c4c87ac94595f750df5664c2c6` |
| 826 | An Ethernet Address Resolution Protocol | November 1982 | `01bc62fe6a37e90f1246ac43e8e145f1322b4ed1474836145c3da93d2bd3c8a6` |
| 894 | A Standard for the Transmission of IP Datagrams over Ethernet Networks | April 1984 | `be88b9301e53f986aca3a0e55e488d1d79bae3f88fe3f257640397bc089e7035` |
| 1034 | Domain Names - Concepts and Facilities | November 1987 | `d6b10a71441df879cc2817d23f2dad120c8a5f87e74eba1e4ed4b743a76a891a` |
| 1035 | Domain Names - Implementation and Specification | November 1987 | `d14ae809fc9b41bbcae26bb38c937c9515808b944f3252b00de9fe0e95f4fdfb` |
| 1122 | Requirements for Internet Hosts -- Communication Layers | October 1989 | `9f526e6bebc868324fedb90aebbcf6e5b15c53fd373ca5d5ce1c2cdcd264e04f` |
| 1123 | Requirements for Internet Hosts -- Application and Support | October 1989 | `3d019cc4777d1ead76c212711d56768bde9361312cd209846b62d2e41dd0301d` |
| 2131 | Dynamic Host Configuration Protocol | March 1997 | `a043b705785b81762505ded4cf71d61392b2b1da2b4ce1e32c13bc984de5f4a5` |
| 2132 | DHCP Options and BOOTP Vendor Extensions | March 1997 | `0cfbedab7cfe859624ae78c07a722bc51bf8ea123a7b43b099fc546ddcf00b90` |
| 4443 | Internet Control Message Protocol (ICMPv6) for the Internet Protocol Version 6 (IPv6) Specification | March 2006 | `f20d1de8878e1142000bfeaa3ad1b0540fab7653a395cf7b07e81d2da2f650bf` |
| 4861 | Neighbor Discovery for IP version 6 (IPv6) | September 2007 | `1a4309c117d765a7c0edfcb2297fc90c3c09f0fcae255f15e53b20748bcc09fb` |
| 4862 | IPv6 Stateless Address Autoconfiguration | September 2007 | `6d3d2af5d2f6c9109b4ebd038ff7a6cec56771bbb0b5d8bd520357e03f097aa2` |
| 4987 | TCP SYN Flooding Attacks and Common Mitigations | August 2007 | `cde4ce998a6c9e34580c27fa5cafdbc4f9b90017b07e0768217bab1b83273d61` |
| 5452 | Measures for Making DNS More Resilient against Forged Answers | January 2009 | `53af0365af1c491204ae27b523699b7732f79cf606ce84fab3b032df7a966c6b` |
| 5681 | TCP Congestion Control | September 2009 | `a2d99a2421d5c57b248394f26ba44fc364aa546680fbf10ff0aa7034dad8b87d` |
| 5927 | ICMP Attacks against TCP | July 2010 | `b90bd4a3dbd1dcfb86bf4369a39cb0d50b08bca82d8ecadf0294a63c299c751d` |
| 5961 | Improving TCP's Robustness to Blind In-Window Attacks | August 2010 | `bba04c91e91f6aa7bd49b491f127bfb3c1280dd22b52f15299d8123f62aab502` |
| 6056 | Recommendations for Transport-Protocol Port Randomization | January 2011 | `2d899269f777944a49b7bcd5373a53284b4b6425a5e957bee90959e976ad26bb` |
| 6298 | Computing TCP's Retransmission Timer | June 2011 | `f3a6d937c0a7653bd431fdbbdcd7920f4e9d9cdb6134fa9db7a74b90d215edf6` |
| 6528 | Defending against Sequence Number Attacks | February 2012 | `1de02139d839860eb49ea553acf75e16b93a6326e4b0eda1ef0daa56433b89da` |
| 6761 | Special-Use Domain Names | February 2013 | `549f6635f34eb0da627568657401f0115e8e33c2ba1ba7e37bf14ab27651339e` |
| 7766 | DNS Transport over TCP - Implementation Requirements | March 2016 | `9fbf70d43e5c423c2464eafd724ef164490b4d8e341e5d9b5fe1023402bb404f` |
| 8200 | Internet Protocol, Version 6 (IPv6) Specification | July 2017 | `371ae3f133d562db5d6385e6def4ca9914c4f831be228ea7779fd28799c2f490` |
| 8305 | Happy Eyeballs Version 2: Better Connectivity Using Concurrency | December 2017 | `9bb65e288b32acae072d1bf403c609f71c32ea7a5087d0056ee9af3cedca47e1` |
| 8504 | IPv6 Node Requirements | January 2019 | `11622564c8042970ea25dfb0f2a214d4a38c39d64d13395467b80e91bc379f19` |
| 9293 | Transmission Control Protocol (TCP) | August 2022 | `6d9ac8be4b0286f8c3d337addf442b2eb6a9b14e1366594ea7fbc273f93dc2d9` |

- Access: free.
- Used by: [network.md](network.md), which cites each by section, or by
  heading for RFC 768, 792, 826 and 894, which have no numbered
  sections; RFC 4443, 4861, 4862, 8200, 8305 and 8504 for IPv6 only
  (network.md N8).

## Graphics

**How to interpret the sRGB color space (specified in IEC 61966-2-1) for
ICC profiles** (International Color Consortium), April 2015.

- Document: <https://registry.color.org/rgb-registry/files/sRGB.pdf>,
  listed at <https://registry.color.org/rgb-registry/srgb>
- Access: free download.
- sha256: `ceed855c29808217a472954abb04cdc60a5bd3200387a93f5c5ffcef8ddf9f1f`
- Used by: `gfx/raster.c`, for sRGB's transfer function: the encoding in
  A.8 and the decoding in B.1, whose red line is the decoding; its green
  and blue lines repeat the red channel's variable, and its linear blue
  line misprints 12.92 as 12.02.
- IEC 61966-2-1, which defines the transfer function, is sold by the IEC
  and not pinned.

**OpenType specification**, version 1.9.1 (Microsoft Learn), each page
pinned as fetched on November 7, 2026, with the date it shows as its
last update ("OpenType").

| Page | Updated | sha256 |
|---|---|---|
| [TrueType fundamentals][ot-ttch01] | 2024-05-30 | `eb5e56fe9ee721d74558f011fd84c12744ba2c3f1285e796fe3f2618db0b624a` |
| [glyf - Glyf data table][ot-glyf] | 2024-05-31 | `c161f9f31207b1c7763541ef4fa44a432c5ed447ea8e4ddc40eed3d464fcf8d4` |
| [CFF2 - Compact font format version 2 table][ot-cff2] | 2024-10-09 | `071673bea4bdf17a76219bd842b26c3cf2a2e0b47dde46da053d79986a9dd07f` |
| [GPOS, the Glyph Positioning Table][ot-gpos] | 2024-05-29 | `34ecfdf76932cc64d02b1f12e473b2400b087af9c6e0b4db22b5688dbc93d83a` |
| [OpenType layout common table formats][ot-common] | 2024-07-06 | `e0c3cc92012156a99c1e49c19c7444b2ef0acc9482bfe1a9947c73a14295d635` |
| [cmap - Character To Glyph Index Mapping Table][ot-cmap] | 2024-05-29 | `6c99893737f661dc7c7d339e882e22a160eb1ee4d53f05c7a2e289afab4e838a` |
| [hmtx - Horizontal metrics table][ot-hmtx] | 2023-01-10 | `667ced0913a7b8855b75af10ba40887f69967f156b3041a4123d166b0ae7c3aa` |
| [OS/2 - OS/2 and Windows metrics table][ot-os2] | 2024-05-31 | `32085ba221973bc029e60f40c0c153b5003a45f3f60f9a8514df209865316281` |
| [OpenType font file][ot-otff] | 2024-05-29 | `efda208b68f88620491834411e0459b2f1fa93cc4a199c1c732924928e3b625d` |
| [head - Font header table][ot-head] | 2024-05-29 | `3533ad42b52667246f6c657d4b2d238f6c448a42bb9003b14c8772bc8784a370` |
| [hhea - Horizontal header table][ot-hhea] | 2023-01-10 | `d5082b84f0e5df86ed1e3f3e8ce43d4dfbdb95751a0aea7def1ebf5ae3b53bb6` |
| [maxp - Maximum profile table][ot-maxp] | 2021-10-26 | `1df9197a4c56aa664cd9cd7e2c54619dfbb06da15e2769b5a97bbd22c3e3f6cc` |
| [loca - Index to location][ot-loca] | 2024-03-15 | `2de02d6928b4a5171ffd1f572f5ba93b2fde517b25b82723ac640bfc92fb6465` |
| [GDEF - Glyph definition table][ot-gdef] | 2024-05-29 | `2cc0896d6b47675f1ce482a988b82cdfe3a8c232098cc90e54dabf868c146a7b` |

[ot-ttch01]: https://learn.microsoft.com/en-us/typography/opentype/spec/ttch01
[ot-glyf]: https://learn.microsoft.com/en-us/typography/opentype/spec/glyf
[ot-cff2]: https://learn.microsoft.com/en-us/typography/opentype/spec/cff2
[ot-gpos]: https://learn.microsoft.com/en-us/typography/opentype/spec/gpos
[ot-common]: https://learn.microsoft.com/en-us/typography/opentype/spec/chapter2
[ot-cmap]: https://learn.microsoft.com/en-us/typography/opentype/spec/cmap
[ot-hmtx]: https://learn.microsoft.com/en-us/typography/opentype/spec/hmtx
[ot-os2]: https://learn.microsoft.com/en-us/typography/opentype/spec/os2
[ot-otff]: https://learn.microsoft.com/en-us/typography/opentype/spec/otff
[ot-head]: https://learn.microsoft.com/en-us/typography/opentype/spec/head
[ot-hhea]: https://learn.microsoft.com/en-us/typography/opentype/spec/hhea
[ot-maxp]: https://learn.microsoft.com/en-us/typography/opentype/spec/maxp
[ot-loca]: https://learn.microsoft.com/en-us/typography/opentype/spec/loca
[ot-gdef]: https://learn.microsoft.com/en-us/typography/opentype/spec/gdef

- Access: free web pages.
- Used by: [ui.md](ui.md): TrueType's quadratic outlines and nonzero
  fill (TrueType fundamentals), composite glyphs (`glyf`), cubic
  outlines (`CFF2`), pair adjustment and mark attachment (`GPOS`), the
  order of lookups (common table formats), glyph 0 for a missing
  character (`cmap`), advances (`hmtx`), line metrics (`OS/2`), and the
  converter (`tools/mkfont.c`); [font-format.md](font-format.md) and
  the converter: the table directory (OpenType font file), `head`,
  `hhea`, `maxp`, `loca` and `glyf` for the outlines, `GDEF`'s glyph
  classes, and the tables above.

**TrueType Reference Manual** (Apple Developer), which shows no version
or date ("Apple TrueType"), fetched on November 7, 2026.

| Chapter | sha256 |
|---|---|
| [1. Digitizing Letterform Designs][apple-rm01] | `0d649be52c2206b40cff6a1d9445df783050d8de39f383aba7dd8f7587eb968c` |
| [2. Font Engine][apple-rm02] | `3ddadcbabef15ad04f1d53e8427124bf011b72f6f0bb847d706eb554700dceb7` |

[apple-rm01]: https://developer.apple.com/fonts/TrueType-Reference-Manual/RM01/Chap1.html
[apple-rm02]: https://developer.apple.com/fonts/TrueType-Reference-Manual/RM02/Chap2.html

- Access: free web pages.
- Used by: [ui.md](ui.md): the nonzero winding rule over overlapping
  contours (chapter 1) and the instruction interpreter that hinting
  needs (chapter 2).

**Inter**, release 4.1, tag `v4.1`, commit
`e3a3d4c57d5ecc01453a575621882a384c1995a3` (The Inter Project
Authors).

- Documents: <https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip>,
  and at the commit
  <https://raw.githubusercontent.com/rsms/inter/e3a3d4c57d5ecc01453a575621882a384c1995a3/LICENSE.txt>
  and
  <https://raw.githubusercontent.com/rsms/inter/e3a3d4c57d5ecc01453a575621882a384c1995a3/Makefile>
- Access: free; the fonts under the SIL Open Font License 1.1, with no
  Reserved Font Name. GitHub publishes no digest of the release's zip,
  so its sha256 is of the copy fetched on November 7, 2026.
- sha256: `9883fdd4a49d4fb66bd8177ba6625ef9a64aa45899767dde3d36aa425756b11e`
  (the zip),
  `262481e844521b326f5ecd053e59b98c8b2da78c8ee1bdbb6e8174305e54935a`
  (`LICENSE.txt`, the same bytes as the zip's) and
  `90cb33bbccb86be751f26760cca86157f86c8a3702add53d60b5f95eacf6326a`
  (`Makefile`)
- Used by: [ui.md](ui.md), the interface's typeface: the static
  TrueType fonts in `extras/ttf/`, whose regular weight,
  `Inter-Regular.ttf`, is 411,640 bytes with sha256
  `40d692fce188e4471e2b3cba937be967878f631ad3ebbbdcd587687c7ebe0c82`;
  their hinting (`Makefile`). Its tables, coverage and kerning were
  measured with fontTools 4.68.0. [build.sh](../../build.sh) fetches
  the zip, whose sha256 `SHA256SUMS` gives, and converts six of its
  static fonts ([font-format.md](font-format.md)).

**SIL Open Font License**, version 1.1, February 26, 2007 ("OFL"), and
**OFL-FAQ**, version 1.1-update7, November 2023 ("OFL FAQ").

- Documents: <https://openfontlicense.org/documents/OFL.txt>,
  <https://openfontlicense.org/ofl-faq/>
- Access: free; the FAQ page shows "Last updated on 12 January 2026".
- sha256: `1d361a8f8e8ce6e68457dcd93fb56e162e6baa3bbb7e7573a290d44399f6b57e`
  (the licence) and
  `ef6f04f954db9536156a0a1586b975cf3f51964f4dd04f8d9c3f66b6a1089827`
  (the FAQ)
- Used by: [ui.md](ui.md) and [font-format.md](font-format.md): a
  converted font as a Modified Version, the Reserved Font Name, and what
  must accompany the font.

**On slight hinting, proper text rendering, stem darkening and LCD
filters**, Nikolaus Waxweiler, November 30, 2015, updated December 8,
2015, and **FreeType Glyph Conventions**, section VI, FreeType Outlines,
last updated October 23, 2022 (the FreeType project) ("FreeType").

- Documents: <https://freetype.org/freetype2/docs/hinting/text-rendering-general.html>,
  <https://freetype.org/freetype2/docs/glyphs/glyphs-6.html>
- Access: free web pages.
- sha256: `ef42c819889f2fa1b4c2e08a0038e80a83d3a9ce0e434401f7b25d47da010ec0`
  and `eae85382a238278ed2a1002a049f09d3f59b59c4e9d112e908221506c92c44bc`
- Used by: [ui.md](ui.md): blending coverage in linear light, the
  thinning it causes and stem darkening; vertical-only snapping;
  implied on-curve points (Glyph Conventions, 1.a), as
  [font-format.md](font-format.md) and `lib/typeface.c` read them.

**Unicode Standard Annex #9: Unicode Bidirectional Algorithm**,
revision 52, for Unicode 18.0.0, September 1, 2026 ("UAX #9").

- Document: <https://www.unicode.org/reports/tr9/tr9-52.html>
- Access: free web page.
- sha256: `3c281b38800bad85b16bf3a169b8cb693d8a2dbceb60903feb26e68a2659ee8a`
- Used by: [ui.md](ui.md), for what left-to-right layout leaves out:
  right-to-left text (1) and shaping (3.5).

**Approximate a circle with cubic Bezier curves**, Spencer Mortensen,
web page, fetched on November 7, 2026, which shows no date.

- Document: <https://spencermortensen.com/articles/bezier-circle/>
- Access: free web page.
- sha256: `b7e8894026c882ebbffa3018eb6c495a2f247c37ba1de3c3b0d5806080c948d4`
- Used by: `lib/outline.c`, for the control points of a quarter circle,
  its standard approximation, and that approximation's largest radial
  drift.

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

## Test reports

**The Kernel Test Anything Protocol (KTAP), version 1**, Linux, tag
`v7.3`, commit `c03c1a14507dfaf8b856c2608f9ac216021fc222`:
`Documentation/dev-tools/ktap.rst`, which
<https://docs.kernel.org/dev-tools/ktap.html> renders.

- Document: <https://raw.githubusercontent.com/torvalds/linux/v7.3/Documentation/dev-tools/ktap.rst>
- Access: free. The document is GPL-2.0; [log.md](log.md) cites it and
  copies none of its text.
- sha256: `b26fa893201bff9b1e20565192ba636f983d9b79e47d957979830fc78a745b39`
- Used by: [log.md](log.md) T2, for the version line, plans, result
  lines and the `SKIP` directive, diagnostic lines, unknown lines and
  nested tests indented two spaces a level; `ci/boot.py`, which reads
  the report.

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

## Shell design references

Specifications, papers and documentation [shell.md](shell.md) cites, by
the names in its first table. Web pages were fetched on November 7,
2026; a page that shows no version or date is identified by its sha256
alone, and a later copy may differ.

**Shell Command Language**, chapter 2 of the Shell and Utilities volume,
The Open Group Base Specifications Issue 8, IEEE Std 1003.1-2024 ("POSIX").

- Document: <https://pubs.opengroup.org/onlinepubs/9799919799/utilities/V3_chap02.html>
- Access: free web page.
- sha256: `0889867a53fe12508a71e8739ec23ecdddc95e6f67697c98e26acc8ff0ec1acd`

**Rc - The Plan 9 Shell**, Tom Duff, Bell Laboratories, as distributed
with Plan 9 ("rc"), and **The Use of Name Spaces in Plan 9**, Rob Pike,
Dave Presotto, Ken Thompson, Howard Trickey and Phil Winterbottom
("Plan 9 names").

- Documents: <https://9p.io/sys/doc/rc.html>, <https://9p.io/sys/doc/names.html>
- Access: free web pages.
- sha256: `ac8f53a72a03991525c5d7fdc7ceb6b91cf2c0b420b5abc7cd8bd256a2dee779`
  (rc) and `6894b37413ce7735ae0cdfaa87c24ecfd6cc0490c0bfa864a77cda4c4ba68645`
  (Plan 9 names)

**Windows PowerShell Language Specification 3.0** (Microsoft Learn),
published in December 2012 and kept, in its own words, "for historical
reference"; the Word original is at
<https://www.microsoft.com/download/details.aspx?id=36389> ("PowerShell").

| Chapter | Updated | sha256 |
|---|---|---|
| [1. Introduction][ps-1] | 2025-01-08 | `5256ec44cf1ad0b7a3ff9a34445621ae1dbfddab4e03b6467cd8a16bbc4e62f2` |
| [8. Statements][ps-8] | 2022-05-16 | `dbf0600401ea1434e564561e43f7c574f354c176c6ae06cd6736dd393d9280fa` |
| [13. Cmdlets][ps-13] | 2021-05-19 | `26a9bd7bfb8872666081b99349684dd23ecd2a86940b62edbf56e9f3d0ba332d` |

- Access: free web pages.

**The Nushell book** ("Nushell"), which shows no version; Nushell's
latest release was 0.116.1.

| Page | sha256 |
|---|---|
| [Pipelines][nu-pipelines] | `0aaeec50ec5b091b851a3e36fa53c3c79e9b2d5443474d170bd76a62b58abbce` |
| [Types of Data][nu-types] | `713368745a7e0183f7d7b3e1f0256058cdf7b91f5a35a908b35045f76cec6942` |
| [Variables][nu-variables] | `5971fa2af5274fb07ab20c86fc5d00128ea8a86d9bef00c4adf9ab07ea19e5ce` |
| [Creating Your Own Errors][nu-errors] | `3e601dc673c7d667115cee433f74fa2f84e53baf243289306cc27f50737364ae` |
| [Running System (External) Commands][nu-externals] | `e9969e9141134bc9d24a091f8c478801679331fafb6a9d22d68e1c6ad46d70f0` |

- Access: free web pages.

**fish-shell 4.9.3 documentation** ("fish").

| Page | sha256 |
|---|---|
| [Design][fish-design] | `3484e82b05f889a1d8ae0f898222a57815928eadaabe84acf5087c3597452c70` |
| [The fish language][fish-language] | `1eca2aefd4a2d98daa531fda43732ba9995ecb32421b80adb40272367d0a6cb1` |
| [Interactive use][fish-interactive] | `45a6250f6f42aba4e4bb93f22cd00949cc9d69e7325c7ca2b57011f180635154` |

- Access: free web pages.

**Oils 0.38.0 documentation** ("Oils"), and **Oils Is Exterior-First
(Code, Text, and Structured Data)**, Andy Chu, the Oils blog, June 20,
2023 ("Chu 2023").

| Document | sha256 |
|---|---|
| [Simple Word Evaluation in Unix Shell][oils-words] | `a36c86ccdbafc23ece315d408eec4d40025a3ce4a0b11da54004e2ad1155f977` |
| [YSH Fixes Shell's Error Handling (errexit)][oils-errors] ("Error Handling") | `cd5cb5489d04b9d469f2c0cc22debc0477269f3b06e55efb78bb22b25b172c9f` |
| [Variable Declaration, Mutation, and Scope][oils-variables] ("Variables") | `538ba7a6c4a924d59d73edb5c356002ad6db133f8b668f2f1eb7e1a245424ba0` |
| [Oils Is Exterior-First][chu-2023] | `5cc93a54f66a4cd50e03f4e68c6f66ae59b50e9cffce47bf32252ecf76332ea1` |

- Access: free web pages.

**Elvish documentation** ("Elvish"), which shows no version; Elvish's
latest release was 0.21.0.

| Page | sha256 |
|---|---|
| [Language specification][elvish-language] | `d98808eaccb76d51b493f94e6f0c408328169f2fcd3343c92b9acc6c13969f65` |
| [Unique semantics][elvish-unique] | `9fe443b9bc3fa1af921dba01528218aebe39b69ff644103f391441a28f361fce` |

- Access: free web pages.

**Plash documentation**, Mark Seaborn ("Plash"): the web site's sources
in the author's repository at commit
`1d77bb2fec3d9cdeb3547c42682818c447492bf4` (December 30, 2009). The
site, <http://plash.beasts.org/>, redirects to a page that is not
found, and the Internet Archive refused requests with HTTP 429, so the
sources are pinned instead.

| Page | File | sha256 |
|---|---|---|
| Introduction | [`plash/web-site/index.xxml`][plash-index] | `97c2f0ea183bc31732aae8cae8731157c0fbf28803879a0c36f78a7026b1e576` |
| The Plash shell | [`plash/web-site/pola-shell.xxml`][plash-shell] | `beea4e135d6fdbd44f2d14eae03b6e20778c38799fd7a459c85f885d063823b0` |
| Powerboxes | [`plash/web-site/powerbox.xxml`][plash-powerbox] | `f16190768586fdad4e0f9e9cbc9e567b3690bc79ab9287eec70c787a714f3721` |

- Access: open source; its `COPYRIGHT` file gives the GNU Lesser General
  Public License, version 2.1 or later. shell.md quotes the pages and
  copies no code.

**Capsicum: practical capabilities for UNIX**, Robert N. M. Watson,
Jonathan Anderson, Ben Laurie and Kris Kennaway, USENIX Security 2010
("Capsicum").

- Document: <https://www.usenix.org/legacy/event/sec10/tech/full_papers/Watson.pdf>,
  USENIX's copy, 17 pages without printed page numbers, cited by section
  and PDF page.
- Access: free download.
- sha256: `7ff6a2cf650b15ad852594578847b14f6ecdf08cdedabdb1a03566a4eab4c2ab`

**Nix 2.35.4 Reference Manual** ("Nix").

| Page | sha256 |
|---|---|
| [`nix run`][nix-run] | `e2673ce35cdbfb53c3395c46a0443e5a0851ce9f9f19e0e501d6a92ca74cb6c2` |
| [`nix env shell`][nix-env-shell] | `5a11ba24651b02c3688720a8ed1a6344de474e0dcf91f31766fbea34723052e0` |
| [`nix-shell`][nix-shell] | `edc3e3f7e78eb251c3fac088603ed1557c38d9ed0333b473a58c2f23d23154c2` |

- Access: free web pages.

**General Decimal Arithmetic Specification**, version 1.70, April 7,
2009, Mike Cowlishaw, IBM ("Decimal").

- Document: <https://speleotrove.com/decimal/decarith.pdf>
- Access: free download, "Reproduced with permission from IBM".
- sha256: `d279ab7ce8b3533188e81eb7db560df1b28c3de623170d8533b9d370ca4d76f0`

**Concise Binary Object Representation (CBOR)**, RFC 8949, STD 94,
December 2020 (IETF) ("CBOR").

- Document: <https://www.rfc-editor.org/rfc/rfc8949.txt>
- Access: free.
- sha256: `f1164a5b31a39350ad46abe29b83575eb933ca6c45366989c118b6b1058a214a`

**Clearing the screen** ("Clear"): the pages that give `clear`, `cls`
and Ctrl+L in other shells, with the version or date each shows.

| Page | Version or date | sha256 |
|---|---|---|
| [clear(1)][ncurses-clear], ncurses | 6.6, 2025-04-05 | `8b7cf93b98e16c8939eca8f73529b7e37a5d8f11ed2ba2ddd55cfa829d724877` |
| [clear][nu-clear], Nushell | none shown | `c22ab158fa8b5524c1fd47fbc9dc03fa9ef9ce073860695ffbf032be657b6db3` |
| [Clear-Host][ps-clear-host] (Microsoft Learn) | 2025-04-29, its `ms.date` | `9c54ba1752fb60892d2fa4106118e408a87cb6003b2a18f57e9b9b12284eae07` |
| [about_PSReadLine_Functions][ps-psreadline] (Microsoft Learn) | Updated 2026-01-18 | `8fc19e6993cb2e39a4f3e09c91b09a324c82ba49444cc1fc0836b480813a73f3` |
| [cls][ms-cls] (Microsoft Learn) | Updated 2023-02-03 | `d90bfefb2339462ef5dcb008ffa0d3e8ecb89afddcf8041b9b9377fa4f592dc1` |
| [Commands For Moving][bash-moving], Bash Reference Manual | Bash 5.3, 18 May 2025 | `3a2d2c0aa2fad6dadced1f7085ec85e8ae00fad963696afa3384c67e9f0d2fec` |
| [Zsh Line Editor][zsh-zle], zsh manual | zsh 5.9.2 | `63211d3d709360f661014e70598718687efac0420f72cacf98fcbc7677510bf1` |
| [`src/edit_mode/keybindings.rs`][reedline-keys], reedline | Tag `v0.52.1`, commit `b72fd34f54abb15ff50886604a82c22f95642a78` | `a2b613280057fe0512ab3c9b142079f8fb09ed2651ec46841e352d8983c109d8` |

- Access: free web pages; reedline is open source, under the MIT
  licence, and shell.md cites one line of it and copies none.

- Used by, for every entry in this section: [shell.md](shell.md).

[ps-1]: https://learn.microsoft.com/en-us/powershell/scripting/lang-spec/chapter-01
[ps-8]: https://learn.microsoft.com/en-us/powershell/scripting/lang-spec/chapter-08
[ps-13]: https://learn.microsoft.com/en-us/powershell/scripting/lang-spec/chapter-13
[nu-pipelines]: https://www.nushell.sh/book/pipelines.html
[nu-types]: https://www.nushell.sh/book/types_of_data.html
[nu-variables]: https://www.nushell.sh/book/variables.html
[nu-errors]: https://www.nushell.sh/book/creating_errors.html
[nu-externals]: https://www.nushell.sh/book/running_externals.html
[fish-design]: https://fishshell.com/docs/current/design.html
[fish-language]: https://fishshell.com/docs/current/language.html
[fish-interactive]: https://fishshell.com/docs/current/interactive.html
[oils-words]: https://oils.pub/release/latest/doc/simple-word-eval.html
[oils-errors]: https://oils.pub/release/latest/doc/error-handling.html
[oils-variables]: https://oils.pub/release/latest/doc/variables.html
[chu-2023]: https://www.oilshell.org/blog/2023/06/ysh-design.html
[elvish-language]: https://elv.sh/ref/language.html
[elvish-unique]: https://elv.sh/learn/unique-semantics.html
[plash-index]: https://github.com/mseaborn/plash/blob/1d77bb2fec3d9cdeb3547c42682818c447492bf4/plash/web-site/index.xxml
[plash-shell]: https://github.com/mseaborn/plash/blob/1d77bb2fec3d9cdeb3547c42682818c447492bf4/plash/web-site/pola-shell.xxml
[plash-powerbox]: https://github.com/mseaborn/plash/blob/1d77bb2fec3d9cdeb3547c42682818c447492bf4/plash/web-site/powerbox.xxml
[nix-run]: https://nix.dev/manual/nix/2.35/command-ref/new-cli/nix3-run.html
[nix-env-shell]: https://nix.dev/manual/nix/2.35/command-ref/new-cli/nix3-env-shell.html
[nix-shell]: https://nix.dev/manual/nix/2.35/command-ref/nix-shell.html
[ncurses-clear]: https://invisible-island.net/ncurses/man/clear.1.html
[nu-clear]: https://www.nushell.sh/commands/docs/clear.html
[ps-clear-host]: https://learn.microsoft.com/en-us/powershell/module/microsoft.powershell.core/clear-host
[ps-psreadline]: https://learn.microsoft.com/en-us/powershell/module/psreadline/about/about_psreadline_functions
[ms-cls]: https://learn.microsoft.com/en-us/windows-server/administration/windows-commands/cls
[bash-moving]: https://www.gnu.org/software/bash/manual/html_node/Commands-For-Moving.html
[zsh-zle]: https://zsh.sourceforge.io/Doc/Release/Zsh-Line-Editor.html
[reedline-keys]: https://github.com/nushell/reedline/blob/b72fd34f54abb15ff50886604a82c22f95642a78/src/edit_mode/keybindings.rs

## Display and interface design references

Systems, papers and standards [display.md](display.md),
[ui.md](ui.md) and [look.md](look.md) compare nova-os with, cited by
the name in each heading.
Web pages were fetched on November 7, 2026; a page with no version is
identified by its date and sha256, and a later copy may differ.

**nitpicker**: Norman Feske and Christian Helmuth, A Nitpicker's guide
to a minimal-complexity secure GUI, Proceedings of ACSAC 2005, as
Genode's publications page gives the venue; the PDF names none.

- Document: <https://genode-labs.com/publications/nitpicker-secure-gui-2005.pdf>,
  listed at <https://genode.org/about/publications>
- Access: free download; no terms stated. Its pages are cited by the
  PDF's page index, since it prints none.
- sha256: `d7cc038bdcd99aa9c7b3b2d665ed941de08e13bffacb233a34e465a5e97eb5e6`

**EWS**: Jonathan S. Shapiro, John Vanderburgh, Eric Northup and David
Chizmadia, Design of the EROS Trusted Window System, Proceedings of the
13th USENIX Security Symposium, August 2004.

- Document: <https://www.usenix.org/legacy/event/sec04/tech/full_papers/shapiro/shapiro.pdf>
- Access: free download; USENIX permits "noncommercial reproduction of
  the work for educational or research purposes".
- sha256: `18ab072a47846cf58fb2758898779b9e84c07b3b5b41e997ead6f5d710b6704a`

**Qubes**: GUI virtualization, the Qubes OS documentation's
`developer/system/gui.rst` at commit
`f7daea25d36a41e7c2b11fce2196ac7592287cfb`.

- Document: <https://raw.githubusercontent.com/QubesOS/qubes-doc/f7daea25d36a41e7c2b11fce2196ac7592287cfb/developer/system/gui.rst>,
  rendered at <https://doc.qubes-os.org/en/latest/developer/system/gui.html>
- Access: free.
- sha256: `7745667512d7a529c6924e9410216e60588c59383de222d8d59bcce56d6b2884`

**Wayland**: release 1.26.0, commit
`87cc8a8728a923fc57938faa81ba0e74f34ecdc7`: the protocol,
`protocol/wayland.xml`, and the book's chapter on it,
`doc/book/src/Protocol.md`.

- Documents: <https://gitlab.freedesktop.org/wayland/wayland/-/raw/87cc8a8728a923fc57938faa81ba0e74f34ecdc7/protocol/wayland.xml>,
  <https://gitlab.freedesktop.org/wayland/wayland/-/raw/87cc8a8728a923fc57938faa81ba0e74f34ecdc7/doc/book/src/Protocol.md>
- Access: free, under an MIT-style notice.
- sha256: `cc860987e54f8d85c940e97fa1270c69b6e4ad31fbcf5a7f00107ce1157f5e07`
  (`wayland.xml`) and
  `3ee13f2b1d232c6617a616b0fbbc9260799c82a8dd7d74cb3b50e327cd8657bd`
  (`Protocol.md`)

**X11**: Robert W. Scheifler, X Window System Protocol, X Version 11
Release 7.7, and David P. Wiggins, Security Extension Specification,
version 7.1, November 15, 1996 (X Consortium Standards).

- Documents: <https://www.x.org/releases/current/doc/xproto/x11protocol.html>,
  <https://www.x.org/releases/current/doc/xextproto/security.html>
- Access: free, under MIT-style notices.
- sha256: `ef66b4aef805ddbf9e9e8575123fe74e74156c79f5f6e43f3fd7e6bffcd8d7c0`
  and `b673a19b52689e4ea0d3b2bb39f1152eeae352e1c166b791db1dd6c00b23d0f4`

**Windows**: Microsoft Learn pages, with the date each shows as its
last update.

| Page | Updated | sha256 |
|---|---|---|
| [Interactive logon: Do not require CTRL+ALT+DEL][ms-sas-policy] | 2017-04-19 | `8aceb8e09630ae2f46c3b080e9c2439d3c76ec6c0f0f33ab5409e5deae7b6379` |
| [Initializing Winlogon][ms-winlogon] | 2021-01-07 | `033f5da3ad3468a0be1a3d8436c2e18225eb93c3f2a6e443aa3f37f2e4bda5ab` |
| [SendInput function (winuser.h)][ms-sendinput] | 2021-10-13 | `9be740f42e07620ed9827fc62fbbc43b75c9645a310b80b1798e36767ce59be1` |

[ms-sas-policy]: https://learn.microsoft.com/en-us/previous-versions/windows/it-pro/windows-10/security/threat-protection/security-policy-settings/interactive-logon-do-not-require-ctrl-alt-del
[ms-winlogon]: https://learn.microsoft.com/en-us/windows/win32/secauthn/initializing-winlogon
[ms-sendinput]: https://learn.microsoft.com/en-us/windows/win32/api/winuser/nf-winuser-sendinput

- Access: free web pages.

**Fullscreen**: WHATWG Fullscreen API Standard, commit snapshot
`a3e9529f1cdff6a7f11b36b569dd38fbba40cb47`, last updated October 19,
2026.

- Document: <https://fullscreen.spec.whatwg.org/commit-snapshots/a3e9529f1cdff6a7f11b36b569dd38fbba40cb47/>
- Access: free, under CC BY 4.0.
- sha256: `4051d5fdbb65974898fefbbcea3e97a6fe216198001c48db61fc6b38ccdabbf3`

**Plan 9**: the manual pages rio(1), rio(4) and draw(3), as 9p.io
serves them; they state no edition.

| Page | sha256 |
|---|---|
| [rio(1)](https://9p.io/magic/man2html/1/rio) | `5b1276c6759594af4ca732a037e7db8973801dc46d54dce2b7411f9b2b256559` |
| [rio(4)](https://9p.io/magic/man2html/4/rio) | `0f73d39c3244d39dfaad6c77429a283a91a20237c879edd2366104f99759a15d` |
| [draw(3)](https://9p.io/magic/man2html/3/draw) | `02fb5c60fe1bb6243033df373bfe151e7031e501655f3b6b7ae2b39158d9f9ff` |

- Access: free web pages.

**Scenic**: Fuchsia's Scenic, the Fuchsia system compositor (last
updated October 12, 2026) and RFC-0147, View System (last updated
September 22, 2026).

- Documents: <https://fuchsia.dev/fuchsia-src/concepts/ui/scenic/index>,
  <https://fuchsia.dev/fuchsia-src/contribute/governance/rfcs/0147_view_system>
- Access: free web pages, under CC BY 4.0.
- sha256: `c6352067317ee9a959227d56bc29756304791f97b2918517908bdfc8cc0ce0ab`
  and `fc59cbe5bf8ab1ad06b59644d225b58b8002bd481e493df643675eb8cc2509d1`

**HTML**: WHATWG HTML Standard, commit snapshot
`5fcc2318395821db61e353705e6368ad9a000af5`, last updated November 6,
2026.

- Document: <https://html.spec.whatwg.org/commit-snapshots/5fcc2318395821db61e353705e6368ad9a000af5/>
- Access: free, under CC BY 4.0.
- sha256: `cda7b99bd4ee71fc87c8be8391d937161950c6becd8fee94e5665acc23622003`

**RemoteViews**: Android's API reference for `android.widget.RemoteViews`,
last updated August 28, 2026.

- Document: <https://developer.android.com/reference/android/widget/RemoteViews>
- Access: free web page.
- sha256: `dd743734efdb082609fe88b07ae670447f56935133ada769c5c601294db4cba0`

**Flutter**: Flutter architectural overview, for Flutter 3.47, last
updated October 24, 2026.

- Document: <https://docs.flutter.dev/resources/architectural-overview>
- Access: free web page, under CC BY 4.0.
- sha256: `042a66f37b1fde1f6bf54d547f6ae74342c894027c9785fdce9ed1f9f7655e57`

**Dear ImGui**: release 1.93.0, commit
`c1aaac65e896a45f81f47a4d68f9959e0f86c859`: `docs/README.md` and
`docs/FAQ.md`.

- Documents: <https://raw.githubusercontent.com/ocornut/imgui/c1aaac65e896a45f81f47a4d68f9959e0f86c859/docs/README.md>,
  <https://raw.githubusercontent.com/ocornut/imgui/c1aaac65e896a45f81f47a4d68f9959e0f86c859/docs/FAQ.md>
- Access: free, under the MIT licence.
- sha256: `1abb04cbd7419abf31de079a619a9c1e50442d39abc5747b41beee8575da93ff`
  and `04dd8e9edb859c27b0e1bf8fe5548497d79b6a2f3f352f5fe9a199ffeac542fd`

**Arcan**: Bjorn Stahl, A deeper dive into the SHMIF IPC system, the
Arcan blog, November 21, 2024.

- Document: <https://arcan-fe.com/2024/11/21/a-deeper-dive-into-the-shmif-ipc-system/>
- Access: free web page; no terms stated.
- sha256: `0a9803468334741e70d212ddd0d2b14dc0d2c5de8a6867f0ab198fb60f1c0c10`

**Adaptive Cards**: Adaptive Cards Overview (Microsoft Learn), last
updated December 10, 2025.

- Document: <https://learn.microsoft.com/en-us/adaptive-cards/>
- Access: free web page.
- sha256: `ebd4af5fa95b0332b13f5ba7d6e1889ad08f24dceb90fe037375f6c852ea9c02`

**WCAG**: Web Content Accessibility Guidelines (WCAG) 2.2, W3C
Recommendation, 12 December 2024.

- Document: <https://www.w3.org/TR/2024/REC-WCAG22-20241212/>
- Access: free web page; "W3C liability, trademark and document use
  rules apply".
- sha256: `6e3c5fe397257cae509a2fb4752b73062cf8cbeb92c2cec618989b17e4cf7057`

## Network design references

Implementations [network.md](network.md) describes or compares nova-os
with. Each was read at the commit given; a hash is of the file as
fetched on November 7, 2026.

**QEMU**, tag `v8.2.2`, commit `11aa0b1ff115b86160c4d37e7c37e6a6b13b77ea`,
which Ubuntu 24.04 ships as `1:8.2.2+ds-0ubuntu1.18` and the CI's Boot
job installs, and tag `v11.1.3`, commit
`005c61c0ef6f6ae615283440b4bb7be51b2c6ddd`, Homebrew's.

- Source: <https://gitlab.com/qemu-project/qemu/-/tree/v8.2.2>,
  <https://gitlab.com/qemu-project/qemu/-/tree/v11.1.3>
- Access: open source; "The QEMU emulator as a whole is released under
  the GNU General Public License, version 2" (`LICENSE`, sha256
  `139c64b7a9ae62858ff969aae622ecfcf4f1519655bd92d2e70a1a2fb615690e` at
  v8.2.2). network.md quotes its documentation and describes behaviour
  read in its source, and copies none of its code.

| File | v8.2.2 sha256 | v11.1.3 sha256 |
|---|---|---|
| `qemu-options.hx` | `a5f306558007e5889536f582dc14c95b5c5550bd865b301af2657c1c9d761462` | `39be98e63d1b2d7b65bcec4bca0e704a2bb79cc4bb52a6f6d071babc189c6e63` |
| `docs/system/devices/net.rst` | `bcaada5eb643bae311cd2fdabe5f4100d13367f37a823c790ab007fdb2f90d34` | `86f818ab71e6faa113c61f1458ad881e78a29d3c4ad5d8ce5c923f995be1d14b` |
| `net/slirp.c` | `83860b9121637f7039d555aab01aa83ef194e110cf2771512f60996ec9fe7a7c` | `f1eb0ede4b4d8016679ef176e33dccfbf659afab1b007ba2a92050a0afbbe799` |
| `hw/net/virtio-net.c` | `b2ed7a397329035c25ec1dcba70ea0ac87fc5e096d514210de31471a5084723a` | `14c4d68590a1c4c2e974b2f9374e5b84102c40e33dce4d4153503ea0b2ba01c6` |
| `hw/virtio/virtio-pci.c` | `8d3da6766cfb2126511aa1af61c843552cd759cc325971c0f414a6ced48a926f` | `affd3c3257c3b77350f7eba692d1fbe2a155d180f5a78b2e500a0c2256f10739` |

**libslirp**, QEMU's user network, tag `v4.7.0`, commit
`3ad1710a96678fe79066b1469cead4058713a1d9`, which Ubuntu 24.04 ships as
`libslirp0` `4.7.0-1ubuntu3.1`, and tag `v4.9.7`, commit
`ce12dcddf710a71dc0de6450ec95edbadf8e9f66`, Homebrew's.

- Source: <https://gitlab.freedesktop.org/slirp/libslirp/-/tree/v4.7.0>,
  <https://gitlab.freedesktop.org/slirp/libslirp/-/tree/v4.9.7>
- Access: open source; each file of `src/` is BSD-3-Clause or MIT by its
  SPDX tag, under the 3-clause terms of `COPYRIGHT`.

| File | v4.7.0 sha256 | v4.9.7 sha256 |
|---|---|---|
| `COPYRIGHT` | `b28aecf4796a6a22054167f0a976de13d9db335669d37afd2dc7ea4c335e1e13` | `5e81ad3841c9073bee91f65c8c74fdc4b67d1f6742cd35fbd22039bb1e920818` |
| `src/bootp.c` | `9e981b131b4cbc7b97c3b903783145639d0ac84c1d2b60fa35d46c2a68716e52` | `107dccef9b66c1498353644c6fb53d6e1ea09ab1c4af7cc5be557749a4b77811` |
| `src/udp.c` | `023ceb269a0bdb2357effb1187af74772b067b492f764ce8bf01c5a01ea58e20` | `932ce4920d9616d01313bf0e5108adbe598a4112fb2ff286d521e9dcc0ce4053` |
| `src/tcp_input.c` | `1fc3d01106864553f72150f99f7bcac4a1f23f382a369b4c960294f531dc93fc` | `3b6837ed27996f9bd6e1fbdb0e8ab72f9076f0af827cdcf36bc956aaed712032` |
| `src/ip_icmp.c` | `c8fd8bea729376520f244dedd8644b32ffaf80cd09a24af7478331ba48a53732` | `4fdcba07bec9db98f3444309877a33241981dc127af88a54c74dfc2d8fd02af4` |
| `src/misc.c` | `b00204dd4f3a502ff540aada3fb31a1a68054af5431c46cfa9c2c920faf54ce1` | not read |
| `src/socket.c` | `e85ae70d413fbae6682037e62072c9d7c2e90e24e212ca7a9b527dd8147fedea` | not read |

**lwIP**, tag `STABLE-2_2_1_RELEASE`, commit
`77dcd25a72509eb83f72b033d219b1d40cd8eb95`, February 5, 2025.

- Source: <https://git.savannah.nongnu.org/cgit/lwip.git/tag/?h=STABLE-2_2_1_RELEASE>
- Access: open source, BSD-3-Clause (`COPYING`).
- sha256: `ef4aac92e05e87cd1cdc140870ed52206ba03d4a7fe46c1e11d7ffa6c87d252b`
  (`COPYING`) and
  `d750b825d4e399eca387bdeb98a78dc96267095eb8f15cdb39e4e88d23c14360`
  (`src/include/lwip/opt.h`)

**picoTCP**, tag `v1.7.0`, commit
`13c00a067190f5ec62340ed932a2bb833c366d63`, February 8, 2017, and
**picoTCP-NG**, tag `v2.1`, commit
`dc9cfc0145151012cf1946774ef1795fe221079c`, December 10, 2020.

- Source: <https://github.com/tass-belgium/picotcp/tree/v1.7.0>,
  <https://github.com/virtualsquare/picotcp/tree/v2.1>
- Access: open source, "the GNU General Public License, version 2, or
  (at your option) version 3" (`COPYING`, the same in both). Read for
  its licence and its size only.
- sha256: `af5a2bc623f97a8707322a971e78067c8a1fd25c7493328b52a9baafee97e735`
  (`COPYING`)

**FreeBSD**, tag `release/15.1.0`, commit
`96841ea08dcfa84b954a32dc5ae1a26c28966cf4`, June 11, 2026.

- Source: <https://github.com/freebsd/freebsd-src/tree/release/15.1.0>
- Access: open source; `sys/netinet` and `sys/netinet6` are
  BSD-3-Clause and BSD-2-Clause by SPDX tag, and `in_cksum.c`
  BSD-4-Clause.
- sha256: `68723fbba7c3f7a62e7b96abca901cf084df26e7205cc1a05993ba541a950950`
  (`sys/netinet/tcp_input.c`),
  `206914254bb3f0c61e3f68946c86ad325c366b55f40ac5404c074277206e7ee5`
  (`sys/netinet/ip_input.c`) and
  `4baff024831f8652215163ef7e838076c055f5fee14773c36456840520ceb175`
  (`sys/netinet/in_cksum.c`)

**smoltcp**, tag `v0.14.0`, commit
`cbfe05f835cfc75bf781addc1c0eb2e1a0df1ea3`, August 17, 2026.

- Source: <https://github.com/smoltcp-rs/smoltcp/tree/v0.14.0>
- Access: open source, 0BSD (`LICENSE-0BSD.txt`).
- sha256: `beb2cad88fab8447f7975564e21f9506e733e14c344836f146fa02e811216694`
  (`LICENSE-0BSD.txt`),
  `d75bdf1a3e4cfb0dedb1a1d32b73940e5cd0a258ad8008511b5b6f3e813c5407`
  (`README.md`) and
  `b490b161e1ddc3fb44e2a9c8d765d3599f48c234560b1210d7a29bc93abc666e`
  (`src/iface/interface/mod.rs`)

**uIP 1.0**, Adam Dunkels's import of the original sources, tag
`uip-1-0`, commit `a49def743f6e5c7d0c0f2d724f0b8e0c563a4a37`.

- Source: <https://github.com/adamdunkels/uip/tree/uip-1-0>
- Access: open source, BSD-3-Clause.
- sha256: `0f616f0b27a0957e5173117cb073ea62f4f094c0f2ad7a9938c3ac05596ddff1`
  (`uip/uip.c`)

**MINIX 3**, commit `4db99f4012570a577414fe2a43697b2f239b699e`, its
`master` as fetched, and the commit that added its lwIP service,
`ef8d499e2d2af900e9b2ab297171d7b088652482`.

- Source: <https://github.com/Stichting-MINIX-Research-Foundation/minix/tree/4db99f4012570a577414fe2a43697b2f239b699e/minix/net/lwip>
- Access: open source, BSD-style (`LICENSE`).
- sha256: `0e63e84fab1fa7a2d8fed9d96d87c36b20ed1c3003c06dc270ef995cc57fa832`
  (`minix/net/lwip/lwip.c`) and
  `90ce7da73c9427845dc86dbb9f88523980180f5c2356f7bc9c562ed9691d0c2c`
  (`minix/lib/liblwip/lib/lwipopts.h`)

**Fuchsia**, Netstack3 pages, with the date each shows as its last
update.

| Page | Updated | sha256 |
|---|---|---|
| [Netstack3 (2021 roadmap)](https://fuchsia.dev/fuchsia-src/contribute/roadmap/2021/netstack3) | 2025-02-28 | `2e8f17c2cf6940eb00633c70da86eaedbc63a4800e5c8307d74cbcb2acae31f2` |
| [Contributing to Netstack3](https://fuchsia.dev/fuchsia-src/contribute/contributing-to-netstack/netstack3) | 2025-03-22 | `76afcf2c3a807f30f1456988c12374e76f06548989d83abc1aaaae6d21c82b65` |

- Access: free web pages.

**Vulnerability reports.**

| Report | Page | Date | sha256 |
|---|---|---|---|
| AMNESIA:33 | [CERT/CC VU#815128](https://kb.cert.org/vuls/id/815128) | Released 2020-12-08, revised 2025-06-06 | `550073db1391cd5b3ff5aa0644ca0caf8de15e3c235cc9158261e4cd4c9480b0`, the page decompressed |
| NAME:WRECK | [Forescout's disclosure](https://www.forescout.com/blog/forescout-and-jsof-disclose-new-dns-vulnerabilities-impacting-millions-of-enterprise-and-consumer-devices/) | April 12, 2021 | `f2772de25d80d8fa28ba3c3083b5915d7c7fef461c3194a1de38b72d0d6df9d9` |

- Access: free web pages. CERT/CC has no note for NAME:WRECK that was
  found, so Forescout's disclosure is the source.

**Private Network Access** and **Local Network Access**, Draft Community
Group Reports of the W3C's Web Platform Incubator Community Group,
edited at Google. Local Network Access says it "builds on top of
Chrome's previously paused" Private Network Access work.

| Document | Date | sha256 |
|---|---|---|
| [Private Network Access](https://wicg.github.io/private-network-access/) | 26 September 2024 | `142676f4955365f250e3a78364595ccd85ef8ce8d928c3025325a41f1bece640` |
| [Local Network Access](https://wicg.github.io/local-network-access/) | 7 August 2026 | `fa3e1152eade831a4d5e45d5f37d70cb1fad0e5a5ece2bddd0cc7d94e3a7f467` |

- Access: free web pages.

**POSIX `<netinet/in.h>`**, Base Definitions of The Open Group Base
Specifications Issue 8, IEEE Std 1003.1-2024.

- Document: <https://pubs.opengroup.org/onlinepubs/9799919799/basedefs/netinet_in.h.html>
- Access: free web page.
- sha256: `ac46ac2417326c68fff196812a97e06b8ec2676fe6f703b4b20aace7cb7559da`

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
