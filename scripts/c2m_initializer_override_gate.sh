#!/bin/bash
# BEHAVIOUR GATE -- the in-tree c2mir driver (c2m) honours C11 6.7.9p19: a
# later initializer overrides any earlier one for the same subobject, in
# static and automatic storage. madc never hands c2mir an overriding list
# (InitializerCursor places every clause first), so its test suite cannot see
# this; c2m compiles C as written and is the self-hosting compiler.
#
# c2mir's collect_init_els recorded every clause; the static data writer then
# kept the FIRST element at an offset, no brace list re-initialized the member
# it named, and the local writer moved its end offset back over a patched
# string character and zero-filled the rest (BUGS.md B138). override_init_els
# now drops what a later initializer overrides.
#
# The reducer is tests/testinitoverride.mad (C, its .expect is the gcc 13 /
# clang 18 output); madc runs it as an ordinary test, this gate runs it
# through c2m. Run from the repo root (fulltest does).
set -u
cd "$(dirname "$0")/.."

ulimit -t 120 2>/dev/null

C2M="${C2M:-obj/mir/host/c2m}"
SRC=tests/testinitoverride.mad
EXPECT=tests/testinitoverride.expect

if [ ! -x "$C2M" ]; then
	echo "c2m_initializer_override_gate: $C2M missing -- build first" >&2
	exit 1
fi
if [ "$(grep -c . "$EXPECT")" -lt 3 ]; then
	echo "c2m_initializer_override_gate: $EXPECT holds no oracle lines" >&2
	exit 1
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cp "$SRC" "$tmp/override.c"
timeout 60 "$C2M" "$tmp/override.c" -eg > "$tmp/out" 2>&1
rc=$?
if [ "$rc" -ne 0 ] || ! cmp -s "$tmp/out" "$EXPECT"; then
	echo "c2m_initializer_override_gate: RED -- c2m (rc=$rc) differs from the gcc/clang oracle:"
	diff "$EXPECT" "$tmp/out"
	exit 1
fi
echo "GREEN -- c2m: a later initializer overrides an earlier one for the same subobject."
