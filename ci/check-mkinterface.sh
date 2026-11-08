#!/bin/sh
# Builds tools/mkinterface.c with the host's C compiler and runs it on each
# description in ci/mkinterface/, each of which breaks one rule of
# docs/spec/interface.md and names in its first comment, "# Refused at line
# N: ...", the line its refusal must give. The check fails unless the tool
# exits 1, writes no header and writes the one line "NAME.desc:N: what is
# wrong" to standard error for every description, and exits 2 for the wrong
# arguments. The bytes a text file in this tree may not hold, a byte above
# 7Eh and a NUL, and a description of more constants than the tool holds are
# written here and refused too.
#
# It then makes the header of ci/probe/probe.desc, which uses what the
# descriptions in interfaces/ do not, and compiles and runs
# tools/check-probe-protocol.c against it under the sanitizers.
#
# A rule the generator gains gets a description in ci/mkinterface/; output
# it must get right gets a line in ci/probe/probe.desc and a check.
#
# Usage: ci/check-mkinterface.sh OUT    (OUT is made, and holds the tool)
set -eu

[ $# -eq 1 ] || { echo "usage: $0 OUT" >&2; exit 2; }
ci=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$ci/.." && pwd)
mkdir -- "$1"
out=$(CDPATH='' cd -- "$1" && pwd)

cc -std=c99 -Wall -Wextra -pedantic -Werror -o "$out/mkinterface" \
  "$root/tools/mkinterface.c"

checked=0
failed=0

# refused DIR NAME LINE: runs the tool from DIR on NAME and requires that
# it refuse it at LINE alone.
refused() {
  checked=$((checked + 1))
  rm -f -- "$out/header.h"
  status=0
  (cd -- "$1" && "$out/mkinterface" "$2" "$out/header.h") 2> "$out/stderr" ||
    status=$?
  problem=
  if [ "$status" -ne 1 ]; then
    problem="exits $status, not 1"
  elif [ -e "$out/header.h" ]; then
    problem="wrote a header"
  elif [ "$(wc -l < "$out/stderr")" -ne 1 ]; then
    problem="wrote more than one line"
  else
    case $(cat -- "$out/stderr") in
      "$2:$3: "*) ;;
      *) problem="refused at $(cat -- "$out/stderr")" ;;
    esac
  fi
  if [ -n "$problem" ]; then
    echo "$2: should be refused at line $3, but $problem" >&2
    failed=$((failed + 1))
  fi
}

cases=0
for description in "$ci"/mkinterface/*.desc; do
  # With no description there, the pattern stands for itself.
  [ -e "$description" ] || break
  cases=$((cases + 1))
  name=$(basename -- "$description")
  line=$(sed -n '1s/^# Refused at line \([0-9][0-9]*\):.*/\1/p' "$description")
  if [ -z "$line" ]; then
    echo "$name: its first line does not say '# Refused at line N: ...'" >&2
    failed=$((failed + 1))
    continue
  fi
  refused "$ci/mkinterface" "$name" "$line"
done

# The bytes the tree's text files cannot hold.
mkdir -- "$out/bytes"
printf '# A case.\ninterface non_ascii\n\n# A caf\303\251.\nconst A 1\n' \
  > "$out/bytes/non_ascii.desc"
refused "$out/bytes" non_ascii.desc 4
printf '# A case.\ninterface nul\n\n# A number.\nconst A 1\000\n' \
  > "$out/bytes/nul.desc"
refused "$out/bytes" nul.desc 5

# One constant past CONSTS_MAX, the 1,024 the tool's table holds, a case too
# long to keep as a file: two lines a constant after the three of the head.
awk 'BEGIN {
  print "# A case.\ninterface limit_constants\n"
  for (n = 1; n <= 1025; n++) printf "# A number.\nconst C%d 1\n", n
}' > "$out/bytes/limit_constants.desc"
refused "$out/bytes" limit_constants.desc 2053

