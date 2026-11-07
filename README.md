# nova-os

nova-os is an operating system in early development: NT's object model on
a small kernel core, written in C and built entirely from source down to a
1,623-byte seed, so that every byte of a release can be rebuilt from that
seed and compared.

It boots in a UEFI virtual machine. nova-boot, the loader, draws the boot
screen and starts the kernel, which runs the first process in user mode.
The first process starts the rest as user-mode processes: the store
service and the namespace server, from the store image, through which it
publishes the store as `/store`
([docs/spec/storage.md](docs/spec/storage.md), F16); the virtio
block and input drivers ([docs/spec/input.md](docs/spec/input.md)); the
console, which takes the display once the boot screen fades and serves
programs text output and lines typed at the keyboard
([docs/spec/console.md](docs/spec/console.md)); with a data disk, the
file system service, which serves `/data` and `/users` from it
([docs/spec/storage.md](docs/spec/storage.md), F18); and the accounts
service and login, which start each user's shell
([docs/spec/accounts.md](docs/spec/accounts.md)). It restarts a driver
or the console if one ends ([docs/spec/ipc.md](docs/spec/ipc.md), I10).
From a test disk it also runs stage0-pe32's bootstrap chain, from its
hex0 seed to M1, as 32-bit Windows programs, and checks each link against
the one nova-nix builds on Windows
([docs/spec/compat.md](docs/spec/compat.md)).
[docs/spec](docs/spec/README.md) lists the specifications, and
[DESIGN.md](DESIGN.md) holds the principles, the decisions made so far and
the stages; [design/backgrounds](design/backgrounds/README.md) holds the
background's prototypes and the check that `gfx/limb.c` draws Limb's
pixels, [design/font](design/font/README.md) the console font's
prototype and the check that `gfx/font.c` draws its glyphs, and
[design/text](design/text/README.md) the prototypes of the filled
outlines and text programs draw with, `lib/outline.c`,
`lib/typeface.c` and `lib/typeset.c`, and the check that the library
draws the prototypes' pixels.

## Build

[build.sh](build.sh) needs a C compiler as `cc`, `curl`, `tar` and
`unzip`. It fetches TinyCC's source, Inter's release and the test disk's
inputs, and writes every image into the directory it is given:

```sh
./build.sh out
```

[SHA256SUMS](SHA256SUMS) lists the sha256 of every file a build must
reproduce. To check a build:

```sh
(cd out && sha256sum -c ../SHA256SUMS)        # Linux
(cd out && shasum -a 256 -c ../SHA256SUMS)    # macOS
```

After a change meant to change those bytes, `ci/update-hashes.sh OUT`
rebuilds into OUT, a new directory, and rewrites SHA256SUMS; CI only
checks it.

## Run

The system boots in QEMU from `out/disk.img`, with UEFI firmware for
x86-64 and a copy of `out/data.img` as its data disk. On macOS,
Homebrew's `qemu` bundles the firmware:

```sh
code=$(brew --prefix)/share/qemu/edk2-x86_64-code.fd
vars=$(brew --prefix)/share/qemu/edk2-i386-vars.fd
```

On Debian or Ubuntu, with the `qemu-system-x86` and `ovmf` packages
installed:

```sh
code=/usr/share/OVMF/OVMF_CODE_4M.fd
vars=/usr/share/OVMF/OVMF_VARS_4M.fd
```

The first time, copy the data disk and the firmware's variable store,
which each boot writes:

```sh
cp out/data.img out/data-run.img
cp "$vars" out/vars.fd && chmod u+w out/vars.fd
```

Then boot:

```sh
qemu-system-x86_64 -machine q35 -m 256M \
  -drive if=pflash,format=raw,readonly=on,file="$code" \
  -drive if=pflash,format=raw,file=out/vars.fd \
  -drive if=virtio,format=raw,readonly=on,file=out/disk.img \
  -drive if=virtio,format=raw,file=out/data-run.img \
  -net none -device virtio-rng-pci -device virtio-keyboard-pci \
  -device virtio-tablet-pci -serial stdio
```

The kernel and every program log to the terminal through the serial
port. Once the boot screen fades, login runs in the console's panel; on
a fresh data disk it creates the administrator's account and starts the
shell. Booted again, `data-run.img` keeps the account and its files, and
login asks for a name and a password.

The virtio-rng device seeds the kernel's random number generator, which
creating an account needs, since QEMU's default processor has neither
RDRAND nor RDSEED. The virtio tablet gives the console a pointer and,
once its driver runs, keeps QEMU from capturing the mouse.

- `out/specimen.img` in place of `out/disk.img` shows the type specimen
  over Limb; the console runs but draws nothing.
- `out/chain.img` in place of `out/disk.img` runs the bootstrap check
  from the test disk; under emulation it takes minutes.
- With `format=vhdx`, the same drive boots `disk.vhdx` or `chain.vhdx`.
- nova-boot draws in the display's native mode, which
  `-vga none -device VGA,edid=on,xres=2560,yres=1600` sets to
  2560 x 1600. For 3840 x 2160, add `refresh_rate=50000,vgamem_mb=32`
  to the device.

nova-os is licensed under the Apache License 2.0; see [LICENSE](LICENSE).
