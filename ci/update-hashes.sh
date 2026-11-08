#!/bin/sh
# Rewrites SHA256SUMS after a change meant to change what the build
# writes: builds the working tree with build.sh in OUT, a new directory,
# and lists the sha256 of each file SHA256SUMS names, and of each NAME
# given, a file the build now writes, as built there. A file the build
# no longer writes fails the run until its line is removed. SHA256SUMS
# is replaced whole, by a rename, or left as it was.
#
# build.sh refuses a download whose sha256 differs from its line, and
# names the sha256 it fetched, so each download's line is written by
# hand from that refusal; a new download's line can start as any 64
# hexadecimal digits.
#
# CI checks SHA256SUMS against a build of the commit (ci/reproduce.sh)
# and never rewrites it, so this refuses to run where CI is set, as
# GitHub Actions sets it.
#
# Usage: ci/update-hashes.sh OUT [NAME...]
set -eu

[ $# -ge 1 ] || { echo "usage: $0 OUT [NAME...]" >&2; exit 2; }
if [ -n "${CI-}" ]; then
  echo "$0: CI is set; CI checks SHA256SUMS and never rewrites it" >&2
  exit 2
fi
ci=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$ci/.." && pwd)
sums=$root/SHA256SUMS
replacement=$root/SHA256SUMS.new

# grep exits 0 on a line that is not a sha256, two spaces and a name, 1
# when every line is one, and above 1 on an error.
status=0
grep -v -x -e '[0-9a-f]\{64\}  ..*' -- "$sums" || status=$?
if [ "$status" -ne 1 ]; then
  echo "$0: SHA256SUMS is unreadable or holds the lines above" >&2
  exit 1
fi

mkdir -- "$1"
out=$(CDPATH='' cd -- "$1" && pwd)
shift
"$root/build.sh" "$out"

if command -v sha256sum > /dev/null 2>&1; then
  sha256() { sha256sum -- "$1"; }
else
  sha256() { shasum -a 256 -- "$1"; }
fi

names=$out/SHA256SUMS.names
sed 's/^[0-9a-f]\{64\}  //' "$sums" > "$names"
if [ $# -gt 0 ]; then
  printf '%s\n' "$@" >> "$names"
fi
LC_ALL=C sort -u -o "$names" "$names"

trap 'rm -f -- "$replacement"' EXIT
trap 'exit 1' HUP INT TERM
(
  cd -- "$out"
  while IFS= read -r name; do
    sha256 "$name"
  done
) < "$names" > "$replacement"
mv -f -- "$replacement" "$sums"
