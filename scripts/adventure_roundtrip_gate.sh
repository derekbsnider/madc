#!/bin/bash
# adventure_roundtrip_gate.sh — Track 7 Phase 1 gate G2: a mid-game save,
# reloaded in a FRESH process, replays into a transcript byte-identical to
# the uninterrupted run — identity outlives representation (design demand
# 4; plan docs/plans/2026-08-20-track7-phase1-text-adventure.md).
# Also: a reloaded world saved again is byte-identical to the first save
# (the format's fixed point), and — the negative control — a corrupted
# save must refuse to load loudly.
set -u
cd "$(dirname "$0")/.." || exit 9
MADC=${MADC_BIN:-bin/madc}
WORLD=tests/adventure.world
DRIVER=tests/testadventure.mad
TMP=tmp/adventure_rt.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

PART1='take lantern
inventory
go east
take key
go west
go north
go down
open door
go down
light lantern'
PART2='look
turns
inspect brass-door
quit'

run() { # world cmdfile outfile
    ( ulimit -t 60; timeout 60 "$MADC" "$DRIVER" "$1" < "$2" ) \
	> "$3" 2> "$3.err"
}

# A: the uninterrupted run.
printf '%s\n%s\n' "$PART1" "$PART2" > "$TMP/all.cmd"
run "$WORLD" "$TMP/all.cmd" "$TMP/a.txt"
rc=$?
if [ $rc -ne 0 ]; then
    echo "adventure_roundtrip_gate: FAIL (uninterrupted run rc=$rc)"
    exit 1
fi
# Blind-harness guard: an empty A comparing equal to an empty B is not a
# pass. The run must have actually reached the clock.
if ! grep -qF 'Turn 9.' "$TMP/a.txt"; then
    echo "adventure_roundtrip_gate: FAIL (run A never reached Turn 9 — harness blind?)"
    exit 1
fi

# B1: part one, then save + quit.
printf '%s\nsave %s/save.world\nquit\n' "$PART1" "$TMP" > "$TMP/b1.cmd"
run "$WORLD" "$TMP/b1.cmd" "$TMP/b1.txt"
rc=$?
if [ $rc -ne 0 ] || [ ! -s "$TMP/save.world" ]; then
    echo "adventure_roundtrip_gate: FAIL (save leg rc=$rc)"
    exit 1
fi

# B2: a fresh process on the save, part two only.
printf '%s\n' "$PART2" > "$TMP/b2.cmd"
run "$TMP/save.world" "$TMP/b2.cmd" "$TMP/b2.txt"
rc=$?
if [ $rc -ne 0 ]; then
    echo "adventure_roundtrip_gate: FAIL (reload leg rc=$rc)"
    exit 1
fi

# Stitch: drop B1's save/quit tail ("> save ...", "Saved.", "> quit") and
# B2's initial auto-look (everything before its first prompt line); the
# seam must vanish.
head -n -3 "$TMP/b1.txt" > "$TMP/combined.txt"
awk '/^> /{f=1} f{print}' "$TMP/b2.txt" >> "$TMP/combined.txt"
if ! cmp -s "$TMP/a.txt" "$TMP/combined.txt"; then
    echo "adventure_roundtrip_gate: FAIL (stitched transcript differs)"
    diff "$TMP/a.txt" "$TMP/combined.txt" | head -20
    exit 1
fi

# The save is a fixed point: load it, save again, byte-identical.
printf 'save %s/save2.world\nquit\n' "$TMP" > "$TMP/b3.cmd"
run "$TMP/save.world" "$TMP/b3.cmd" "$TMP/b3.txt"
if ! cmp -s "$TMP/save.world" "$TMP/save2.world"; then
    echo "adventure_roundtrip_gate: FAIL (resave differs from save)"
    exit 1
fi

# Negative control: a corrupted save must refuse to load, loudly.
sed 's/%link/%bogus/' "$TMP/save.world" > "$TMP/bad.world"
printf 'quit\n' > "$TMP/nc.cmd"
run "$TMP/bad.world" "$TMP/nc.cmd" "$TMP/nc.txt"
rc=$?
if [ $rc -eq 0 ]; then
    echo "adventure_roundtrip_gate: NEGATIVE CONTROL FAILED (corrupt save loaded)"
    exit 1
fi
if ! grep -q 'world_open' "$TMP/nc.txt.err"; then
    echo "adventure_roundtrip_gate: NEGATIVE CONTROL FAILED (refusal was silent)"
    exit 1
fi

echo "adventure_roundtrip_gate: OK (transcript stitched clean; save is a fixed point; corrupt save refused)"
