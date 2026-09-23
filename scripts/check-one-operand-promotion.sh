#!/bin/bash
# DRIFT-PREVENTION GATE -- an arithmetic operand's parse-side type has ONE
# owner per step (include/tokens.h, defined in src/parser.cpp):
#   operand_value_type     the value it denotes ([expr]/5: a reference -- a
#                          DataDefREF, or a reference VARIABLE madc lowers as
#                          its pointer -- is its referent);
#   promoted_operand_type  that value after the integer promotions
#                          ([conv.prom]; bit-field and enum aware), over the
#                          type-level core integer_promoted_type.
#
# Every operator's datadef() read its children as `left->datadef()` and
# re-derived the promotion by hand: unary `~`/`-` kept the operand's type
# (`f(~uc)` picked f(unsigned char)), the shifts spelled the int floor out
# twice, a bit-field kept its declared unsigned type, and a reference operand
# typed `rl + 2` as the reference's POINTER (auto bound a pointer to 42 and
# crashed).
#
# Two rules:
#   1. no `left->datadef()` / `right->datadef()` in include/tokens.h code
#      lines -- a child is read through an owner;
#   2. the int promotion floor (`ddINT.size`) is spelled only inside the
#      owners: usual_arithmetic_result, integer_promoted_type,
#      promoted_operand_type.
# Two-sided: the negative control proves both rules still bite.
set -u
cd "$(dirname "$0")/.."

CHILD='\b(left|right)->datadef\(\)'
ALLOWED='usual_arithmetic_result|integer_promoted_type|promoted_operand_type'

code_lines() { grep -HnE "$1" "${@:2}" | grep -vE '^[^:]*:[0-9]+:[[:space:]]*//'; }

# floor_sites FILE... : every code line spelling ddINT.size outside the owners,
# naming the function it sits in (the last column-0 definition line above it;
# a column-0 `}` or a class/struct head ends it, so an indented method body in
# a class is never credited to the free function before the class).
floor_sites() {
	awk -v allowed="^($ALLOWED)$" '
		FNR == 1 { fn = "" }
		/^[A-Za-z_][^;]*[A-Za-z_0-9]+[ \t]*\(/ && $0 !~ /;[ \t]*$/ {
			line = $0; sub(/\(.*/, "", line); n = split(line, w, /[^A-Za-z_0-9]+/)
			fn = w[n]
		}
		/^(class|struct)[ \t]/ { fn = "" }
		/ddINT\.size/ && $0 !~ /^[ \t]*\/\// && fn !~ allowed {
			print FILENAME ":" FNR ": [" fn "] " $0
		}
		/^}/ { fn = "" }' "$@"
}

ctl=$(mktemp -d)
trap 'rm -rf "$ctl"' EXIT
cat > "$ctl/t.h" <<'CTL'
	DataDef *ld = left ? left->datadef() : NULL;
	// a comment naming right->datadef() is skipped
	DataDef *rd = promoted_operand_type(right);
CTL
cat > "$ctl/f.cpp" <<'CTL'
DataDef *integer_promoted_type(DataDef *dd)
{
    if ( dd->size < ddINT.size ) return dd;
}
static DataDef *shift_type(DataDef *ld)
{
    if ( ld->size == ddINT.size && ld->is_unsigned() ) return ld;
}
class TokenShift: public TokenOperator
{
    virtual DataDef *datadef() const override
    { if ( ld->size > ddINT.size ) return ld; }
};
CTL
c1=$(code_lines "$CHILD" "$ctl/t.h" | grep -c .)
c2=$(floor_sites "$ctl/f.cpp" | grep -c .)
if [ "$c1" -ne 1 ] || [ "$c2" -ne 2 ]; then
	echo "check-one-operand-promotion: NEGATIVE CONTROL FAILED -- a direct child"
	echo "  read matched $c1 of 1, a hand-rolled int floor $c2 of 2"
	exit 1
fi

cr=$(code_lines "$CHILD" include/tokens.h)
n1=$(printf '%s' "$cr" | grep -c . || true)
echo "direct child datadef() reads in include/tokens.h: $n1 (target 0)"
if [ "$n1" -ne 0 ]; then
	printf '%s\n' "$cr"
	echo "  -> read the operand through operand_value_type (its value) or"
	echo "     promoted_operand_type (its promoted value)."
	exit 1
fi
fl=$(floor_sites $(git ls-files 'src/*.cpp' 'include/*.h'))
n2=$(printf '%s' "$fl" | grep -c . || true)
echo "int promotion floor spelled outside the owners: $n2 (target 0)"
if [ "$n2" -ne 0 ]; then
	printf '%s\n' "$fl"
	echo "  -> ask promoted_operand_type / integer_promoted_type (tokens.h)."
	exit 1
fi
echo "GREEN -- an arithmetic operand's value and promoted type each have one owner."
