#!/usr/bin/env bash
# check-policy-fanout.sh — CLI registration-policy fanout gate
# (DupFamily cli_registration_policy_fanout, dupaudit 2026-08-23).
#
# RULE: a registration-policy field written on prog->registration_policy in
# src/madc.cpp must ALSO be written on engine.registration_policy. Per-TU
# --project Programs inherit ONLY the engine copy (MadcEngine::
# configure_program, parser.cpp) — a prog-only write is a CLI flag that
# silently never reaches project translation units.
# --no-embedded-headers was live-broken exactly this way.
#
# EXCLUDED fields (documented prog-only state):
#   enable_forest_bind — forest-bind state is resolved per-invocation and
#                        handed to the madc_project_* entry points explicitly.
#
# Usage: check-policy-fanout.sh [file]
# (default src/madc.cpp; the argument exists so the negative control can run
#  the gate against a historical copy: git show <sha>:src/madc.cpp > f; $0 f)
set -u
cd "$(dirname "$0")/.." || exit 2
FILE="${1:-src/madc.cpp}"
EXCLUDE="enable_forest_bind"
if [ ! -r "$FILE" ]; then
	echo "policy-fanout: cannot read $FILE" >&2
	exit 2
fi
rc=0
prog_fields=$(grep -oE 'prog->registration_policy\.[a-z_]+[[:space:]]*=' "$FILE" \
	| sed -E 's/.*\.([a-z_]+)[[:space:]]*=/\1/' | sort -u)
for f in $prog_fields; do
	skip=0
	for e in $EXCLUDE; do
		if [ "$f" = "$e" ]; then
			skip=1
		fi
	done
	if [ "$skip" -eq 1 ]; then
		continue
	fi
	if ! grep -qE "engine\.registration_policy\.$f[[:space:]]*=" "$FILE"; then
		echo "POLICY-FANOUT: prog->registration_policy.$f is written in $FILE with NO engine.registration_policy.$f companion -- --project TUs will never see it"
		rc=1
	fi
done
if [ "$rc" -eq 0 ]; then
	echo "policy-fanout: all CLI registration-policy fields ride the engine (excluded: $EXCLUDE)"
fi
exit $rc
