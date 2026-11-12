#!/bin/sh
# Builds nova-os's images in OUT from the tree that holds this script:
# TinyCC, compiled from its source by the host's C compiler, links the
# loader, the kernel and every program; tools the host compiler builds
# convert Inter's faces to nova-os fonts and write the store image, the
# system's boot disk, the test disk, the specimen disk, the fallback disk,
# the no-answer disk, the window disk, the chain disk, the logout disk and
# an empty data disk (docs/spec/disk-layout.md).
# SHA256SUMS gives the sha256 of every file in OUT that a build must
# reproduce, and the build checks each file it downloads against its
# line there before anything reads it.
#
# Usage: ./build.sh [--werror] OUT
#
# OUT is made if it does not exist. The build reads nothing in OUT that
# it did not write, so building into the same OUT again gives the same
# bytes. --werror adds -Wall -Werror to every TinyCC command, so that a
# warning TinyCC gives on nova-os's sources fails the build.
#
# NOVA_OS_DOWNLOAD_DIR, if set and not empty, names a directory, made if
# it does not exist, that keeps the build's downloads: a file there whose
# sha256 is the one SHA256SUMS gives is used in place of a fetch, any
# other is fetched again, and each file fetched is kept there.
#
# Needs a C compiler as cc, curl, tar and unzip. Runs under sh on Linux
# and macOS and under busybox-w32's sh on Windows.
set -eu

readonly TINYCC_REV=cb41cbfe717e4c00d7bb70035cda5ee5f0ff9341
readonly TINYCC_VERSION=0.9.28
readonly TINYCC_ARCHIVE=https://github.com/TinyCC/tinycc/archive/$TINYCC_REV.tar.gz

# The optimization the host's C compiler builds TinyCC and the host's
# tools with, which changes how fast they run but not what they write.
readonly HOST_OPTIMIZATION=-O2

# For the native subsystem TinyCC aligns sections to 32 bytes but writes
# 4096 and 512 byte alignments into the header, so every native link
# sets those alignments, in hexadecimal, to make its layout match.
readonly SECTION_ALIGNMENT=1000
readonly FILE_ALIGNMENT=200

readonly DATA_DISK_MIB=64

# Inter, the interface's typeface: the release, the six static faces the
# designs set type in, and the Unicode blocks docs/spec/ui.md U3 names,
# as the ranges mkfont takes.
readonly INTER_VERSION=4.1
readonly INTER_ARCHIVE=https://github.com/rsms/inter/releases/download/v$INTER_VERSION/Inter-$INTER_VERSION.zip
readonly INTER_FACES='Inter-Regular Inter-Medium Inter-SemiBold
  InterDisplay-Light InterDisplay-Regular InterDisplay-SemiBold'
readonly FONT_RANGES='20-7E A0-24F 370-3FF 400-482 48A-4FF 2000-209F
  20A0-20CF 2190-2193'

# stage0-pe32 and the trees its chain builds from, at the commits
# nova-nix's stage0-pe32 package pins.
readonly RAW=https://raw.githubusercontent.com/Novavero-AI
readonly STAGE0=$RAW/stage0-pe32/ff1c2ad86a91d3d7d086722ffed49dfe18dd50d0
readonly M2LIBC=$RAW/M2libc/13e57eb52bd30a6b0296f1cef1c54fc40065f07b
readonly M2PLANET=$RAW/M2-Planet/ee0bf91eb02390c2d40bd85379113e524f0c752b
readonly MESCC_TOOLS=$RAW/mescc-tools/578ac99623d291e2b9eacf03cacc390cfe512661

usage() {
  echo "usage: $0 [--werror] OUT" >&2
  exit 2
}

werror=no
if [ "${1-}" = --werror ]; then
  werror=yes
  shift
fi
[ $# -eq 1 ] || usage
case $1 in
  -*) usage ;;
esac

src=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
mkdir -p -- "$1"
out=$(CDPATH='' cd -- "$1" && pwd)
tcc_dir=$out/tinycc-$TINYCC_REV
downloads=
if [ -n "${NOVA_OS_DOWNLOAD_DIR-}" ]; then
  mkdir -p -- "$NOVA_OS_DOWNLOAD_DIR"
  downloads=$(CDPATH='' cd -- "$NOVA_OS_DOWNLOAD_DIR" && pwd)
