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
pixels, and [design/font](design/font/README.md) the console font's
prototype and the check that `gfx/font.c` draws its glyphs.

## Build

The images are compiled by TinyCC at commit
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
  -o BOOTX64.EFI boot/nova-boot.c boot/tables.c boot/switch.S gfx/gfx.c \
  gfx/raster.c gfx/boot_screen.c
./x86_64-win32-tcc -shared -nostdlib -nostdinc -I tinycc-$rev/include \
  -I include -I gfx -I boot -I kernel -Wl,-subsystem=native \
  -Wl,-e=kernel_main -Wl,-section-alignment=1000 -Wl,-file-alignment=200 \
  -o kernel.exe kernel/main.c kernel/x86_64/arch.c kernel/x86_64/gdt.c \
  kernel/x86_64/idt.c kernel/x86_64/apic.c kernel/x86_64/paging.c \
  kernel/x86_64/serial.c kernel/x86_64/clock.c kernel/x86_64/context.c \
  kernel/x86_64/entry.S kernel/x86_64/switch.S kernel/x86_64/entropy.c \
  kernel/x86_64/fpu.c kernel/x86_64/user.c kernel/x86_64/syscall.S \
  kernel/x86_64/msr.S kernel/x86_64/speculation.c \
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
  gfx/raster.c gfx/boot_screen.c gfx/limb.c gfx/background.c
./x86_64-win32-tcc -nostdlib -nostdinc -I tinycc-$rev/include \
  -I include -I native -I lib -I drivers -I kernel/random -I store \
  -I test -Wl,-subsystem=native -Wl,-e=init_main \
  -Wl,-section-alignment=1000 -Wl,-file-alignment=200 -o init.exe \
  init/init.c init/chain.c init/compat.c init/view.c init/platform.c \
  init/store.c init/console.c init/data.c init/login_start.c \
  drivers/pci.c drivers/virtio_pci.c store/reader.c lib/path.c \
  lib/program.c lib/log.c native/native.S kernel/random/sha256.c
./x86_64-win32-tcc -nostdlib -nostdinc -I tinycc-$rev/include \
  -I include -I native -I lib -I drivers -Wl,-subsystem=native \
  -Wl,-e=virtio_blk_main -Wl,-section-alignment=1000 \
  -Wl,-file-alignment=200 -o virtblk.exe drivers/virtio_blk.c \
  drivers/virtio_driver.c drivers/virtio_pci.c drivers/pci.c lib/log.c \
  native/native.S
./x86_64-win32-tcc -nostdlib -nostdinc -I tinycc-$rev/include \
  -I include -I native -I lib -I drivers -Wl,-subsystem=native \
  -Wl,-e=virtio_input_main -Wl,-section-alignment=1000 \
  -Wl,-file-alignment=200 -o virtinp.exe drivers/virtio_input.c \
  drivers/virtio_driver.c drivers/virtio_pci.c drivers/pci.c lib/log.c \
  native/native.S
./x86_64-win32-tcc -nostdlib -nostdinc -I tinycc-$rev/include \
  -I include -I native -I lib -I gfx -I kernel/random \
  -Wl,-subsystem=native -Wl,-e=console_main -Wl,-section-alignment=1000 \
  -Wl,-file-alignment=200 -o console.exe console/console.c console/text.c \
  console/keys.c console/panel.c gfx/gfx.c gfx/raster.c gfx/font.c \
  gfx/limb.c lib/log.c native/native.S
./i386-win32-tcc -shared -nostdlib -nostdinc -I tinycc-$rev/include \
  -I include -Wl,-subsystem=native -Wl,-e=ntdll_entry \
  -Wl,-section-alignment=1000 -Wl,-file-alignment=200 \
  -o ntdll.dll ntdll/ntdll.c ntdll/entry.S
./x86_64-win32-tcc -nostdlib -nostdinc -I tinycc-$rev/include \
  -I include -I native -I lib -I kernel/random -Wl,-subsystem=native \
  -Wl,-e=store_main -Wl,-section-alignment=1000 -Wl,-file-alignment=200 \
  -o store.exe store/store.c store/reader.c lib/name.c lib/log.c \
  native/native.S kernel/random/sha256.c
./x86_64-win32-tcc -nostdlib -nostdinc -I tinycc-$rev/include \
  -I include -I native -I lib -Wl,-subsystem=native \
  -Wl,-e=namespace_main -Wl,-section-alignment=1000 \
  -Wl,-file-alignment=200 -o namespace.exe namespace/namespace.c \
  lib/name.c lib/log.c native/native.S
