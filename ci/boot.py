"""Boot a nova-os disk under QEMU and check what it reports.

Usage: python3 -I ci/boot.py --code CODE.fd --vars VARS.fd --log LOG
                             [--timeout SECONDS] [--expect TEXT ...]
                             [--data DATA [--corrupt-gpt KIND]]
                             {system,persist,chain} DISK [-- QEMU_ARG ...]

Boots DISK with the machine and devices of README.md's Run section,
headless, with the firmware CODE.fd and a copy of VARS.fd, a writable
copy of DATA as the data disk after it, and the QEMU arguments given
after --. The guest's serial port is echoed with the time since QEMU
started and written to LOG.

In system mode, the boot of disk.img must report each kernel self-test
passing, the store service serving, the test program from /store
passing its checks, the virtio block driver reading the GPT, init's
checks passing, startup done, the background drawn and the console
drawn. With a data disk whose GPT is whole, the accounts service must
report no account and login must start; then, typed through QMP,
QEMU's monitor protocol, the first account is created, the shell
starts, and the shell must report running whoami for it; the
password, typed at the shell's prompt too, must appear nowhere in the
log. The session then makes a directory in its home, writes a file in
it twice, shows it, moves it, writes and removes an empty file, and is
refused a directory under /store and a file in another user's home.
The shell must log each command's status, and then the data disk, read
on the host as docs/spec/pool-format.md lays it out, must hold the
account's home alone in /users and the command's change in the home,
and nothing else there; no word of a name or line typed for the
commands may appear in the log. In persist mode the same copy of the
data disk is booted twice: the first boot is system mode's, and the
second must report the account the first made and, typed again, log
in with it, run whoami and show the file, which the disk must still
hold. In chain mode, the boot of
chain.img must report each of the bootstrap chain's steps, each pinned
link and init's checks passing.
Each --expect TEXT adds a line the boot must report. With a data disk,
the boot must also report the file system service opening its pool,
/data and /users published, and the data disk's check passing. With
--corrupt-gpt, the copy's two GPT tables are changed first: header or
entries change a byte of each header's CRC or of each entry array;
extent moves the pool's entry to start at LBA 0, below the first usable
LBA; usable moves the entry and the first usable LBA both to LBA 1, the
header's; overlap adds a second entry over the pool's blocks; the last
three with every CRC made to match. The boot must then report the service
refusing the disk, /data and /users not published, and the accounts
service and login not started.

The check fails at once on a line from nova-os that reports a failure,
a halt, a fault or a supervised process restarted or given up, or if
QEMU ends, which it does on a reset, and otherwise when the timeout
passes with a line not yet reported.
"""

import argparse
import hashlib
import json
import os
import queue
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import zlib

# README.md's Run command, with the display off, the serial port on
# standard output and a QMP socket for the keys. -no-reboot makes a reset
# end QEMU, so that the lines of a second boot cannot complete the
# first's.
MEMORY = "256M"
DEVICES = ["-net", "none", "-device", "virtio-rng-pci",
           "-device", "virtio-keyboard-pci", "-device", "qemu-xhci",
           "-device", "usb-tablet", "-no-reboot"]

SELF_TESTS = ["nova: %s self-test passed" % name
              for name in ("page allocator", "random", "object", "thread")]
INIT_PASSED = "nova-init: checks passed"
STARTUP_DONE = "nova: first process reports startup done"

SYSTEM_LINES = SELF_TESTS + [
    "nova-store: serving ",
    "nova-hello: checks passed",
    "nova-init: the bootstrap check is not on this disk",
    'nova-virtio-blk: lba 1: GPT header, signature "EFI PART"',
    INIT_PASSED,
    STARTUP_DONE,
    "nova: background drawn in ",
    "nova-console: drawing on the ",
]

