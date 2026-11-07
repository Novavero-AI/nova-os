# nova-os

nova-os is an operating system in early development: NT's object model on
a small kernel core, written in C and built entirely from source down to a
1,623-byte seed, so that every byte of a release can be rebuilt from that
seed and compared.

It boots in a UEFI virtual machine. nova-boot, the loader, draws the boot
screen, loads the kernel from `\nova\kernel.exe` and reads the other
files in `\nova\`, the first process's `init.exe` among them, exits the
firmware's boot services and jumps to the kernel, which runs the first
process in user mode, reports itself and the process's exit code on the
first serial port and, once started, fades the boot screen into the
desktop background. From the test disk, which holds the inputs of
stage0-pe32's chain, the first process runs the bootstrap check: the
chain, from its hex0 seed to M1, as 32-bit Windows programs, with each
link checked against the one nova-nix builds on Windows
([docs/spec/compat.md](docs/spec/compat.md)). It then enumerates PCI and
starts the virtio block driver, `virtblk.exe`, as a user-mode process of
its own, which reads the boot disk's partition table and the EFI System
Partition's boot sector through its virtqueue.
[DESIGN.md](DESIGN.md) holds the principles, the decisions made so far and
the stages; [design/backgrounds](design/backgrounds/README.md) holds the
background's prototypes and the check that `gfx/limb.c` draws Limb's
pixels.

## Build

The five images are compiled by TinyCC at commit
cb41cbfe717e4c00d7bb70035cda5ee5f0ff9341, built twice: as a cross
compiler that writes x86-64 PE, and as one that writes i386 PE for ntdll.
The archive's sha256 is
`7381f944aa9255cd560e8192c5b0241246d2c83eea54b8eb317102992fe1e2bb`.

```sh
rev=cb41cbfe717e4c00d7bb70035cda5ee5f0ff9341
curl -L -o tinycc.tar.gz https://github.com/TinyCC/tinycc/archive/$rev.tar.gz
tar xzf tinycc.tar.gz
mkdir cfg && : > cfg/config.h
cc -o x86_64-win32-tcc -I cfg -I tinycc-$rev \
  -D TCC_TARGET_X86_64 -D TCC_TARGET_PE -D ONE_SOURCE=1 \
  -D 'TCC_VERSION="0.9.28"' tinycc-$rev/tcc.c
cc -o i386-win32-tcc -I cfg -I tinycc-$rev \
  -D TCC_TARGET_I386 -D TCC_TARGET_PE -D ONE_SOURCE=1 \
  -D 'TCC_VERSION="0.9.28"' tinycc-$rev/tcc.c
./x86_64-win32-tcc -shared -nostdlib -nostdinc -I tinycc-$rev/include \
  -I include -I gfx -I boot -Wl,-subsystem=efiapp -Wl,-e=efi_main \
  -o BOOTX64.EFI boot/nova-boot.c boot/tables.c boot/switch.S gfx/gfx.c
./x86_64-win32-tcc -shared -nostdlib -nostdinc -I tinycc-$rev/include \
  -I include -I gfx -I boot -I kernel -Wl,-subsystem=native \
  -Wl,-e=kernel_main -Wl,-section-alignment=1000 -Wl,-file-alignment=200 \
  -o kernel.exe kernel/main.c kernel/x86_64/arch.c kernel/x86_64/gdt.c \
  kernel/x86_64/idt.c kernel/x86_64/apic.c kernel/x86_64/paging.c \
  kernel/x86_64/serial.c kernel/x86_64/clock.c kernel/x86_64/context.c \
  kernel/x86_64/entry.S kernel/x86_64/switch.S kernel/x86_64/entropy.c \
  kernel/x86_64/fpu.c kernel/x86_64/user.c kernel/x86_64/syscall.S \
  kernel/mm/page.c kernel/mm/page_test.c kernel/mm/memory_map.c \
  kernel/acpi/acpi.c kernel/sched/thread.c kernel/sched/wait.c \
  kernel/sched/sched_test.c kernel/random/chacha20.c \
  kernel/random/sha256.c kernel/random/generator.c kernel/random/random.c \
  kernel/random/random_test.c kernel/object/object.c \
  kernel/object/channel.c kernel/object/resource.c kernel/object/job.c \
  kernel/object/section.c kernel/object/process.c kernel/object/image.c \
  kernel/object/port.c kernel/object/interrupt.c \
  kernel/object/initiator.c kernel/object/first_process.c \
  kernel/object/object_test.c kernel/syscall/syscall.c gfx/gfx.c \
  gfx/limb.c gfx/background.c
