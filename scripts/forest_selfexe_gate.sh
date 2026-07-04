#!/usr/bin/env bash
# B3 gate: the appended-blob placement. A COPY of the madc binary with a
# frozen forest appended must find its own blob (readlink /proc/self/exe ->
# mmap -> EOF footer), thaw it in that fresh process, compile, and run —
# producing the same output the source test pins in its .expect fixture.
#
# Run from the repo root (fulltest does). Uses tests/testfreezerun.{mad,expect}
# as the payload so the oracle stays in ONE place.
set -u
cd "$(dirname "$0")/.."

ulimit -t 300 2>/dev/null

SRC=tests/testfreezerun.mad
EXP=tests/testfreezerun.expect
BIN=tmp/forest_selfexe_madc

if [ ! -f bin/madc ] || [ ! -f "$SRC" ] || [ ! -f "$EXP" ]; then
    echo "forest_selfexe_gate: missing bin/madc or $SRC/$EXP"
    exit 1
fi

mkdir -p tmp
rm -f "$BIN"
cp bin/madc "$BIN"

if ! timeout 120 bin/madc --freeze-append="$BIN" "$SRC" >/dev/null 2>&1; then
    echo "forest_selfexe_gate: --freeze-append FAILED"
    rm -f "$BIN"
    exit 1
fi

out=$(timeout 60 "$BIN" --run-frozen 2>/dev/null)
rc=$?
rm -f "$BIN"

if [ $rc -ne 0 ]; then
    echo "forest_selfexe_gate: --run-frozen FAILED (rc=$rc)"
    exit 1
fi

fail=0
while IFS= read -r line; do
    [ -z "$line" ] && continue
    case "$out" in
	*"$line"*) ;;
	*) echo "forest_selfexe_gate: output missing expected line: $line"
	   fail=1 ;;
    esac
done < "$EXP"

if [ $fail -ne 0 ]; then
    echo "forest_selfexe_gate: FAILED"
    exit 1
fi

echo "forest_selfexe_gate: GREEN — appended forest ran from /proc/self/exe"
exit 0
