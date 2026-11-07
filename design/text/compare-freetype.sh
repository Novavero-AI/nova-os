#!/bin/sh
# Compares the reference sheet's text as lib/ draws it with FreeType's
# rendering of the same glyphs (compare-freetype.c), from the nova-os
# fonts in FONTS and the TrueType fonts they were converted from, which
# lie beside them, each NAME.ttf beside NAME.nvf. Needs FreeType's
# headers and library, found through pkg-config's freetype2.
# Usage: compare-freetype.sh FONTS
set -eu

[ $# -eq 1 ] || { echo "usage: $0 FONTS" >&2; exit 2; }
here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/../.." && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

pkg-config --modversion freetype2
# CC and pkg-config's flags are left unquoted: each is a list of words.
# shellcheck disable=SC2046
${CC:-cc} -std=c99 -O2 -Wall -Wextra -pedantic -Werror \
  -I "$root/include" -I "$root/gfx" -I "$root/lib" \
  $(pkg-config --cflags freetype2) \
  -o "$work/compare-freetype" "$here/compare-freetype.c" "$here/sheet.c" \
  "$root/lib/outline.c" "$root/lib/typeface.c" "$root/lib/typeset.c" \
  "$root/gfx/raster.c" "$root/gfx/limb.c" $(pkg-config --libs freetype2)
"$work/compare-freetype" "$here/sheet.txt" "$1" "$1"
