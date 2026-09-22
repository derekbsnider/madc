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
# TWO FIXTURES: tests/cross/aarch64_ldouble.c (every builtin, every operand built
# at run time) and tests/cross/aarch64_ldouble_const.c (every route a CONSTANT's
# bytes take to the target — folded, literal, static scalar/array/member,
# _Complex — which the x86-hosted cross compiler used to write in the host's x87
# layout; aarch64 read them as binary128 near zero). THREE LEGS each:
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
CONSTFIX=tests/cross/aarch64_ldouble_const.c
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

# --- oracle + control (on the runtime fixture) ------------------------------
"$CC" -O0 "$FIX" -o "$D/gcc" && $QEMU "$D/gcc" > "$D/oracle" \
	|| { echo "aarch64_ldouble_lane: the gcc oracle did not build or run"; exit 1; }
if ! grep -qx 'i2ld 9007199254740993.0' "$D/oracle" \
   || grep -qx 'i2ld 9007199254740992.0' "$D/oracle"; then
	echo "aarch64_ldouble_lane: CONTROL FAILED — the oracle did not compute in binary128:"
	sed 's/^/    /' "$D/oracle"
	exit 1
fi
echo "control: oracle computes binary128 ($(wc -l < "$D/oracle") lines)"

mkdir -p "$JITB"
if ! make -C third_party/mir BUILD_DIR="$JITB" CC="$CC" "$JITB/c2m" > "$D/c2m.log" 2>&1; then
	echo "RED  jit: the aarch64 c2m did not build"; tail -5 "$D/c2m.log"; fail=1
fi

for fx in "$FIX" "$CONSTFIX"; do
	n=$(basename "$fx" .c)
	"$CC" -O0 "$fx" -o "$D/$n.gcc" && $QEMU "$D/$n.gcc" > "$D/$n.oracle" \
		|| { echo "RED  $n: the gcc oracle did not build or run"; fail=1; continue; }
	# aot: madc-aarch64-linux -c, linked by aarch64 gcc
	if ! "$MADC" --std=c17 -c -o "$D/$n.o" "$fx" > "$D/$n.aot.log" 2>&1; then
		echo "RED  $n aot: madc -c failed"; sed 's/^/    /' "$D/$n.aot.log" | head -5; fail=1
	else
		dotted=$("$NM" -u "$D/$n.o" | grep -c ' mir\.' || true)
		if [ "$dotted" -ne 0 ]; then
			echo "RED  $n aot: the object imports $dotted dotted mir.* builtin(s) that nothing exports:"
			"$NM" -u "$D/$n.o" | grep ' mir\.' | sed 's/^/    /'; fail=1
		elif ! "$CC" "$D/$n.o" -o "$D/$n.madc" 2> "$D/$n.link.err"; then
			echo "RED  $n aot: link failed"; sed 's/^/    /' "$D/$n.link.err" | head -5; fail=1
		elif ! $QEMU "$D/$n.madc" > "$D/$n.aot" 2>&1 || ! diff -q "$D/$n.oracle" "$D/$n.aot" >/dev/null; then
			echo "RED  $n aot: output differs from the gcc oracle"; diff "$D/$n.oracle" "$D/$n.aot" | head -12; fail=1
		else
			echo "GREEN $n aot: 0 dotted imports, links, byte-identical to gcc"
		fi
	fi
	# jit: a gcc-built aarch64 c2m through the generator (-eg)
	if [ -x "$JITB/c2m" ]; then
		if ! $QEMU "$JITB/c2m" "$fx" -eg > "$D/$n.jit" 2> "$D/$n.jit.err" \
		   || ! diff -q "$D/$n.oracle" "$D/$n.jit" >/dev/null; then
			echo "RED  $n jit: output differs from the gcc oracle"; head -3 "$D/$n.jit.err"
			diff "$D/$n.oracle" "$D/$n.jit" | head -12; fail=1
		else
			echo "GREEN $n jit: byte-identical to gcc"
		fi
	fi
done

[ "$fail" -eq 0 ] && echo "aarch64_ldouble_lane: GREEN (control; aot + jit on both fixtures)" && rm -rf "$D"
exit "$fail"
