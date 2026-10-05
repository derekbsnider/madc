#!/bin/bash
# check-one-event-builder.sh — a madcide test builds its client events with
# <madcide/harness>'s ev_text / ev_key / ev_action, never its own copy.
#
# The builders write the value a session reads (`event`, `event_code`, and
# the kind's fields); a copy in a test drifts from the session's reading
# unseen, and the tests then pin events no client sends. On 2026-10-05 six
# tests carried copies (one a second shared file); they were folded into the
# harness (KG DupFamily test_event_builders).
#
# Rule: a test that compiles madcide's base (madcide_core.inc) or includes
# the harness defines none of ev_text, ev_key, ev_action. Other builders (a
# pointer event, a key with an option) stay the test's own. A test of
# another editor (testvised: vised, not madcide) is outside the rule.
#
# Negative control: a synthetic madcide test defining ev_key must FAIL, and
# one using the harness's must PASS — else the gate itself is broken.

set -u
cd "$(dirname "$0")/.." || exit 2

# scan FILE... — prints each madcide test's own copy; 0 = clean.
scan() {
	local f bad=0
	for f in "$@"; do
		grep -q -E 'madcide_core\.inc|madcide/harness' "$f" || continue
		if grep -n -H -E '^(inline )?void ev_(text|key|action)\(' "$f"; then
			bad=1
		fi
	done
	return $bad
}

tmpd=$(mktemp -d)
printf '#include "../tools/madcide/madcide_core.inc"\nvoid ev_key(var &e, const char *k)\n{\n}\n' > "$tmpd/bad.mad"
printf '#include <madcide/harness>\nvoid ev_key_opt(var &e, const char *k, long o)\n{\n}\n' > "$tmpd/good.mad"
if scan "$tmpd/bad.mad" > /dev/null; then
	rm -rf "$tmpd"
	echo "check-one-event-builder: NEGATIVE CONTROL FAILED — a test's own ev_key passed" >&2
	exit 2
fi
if ! scan "$tmpd/good.mad" > /dev/null; then
	rm -rf "$tmpd"
	echo "check-one-event-builder: POSITIVE CONTROL FAILED — a test's other builder was flagged" >&2
	exit 2
fi
rm -rf "$tmpd"

files=$(git ls-files 'tests/*.mad' 'tools/chthonia/tests/*.mad')
# shellcheck disable=SC2086
if ! out=$(scan $files); then
	echo "check-one-event-builder: a madcide test builds its own client events:" >&2
	echo "$out" >&2
	echo "  -> use <madcide/harness>'s ev_text / ev_key / ev_action" >&2
	exit 1
fi
echo "check-one-event-builder: OK (madcide tests build their events through <madcide/harness>)"
