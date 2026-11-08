"""Boot a nova-os disk under QEMU and check what it reports.

Usage: python3 -I ci/boot.py --code CODE.fd --vars VARS.fd --log LOG
                             [--timeout SECONDS] [--expect LINE ...]
                             [--tests] [--data DATA [--corrupt-gpt KIND]]
                             [--fonts FONTS [--screen SCREEN.ppm]]
                             {system,persist,chain,specimen} DISK
                             [-- QEMU_ARG ...]

Boots DISK with the machine and devices of README.md's Run section,
headless, with the firmware CODE.fd and a copy of VARS.fd, a writable
copy of DATA as the data disk after it, and the QEMU arguments given
after --. The guest's serial port is echoed with the time since QEMU
started and written to LOG.

Every line nova-os writes has the form docs/spec/log.md T1 gives,
component: event key=value ..., and the boot is checked by the lines
it waits for, each matched by its component, its event and the keys
the check names, never by the rest of its text. With --tests, DISK is
a test disk (docs/spec/disk-layout.md), and the boot must also write a
whole test report in KTAP version 1 (log.md T2): the check prints the
report's plan, each test's result, and every test not ok with its
diagnostics, and fails on a missing version line or plan, a result out
of order, a count short of its plan, any test not ok, or a test the
boot must run missing or skipped: the kernel's self-tests, the first
process's checks of its calls, the test program from /store and the
boot disk's read, the data disk's check with a data disk whose GPT is
whole, and in chain mode the chain, each of its steps and each pinned
link. Without --tests, a report anywhere in the log fails the boot, and
the kernel must report its random number generator's self-test
passing. chain and specimen take only a test disk.

In system mode, the boot must report the store service serving, startup
done, the background drawn, the console drawn, its panel's backdrop
made from the kernel's background or, on a test disk, whose console is
told of a display the background does not match, computed
(docs/spec/console.md C5), and the console taking the description of
each of the keyboard, the mouse and the tablet, the tablet's with its
axes' range. Then, sent through QMP, QEMU's monitor
protocol, the tablet moves, each of three buttons is clicked, the wheel
turns up and down and the mouse moves, and the console must report
each; no position sent may appear in a line of the console or an input
driver. With a data disk whose GPT is whole, the accounts service must
report its self-test passing and no account, and login must start;
then, typed through QMP, the first account is created, the shell
starts, and the shell must report running whoami for it; the password,
typed at the shell's prompt too, must appear nowhere in the log. The
session then makes a directory in its home, writes a file in it twice,
shows it, moves it, writes and removes an empty file, and is refused a
directory under /store and a file in another user's home. The shell
must log each command's status, and then the data disk, read on the
host as docs/spec/pool-format.md lays it out, must hold the account's
home alone in /users and the command's change in the home, and nothing
else there; no word of a name or line typed for the commands may
appear in the log. In system mode the session then writes a file of
one-character lines, which fill the console's panel past a screen, and
once the screen shows the shell's prompt after them the wheel turns
back and forward through QMP. Before the turns and after each, QMP's
screendump must come to equal, within SCREEN_TIMEOUT, the screen
ci/screens.c draws from the same lines, prompt and turns, with the
caret in either phase of its blink, built with the host's C compiler,
CC if set. In persist mode the same copy of the data disk is booted
twice: the first boot is system mode's but for the wheel, and the
second must report the account the first made and, typed again, log in
with it, run whoami and show the file, which the disk must still hold.
In chain mode, the boot of chain.img must report the chain's steps
passing in its test report. In specimen mode, the boot of specimen.img
must report the display given to the type specimen from /store and the
specimen drawn; then QMP's screendump, kept at SCREEN.ppm if given,
must equal pixel for pixel the screen design/text/specimen.py draws at
its size with the nova-os fonts in FONTS. In every mode but chain, the
kernel must also report the frames the boot screen's spinner started
while the background was prepared, from the start of the kernel's
drawing to the first process's startup report: at least one, unless
the preparation ended within the longest a turning spinner goes
between frames.
Each --expect LINE, in T1's form, adds a line the boot must report,
with the keys LINE gives. With a data disk, the boot must also report
the file system service opening its pool and /data and /users
published. With --corrupt-gpt, the copy's two GPT tables are changed
first: header or entries change a byte of each header's CRC or of each
entry array; extent moves the pool's entry to start at LBA 0, below the
first usable LBA; usable moves the entry and the first usable LBA both
to LBA 1, the header's; overlap adds a second entry over the pool's
blocks; the last three with every CRC made to match. The boot must then
report the service refusing the disk for that check, /data and /users
not published, and the accounts service and login not started.

The check fails at once on a line from nova-os that reports a failure,
as T1's table gives them, or if QEMU ends, which it does on a reset,
and otherwise when the timeout passes with a line not yet reported.
"""

import argparse
import hashlib
import json
import os
import queue
import re
import shlex
import shutil
import socket
import string
import subprocess
import sys
import tempfile
import threading
import time
import zlib

# README.md's Run command, with the display off, the serial port on
# standard output, a QMP socket for the keys and the pointer, and a
# virtio mouse, so that a relative pointer's events reach a driver too.
# The mouse comes before the tablet: QEMU sends a button or the wheel to
# the device whose driver set DRIVER_OK last of those that take them
# (hw/input/virtio-input.c, virtio_input_set_status; ui/input.c,
# qemu_input_find_handler), and the input drivers start in the order of
# the PCI walk, so the tablet takes them, as it does in the Run command.
# -no-reboot makes a reset end QEMU, so that the lines of a second boot
# cannot complete the first's.
MEMORY = "256M"
DEVICES = ["-net", "none", "-device", "virtio-rng-pci",
           "-device", "virtio-keyboard-pci", "-device", "virtio-mouse-pci",
           "-device", "virtio-tablet-pci", "-no-reboot"]

# A line of nova-os's log (docs/spec/log.md T1): its component, the
# event's words, and its keys, each value bare or in double quotes. A
# line can follow other output on the serial port, so it is found where
# its component starts.
COMPONENT = r"nova(?:-[a-z0-9-]+)?"
EVENT_LINE = re.compile(r"(%s): ([a-z0-9-]+(?: [a-z0-9-]+)*)"
                        r"((?: [a-z][a-z0-9_]*=(?:\"[^\"]*\"|[^ \"]+))*)$"
                        % COMPONENT)
LINE_START = re.compile(r"%s: " % COMPONENT)
KEY = re.compile(r' ([a-z][a-z0-9_]*)=(?:"([^"]*)"|([^ "]+))')


def parse_line(text):
    """The component, event and keys of the nova-os line in text, or
    None."""
    for start in LINE_START.finditer(text):
        match = EVENT_LINE.match(text, start.start())
        if match:
            return (match.group(1), match.group(2),
                    {k.group(1): k.group(2) if k.group(2) is not None
                     else k.group(3) for k in KEY.finditer(match.group(3))})
    return None


class Event:
    """A line the boot must report: its component and event, with keys,
    each a value it must have, or a pattern it must match whole, and
    keys it must not have."""

    def __init__(self, line, absent=(), **patterns):
        self.text = line
        self.component, self.event, self.keys = parse_line(line)
        self.keys.update(patterns)
        self.absent = absent

    def matches(self, parsed):
        if not parsed or parsed[:2] != (self.component, self.event):
            return False
        keys = parsed[2]
        for key, wanted in self.keys.items():
            if key not in keys:
                return False
            if isinstance(wanted, re.Pattern):
                if not wanted.fullmatch(keys[key]):
                    return False
            elif keys[key] != wanted:
                return False
        return not any(key in keys for key in self.absent)

    def __str__(self):
        return self.text + "".join(" no %s" % key for key in self.absent)


# A key's value that may be anything, for a key the line must have.
ANY_VALUE = re.compile(r".+")

RANDOM_PASSED = Event("nova: selftest passed name=random")
STARTUP_DONE = Event("nova: startup done")

