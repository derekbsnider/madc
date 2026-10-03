#!/bin/bash
# DRIFT-PREVENTION GATE -- "turn a MIR definition into an import in place" has
# ONE owner, def_item_to_import() (third_party/mir/mir.c).
#
# MIR_module_privatize_for_link converted a data definition by hand, and the
# loader's vague-linkage rule (a later module's linkonce copy binds to the
# first) needs the same conversion for funcs and data. The conversion must
# keep the item's identity, free the right payload, and reuse the interned
# name the item table hashes; a second copy would drift on one of the three.
#
# One rule over MIR and madc: no code line sets an item's type to
# MIR_import_item by hand, unless it is marked `// allowed-exception: <why>`
# or `/* allowed-exception: <why> */` on the same line. Two-sided: the
# negative control proves the pattern bites.
set -u
cd "$(dirname "$0")/.."

SCAN='(\.|->)item_type *= *MIR_import_item'

code_lines() { grep -HnE "$1" "${@:2}" | grep -vE '^[^:]*:[0-9]+:[[:space:]]*(//|/?\*)'; }

ctl=$(mktemp)
trap 'rm -f "$ctl"' EXIT
cat > "$ctl" <<'CTL'
      item->item_type = MIR_import_item;
  item->item_type = MIR_import_item; /* allowed-exception: control */
  /* the older `item->item_type = MIR_import_item` by hand (a comment: skipped) */
CTL
c=$(code_lines "$SCAN" "$ctl" | grep -v 'allowed-exception' | grep -c .)
if [ "$c" -ne 1 ]; then
	echo "check-one-mir-def-to-import: NEGATIVE CONTROL FAILED -- matched $c of 1"
	exit 1
fi

files=$(git ls-files 'third_party/mir/*.c' 'third_party/mir/c2mir/*.c' 'src/*.cpp' 'src/*.c')
un=$(code_lines "$SCAN" $files | grep -v 'allowed-exception')
n=$(printf '%s' "$un" | grep -c . || true)
echo "hand-written definition-to-import conversions: $n (target 0)"
if [ "$n" -ne 0 ]; then
	printf '%s\n' "$un"
	echo "  -> call def_item_to_import() (mir.c)."
	exit 1
fi
echo "GREEN -- \"a MIR definition becomes an import in place\" has one owner."
