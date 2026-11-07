# nova-os design

nova-os is an operating system with NT's object model on a small kernel
core, written in C and built entirely from source down to a 1,623-byte hex0
seed, so that every byte of a release can be rebuilt from that seed and
compared.

## Principles

1. **Reproducible from the seed.** Every artifact, from the compiler to the
   kernel to the boot image, is a nova-nix derivation whose chain starts at
   stage0-pe32's seed. The same inputs give the same bytes.
2. **NT's model, not POSIX, on a small core.** The programming model is
   NT's: kernel objects in one namespace, handles with access rights,
   objects a thread can wait on, and asynchronous I/O requests, exposed
   through a native system-call API. The kernel keeps only what needs the
   processor's privilege: address spaces, threads and scheduling,
   interrupts and timers, physical memory, the object and handle table,
   and messages between processes. Drivers, file systems, networking and
   the compositor are user-mode processes holding handles to the devices
   and memory they serve, so a failed driver is restarted instead of
   stopping the system, and the privileged core stays small enough to
   prove. Programming interfaces sit above the native API as subsystems; a
   POSIX layer, if one is ever wanted, is one of them, never the base.
3. **Interface and developer experience first.** The boot screen and the
   desktop are designed, not left for last. Building, booting and seeing a
   change takes one command and seconds. Applications get a documented,
   modern API over the native one, and an SDK.
4. **Portable by construction.** Boot through UEFI, discover hardware
   through ACPI, and keep everything hardware-specific in the kernel's
   architecture layer and in drivers. x86-64 first, then ARM64.
5. **Verifiable.** A proof checker built from the seed ships in the base
   system, and chosen invariants of the system, the kernel core's isolation
   first, carry machine-checked proofs.
   The boot image's TPM measurements can be computed from source, so a
   machine's signed boot record shows it runs the code built from the seed.
6. **Self-hosting.** nova-os runs the seed ladder and nova-nix and rebuilds
   itself; the result must equal the Windows-hosted build byte for byte.

## Decisions

- **Language:** C, the language the seed ladder reaches first.
- **Kernel structure:** a small privileged core with drivers in user mode
  (principle 2). A driver that moves memory by DMA is confined only where
  the platform's IOMMU confines it; without one, its isolation is not
  enforced.
- **License:** Apache-2.0. Code is written from public documentation and
  observed behavior; nothing is copied from Microsoft, from leaked sources,
  or from GPL projects (ReactOS, Linux, stage0-uefi).
- **First target:** x86-64, booting through UEFI in a Hyper-V Generation 2
  virtual machine. Gen 2 firmware cannot enroll new Secure Boot keys, and an
  image signed by Microsoft would not be reproducible from source, so the VM
  runs with Secure Boot off.
- **Compiler:** an x86-64 PE cross compiler that tinycc-native compiles
  from TinyCC's source. A UEFI image is linked with `-shared` so that it
  carries base relocations, which TinyCC writes only for DLL-style images.

## Stages

Each stage is demonstrable on its own and reproducible from the seed.

1. **Boot image:** `\EFI\BOOT\BOOTX64.EFI` and a disk image holding it
   (GPT, an EFI System Partition with FAT32, a VHDX container), written by a
   C tool the chain compiles. It prints to the screen in the VM.
2. **Kernel:** the loader hands the machine to the kernel, which sets up
   paging, interrupts, a timer, physical memory management and a
   framebuffer console.
3. **Core:** the kernel's own address spaces, threads and scheduling, user
   mode, the object and handle table, messages between processes, and the
   system-call API.
4. **Userland and interface:** user-mode drivers, a shell, the native API's
   user layer, the desktop.
5. **Self-hosting:** the seed ladder and nova-nix run on nova-os and
   rebuild it.

## Testing

The first target is a VM whose disk lives on a spare volume of the test
machine, never the host's own system disk. Real hardware comes later, booted
from a spare drive through the firmware's boot menu.

The repository runs no CI while it is private. Changes are checked on a
development machine with host-built stand-ins of the chain's compilers and
by booting the VM by hand.

## Trust

Each layer runs on the one below it, so a claim about nova-os is only as
strong as the lowest layer built from source.

| Layer | Built from source |
|---|---|
| Applications and interface | yes |
| Kernel | yes, from the seed |
| Compiler | yes, from the seed |
| Seed | yes; 1,623 bytes, checked by hand and by proof |
| Build host | Windows until nova-os rebuilds itself (stage 5) |
| Firmware | open firmware in a VM; coreboot where a board allows, with the vendor's binary parts |
| CPU security processor and microcode | no; the CPU vendor's signed code |
| CPU design | only on open hardware, such as a RISC-V core on an FPGA |
| Fabrication and physics | trusted |
