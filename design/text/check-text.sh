#!/bin/sh
# Checks lib/'s outlines and text against their prototype: renders the
# reference sheet, sheet.txt, with render-text.c and with sheet.py, from
# the nova-os fonts in FONTS, and compares the two images byte for byte.
# Usage: check-text.sh FONTS
set -eu

[ $# -eq 1 ] || { echo "usage: $0 FONTS" >&2; exit 2; }
fonts=$1
here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# CC is left unquoted so that it can carry a wrapper, as in make.
${CC:-cc} -std=c99 -O2 -Wall -Wextra -pedantic -Werror \
  -I "$root/include" -I "$root/gfx" -I "$root/lib" \
  -o "$work/render-text" "$here/render-text.c" "$here/sheet.c" \
  "$root/lib/outline.c" "$root/lib/typeface.c" "$root/lib/typeset.c" \
  "$root/gfx/raster.c" "$root/gfx/limb.c"

status=0
compare() {
  if cmp -s "$work/c.ppm" "$work/python.ppm"; then
    echo "$1: identical"
  else
    differ=$(cmp -l "$work/c.ppm" "$work/python.ppm" | wc -l | tr -d ' ')
    echo "$1: $differ bytes differ"
    status=1
  fi
}

"$work/render-text" "$here/sheet.txt" "$fonts" > "$work/c.ppm"
python3 -I "$here/sheet.py" "$here/sheet.txt" "$fonts" "$work/python.ppm"
compare sheet

exit "$status"
