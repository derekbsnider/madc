#!/bin/bash
# DRIFT-PREVENTION GATE -- "is this type a function pointer" has ONE owner,
# DataDef::as_fptr_dd() (and "a function", as_funcdef_dd()).
#
# Both forward through a DataDefCONST wrapper. The two spellings they replace
# do not: `dynamic_cast<DataDefFPTR *>(dd)` is NULL for a const-qualified
# function pointer (`const fn_t *pc; (*pc)(3)` was refused by the call arms),
# and `is_function() && is_numeric()` is TRUE for one -- a DataDefCONST
# forwards both -- so the static_cast that followed it read a DataDefCONST as
# a DataDefFPTR. A bare is_function() is true for a function AND a function
# pointer; ask it only when the meaning is "either".
#
# Two rules over src/ and include/:
#   1. the oblique spelling `is_function() && ... is_numeric()` in no code line;
#   2. a `dynamic_cast<DataDefFPTR *>` only where the code asks about the FPTR
#      NODE itself (declaration rendering, a structural type walk), and then
#      marked `// allowed-exception: <why>` on the same line.
# Two-sided: the negative control proves both patterns still bite.
set -u
cd "$(dirname "$0")/.."

OBLIQUE='is_function\(\) *&& *[A-Za-z_>.()-]*is_numeric\(\)|is_numeric\(\) *&& *[A-Za-z_>.()-]*is_function\(\)'
CAST='dynamic_cast<DataDefFPTR \*>'

code_lines() { grep -HnE "$1" "${@:2}" | grep -vE '^[^:]*:[0-9]+:[[:space:]]*//'; }

ctl=$(mktemp)
trap 'rm -f "$ctl"' EXIT
cat > "$ctl" <<'CTL'
	if ( dd->is_function() && dd->is_numeric() )
	// the older `is_function() && is_numeric()` test (a comment: skipped)
	DataDefFPTR *a = dynamic_cast<DataDefFPTR *>(dd);
	DataDefFPTR *b = dynamic_cast<DataDefFPTR *>(dd); // allowed-exception: node
CTL
c1=$(code_lines "$OBLIQUE" "$ctl" | grep -c .)
c2=$(code_lines "$CAST" "$ctl" | grep -v 'allowed-exception' | grep -c .)
if [ "$c1" -ne 1 ] || [ "$c2" -ne 1 ]; then
	echo "check-one-fptr-predicate: NEGATIVE CONTROL FAILED -- oblique spelling"
	echo "  matched $c1 of 1, an unmarked DataDefFPTR cast $c2 of 1"
	exit 1
fi

files=$(git ls-files 'src/*.cpp' 'src/*.c' 'include/*.h')
ob=$(code_lines "$OBLIQUE" $files)
n1=$(printf '%s' "$ob" | grep -c . || true)
echo "oblique function-pointer tests (is_function() && is_numeric()): $n1 (target 0)"
if [ "$n1" -ne 0 ]; then
	printf '%s\n' "$ob"
	echo "  -> ask DataDef::as_fptr_dd(): it sees through a DataDefCONST and names the type."
	exit 1
fi
un=$(code_lines "$CAST" $files | grep -v 'allowed-exception')
n2=$(printf '%s' "$un" | grep -c . || true)
marked=$(code_lines "$CAST" $files | grep -c 'allowed-exception' || true)
echo "unmarked dynamic_cast<DataDefFPTR *>: $n2 (target 0); marked structural sites: $marked"
if [ "$n2" -ne 0 ]; then
	printf '%s\n' "$un"
	echo "  -> ask DataDef::as_fptr_dd() (const-safe); a structural site that must see the"
	echo "     FPTR node itself says so with // allowed-exception: <why>."
	exit 1
fi
echo "GREEN -- \"is this a function pointer\" has one owner."
