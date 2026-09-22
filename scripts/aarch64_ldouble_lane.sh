#!/bin/bash
# aarch64_ldouble_lane.sh — long double on madc's aarch64-linux axis, under qemu.
#
# WHY THIS LANE EXISTS. aarch64-linux is a madc build target (`make -C src
# cross-aarch64-linux`, the emit-only cross compiler) that no lane ever ran, and
# its long double was broken without anyone seeing it: the MIR generator
# lowered every LD operation to a call to a C helper under a DOTTED name,
# "mir.ldadd", that nothing on this target exports, so any madc-compiled program
# doing long double arithmetic failed to LINK (18 undefined mir.* references).
# The builtins now call libgcc's soft-float routines by their real names, as
# gcc's own objects do. This lane is what keeps that true.
#
# THREE LEGS, one fixture (tests/cross/aarch64_ldouble.c, every operand built at
# run time):
#   oracle  aarch64-linux-gnu-gcc's binary under qemu — the expected output.
#   aot     bin/madc-aarch64-linux -c, linked by aarch64 gcc, run under qemu:
#           must import NO dotted mir.* name, and must print the oracle's bytes.
#   jit     a gcc-built aarch64 c2m (MIR's own driver) running the fixture
#           through the generator (-eg) under qemu — the libmir JIT path.
#
# CONTROL (two-sided): the oracle must print the binary128-only value
# `i2ld 9007199254740993.0` (2^53+1 is not a double) AND must not print the
# binary64 rounding of it — so a qemu, libgcc or probe that silently degraded to
# double cannot turn this lane vacuously green.
set -u
cd "$(dirname "$0")/.."

FIX=tests/cross/aarch64_ldouble.c
CC=aarch64-linux-gnu-gcc
NM=aarch64-linux-gnu-nm
QEMU="qemu-aarch64-static -L /usr/aarch64-linux-gnu"
MADC=bin/madc-aarch64-linux
JITB="$PWD/obj/mir/aarch64-native"
D=$(mktemp -d "$PWD/tmp/aarch64ld.XXXXXX")
fail=0

for t in "$CC" "$NM" qemu-aarch64-static; do
	command -v "$t" >/dev/null 2>&1 || {
		echo "aarch64_ldouble_lane: $t is missing (scripts/provision_container.sh)"; exit 2; }
done
[ -x "$MADC" ] || { echo "aarch64_ldouble_lane: $MADC missing — make -C src cross-aarch64-linux"; exit 2; }

# --- oracle + control -------------------------------------------------------
"$CC" -O0 "$FIX" -o "$D/gcc" && $QEMU "$D/gcc" > "$D/oracle" \
	|| { echo "aarch64_ldouble_lane: the gcc oracle did not build or run"; exit 1; }
if ! grep -qx 'i2ld 9007199254740993.0' "$D/oracle" \
   || grep -qx 'i2ld 9007199254740992.0' "$D/oracle"; then
	echo "aarch64_ldouble_lane: CONTROL FAILED — the oracle did not compute in binary128:"
	sed 's/^/    /' "$D/oracle"
	exit 1
fi
echo "control: oracle computes binary128 ($(wc -l < "$D/oracle") lines)"

# --- aot leg ----------------------------------------------------------------
if ! "$MADC" --std=c17 -c -o "$D/madc.o" "$FIX" > "$D/aot.log" 2>&1; then
	echo "RED  aot: madc -c failed"; sed 's/^/    /' "$D/aot.log" | head -5; fail=1
else
	dotted=$("$NM" -u "$D/madc.o" | grep -c ' mir\.' || true)
	if [ "$dotted" -ne 0 ]; then
		echo "RED  aot: the object imports $dotted dotted mir.* builtin(s) that nothing exports:"
		"$NM" -u "$D/madc.o" | grep ' mir\.' | sed 's/^/    /'; fail=1
	elif ! "$CC" "$D/madc.o" -o "$D/madc" 2> "$D/link.err"; then
		echo "RED  aot: link failed"; sed 's/^/    /' "$D/link.err" | head -5; fail=1
	elif ! $QEMU "$D/madc" > "$D/aot" 2>&1 || ! diff -q "$D/oracle" "$D/aot" >/dev/null; then
		echo "RED  aot: output differs from the gcc oracle"; diff "$D/oracle" "$D/aot" | head -12; fail=1
	else
		echo "GREEN aot: 0 dotted imports, links, byte-identical to gcc"
	fi
fi

# --- jit leg ----------------------------------------------------------------
mkdir -p "$JITB"
if ! make -C third_party/mir BUILD_DIR="$JITB" CC="$CC" "$JITB/c2m" > "$D/c2m.log" 2>&1; then
	echo "RED  jit: the aarch64 c2m did not build"; tail -5 "$D/c2m.log"; fail=1
elif ! $QEMU "$JITB/c2m" "$FIX" -eg > "$D/jit" 2> "$D/jit.err" || ! diff -q "$D/oracle" "$D/jit" >/dev/null; then
	echo "RED  jit: output differs from the gcc oracle"; head -3 "$D/jit.err"; diff "$D/oracle" "$D/jit" | head -12; fail=1
else
	echo "GREEN jit: byte-identical to gcc"
fi

[ "$fail" -eq 0 ] && echo "aarch64_ldouble_lane: GREEN (oracle, aot, jit)" && rm -rf "$D"
exit "$fail"
