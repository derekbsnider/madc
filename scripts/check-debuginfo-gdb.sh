#!/bin/bash
# check-debuginfo-gdb.sh — a -g AOT executable debugs in gdb.
#
# madc writes the DWARF of a -g image itself (MIR's debug builder), and its
# frames are described by the image's .eh_frame (B205: MIR writes CFI for
# every generated function). gcc 13 and clang 18 -g on x86-64 emit .eh_frame
# and no .debug_frame; gdb unwinds from .eh_frame. This gate drives gdb in
# batch over a three-deep call chain and holds what a debugging session needs:
#   1. the -g image carries .eh_frame and no .debug_frame (the canon shape);
#   2. a breakpoint by function name stops on the right source line;
#   3. the backtrace reaches main through every frame, arguments included;
#   4. locals read correctly in the stopped frame (after stepping) and in each
#      caller frame;
#   5. the same program built WITHOUT -g has no locals for gdb (the negative
#      control: the gate reads madc's DWARF, not something gdb guesses).
# Oracle: gcc -g -O0 on the same program prints the same lines.
set -u
cd "$(dirname "$0")/.." || exit 2
MADC=${MADC_BIN:-bin/madc}
if [ ! -x "$MADC" ]; then
	echo "check-debuginfo-gdb: $MADC not built" >&2
	exit 2
fi
if [ "$(uname -s)" != "Linux" ]; then
	echo "check-debuginfo-gdb: SKIP — the gate drives gdb over an ELF image (Linux hosts)"
	exit 0
fi
if ! command -v gdb > /dev/null || ! command -v readelf > /dev/null; then
	echo "check-debuginfo-gdb: SKIP — gdb or readelf is not installed on this host" >&2
	exit 0
fi
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/prog.c" <<'EOF'
#include <stdio.h>
static int leaf(int depth, int seed)
{
	int squared = seed * seed;
	int total = squared + depth;
	printf("leaf %d %d\n", depth, total);
	return total;
}
static int middle(int n)
{
	int doubled = n * 2;
	int r = leaf(n, doubled);
	return r + doubled;
}
int main(void)
{
	int start = 3;
	int result = middle(start);
	printf("result %d\n", result);
	return 0;
}
EOF
cat > "$tmp/cmds" <<'EOF'
set pagination off
set confirm off
break leaf
run
bt
next
next
info locals
up
info locals
up
info locals
continue
EOF
fail=0
need() { # need <output> <regex> <what>
	if ! grep -E -q "$2" <<< "$1"; then
		echo "check-debuginfo-gdb: $3 — expected /$2/" >&2
		fail=1
	fi
}
if ! "$MADC" -g -o "$tmp/prog_g" "$tmp/prog.c" > "$tmp/build.log" 2>&1; then
	echo "check-debuginfo-gdb: madc -g failed:" >&2
	cat "$tmp/build.log" >&2
	exit 1
fi
sections=$(readelf -SW "$tmp/prog_g")
need "$sections" '\.eh_frame ' "the -g image carries no .eh_frame"
if grep -q '\.debug_frame' <<< "$sections"; then
	echo "check-debuginfo-gdb: the -g image carries .debug_frame beside .eh_frame (gcc/clang -g: .eh_frame only)" >&2
	fail=1
fi
out=$(DEBUGINFOD_URLS= timeout 60 gdb -nx -batch -x "$tmp/cmds" "$tmp/prog_g" 2>&1)
need "$out" 'Breakpoint 1, leaf \(depth=3, seed=6\) at .*prog\.c:4' "the breakpoint does not stop in leaf at line 4 with its arguments"
need "$out" '#1 .* in middle \(n=3\) at .*prog\.c:12' "the backtrace does not reach middle"
need "$out" '#2 .* in main \(\) at .*prog\.c:18' "the backtrace does not reach main"
need "$out" 'squared = 36' "leaf's locals do not read after stepping"
need "$out" 'total = 39' "leaf's locals do not read after stepping"
need "$out" 'doubled = 6' "middle's locals do not read from its frame"
need "$out" 'start = 3' "main's locals do not read from its frame"
need "$out" 'result 45' "the program does not run to its end under gdb"
if [ "$fail" != 0 ]; then
	echo "--- gdb output:" >&2
	echo "$out" >&2
	exit 1
fi
if ! "$MADC" -o "$tmp/prog_nog" "$tmp/prog.c" > "$tmp/build.log" 2>&1; then
	echo "check-debuginfo-gdb: madc (no -g) failed:" >&2
	cat "$tmp/build.log" >&2
	exit 1
fi
out=$(DEBUGINFOD_URLS= timeout 60 gdb -nx -batch -x "$tmp/cmds" "$tmp/prog_nog" 2>&1)
if grep -q 'doubled = 6' <<< "$out"; then
	echo "check-debuginfo-gdb: NEGATIVE CONTROL FAILED — locals read from an image built without -g" >&2
	exit 1
fi
echo "check-debuginfo-gdb: OK — a -g image (.eh_frame, no .debug_frame) stops, backtraces and reads locals in gdb; without -g it has none"
