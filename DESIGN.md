# nova-os design

nova-os is an operating system modeled on Windows NT's object model, with
a small kernel core, written in C and built entirely from source down to a
1,623-byte hex0 seed, so that every byte of a release can be rebuilt from
that seed and compared.

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
   modern API over the native one, and an SDK. The graphical interface and
   the shell are two clients of one system API: every setting, action and
   piece of state the interface offers is reachable from the shell and
   from scripts, and the reverse, so neither holds a fact or a power the
   other lacks.
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
7. **A program's authority is its derivation.** No process has ambient
   authority: it starts with only the handles its creator passes it. A
   program's creator passes handles to exactly what its nova-nix derivation
   declares, the store paths of its closure and the services and devices
   the derivation names. The declaration that confines the build is the
   one that bounds the running program, and the kernel enforces it, so what
   a program can reach is read from its recipe before it runs.
8. **Accountable by construction.** Every effect a program has outside its
   own address space goes through a handle to a kernel object or a message
   to another process, so a session can be logged and replayed. With
   environments reproducible from the seed and authority bounded by the
   derivation, what a program, an AI agent included, could and could not
   do is a fact that can be checked, not a policy to trust.

## Decisions

- **Language:** C, the language the seed ladder reaches first.
- **Kernel structure:** a small privileged core with drivers in user mode
  (principle 2). A driver that moves memory by DMA is confined only where
  the platform's IOMMU confines it; without one, its isolation is not
  enforced.
- **License:** Apache-2.0. Code is written from public documentation and
  observed behavior; nothing is copied from Microsoft, from leaked sources,
  or from GPL projects (ReactOS, Linux, stage0-uefi).
- **First target:** x86-64, booting through UEFI in QEMU and in a Hyper-V
  Generation 2 virtual machine. Drivers come first for QEMU's virtio
  devices, whose OASIS specification is open and pinned, and then for
  Hyper-V's synthetic devices. Gen 2 firmware cannot enroll new Secure Boot
  keys, and an image signed by Microsoft would not be reproducible from
  source, so the VM runs with Secure Boot off.
- **Storage:** one tree with no drive letters and no `/dev`. `/store` holds
  every program, library and the OS itself, read-only and
  content-addressed; `/system` is the running system, a pointer into the
  store switched atomically; `/users/<name>` holds each user's files and
  profile; `/data` holds services' state. Store paths hash the store
  directory as `/nix/store` on every host, so each is named alike
  everywhere. The native file system is copy-on-write with SHA-256
  checksums, and only the store service writes the store
  ([docs/spec/storage.md](docs/spec/storage.md)).
- **Compiler:** an x86-64 PE cross compiler that tinycc-native compiles
  from TinyCC's source.

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

CI checks every pull request and every push to main:

- **Reproduce** runs `build.sh` on Linux and on macOS (Apple silicon),
  each host's compiler building TinyCC, and checks every file
  `SHA256SUMS` lists, so both hosts must give its bytes. It checks with
  `ci/check-downloads.py` that a download whose sha256 differs from
  `SHA256SUMS` stops the build before use, and that a build whose
  download directory holds every download succeeds with curl's every
  connection refused. The build stops where a protocol header tracked in
  `include/` is not what `mkinterface` makes of its description in
  `interfaces/`. It also builds with `--werror`, TinyCC's `-Wall
  -Werror`, runs `design/text/check-text.sh` on the fonts the build
  converted, and on Linux `design/text/compare-freetype.sh`.
- **Seed** runs `build.sh` on Windows, under busybox-w32's sh, with a
  TinyCC and a busybox that nova-nix builds from stage0-pe32's seed, and
  checks the same files, then builds again from the downloads the first
  build kept, and checks that build too. It runs on every push to main,
  weekly, and on a pull request that changes the job or a script it
  runs.
- **Lint** checks that tracked text is ASCII and that clang's `-Wall
  -Wextra -pedantic -Wshadow` find nothing in any C source, built with
  its `compile_flags.txt`, except `tools/check-probe-protocol.c`, which
  `ci/check-mkinterface.sh` builds with them against the header it makes;
  it runs `tools/check-masks.c`, `tools/check-fade.c`,
  `tools/check-pointer.c`, `tools/check-driver-end.c`,
  `tools/check-text.c`, `tools/check-endpoint.c`,
  `tools/check-backdrop.c`, `tools/check-outline.c`,
  `tools/check-compose.c`, `tools/check-window.c`,
  `tools/check-route.c`, `tools/check-backlog.c`, `tools/check-nvo.c`,
  `tools/check-decimal.c` (on the decimal test cases it downloads, pinned
  by sha256),
  `design/backgrounds/check-limb.sh`, `ci/check-mkinterface.sh`, which
  runs `tools/check-probe-protocol.c` under the sanitizers,
  `tools/check-console-protocol.c` and `tools/check-display-protocol.c`,
  `ci/check-boot.py`, which replays serial lines through `ci/boot.py` to
  check which failure lines the fallback boot accepts, and actionlint
  over the workflows.
- **Boot** boots the reproduced disks under QEMU with Ubuntu 24.04's
  OVMF, with a copy of `data.img` as the data disk: `disk.img`, the
  system's disk, on QEMU's default processor, and `test.img`, the test
  disk, on Skylake-Client, which needs page-table isolation, on `-cpu
  max`, and under KVM. The serial log must show each part starting, by
  the events its lines report ([log.md](docs/spec/log.md)), and a test
  disk's report, in KTAP, every test passing: the kernel's self-tests,
  the first process's checks of its calls, the data disk's check, the
  test program from `/store` and the boot disk's read. Then, sent
  through QEMU's monitor, a tablet's moves, clicks and wheel turns and
  a mouse's moves must reach the console, and, typed through it, the
  first boot's account is created on the fresh data disk and the shell
  runs `whoami`; the session then fills the console's panel, and the
  wheel scrolls it back and forward, each screen checked equal to the
  one `ci/screens.c` draws on the host with the console's own drawing
  code, the panel made from the kernel's background on `disk.img` and
  computed on `test.img`. Each of these sessions ends with logout,
  which login must report. A boot of the test disk boots the same data
  disk again, logs in with that account, which it must still hold, and
  its data disk's check reads back what the first boot wrote. Other boots of `disk.img`
  corrupt the data disk's GPT, and the log must show the file system
  service refusing the disk. A boot of `specimen.img` takes QMP's
  screendump once the type specimen has drawn, which must equal its
  prototype's render pixel for pixel. A boot of `logout.img`, whose
  session runs a program in the shell's place that starts programs and
  ends without waiting for them, must show login stopping each of them
  as it ends the session, and a second session, logged into after the
  first, running programs as the first did. A boot of `fallback.img`
  must show the compositor take the display and end at each of its two
  starts, restarted once and then given up, each input driver started
  again for the second start, the screen Limb alone after the
  compositor's first frame and the console draw the display after it,
  with no other failure line, and then goes on as the test disk's boot
  does, logging in through the console's keys.
- **Chain** boots `chain.img` and checks that each step of the bootstrap
  chain passes in its test report and each pinned link matches.

The jobs' scripts are in `ci/`, each with its usage in its header. CI
only checks `SHA256SUMS`; `ci/update-hashes.sh`, which refuses to run
in CI, rewrites it from a build after a change meant to change the
bytes.

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