# The console's line for each endpoint's description
# (include/input_protocol.h): QEMU's keyboard, its mouse, which has no
# absolute axis, and its tablet, whose axes run from 0 to 0x7FFF
# (include/ui/input.h, INPUT_EVENT_ABS_MAX).
TABLET_MAX = 0x7FFF
INPUT_LINES = [
    Event("nova-console: input kind=keyboard"),
    Event("nova-console: input kind=pointer", absent=("x", "y")),
    Event("nova-console: input kind=pointer x=0..%d y=0..%d"
          % (TABLET_MAX, TABLET_MAX)),
]

# The kernel draws the background offscreen while the boot screen's
# spinner turns, hands it to the first process and waits for the
# process's startup report, which follows the console's preparation of
# its panel; it logs how long that preparation took, from the drawing's
# start to the report, in milliseconds to the tenth, rounded down, and
# the frames the spinner started over it, as keys of its background
# line. A turning spinner starts a frame at least every SPINNER_GAP_MS,
# the sum of its parts below, while one other thread is ready, the
# drawing or the console's preparation; each further thread ready
# meanwhile delays it by a slice more. The bound takes the ticks to come
# on time, which under TCG they need not, but there the preparation
# takes over ten times as long. A preparation that may have lasted
# longer than the bound must show MIN_SPINNER_FRAMES; one that ended
# within it may show none.
BACKGROUND = Event("nova: background", prepare_ms=re.compile(r"\d+\.\d"),
                   spinner_frames=re.compile(r"\d+"))
MIN_SPINNER_FRAMES = 1
TENTHS_PER_MS = 10
# The bound's parts, each the kernel's: the spinner's next frame falls
# due within FRAME_MS (kernel/main.c); its wait ends at the first timer
# tick at or after that, and a tick comes every TICK_MS, at TIMER_HZ
# (kernel/x86_64/arch.h); it then waits behind the thread that is
# ready, which keeps the processor for its TIME_SLICE_MS
# (kernel/sched/thread.h) and on to the first tick after the slice
# ends. sched_timer_interrupt (kernel/sched/thread.c) ends the wait and
# the slice at those ticks.
FRAME_MS = 33
TICK_MS = 1
TIME_SLICE_MS = 10
SPINNER_GAP_MS = FRAME_MS + TICK_MS + TIME_SLICE_MS + TICK_MS

# The console's lines on its panel's backdrop: on the system disk, its
# line once it draws the panel, made from the kernel's background; on a
# test disk, whose console the first process tells of a display a row
# taller than the background (init/console.c, MISMATCH_ROWS), its
# refusal of the background for that, and the panel's Limb computed at
# the display.
CONSOLE_DRAWN = {
    False: [Event("nova-console: display backdrop=section",
                  screen=ANY_VALUE)],
    True: [Event('nova-console: background refused reason="another size"'),
           Event("nova-console: display backdrop=computed",
                 screen=ANY_VALUE)],
}

SYSTEM_LINES = [
    Event("nova-store: serving"),
    STARTUP_DONE,
    BACKGROUND,
] + INPUT_LINES

CHAIN_LINES = [STARTUP_DONE]

# The specimen's boot reports what a system boot does up to the
# display's release, and then the specimen drawing in the console's
# place.
SPECIMEN_LINES = [
    Event("nova-store: serving"),
    STARTUP_DONE,
    BACKGROUND,
    Event("nova-init: display given",
          path=re.compile(r"/store/[^/]+-specimen/bin/specimen\.exe")),
    Event("nova-specimen: drawn", screen=ANY_VALUE),
]

SPECIMEN_PROTOTYPE = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                  "..", "design", "text", "specimen.py")
SCREEN_COPY = "screen.ppm"
PROTOTYPE_COPY = "prototype.ppm"
EXPECTED_COPY = "expected.ppm"

# The tests a test disk's report must hold, passed and not skipped, by
# their names from the report's top (docs/spec/log.md T2): the kernel's
# self-tests, the first process's, with the data disk's check only with
# a data disk whose GPT is whole and the chain only from chain.img, and
# in the chain, each step, named by the file it writes, the links among
# them those nova-nix pins.
KERNEL_TESTS = ["kernel"] + ["kernel/" + name
                             for name in ("page", "random", "object",
                                          "thread")]
INIT_TESTS = ["init", "init/calls", "init/hello", "init/boot-disk"]
FSTEST = "init/fstest"
CHAIN = "init/chain"
CHAIN_STEPS = 20
CHAIN_LINKS = ["hex0", "hex1", "hex2", "catm", "M0", "cc_x86", "M2", "M1"]
# A link's passing result carries "pinned" and its name, which shows its
# pin was checked.
CHAIN_LINK_DATA = {"%s/%s.exe" % (CHAIN, link): "pinned " + link
                   for link in CHAIN_LINKS}

# ci/screens.c, which draws the screens a boot must show on the host with
# the guest's own drawing code, built as its header gives with the
# host's C compiler, CC if set; and how long a step's screen may take to
# show (docs/spec/display.md D19).
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
SCREENS_SOURCES = ["ci/screens.c", "console/text.c", "console/view.c",
                   "console/panel.c", "gfx/gfx.c", "gfx/font.c",
                   "gfx/raster.c", "gfx/limb.c"]
SCREENS_INCLUDES = ["include", "gfx", "console", "lib"]
SCREENS_FLAGS = ["-std=c11", "-O2", "-Wall", "-Wextra", "-pedantic",
                 "-Werror"]
SCREENS_PROGRAM = "screens"
CONSOLE_SCREEN = "console"
TYPED_LINE = "line"
WRITTEN = "write"
WHEEL_TURNED = "wheel"
CARET = "caret"
CARET_PHASES = ["on", "off"]
SCREEN_TIMEOUT = 60
PPM_MAGIC = b"P6"
PPM_MAXVAL = b"255"
PPM_FIELDS = 4

DEFAULT_TIMEOUTS = {"system": 300, "persist": 300, "chain": 2700,
                    "specimen": 300}

DATA_LINES = [
    Event("nova-fs: pool opened", lba=ANY_VALUE),
    Event("nova-init: data published"),
]

# What --corrupt-gpt changes in the data disk (UEFI 2.11 5.3), in the
# primary table at LBA 1 and the backup it names: a byte of each
# header's HeaderCRC32, or of the first entry's PartitionName in each
# entry array, which only a CRC check refuses; or, with each array's
# CRC and each header's made to match, the first entry, the pool's as
# mkdata writes it, moved to start at LBA 0; or moved to start at LBA 1,
# the header's, with each header's FirstUsableLBA moved there too, so
# that only the header's own placing of its usable LBAs is wrong; or a
# second entry, a Linux filesystem's, over the same blocks: which only
# the checks of 5.3.1 refuse. The usable range and the entry both moved
# to LBA 0 instead kept EDK2 from booting the system disk at all. Both
# tables are changed because firmware restores a primary table that
# fails from a backup that passes, as 5.3.2 asks and EDK2 does on a
# writable disk.
SECTOR = 512
PRIMARY_LBA = 1
HEADER_SIZE = 12
HEADER_CRC = 16
ALTERNATE_LBA = 32
FIRST_USABLE = 40
ENTRIES_LBA = 72
ENTRY_COUNT = 80
ENTRY_SIZE = 84
ENTRIES_CRC = 88
ENTRY_UNIQUE = 16
ENTRY_FIRST = 32
ENTRY_LAST = 40
ENTRY_NAME = 56
GUID_SIZE = 16
LINUX_FILESYSTEM = bytes.fromhex("af3dc60f838472478e793d69d8477de4")
GPT_REFUSED = {
    "header": Event("nova-fs: gpt refused check=header"),
    "entries": Event("nova-fs: gpt refused check=entries"),
    "extent": Event("nova-fs: gpt refused check=extent"),
    "usable": Event("nova-fs: gpt refused check=layout"),
    "overlap": Event("nova-fs: gpt refused check=extent"),
}
DATA_REFUSED = Event("nova-init: data unpublished")
LOGIN_ABSENT = Event('nova-init: login unavailable reason="no /users"')

