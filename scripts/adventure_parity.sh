#!/bin/bash
# adventure_parity.sh — Track 7 Phase 2 gate G-A (fragment stage, slice A5;
# plan docs/plans/2026-08-20-adventure-430-plan.md): every fragment under
# examples/adventure/tests/fragments/ replays BYTE-IDENTICALLY through the
# madc game. <name>.input feeds `bin/madc examples/adventure/advent.mad`;
# stdout must equal <name>.oracle exactly (cmp).
#
# Oracle bytes come from the reference implementation (open-adventure's
# advent binary / its .chk corpus), cut at the A5 skeleton's EOF contract:
# through the final command's response plus the read loop's trailing blank
# line — the reference then appends its terminate/score tail, which lands
# with slice A9 (fixtures regenerate to full length there, and A10 grows
# this gate into the whole-log corpus).
#
# Negative control: a deliberately corrupted expectation must FAIL, or the
# compare is not biting.
set -u
cd "$(dirname "$0")/.." || exit 9
MADC=${MADC_BIN:-bin/madc}
FRAG=examples/adventure/tests/fragments
GAME=examples/adventure/advent.mad
TMP=tmp/advent_parity.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

fail=0
ran=0
for input in "$FRAG"/*.input; do
    [ -e "$input" ] || continue
    name=$(basename "$input" .input)
    oracle="$FRAG/$name.oracle"
    if [ ! -f "$oracle" ]; then
	echo "adventure_parity: $name has no .oracle fixture"
	fail=1
	continue
    fi
    ( ulimit -t 60; timeout 60 "$MADC" "$GAME" < "$input" ) \
	> "$TMP/$name.out" 2> "$TMP/$name.err"
    if ! cmp -s "$TMP/$name.out" "$oracle"; then
	echo "adventure_parity: FRAGMENT $name DIVERGES:"
	cmp "$TMP/$name.out" "$oracle" 2>&1 | head -2
	diff "$TMP/$name.out" "$oracle" 2>&1 | head -8
	sed -e 's/^/  stderr: /' "$TMP/$name.err" | head -4
	fail=1
    fi
    ran=$((ran + 1))
done
if [ "$ran" -eq 0 ]; then
    echo "adventure_parity: no fragments found (fixture dir missing?)"
    exit 1
fi

# Negative control: append a byte to the first oracle — must be caught.
first=$(ls "$FRAG"/*.oracle | head -1)
name=$(basename "$first" .oracle)
cat "$first" > "$TMP/corrupt.oracle"
printf 'X' >> "$TMP/corrupt.oracle"
( ulimit -t 60; timeout 60 "$MADC" "$GAME" < "$FRAG/$name.input" ) \
    > "$TMP/nc.out" 2> /dev/null
if cmp -s "$TMP/nc.out" "$TMP/corrupt.oracle"; then
    echo "adventure_parity: NEGATIVE CONTROL FAILED (corrupt oracle passed)"
    fail=1
fi

if [ "$fail" -ne 0 ]; then
    echo "adventure_parity: FAIL"
    exit 1
fi
echo "adventure_parity: $ran fragment(s) byte-identical (+ negative control)"
exit 0