./x86_64-win32-tcc -nostdlib -nostdinc -I tinycc-$rev/include \
  -I include -I native -I lib -I drivers -I kernel/random \
  -Wl,-subsystem=native -Wl,-e=init_main -Wl,-section-alignment=1000 \
  -Wl,-file-alignment=200 -o init.exe init/init.c init/chain.c \
  init/compat.c init/view.c init/platform.c init/loader.c drivers/pci.c \
  drivers/virtio_pci.c \
  lib/log.c native/native.S kernel/random/sha256.c
./x86_64-win32-tcc -nostdlib -nostdinc -I tinycc-$rev/include \
  -I include -I native -I lib -I drivers -Wl,-subsystem=native \
  -Wl,-e=virtio_blk_main -Wl,-section-alignment=1000 \
  -Wl,-file-alignment=200 -o virtblk.exe drivers/virtio_blk.c \
  drivers/virtio_pci.c drivers/pci.c lib/log.c native/native.S
./i386-win32-tcc -shared -nostdlib -nostdinc -I tinycc-$rev/include \
  -I include -Wl,-subsystem=native -Wl,-e=ntdll_entry \
  -Wl,-section-alignment=1000 -Wl,-file-alignment=200 \
  -o ntdll.dll ntdll/ntdll.c ntdll/entry.S
```

`-shared` makes TinyCC write base relocations, which the firmware needs to
load nova-boot, and nova-boot to load the kernel, at addresses of their
choosing; TinyCC writes them only for a DLL-style image. `init.exe` is
linked without it: the kernel places the first process at the image's own
base address, and the first process places the virtio block driver,
`virtblk.exe`, at its own. `ntdll.dll` is linked with `-shared` for its
export table, which TinyCC writes only for a DLL, and is placed at its own
base address too. For the native subsystem TinyCC aligns sections to 32
bytes but still writes the 4096 and 512 byte alignments into the header,
so the links of the kernel, `init.exe`, `virtblk.exe` and `ntdll.dll` set
those alignments (in hexadecimal) to make their layout match. TinyCC also
writes `ntdll.def`, its exports, which nothing reads. Each build gives the
same bytes:

| Image | Size | sha256 |
|---|---|---|
| `BOOTX64.EFI` | 28,160 | `04b0989e077d4ca8838dac1b3007b744025370165c1846903b010f4c3853ba44` |
| `kernel.exe` | 130,048 | `461d3412eafaf618037658c76c2b7b577ac4932a80259be34e40550050bd75e4` |
| `init.exe` | 43,520 | `8d2ebdda1d7eb6d5fdce8d4d27c5039f96a3c34ca94aced4641815642f2696ce` |
| `virtblk.exe` | 14,848 | `c1bce54d00c905a867e88ed7a9cfaaf860264be5ddac929609d96a704df124dd` |
| `ntdll.dll` | 10,240 | `9a43b4e3b773c1b5a3b6eb541fd0de9c3f3675d2867d1193b707118648853d10` |

## Disk image

`tools/mkimage.c` writes a boot disk as two files: a raw 64 MiB disk
image, and the same disk in a fixed VHDX file, the format a Hyper-V
Generation 2 virtual machine boots. The disk has a GPT with one EFI System
Partition, formatted FAT32, holding `\EFI\BOOT\BOOTX64.EFI` and in
`\NOVA\` the other files given, under 8.3 names
([docs/spec/disk-layout.md](docs/spec/disk-layout.md)). The tool is C99
with only the standard library, so the host compiler or TinyCC builds it.

The block below writes two disks. The system's, `disk.img` and
`disk.vhdx`, holds in `\NOVA\` the kernel, `INIT.EXE`, `NTDLL.DLL` and
`VIRTBLK.EXE`. The test disk, `chain.img` and `chain.vhdx`, holds the
same files and the inputs of stage0-pe32's chain up to M1, which the
bootstrap check reads: its seed and the files of stage0-pe32 and of the
M2libc, M2-Planet and mescc-tools trees it builds from, at the commits
nova-nix's stage0-pe32 package pins.

```sh
raw=https://raw.githubusercontent.com/Novavero-AI
stage0=$raw/stage0-pe32/ff1c2ad86a91d3d7d086722ffed49dfe18dd50d0
m2libc=$raw/M2libc/13e57eb52bd30a6b0296f1cef1c54fc40065f07b
m2planet=$raw/M2-Planet/ee0bf91eb02390c2d40bd85379113e524f0c752b
mescc=$raw/mescc-tools/578ac99623d291e2b9eacf03cacc390cfe512661
mkdir -p chain
while read -r name url; do curl -L -o chain/$name $url; done <<EOF
hex0seed.exe $stage0/bootstrap-seeds/PE32/i386/hex0-seed.exe
hex0_x86.hex $stage0/x86/hex0_x86.hex0
hex1_x86.hex $stage0/x86/hex1_x86.hex0
hex2_x86.hex $stage0/x86/hex2_x86.hex1
catm_x86.hex $stage0/x86/catm_x86.hex2
pe32i386.hex $stage0/x86/PE32-i386.hex2
ntdll386.hex $stage0/x86/ntdll-i386.hex2
m0_x86.hex $stage0/x86/M0_x86.hex2
cc_x86.m1 $stage0/x86/cc_x86.M1
libccore.m1 $stage0/x86/libc-core.M1
libcboot.m1 $stage0/x86/libc-bootstrap.M1
pe-end.m1 $stage0/x86/pe-end-shim.M1
x86_defs.m1 $m2libc/x86/x86_defs.M1
bootstrp.c $m2libc/x86/windows/bootstrap.c
bootable.c $m2libc/bootstrappable.c
cc.h $m2planet/cc.h
cc_globl.c $m2planet/cc_globals.c
cc_readr.c $m2planet/cc_reader.c
cc_strng.c $m2planet/cc_strings.c
cc_types.c $m2planet/cc_types.c
cc_emit.c $m2planet/cc_emit.c
cc_core.c $m2planet/cc_core.c
cc_macro.c $m2planet/cc_macro.c
cc.c $m2planet/cc.c
stringfy.c $mescc/stringify.c
m1-macro.c $mescc/M1-macro.c
EOF
cc -o mkimage tools/mkimage.c
./mkimage disk.img disk.vhdx BOOTX64.EFI kernel.exe init.exe ntdll.dll \
  virtblk.exe
