# boot

Two UEFI applications for x86-64, each written against the UEFI 2.10
specification with no UEFI library:

- `hello.c` prints a line through the firmware's console and waits for a key.
- `splash.c` draws nova-os's boot screen into the framebuffer the Graphics
  Output Protocol gives it: a dithered gradient with a soft glow, the
  wordmark "nova" in anti-aliased strokes, and a spinner that turns until a
  key is pressed.

## Build

They are compiled by TinyCC at commit
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
  -Wl,-subsystem=efiapp -Wl,-e=efi_main -o BOOTX64.EFI boot/splash.c
```

`-shared` makes TinyCC write base relocations, which the firmware needs to
load the image at an address of its choosing; TinyCC writes them only for a
DLL-style image. Each build gives the same bytes:

| Source | Size | sha256 |
|---|---|---|
| `hello.c` | 3,584 | `f01a6eb42f744db0e36636c794cfe19e4e1bd680a8170772537778fca6bac750` |
| `splash.c` | 9,216 | `66e20fa61c5602ca2b6ac3fc3ef301dcaca8a35b11d8a23c40738a75e8de0977` |

## Run

With QEMU and the EDK2 firmware it bundles (under `$(brew --prefix)/share/qemu`
on a Mac with Homebrew):

```sh
fw=$(brew --prefix)/share/qemu
mkdir -p esp/EFI/BOOT && cp BOOTX64.EFI esp/EFI/BOOT/
cp $fw/edk2-i386-vars.fd vars.fd && chmod u+w vars.fd
qemu-system-x86_64 -machine q35 -m 256M \
  -drive if=pflash,format=raw,readonly=on,file=$fw/edk2-x86_64-code.fd \
  -drive if=pflash,format=raw,file=vars.fd \
  -drive if=virtio,format=raw,file=fat:esp,readonly=on \
  -net none -device qemu-xhci -device usb-tablet
```

The firmware boots `esp/EFI/BOOT/BOOTX64.EFI`. The firmware's variable store
needs a writable copy, and the folder is attached as a read-only virtio disk,
since QEMU's default IDE disk cannot be read-only. The USB tablet gives the
window an absolute pointer, so QEMU does not capture the mouse.
