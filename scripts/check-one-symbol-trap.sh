#!/bin/bash
# DRIFT-PREVENTION GATE -- "a hand-built MIR function that reports a symbol by
# name" has ONE owner, cir_new_symbol_trap_fn() (src/madc_cir.cpp).
#
# --run-frozen's trap stubs and the interactive session's function stubs (plan
# §42 D27) are the same shape: a function whose body calls a host handler with
# the symbol's name, so a trap that fires names what it stands for. The two
# were written twice, by hand, before they shared the owner.
#
# One rule over madc's own sources: no code line builds a MIR call instruction
# or a MIR string datum by hand, unless it is marked
# `// allowed-exception: <why>` or `/* allowed-exception: <why> */` on the same
# line (the owner marks its own). Two-sided: the negative control proves the
# pattern bites.
set -u
cd "$(dirname "$0")/.."

SCAN='MIR_new_(call_insn|string_data)[[:space:]]*\('

code_lines() { grep -HnE "$1" "${@:2}" | grep -vE '^[^:]*:[0-9]+:[[:space:]]*(//|/?\*)'; }

ctl=$(mktemp)
trap 'rm -f "$ctl"' EXIT
cat > "$ctl" <<'CTL'
	MIR_append_insn(ctx, f, MIR_new_call_insn(ctx, 3, p, h, d));
	MIR_item_t d = MIR_new_string_data(ctx, nm, s); // allowed-exception: control
	// the older MIR_new_call_insn(ctx, 3, ...) by hand (a comment: skipped)
CTL
c=$(code_lines "$SCAN" "$ctl" | grep -v 'allowed-exception' | grep -c .)
if [ "$c" -ne 1 ]; then
	echo "check-one-symbol-trap: NEGATIVE CONTROL FAILED -- matched $c of 1"
	exit 1
fi

files=$(git ls-files 'src/*.cpp' 'src/*.c' 'src/*.h' 'include/*.h')
un=$(code_lines "$SCAN" $files | grep -v 'allowed-exception')
n=$(printf '%s' "$un" | grep -c . || true)
echo "hand-built MIR symbol reporters: $n (target 0)"
if [ "$n" -ne 0 ]; then
	printf '%s\n' "$un"
	echo "  -> call cir_new_symbol_trap_fn() (src/madc_cir.cpp)."
	exit 1
fi
echo "GREEN -- \"a MIR function that reports a symbol by name\" has one owner."
