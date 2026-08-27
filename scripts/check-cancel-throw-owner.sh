#!/usr/bin/env bash
# check-cancel-throw-owner.sh — the current-task cancellation check-and-throw
# has ONE owner.
#
# Rule (DupFamily current_task_cancel_throw, consolidated with MT-3):
# throwing THE cancelled literal for the CURRENT task happens only in
# __madc_task_throw_if_cancelled (rt_task.c) — every blocking verb's entry
# and resume gates call it. MT-5's keyword verbs and future blocking
# surfaces are exactly where a hand-spelled copy would be born.
#
# Marker: the direct call `__madc_throw_cstr(__madc_task_cancelled_text())`.
# Exactly two are legal, BOTH in rt_task.c: the owner itself, and
# scope_end's OUTCOME throw (a different rule — rethrowing a scope's
# cancelled state, not checking the current task). Zero elsewhere in src/.
set -u

SRCDIR="$(dirname "$0")/../src"
MARKER='__madc_throw_cstr(__madc_task_cancelled_text())'

count_in()
{
	grep -rF "$MARKER" "$@" 2>/dev/null | wc -l | tr -d ' '
}

n_task=$(grep -cF "$MARKER" "$SRCDIR/rt/rt_task.c")
n_all=$(count_in "$SRCDIR" --include='*.c' --include='*.cpp' --include='*.h')
if [ "$n_task" -ne 2 ] || [ "$n_all" -ne 2 ]; then
	echo "check-cancel-throw-owner: FAIL — $n_task site(s) in rt_task.c," \
	     "$n_all in src/ (expected 2/2: the owner + scope_end's outcome" \
	     "throw, both in rt_task.c). A new cancellation gate must call" \
	     "__madc_task_throw_if_cancelled — see the DupFamily" \
	     "current_task_cancel_throw." >&2
	exit 1
fi

# Negative control: a synthetic hand-spelled copy must be detected.
tmp=$(mktemp --suffix=.cpp)
echo "	__madc_throw_cstr(__madc_task_cancelled_text());	// synthetic" > "$tmp"
if [ "$(grep -cF "$MARKER" "$tmp")" -ne 1 ]; then
	rm -f "$tmp"
	echo "check-cancel-throw-owner: FAIL — negative control did not" \
	     "match a synthetic copy (the marker went blind)." >&2
	exit 1
fi
rm -f "$tmp"

echo "check-cancel-throw-owner: OK (one gate owner: __madc_task_throw_if_cancelled)"
exit 0
