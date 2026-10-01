#!/bin/bash
# macho_dylib_gate.sh — the Mach-O MH_DYLIB EMISSION gate (`madc -shared` on
# the Apple targets; plan §41.11a step 4, ROADMAP 6.5).
#
# The promise under test: MIR_object_emit_executable with shared_p writes a
# dylib a real Mach-O linker accepts —
#
#   * MH_DYLIB, no LC_MAIN / LC_LOAD_DYLINKER, and an LC_ID_DYLIB naming
#     "@rpath/" + the output's basename;
#   * an export trie holding every defined global and no local, which
#     ld64.lld (an independent reader) walks to resolve a program's
#     references — `lib_f`, `lib_foo` and `lib_foobar` are prefixes of one
#     another, the shape a flat trie mis-walks;
#   * the program linked against it records the install name;
#   * the madc runtime (D5): libmadc-0.dylib names itself
#     @rpath/libmadc-0.dylib; a runtime-needing program and a runtime-needing
#     library (a plugin's shape) load it by that name, carry LC_RPATHs
#     starting @executable_path/../lib (dyld's token, though the emitting
#     madc runs on Linux), and every madc-runtime bind they make is in its
#     export trie; the runtime-free library above (the negative control)
#     loads no libmadc and carries no LC_RPATH.
#
# Execution (dyld loading it, its initializers running) is the Mac
# battery's job; this gate proves the STRUCTURE on the container, both
# arches, with llvm-otool / llvm-objdump / ld64.lld as the authorities.
#
# Container artifacts required — SKIP (rc 0) when missing, the
# macho_obj_gate precedent: the cross madcs (make -C src cross-arm64-macos
# cross-x86-64-macos), llvm-18 tools, clang-18 + lld and the macOS SDK. The
# runtime legs also need each arch's obj/hosted-<arch>-macos/libmadc-0.dylib
# (make -C src hosted-<arch>-macos, or release-macos) and say SKIP for an
# arch that lacks it.
# Knobs (env): OTOOL / OBJDUMP name the readers, CLANG the linking driver,
# MACOS_SDK the SDK. A SKIP names the knob that would lift it.
set -u
cd "$(dirname "$0")/.."
D=tmp/machodylibgate
rc=0
pass() { echo "  ok   $1"; }
fail() { echo "  FAIL $1"; rc=1; }
skip() { echo "macho_dylib_gate: SKIP ($1)"; exit 0; }

run() { ( ulimit -t 300; timeout 400 "$@" ); }

OTOOL="${OTOOL:-llvm-otool-18}"
OBJDUMP="${OBJDUMP:-llvm-objdump-18}"
CLANG="${CLANG:-clang-18}"
MACOS_SDK="${MACOS_SDK:-/workspace/sdk/MacOSX.sdk}"
for pair in "OTOOL=$OTOOL" "OBJDUMP=$OBJDUMP" "CLANG=$CLANG"; do
	command -v "${pair#*=}" >/dev/null 2>&1 || skip "${pair#*=} not installed (knob ${pair%%=*})"
done
[ -d "$MACOS_SDK" ] || skip "$MACOS_SDK missing (knob MACOS_SDK)"
for a in arm64 x86-64; do
	[ -x "bin/madc-$a-macos" ] || skip "bin/madc-$a-macos not built (make -C src cross-$a-macos)"
done

rm -rf "$D"
mkdir -p "$D"
cat > "$D/lib.c" <<'EOF'
int lib_counter = 7;
static int lib_hidden = 3;
int lib_answer(int x) { return 40 + x + lib_hidden - 3; }
int lib_foo(void) { return 1; }
int lib_foobar(void) { return 2; }
int lib_f(void) { return lib_counter; }
EOF
cat > "$D/main.c" <<'EOF'
#include <stdio.h>
int lib_answer(int x);
int lib_foo(void);
int lib_foobar(void);
int lib_f(void);
extern int lib_counter;
int main(void) { printf("%d %d %d %d %d\n", lib_answer(2), lib_foo(), lib_foobar(), lib_f(), lib_counter); return 0; }
EOF

