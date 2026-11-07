#!/bin/sh
# Builds the tree at HEAD with its build.sh in OUT, a new directory, and
# checks every file its SHA256SUMS names against what the build wrote.
# The tree is archived into OUT/src, so that the build reads its bytes
# as committed; the images land in OUT itself.
#
# Usage: ci/reproduce.sh OUT
set -eu

[ $# -eq 1 ] || { echo "usage: $0 OUT" >&2; exit 2; }
out=$1
ci=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(git -C "$ci" rev-parse --show-toplevel)

mkdir -- "$out"
mkdir -- "$out/src"
git -C "$root" archive HEAD | tar -x -C "$out/src"
sh -x "$out/src/build.sh" "$out"

python3 -I "$ci/check-hashes.py" "$out/src/SHA256SUMS" "$out"