# init prints a link's line before comparing it with its pin, and stops
# with "the chain failed" on a mismatch, so the line naming every link
# is the one that shows each matched.
CHAIN_STEPS = 20
CHAIN_LINKS = ["hex0", "hex1", "hex2", "catm", "M0", "cc_x86", "M2", "M1"]
CHAIN_LINES = (
    SELF_TESTS
    + ["nova-init: step %d of %d: " % (step, CHAIN_STEPS)
       for step in range(1, CHAIN_STEPS + 1)]
    + [", nova-nix's %s" % link for link in CHAIN_LINKS]
    + ["nova-init: the chain built nova-nix's "
       + ", ".join(CHAIN_LINKS[:-1]) + " and " + CHAIN_LINKS[-1],
       INIT_PASSED, STARTUP_DONE])

DEFAULT_TIMEOUTS = {"system": 300, "persist": 300, "chain": 2700}

DATA_LINES = [
    "nova-fs: pool at lba ",
    "nova-init: /data and /users published from the data disk",
    "nova-fstest: checks passed",
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
EXTENT_REFUSED = ("nova-fs: the disk's pool entry lies outside the usable "
                  "LBAs or overlaps another")
GPT_REFUSED = {
    "header": "nova-fs: the disk's GPT header fails its check",
    "entries": "nova-fs: the disk's GPT entry array fails its check",
    "extent": EXTENT_REFUSED,
    "usable": ("nova-fs: the disk's GPT header places its tables or its "
               "usable LBAs where they overlap"),
    "overlap": EXTENT_REFUSED,
}
DATA_REFUSED = "nova-init: /data and /users are not published"
LOGIN_ABSENT = ("nova-init: no /data and /users; the accounts service and "
                "login do not start")

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

# What login and the accounts service report before login reads its
# first line, and then on the first boot and on a later one.
FIRST_BOOT_READY = ["nova-accounts: 0 accounts", "nova-login: on the console"]
LATER_BOOT_READY = ["nova-accounts: 1 account", "nova-login: on the console"]
CREATED = ["nova-accounts: created %s, id 1, an administrator" % ACCOUNT_NAME,
           "nova-shell: a session for %s, with the administrator handle"
           % ACCOUNT_NAME]
LOGGED_IN = ["nova-accounts: %s logged in" % ACCOUNT_NAME,
             "nova-shell: a session for %s, with the administrator handle"
             % ACCOUNT_NAME]
WHOAMI = "whoami"
WHOAMI_RAN = "nova-shell: whoami: %s, id 1, an administrator" % ACCOUNT_NAME
NO_COMMAND = "nova-shell: %s runs no command" % ACCOUNT_NAME

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
STATUS_RAN = "nova-shell: %s: %%s gives status %%d" % ACCOUNT_NAME
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

# What no line of the log may hold: the password, and each word of the
# names and lines typed for the file commands, words no other line of
# the log holds, so that a log of any of them is found.
UNLOGGED = ([(ACCOUNT_PASSWORD, "the account's password")]
            + [(word, "%r, typed for a file command" % word)
               for word in ["notes", "draft", "final", "stale", "nobody"]
               + " ".join([FIRST_DRAFT] + FINAL_LINES).split()])

# The account's home on the data disk's volume, which must be the only
# entry of /users.
USERS = ("users",)
HOME = USERS + (ACCOUNT_NAME,)

# The accounts service hashes each password with Argon2id over 64 MiB in
# 3 passes, which takes about 12 seconds under TCG on an Apple M2 and
# can take several times that on a shared runner; a line typed and
# answered without a hash takes a second or two.
HASH_TIMEOUT = 300
KEY_TIMEOUT = 60

# What a nova-os line says when something went wrong: a check failed, a
# kernel or user fault, the first process ended, or init's supervisor
# restarted a driver or gave one up.
NOVA = "nova"
FAILURES = ["failed", "halted", "NOT SEEDED", "nova: exception",
            "nova: user exception", "nova: first process ended",
            "nova-init: restarting the ", " is given up"]

QMP_TIMEOUT = 30
QUIT_TIMEOUT = 30

# The data disk's writable copy, in the scratch directory.
DATA_COPY = "data.img"


class BootFailed(Exception):
    pass


class Serial:
    """The guest's serial lines, read from QEMU's standard output. What
    a line holds of UNLOGGED is noted in leaked."""

    def __init__(self, stream, log):
        self.lines = queue.Queue()
        self.start = time.monotonic()
        self.leaked = set()
        self.reader = threading.Thread(target=self.read, args=(stream, log))
        self.reader.start()

    def read(self, stream, log):
        for raw in stream:
            log.write(raw)
            log.flush()
            text = raw.rstrip(b"\r\n").replace(b"\r", b"")
            self.leaked.update(what for unlogged, what in UNLOGGED
                               if unlogged.encode("ascii") in text)
            self.lines.put(text.decode("ascii", "replace"))
        self.lines.put(None)

    def wait_for(self, wanted, deadline):
        """Read lines until each text in wanted has appeared in one."""
        missing = list(wanted)
        while missing:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise BootFailed("timed out; not reported:\n  "
                                 + "\n  ".join(missing))
            try:
                line = self.lines.get(timeout=remaining)
            except queue.Empty:
                continue
            if line is None:
                raise BootFailed("QEMU ended; not reported:\n  "
                                 + "\n  ".join(missing))
            print("%8.1f  %s" % (time.monotonic() - self.start, line),
                  flush=True)
            at = line.find(NOVA)
            if at >= 0 and any(word in line[at:] for word in FAILURES):
                raise BootFailed("reported a failure: " + line[at:])
            missing = [text for text in missing if text not in line]


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


def run_commands(serial, qmp, commands, data):
    """Type each command's lines, wait for the status it must log, and
    check the home's tree on data, the data disk, which QEMU holds open
    but writes no more until the next line is typed."""
    for lines, command, status, wanted in commands:
        for text in lines:
            type_line(qmp, text)
        serial.wait_for([STATUS_RAN % (command, status)],
                        time.monotonic() + KEY_TIMEOUT)
        check_home(data, wanted)
        print("          the data disk holds the home %s left" % command,
              flush=True)


def log_in(serial, qmp_path, data, first):
    """Create the account on a first boot, or log in with it on a later
    one, then run whoami and the file commands in its shell, checking
    data, the data disk, after each."""
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(QMP_TIMEOUT)
        connection.connect(qmp_path)
        with connection.makefile("rwb") as stream:
            qmp = Qmp(stream)
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
                         FILE_COMMANDS if first else LATER_FILE_COMMANDS,
                         data)


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


