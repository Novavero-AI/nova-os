"""Check that build.sh checks each download before using it, and that a
download directory spares a rebuild the network.

Usage: python3 -I ci/check-downloads.py DOWNLOADS OUT

DOWNLOADS is a download directory a build has filled, with
NOVA_OS_DOWNLOAD_DIR set to it (README.md, Build), and is only read.
OUT is a new directory, which holds the builds made here and a copy of
DOWNLOADS. Each build runs the build.sh of the tree that holds this
script under sh.

Where networking is made to fail, every variable curl reads an HTTP or
HTTPS proxy from names a port on the loopback address that this script
holds bound and never listens on, so that curl's every connection to
build.sh's https URLs is refused, and the variables that exempt a host
from the proxy are unset.

1. A build with networking failing and an empty download directory
   must fail with curl's error before its first download reaches OUT.
   This checks that the check in 4 bites: build.sh's curl cannot fetch.
2. A build whose curl is a stub, which writes the same bytes for every
   URL, must stop at its first download, TinyCC's archive, naming it,
   the sha256 SHA256SUMS gives for it and the sha256 of the stub's
   bytes, before TinyCC is unpacked or compiled; and no file with the
   stub's bytes may be left in OUT or in its download directory.
3. A build whose download directory, the copy of DOWNLOADS, holds
   other bytes under TinyCC's archive's name must succeed, and the copy
   must then hold the archive SHA256SUMS gives.
4. A build with networking failing and that download directory must
   succeed, so it made no request, and reproduce SHA256SUMS.
"""

import hashlib
import os
import pathlib
import shlex
import shutil
import socket
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILD = ROOT / "build.sh"
SUMS = ROOT / "SHA256SUMS"
CHECK_HASHES = ROOT / "ci" / "check-hashes.py"

DOWNLOAD_DIR_VARIABLE = "NOVA_OS_DOWNLOAD_DIR"

# build.sh's first download, and the name of the cross compilers it
# builds from it.
FIRST_DOWNLOAD = "tinycc.tar.gz"
TINYCC_TREE_PREFIX = "tinycc-"
CROSS_COMPILER_SUFFIX = "-win32-tcc"

# Every variable curl takes an HTTP or HTTPS proxy from, and those that
# exempt a host from it (curl's manual, ENVIRONMENT).
PROXY_VARIABLES = ("http_proxy", "https_proxy", "HTTPS_PROXY", "all_proxy",
                   "ALL_PROXY")
NO_PROXY_VARIABLES = ("no_proxy", "NO_PROXY")
LOOPBACK = "127.0.0.1"

# The prefix of the line curl -S writes when a transfer fails.
CURL_ERROR = "curl: ("

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


def build(out, downloads, env):
    """Run build.sh into out with downloads as its download directory
    and env as its environment, and return the finished process."""
    environment = dict(env)
    environment[DOWNLOAD_DIR_VARIABLE] = str(downloads)
    return subprocess.run(["sh", str(BUILD), str(out)], env=environment,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                          check=False)


def fail(what, result):
    """Print a failed check and the output of the build it ran."""
    sys.stdout.buffer.write(result.stdout)
    sys.stdout.buffer.write(result.stderr)
    sys.exit("FAILED  " + what)


def offline(port):
    """The environment with every proxy variable naming port."""
    env = dict(os.environ)
    for name in NO_PROXY_VARIABLES:
        env.pop(name, None)
    for name in PROXY_VARIABLES:
        env[name] = "http://%s:%d" % (LOOPBACK, port)
    return env


def files_holding(directory, data):
    """The files under directory whose bytes are data."""
    return [path for path in directory.rglob("*")
            if path.is_file() and path.stat().st_size == len(data)
            and path.read_bytes() == data]


def check_curl_fails(out, env):
    downloads = out / "downloads-empty"
    result = build(out / "no-network", downloads, env)
    if result.returncode == 0:
        fail("a build with networking failing succeeded", result)
    if CURL_ERROR not in result.stderr.decode(errors="replace"):
        fail("a build with networking failing gave no curl error", result)
    if (out / "no-network" / FIRST_DOWNLOAD).exists():
        fail("a build with networking failing wrote " + FIRST_DOWNLOAD,
             result)
    print("ok  a build with networking failing fails at curl")


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
    downloads = out / "downloads-wrong-bytes"
    result = build(built, downloads, env)
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
    kept = files_holding(built, STUB_BYTES) + files_holding(downloads,
                                                            STUB_BYTES)
    if kept:
        fail("the build kept the bytes: " + ", ".join(map(str, kept)), result)
    print("ok  a download with other bytes than its pin stops the build "
          "before use")


def check_refetch(out, downloads):
    (downloads / FIRST_DOWNLOAD).write_bytes(STUB_BYTES)
    result = build(out / "refetch", downloads, os.environ)
    if result.returncode != 0:
        fail("a build with a wrong file in its download directory failed",
             result)
    if sha256(downloads / FIRST_DOWNLOAD) != pinned(FIRST_DOWNLOAD):
        fail("the build did not keep " + FIRST_DOWNLOAD + " as fetched",
             result)
    print("ok  a wrong file in the download directory is fetched again "
          "and kept")


def check_no_request(out, downloads, env):
    built = out / "offline"
    result = build(built, downloads, env)
    if result.returncode != 0:
        fail("a build from a full download directory made a request",
             result)
    reproduced = subprocess.run([sys.executable, "-I", str(CHECK_HASHES),
                                 str(SUMS), str(built)], check=False)
    if reproduced.returncode != 0:
        sys.exit("FAILED  a build from the download directory does not "
                 "match SHA256SUMS")
    print("ok  a build from a full download directory makes no request "
          "and matches SHA256SUMS")


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: check-downloads.py DOWNLOADS OUT")
    # Absolute, since build.sh changes directory and the stub's directory
    # is put in PATH.
    filled = pathlib.Path(sys.argv[1]).resolve()
    out = pathlib.Path(sys.argv[2]).resolve()
    out.mkdir()
    downloads = out / "downloads"
    shutil.copytree(filled, downloads)
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as closed:
        closed.bind((LOOPBACK, 0))
        env = offline(closed.getsockname()[1])
        check_curl_fails(out, env)
        check_wrong_bytes_stop(out)
        check_refetch(out, downloads)
        check_no_request(out, downloads, env)
    return 0


if __name__ == "__main__":
    sys.exit(main())
