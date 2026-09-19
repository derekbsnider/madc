#!/bin/bash
# RATCHET GATE — ONE declarator reader in the parser.
#
# The rule: a C/C++ declarator ([dcl.decl] named, [dcl.name] abstract) —
# ptr-operators (`*` cv, `&`, `&&`, `C::[D::]*` cv), then `( declarator )` or a
# declarator-id, then `[dim]` / `(params)` suffixes applied inside-out — is read
# from the token stream by ONE recursive owner, Program::parse_declarator
# (src/parser.cpp), composed from the existing partial owners
# (consume_declarator_stars, parse_member_pointer_owner,
# parse_member_fnptr_declarator, parse_fnptr_member_tail, parseFnPtrParams,
# parse_ptr_array_suffix). No arm hand-rolls its own reading of that grammar.
#
# Why: on 2026-09-19 the tree held FOURTEEN token-consuming declarator readers
# (parseFunction params, parseDeclaration vars, three typedef arms + a list
# tail, struct/class members, K&R params, using-alias, template-argument
# type-id, cast type-id, sizeof type-id, parseFnPtrParams' own per-parameter
# block) beside NINE owners they never adopted. Each copy read a SUBSET of the
# grammar and the missing part was the bug: the typedef arm had no
# `(&N)[K]`, the alias arm had NO array suffix, the template-argument reader
# had no `(C::*)(A)`, and the two array-suffix owners DISAGREED (one nests a
# CArray per dimension, the other multiplied every dimension into one count,
# so `typedef int M[2][3]; M m; m[1][2]` refused). Twenty g++.dg tests hung on
# it. Plan: docs/plans/2026-09-19-declarator-reader-consolidation.md.
#
# Markers — the CONCEPT "construct a derived declarator type from tokens" and
# "read a parameter-type-list", not one spelling of a reader:
#   parseFnPtrParams([^)]      calls WITH an argument (comments say `()`), plus
#                              the definition itself
#   new DataDefFPTR(           function / function-pointer type construction
#   new DataDefCArray(         array type construction
#   new DataDefMemberPtr(      pointer-to-data-member construction
#   new DataDefMemberFnPtr(    pointer-to-member-function construction
#
# Non-declarator sites — they build from a TREE or an expression, not from
# declarator tokens, and stay for good (counted inside the end-state numbers):
#   peel/rebuild of alias dims (~3245), __builtin_va_list (~3505),
#   ClassTypePattern CArray resolve (~8260), string-literal type (~13892),
#   `&C::member` constants (~31966 MemberFnPtr, ~31982 MemberPtr),
#   ternary call typing (~42332), deduction decay (~60273),
#   fn-ptr variable bound from a function (~72627), VLA element type (~73831).
#
# Ratchet: a count must never RISE above BASELINE. Each migration LOWERS the
# baseline in the same commit (a count below BASELINE without that edit also
# fails — the owner was renamed or a reader was deleted without saying so).
# Never add an exemption; migrate the reader to the owner.
#
# Negative controls: (1) every marker must still match the known-good owner
# line it was keyed on — a marker that stops matching the owner has drifted;
# (2) `--selftest` mutates a COPY of the source (one extra construction, then
# one removed) and requires the gate to FAIL both ways.
set -u
cd "$(dirname "$0")/.."

SRC="${MADC_GATE_SRC:-src/parser.cpp}"   # override exists ONLY for --selftest

# BASELINE (measured 2026-09-19 @ 85f3c91d4) -> END STATE after the plan lands
BASE_FNPTRPARAMS=11   # -> 2  (definition + the owner's suffix call)
BASE_FPTR=14          # -> 11 (owner x2 + the 9 non-declarator sites above)
BASE_CARRAY=8         # -> 5  (owner x1 + 4 non-declarator sites)
BASE_MEMBERPTR=6      # -> 2  (owner + the &C::field constant)
BASE_MEMBERFNPTR=3    # -> 2  (owner + the &C::method constant)

