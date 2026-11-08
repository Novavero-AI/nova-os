"""Check that build.sh checks each download before using it.

Usage: python3 -I ci/check-downloads.py OUT

OUT is a new directory, which holds the build made here. The build runs
the build.sh of the tree that holds this script under sh, with curl a
stub that writes the same bytes for every URL. It must stop at its
first download, TinyCC's archive, naming it, the sha256 SHA256SUMS
gives for it and the sha256 of the stub's bytes, before TinyCC is
unpacked or compiled; and no file with the stub's bytes may be left in
OUT.
"""

import hashlib
import os
import pathlib
import shlex
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILD = ROOT / "build.sh"
SUMS = ROOT / "SHA256SUMS"

# build.sh's first download, and the name of the cross compilers it
# builds from it.
FIRST_DOWNLOAD = "tinycc.tar.gz"
TINYCC_TREE_PREFIX = "tinycc-"
CROSS_COMPILER_SUFFIX = "-win32-tcc"

STUB_BYTES = b"not a download build.sh pins\n"
STUB_CURL = """#!/bin/sh
# Stands in for curl: writes the same bytes to the file -o names,
# whatever the URL.
while [ $# -gt 0 ]; do
  if [ "$1" = -o ]; then
    exec cp -- {bytes} "$2"
  fi
  shift
done
echo "$0: no -o" >&2
exit 2
"""


def pinned(name):
    """The sha256 SHA256SUMS gives for name."""
    for line in SUMS.read_text(encoding="ascii").splitlines():
        digest, _, file = line.partition("  ")
        if file == name:
            return digest
    sys.exit("SHA256SUMS gives no sha256 for " + name)


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def build(out, env):
    """Run build.sh into out with env as its environment, and return the
    finished process."""
    return subprocess.run(["sh", str(BUILD), str(out)], env=env,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          check=False)


def fail(what, result):
    """Print a failed check and the output of the build it ran."""
    sys.stdout.buffer.write(result.stdout)
    sys.stdout.buffer.write(result.stderr)
    sys.exit("FAILED  " + what)


def files_holding(directory, data):
    """The files under directory whose bytes are data."""
    return [path for path in directory.rglob("*")
            if path.is_file() and path.stat().st_size == len(data)
            and path.read_bytes() == data]


def check_wrong_bytes_stop(out):
    stub = out / "stub"
    stub.mkdir()
    payload = stub / "bytes"
    payload.write_bytes(STUB_BYTES)
    curl = stub / "curl"
    curl.write_text(STUB_CURL.format(bytes=shlex.quote(str(payload))),
                    encoding="ascii")
    curl.chmod(0o755)
    env = dict(os.environ, PATH=str(stub) + os.pathsep + os.environ["PATH"])
    built = out / "wrong-bytes"
    result = build(built, env)
    if result.returncode == 0:
        fail("a build given other bytes than its pins succeeded", result)
    stderr = result.stderr.decode(errors="replace")
    for word in (FIRST_DOWNLOAD, pinned(FIRST_DOWNLOAD), sha256(payload)):
        if word not in stderr:
            fail("the build's message does not name " + word, result)
    used = [path.name for path in built.iterdir()
            if path.name.startswith(TINYCC_TREE_PREFIX)
            or path.name.endswith(CROSS_COMPILER_SUFFIX)]
    if used:
        fail("the build used the bytes: " + ", ".join(used), result)
    kept = files_holding(built, STUB_BYTES)
    if kept:
        fail("the build kept the bytes: " + ", ".join(map(str, kept)), result)
    print("ok  a download with other bytes than its pin stops the build "
          "before use")


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: check-downloads.py OUT")
    # Absolute, since build.sh changes directory and the stub's directory
    # is put in PATH.
    out = pathlib.Path(sys.argv[1]).resolve()
    out.mkdir()
    check_wrong_bytes_stop(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
