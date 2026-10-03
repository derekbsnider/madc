#!/bin/bash
# RATCHET GATE -- a ptr-operator read from tokens goes through the declarator
# owner. A `*` (with its cv), `&`, `&&`, `( declarator )` and the suffixes are
# Program::parse_declarator's (a type-id: Program::parse_type_id; a `*`+cv
# run: consume_declarator_stars). A loop that tests a token for `*` and folds
# getPointerType over it by hand reads a SUBSET of that grammar, and the
# missing part is the bug.
#
# The trailing return type was read that way twice (parseFunction's eager
# path and the deferred body replay): no leading cv, no `(&)[N]`, no
# `(*)(params)`, and what the loop did not read was dropped, so `-> int
# (*)(int)` became `int` (sizeof 4) and a member's `-> const int (&)[3]` fell
# back to body deduction (sizeof 8). Both now read through
# Program::adopt_trailing_return_type over parse_type_id (B132;
# tests/testtrailingreturntypeid).
#
# Marker: a line testing `TokenID::tkMul` with `getPointerType(` on it or on
# one of the next two lines, over the tracked src/*.cpp and include/*.h.
# Ratchet: the count never rises above BASELINE, and a count below it fails
# until BASELINE is lowered in the same commit. Migrate a site to the owner;
# never add an exemption.
set -u
cd "$(dirname "$0")/.."

# 9 before B132 (2026-10-02); the two trailing-return readers moved to
# parse_type_id. 7 -> 5 the same day: a named cast's `< type-id >`, its
# expression and its constant form, reads through parse_named_cast_target
# over parse_type_id (tests/testnamedcasttypeid). The rest: a
# template-argument spelling, the `T *` / `T &` member suffix, a pointer
# base in a class-pattern spelling, a comma declarator's return type, and a
# multi-return entry type (KG DupFamily hand_rolled_ptr_operator_fold).
BASELINE=5

scan() {
	awk '
	FNR == 1 { m = 0 }
	/TokenID::tkMul/ { m = FNR }
	/getPointerType\(/ && m && FNR - m <= 2 { print FILENAME ":" FNR; m = 0 }
	' "$@"
}

ctl=$(mktemp)
trap 'rm -f "$ctl"' EXIT
cat > "$ctl" <<'CTL'
	    if ( s && s->id() == TokenID::tkMul )
		{ nextToken(); new_ret = getPointerType(new_ret); continue; }
	while ( next && next->id() == TokenID::tkMul )
	{
	    next_return = getPointerType(next_return);
	}
	DataDef *t = parse_type_id(&rtt->definition, lead_cv, decl);
	if ( t->id() == TokenID::tkMul )
	    ++stars;
	report(stars);
	DataDef *p = getPointerType(base);
CTL
c=$(scan "$ctl" | grep -c .)
if [ "$c" -ne 2 ]; then
	echo "check-one-ptr-operator-fold: NEGATIVE CONTROL FAILED -- matched $c of 2"
	scan "$ctl"
	exit 1
fi

files=$(git ls-files 'src/*.cpp' 'include/*.h')
hits=$(scan $files)
n=$(printf '%s' "$hits" | grep -c . || true)
echo "ptr-operator folds read by hand: $n (baseline $BASELINE, target 0)"
if [ "$n" -gt "$BASELINE" ]; then
	echo "REGRESSION -- a new hand-rolled ptr-operator fold:"
	printf '%s\n' "$hits" | sed 's/^/  /'
	echo "  -> read a type-id with Program::parse_type_id, a declarator with"
	echo "     Program::parse_declarator, a \`*\`+cv run with consume_declarator_stars."
	exit 1
fi
if [ "$n" -lt "$BASELINE" ]; then
	echo "RATCHET FORWARD -- $((BASELINE - n)) site(s) migrated."
	echo "Lower BASELINE in $0 to $n to lock the gain in."
	exit 1
fi
echo "held at baseline -- $n site(s) still to migrate."
exit 0
