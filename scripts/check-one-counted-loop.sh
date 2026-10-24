#!/bin/bash
# DRIFT-PREVENTION GATE -- a counted loop the CIR builder synthesizes has ONE
# owner, CirBuilder::counted_loop (src/cir_builder.cpp):
# `for (long <ci> = <start>; <cond>; <ci> += 1) <body>` (down: `-= 1`), with
# an optional advance joined to the step.
#
# The class-array element loops, delete[]'s per-element destructor loop, the
# synthesized array destructor (synth_array_dtor_def), the member-pointer null
# stores, the four range-for lowerings and the three dump rows (cir_dump.cpp)
# each built that loop by hand -- the index declaration, the condition, the
# step, the N_FOR.
#
# One rule over src/: no `node5(N_FOR,` except where the line is marked
# `// allowed-exception: <why>` -- the owner itself, and the translator of a
# USER `for` statement, whose init, condition and step come from the program.
# Two-sided: the negative control proves the pattern still bites.
set -u
cd "$(dirname "$0")/.."

LOOP='node5\(N_FOR,'

code_lines() { grep -HnE "$1" "${@:2}" | grep -vE '^[^:]*:[0-9]+:[[:space:]]*//'; }

ctl=$(mktemp)
trap 'rm -f "$ctl"' EXIT
cat > "$ctl" <<'CTL'
	append(items, node5(N_FOR, list(), idecl, cond, step, dtor_call));
	// node5(N_FOR, ...) in a comment: skipped
	return node5(N_FOR, list(), init, cond, step, blk, origin); // allowed-exception: control
CTL
c=$(code_lines "$LOOP" "$ctl" | grep -v 'allowed-exception' | grep -c .)
m=$(code_lines "$LOOP" "$ctl" | grep -c 'allowed-exception')
if [ "$c" -ne 1 ] || [ "$m" -ne 1 ]; then
	echo "check-one-counted-loop: NEGATIVE CONTROL FAILED"
	echo "  an unmarked loop matched $c of 1, a marked one $m of 1"
	exit 1
fi

files=$(git ls-files --cached --others --exclude-standard 'src/*.cpp' 'src/*.h')
bad=$(code_lines "$LOOP" $files | grep -v 'allowed-exception')
n=$(printf '%s' "$bad" | grep -c . || true)
marked=$(code_lines "$LOOP" $files | grep -c 'allowed-exception' || true)
echo "hand-built counted loops: $n (target 0); marked sites: $marked"
if [ "$n" -ne 0 ]; then
	printf '%s\n' "$bad"
	echo "  -> build the loop with CirBuilder::counted_loop (or, over the"
	echo "     elements of a class array, class_array_index_loop)."
	exit 1
fi
echo "GREEN -- a counted loop the CIR builder synthesizes has one owner."