# One message past MESSAGES_MAX, the 256 the tool's table holds: the 257th
# event's line is the 17th, and five more for each event before it.
awk 'BEGIN {
  print "# A case.\ninterface limit_messages\n"
  print "# A header.\nstruct HDR\n  u32 transaction  # the first\n  u32 kind  # the kind\nend\n"
  print "# A channel.\nchannel c\n  select kind\n  invalid server close\n  invalid client close\n"
  for (n = 1; n <= 257; n++)
    printf "  # An event.\n  event tick%d TICK%d %d\n    body HDR\n  end\n\n", n, n, n
  print "end"
}' > "$out/bytes/limit_messages.desc"
refused "$out/bytes" limit_messages.desc 1297

# A body of eleven structures of 96 constrained fields: 1,056 constraints,
# past the 1,024 a message's checks hold, refused at the message's line.
awk 'BEGIN {
  print "# A case.\ninterface limit_constraints\n"
  print "# Constrained fields.\nstruct LEAF"
  for (n = 1; n <= 96; n++) printf "  u8 f%d max 5  # a field\n", n
  print "end\n"
  print "# A body.\nstruct BODY\n  u32 transaction  # the first\n  u32 kind  # the kind"
  for (n = 1; n <= 11; n++) printf "  LEAF a%d  # a leaf\n", n
  print "end\n"
  print "# A channel.\nchannel c\n  select kind\n  invalid server close\n  invalid client close\n"
  print "  # An event.\n  event tick TICK 1\n    body BODY\n  end\nend"
}' > "$out/bytes/limit_constraints.desc"
refused "$out/bytes" limit_constraints.desc 128

# A body of twelve structures of 92 counted arrays: 1,104 arrays, past the
# 1,024 a message's checks hold, refused at the message's line.
awk 'BEGIN {
  print "# A case.\ninterface limit_arrays\n"
  print "# Counted arrays.\nstruct LEAF\n  u32 n  # a count"
  for (n = 1; n <= 92; n++) printf "  u8 a%d array 1 count n  # an array\n", n
  print "end\n"
  print "# A body.\nstruct BODY\n  u32 transaction  # the first\n  u32 kind  # the kind"
  for (n = 1; n <= 12; n++) printf "  LEAF b%d  # a leaf\n", n
  print "end\n"
  print "# A channel.\nchannel c\n  select kind\n  invalid server close\n  invalid client close\n"
  print "  # An event.\n  event tick TICK 1\n    body BODY\n  end\nend"
}' > "$out/bytes/limit_arrays.desc"
refused "$out/bytes" limit_arrays.desc 126

# A declaration documented in 513 lines, one past DOC_LINES_MAX, is refused at
# the first of them, the 4th line; 512 lines are made into a header.
awk 'BEGIN {
  print "# A case.\ninterface limit_doc_lines\n"
  for (n = 1; n <= 513; n++) print "# A line of documentation."
  print "const LIMIT_DOC 1"
}' > "$out/bytes/limit_doc_lines.desc"
refused "$out/bytes" limit_doc_lines.desc 4
awk 'BEGIN {
  print "# A case.\ninterface limit_doc_lines_held\n"
  for (n = 1; n <= 512; n++) print "# A line of documentation."
  print "const LIMIT_DOC 1"
}' > "$out/bytes/limit_doc_lines_held.desc"
if ! "$out/mkinterface" "$out/bytes/limit_doc_lines_held.desc" \
  "$out/bytes/limit_doc_lines_held.h"; then
  echo "limit_doc_lines_held.desc: 512 lines of documentation are refused" >&2
  failed=$((failed + 1))
fi

# The wrong arguments.
status=0
"$out/mkinterface" 2> /dev/null || status=$?
if [ "$status" -ne 2 ]; then
  echo "no arguments: exits $status, not 2" >&2
  failed=$((failed + 1))
fi

echo "$checked descriptions checked, $failed not refused as they say"
if [ "$cases" -eq 0 ]; then
  echo "no description in $ci/mkinterface/" >&2
  exit 1
fi
if [ "$failed" -ne 0 ]; then
  exit 1
fi

mkdir -- "$out/probe"
"$out/mkinterface" "$ci/probe/probe.desc" "$out/probe/probe_protocol.h"
cc -std=c11 -Wall -Wextra -pedantic -Wshadow -Wtrigraphs -Werror \
  -fsanitize=address,undefined -fno-sanitize-recover=all \
  -I "$root/include" -I "$out/probe" -o "$out/probe/check-probe-protocol" \
  "$root/tools/check-probe-protocol.c"
"$out/probe/check-probe-protocol"
