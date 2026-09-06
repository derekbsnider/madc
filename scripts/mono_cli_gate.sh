#!/bin/bash
# mono_cli_gate.sh — the monolithic CLI form stays alive under the shared
# default (packaging arc PK2, docs/plans/2026-09-01-packaging-arc.md
# "Lane shape"). fulltest builds bin/madc-mono (the same madc.o
# whole-archived against the static libmadc — the recipe bin/madc used
# when monolithic was the default); this gate proves the optional form
# both LINKS and RUNS: no libmadc.so dependency, and a real program
# executes. It is the monolithic form's whole standing coverage — the
# full battery runs the default (packaged) shape only.
# Negative control: the shape assertion must FAIL on bin/madc-thin,
# which by construction depends on libmadc.so — a gate that cannot fail
# is not a gate.
set -u
cd "$(dirname "$0")/.."
say() { echo "mono_cli_gate: $*"; }

MONO=bin/madc-mono
THIN=bin/madc-thin

shape_is_static() { ! ldd "$1" 2>/dev/null | grep -q 'libmadc'; }

[ -x "$MONO" ] || { say "no $MONO — fulltest builds it"; exit 1; }
[ -x "$THIN" ] || { say "no $THIN — fulltest builds it (forest_library_gate's vehicle)"; exit 1; }

if shape_is_static "$THIN"; then
	say "NEGATIVE CONTROL FAILED — the shape assertion passed a libmadc.so-linked binary (the gate cannot fail)"
	exit 1
fi

if ! shape_is_static "$MONO"; then
	say "FAIL: $MONO depends on libmadc.so — the monolithic link regressed"
	exit 1
fi

mkdir -p tmp
printf 'println("mono-cli-alive");\n' > tmp/mono_cli_gate.mad
OUT=$( ( ulimit -t 30; timeout 60 "$MONO" tmp/mono_cli_gate.mad ) 2>&1 )
if [ "$OUT" != "mono-cli-alive" ]; then
	say "FAIL: monolithic CLI run produced: $OUT"
	exit 1
fi

say "OK — the monolithic form links static and runs (negative control bit on the thin CLI)"
