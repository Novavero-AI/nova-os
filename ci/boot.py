"""Boot a nova-os disk under QEMU and check what it reports.

Usage: python3 -I ci/boot.py --code CODE.fd --vars VARS.fd --log LOG
                             [--timeout SECONDS] [--expect TEXT ...]
                             {system,chain} DISK [-- QEMU_ARG ...]

Boots DISK with the machine and devices of README.md's Run section,
headless, with the firmware CODE.fd and a copy of VARS.fd, and the
QEMU arguments given after --. The guest's serial port is echoed with
the time since QEMU started and written to LOG.

In system mode, the boot of disk.img must report each kernel self-test
passing, the store service serving, the test program from /store
passing its checks, the virtio block driver reading the GPT, init's
checks passing, startup done, the background drawn and the console
drawn with its demo client running; then a line typed through QMP,
QEMU's monitor protocol, must come back from the demo client. In chain
mode, the boot of chain.img must report each of the
bootstrap chain's steps, each pinned link and init's checks passing.
Each --expect TEXT adds a line the boot must report.

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


def boot(args, scratch, log):
    vars_copy = os.path.join(scratch, "vars.fd")
    shutil.copyfile(args.vars, vars_copy)
    qmp_path = os.path.join(scratch, "qmp.sock")
    command = (
        ["qemu-system-x86_64", "-machine", "q35", "-m", MEMORY,
         "-drive", "if=pflash,format=raw,readonly=on,file=" + args.code,
         "-drive", "if=pflash,format=raw,file=" + vars_copy,
         "-drive", "if=virtio,format=raw,readonly=on,file=" + args.disk]
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
        serial.wait_for(lines + args.expect,
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
    parser.add_argument("mode", choices=sorted(DEFAULT_TIMEOUTS))
    parser.add_argument("disk")
    parser.add_argument("qemu_args", nargs="*", metavar="QEMU_ARG")
    args = parser.parse_args()
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