fi

# The host's tools, C99 with only the standard library, built from the
# tree alone before anything is downloaded: sha256 prints a file's
# sha256, mknar writes a store path as nova-nix exports one, mkstore the
# store image (docs/spec/store-image.md), mkfont a TrueType font as a
# nova-os font (docs/spec/font-format.md), mkimage a boot disk as a raw
# image and as a VHDX file (docs/spec/disk-layout.md), mkdata a data
# disk holding an empty pool (docs/spec/pool-format.md), mkinterface the
# C header of an interface description (docs/spec/interface.md).
cd -- "$src"
cc "$HOST_OPTIMIZATION" -o "$out/sha256" -I include -I kernel/random \
  tools/sha256.c kernel/random/sha256.c
cc "$HOST_OPTIMIZATION" -o "$out/mknar" -I include -I kernel/random \
  tools/mknar.c tools/store_path.c kernel/random/sha256.c
cc "$HOST_OPTIMIZATION" -o "$out/mkstore" -I include -I kernel/random \
  tools/mkstore.c tools/store_path.c kernel/random/sha256.c
cc "$HOST_OPTIMIZATION" -o "$out/mkfont" -I include tools/mkfont.c
cc "$HOST_OPTIMIZATION" -o "$out/mkimage" tools/mkimage.c
cc "$HOST_OPTIMIZATION" -o "$out/mkdata" -I include -I kernel/random -I fs \
  tools/mkdata.c fs/pool.c kernel/random/sha256.c
cc "$HOST_OPTIMIZATION" -o "$out/mkinterface" tools/mkinterface.c

# Each interface description makes its protocol header, which must be
# the one tracked in include/, the header every program is built from
# (docs/spec/interface.md P16). A stale header stops the build here, before
# anything is compiled from it.
mkdir -p -- "$out/include"
for description in interfaces/*.desc; do
  name=${description##*/}
  name=${name%.desc}
  header=include/${name}_protocol.h
  "$out/mkinterface" "$description" "$out/$header"
  if ! cmp -s -- "$out/$header" "$header"; then
    echo "$0: $header is not what mkinterface makes of $description;" \
      "regenerate it with: $out/mkinterface $description $header" >&2
    exit 1
  fi
done

# pinned NAME: the sha256 SHA256SUMS gives for NAME.
pinned() {
  while read -r sum file; do
    if [ "$file" = "$1" ]; then
      echo "$sum"
      return
    fi
  done < "$src/SHA256SUMS"
  echo "$0: SHA256SUMS gives no sha256 for $1" >&2
  exit 1
}

# fetch NAME URL: writes URL's file as OUT/NAME once its sha256 is the
# one SHA256SUMS gives for NAME. A copy of the download directory's NAME
# with that sha256 stands in for the fetch, and a fetched file is kept
# there. Fetched bytes with any other sha256 stop the build, and are
# removed, before anything reads them.
fetch() {
  pin=$(pinned "$1")
  part=$out/$1.part
  if [ -n "$downloads" ] && [ -f "$downloads/$1" ] &&
    cp -- "$downloads/$1" "$part" &&
    [ "$("$out/sha256" "$part")" = "$pin" ]; then
    mv -f -- "$part" "$out/$1"
    return
  fi
  curl -fsSL -o "$part" "$2"
  fetched=$("$out/sha256" "$part")
  if [ "$fetched" != "$pin" ]; then
    rm -f -- "$part"
    echo "$0: $1 from $2 has sha256 $fetched; SHA256SUMS gives $pin" >&2
    exit 1
  fi
  mv -f -- "$part" "$out/$1"
  if [ -n "$downloads" ]; then
    # Copied under a name holding this shell's process ID, then renamed
    # into place, so that a build reading the directory meanwhile finds
    # the whole file or none.
    kept=$downloads/$1.$$
    mkdir -p -- "$(dirname -- "$kept")"
    if ! { cp -- "$out/$1" "$kept" && mv -f -- "$kept" "$downloads/$1"; }
    then
      rm -f -- "$kept"
      exit 1
    fi
  fi
}

