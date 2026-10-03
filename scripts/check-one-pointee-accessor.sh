#!/bin/bash
# DRIFT-PREVENTION GATE -- "is this type a pointer, and what does one level
# point at" has ONE owner, DataDef::as_pointer_dd() (null-safe:
# pointer_dd_of()).
#
# as_pointer_dd() forwards through a DataDefQUAL wrapper; the spelling it
# replaces does not: `dynamic_cast<DataDefPTR *>(dd)` is NULL for a QUALIFIED
# pointer (`int *volatile` -- a member's or a typedef's top-level cv is its
# type's), so `n.next->v` through a `struct N *volatile next` member was
# refused ("not a typed pointer") and `n.p + 2` stepped by the pointer's own
# size. 119 casts existed on 2026-09-23.
#
# The rule over src/ and include/: a `dynamic_cast<DataDefPTR *>` (or its
# const twin) only where the code dispatches on the node's EXACT class -- a
# type rebuild (subst_datadef: a PTR arm before the QUAL arm must not swallow
# QUAL(PTR)), a substitution key, a forest record, a structural spelling --
# and then marked `// allowed-exception: <why>` on the same line.
# Two-sided: the negative control proves the pattern still bites.
set -u
cd "$(dirname "$0")/.."

CAST='dynamic_cast<(const )?DataDefPTR \*>'

code_lines() { grep -HnE "$1" "${@:2}" | grep -vE '^[^:]*:[0-9]+:[[:space:]]*//'; }

ctl=$(mktemp)
trap 'rm -f "$ctl"' EXIT
cat > "$ctl" <<'CTL'
	DataDefPTR *a = dynamic_cast<DataDefPTR *>(dd);
	const DataDefPTR *b = dynamic_cast<const DataDefPTR *>(dd);
	// dynamic_cast<DataDefPTR *>(dd) in a comment: skipped
	DataDefPTR *c = dynamic_cast<DataDefPTR *>(dd); // allowed-exception: node
CTL
c1=$(code_lines "$CAST" "$ctl" | grep -v 'allowed-exception' | grep -c .)
if [ "$c1" -ne 2 ]; then
	echo "check-one-pointee-accessor: NEGATIVE CONTROL FAILED -- an unmarked DataDefPTR cast"
	echo "  matched $c1 of 2"
	exit 1
fi

files=$(git ls-files 'src/*.cpp' 'src/*.c' 'include/*.h')
un=$(code_lines "$CAST" $files | grep -v 'allowed-exception')
n=$(printf '%s' "$un" | grep -c . || true)
marked=$(code_lines "$CAST" $files | grep -c 'allowed-exception' || true)
echo "unmarked dynamic_cast<DataDefPTR *>: $n (target 0); marked structural sites: $marked"
if [ "$n" -ne 0 ]; then
	printf '%s\n' "$un"
	echo "  -> ask DataDef::as_pointer_dd() / pointer_dd_of(): a qualified pointer IS a pointer."
	echo "     A site that must see the PTR node itself (a rebuild, a key, a record) says so"
	echo "     with // allowed-exception: <why>."
	exit 1
fi
echo "GREEN -- \"one pointee level\" has one owner."