for a in arm64 x86-64; do
	tgt=arm64-apple-macos12
	[ "$a" = x86-64 ] && tgt=x86_64-apple-macos12
	lib="$D/$a/libx.dylib"
	mkdir -p "$D/$a"
	# A runtime-free C library: no eval shims (they import the value
	# runtime, which has no darwin library until D5).
	if ! run "bin/madc-$a-macos" --std=c17 -fno-eval-shims -shared -o "$lib" "$D/lib.c" \
			>"$D/$a/emit.log" 2>&1; then
		fail "[$a] -shared emit failed: $(tail -1 "$D/$a/emit.log")"
		continue
	fi
	"$OTOOL" -hv "$lib" 2>/dev/null | grep -q " DYLIB " \
		&& pass "[$a] MH_DYLIB" || fail "[$a] the header is not MH_DYLIB"
	lcs="$("$OTOOL" -l "$lib" 2>/dev/null)"
	echo "$lcs" | grep -q "name @rpath/libx.dylib " \
		&& pass "[$a] LC_ID_DYLIB @rpath/libx.dylib" || fail "[$a] no LC_ID_DYLIB @rpath/libx.dylib"
	n=$(echo "$lcs" | grep -cE "cmd LC_(MAIN|LOAD_DYLINKER)$")
	[ "$n" = "0" ] && pass "[$a] no LC_MAIN / LC_LOAD_DYLINKER" \
		|| fail "[$a] $n executable-only load command(s) in a dylib"
	ex="$("$OBJDUMP" --macho --exports-trie "$lib" 2>/dev/null)"
	got=$(echo "$ex" | grep -oE "_lib_[a-z]+$" | sort | paste -sd' ')
	[ "$got" = "_lib_answer _lib_counter _lib_f _lib_foo _lib_foobar" ] \
		&& pass "[$a] the export trie holds the five globals, no local" \
		|| fail "[$a] export trie: '$got'"
	if run "$CLANG" -target "$tgt" --sysroot "$MACOS_SDK" -fuse-ld=lld -o "$D/$a/main" \
			"$D/main.c" "$lib" >"$D/$a/link.log" 2>&1; then
		pass "[$a] ld64.lld links a program against it (every name through the trie)"
		"$OTOOL" -L "$D/$a/main" 2>/dev/null | grep -q "@rpath/libx.dylib" \
			&& pass "[$a] the program records @rpath/libx.dylib" \
			|| fail "[$a] the program does not record the install name"
	else
		fail "[$a] ld64.lld refused the dylib: $(grep -m1 error "$D/$a/link.log")"
	fi
done

cat > "$D/rt.mad" <<'EOF'
int main()
{
	var x = { "a": 1 };
	println("hi {} {}", 42, x["a"]);
	return 0;
}
EOF
cat > "$D/rtlib.mad" <<'EOF'
extern "C" const char *rt_greet(const char *who)
{
	return format("hello, {}", who);
}
EOF
# A madc-runtime import: the runtime's C surface or its C++ namespace.
rt_names() { grep -E '^_(_madc|madc|madarray|__madc|_ZN4madc)'; }
binds_of() { "$OBJDUMP" --macho --bind "$1" 2>/dev/null | awk 'NR>3{print $NF}' | sort -u; }

for a in arm64 x86-64; do
	dy="obj/hosted-$a-macos/libmadc-0.dylib"
	if [ ! -f "$dy" ]; then
		echo "  SKIP [$a] runtime legs: $dy not built (make -C src hosted-$a-macos)"
		continue
	fi
	"$OTOOL" -l "$dy" 2>/dev/null | grep -q "name @rpath/libmadc-0.dylib " \
		&& pass "[$a] libmadc-0.dylib names itself @rpath/libmadc-0.dylib" \
		|| fail "[$a] libmadc-0.dylib has no LC_ID_DYLIB @rpath/libmadc-0.dylib"
	"$OBJDUMP" --macho --exports-trie "$dy" 2>/dev/null | awk 'NR>2{print $2}' | sort -u > "$D/$a/rtexports.txt"
	for k in exe lib; do
		img="$D/$a/rt-$k"
		if [ $k = exe ]; then
			run "bin/madc-$a-macos" -o "$img" "$D/rt.mad" >"$D/$a/rt-$k.log" 2>&1
		else
			run "bin/madc-$a-macos" -shared -o "$img" "$D/rtlib.mad" >"$D/$a/rt-$k.log" 2>&1
		fi
		if [ $? -ne 0 ]; then
			fail "[$a] runtime $k emit failed: $(tail -1 "$D/$a/rt-$k.log")"
			continue
		fi
		lcs="$("$OTOOL" -l "$img" 2>/dev/null)"
		echo "$lcs" | grep -q "name @rpath/libmadc-0.dylib " \
			&& pass "[$a] the runtime $k loads @rpath/libmadc-0.dylib" \
			|| fail "[$a] the runtime $k does not load @rpath/libmadc-0.dylib"
		first=$(echo "$lcs" | grep -A2 -E "cmd LC_RPATH$" | grep -m1 " path " | awk '{print $2}')
		[ "$first" = "@executable_path/../lib" ] \
			&& pass "[$a] the runtime $k's first LC_RPATH is @executable_path/../lib" \
			|| fail "[$a] the runtime $k's first LC_RPATH is '$first'"
		binds_of "$img" | rt_names > "$D/$a/rt-$k-binds.txt"
		nb=$(wc -l < "$D/$a/rt-$k-binds.txt")
		miss=$(comm -23 "$D/$a/rt-$k-binds.txt" "$D/$a/rtexports.txt" | paste -sd' ')
		if [ "$nb" -gt 0 ] && [ -z "$miss" ]; then
			pass "[$a] the runtime $k's $nb madc-runtime binds are all libmadc-0.dylib exports"
		else
			fail "[$a] the runtime $k: $nb runtime binds, not exported: '$miss'"
		fi
	done
	nl=$("$OTOOL" -l "$D/$a/libx.dylib" 2>/dev/null | grep -cE "libmadc-0.dylib |cmd LC_RPATH$")
	[ "$nl" = "0" ] && pass "[$a] the runtime-free library loads no libmadc and carries no LC_RPATH" \
		|| fail "[$a] the runtime-free library carries $nl libmadc / LC_RPATH load command(s)"
done

[ $rc -eq 0 ] && echo "macho_dylib_gate: OK" || echo "macho_dylib_gate: FAILURES"
exit $rc