# TinyCC as two cross compilers, one writing x86-64 PE and one writing
# i386 PE for ntdll. tcc.h includes config.h, which TinyCC's configure
# writes; configure does not run here, so an empty one stands in and
# the -D flags give what the build needs.
cross_compiler() {
  cc "$HOST_OPTIMIZATION" -o "$1-win32-tcc" -I cfg -I "tinycc-$TINYCC_REV" \
    -D "TCC_TARGET_$2" -D TCC_TARGET_PE -D ONE_SOURCE=1 \
    -D "TCC_VERSION=\"$TINYCC_VERSION\"" "tinycc-$TINYCC_REV/tcc.c"
}

cd -- "$out"
fetch tinycc.tar.gz "$TINYCC_ARCHIVE"
tar -xzf tinycc.tar.gz
mkdir -p cfg
: > cfg/config.h
cross_compiler x86_64 X86_64
cross_compiler i386 I386

# The store paths the test program and the type specimen record, so
# they are written before either is linked: greeting, a file, and
# Inter's faces as nova-os fonts beside its licence.
greeting=$(./mknar greeting greeting file . "$src/test/greeting.txt")
fetch "Inter-$INTER_VERSION.zip" "$INTER_ARCHIVE"
set --
for face in $INTER_FACES; do
  set -- "$@" "extras/ttf/$face.ttf"
done
unzip -o -j -d inter "Inter-$INTER_VERSION.zip" LICENSE.txt "$@"
set --
for face in $INTER_FACES; do
  # shellcheck disable=SC2086 # FONT_RANGES is a list of arguments.
  ./mkfont "inter/$face.ttf" "inter/$face.nvf" $FONT_RANGES
  set -- "$@" file "share/fonts/$face.nvf" "inter/$face.nvf"
done
fonts=$(./mknar fonts "inter-$INTER_VERSION" dir share dir share/fonts \
  file share/fonts/LICENSE.txt inter/LICENSE.txt "$@")

# Each image is linked from the tree's root with relative source paths,
# so that no byte depends on where the tree or OUT is.
cd -- "$src"

# TinyCC splits an -I path at each ':' (';' on Windows), so its own
# headers are named through -B, which it takes whole, and {B}, which
# stands for that directory.
tcc() {
  compiler=$1
  shift
  if [ "$werror" = yes ]; then
    set -- -Wall -Werror "$@"
  fi
  "$out/$compiler-win32-tcc" -nostdlib -nostdinc -B "$tcc_dir" \
    -I '{B}/include' -I include "$@"
}

# A program the kernel, the first process or login starts. It is linked
# without -shared, so it carries no base relocations: each is placed at
# its own image's base address. Arguments: the image's name, its entry
# point, then its -I and -D flags and its sources.
program() {
  name=$1
  entry=$2
  shift 2
  tcc x86_64 -Wl,-subsystem=native -Wl,-e="$entry" \
    -Wl,-section-alignment=$SECTION_ALIGNMENT \
    -Wl,-file-alignment=$FILE_ALIGNMENT -o "$out/$name" "$@"
}

# -shared makes TinyCC write base relocations, which it writes only for
# a DLL-style image: the firmware needs them to load nova-boot, and
# nova-boot to load the kernel, at addresses of their choosing.
tcc x86_64 -shared -I gfx -I boot -Wl,-subsystem=efiapp -Wl,-e=efi_main \
  -o "$out/BOOTX64.EFI" boot/nova-boot.c boot/tables.c boot/switch.S \
  gfx/gfx.c gfx/flush.c gfx/raster.c gfx/boot_screen.c
