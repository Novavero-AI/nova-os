# nova-os

nova-os is an operating system in early development: an NT-style hybrid
kernel, written in C and built entirely from source down to a 1,623-byte
seed, so that every byte of a release can be rebuilt from that seed and
compared.

It boots in a UEFI virtual machine. nova-boot, the loader, draws the boot
screen, loads the kernel from `\nova\kernel.exe`, exits the firmware's boot
services and jumps to the kernel, which keeps the boot screen's spinner
turning and reports itself on the first serial port.
[DESIGN.md](DESIGN.md) holds the principles, the decisions made so far and
the stages.

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
  -o BOOTX64.EFI boot/nova-boot.c gfx/gfx.c
./x86_64-win32-tcc -shared -nostdlib -nostdinc -I tinycc-$rev/include \
  -I include -I gfx -I boot -Wl,-subsystem=native -Wl,-e=kernel_main \
  -Wl,-section-alignment=1000 -Wl,-file-alignment=200 \
  -o kernel.exe kernel/main.c kernel/x86_64/arch.c kernel/x86_64/gdt.c \
  kernel/x86_64/idt.c kernel/x86_64/apic.c kernel/x86_64/serial.c \
  kernel/x86_64/entry.S kernel/mm/page.c kernel/mm/page_test.c \
  kernel/mm/boot_tables.c gfx/gfx.c
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
| `BOOTX64.EFI` | 13,824 | `63bdcd57a23d526367924fd6ac8d7956a26fb5ee5802fc6de17a4e2a4fd8dfd1` |
| `kernel.exe` | 20,480 | `48180bf81a346a7b0fa7601d1db1aa0c98f33e69b3c0c362cfb1bd1066ce618a` |

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
| `disk.img` | 67,108,864 | `bdbc31bfed3a94234e5e147fc3a2f53e8eebd55184e00a02f193f638402aec2b` |
| `disk.vhdx` | 71,303,168 | `a6c1698b557544090ccdaf052eb96389884d79bc8bed323a3e4d2b36365eb685` |

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
  -net none -device qemu-xhci -device usb-tablet -serial stdio
```

The kernel prints its lines to the terminal through the serial port. The
firmware's variable store needs a writable copy, and the folder is attached
as a read-only virtio disk, since QEMU's default IDE disk cannot be
read-only. The USB tablet gives the window an absolute pointer, so QEMU does
not capture the mouse; on a Mac, `-display cocoa,zoom-to-fit=on
-full-screen` gives it a full-screen Space.

nova-os is licensed under the Apache License 2.0; see [LICENSE](LICENSE).
