#!/bin/bash
# smaug_gate.sh — the REAL SMAUG boots under this madc (owner directive
# 2026-08-28: the mini SMAUG-shaped suite tests soak reduced constructs,
# not the artifact — SMAUG itself compiled+booted for months while broken
# in every archived release because no lane compiled the real tree).
#
# The gate: madc builds the actual MadSMAUG --project (51 C89 TUs via
# compile_commands.json, the canonical MadSMAUG.sh invocation), links, and
# JIT-boots it until the server's "ready at" line appears — then kills it.
# Two-sided: it FAILS on any madc diagnostic (SMAUG's build is warning-free
# at gcc-default parity) and on a boot that never reaches ready.
#
# MadSMAUG is a separate repository (Diku/Merc/SMAUG license stack); the
# gate SKIPS — loudly — when the checkout is absent (fresh clones, CI
# without the sibling repo). The build container carries it at
# /workspace/MadSMAUG (the repo-root MadSMAUG symlink).
set -u
cd "$(dirname "$0")/.."

BIN="${MADC_BIN:-bin/madc}"
SMAUG="${MADC_SMAUG_DIR:-MadSMAUG}"
PORT="${MADC_SMAUG_PORT:-46100}"
LOG=tmp/smaug_gate.log

if [ ! -f "$SMAUG/compile_commands.json" ]; then
	echo "smaug_gate: SKIP — no MadSMAUG checkout at '$SMAUG'" \
	     "(set MADC_SMAUG_DIR; the gate only runs beside the sibling repo)"
	exit 0
fi
if [ ! -x "$BIN" ]; then
	echo "smaug_gate: $BIN missing — build first" >&2
	exit 1
fi

SMAUG_ABS=$(cd "$SMAUG" && pwd -P)
MADC_ABS=$(cd "$(dirname "$BIN")" && pwd -P)/$(basename "$BIN")

# The runtime data tree, exactly MadSMAUG.sh's layout (SMAUG opens
# ../system/... relative to a cwd of <data>/area). Reuse an existing tree;
# build one beside the gate's tmp/ when absent.
DATA="$SMAUG_ABS/runtime"
if [ ! -d "$DATA/area" ] || [ ! -d "$DATA/system" ]; then
	DATA="$(pwd -P)/tmp/smaug_gate_runtime"
	if [ ! -d "$DATA/area" ] || [ ! -d "$DATA/system" ]; then
		echo "smaug_gate: building runtime data tree at $DATA"
		mkdir -p "$DATA"
		rm -rf "$DATA/area" "$DATA/system"
		cp -rL "$SMAUG_ABS/upstream/smaug1.8/area"   "$DATA/area"
		cp -rL "$SMAUG_ABS/upstream/smaug1.8/system" "$DATA/system"
		for d in boards building clans classes corpses councils deity \
			 gods houses log new player races vault backup deleted doc; do
			[ -d "$SMAUG_ABS/upstream/smaug1.8/$d" ] \
				&& ln -sfn "$SMAUG_ABS/upstream/smaug1.8/$d" "$DATA/$d"
		done
	fi
fi

mkdir -p tmp
: > "$LOG"

# Boot in the background; poll for the ready line; kill either way. The
# wall cap covers a hung parse (the suite discipline: no un-capped runs).
(
	cd "$DATA/area"
	ulimit -t 300
	exec timeout 240 "$MADC_ABS" -lcrypt \
		"$SMAUG_ABS/compile_commands.json" smaug "$PORT"
) > "$LOG" 2>&1 &
PID=$!

READY=0
for _ in $(seq 1 120); do
	if grep -aq "ready at" "$LOG"; then READY=1; break; fi
	kill -0 "$PID" 2>/dev/null || break
	sleep 2
done
kill "$PID" 2>/dev/null
wait "$PID" 2>/dev/null

WARNS=$(grep -ac "warning" "$LOG" || true)
ERRS=$(grep -ac "error\|SIGSEGV\|cir_compile failed" "$LOG" || true)

if [ "$READY" -ne 1 ]; then
	echo "smaug_gate: FAIL — SMAUG never reached its ready line ($LOG)" >&2
	tail -12 "$LOG" >&2
	exit 1
fi
if [ "$WARNS" -ne 0 ] || [ "$ERRS" -ne 0 ]; then
	echo "smaug_gate: FAIL — $WARNS warning(s), $ERRS error line(s) on the" \
	     "SMAUG build (bar: zero — gcc-default parity; $LOG)" >&2
	grep -a "warning\|error" "$LOG" | head -12 >&2
	exit 1
fi
echo "smaug_gate: OK — the real SMAUG (51-TU --project) compiled, linked," \
     "and booted to ready under $BIN with zero diagnostics"
