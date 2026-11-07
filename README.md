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
desktop background. The first process runs stage0-pe32's hex0 seed as a
32-bit Windows program and checks that it assembles itself
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
  -Wl,-file-alignment=200 -o init.exe init/init.c init/seed.c \
  init/platform.c init/loader.c drivers/pci.c lib/log.c native/native.S \
  kernel/random/sha256.c
./x86_64-win32-tcc -nostdlib -nostdinc -I tinycc-$rev/include \
  -I include -I native -I lib -I drivers -Wl,-subsystem=native \
  -Wl,-e=virtio_blk_main -Wl,-section-alignment=1000 \
  -Wl,-file-alignment=200 -o virtblk.exe drivers/virtio_blk.c \
  drivers/pci.c lib/log.c native/native.S
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
| `BOOTX64.EFI` | 28,160 | `28c8fd78bf515d63034962c0bbaa557e2d76dbc614293ff36349264335df2af8` |
| `kernel.exe` | 121,856 | `3af0886ba43d69b234e097c8a8fa529bf10d4ed1f7a490cd47b84dba547088f7` |
| `init.exe` | 32,256 | `f57a52fb4b6059808d551e6c7e95e49894251758c214af64f4bc9046bc4272d8` |
| `virtblk.exe` | 14,336 | `160cab88efa70800ab58f1d36b073b1bbfd816d3727df7758d8e66fbbc1b08df` |
| `ntdll.dll` | 9,216 | `dcbe0870b27aefe05ef8c4bdf756b94c4f9c73b46667367282a0ade4aafe97b8` |

## Disk image

`tools/mkimage.c` writes the boot disk twice: `disk.img`, a raw 64 MiB
disk, and `disk.vhdx`, the same disk in a fixed VHDX file, the format a
Hyper-V Generation 2 virtual machine boots. The disk has a GPT with one EFI
System Partition, formatted FAT32, holding `\EFI\BOOT\BOOTX64.EFI` and in
`\NOVA\` the kernel, `INIT.EXE`, `NTDLL.DLL`, `VIRTBLK.EXE`, and
stage0-pe32's seed and the source it assembles, fetched from commit
ff1c2ad86a91d3d7d086722ffed49dfe18dd50d0 under 8.3 names
([docs/spec/disk-layout.md](docs/spec/disk-layout.md)). The tool is C99
with only the standard library, so the host compiler or TinyCC builds it.

```sh
seed=ff1c2ad86a91d3d7d086722ffed49dfe18dd50d0
raw=https://raw.githubusercontent.com/Novavero-AI/stage0-pe32/$seed
curl -L -o hex0seed.exe $raw/bootstrap-seeds/PE32/i386/hex0-seed.exe
curl -L -o hex0_x86.hex $raw/x86/hex0_x86.hex0
cc -o mkimage tools/mkimage.c
./mkimage disk.img disk.vhdx BOOTX64.EFI kernel.exe init.exe ntdll.dll \
  hex0seed.exe hex0_x86.hex virtblk.exe
```

The seed and its source are stage0-pe32's, unmodified:

| File | Size | sha256 |
|---|---|---|
| `hex0seed.exe` | 1,623 | `b9db7be0fed7770be9fca46b7aa551b611fc60eaf1112a053531aae14aa46742` |
| `hex0_x86.hex` | 28,673 | `982c377dbc539d95c5c13fcb9e6707f5f9420a94b36ac94e3a6390e179780ec9` |

From the images above, each run gives the same bytes:

| Image | Size | sha256 |
|---|---|---|
| `disk.img` | 67,108,864 | `64bbf02f6ca48a98ce0cb6f89127074cdc3a42e16ae3e758ef86fa8357ede08c` |
| `disk.vhdx` | 71,303,168 | `a0e9ade5274a8e5bb119acc0630f55c03ecfa062cf6b1567b1415fe4c99e36a6` |

To boot either file in QEMU, replace the `fat:esp` drive in the command
below with `-drive if=virtio,format=raw,file=disk.img` or
`-drive if=virtio,format=vhdx,file=disk.vhdx`; the `esp` folder is then not
needed.

## Run

With QEMU and the EDK2 firmware it bundles (under `$(brew --prefix)/share/qemu`
on a Mac with Homebrew):

```sh
fw=$(brew --prefix)/share/qemu
mkdir -p esp/EFI/BOOT esp/nova
cp BOOTX64.EFI esp/EFI/BOOT/
cp kernel.exe init.exe ntdll.dll hex0seed.exe hex0_x86.hex virtblk.exe \
  esp/nova/
cp $fw/edk2-i386-vars.fd vars.fd && chmod u+w vars.fd
qemu-system-x86_64 -machine q35 -m 256M \
  -drive if=pflash,format=raw,readonly=on,file=$fw/edk2-x86_64-code.fd \
  -drive if=pflash,format=raw,file=vars.fd \
  -drive if=virtio,format=raw,file=fat:esp,readonly=on \
  -net none -device virtio-rng-pci -device qemu-xhci -device usb-tablet \
  -serial stdio
```

The kernel prints its lines to the terminal through the serial port,
among them the first process's, which end with the sha256 of what the
seed wrote, `b9db7be0...` as above, and `nova-init: checks passed`. The
firmware's variable store needs a writable copy, and the folder is attached
as a read-only virtio disk, since QEMU's default IDE disk cannot be
read-only. The virtio-rng device gives the firmware a random number
generator, which nova-boot asks for the seed of the kernel's generator:
QEMU's default processor has neither RDRAND nor RDSEED, so without the
device the kernel's generator has no seed and says so. The USB tablet
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
