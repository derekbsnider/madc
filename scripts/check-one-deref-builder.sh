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
#      a new hand-rolled reader cannot hide behind the builder;
#   3. the `&` twin: an address-of node (TokenAddrOf / TokenAddrExpr) is built
#      only by build_address_of, by parseAddressOfExpression's two kept arms (a
#      compound literal; an UNPARENTHESIZED qualified-id, which
#      [expr.unary.op]/4 decides on the spelling), and by
#      reference_bind_address_expr (reference binding). The & reader once
#      hand-read its operand too: `&*p` refused, and `&f` on a function-POINTER
#      variable returned `f` (tests/testaddrofoperand).
#   4. what an ARRAY operand denotes (its element -- the ROW, multi-dimensional)
#      is Program::array_operand_element_type's: madc stores an array flattened,
#      so a pointer minted from a node's flattened type (`getPointerType(tv->
#      var.type)`) is the scalar's, not the row's. Four sites did that; an array
#      of function pointers read as one (`(*table)(5)` emitted `table(5)`,
#      `sizeof(*table)` 16) and rows decayed to the scalar (tests/
#      testfptrarrayderef, testarrayrowderef);
#   5. the END of an expression is Program::finish_expression: a second operator
#      drain (`while ( !opStack.empty() )`) elsewhere in parser.cpp is a copy
#      without the juxtaposition check -- the conditional-end copy built
#      `int r = (x)(4)` as `int r = x = 4` (tests/testjuxtaposeinit).
#   6. the cast arm reads its operand with parseCastExpression and nothing
#      else: between `cast_expr_tb = nextToken()` and the TokenCast it
#      builds, exactly one parseCastExpression and no other reader. Nine shape
#      arms read it before; the literal arm read only the literal
#      (`(long)"abc"[1]`, tests/testcastoperand).
# Two-sided: the negative control proves every pattern still bites.
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
    exStack.push(new TokenAddrOf(*var, aptr));
}
CTL
c1=$(outside_owners "$ctl_dir/ctl.cpp" | grep -c .)
c3=$(awk '/^TokenBase \*Program::(build_address_of|parseAddressOfExpression|reference_bind_address_expr)\(/ { inside = 1 }
	!inside && /new TokenAddr(Of|Expr)\(/ { n++ } inside && /^}/ { inside = 0 } END { print n+0 }' "$ctl_dir/ctl.cpp")
c2=$(grep -E 'build_indirection\(' "$ctl_dir/ctl.cpp" | grep -vE 'Program::build_indirection|build_indirection\(parseCastExpression\(' | grep -c .)
if [ "$c1" -ne 1 ] || [ "$c2" -ne 1 ] || [ "$c3" -ne 1 ]; then
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
ADDR_PAT='new TokenAddr(Of|Expr)\('
ADDR_OWNERS='^TokenBase \*Program::(build_address_of|parseAddressOfExpression|reference_bind_address_expr)\('
abad=$(awk -v newpat="$ADDR_PAT" -v owners="$ADDR_OWNERS" '
	$0 ~ owners { inside = 1 }
	!inside && $0 ~ newpat { print FILENAME ":" FNR ": " $0 }
	inside && /^}/ { inside = 0 }
' src/*.cpp)
an=$(printf '%s' "$abad" | grep -c . || true)
echo "address-of nodes built outside build_address_of and its two kept arms: $an (target 0)"
if [ "$an" -ne 0 ]; then
	printf '%s\n' "$abad"
	echo "  -> build an address-of with Program::build_address_of(parseCastExpression(first), amp)."
	exit 1
fi
# The & reader reads no operand by hand: its body holds exactly one
# parseExpression (the compound-literal arm) and no parsePostfixChain, and
# every build_address_of call is fed by parseCastExpression. (The hand-read
# reader had four parseExpression calls and one parsePostfixChain.)
amp_body() {
	awk '/^TokenBase \*Program::parseAddressOfExpression\(/ { inside = 1 }
	     inside { print } inside && /^}/ { exit }' "$@"
}
npe=$(amp_body src/parser.cpp | grep -c 'parseExpression(' || true)
npc=$(amp_body src/parser.cpp | grep -c 'parsePostfixChain(' || true)
acalls=$(grep -nE 'build_address_of\(' src/*.cpp | grep -v 'Program::build_address_of')
ahand=$(printf '%s\n' "$acalls" | grep -v 'build_address_of(parseCastExpression(' | grep -c . || true)
echo "& reader: parseExpression $npe (target 1, the compound literal), parsePostfixChain $npc (target 0), hand-fed build_address_of $ahand (target 0)"
if [ "$npe" -ne 1 ] || [ "$npc" -ne 0 ] || [ "$ahand" -ne 0 ]; then
	echo "  -> the operand of \`&\` is a cast-expression: read it with Program::parseCastExpression."
	exit 1
fi
# 4. array decay from a flattened node type, outside the owner
DECAY_PAT='getPointerType\([A-Za-z_]+(->|\.)(var|object)\.type\)'
DECAY_OWNER='^DataDef \*Program::array_operand_element_type\('
decay_outside() {
	awk -v pat="$DECAY_PAT" -v owner="$DECAY_OWNER" '
		$0 ~ owner { inside = 1 }
		!inside && $0 ~ pat { print FILENAME ":" FNR ": " $0 }
		inside && /^}/ { inside = 0 }
	' "$@"
}
# 5. an operator-stack drain outside finish_expression
drain_outside() {
	awk '/^TokenBase \*Program::finish_expression\(/ { inside = 1 }
	     !inside && /while \( !opStack\.empty\(\) \)$/ { print FILENAME ":" FNR ": " $0 }
	     inside && /^}/ { inside = 0 }' "$@"
}
cat > "$ctl_dir/ctl2.cpp" <<'CTL'
DataDef *Program::array_operand_element_type(TokenBase *e)
{
    return getPointerType(tv->var.type);
}
TokenBase *Program::finish_expression(std::stack<TokenBase *> &o, std::stack<TokenBase *> &x)
{
    while ( !opStack.empty() )
	popOperator(opStack, exStack);
}
static void arm() {
	    return getPointerType(tm->var.type);
	    while ( !opStack.empty() )
		popOperator(opStack, exStack);
}
CTL
d1=$(decay_outside "$ctl_dir/ctl2.cpp" | grep -c .)
d2=$(drain_outside "$ctl_dir/ctl2.cpp" | grep -c .)
if [ "$d1" -ne 1 ] || [ "$d2" -ne 1 ]; then
	echo "check-one-deref-builder: NEGATIVE CONTROL FAILED -- flattened decay matched"
	echo "  $d1 of 1, an operator drain outside finish_expression $d2 of 1"
	exit 1
fi
dbad=$(decay_outside src/*.cpp)
dn=$(printf '%s' "$dbad" | grep -c . || true)
echo "array decay minted from a flattened node type outside array_operand_element_type: $dn (target 0)"
if [ "$dn" -ne 0 ]; then
	printf '%s\n' "$dbad"
	echo "  -> ask Program::array_operand_element_type / array_decay_pointer (the row, not the scalar)."
	exit 1
fi
rbad=$(drain_outside src/parser.cpp)
rn=$(printf '%s' "$rbad" | grep -c . || true)
echo "operator-stack drains outside finish_expression: $rn (target 0)"
if [ "$rn" -ne 0 ]; then
	printf '%s\n' "$rbad"
	echo "  -> end the expression with Program::finish_expression (it refuses juxtaposed operands)."
	exit 1
fi
# 6. the cast arm's operand reader
cast_body() {
	awk '/TokenBase \*cast_expr_tb = nextToken\(\);/ { inside = 1 }
	     inside { print } inside && /new TokenCast\(cast_dd, cast_expr\)/ { exit }' "$@"
}
cat > "$ctl_dir/ctl3.cpp" <<'CTL'
			    TokenBase *cast_expr_tb = nextToken();
			    TokenBase *cast_expr = parseCastExpression(cast_expr_tb);
			    if ( lit ) cast_expr = parsePostfixChain(cast_expr_tb);
			    exStack.push(new TokenCast(cast_dd, cast_expr));
CTL
k1=$(cast_body "$ctl_dir/ctl3.cpp" | grep -vE '^[[:space:]]*//' | grep -oE '(try_)?parse[A-Za-z_]+\(|materialize_[a-z_]+\(|evaluate_type_query\(' | grep -vc '^parseCastExpression(')
if [ "$k1" -ne 1 ]; then
	echo "check-one-deref-builder: NEGATIVE CONTROL FAILED -- a second cast-operand"
	echo "  reader matched $k1 of 1"
	exit 1
fi
cb=$(cast_body src/parser.cpp | grep -vE '^[[:space:]]*//')
ncast=$(printf '%s\n' "$cb" | grep -c 'parseCastExpression(' || true)
nother=$(printf '%s\n' "$cb" | grep -oE '(try_)?parse[A-Za-z_]+\(|materialize_[a-z_]+\(|evaluate_type_query\(' | grep -vc '^parseCastExpression(' || true)
echo "cast operand: parseCastExpression $ncast (target 1), other readers $nother (target 0)"
if [ "$ncast" -ne 1 ] || [ "$nother" -ne 0 ]; then
	printf '%s\n' "$cb" | grep -nE 'parse[A-Z][A-Za-z]*\(|materialize_'
	echo "  -> the operand of a cast is a cast-expression: Program::parseCastExpression reads it."
	exit 1
fi
echo "GREEN -- a unary \`*\` and a unary \`&\` each have one operand reader and one builder;"
echo "  a cast reads its operand with the same reader; an array operand's element and"
echo "  the end of an expression each have one owner."