# The pool format, version 1, as include/pool_format.h numbers it: the
# pool partition's type GUID in GPT's byte order; a superblock's fields;
# a block pointer's; the root block's; a node record's; a directory
# entry's; and the node kinds.
POOL_TYPE = bytes([0x24, 0x8f, 0x76, 0xb1, 0x5a, 0x22, 0x6d, 0x43,
                   0xaa, 0xbd, 0x66, 0x9b, 0x87, 0x4b, 0xac, 0x7f])
POOL_BLOCK = 4096
POOL_RING_SLOTS = 4
DIGEST_SIZE = 32
POOL_MAGIC = b"NOVAPOOL"
POOL_VERSION = 1
SUPER_VERSION = 8
SUPER_BLOCK_SIZE = 12
SUPER_BLOCKS = 16
SUPER_TXG = 24
SUPER_ROOT = 32
SUPER_DIGEST = 96
POINTER_FIRST = 0
POINTER_SECOND = 8
POINTER_DIGEST = 32
POINTER_SIZE = 64
POOL_FANOUT = POOL_BLOCK // POINTER_SIZE
ROOT_TABLE_LEVELS = 16
ROOT_TABLE = 24
RECORD_KIND = 0
RECORD_LEVELS = 2
RECORD_SIZE_FIELD = 8
RECORD_ROOT = 16
RECORD_SIZE = 128
RECORDS_PER_BLOCK = POOL_BLOCK // RECORD_SIZE
ROOT_SLOT = 0
POOL_ENTRY_SLOT = 0
POOL_ENTRY_KIND = 4
POOL_ENTRY_LENGTH = 5
POOL_ENTRY_NAME = 6
DIRECTORY = "directory"
FILE = "file"
KINDS = {1: DIRECTORY, 2: FILE}

# The account the boots make and log in with: a name and a password
# typed as QMP's send-key names their keys. The name is logged, as the
# accounts service and the shell log it; the password must never be,
# and is typed once more at the shell's prompt, where a person who took
# a drawn prompt for login's would type it, to check that the shell
# keeps it out of the log too. It has no space, so that a log of the
# first word typed would hold all of it.
ACCOUNT_NAME = "ci"
ACCOUNT_PASSWORD = "civerifies"
QCODES = {" ": "spc", ".": "dot", "/": "slash"}

# The account's home on the data disk's volume, which must be the only
# entry of /users.
USERS = ("users",)
HOME = USERS + (ACCOUNT_NAME,)

# What login and the accounts service report before login reads its
# first line, and then on the first boot and on a later one.
# The accounts service's self-test of BLAKE2b and Argon2id
# (docs/spec/log.md T3) is reported before its count.
ACCOUNTS_PASSED = Event("nova-accounts: selftest passed name=argon2id")
LOGIN_READY = Event("nova-login: ready session=console")
FIRST_BOOT_READY = [ACCOUNTS_PASSED, Event("nova-accounts: ready accounts=0"),
                    LOGIN_READY]
LATER_BOOT_READY = [ACCOUNTS_PASSED, Event("nova-accounts: ready accounts=1"),
                    LOGIN_READY]
SESSION = Event("nova-shell: session name=%s role=administrator"
                % ACCOUNT_NAME)
CREATED = [Event("nova-accounts: account created name=%s id=1 "
                 "role=administrator" % ACCOUNT_NAME), SESSION]
LOGGED_IN = [Event("nova-accounts: login accepted name=%s" % ACCOUNT_NAME),
             SESSION]
WHOAMI = "whoami"
WHOAMI_RAN = Event("nova-shell: whoami name=%s id=1 role=administrator"
                   % ACCOUNT_NAME)
NO_COMMAND = Event("nova-shell: command unknown user=%s" % ACCOUNT_NAME)

# The file commands the first boot's session runs in its home, each as
# the lines typed for it, the status the shell must log for it, in a
# line that names the command and the status alone, and the tree the
# home must then hold on the data disk, read on the host: each path
# below the home, with a file's bytes or DIRECTORY. A directory is made,
# a file written and written again, which replaces it, shown and moved,
# an empty file written and removed; then a directory is refused under
# /store, which the session holds only to read, and a file in another
# user's home, which it holds no handle to. A later boot shows the file
# again. Each command's own flush is what puts its change on the disk:
# the shell logs the status once the flush is answered, and nothing
# else writes the volume while the session runs.
STATUS_RAN = "nova-shell: command ended user=%s name=%%s status=%%d" % (
    ACCOUNT_NAME)
OK = 0
ERR_ACCESS_DENIED = 6
ERR_NOT_FOUND = 16
END_OF_LINES = "."
FIRST_DRAFT = "apricot compote"
FINAL_LINES = ["quince marmalade", "damson chutney"]


def file_of(lines):
    return "".join(line + "\n" for line in lines).encode("ascii")


MADE = {"notes": DIRECTORY}
DRAFTED = {**MADE, "notes/draft": file_of([FIRST_DRAFT])}
REPLACED = {**MADE, "notes/draft": file_of(FINAL_LINES)}
MOVED = {**MADE, "notes/final": file_of(FINAL_LINES)}
STALE = {**MOVED, "notes/stale": file_of([])}
FILE_COMMANDS = [
    (["mkdir notes"], "mkdir", OK, MADE),
    (["write notes/draft", FIRST_DRAFT, END_OF_LINES], "write", OK, DRAFTED),
    (["write notes/draft"] + FINAL_LINES + [END_OF_LINES], "write", OK,
     REPLACED),
    (["cat notes/draft"], "cat", OK, REPLACED),
    (["mv notes/draft notes/final"], "mv", OK, MOVED),
    (["write notes/stale", END_OF_LINES], "write", OK, STALE),
    (["rm notes/stale"], "rm", OK, MOVED),
    (["mkdir /store/notes"], "mkdir", ERR_ACCESS_DENIED, MOVED),
    (["write /users/nobody/notes"], "write", ERR_NOT_FOUND, MOVED),
]
LATER_FILE_COMMANDS = [(["cat notes/final"], "cat", OK, MOVED)]

# The wheel's check (docs/spec/display.md D19, phase 1). In a system
# boot the session then writes a file of SCROLL_LINES, each echoed on a
# line of the console's panel of its own, as many as the panel's 30
# lines and the 6 the turns below scroll back, so that the view turned
# back shows lines typed alone, which ci/screens.c draws. Each is
# one character because QEMU queues at most 1,024 key events, syncs and
# delays, six a key, and drops a key past them (ui/input.c, queue_limit,
# qmp_send_key; ui/input-legacy.c in 8.2.2), so the command and its
# lines, typed before the status it logs is awaited, stay under 170
# keys; the home must then hold the file whole, so no key was dropped.
SCROLL_LINES = list(string.ascii_lowercase + string.digits)
SCROLL_FILE = "notes/alphabet"
SCROLL_COMMAND = (["write " + SCROLL_FILE] + SCROLL_LINES + [END_OF_LINES],
                  "write", OK, {**MOVED, SCROLL_FILE: file_of(SCROLL_LINES)})

# The shell's prompt after the write, which ci/screens.c draws on the
# line after the dot: the account's name, "@nova ", the working
# directory, its home, and "> " (shell/shell.c, prompt). The screen with
# it drawn and its read waiting is matched before the wheel turns, since
# the shell logs the write's status before it writes the prompt, and a
# write brings the view back to the end.
PROMPT = "%s@nova /%s> " % (ACCOUNT_NAME, "/".join(HOME))

# The wheel's turns, each detent a press and release of QMP's wheel-up or
# wheel-down in one input-send-event, which QEMU sends as REL_WHEEL 1 or
# -1 and a release no driver takes, ended by one SYN_REPORT
# (hw/input/virtio-input-hid.c, ui/input.c), with the screen matched
# after each turn: two detents back, one forward, two more forward,
# which the end stops after one, and two back, which show again what the
# first two did. Each screen differs from every one the turns before it
# leave on the screen, so it matches only once the console has taken
# them.
WHEEL_TURNS = [2, -1, -2, 2]