./x86_64-win32-tcc -nostdlib -nostdinc -I tinycc-$rev/include \
  -I include -I native -I lib -I kernel/random -Wl,-subsystem=native \
  -Wl,-e=fs_main -Wl,-section-alignment=1000 -Wl,-file-alignment=200 \
  -o fs.exe fs/fs.c fs/pool.c lib/name.c lib/log.c native/native.S \
  kernel/random/sha256.c
./x86_64-win32-tcc -nostdlib -nostdinc -I tinycc-$rev/include \
  -I include -I native -I lib -I test -Wl,-subsystem=native \
  -Wl,-e=fstest_main -Wl,-section-alignment=1000 \
  -Wl,-file-alignment=200 -o fstest.exe test/fstest.c lib/path.c \
  lib/log.c native/native.S
./x86_64-win32-tcc -nostdlib -nostdinc -I tinycc-$rev/include \
  -I include -I native -I lib -I kernel/random -Wl,-subsystem=native \
  -Wl,-e=accounts_main -Wl,-section-alignment=1000 \
  -Wl,-file-alignment=200 -o accounts.exe accounts/accounts.c \
  accounts/argon2.c accounts/blake2b.c accounts/record.c lib/path.c \
  lib/log.c native/native.S
./x86_64-win32-tcc -nostdlib -nostdinc -I tinycc-$rev/include \
  -I include -I native -I lib -I kernel/random -Wl,-subsystem=native \
  -Wl,-e=login_main -Wl,-section-alignment=1000 -Wl,-file-alignment=200 \
  -o login.exe login/login.c lib/program.c lib/console.c lib/path.c \
  lib/log.c native/native.S
./x86_64-win32-tcc -nostdlib -nostdinc -I tinycc-$rev/include \
  -I include -I native -I lib -I kernel/random -Wl,-subsystem=native \
  -Wl,-e=shell_main -Wl,-section-alignment=1000 -Wl,-file-alignment=200 \
  -o shell.exe shell/shell.c lib/console.c lib/path.c lib/log.c \
  native/native.S
cc -o mknar -I include -I kernel/random tools/mknar.c tools/store_path.c \
  kernel/random/sha256.c
cc -o mkstore -I include -I kernel/random tools/mkstore.c \
  tools/store_path.c kernel/random/sha256.c
greeting=$(./mknar greeting greeting file . test/greeting.txt)
./x86_64-win32-tcc -nostdlib -nostdinc -I tinycc-$rev/include \
  -I include -I native -I lib -I test -Wl,-subsystem=native \
  -Wl,-e=hello_main -Wl,-section-alignment=1000 -Wl,-file-alignment=200 \
  -D "GREETING_PATH=\"/store/$greeting\"" -o hello.exe test/hello.c \
  lib/path.c lib/log.c native/native.S
services=$(./mknar services nova-services dir bin \
  exec bin/store.exe store.exe exec bin/namespace.exe namespace.exe)
./mknar session nova-session dir bin exec bin/accounts.exe accounts.exe \
  exec bin/login.exe login.exe exec bin/shell.exe shell.exe
./mknar hello hello -r $greeting dir bin exec bin/hello.exe hello.exe \
  dir share link share/greeting /store/$greeting \
  link share/greeting-relative ../../$greeting \
  link share/greeting-windows C:/nix/store/$greeting
./mkstore store.img $services greeting.narinfo greeting.nar \
  services.narinfo services.nar hello.narinfo hello.nar \
  session.narinfo session.nar
