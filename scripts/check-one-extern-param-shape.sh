#!/bin/bash
# DRIFT-PREVENTION GATE -- the shape of a callee's PARAMETER in an output
# extern prototype has ONE owner, CirBuilder::native_param_shape (an object
# or reference -> void*, a pointer -> its pointee's char* / void*, a function
# pointer -> its declarator through param_decl, a by-value class -> its tag
# or the invisible reference, a scalar -> native_scalar_specs).
#
# The call paths that hand-rolled it declared every non-pointer parameter
# the 64-bit scalar: a function pointer argument then met an integer
# parameter (c2mir: "using pointer without cast for integer type
# parameter" -- std::ios_base::register_callback, tests/
# testregistercallback.mad) and a double one was passed in a general
# register while the callee read xmm0. A runtime extern whose parameter IS a
# 64-bit scalar names its own shape on a named ExternParam, never as an
# `eparams` element.
#
# Rule over src/cir_builder.cpp: no `eparams.push_back` of a hand-written
# scalar shape ({N_LONG, N_LONG} / {N_CHAR} / {N_INT} / {N_DOUBLE}).
# Two-sided: the negative control proves the pattern still bites.
set -u
cd "$(dirname "$0")/.."

PAT='eparams\.push_back\(\{ *\{ *N_(LONG|CHAR|INT|DOUBLE|UNSIGNED)'

code_lines() { grep -HnE "$1" "${@:2}" | grep -vE '^[^:]*:[0-9]+:[[:space:]]*//'; }

ctl=$(mktemp)
trap 'rm -f "$ctl"' EXIT
cat > "$ctl" <<'CTL'
			eparams.push_back({ {N_LONG, N_LONG}, false });
				eparams.push_back({ {N_CHAR}, true });
	// eparams.push_back({ {N_LONG, N_LONG}, false }); (a comment: skipped)
		eparams.push_back(native_param_shape(pt, refp));
		eparams.push_back({ {N_VOID}, true });   // this (void*)
CTL
c=$(code_lines "$PAT" "$ctl" | grep -c .)
if [ "$c" -ne 2 ]; then
	echo "check-one-extern-param-shape: NEGATIVE CONTROL FAILED -- matched $c of 2"
	exit 1
fi

files=$(git ls-files --cached --others --exclude-standard 'src/cir_builder.cpp')
hits=$(code_lines "$PAT" $files)
n=$(printf '%s' "$hits" | grep -c . || true)
echo "hand-written extern parameter shapes: $n (target 0)"
if [ "$n" -ne 0 ]; then
	printf '%s\n' "$hits"
	echo "  -> eparams.push_back(native_param_shape(pt, refp)): the one owner of a"
	echo "     parameter's extern shape."
	exit 1
fi
echo "GREEN -- an extern parameter's shape has one owner."