# The pointer's events, each one QMP input-send-event command, which
# QEMU ends with one SYN_REPORT (ui/input.c, qmp_input_send_event): the
# tablet moved to POINTER_AT, whose numbers no other line of the log
# holds; each button the console names pressed and released; the wheel
# turned up and down, each turn a press, which QEMU sends as REL_WHEEL,
# and a release, which it sends as a button no driver takes
# (hw/input/virtio-input-hid.c); and the mouse moved, which alone takes
# relative motion. Then the line the console must log for each.
POINTER_AT = {"x": 13579, "y": 24680}
BUTTONS = ["left", "right", "middle"]


def button(name, down):
    return [{"type": "btn", "data": {"button": name, "down": down}}]


def move(kind, at):
    return [{"type": kind, "data": {"axis": axis, "value": value}}
            for axis, value in at.items()]


POINTER_EVENTS = ([move("abs", POINTER_AT)]
                  + [button(name, down)
                     for name in BUTTONS + ["wheel-up", "wheel-down"]
                     for down in (True, False)]
                  + [move("rel", {"x": 7, "y": -3})])
POINTER_LINES = ([Event("nova-console: pointer move mode=absolute axes=xy")]
                 + [Event("nova-console: pointer %s button=%s"
                          % (change, name))
                    for name in BUTTONS
                    for change in ("press", "release")]
                 + [Event("nova-console: pointer wheel direction=up"),
                    Event("nova-console: pointer wheel direction=down"),
                    Event("nova-console: pointer move mode=relative "
                          "axes=xy")])

# What no line of the log may hold: the password, each word of the names
# and lines typed for the file commands, words no other line of the log
# holds, so that a log of any of them is found, but for the wheel's
# lines, single characters, which other lines hold, and, in a line of the
# console or an input driver, the only processes that see it, the
# pointer's position as sent, which the driver passes on as it is, as a
# whole number. Other lines hold numbers that change from run to run,
# such as the kernel's timings and the generator's first bytes, which
# can hold a position's digits.
def literal(word):
    return re.compile(re.escape(word.encode("ascii")))


SEES_POSITION = rb"nova-(?:console|virtio-input): .*"


def whole_number(value):
    return re.compile(SEES_POSITION + rb"(?<![0-9])%d(?![0-9])" % value)


UNLOGGED = ([(literal(ACCOUNT_PASSWORD), "the account's password")]
            + [(literal(word), "%r, typed for a file command" % word)
               for word in ["notes", "draft", "final", "stale", "nobody",
                            "alphabet"]
               + " ".join([FIRST_DRAFT] + FINAL_LINES).split()]
            + [(whole_number(value), "the pointer's %s" % axis)
               for axis, value in POINTER_AT.items()])

# The accounts service hashes each password with Argon2id over 64 MiB in
# 3 passes, which takes about 12 seconds under TCG on an Apple M2 and
# can take several times that on a shared runner; a line typed and
# answered without a hash takes a second or two.
HASH_TIMEOUT = 300
KEY_TIMEOUT = 60

# The lines that report a failure (docs/spec/log.md T1): an event that
# ends with FAILED, and these.
FAILED = "failed"
FAILURES = {("nova", "halted"), ("nova", "exception"),
            ("nova", "user exception"), ("nova", "first process ended"),
            ("nova", "random unseeded"), ("nova-init", "child restarted"),
            ("nova-init", "child given up")}


def reports_failure(parsed):
    component, event, _ = parsed
    return (event.split()[-1] == FAILED
            or (component, event) in FAILURES)

# A test report in KTAP version 1 (docs/spec/log.md T2, as
# docs/spec/README.md pins it): a version line, a plan, then a result
# for each test, a failure's diagnostics before it, and a test's own
# tests, a whole report of their own one KTAP_INDENT further in, just
# before its result. A line that is none of these is an unknown line,
# which KTAP allows anywhere.
KTAP_VERSION = "KTAP version 1"
KTAP_INDENT = "  "
KTAP_PLAN = re.compile(r"1\.\.(\d+)")
KTAP_RESULT = re.compile(r"(ok|not ok) (\d+)(?: ([^#]*?))?(?: # (.*))?")
KTAP_DIAGNOSTIC = "#"
KTAP_SKIP = re.compile(r"SKIP(?: (.*))?")


class KtapList:
    """The tests at one depth of a report: the plan, once read, and each
    test's result as it comes."""

    def __init__(self, depth):
        self.depth = depth
        self.plan = None
        self.tests = []


class KtapTest:
    """A test's result: its name, whether it passed, the reason it was
    skipped, or None if it ran, the data after its hash sign if it ran,
    the diagnostics written at its depth since the result before it, the
    list of its own tests, or None, and whether its result line came, or
    the test stands for a list whose result never did."""

    def __init__(self, name, passed, skipped, data, diagnostics, tests,
                 reported=True):
        self.name = name
        self.passed = passed
        self.skipped = skipped
        self.data = data
        self.diagnostics = diagnostics
        self.tests = tests
        self.reported = reported


def reported(tests):
    """How many of tests' results came."""
    return sum(1 for test in tests.tests if test.reported)


class Ktap:
    """A boot's test report, read a line at a time. done once the top
    plan's last result has come; errors notes each line out of place."""

    def __init__(self):
        self.lists = []
        self.top = None
        self.diagnostics = {}
        self.errors = []
        self.done = False

    def feed(self, line):
        text = line.rstrip()
        indent = len(text) - len(text.lstrip(" "))
        text = text.strip()
        if self.done or not text or indent % len(KTAP_INDENT):
            return
        depth = indent // len(KTAP_INDENT)
        if text == KTAP_VERSION:
            self.version(depth)
            return
        plan = KTAP_PLAN.fullmatch(text)
        result = KTAP_RESULT.fullmatch(text)
        if depth >= len(self.lists):
            if self.top is not None and (plan or result):
                self.errors.append("a %s at depth %d with no version line "
                                   "before it" % ("plan" if plan
                                                  else "result", depth))
            return
        if plan and depth == len(self.lists) - 1:
            self.set_plan(self.lists[depth], int(plan.group(1)))
        elif result:
            self.result(depth, result)
        elif text.startswith(KTAP_DIAGNOSTIC):
            self.diagnostics.setdefault(depth, []).append(
                text[len(KTAP_DIAGNOSTIC):].strip())

    def version(self, depth):
        if depth == 0 and self.top is None:
            self.top = KtapList(0)
            self.lists = [self.top]
        elif depth == len(self.lists) and depth > 0:
            self.lists.append(KtapList(depth))
        else:
            self.errors.append("a version line at depth %d out of place"
                               % depth)

    def set_plan(self, tests, count):
        if tests.plan is not None or tests.tests:
            self.errors.append("a plan at depth %d after its first"
                               % tests.depth)
        else:
            tests.plan = count

    def close(self, tests, name):
        """Check that tests, the list of the test name, holds its plan."""
        if tests.plan is None:
            self.errors.append("%s: no plan" % name)
        elif len(tests.tests) != tests.plan:
            self.errors.append("%s: %d results for its plan of 1..%d"
                               % (name, len(tests.tests), tests.plan))

    def result(self, depth, match):
        """A result at depth, which ends a deeper list if one is open:
        that list is this test's own."""
        number, name = int(match.group(2)), (match.group(3) or "").strip()
        own = None
        while len(self.lists) > depth + 1:
            deeper = self.lists.pop()
            if len(self.lists) == depth + 1:
                own = deeper
            else:
                self.close(deeper, "a list at depth %d" % deeper.depth)
        if own:
            self.close(own, name)
        tests = self.lists[depth]
        if tests.plan is None:
            self.errors.append("%s: a result at depth %d before a plan"
                               % (name, depth))
        if number != len(tests.tests) + 1:
            self.errors.append("%s: numbered %d, not %d"
                               % (name, number, len(tests.tests) + 1))
        data = match.group(4)
        skip = KTAP_SKIP.fullmatch(data or "")
        tests.tests.append(KtapTest(
            name, match.group(1) == "ok",
            (skip.group(1) or "") if skip else None,
            None if skip else data, self.diagnostics.pop(depth, []), own))
        for deeper in [d for d in self.diagnostics if d > depth]:
            del self.diagnostics[deeper]
        if depth == 0 and tests.plan == len(tests.tests):
            self.done = True

    def finish(self):
        """Check every list still open, once no more lines will come. An
        open list below the top is the list of a test whose result never
        came, which is added to the list above it as not ok, so that its
        tests are reported."""
        unfinished = []
        for depth in range(len(self.lists) - 1, -1, -1):
            tests = self.lists[depth]
            if depth == 0:
                self.close(tests, "the report")
                continue
            above = self.lists[depth - 1]
            name = "<test %d, no result>" % (len(above.tests) + 1)
            self.close(tests, name)
            unfinished.append((above, name, tests))
        for above, name, tests in reversed(unfinished):
            above.tests.append(KtapTest(name, False, None, None, [], tests,
                                        reported=False))
        self.lists = []

    def walk(self, tests=None, prefix=""):
        """Each test, below tests or the top, with its path of names."""
        for test in (tests or self.top).tests:
            path = prefix + test.name
            yield path, test
            if test.tests:
                yield from self.walk(test.tests, path + "/")

    def problems(self, required, plans, skipped, data):
        """What fails the report: no version line, a line out of place, a
        test not ok, a test of required missing, failed or skipped, a
        list of plans' paths whose plan is not the count given, a test of
        skipped that ran, or a test of data's paths whose result does not
        carry the data given. A report that stopped short names no
        missing test: its count does."""
        if self.top is None:
            return ["no test report: no %r line" % KTAP_VERSION]
        found = dict(self.walk())
        problems = list(self.errors)
        problems += ["%s: not ok" % path for path, test in found.items()
                     if not test.passed]
        for path in required:
            test = found.get(path)
            if test is None:
                if self.done:
                    problems.append("%s: not reported" % path)
            elif test.skipped is not None:
                problems.append("%s: skipped, %s" % (path, test.skipped))
        for path, count in plans.items():
            test = found.get(path)
            if test and test.skipped is None and (
                    not test.tests or test.tests.plan != count):
                problems.append("%s: not %d tests" % (path, count))
        for path in skipped:
            test = found.get(path)
            if test and test.skipped is None:
                problems.append("%s: ran, where it must be skipped" % path)
        for path, wanted in data.items():
            test = found.get(path)
            if test and test.passed and test.data != wanted:
                problems.append("%s: carries %r, not %r"
                                % (path, test.data, wanted))
        return problems

    def summary(self):
        """The report as the check prints it: the plan, each test with its
        own plan, and every test not ok or skipped with its
        diagnostics or reason."""
        if self.top is None:
            return ["ktap: no report"]
        lines = ["ktap: plan 1..%s, %d reported"
                 % (self.top.plan, reported(self.top))]
        for path, test in self.walk():
            plan = (" (1..%s, %d reported)"
                    % (test.tests.plan, reported(test.tests))
                    if test.tests else "")
            if not test.passed:
                lines.append("ktap: not ok %s%s" % (path, plan))
                lines += ["ktap:   # " + line for line in test.diagnostics]
            elif test.skipped is not None:
                lines.append("ktap: skipped %s: %s" % (path, test.skipped))
            elif test.tests:
                lines.append("ktap: ok %s%s" % (path, plan))
        return lines


