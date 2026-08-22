#!/bin/bash
# GATE — auto-include position gates hold (lexer identifier scan).
#
# The rule: the auto-include scan matches a std-surface name (`set`,
# `getline`, `string`, ...) ONLY where the std entity can be meant. An
# identifier in member-access position (`c.set`), behind a non-std
# qualifier (`madc::getline`, `Counter::set`), or in a declaration head is
# the USER'S name — matching it pulls that header's whole C++ include tree
# into the TU. Measured on examples/adventure: the false pulls cost 776K
# lexed tokens (~1MB of libstdc++ source per affected TU) on a 4.4K-line
# C-shaped program — the dominant cold-JIT-startup cost.
#
# Mechanism: `-dM` dumps the effective macro table after lex (auto-include
# injection happens during lex), so a pulled <set>/<string> shows as its
# own include guard. Two sides:
#   1. tests/testautoincpos.mad (every trigger word in a gated position)
#      must NOT show a <set>/<string> guard macro.
#   2. positive control: the same words in ungated positions MUST show
#      them — proves the detector detects (a lex-phase change that stops
#      -dM from seeing auto-includes would otherwise pass side 1 forever).
set -u
cd "$(dirname "$0")/.."

MADC_BIN="${MADC_BIN:-bin/madc}"
GUARDS='_GLIBCXX_SET|_GLIBCXX_STRING|_LIBCPP_SET|_LIBCPP_STRING'

macros=$("$MADC_BIN" -dM tests/testautoincpos.mad 2>/dev/null)
bad=$(echo "$macros" | grep -E "$GUARDS")
if [ -n "$bad" ]; then
	echo "check-autoinclude-position: gated positions PULLED a C++ header:"
	echo "$bad"
	echo ""
	echo "tests/testautoincpos.mad uses set/getline only in member-access"
	echo "and non-std-qualified positions; the scan must not match them."
	echo "Owner: Program::auto_include_standard_identifier (src/lexer.cpp)."
	exit 1
fi

mkdir -p tmp
ctl=tmp/check_autoinc_positive.mad
printf 'x = set;\ny = getline;\n' > "$ctl"
posmacros=$("$MADC_BIN" -dM "$ctl" 2>/dev/null)
if ! echo "$posmacros" | grep -qE "$GUARDS"; then
	echo "check-autoinclude-position: POSITIVE CONTROL FAILED —"
	echo "ungated bare 'set'/'getline' no longer pull <set>/<string>,"
	echo "so side 1 proves nothing. Fix the control or the scan."
	exit 1
fi

echo "check-autoinclude-position: OK (gated positions lean; positive control pulls)"
exit 0
