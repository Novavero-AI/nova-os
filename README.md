# nova-os

nova-os is an operating system in early development: NT's object model on
a small kernel core, written in C and built entirely from source down to a
1,623-byte seed, so that every byte of a release can be rebuilt from that
seed and compared.

It boots in a UEFI virtual machine. nova-boot, the loader, draws the boot
screen, loads the kernel from `\nova\kernel.exe`, exits the firmware's boot
services and jumps to the kernel, which reports itself on the first serial
port and, once started, fades the boot screen into the desktop background.
[DESIGN.md](DESIGN.md) holds the principles, the decisions made so far and
the stages; [design/backgrounds](design/backgrounds/README.md) holds the
background's prototypes and the check that `gfx/limb.c` draws Limb's
pixels.

## Build

Both images are compiled by TinyCC at commit
cb41cbfe717e4c00d7bb70035cda5ee5f0ff9341, built as a cross compiler that
writes x86-64 PE. The archive's sha256 is
`7381f944aa9255cd560e8192c5b0241246d2c83eea54b8eb317102992fe1e2bb`.

```sh
rev=cb41cbfe717e4c00d7bb70035cda5ee5f0ff9341
curl -L -o tinycc.tar.gz https://github.com/TinyCC/tinycc/archive/$rev.tar.gz
tar xzf tinycc.tar.gz
mkdir cfg && : > cfg/config.h
cc -o x86_64-win32-tcc -I cfg -I tinycc-$rev \
  -D TCC_TARGET_X86_64 -D TCC_TARGET_PE -D ONE_SOURCE=1 \
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
  kernel/mm/page.c kernel/mm/page_test.c kernel/sched/thread.c \
  kernel/sched/wait.c kernel/sched/sched_test.c kernel/random/chacha20.c \
  kernel/random/sha256.c kernel/random/generator.c kernel/random/random.c \
  kernel/random/random_test.c gfx/gfx.c gfx/limb.c gfx/background.c
```

`-shared` makes TinyCC write base relocations, which the firmware needs to
load nova-boot, and nova-boot to load the kernel, at addresses of their
choosing; TinyCC writes them only for a DLL-style image. For the native
subsystem TinyCC aligns sections to 32 bytes but still writes the
4096 and 512 byte alignments into the header, so the kernel's link sets
those alignments (in hexadecimal) to make its layout match. Each build
gives the same bytes:

| Image | Size | sha256 |
|---|---|---|
| `BOOTX64.EFI` | 25,088 | `72062013e5dbb01ffd255e7a1befe0c60fdd7d21d36f6787751471966dbe8ce1` |
| `kernel.exe` | 59,904 | `7a704c1bedd768cfc1ea9ca3e8e22c3cded218fba85adabb8be416db8f6de631` |

## Disk image

`tools/mkimage.c` writes the boot disk twice: `disk.img`, a raw 64 MiB
disk, and `disk.vhdx`, the same disk in a fixed VHDX file, the format a
Hyper-V Generation 2 virtual machine boots. The disk has a GPT with one EFI
System Partition, formatted FAT32, holding `\EFI\BOOT\BOOTX64.EFI` and
`\NOVA\KERNEL.EXE`. The tool is C99 with only the standard library, so the
host compiler or TinyCC builds it.

```sh
cc -o mkimage tools/mkimage.c
./mkimage disk.img disk.vhdx BOOTX64.EFI kernel.exe
```

From the images above, each run gives the same bytes:

| Image | Size | sha256 |
|---|---|---|
| `disk.img` | 67,108,864 | `8f69cbe89796944abcf9501af8b56767c58dfb05145813b6c45e3f91ed0ea582` |
| `disk.vhdx` | 71,303,168 | `397ee459847f0b57c0485d5fb5e3aba68a5db304c382a290060dd462ad59f076` |

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
cp BOOTX64.EFI esp/EFI/BOOT/ && cp kernel.exe esp/nova/
cp $fw/edk2-i386-vars.fd vars.fd && chmod u+w vars.fd
qemu-system-x86_64 -machine q35 -m 256M \
  -drive if=pflash,format=raw,readonly=on,file=$fw/edk2-x86_64-code.fd \
  -drive if=pflash,format=raw,file=vars.fd \
  -drive if=virtio,format=raw,file=fat:esp,readonly=on \
  -net none -device virtio-rng-pci -device qemu-xhci -device usb-tablet \
  -serial stdio
```

The kernel prints its lines to the terminal through the serial port. The
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