tcc x86_64 -shared -I gfx -I boot -I kernel -Wl,-subsystem=native \
  -Wl,-e=kernel_main -Wl,-section-alignment=$SECTION_ALIGNMENT \
  -Wl,-file-alignment=$FILE_ALIGNMENT -o "$out/kernel.exe" kernel/main.c \
  kernel/selftest.c \
  kernel/x86_64/arch.c kernel/x86_64/gdt.c kernel/x86_64/idt.c \
  kernel/x86_64/apic.c kernel/x86_64/paging.c kernel/x86_64/serial.c \
  kernel/x86_64/clock.c kernel/x86_64/context.c kernel/x86_64/entry.S \
  kernel/x86_64/switch.S kernel/x86_64/entropy.c kernel/x86_64/fpu.c \
  kernel/x86_64/user.c kernel/x86_64/syscall.S kernel/x86_64/msr.S \
  kernel/x86_64/speculation.c kernel/mm/page.c kernel/mm/page_test.c \
  kernel/mm/memory_map.c kernel/acpi/acpi.c kernel/sched/thread.c \
  kernel/sched/wait.c kernel/sched/sched_test.c kernel/random/chacha20.c \
  kernel/random/sha256.c kernel/random/generator.c kernel/random/random.c \
  kernel/random/random_test.c kernel/object/object.c \
  kernel/object/channel.c kernel/object/resource.c kernel/object/job.c \
  kernel/object/section.c kernel/object/process.c kernel/object/image.c \
  kernel/object/port.c kernel/object/interrupt.c \
  kernel/object/initiator.c kernel/object/first_process.c \
  kernel/object/object_test.c kernel/syscall/syscall.c gfx/gfx.c \
  gfx/flush.c gfx/raster.c gfx/boot_screen.c gfx/limb.c gfx/background.c

# ntdll.dll is linked with -shared for its export table, which TinyCC
# writes only for a DLL, and is placed at its own base address too.
# TinyCC also writes ntdll.def, its exports, which nothing reads.
tcc i386 -shared -Wl,-subsystem=native -Wl,-e=ntdll_entry \
  -Wl,-section-alignment=$SECTION_ALIGNMENT \
  -Wl,-file-alignment=$FILE_ALIGNMENT -o "$out/ntdll.dll" ntdll/ntdll.c \
  ntdll/entry.S

program init.exe init_main -I native -I lib -I drivers -I kernel/random \
  -I store -I test init/init.c init/chain.c init/compat.c init/view.c \
  init/platform.c init/store.c init/console.c init/compositor.c init/data.c \
  init/login_start.c init/specimen_start.c init/window_test.c init/tests.c \
  init/kill.c drivers/pci.c drivers/virtio_pci.c store/reader.c lib/path.c \
  lib/program.c lib/log.c lib/ktap.c lib/driver_end.c native/native.S \
  kernel/random/sha256.c
program virtblk.exe virtio_blk_main -I native -I lib -I drivers \
  drivers/virtio_blk.c drivers/virtio_driver.c drivers/virtio_pci.c \
  drivers/pci.c lib/log.c native/native.S
program virtinp.exe virtio_input_main -I native -I lib -I drivers \
  drivers/virtio_input.c drivers/pointer.c drivers/virtio_driver.c \
  drivers/virtio_pci.c drivers/pci.c lib/log.c native/native.S
program console.exe console_main -I native -I lib -I gfx -I kernel/random \
  console/console.c console/pane.c console/text.c console/view.c \
  console/panel.c gfx/gfx.c gfx/flush.c gfx/raster.c gfx/font.c gfx/limb.c \
  lib/endpoint.c lib/keymap.c lib/mapping.c lib/timing.c lib/log.c \
  native/native.S
program compose.exe compositor_main -I native -I lib -I gfx -I compositor \
  compositor/compositor.c compositor/serve.c lib/compose.c lib/outline.c \
  lib/window.c lib/backlog.c lib/route.c lib/endpoint.c lib/keyroute.c \
  lib/keymap.c lib/mapping.c lib/timing.c lib/log.c gfx/gfx.c gfx/flush.c \
  gfx/raster.c gfx/limb.c native/native.S
program wintest.exe wintest_main -I native -I lib -I gfx -I test \
  test/wintest.c lib/mapping.c lib/timing.c lib/log.c native/native.S
program store.exe store_main -I native -I lib -I kernel/random \
  store/store.c store/reader.c lib/name.c lib/log.c native/native.S \
  kernel/random/sha256.c
program namespace.exe namespace_main -I native -I lib \
  namespace/namespace.c lib/name.c lib/log.c native/native.S
program fs.exe fs_main -I native -I lib -I kernel/random fs/fs.c \
  fs/pool.c lib/name.c lib/log.c native/native.S kernel/random/sha256.c
program fstest.exe fstest_main -I native -I lib -I test test/fstest.c \
  lib/path.c lib/log.c lib/ktap.c native/native.S
