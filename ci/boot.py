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
log. In persist mode the same copy of the data disk is booted twice:
the first boot is system mode's, and the second must report the
account the first made and, typed again, log in with it and run
whoami. In chain mode, the boot of chain.img must report each of the
bootstrap chain's steps, each pinned link and init's checks passing.
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

# The account the boots make and log in with: a name and a password
# typed as QMP's send-key names their keys. The name is logged, as the
# accounts service and the shell log it; the password must never be,
# and is typed once more at the shell's prompt, where a person who took
# a drawn prompt for login's would type it, to check that the shell
# keeps it out of the log too. It has no space, so that a log of the
# first word typed would hold all of it.
ACCOUNT_NAME = "ci"
ACCOUNT_PASSWORD = "civerifies"
QCODES = {" ": "spc"}

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


class BootFailed(Exception):
    pass


class Serial:
    """The guest's serial lines, read from QEMU's standard output. A line
    that holds the account's password is noted in leaked."""

    def __init__(self, stream, log):
        self.lines = queue.Queue()
        self.start = time.monotonic()
        self.leaked = False
        self.reader = threading.Thread(target=self.read, args=(stream, log))
        self.reader.start()

    def read(self, stream, log):
        for raw in stream:
            log.write(raw)
            log.flush()
            text = raw.rstrip(b"\r\n").replace(b"\r", b"")
            if ACCOUNT_PASSWORD.encode("ascii") in text:
                self.leaked = True
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


def log_in(serial, qmp_path, first):
    """Create the account on a first boot, or log in with it on a later
    one, then run whoami in its shell."""
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


def data_disk(args, scratch):
    """A writable copy of the data disk, damaged as --corrupt-gpt asks;
    its drive's arguments, and the lines the boot must then report."""
    if not args.data:
        return [], []
    copy = os.path.join(scratch, "data.img")
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
    account if first or log in with it, and run whoami."""
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
            log_in(serial, qmp_path, first)
    finally:
        stop(qemu)
        if serial:
            serial.reader.join()
        qemu.stdout.close()
    if serial.leaked:
        raise BootFailed("the serial log holds the account's password")


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
