#!/bin/sh
# Checks every tracked C source with clang -fsyntax-only, the warnings
# below as errors, and the flags of the compile_flags.txt nearest it,
# from that file's directory, as clangd reads them. Every source is
# checked, and the exit status is nonzero if any gave a diagnostic or
# there was none to check.
#
# Usage: ci/clang-warnings.sh    (CLANG names the compiler, clang by default)
set -eu

cd "$(git -C "$(dirname -- "$0")" rev-parse --show-toplevel)"
clang=${CLANG:-clang}
"$clang" --version | head -n 1

# compile_flags.txt holds one flag per line, so fields split at newlines
# only, and globbing stays off so that no flag is taken as a pattern.
IFS='
'
set -f
checked=0
failed=0
for source in $(git ls-files '*.c'); do
  # It includes a header that ci/check-mkinterface.sh makes, and is compiled
  # there with these warnings and the sanitizers.
  [ "$source" = tools/check-probe-protocol.c ] && continue
  checked=$((checked + 1))
  dir=$(dirname "$source")
  until [ -f "$dir/compile_flags.txt" ]; do
    if [ "$dir" = . ]; then
      echo "$source: no compile_flags.txt in its directory or above" >&2
      failed=$((failed + 1))
      continue 2
    fi
    dir=$(dirname "$dir")
  done
  flags=$(cat "$dir/compile_flags.txt")
  standard=
  case $source in
    # These are C11, as the build lines in their headers say, because
    # include/cpu.h, include/input_protocol.h, include/native_abi.h and
    # the generated protocol headers are; tools/compile_flags.txt gives
    # the C99 the other tools are written in. Drop this once they have a
    # compile_flags.txt of their own.
    tools/check-masks.c | tools/check-pointer.c | tools/check-text.c | \
      tools/check-backdrop.c | tools/check-compose.c | \
      tools/check-console-protocol.c | tools/check-display-protocol.c | \
      tools/check-endpoint.c)
      standard=-std=c11 ;;
  esac
  # shellcheck disable=SC2086
  if ! (cd "$dir" && "$clang" -fsyntax-only -Wall -Wextra -pedantic \
    -Wshadow -Werror $flags $standard "${source#"$dir"/}"); then
    failed=$((failed + 1))
  fi
done
echo "$checked C sources checked, $failed with diagnostics"
[ "$checked" -gt 0 ] && [ "$failed" -eq 0 ]
