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
#   * the program linked against it records the install name.
#
# Execution (dyld loading it, its initializers running) is the Mac
# battery's job; this gate proves the STRUCTURE on the container, both
# arches, with llvm-otool / llvm-objdump / ld64.lld as the authorities.
#
# Container artifacts required — SKIP (rc 0) when missing, the
# macho_obj_gate precedent: the cross madcs (make -C src cross-arm64-macos
# cross-x86-64-macos), llvm-18 tools, clang-18 + lld and the macOS SDK.
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

[ $rc -eq 0 ] && echo "macho_dylib_gate: OK" || echo "macho_dylib_gate: FAILURES"
exit $rc
