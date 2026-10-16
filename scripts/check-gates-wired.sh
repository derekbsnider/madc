#!/usr/bin/env bash
# check-gates-wired.sh — every one-owner gate runs in `make gates`.
#
# A consolidated family leaves a gate behind in fulltest
# (.claude/rules/branching.md); `fulltest` runs `gates`, and src/Makefile's
# `gates` target names its scripts one by one. A scripts/check-one-*.sh that
# is not named there runs only in the per-commit loop and the seam battery's
# static stage, never in fulltest — the family it guards can regrow under a
# green fulltest. Two-sided: the negative control proves the check still bites.
set -u
cd "$(dirname "$0")/.." || exit 2

# unwired MAKEFILE GATE... — prints each gate the `gates` target never runs.
unwired()
{
	local mk="$1" g
	shift
	for g in "$@"; do
		awk '
			/^gates:/ { on = 1; next }
			on && /^[^\t]/ { exit }
			on { print }' "$mk" | grep -qF "scripts/$(basename "$g")" \
			|| echo "$g"
	done
}

mapfile -t gates < <(git ls-files --cached --others --exclude-standard \
		     'scripts/check-one-*.sh')
missing=$(unwired src/Makefile "${gates[@]}")
if [ -n "$missing" ]; then
	echo "check-gates-wired: one-owner gate(s) not run by src/Makefile's" \
	     "'gates' target (so not by fulltest):" >&2
	printf '  %s\n' $missing >&2
	exit 1
fi

# Controls: a gates target naming one script, against two scripts.
ctl=$(mktemp)
trap 'rm -f "$ctl"' EXIT
printf 'gates: deps\n\t@cd .. && bash scripts/check-one-a.sh\n\nother:\n\t@cd .. && bash scripts/check-one-b.sh\n' \
	> "$ctl"
if [ "$(unwired "$ctl" scripts/check-one-a.sh scripts/check-one-b.sh)" != \
     "scripts/check-one-b.sh" ]; then
	echo "check-gates-wired: FAIL — control: a gate named only outside" \
	     "'gates' was not reported, or a wired one was (the check went blind)" >&2
	exit 1
fi

echo "check-gates-wired: OK (all ${#gates[@]} one-owner gates run in make gates)"
exit 0