program accounts.exe accounts_main -I native -I lib -I kernel/random \
  accounts/accounts.c accounts/argon2.c accounts/blake2b.c \
  accounts/record.c lib/path.c lib/log.c native/native.S
program login.exe login_main -I native -I lib -I kernel/random \
  login/login.c lib/program.c lib/console.c lib/path.c lib/log.c \
  native/native.S
program shell.exe shell_main -I native -I lib -I kernel/random \
  shell/shell.c lib/console.c lib/path.c lib/log.c native/native.S
program hello.exe hello_main -I native -I lib -I test \
  -D "GREETING_PATH=\"/store/$greeting\"" test/hello.c lib/path.c \
  lib/log.c lib/ktap.c native/native.S
program specimen.exe specimen_main -I native -I lib -I gfx -I test \
  -D "FONTS_PATH=\"/store/$fonts/share/fonts\"" test/specimen.c \
  test/specimen_draw.c lib/outline.c lib/typeface.c lib/typeset.c \
  lib/mapping.c lib/path.c lib/log.c gfx/gfx.c gfx/flush.c gfx/raster.c \
  gfx/limb.c native/native.S

# The logout disk's two programs: the victim, which a session's program
# runs and which stays running, and the driver, which stands in for the
# shell and records the victim's store path, so the victim's package is
# written before the driver is linked.
program victim.exe victim_main -I native -I lib -I test test/victim.c \
  lib/log.c native/native.S
victim=$(cd -- "$out" && ./mknar victim victim dir bin exec bin/victim.exe \
  victim.exe)
program driver.exe driver_main -I native -I lib -I test \
  -D "VICTIM_PATH=\"/store/$victim/bin/victim.exe\"" test/driver.c \
  lib/path.c lib/log.c native/native.S

# nvo's library, lib/nvo_*.c, is in no program yet. Linking it alone,
# with --werror's -Wall -Werror, checks that it builds for the guest with
# TinyCC and leaves no symbol undefined; no program is built from it and
# the result is not kept.
program nvo-library.exe nvo_parse -I lib lib/nvo_decimal.c lib/nvo_syntax.c \
  lib/nvo_value.c
rm -f -- "$out/nvo-library.exe"

# The routing of a pointer's reports to the window, lib/route.c, is in no
# program yet. Linking it alone, with --werror's -Wall -Werror, checks
# that it builds for the guest with TinyCC and leaves no symbol
# undefined; no program is built from it and the result is not kept.
program route-library.exe route_report -I lib -I gfx lib/route.c
rm -f -- "$out/route-library.exe"

# The store image's paths: nova-services, the programs the image's
# header names for the first process to start; greeting; hello, the
# test program, which refers to the greeting and links to it;
# nova-session, the accounts service, login and the shell; Inter's
# fonts; and specimen, the type specimen, which refers to the fonts.
# The logout disk's store image holds nova-services, victim, and its own
# nova-session, whose shell is the driver.
cd -- "$out"
services=$(./mknar services nova-services dir bin \
  exec bin/store.exe store.exe exec bin/namespace.exe namespace.exe)
./mknar session nova-session dir bin exec bin/accounts.exe accounts.exe \
  exec bin/login.exe login.exe exec bin/shell.exe shell.exe
./mknar hello hello -r "$greeting" dir bin exec bin/hello.exe hello.exe \
  dir share link share/greeting "/store/$greeting" \
  link share/greeting-relative "../../$greeting" \
  link share/greeting-windows "C:/nix/store/$greeting"
./mknar specimen specimen -r "$fonts" dir bin \
  exec bin/specimen.exe specimen.exe
./mkstore store.img "$services" greeting.narinfo greeting.nar \
  services.narinfo services.nar hello.narinfo hello.nar \
  session.narinfo session.nar fonts.narinfo fonts.nar \
  specimen.narinfo specimen.nar
./mknar logout-session nova-session dir bin exec bin/accounts.exe \
  accounts.exe exec bin/login.exe login.exe exec bin/shell.exe driver.exe
mkdir -p logout
./mkstore logout/store.img "$services" services.narinfo services.nar \
  victim.narinfo victim.nar logout-session.narinfo logout-session.nar

