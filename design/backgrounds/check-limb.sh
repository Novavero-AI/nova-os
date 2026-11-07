#!/bin/sh
# Checks gfx/limb.c against its prototype, limb.py: renders Limb with both
# at each size given, 2560x1600 and 1920x1080 by default, and compares the
# two images byte for byte.
# Usage: check-limb.sh [WIDTHxHEIGHT ...]
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

# CC is left unquoted so that it can carry a wrapper, as in make.
${CC:-cc} -std=c99 -O2 -Wall -Wextra -pedantic -Werror \
  -I "$root/include" -I "$root/gfx" \
  -o "$work/render-limb" "$here/render-limb.c" "$root/gfx/limb.c"

[ $# -gt 0 ] || set -- 2560x1600 1920x1080
status=0
for size in "$@"; do
  case $size in
    *x*) ;;
    *) echo "check-limb.sh: $size is not WIDTHxHEIGHT" >&2; exit 2 ;;
  esac
  width=${size%%x*}
  height=${size#*x}
  "$work/render-limb" "$width" "$height" > "$work/c.ppm"
  python3 "$here/render.py" limb "$width" "$height" "$work/python.ppm"
  if cmp -s "$work/c.ppm" "$work/python.ppm"; then
    echo "$size: identical"
  else
    differ=$(cmp -l "$work/c.ppm" "$work/python.ppm" | wc -l | tr -d ' ')
    echo "$size: $differ bytes differ"
    status=1
  fi
done
exit "$status"
