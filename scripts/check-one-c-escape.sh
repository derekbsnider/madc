#!/bin/bash
# DRIFT-PREVENTION GATE -- "the body of a C literal: escape these bytes" has ONE
# owner, __madc_c_escape() (src/rt/rt_dump.c; dupaudit family
# c_string_literal_escape).
#
# The rule is: canonical escapes, and octal for any other non-printable byte,
# because octal caps at three digits and a hex escape would swallow a hex
# digit after it. The compiler's literal spellings (madc_c_escape_string:
# token spelling, --emit=c11's N_STR) and the REPL's value display (D10)
# must spell the same bytes the same way. The runtime is strict C11 (the
# ledger lane), so the one owner is C, and the C++ spelling wraps it.
#
# One rule over madc's own sources: no code line writes an octal escape by
# hand, as a printf `%03o` or as the three-digit shift arithmetic, unless it
# is marked `// allowed-exception: <why>` or `/* allowed-exception: <why> */`
# on the same line (the owner marks its own). Two-sided: the negative control
# proves the pattern bites.
set -u
cd "$(dirname "$0")/.."

SCAN='%03o|\(c *>> *6\) *& *7'

code_lines() { grep -HnE "$1" "${@:2}" | grep -vE '^[^:]*:[0-9]+:[[:space:]]*(//|/?\*)'; }

ctl=$(mktemp)
trap 'rm -f "$ctl"' EXIT
cat > "$ctl" <<'CTL'
	snprintf(buf, sizeof(buf), "\\%03o", c);
	buf[n++] = (char)('0' + ((c >> 6) & 7)); // allowed-exception: control
	// the older snprintf "\\%03o" by hand (a comment: skipped)
CTL
c=$(code_lines "$SCAN" "$ctl" | grep -v 'allowed-exception' | grep -c .)
if [ "$c" -ne 1 ]; then
	echo "check-one-c-escape: NEGATIVE CONTROL FAILED -- matched $c of 1"
	exit 1
fi

files=$(git ls-files 'src/*.cpp' 'src/*.c' 'src/*.h' 'include/*.h')
un=$(code_lines "$SCAN" $files | grep -v 'allowed-exception')
n=$(printf '%s' "$un" | grep -c . || true)
echo "hand-written C octal escapes: $n (target 0)"
if [ "$n" -ne 0 ]; then
	printf '%s\n' "$un"
	echo "  -> call __madc_c_escape() (src/rt/rt_dump.c), or madc_c_escape_string in C++."
	exit 1
fi
echo "GREEN -- \"escape a C literal's bytes\" has one owner."
