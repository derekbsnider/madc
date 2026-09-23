#!/bin/bash
# DRIFT-PREVENTION GATE -- a unary `*` has ONE operand reader and ONE builder.
#
# The operand of `*` is a cast-expression (C11 6.5.3.2, [expr.unary.op]/1).
# Program::parseCastExpression reads it -- the expression engine, bounded at the
# cast-expression -- and Program::build_indirection builds the dereference node.
# SIX hand-rolled readers preceded them (the single-, two- and N-star deref arms,
# the cast operand's deref reader, sizeof's two star paths), each a partial copy
# of the grammar and each wrong somewhere: `**c.pp()` refused, `**(q)++`
# dereferenced once, `*&x + 1` read as `*(&x + 1)`, `(char)**pp * 100` cast the
# product (tests/testderefoperandc, testderefoperandcpp).
#
# Two rules, both checked over src/*.cpp:
#   1. a deref node (TokenDeref / TokenDerefStep / TokenDerefExpr) is constructed
#      ONLY inside build_indirection -- plus the one named exception,
#      reference_bind_address_expr, which re-designates a reference CAST's object
#      (`*addr`) and reads no `*` from the source;
#   2. every build_indirection call reads its operand with parseCastExpression --
#      a new hand-rolled reader cannot hide behind the builder.
# Two-sided: the negative control proves both patterns still bite.
set -u
cd "$(dirname "$0")/.."

NEW_PAT='new TokenDeref(Expr|Step)?\('
OWNERS='^(TokenBase|static [A-Za-z_]+ \*) ?\*?Program::(build_indirection|reference_bind_address_expr)\('

# Lines constructing a deref node OUTSIDE the owner functions (a function body
# runs from its definition line to the first column-0 `}`).
outside_owners() {
	awk -v newpat="$NEW_PAT" -v owners="$OWNERS" '
		$0 ~ owners { inside = 1 }
		!inside && $0 ~ newpat { print FILENAME ":" FNR ": " $0 }
		inside && /^}/ { inside = 0 }
	' "$@"
}

# Negative control: a construction outside the owner and a builder call fed by a
# hand-rolled reader must both be caught; the same construction inside the
# owner must not.
ctl_dir=$(mktemp -d)
trap 'rm -rf "$ctl_dir"' EXIT
cat > "$ctl_dir/ctl.cpp" <<'CTL'
TokenBase *Program::build_indirection(TokenBase *operand, TokenBase *star)
{
    return new TokenDerefExpr(operand, NULL);
}
static void arm() {
    exStack.push(new TokenDerefExpr(deref_expr, base));
    exStack.push(build_indirection(parsePostfixChain(first), tb));
}
CTL
c1=$(outside_owners "$ctl_dir/ctl.cpp" | grep -c .)
c2=$(grep -E 'build_indirection\(' "$ctl_dir/ctl.cpp" | grep -vE 'Program::build_indirection|build_indirection\(parseCastExpression\(' | grep -c .)
if [ "$c1" -ne 1 ] || [ "$c2" -ne 1 ]; then
	echo "check-one-deref-builder: NEGATIVE CONTROL FAILED -- construction outside"
	echo "  the owner matched $c1 of 1, a hand-read builder operand $c2 of 1"
	exit 1
fi

bad=$(outside_owners src/*.cpp)
n=$(printf '%s' "$bad" | grep -c . || true)
echo "deref nodes built outside build_indirection: $n (target 0)"
if [ "$n" -ne 0 ]; then
	printf '%s\n' "$bad"
	echo "  -> build a dereference with Program::build_indirection(parseCastExpression(first), star)."
	exit 1
fi

calls=$(grep -nE 'build_indirection\(' src/*.cpp | grep -v 'Program::build_indirection')
ncalls=$(printf '%s' "$calls" | grep -c . || true)
hand=$(printf '%s\n' "$calls" | grep -v 'build_indirection(parseCastExpression(' | grep -c . || true)
echo "build_indirection calls: $ncalls, operand not read by parseCastExpression: $hand (target 0)"
if [ "$ncalls" -lt 1 ] || [ "$hand" -ne 0 ]; then
	printf '%s\n' "$calls"
	echo "  -> the operand of \`*\` is a cast-expression: read it with Program::parseCastExpression."
	exit 1
fi
echo "GREEN -- a unary \`*\` has one operand reader and one builder."
