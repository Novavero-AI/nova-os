#!/bin/sh
# Rebuilds what README.md's Build and Disk image sections build, by
# running their sh blocks verbatim in OUT, a new directory holding the
# tree at HEAD, and checks every file the README gives a size and sha256
# for against what the blocks wrote.
#
# Usage: ci/reproduce.sh OUT
set -eu

[ $# -eq 1 ] || { echo "usage: $0 OUT" >&2; exit 2; }
out=$1
ci=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(git -C "$ci" rev-parse --show-toplevel)

mkdir -- "$out"
git -C "$root" archive HEAD | tar -x -C "$out"
cd -- "$out"

# The README's first two sh blocks are Build's and Disk image's; the
# third, Run's, boots the result.
awk '/^```sh$/ { blocks++; if (blocks <= 2) { inside = 1; next } }
     /^```$/ { inside = 0 }
     inside' README.md > readme.sh
sh -ex readme.sh

python3 -I "$ci/check-hashes.py" README.md .