```

`-shared` makes TinyCC write base relocations, which the firmware needs to
load nova-boot, and nova-boot to load the kernel, at addresses of their
choosing; TinyCC writes them only for a DLL-style image. `init.exe` is
linked without it: the kernel places the first process at the image's own
base address, and the first process places each program it starts, the
drivers `virtblk.exe` and `virtinp.exe`, the console `console.exe`,
the file system service `fs.exe` and its check `fstest.exe`, and the
programs from `/store`, at its own; login places the programs it starts
from `/store` at theirs too. `ntdll.dll` is
linked with `-shared` for its export table, which TinyCC writes only for a
DLL, and is placed at its own base address too. For the native subsystem
TinyCC aligns sections to 32 bytes but still writes the 4096 and 512 byte
alignments into the header, so the links of the kernel and of every
program set those alignments (in hexadecimal) to make their layout match.
TinyCC also writes `ntdll.def`, its exports, which nothing reads. Each
build gives the same bytes:

| Image | Size | sha256 |
|---|---|---|
| `BOOTX64.EFI` | 29,696 | `ed744ea9428e8fd8a0cd1f8173cdfead7fc0d9ad98f1fa48b1f94f417337c70f` |
| `kernel.exe` | 204,288 | `a9f73a46f26daba1bc2454cac2dfd80eeb369cae1a266e638fb76d215e018b0d` |
| `init.exe` | 92,672 | `4fdc5b87c0fcc0d9975632e56dd38e6dfae3030a769b976c8418276c0b1221e7` |
| `virtblk.exe` | 19,456 | `14c68c048f14db914dc5a3164a7e9806625b2c4089f72a7b3cce835eaa24fc02` |
| `virtinp.exe` | 15,872 | `f4322918ebc64193d49a56c2e638c2c4081afeb13251826c9e56f2371f448704` |
| `console.exe` | 36,352 | `b3d3caa685145e77e152e6599e6aa8b6660a89ea70931ca57208b0b043f89ded` |
| `ntdll.dll` | 10,240 | `9a43b4e3b773c1b5a3b6eb541fd0de9c3f3675d2867d1193b707118648853d10` |
| `store.exe` | 28,672 | `08b1663767426a0daf34b2f9b0c56bb2bc5e805d692b39bd83deef89355cee1a` |
| `namespace.exe` | 11,264 | `ea99c11b7bbc69cf1b10e383391216c7c7bc25fd8b7ba48c9ee47de8c7c4182b` |
| `hello.exe` | 20,480 | `31f37c7fce61b532e1ece9dab9d55ee49ff76ee4ed621533fac2f7cf6de4ab60` |
| `fs.exe` | 42,496 | `180a2a8323b06b8ed933a9dbde69f616abe2b3868e4cab245ea586a6deccda13` |
| `fstest.exe` | 32,256 | `6791c0df869ed8f5704ec3fba1076bfd54f9e78a2d5caec2632bb7057c439205` |
| `accounts.exe` | 39,936 | `726a0c515d3919ece07865ccf57f9944899077162550b97a2f86107c20a00a8d` |
| `login.exe` | 32,256 | `a8bb35d7f9fcf30c944e5acffda7ed04c76b5d508370eea04a4bea9b28366bfd` |
| `shell.exe` | 30,208 | `9f61351ca31257f0ddd118257f1cd128fe94dae626f2348fa258fc19d6699df4` |

The block also writes the store image, `store.img`
([docs/spec/store-image.md](docs/spec/store-image.md)), from these
store paths: `nova-services`, holding the store service and the
namespace server, which the image's header names as the programs the
first process starts; `greeting`, a file; `hello`, holding the test
program, which records the greeting's path and so refers to it, and
links to the greeting; and `nova-session`, holding the accounts
service, login and the shell. `tools/mknar.c` writes each path as nova-nix
exports one, a NAR and a narinfo, and prints its base name;
`tools/mkstore.c` checks each NAR against its narinfo and writes the
image. nova-nix does not run in this build, so mknar stands in for its
export: every path is content-addressed, named from its NAR hash and its
references as upstream Nix names a recursive SHA-256 path
(`makeFixedOutputPath`, hashing `/nix/store` as every host does), which
for a path without references is the name nova-nix gives the same tree.
No path is a derivation's output; the test program is compiled by the
block above. Each build gives the same bytes:

| Image | Size | sha256 |
|---|---|---|
| `store.img` | 180,224 | `406588b65df4919068c238e7fd25c9424623a0fb0f6505e072969b21c354b28d` |

## Disk image

`tools/mkimage.c` writes a boot disk as two files: a raw 64 MiB disk
image, and the same disk in a fixed VHDX file, the format a Hyper-V
Generation 2 virtual machine boots. The disk has a GPT with one EFI System
Partition, formatted FAT32, holding `\EFI\BOOT\BOOTX64.EFI` and in
`\NOVA\` the other files given, under 8.3 names
([docs/spec/disk-layout.md](docs/spec/disk-layout.md)). The tool is C99
with only the standard library, so the host compiler or TinyCC builds it.

The block below writes two boot disks and a data disk. The system's,
`disk.img` and `disk.vhdx`, holds the files its `mkimage` line names.
The test disk, `chain.img` and `chain.vhdx`, holds the same files and
the inputs of stage0-pe32's chain up to M1, which the bootstrap check
reads: its seed and the files of stage0-pe32 and of the M2libc,
M2-Planet and mescc-tools trees it builds from, at the commits nova-nix's
stage0-pe32 package pins.
`tools/mkdata.c` writes the data disk, `data.img`, raw, of the size given
in MiB: a GPT with one pool partition holding an empty data volume
([docs/spec/pool-format.md](docs/spec/pool-format.md)). It is C99 too,
and shares the pool's encoding, `fs/pool.c`, with the file system
service.

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
  virtblk.exe virtinp.exe console.exe store.img fs.exe fstest.exe
./mkimage chain.img chain.vhdx BOOTX64.EFI kernel.exe init.exe ntdll.dll \
  virtblk.exe virtinp.exe console.exe store.img fs.exe fstest.exe \
  chain/*
cc -o mkdata -I include -I kernel/random -I fs tools/mkdata.c fs/pool.c \
  kernel/random/sha256.c
./mkdata data.img 64
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
| `disk.img` | 67,108,864 | `4e2f89373588e180252d1d95ccb382412db8764d7d9d78d1bcbd42666623e995` |
| `disk.vhdx` | 71,303,168 | `6a515baba70f1cf1c7ffd7d7942f6a229c47c3fa4063da49de26fbfb22915816` |
| `chain.img` | 67,108,864 | `f19085392846809b5b831ca10363d9126c015b555513b58b93a69c7512afd8cc` |
| `chain.vhdx` | 71,303,168 | `b5ef32c7fca8736355debe27e156e9bde909473cdcd67d511e8d81a4724204e3` |
| `data.img` | 67,108,864 | `fc0e119b885b341b5385e351b1f334c4da7cb921ef2dfbd994e9e20917e27804` |

## Run

With QEMU and the EDK2 firmware it bundles (under `$(brew --prefix)/share/qemu`
on a Mac with Homebrew), the system boots from `disk.img`, with a copy of
`data.img` as its data disk:

```sh
fw=$(brew --prefix)/share/qemu
cp $fw/edk2-i386-vars.fd vars.fd && chmod u+w vars.fd
cp data.img data-run.img
qemu-system-x86_64 -machine q35 -m 256M \
  -drive if=pflash,format=raw,readonly=on,file=$fw/edk2-x86_64-code.fd \
  -drive if=pflash,format=raw,file=vars.fd \
  -drive if=virtio,format=raw,readonly=on,file=disk.img \
  -drive if=virtio,format=raw,file=data-run.img \
  -net none -device virtio-rng-pci -device virtio-keyboard-pci \
  -device qemu-xhci -device usb-tablet -serial stdio