status=0
check() {
	local label="$1" pattern="$2" baseline="$3"
	local count
	count=$(grep -c -- "$pattern" "$SRC")
	if [ "$count" -gt "$baseline" ]; then
		echo "REGRESSION — $count '$label' site(s) in $SRC (baseline $baseline)."
		echo "A new declarator reader was written beside Program::parse_declarator — migrate it, do not add an exemption:"
		grep -n -- "$pattern" "$SRC"
		status=1
	elif [ "$count" -lt "$baseline" ]; then
		echo "REGRESSION — $count '$label' site(s) in $SRC, BELOW baseline $baseline."
		echo "A reader was migrated or the owner renamed: lower BASELINE in THIS commit (never silently)."
		status=1
	fi
}

check "parseFnPtrParams call"      'parseFnPtrParams([^)]'     "$BASE_FNPTRPARAMS"
check "new DataDefFPTR("           'new DataDefFPTR('          "$BASE_FPTR"
check "new DataDefCArray("         'new DataDefCArray('        "$BASE_CARRAY"
check "new DataDefMemberPtr("      'new DataDefMemberPtr('     "$BASE_MEMBERPTR"
check "new DataDefMemberFnPtr("    'new DataDefMemberFnPtr('   "$BASE_MEMBERFNPTR"

# Negative control (1): the markers must match the owners' own lines. Until
# parse_declarator lands (plan T3) the owners are the partial ones; update
# these anchors in the commit that folds them in.
control() {
	local what="$1" pattern="$2"
	if ! grep -q -- "$pattern" "$SRC"; then
		echo "REGRESSION — negative control failed: marker no longer matches $what."
		echo "If the owner moved or was renamed, re-anchor this gate in the same commit."
		status=1
	fi
}
control "parseFnPtrParams' definition"               'FuncDef \*Program::parseFnPtrParams(DataDef &returns)'
control "parse_fnptr_member_tail's FPTR construction" 'return new DataDefFPTR(func);'
control "nest_carray_dims' CArray construction"       'DataDefCArray \*level = new DataDefCArray(\*arr, nm, dims\[i\],'
control "parse_member_fnptr_declarator's construction" 'return new DataDefMemberFnPtr(owner, owner_name, fp ? fp->target : NULL, const_method);'
control "parse_declarator's member-fn-ptr fold"        'dd = new DataDefMemberFnPtr(owner, owner_name, fresh_fn->target,'
control "parse_declarator's function-type suffix"      'DataDefFPTR \*fp = new DataDefFPTR(func);'
control "parse_declarator's fn-pointer twin over a function typedef" 'DataDefFPTR \*twin = new DataDefFPTR(fn_base->target);'
control "parse_member_pointer_owner adoption (variable arm)" 'decl_type = new DataDefMemberPtr(mp_owner, mp_owner_name, \*decl_type);'

if [ "${1:-}" = "--selftest" ]; then
	# Negative control (2): the gate must FAIL on a mutated copy, both ways.
	if [ -n "${MADC_GATE_SRC:-}" ]; then
		echo "selftest: refusing to recurse under MADC_GATE_SRC"; exit 2
	fi
	work=tmp/gate-declarator-selftest
	mkdir -p "$work"
	cp src/parser.cpp "$work/plus.cpp"
	printf '%s\n' 'static DataDef *gate_selftest_extra() { return new DataDefFPTR(NULL); }' >> "$work/plus.cpp"
	if MADC_GATE_SRC="$work/plus.cpp" bash "$0" >/dev/null 2>&1; then
		echo "SELFTEST FAILED — an extra 'new DataDefFPTR(' was not caught."; exit 1
	fi
	grep -v 'return new DataDefFPTR(func);' src/parser.cpp > "$work/minus.cpp"
	if MADC_GATE_SRC="$work/minus.cpp" bash "$0" >/dev/null 2>&1; then
		echo "SELFTEST FAILED — a deleted owner construction was not caught."; exit 1
	fi
	rm -rf "$work"
	echo "one-declarator-reader gate selftest: GREEN — both mutations refused."
fi

if [ "$status" -ne 0 ]; then
	exit 1
fi
echo "one-declarator-reader gate: GREEN — declarator readers at baseline ($BASE_FNPTRPARAMS/$BASE_FPTR/$BASE_CARRAY/$BASE_MEMBERPTR/$BASE_MEMBERFNPTR)."
exit 0
