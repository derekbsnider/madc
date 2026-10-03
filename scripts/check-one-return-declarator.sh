#!/bin/bash
# DRIFT-PREVENTION GATE -- a function's C RETURN type in a declarator has ONE
# owner, CirBuilder::append_return_declarator (src/cir_builder.cpp).
#
# func_def, func_proto, the declared-only extern prototype, translate_return's
# temps, fnptr_decl_pieces and the pointer-to-member call's cast each spelled
# the return type by hand: dd_peel_pointers(ret..., &level_cv), then a loop
# appending one N_POINTER per level. The copies diverged. The extern had no
# function-pointer return (`int (*get(void))(void);` was prototyped
# `extern long long get(void)`), none spelled a pointer-to-array return's
# pointee dims (`int (*g(void))[3]` emitted `int *g(void)`), and the
# pointer-to-member cast read a `const int *&` return as a pointer of the
# wrong depth. Declarations of one function must agree.
#
# Two rules over src/:
#   1. no return type peeled with its level-cv record,
#      `dd_peel_pointers(ret..., &...)`;
#   2. no loop appending return pointer levels,
#      `for (int rs = 0; rs < ret..._stars|depth|levels; ...)`,
# except where the line is marked `// allowed-exception: <why>`.
# Two-sided: the negative control proves both patterns still bite.
set -u
cd "$(dirname "$0")/.."

PEEL='dd_peel_pointers\(ret[A-Za-z_]*, *&'
LOOP='for \((int|size_t) [a-z_]+ = 0; [a-z_]+ < [A-Za-z_]*ret[A-Za-z_]*(stars|depth|levels)'

code_lines() { grep -HnE "$1" "${@:2}" | grep -vE '^[^:]*:[0-9]+:[[:space:]]*//'; }

ctl=$(mktemp)
trap 'rm -f "$ctl"' EXIT
cat > "$ctl" <<'CTL'
	int ret_star_depth = dd_peel_pointers(ret_dd, &ret_level_cv);
	for (int rs = 0; rs < ret_decl_stars; rs++)
	// for (int rs = 0; rs < ret_decl_stars; rs++) (a comment: skipped)
	for (size_t rs = 0; rs < ret_stars; ++rs) // allowed-exception: control
CTL
c1=$(code_lines "$PEEL" "$ctl" | grep -v 'allowed-exception' | grep -c .)
c2=$(code_lines "$LOOP" "$ctl" | grep -v 'allowed-exception' | grep -c .)
if [ "$c1" -ne 1 ] || [ "$c2" -ne 1 ]; then
	echo "check-one-return-declarator: NEGATIVE CONTROL FAILED"
	echo "  a return peel matched $c1 of 1, an unmarked return-star loop $c2 of 1"
	exit 1
fi

files=$(git ls-files 'src/*.cpp' 'src/*.h')
bad=$( { code_lines "$PEEL" $files; code_lines "$LOOP" $files; } | grep -v 'allowed-exception')
n=$(printf '%s' "$bad" | grep -c . || true)
marked=$( { code_lines "$PEEL" $files; code_lines "$LOOP" $files; } | grep -c 'allowed-exception' || true)
echo "hand-rolled return declarators: $n (target 0); marked sites: $marked"
if [ "$n" -ne 0 ]; then
	printf '%s\n' "$bad"
	echo "  -> build the return type's specifiers and pieces with"
	echo "     CirBuilder::append_return_declarator (after the N_FUNC)."
	exit 1
fi
echo "GREEN -- a function's return type in a declarator has one owner."