QMP_TIMEOUT = 30
QUIT_TIMEOUT = 30

# The data disk's writable copy, in the scratch directory.
DATA_COPY = "data.img"


class BootFailed(Exception):
    pass


def check_spinner(keys, line):
    """Fail if keys, those of the kernel's background line, show fewer
    than MIN_SPINNER_FRAMES over a preparation that may have lasted
    longer than SPINNER_GAP_MS; say so if they show fewer over one that
    ended within it. The logged time is rounded down, so the preparation
    ended within the bound only if a tenth of a millisecond more does."""
    frames = int(keys["spinner_frames"])
    whole, tenth = (int(part) for part in keys["prepare_ms"].split("."))
    if frames >= MIN_SPINNER_FRAMES:
        return
    if (whole * TENTHS_PER_MS + tenth + 1
            > SPINNER_GAP_MS * TENTHS_PER_MS):
        raise BootFailed("the spinner stood still: " + line)
    print("          the background was prepared within the %d ms a "
          "turning spinner can go between frames" % SPINNER_GAP_MS,
          flush=True)


class Serial:
    """The guest's serial lines, read from QEMU's standard output. What
    a line holds of UNLOGGED is noted in leaked. With report, a Ktap,
    every line is read into it; without, a report's version line fails
    the boot."""

    def __init__(self, stream, log, report):
        self.lines = queue.Queue()
        self.start = time.monotonic()
        self.leaked = set()
        self.report = report
        self.reader = threading.Thread(target=self.read, args=(stream, log))
        self.reader.start()

    def read(self, stream, log):
        for raw in stream:
            log.write(raw)
            log.flush()
            text = raw.rstrip(b"\r\n").replace(b"\r", b"")
            self.leaked.update(what for unlogged, what in UNLOGGED
                               if unlogged.search(text))
            self.lines.put(text.decode("ascii", "replace"))
        self.lines.put(None)

    def wait_for(self, wanted, deadline, report_done=False):
        """Read lines until each Event in wanted has matched one, and with
        report_done, until the test report has ended."""
        missing = list(wanted)
        while missing or (report_done and not self.report.done):
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise BootFailed("timed out; not reported:\n  "
                                 + self.unreported(missing, report_done))
            try:
                line = self.lines.get(timeout=remaining)
            except queue.Empty:
                continue
            if line is None:
                raise BootFailed("QEMU ended; not reported:\n  "
                                 + self.unreported(missing, report_done))
            print("%8.1f  %s" % (time.monotonic() - self.start, line),
                  flush=True)
            if self.report:
                self.report.feed(line)
            elif line.strip() == KTAP_VERSION:
                raise BootFailed("a test report from a disk that is not a "
                                 "test disk: " + line)
            parsed = parse_line(line)
            if not parsed:
                if LINE_START.search(line):
                    raise BootFailed("a line not in the log's form "
                                     "(docs/spec/log.md T1): " + line)
                continue
            if reports_failure(parsed):
                raise BootFailed("reported a failure: "
                                 + line[LINE_START.search(line).start():])
            if BACKGROUND.matches(parsed):
                check_spinner(parsed[2], line)
            missing = [event for event in missing
                       if not event.matches(parsed)]

    def unreported(self, missing, report_done):
        return "\n  ".join([str(event) for event in missing]
                           + (["the end of the test report"]
                              if report_done and not self.report.done
                              else []))


class Qmp:
    """QMP over a stream connected to QEMU's monitor socket."""

    def __init__(self, stream):
        self.stream = stream
        self.receive()
        self.execute("qmp_capabilities")

    def receive(self):
        line = self.stream.readline()
        if not line:
            raise BootFailed("QMP connection closed")
        return json.loads(line)

    def execute(self, command, **arguments):
        message = {"execute": command}
        if arguments:
            message["arguments"] = arguments
        self.stream.write(json.dumps(message).encode("ascii") + b"\n")
        self.stream.flush()
        while True:
            reply = self.receive()
            if "error" in reply:
                raise BootFailed("QMP %s: %s" % (command,
                                                 reply["error"]["desc"]))
            if "return" in reply:
                return reply["return"]


def type_line(qmp, text, shown=True):
    """Type text and Enter; a password is not printed."""
    print("          send-key %s, Enter" % (text if shown else "(password)"),
          flush=True)
    for name in [QCODES.get(c, c) for c in text] + ["ret"]:
        qmp.execute("send-key", keys=[{"type": "qcode", "data": name}])


def point(serial, qmp):
    """Send the pointer's events and wait for the console's lines."""
    for events in POINTER_EVENTS:
        print("          input-send-event %s"
              % ", ".join("%s %s" % (e["type"], " ".join(
                  str(v) for v in e["data"].values())) for e in events),
              flush=True)
        qmp.execute("input-send-event", events=events)
    serial.wait_for(POINTER_LINES, time.monotonic() + KEY_TIMEOUT)


