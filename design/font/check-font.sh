#!/bin/sh
# Checks gfx/font.c against its prototype, font.py: renders every mask
# with both for cells of each width given, 8 and 16 pixels by default,
# and compares the two atlases byte for byte.
# Usage: check-font.sh [WIDTH ...]
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# CC is left unquoted so that it can carry a wrapper, as in make.
${CC:-cc} -std=c99 -O2 -Wall -Wextra -pedantic -Werror \
  -I "$root/include" -I "$root/gfx" \
  -o "$work/render-font" "$here/render-font.c" "$root/gfx/font.c" \
  "$root/gfx/raster.c"

[ $# -gt 0 ] || set -- 8 16
status=0
for width in "$@"; do
  "$work/render-font" "$width" > "$work/c.pgm"
  python3 "$here/font.py" atlas "$width" "$work/python.pgm"
  if cmp -s "$work/c.pgm" "$work/python.pgm"; then
    echo "$width: identical"
  else
    differ=$(cmp -l "$work/c.pgm" "$work/python.pgm" | wc -l | tr -d ' ')
    echo "$width: $differ bytes differ"
    status=1
  fi
done
exit "$status"
