"""Boot a nova-os disk under QEMU and check what it reports.

Usage: python3 -I ci/boot.py --code CODE.fd --vars VARS.fd --log LOG
                             [--timeout SECONDS] [--expect TEXT ...]
                             [--data DATA [--corrupt-gpt {header,entries}]]
                             {system,chain} DISK [-- QEMU_ARG ...]

Boots DISK with the machine and devices of README.md's Run section,
headless, with the firmware CODE.fd and a copy of VARS.fd, a writable
copy of DATA as the data disk after it, and the QEMU arguments given
after --. The guest's serial port is echoed with the time since QEMU
started and written to LOG.

In system mode, the boot of disk.img must report each kernel self-test
passing, the store service serving, the test program from /store
passing its checks, the virtio block driver reading the GPT, init's
checks passing, startup done, the background drawn and the console
drawn with its demo client running; then a line typed through QMP,
QEMU's monitor protocol, must come back from the demo client. In chain
mode, the boot of chain.img must report each of the
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
refusing the disk and /data and /users not published.

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
    "nova-console-echo: writing the greeting",
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

DEFAULT_TIMEOUTS = {"system": 300, "chain": 2700}

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

# A line typed as QMP's send-key names its keys, and what the demo
# client logs once it has echoed the line, which gives the length alone.
LINE_KEYS = ["h", "i", "ret"]
LINE_ECHOED = "nova-console-echo: echoed a line of 2 bytes"
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
    """The guest's serial lines, read from QEMU's standard output."""

    def __init__(self, stream, log):
        self.lines = queue.Queue()
        self.start = time.monotonic()
        self.reader = threading.Thread(target=self.read, args=(stream, log))
        self.reader.start()

    def read(self, stream, log):
        for raw in stream:
            log.write(raw)
            log.flush()
            text = raw.rstrip(b"\r\n").replace(b"\r", b"")
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


def type_line(serial, qmp_path):
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(QMP_TIMEOUT)
        connection.connect(qmp_path)
        with connection.makefile("rwb") as stream:
            qmp = Qmp(stream)
            for name in LINE_KEYS:
                print("          send-key %s" % name, flush=True)
                qmp.execute("send-key",
                            keys=[{"type": "qcode", "data": name}])
            serial.wait_for([LINE_ECHOED], time.monotonic() + KEY_TIMEOUT)


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
        lines = [GPT_REFUSED[args.corrupt_gpt], DATA_REFUSED]
    return ["-drive", "if=virtio,format=raw,file=" + copy], lines


def boot(args, scratch, log):
    vars_copy = os.path.join(scratch, "vars.fd")
    shutil.copyfile(args.vars, vars_copy)
    qmp_path = os.path.join(scratch, "qmp.sock")
    data_drive, data_lines = data_disk(args, scratch)
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
        lines = SYSTEM_LINES if args.mode == "system" else CHAIN_LINES
        serial.wait_for(lines + data_lines + args.expect,
                        serial.start + args.timeout)
        if args.mode == "system":
            type_line(serial, qmp_path)
    finally:
        stop(qemu)
        if serial:
            serial.reader.join()
        qemu.stdout.close()


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
    if args.timeout is None:
        args.timeout = DEFAULT_TIMEOUTS[args.mode]
    with tempfile.TemporaryDirectory() as scratch, \
            open(args.log, "wb") as log:
        try:
            boot(args, scratch, log)
        except (BootFailed, OSError) as failure:
            print("FAILED: %s" % failure, flush=True)
            return 1
    print("passed", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