def run_commands(serial, qmp, commands, data):
    """Type each command's lines, wait for the status it must log, and
    check the home's tree on data, the data disk, which QEMU holds open
    but writes no more until the next line is typed."""
    for lines, command, status, wanted in commands:
        for text in lines:
            type_line(qmp, text)
        serial.wait_for([Event(STATUS_RAN % (command, status))],
                        time.monotonic() + KEY_TIMEOUT)
        check_home(data, wanted)
        print("          the data disk holds the home %s left" % command,
              flush=True)


def log_in(serial, qmp, data, first):
    """Create the account on a first boot, or log in with it on a later
    one, then run whoami and the file commands in its shell, checking
    data, the data disk, after each."""
    type_line(qmp, ACCOUNT_NAME)
    type_line(qmp, ACCOUNT_PASSWORD, shown=False)
    if first:
        type_line(qmp, ACCOUNT_PASSWORD, shown=False)
    serial.wait_for(CREATED if first else LOGGED_IN,
                    time.monotonic() + HASH_TIMEOUT)
    type_line(qmp, WHOAMI)
    serial.wait_for([WHOAMI_RAN], time.monotonic() + KEY_TIMEOUT)
    type_line(qmp, ACCOUNT_PASSWORD, shown=False)
    serial.wait_for([NO_COMMAND], time.monotonic() + KEY_TIMEOUT)
    run_commands(serial, qmp,
                 FILE_COMMANDS if first else LATER_FILE_COMMANDS, data)


def drive(serial, qmp_path, data, accounts, first, screens, scratch):
    """Over QMP: send the pointer's events, then, with accounts, create
    the account or log in with it as log_in does, and with screens, the
    program ci/screens.c builds to, check the wheel as scroll does."""
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(QMP_TIMEOUT)
        connection.connect(qmp_path)
        with connection.makefile("rwb") as stream:
            qmp = Qmp(stream)
            point(serial, qmp)
            if accounts:
                log_in(serial, qmp, data, first)
            if screens:
                scroll(serial, qmp, data, screens, scratch)


def read_ppm(path):
    """The width, height and pixel bytes of a binary PPM of maxval 255,
    with no comment: the magic, width, height and maxval separated by
    whitespace, then one whitespace byte, then the pixels (Netpbm's PPM
    format)."""
    with open(path, "rb") as f:
        data = f.read()
    fields, at = [], 0
    for _ in range(PPM_FIELDS):
        while at < len(data) and data[at:at + 1].isspace():
            at += 1
        start = at
        while at < len(data) and not data[at:at + 1].isspace():
            at += 1
        fields.append(data[start:at])
    if (fields[0] != PPM_MAGIC or not fields[1].isdigit()
            or not fields[2].isdigit() or fields[3] != PPM_MAXVAL):
        raise BootFailed("%s is not a binary PPM of maxval 255" % path)
    width, height = int(fields[1]), int(fields[2])
    pixels = data[at + 1:]
    if len(pixels) != 3 * width * height:
        raise BootFailed("%s holds %d pixel bytes, not %d"
                         % (path, len(pixels), 3 * width * height))
    return width, height, pixels


def check_specimen(qmp_path, scratch, fonts, screen):
    """Take QMP's screendump, keep it at screen if given, and compare it
    with the prototype's specimen at its size."""
    shot = screen or os.path.join(scratch, SCREEN_COPY)
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(QMP_TIMEOUT)
        connection.connect(qmp_path)
        with connection.makefile("rwb") as stream:
            Qmp(stream).execute("screendump",
                                filename=os.path.abspath(shot))
    width, height, pixels = read_ppm(shot)
    wanted_path = os.path.join(scratch, PROTOTYPE_COPY)
    subprocess.run([sys.executable, "-I", SPECIMEN_PROTOTYPE, fonts,
                    str(width), str(height), wanted_path], check=True)
    _, _, wanted = read_ppm(wanted_path)
    differ = differing(pixels, wanted)
    if differ:
        raise BootFailed("the %dx%d screen differs from the specimen's "
                         "prototype in %d pixels" % (width, height, differ))
    print("          the %dx%d screen equals the specimen's prototype, "
          "sha256 %s" % (width, height, hashlib.sha256(pixels).hexdigest()),
          flush=True)


def differing(pixels, wanted):
    """How many pixels of two images of one size differ."""
    return sum(1 for i in range(0, len(pixels), 3)
               if pixels[i:i + 3] != wanted[i:i + 3])


def build_screens(scratch):
    """Build ci/screens.c in scratch; the program's path."""
    program = os.path.join(scratch, SCREENS_PROGRAM)
    command = (shlex.split(os.environ.get("CC") or "cc") + SCREENS_FLAGS
               + [flag for include in SCREENS_INCLUDES
                  for flag in ("-I", os.path.join(ROOT, include))]
               + ["-o", program]
               + [os.path.join(ROOT, source) for source in SCREENS_SOURCES])
    print(" ".join(command), flush=True)
    if subprocess.run(command).returncode != 0:
        raise BootFailed("ci/screens.c did not build")
    return program


def draw_screen(screens, scratch, width, height, inputs):
    """The pixels of the console's screen screens draws at width x height
    from inputs."""
    path = os.path.join(scratch, EXPECTED_COPY)
    command = [screens, CONSOLE_SCREEN, str(width), str(height)] + inputs
    with open(path, "wb") as out:
        if subprocess.run(command, stdout=out).returncode != 0:
            raise BootFailed("ci/screens.c drew no screen")
    return read_ppm(path)[2]


def match_screen(qmp, scratch, screens, inputs):
    """Take QMP's screendump until its pixels' SHA-256 is that of the
    console's screen screens draws at its size from inputs, with the
    caret in either phase of its blink, which a screen with no caret in
    view draws alike, or SCREEN_TIMEOUT passes."""
    shot = os.path.abspath(os.path.join(scratch, SCREEN_COPY))
    deadline = time.monotonic() + SCREEN_TIMEOUT
    qmp.execute("screendump", filename=shot)
    width, height, pixels = read_ppm(shot)
    wanted = [draw_screen(screens, scratch, width, height,
                          inputs + [CARET, phase])
              for phase in CARET_PHASES]
    digests = [hashlib.sha256(image).digest() for image in wanted]
    while hashlib.sha256(pixels).digest() not in digests:
        if time.monotonic() >= deadline:
            raise BootFailed("after %d s the %dx%d screen differs from "
                             "ci/screens.c's in %d pixels"
                             % (SCREEN_TIMEOUT, width, height,
                                min(differing(pixels, image)
                                    for image in wanted)))
        qmp.execute("screendump", filename=shot)
        _, _, pixels = read_ppm(shot)
    print("          the %dx%d screen equals ci/screens.c's, sha256 %s"
          % (width, height, hashlib.sha256(pixels).hexdigest()), flush=True)


def scroll(serial, qmp, data, screens, scratch):
    """Write the file of SCROLL_COMMAND, its lines filling the console's
    panel, wait for the shell's prompt after it, then turn the wheel as
    WHEEL_TURNS gives, and match each screen with the one screens draws
    from the lines, the prompt and the turns."""
    run_commands(serial, qmp, [SCROLL_COMMAND], data)
    inputs = [word for line in SCROLL_LINES + [END_OF_LINES]
              for word in (TYPED_LINE, line)] + [WRITTEN, PROMPT]
    match_screen(qmp, scratch, screens, inputs)
    for detents in WHEEL_TURNS:
        name = "wheel-up" if detents > 0 else "wheel-down"
        for _ in range(abs(detents)):
            print("          input-send-event btn %s" % name, flush=True)
            qmp.execute("input-send-event",
                        events=button(name, True) + button(name, False))
            inputs += [WHEEL_TURNED, "1" if detents > 0 else "-1"]
        match_screen(qmp, scratch, screens, inputs)


def stop(qemu):
    if qemu.poll() is None:
        qemu.terminate()
        try:
            qemu.wait(timeout=QUIT_TIMEOUT)
        except subprocess.TimeoutExpired:
            qemu.kill()
            qemu.wait()


def read_u64(disk, offset):
    disk.seek(offset)
    return int.from_bytes(disk.read(8), "little")


def read_u32(disk, offset):
    disk.seek(offset)
    return int.from_bytes(disk.read(4), "little")