def boot(args, scratch, log, data_drive, data_lines, first):
    """Boot once with data_drive, the data disk's arguments, and check
    the boot's lines; with a data disk whose GPT is whole, create the
    account if first or log in with it, and run whoami and the file
    commands."""
    vars_copy = os.path.join(scratch, "vars.fd")
    shutil.copyfile(args.vars, vars_copy)
    qmp_path = os.path.join(scratch, "qmp.sock")
    accounts = args.mode != "chain" and data_drive and not args.corrupt_gpt
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
    try:
        serial = Serial(qemu.stdout, log)
        lines = CHAIN_LINES if args.mode == "chain" else SYSTEM_LINES
        serial.wait_for(lines + data_lines + ready + args.expect,
                        serial.start + args.timeout)
        if accounts:
            log_in(serial, qmp_path, os.path.join(scratch, DATA_COPY),
                   first)
    finally:
        stop(qemu)
        if serial:
            serial.reader.join()
        qemu.stdout.close()
    if serial.leaked:
        raise BootFailed("the serial log holds "
                         + "; ".join(sorted(serial.leaked)))


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
                        metavar="TEXT", help="a line the boot must report")
    parser.add_argument("--data",
                        help="the data disk, attached as a writable copy")
    parser.add_argument("--corrupt-gpt", choices=sorted(GPT_REFUSED),
                        help="change a byte of the data disk's GPT")
    parser.add_argument("mode", choices=sorted(DEFAULT_TIMEOUTS))
    parser.add_argument("disk")
    parser.add_argument("qemu_args", nargs="*", metavar="QEMU_ARG")
    args = parser.parse_args()
    if args.corrupt_gpt and not args.data:
        parser.error("--corrupt-gpt needs --data")
    if args.mode == "persist" and (not args.data or args.corrupt_gpt):
        parser.error("persist needs --data, whole")
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
