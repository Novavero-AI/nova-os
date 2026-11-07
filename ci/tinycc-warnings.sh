#!/bin/sh
# Runs README.md's Build block in OUT, a new directory holding the tree
# at HEAD, with -Wall -Werror added to every TinyCC command, so that any
# warning TinyCC gives on nova-os's sources fails the build.
#
# Usage: ci/tinycc-warnings.sh OUT
set -eu

[ $# -eq 1 ] || { echo "usage: $0 OUT" >&2; exit 2; }
out=$1
ci=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(git -C "$ci" rev-parse --show-toplevel)

mkdir -- "$out"
git -C "$root" archive HEAD | tar -x -C "$out"
cd -- "$out"

awk '/^```sh$/ { blocks++; if (blocks == 1) { inside = 1; next } }
     /^```$/ { inside = 0 }
     inside' README.md |
  sed 's|^\(\./[a-z0-9_]*-win32-tcc\) |\1 -Wall -Werror |' > build.sh
if ! grep -q -e '-win32-tcc -Wall -Werror ' build.sh; then
  echo "README.md's Build block has no TinyCC command to check" >&2
  exit 1
fi
sh -ex build.sh