def write_u32(disk, offset, value):
    disk.seek(offset)
    disk.write(value.to_bytes(4, "little"))


def flip(disk, offset):
    disk.seek(offset)
    byte = disk.read(1)[0]
    disk.seek(offset)
    disk.write(bytes([byte ^ 0xff]))


def move_to(lba):
    """A change that moves the first entry to start at lba."""
    def change(array, size):
        array[ENTRY_FIRST:ENTRY_FIRST + 8] = lba.to_bytes(8, "little")
    return change


def add_overlap(array, size):
    second = bytearray(array[:size])
    second[:GUID_SIZE] = LINUX_FILESYSTEM
    second[ENTRY_UNIQUE] ^= 0xff
    array[size:2 * size] = second


def rewrite_entries(disk, header, change, first_usable=None):
    """Change the entry array the header at LBA header names, and the
    header's FirstUsableLBA if first_usable is given, and make the
    array's CRC and the header's match."""
    base = header * SECTOR
    entries = read_u64(disk, base + ENTRIES_LBA) * SECTOR
    size = read_u32(disk, base + ENTRY_SIZE)
    length = read_u32(disk, base + ENTRY_COUNT) * size
    disk.seek(entries)
    array = bytearray(disk.read(length))
    change(array, size)
    disk.seek(entries)
    disk.write(array)
    write_u32(disk, base + ENTRIES_CRC, zlib.crc32(array))
    if first_usable is not None:
        disk.seek(base + FIRST_USABLE)
        disk.write(first_usable.to_bytes(8, "little"))
    header_size = read_u32(disk, base + HEADER_SIZE)
    disk.seek(base)
    fields = bytearray(disk.read(header_size))
    fields[HEADER_CRC:HEADER_CRC + 4] = bytes(4)
    write_u32(disk, base + HEADER_CRC, zlib.crc32(fields))


def corrupt_gpt(disk, what):
    """Change both GPT tables as --corrupt-gpt what asks."""
    backup = read_u64(disk, PRIMARY_LBA * SECTOR + ALTERNATE_LBA)
    for header in (PRIMARY_LBA, backup):
        if what == "header":
            flip(disk, header * SECTOR + HEADER_CRC)
        elif what == "entries":
            entries = read_u64(disk, header * SECTOR + ENTRIES_LBA)
            flip(disk, entries * SECTOR + ENTRY_NAME)
        elif what == "overlap":
            rewrite_entries(disk, header, add_overlap)
        elif what == "extent":
            rewrite_entries(disk, header, move_to(0))
        else:
            rewrite_entries(disk, header, move_to(PRIMARY_LBA), PRIMARY_LBA)


def field(data, at, size):
    return int.from_bytes(data[at:at + size], "little")