# The system's boot disk holds the loader and, in \NOVA\, these files.
set -- BOOTX64.EFI kernel.exe init.exe ntdll.dll virtblk.exe virtinp.exe \
  compose.exe console.exe store.img fs.exe
./mkimage disk.img disk.vhdx "$@"

# The logout disk holds the system disk's files but for its store image,
# which holds a session whose program is the driver.
logout_disk() {
  for file in "$@"; do
    shift
    case $file in
      store.img) file=logout/store.img ;;
    esac
    set -- "$@" "$file"
  done
  ./mkimage logout.img logout.vhdx "$@"
}
logout_disk "$@"

# The test disk holds the same files, the data disk's check, TESTS, an
# empty file, which makes the kernel and the first process run their
# tests and report them, and NOMOTION, an empty file, which on a test
# disk makes the first process set ui.reduce-motion, so that the
# console's caret holds still for a screen to be compared.
: > TESTS
: > NOMOTION
set -- "$@" fstest.exe TESTS NOMOTION
./mkimage test.img test.vhdx "$@"

# The specimen disk holds the test disk's files and SPECIMEN, an empty
# file, which makes the first process give the display to the type
# specimen from /store in place of the console.
: > SPECIMEN
./mkimage specimen.img specimen.vhdx "$@" SPECIMEN

# The fallback disk holds the test disk's files and COMPFAIL, an empty
# file, which makes the compositor end just after the first frame that
# shows the console's window, at each start, so that it is given up and
# the console takes the display from it.
: > COMPFAIL
./mkimage fallback.img fallback.vhdx "$@" COMPFAIL

# The no-answer disk holds the test disk's files and NOANSWER, an empty
# file, which makes the compositor answer no connection, at each start,
# so that the first process ends it at its deadline, it is given up, and
# the console takes the display from it.
: > NOANSWER
./mkimage noanswer.img noanswer.vhdx "$@" NOANSWER

# The window disk holds the test disk's files, wintest.exe, the window test
# client, and WINDOW, an empty file, which makes the first process start the
# client, which makes a window and shows two frames in it, in the console's
# place as the compositor's client.
: > WINDOW
./mkimage window.img window.vhdx "$@" wintest.exe WINDOW

# The chain disk holds the test disk's files and, under 8.3 names, the
# inputs of stage0-pe32's chain up to M1, unmodified, which the
# bootstrap check reads.
mkdir -p chain
while read -r name url; do
  fetch "chain/$name" "$url"
  set -- "$@" "chain/$name"
done <<EOF
hex0seed.exe $STAGE0/bootstrap-seeds/PE32/i386/hex0-seed.exe
hex0_x86.hex $STAGE0/x86/hex0_x86.hex0
hex1_x86.hex $STAGE0/x86/hex1_x86.hex0
hex2_x86.hex $STAGE0/x86/hex2_x86.hex1
catm_x86.hex $STAGE0/x86/catm_x86.hex2
pe32i386.hex $STAGE0/x86/PE32-i386.hex2
ntdll386.hex $STAGE0/x86/ntdll-i386.hex2
m0_x86.hex $STAGE0/x86/M0_x86.hex2
cc_x86.m1 $STAGE0/x86/cc_x86.M1
libccore.m1 $STAGE0/x86/libc-core.M1
libcboot.m1 $STAGE0/x86/libc-bootstrap.M1
pe-end.m1 $STAGE0/x86/pe-end-shim.M1
x86_defs.m1 $M2LIBC/x86/x86_defs.M1
bootstrp.c $M2LIBC/x86/windows/bootstrap.c
bootable.c $M2LIBC/bootstrappable.c
cc.h $M2PLANET/cc.h
cc_globl.c $M2PLANET/cc_globals.c
cc_readr.c $M2PLANET/cc_reader.c
cc_strng.c $M2PLANET/cc_strings.c
cc_types.c $M2PLANET/cc_types.c
cc_emit.c $M2PLANET/cc_emit.c
cc_core.c $M2PLANET/cc_core.c
cc_macro.c $M2PLANET/cc_macro.c
cc.c $M2PLANET/cc.c
stringfy.c $MESCC_TOOLS/stringify.c
m1-macro.c $MESCC_TOOLS/M1-macro.c
EOF
./mkimage chain.img chain.vhdx "$@"

./mkdata data.img "$DATA_DISK_MIB"
