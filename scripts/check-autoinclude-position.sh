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
#   3. tests/testdialectstdiostreams.mad (stdin/stdout/stderr, no include)
#      must NOT show a stdio.h guard — the dialect hard-codes its streams;
#      its positive control (the program's own #include <stdio.h>) must.
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

# The madc dialect's stdio streams are hard-coded, as println is (owner
# 2026-10-17, Program::add_stdio_streams): naming stdin / stdout / stderr
# pulls no <stdio.h>. A pulled stdio.h shows in -dM as its include guard
# (glibc _STDIO_H, darwin _STDIO_H_, mingw _INC_STDIO). Positive control: the
# same streams with the program's own #include <stdio.h> show it.
STDIO_GUARDS='^#define (_STDIO_H|_STDIO_H_|_INC_STDIO)( |$)'
smacros=$("$MADC_BIN" -dM tests/testdialectstdiostreams.mad 2>/dev/null)
sbad=$(echo "$smacros" | grep -E "$STDIO_GUARDS")
if [ -n "$sbad" ]; then
	echo "check-autoinclude-position: the dialect's stdio streams PULLED <stdio.h>:"
	echo "$sbad"
	echo ""
	echo "tests/testdialectstdiostreams.mad names stdin/stdout/stderr and"
	echo "includes nothing; the dialect hard-codes them (no provider row)."
	echo "Owner: Program::add_stdio_streams (src/parser.cpp)."
	exit 1
fi
sctl=tmp/check_autoinc_stdio_positive.mad
printf '#include <stdio.h>\nint main() { println(stderr, "x"); return 0; }\n' > "$sctl"
if ! "$MADC_BIN" -dM "$sctl" 2>/dev/null | grep -qE "$STDIO_GUARDS"; then
	echo "check-autoinclude-position: POSITIVE CONTROL FAILED —"
	echo "a program including <stdio.h> shows no stdio guard under -dM,"
	echo "so the stdio leg proves nothing. Fix the control or the guard list."
	exit 1
fi

echo "check-autoinclude-position: OK (gated positions lean; dialect streams pull no stdio.h; positive controls pull)"
exit 0
