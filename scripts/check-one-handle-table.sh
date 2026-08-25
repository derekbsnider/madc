#!/bin/bash
# check-one-handle-table.sh — ONE slot+1 handle registry (AST-1 dupaudit).
#
# The rule: handle registries (handle = slot index + 1; get bounds-checks;
# close deletes + NULLS the slot so handles never reuse) have ONE owner —
# include/handle_table.h. Four hand-rolled copies existed (ui_sessions,
# ui_tuis, and the two parse-handle registries); a fifth must not appear.
#
# Markers (the concept's two distinctive spellings):
#   1. the handle bounds test:      < 1 || (size_t)NAME >
#   2. the slot-nulling assignment: [(size_t)NAME - 1] = (
# Any match outside include/handle_table.h is a new hand-rolled registry —
# migrate it to handle_table<T>; never add an exemption here.
set -u
cd "$(dirname "$0")/.."

fail=0

scan() {
	# $1 = ERE pattern, $2 = label
	local hits
	hits=$(grep -rnE "$1" src include \
		--include='*.cpp' --include='*.h' --include='*.inc' \
		2>/dev/null | grep -v '^include/handle_table.h:')
	if [ -n "$hits" ]; then
		echo "check-one-handle-table: hand-rolled $2 outside handle_table.h:"
		echo "$hits"
		fail=1
	fi
}

scan '< 1 \|\| \(size_t\)[a-zA-Z_]+ >' 'handle bounds test'
scan '\[\(size_t\)[a-zA-Z_]+ - 1\] = \(' 'slot-nulling assignment'

# Negative control: each marker must catch a synthetic violation, or the
# gate is dead and the verdict means nothing.
ctrl=$(mktemp)
cat > "$ctrl" <<'EOF'
if ( h < 1 || (size_t)h > v.size() ) return 0;
v[(size_t)h - 1] = (thing *)0;
EOF
if ! grep -qE '< 1 \|\| \(size_t\)[a-zA-Z_]+ >' "$ctrl"; then
	echo "check-one-handle-table: NEGATIVE CONTROL FAILED (bounds marker)"
	fail=1
fi
if ! grep -qE '\[\(size_t\)[a-zA-Z_]+ - 1\] = \(' "$ctrl"; then
	echo "check-one-handle-table: NEGATIVE CONTROL FAILED (null marker)"
	fail=1
fi
rm -f "$ctrl"

if [ "$fail" -ne 0 ]; then
	exit 1
fi
echo "check-one-handle-table: OK (one owner, controls live)"