```

The kernel prints its lines, and those of the programs it runs, to the
terminal through the serial port; the first process says
`nova-init: checks passed` once its checks pass. Login then runs in the
console's panel.

On a fresh data disk login creates the administrator's account and
starts the shell, whose `help` lists its commands. Booted again, the
same `data-run.img` keeps the account and the files the last boot
wrote, and login asks for a name and a password
([docs/spec/accounts.md](docs/spec/accounts.md)).

For the bootstrap check, boot `chain.img` in place of `disk.img`: the
first process logs what each step of the chain wrote and checks each
link against the one nova-nix pins
([docs/spec/compat.md](docs/spec/compat.md)). Under QEMU's emulation,
without hardware acceleration, the chain takes about 11 minutes on an
Apple M2. With `format=vhdx`, the same drive boots `disk.vhdx` or
`chain.vhdx`.

A system disk whose `\NOVA\` also holds a file named `FSLOOP` has
`fstest.exe` commit generations of its crash state until the machine
stops, so that a test can kill QEMU at any moment and check, at the next
boot, that the disk holds the generation last committed or the one after
it, whole.

The firmware's variable store needs a writable copy. The disk is attached
through virtio, so that the virtio block driver has the boot disk to
read, and read-only, so that a boot leaves the image as built. The data
disk is attached writable, as a copy, since each boot writes it, and
after the system's, since the first process takes the second virtio
block function for it; without it there are no accounts, and neither
the accounts service nor login starts. The
virtio-rng device gives the firmware a random number generator, which
nova-boot asks for the seed of the kernel's generator: QEMU's default
processor has neither RDRAND nor RDSEED, so without the device the
kernel's generator has no seed and says so, and no account can be
created, since its salt comes from that generator. The virtio keyboard
gives the input driver its device, so the keys typed in QEMU's window
reach the console, which edits the line login or the shell reads and
shows it in the panel. The USB tablet gives the window an
absolute pointer, so QEMU does not capture the mouse; on a Mac,
`-display cocoa,zoom-to-fit=on -full-screen` gives it a full-screen
Space.

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