class Pool:
    """The data volume of the pool on a data disk, read as
    docs/spec/pool-format.md lays it out and include/pool_format.h
    numbers it: the newest valid superblock's tree, with every block
    checked against its pointer's digest."""

    def __init__(self, disk):
        self.disk = disk
        first, last = self.extent()
        self.start = first * SECTOR
        self.blocks = (last - first + 1) * SECTOR // POOL_BLOCK
        root = self.read(self.newest_root())
        self.table_levels = root[ROOT_TABLE_LEVELS]
        self.table = root[ROOT_TABLE:ROOT_TABLE + POINTER_SIZE]

    def extent(self):
        """The first and last LBA of the pool's partition."""
        entries = read_u64(self.disk, PRIMARY_LBA * SECTOR + ENTRIES_LBA)
        count = read_u32(self.disk, PRIMARY_LBA * SECTOR + ENTRY_COUNT)
        size = read_u32(self.disk, PRIMARY_LBA * SECTOR + ENTRY_SIZE)
        for index in range(count):
            self.disk.seek(entries * SECTOR + index * size)
            entry = self.disk.read(size)
            if entry[:GUID_SIZE] == POOL_TYPE:
                return (field(entry, ENTRY_FIRST, 8),
                        field(entry, ENTRY_LAST, 8))
        raise BootFailed("the data disk has no pool partition")

    def block(self, number):
        self.disk.seek(self.start + number * POOL_BLOCK)
        return self.disk.read(POOL_BLOCK)

    def newest_root(self):
        """The root pointer of the valid superblock of the newest
        commit."""
        newest = None
        for ring in (0, self.blocks - POOL_RING_SLOTS):
            for slot in range(POOL_RING_SLOTS):
                block = self.block(ring + slot)
                txg = field(block, SUPER_TXG, 8)
                digest = block[SUPER_DIGEST:SUPER_DIGEST + DIGEST_SIZE]
                if (block[:len(POOL_MAGIC)] == POOL_MAGIC
                        and field(block, SUPER_VERSION, 4) == POOL_VERSION
                        and field(block, SUPER_BLOCK_SIZE, 4) == POOL_BLOCK
                        and field(block, SUPER_BLOCKS, 8) == self.blocks
                        and txg % POOL_RING_SLOTS == slot
                        and hashlib.sha256(block[:SUPER_DIGEST]).digest()
                        == digest
                        and (newest is None or txg > newest[0])):
                    newest = (txg, block[SUPER_ROOT:SUPER_ROOT
                                         + POINTER_SIZE])
        if newest is None:
            raise BootFailed("the data disk holds no valid superblock")
        return newest[1]

    def read(self, pointer):
        """The block pointer names, from a copy its digest matches; a
        hole reads as zeros."""
        first = field(pointer, POINTER_FIRST, 8)
        if first == 0:
            return bytes(POOL_BLOCK)
        for copy in (first, field(pointer, POINTER_SECOND, 8)):
            if copy:
                block = self.block(copy)
                if (hashlib.sha256(block).digest()
                        == pointer[POINTER_DIGEST:POINTER_SIZE]):
                    return block
        raise BootFailed("the data disk's block %d fails its checksum"
                         % first)

    def object_block(self, pointer, levels, index):
        """Block index of the object of levels levels under pointer."""
        if levels == 0:
            return bytes(POOL_BLOCK)
        if index >= POOL_FANOUT ** (levels - 1):
            raise BootFailed("the data disk names a block past its object")
        for level in range(levels, 1, -1):
            span = POOL_FANOUT ** (level - 2)
            at = index // span * POINTER_SIZE
            pointer = self.read(pointer)[at:at + POINTER_SIZE]
            index %= span
        return self.read(pointer)

    def node(self, slot):
        """The kind and bytes of the node at slot: a file's bytes, or a
        directory's entries."""
        block = self.object_block(self.table, self.table_levels,
                                  slot // RECORDS_PER_BLOCK)
        at = slot % RECORDS_PER_BLOCK * RECORD_SIZE
        record = block[at:at + RECORD_SIZE]
        size = field(record, RECORD_SIZE_FIELD, 8)
        pointer = record[RECORD_ROOT:RECORD_ROOT + POINTER_SIZE]
        data = b"".join(self.object_block(pointer, record[RECORD_LEVELS], i)
                        for i in range(-(-size // POOL_BLOCK)))
        return KINDS.get(record[RECORD_KIND], "free"), data[:size]

    def entries(self, slot):
        """The directory at slot's entries: each name's slot and kind."""
        kind, data = self.node(slot)
        if kind != DIRECTORY:
            raise BootFailed("the data disk's node %d is a %s, not a "
                             "directory" % (slot, kind))
        entries = {}
        at = 0
        while at < len(data):
            length = data[at + POOL_ENTRY_LENGTH]
            name = data[at + POOL_ENTRY_NAME:at + POOL_ENTRY_NAME + length]
            entries[name.decode("utf-8")] = (
                field(data, at + POOL_ENTRY_SLOT, 4),
                KINDS.get(data[at + POOL_ENTRY_KIND], "free"))
            at += POOL_ENTRY_NAME + length
        return entries

    def lookup(self, path):
        """The slot of the node at path, a tuple of names below the
        volume's root."""
        slot = ROOT_SLOT
        for depth, name in enumerate(path):
            entries = self.entries(slot)
            if name not in entries:
                raise BootFailed("the data disk has no /%s"
                                 % "/".join(path[:depth + 1]))
            slot = entries[name][0]
        return slot

    def tree(self, slot, prefix=""):
        """Each path below the directory at slot, after prefix, with a
        file's bytes, DIRECTORY or another node's kind."""
        found = {}
        for name, (child, kind) in self.entries(slot).items():
            path = prefix + name
            if kind == DIRECTORY:
                found[path] = DIRECTORY
                found.update(self.tree(child, path + "/"))
            else:
                found[path] = self.node(child)[1] if kind == FILE else kind
        return found


def check_home(path, wanted):
    """Check that the data disk at path holds the account's home alone in
    /users, and that the home holds the tree wanted, no more."""
    with open(path, "rb") as disk:
        pool = Pool(disk)
        users = sorted(pool.entries(pool.lookup(USERS)))
        if users != [ACCOUNT_NAME]:
            raise BootFailed("the data disk's /users holds %s" % users)
        found = pool.tree(pool.lookup(HOME))
    if found != wanted:
        raise BootFailed("the data disk's home holds %s, not %s"
                         % (found, wanted))


def data_disk(args, scratch):
    """A writable copy of the data disk, damaged as --corrupt-gpt asks;
    its drive's arguments, and the lines the boot must then report."""
    if not args.data:
        return [], []
    copy = os.path.join(scratch, DATA_COPY)
    shutil.copyfile(args.data, copy)
    lines = DATA_LINES
    if args.corrupt_gpt:
        with open(copy, "r+b") as disk:
            corrupt_gpt(disk, args.corrupt_gpt)
        lines = [GPT_REFUSED[args.corrupt_gpt], DATA_REFUSED, LOGIN_ABSENT]
    return ["-drive", "if=virtio,format=raw,file=" + copy], lines


def required_tests(args):
    """What a test disk's report must hold, as Ktap.problems takes it: the
    tests that must pass, the lists of tests whose plan must be a given
    count, the tests that must be skipped, and the data results must
    carry. The chain runs only from chain.img and is skipped elsewhere."""
    required = KERNEL_TESTS + INIT_TESTS
    plans, skipped, data = {}, [], {}
    if args.data and not args.corrupt_gpt:
        required.append(FSTEST)
    if args.mode == "chain":
        required += [CHAIN] + list(CHAIN_LINK_DATA)
        plans[CHAIN] = CHAIN_STEPS
        data = CHAIN_LINK_DATA
    else:
        skipped.append(CHAIN)
    return required, plans, skipped, data


def report_problems(report, args):
    """Print the test report, once no more of it will come, and what
    fails it (Ktap.problems)."""
    report.finish()
    for line in report.summary():
        print(line, flush=True)
    problems = report.problems(*required_tests(args))
    for problem in problems:
        print("ktap: " + problem, flush=True)
    return problems


def boot(args, scratch, log, data_drive, data_lines, first):
    """Boot once with data_drive, the data disk's arguments, and check
    the boot's lines, and with --tests its test report; for the
    specimen, check its screen; in system and persist modes, send the
    pointer's events, and with a data disk whose GPT is whole create the
    account if first or log in with it, and run whoami and the file
    commands."""
    vars_copy = os.path.join(scratch, "vars.fd")
    shutil.copyfile(args.vars, vars_copy)
    qmp_path = os.path.join(scratch, "qmp.sock")
    accounts = (args.mode not in ("chain", "specimen") and data_drive
                and not args.corrupt_gpt)
    screens = None
    if accounts and args.mode == "system":
        screens = build_screens(scratch)
    ready = []
    if accounts:
        ready = FIRST_BOOT_READY if first else LATER_BOOT_READY
    command = (
        ["qemu-system-x86_64", "-machine", "q35", "-m", MEMORY,
         "-drive", "if=pflash,format=raw,readonly=on,file=" + args.code,
         "-drive", "if=pflash,format=raw,file=" + vars_copy,
         "-drive", "if=virtio,format=raw,readonly=on,file=" + args.disk]
        + data_drive
        + DEVICES
        + ["-display", "none", "-serial", "stdio",
           "-qmp", "unix:%s,server=on,wait=off" % qmp_path]
        + args.qemu_args)
    print(" ".join(command), flush=True)
    qemu = subprocess.Popen(command, stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE)
    serial = None
    report = Ktap() if args.tests else None
    try:
        serial = Serial(qemu.stdout, log, report)
        lines = {"chain": CHAIN_LINES,
                 "specimen": SPECIMEN_LINES}.get(
                     args.mode, SYSTEM_LINES + CONSOLE_DRAWN[args.tests])
        if not args.tests:
            lines = [RANDOM_PASSED] + lines
        try:
            serial.wait_for(lines + data_lines + ready + args.expect,
                            serial.start + args.timeout,
                            report_done=bool(report))
        except BootFailed:
            if report:
                report_problems(report, args)
            raise
        if report and report_problems(report, args):
            raise BootFailed("the test report fails")
        if args.mode == "specimen":
            check_specimen(qmp_path, scratch, args.fonts, args.screen)
        if args.mode in ("system", "persist"):
            drive(serial, qmp_path, os.path.join(scratch, DATA_COPY),
                  accounts, first, screens, scratch)
    finally:
        stop(qemu)
        if serial:
            serial.reader.join()
        qemu.stdout.close()
    if serial.leaked:
        raise BootFailed("the serial log holds "
                         + "; ".join(sorted(serial.leaked)))


def expected_line(text):
    """--expect's LINE as an Event."""
    if not parse_line(text):
        raise argparse.ArgumentTypeError(
            "not a line of the form component: event key=value ...: %r"
            % text)
    return Event(text)


def main():
    parser = argparse.ArgumentParser(
        description="Boot a nova-os disk under QEMU and check its serial log.")
    parser.add_argument("--code", required=True,
                        help="the firmware's code, read only")
    parser.add_argument("--vars", required=True,
                        help="the firmware's variable store, copied")
    parser.add_argument("--log", required=True,
                        help="where to write the serial log")
    parser.add_argument("--timeout", type=float,
                        help="seconds the boot may take")
    parser.add_argument("--expect", action="append", default=[],
                        type=expected_line, metavar="LINE",
                        help="a line the boot must report, in the log's "
                        "form, with the keys it gives")
    parser.add_argument("--tests", action="store_true",
                        help="the disk is a test disk, whose boot must "
                        "report its tests, all passing")
    parser.add_argument("--data",
                        help="the data disk, attached as a writable copy")
    parser.add_argument("--corrupt-gpt", choices=sorted(GPT_REFUSED),
                        help="change a byte of the data disk's GPT")
    parser.add_argument("--fonts",
                        help="the nova-os fonts the specimen's prototype "
                        "draws with")
    parser.add_argument("--screen",
                        help="where to keep the specimen's screendump")
    parser.add_argument("mode", choices=sorted(DEFAULT_TIMEOUTS))
    parser.add_argument("disk")
    parser.add_argument("qemu_args", nargs="*", metavar="QEMU_ARG")
    args = parser.parse_args()
    if args.corrupt_gpt and not args.data:
        parser.error("--corrupt-gpt needs --data")
    if args.mode == "persist" and (not args.data or args.corrupt_gpt):
        parser.error("persist needs --data, whole")
    if args.mode in ("chain", "specimen") and not args.tests:
        parser.error("%s takes only a test disk, with --tests" % args.mode)
    if (args.mode == "specimen") != bool(args.fonts):
        parser.error("specimen needs --fonts, and only specimen takes it")
    if args.screen and args.mode != "specimen":
        parser.error("only specimen takes --screen")
    if args.timeout is None:
        args.timeout = DEFAULT_TIMEOUTS[args.mode]
    with tempfile.TemporaryDirectory() as scratch, \
            open(args.log, "wb") as log:
        try:
            data_drive, data_lines = data_disk(args, scratch)
            boot(args, scratch, log, data_drive, data_lines, first=True)
            if args.mode == "persist":
                print("booting the same data disk again", flush=True)
                boot(args, scratch, log, data_drive, data_lines, first=False)
        except (BootFailed, OSError) as failure:
            print("FAILED: %s" % failure, flush=True)
            return 1
    print("passed", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