./mkimage chain.img chain.vhdx BOOTX64.EFI kernel.exe init.exe ntdll.dll \
  virtblk.exe chain/*
```

The chain's inputs are those trees' files, unmodified:

| File | Size | sha256 |
|---|---|---|
| `chain/hex0seed.exe` | 1,623 | `b9db7be0fed7770be9fca46b7aa551b611fc60eaf1112a053531aae14aa46742` |
| `chain/hex0_x86.hex` | 28,673 | `982c377dbc539d95c5c13fcb9e6707f5f9420a94b36ac94e3a6390e179780ec9` |
| `chain/hex1_x86.hex` | 35,593 | `35cbd2e9be676323dd4a0c92c763a19e5d9253e6b44b77ffc3c31aa3249ff2d2` |
| `chain/hex2_x86.hex` | 37,490 | `3e7348a01bc55dfa82b3ba32487feceaee91cf810a3727b71449d52f344f282b` |
| `chain/catm_x86.hex` | 35,650 | `15d8657cafb604081fc5b95dbdf9af932dcd5630255cbcbe1d4315732264ad42` |
| `chain/pe32i386.hex` | 19,108 | `f7bc6c53d1561a18cdff6d0d464c1c84c6360a2696d94b3e4ecf4c5057b6dcd9` |
| `chain/ntdll386.hex` | 36,426 | `c715048ceca269c32745572fde4241bf8f7ee8c1ed6edf3ce861fe484223a081` |
| `chain/m0_x86.hex` | 33,306 | `f8e6154bfc85022d8db79ff7e7d02e5830fc0b6501cbe5f696753523b20ed386` |
| `chain/cc_x86.m1` | 203,841 | `1002acfe72c3dd2424e24af7d38a611f8b689754d68e9f9d98ac462730a0feeb` |
| `chain/libccore.m1` | 30,096 | `d22dbad850aae74f6901c9ddc97205c5356b61c76afdb064fde488f555a85eff` |
| `chain/libcboot.m1` | 758 | `bfe10a57c0d83f938f89d48e58b43d70e4bea7a17cb784db9369dca8a130c6fd` |
| `chain/pe-end.m1` | 457 | `0139f8f0097c06924b0776c54a797e4c3241a1acaed35d25965f99a26e520337` |
| `chain/x86_defs.m1` | 6,294 | `7f2a56c9bcac578389d12949c98692fd77180c7323c2dc9f2d4dd79a0b5f3cd6` |
| `chain/bootstrp.c` | 4,148 | `c838905fa2f33c6dcebf74e3e729599912602985f69f3212f314c44c00431461` |
| `chain/bootable.c` | 3,825 | `4c77cee2334816719d0db30ded2388fc0d8d676ba2b39b46b21e3b956814b5c2` |
| `chain/cc.h` | 3,585 | `4ea75a3ae7e9559bc8f80c07892d14f31525dba99e075587548a38685319a7c4` |
| `chain/cc_globl.c` | 2,027 | `9ca4b663b780802979bb59acaacc35e9f0238526737dde2a3c8ca94ef753ebf5` |
| `chain/cc_readr.c` | 9,177 | `c54db85d4bbf5f66afe2f1c6f8ba2b969a9d4584356c9d2970227ba1f6e33d27` |
| `chain/cc_strng.c` | 5,511 | `723809f52ab919ac540e574cdd196c02aba2ce662753417e4b47958416656a4c` |
| `chain/cc_types.c` | 23,142 | `710a5b53ad66c3f25cccdd00ac645aa44136b11b72dcef7aabfc51be0dc8520c` |
| `chain/cc_emit.c` | 30,356 | `fee9eef38574837b7e57a0aeef635a3b0dbb49cef4c693292cf27d90bcd1176a` |
| `chain/cc_core.c` | 104,016 | `c9270db70928ba370f866eae9c8fc796ae7ad19558568616930903e2c36c78a6` |
| `chain/cc_macro.c` | 25,722 | `8f1b2360f82cca6cb2fe85f2af62b9e40316c6bd79eec5d0d97483cc921e819c` |
| `chain/cc.c` | 11,307 | `e52139205480424c2aad1b955a969edda15fdca540c5c73b3c7e827002134e21` |
| `chain/stringfy.c` | 2,517 | `2cc09fd299c2bdff1a5e32d1baf84bd92a01f4447c475a671a80fa6549bf1302` |
| `chain/m1-macro.c` | 19,911 | `cc83d1625a5d3daf2059fdfd0f362bfd1e1d19b8880913439f30f38a7c7b4fed` |

From the images above, each run gives the same bytes:

| Image | Size | sha256 |
|---|---|---|
| `disk.img` | 67,108,864 | `82c82bc6f3cf56aa0fe401fb2115f164813b96e6af9b059a531d22b9cea93d7f` |
| `disk.vhdx` | 71,303,168 | `f9a71a101ba64e1d13dc0db2b157ce85194c5ff4bf7d7522f8f6f6a6f9f9eed1` |
| `chain.img` | 67,108,864 | `ed82917970a1970dbaa1278cc2d0412d632396022df5ee5fc62057077b85e45b` |
| `chain.vhdx` | 71,303,168 | `909202a8abc2c74d5fc87865ba88aa9d816370280b889bd5322dfd6e0db1e338` |

## Run

With QEMU and the EDK2 firmware it bundles (under `$(brew --prefix)/share/qemu`
on a Mac with Homebrew), the system boots from `disk.img`:

```sh
fw=$(brew --prefix)/share/qemu
cp $fw/edk2-i386-vars.fd vars.fd && chmod u+w vars.fd
qemu-system-x86_64 -machine q35 -m 256M \
  -drive if=pflash,format=raw,readonly=on,file=$fw/edk2-x86_64-code.fd \
  -drive if=pflash,format=raw,file=vars.fd \
  -drive if=virtio,format=raw,readonly=on,file=disk.img \
  -net none -device virtio-rng-pci -device qemu-xhci -device usb-tablet \
  -serial stdio
```

The kernel prints its lines to the terminal through the serial port,
among them the first process's, which say
`nova-init: the bootstrap check is not on this disk` and end with
`nova-init: checks passed`.

For the bootstrap check, boot `chain.img` in place of `disk.img`. The
first process then gives the size and sha256 of what each of the
chain's 20 steps wrote, each link's the one nova-nix pins, and says
`nova-init: the chain built nova-nix's hex0, hex1, hex2, catm, M0,
cc_x86, M2 and M1` before it ends with `nova-init: checks passed`.
Under QEMU's emulation, without hardware acceleration, the chain takes
about 11 minutes on an Apple M2. With `format=vhdx`, the same drive
boots `disk.vhdx` or `chain.vhdx`.

The firmware's variable store needs a writable copy. The disk is attached
through virtio, so that the virtio block driver has the boot disk to
read, and read-only, so that a boot leaves the image as built. The
virtio-rng device gives the firmware a random number generator, which
nova-boot asks for the seed of the kernel's generator: QEMU's default
processor has neither RDRAND nor RDSEED, so without the device the
kernel's generator has no seed and says so. The USB tablet
gives the window an absolute pointer, so QEMU does not capture the
mouse; on a Mac, `-display cocoa,zoom-to-fit=on -full-screen` gives it a
full-screen Space.

nova-boot draws in the display's native mode. For a demo pixel for
pixel, give the VM a display the size of the host's screen by adding
`-vga none -device VGA,edid=on,xres=2560,yres=1600` with the screen's
width and height; `system_profiler SPDisplaysDataType` prints a Mac's as
its Resolution. The firmware takes that size from the EDID QEMU writes
if the video memory holds `xres` x `yres` x 4 bytes (`vgamem_mb`, 16 MiB
by default) and the EDID's first detailed timing carries the size, which
it does only while `xres` and `yres` are below 4096 and the timing's
pixel clock at `refresh_rate` (in millihertz, 75000 by default) is at
most 655.35 MHz. For 3840 x 2160, add `refresh_rate=50000,vgamem_mb=32`.

nova-os is licensed under the Apache License 2.0; see [LICENSE](LICENSE).
