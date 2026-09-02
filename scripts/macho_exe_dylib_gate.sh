#!/bin/bash
# macho_exe_dylib_gate.sh — the Mach-O executable DYLIB-BINDING gate
# (darwin AOT C++ slice, macos-release-lane plan session #81).
#
# The promise under test: MIR_object_emit_executable's Mach-O writer loads
# the dylibs the emit lane names (params->needed, full install names) and
# binds imports so dyld can resolve them there:
#
#   * NO extras (a pure-C program): exactly one LC_LOAD_DYLIB (libSystem)
#     and every bind attributed to libSystem two-level — the pre-slice
#     shape, structurally identical to every earlier image. This leg is
#     the negative control: if it ever grows a libc++ LC or a flat bind,
#     the trigger leaked.
#   * WITH extras (a C++ program under -static-libmadc): LC_LOAD_DYLIB
#     /usr/lib/libc++.1.dylib rides beside libSystem and every import
#     binds with the flat-namespace special ordinal (a header-less Mac
#     has no .tbd stubs to attribute symbols per-dylib), while the madc
#     runtime is ledger-merged IN-image: defined in the symtab, absent
#     from the bind table.
#
# Execution is the Mac battery's job (leg 6d — dyld actually resolving
# the flat binds); this gate proves the STRUCTURE on the container with
# llvm-otool / llvm-objdump, the independent authorities.
#
# Container artifacts required — SKIP (rc 0) when missing, macho_obj_gate
# precedent: the cross madc (make -C src cross-arm64-macos), llvm-18
# tools, and a darwin forest container for the ledger leg
# (obj/hosted-arm64-macos/forest.bin, produced by `make -C src
# release-macos`; the forest-less cross madc has no AOT ledger of its
# own, so the gate hands it the hosted container via --forest-bind).
#
# Knobs (env; defaults = the container's versioned apt spellings, so the
# fulltest posture is unchanged): OTOOL / OBJDUMP / NM name the Mach-O
# readers (a darwin host: $(brew --prefix llvm@18)/bin/llvm-otool etc.),
# MADC the emitting madc, FOREST the darwin forest container. A SKIP names
# the knob that would lift it — never silent about WHICH tool was missing.
set -u
cd "$(dirname "$0")/.."
D=tmp/machoexedylibgate
rc=0
pass() { echo "  ok   $1"; }
fail() { echo "  FAIL $1"; rc=1; }
skip() { echo "macho_exe_dylib_gate: SKIP ($1)"; exit 0; }

run() { ( ulimit -t 300; timeout 400 "$@" ); }

OTOOL="${OTOOL:-llvm-otool-18}"
OBJDUMP="${OBJDUMP:-llvm-objdump-18}"
NM="${NM:-llvm-nm-18}"
for pair in "OTOOL=$OTOOL" "OBJDUMP=$OBJDUMP" "NM=$NM"; do
	command -v "${pair#*=}" >/dev/null 2>&1 || skip "${pair#*=} not installed (knob ${pair%%=*})"
done
MADC="${MADC:-bin/madc-arm64-macos}"
[ -x "$MADC" ] || skip "$MADC not built (make -C src cross-arm64-macos; knob MADC)"
FOREST="${FOREST:-obj/hosted-arm64-macos/forest.bin}"
[ -f "$FOREST" ] || skip "$FOREST not built (make -C src release-macos; knob FOREST)"

rm -rf "$D"
mkdir -p "$D"

printf '#include <stdio.h>\nint main(void) { printf("c ok\\n"); return 0; }\n' > "$D/cprog.c"
cat > "$D/xprog.mad" <<'EOF'
#include <iostream>
int main()
{
	std::cout << "otry ok" << std::endl;
	return 0;
}
EOF

load_dylibs() { "$OTOOL" -l "$1" 2>/dev/null | grep -A2 "LC_LOAD_DYLIB" | grep -c "name "; }
binds() { "$OBJDUMP" --macho --bind "$1" 2>/dev/null; }

# --- leg A: pure C, no extras — the two-level negative control ------------
if run "$MADC" -o "$D/cprog" "$D/cprog.c" >"$D/c.log" 2>&1; then
	n=$(load_dylibs "$D/cprog")
	[ "$n" = "1" ] && pass "[A] one LC_LOAD_DYLIB (libSystem) only" \
		|| fail "[A] expected 1 load dylib, found $n — the extras trigger leaked into a pure-C image"
	b="$(binds "$D/cprog")"
	echo "$b" | grep -q "libSystem  *_printf" \
		&& pass "[A] _printf binds two-level against libSystem" \
		|| fail "[A] _printf is not a two-level libSystem bind"
	fl=$(echo "$b" | grep -c "flat-namespace")
	[ "$fl" = "0" ] && pass "[A] zero flat-namespace binds" \
		|| fail "[A] $fl flat-namespace bind(s) in a no-extras image"
else
	fail "[A] pure-C -o emit failed: $(tail -1 "$D/c.log")"
fi

# --- leg B: C++ under -static-libmadc — libc++ loaded, imports flat -------
if run "$MADC" --forest-bind="$FOREST" -static-libmadc -o "$D/xprog" \
		"$D/xprog.mad" >"$D/x.log" 2>&1; then
	lcs=$("$OTOOL" -l "$D/xprog" 2>/dev/null | grep -A2 "LC_LOAD_DYLIB" | grep "name ")
	echo "$lcs" | grep -q "/usr/lib/libc++.1.dylib" \
		&& pass "[B] LC_LOAD_DYLIB /usr/lib/libc++.1.dylib present" \
		|| fail "[B] no libc++ load command: $lcs"
	b="$(binds "$D/xprog")"
	echo "$b" | grep "flat-namespace" | grep -q "__ZNSt" \
		&& pass "[B] mangled C++ imports bind flat-namespace (the probe-3 symbol class)" \
		|| fail "[B] no flat-namespace bind for a mangled C++ import"
	ls=$(echo "$b" | grep -c "libSystem ")
	[ "$ls" = "0" ] && pass "[B] no two-level binds remain beside the flat set" \
		|| fail "[B] $ls libSystem-attributed bind(s) mixed into a flat image"
	mb=$(echo "$b" | grep -c "__madc_")
	[ "$mb" = "0" ] && pass "[B] zero __madc_ imports (ledger merged in-image)" \
		|| fail "[B] $mb __madc_ bind(s) — the ledger merge left runtime imports"
	"$NM" "$D/xprog" 2>/dev/null | grep -q " T ___madc_try_push" \
		&& pass "[B] ___madc_try_push defined in the image symtab" \
		|| fail "[B] ___madc_try_push not defined in the image"
else
	fail "[B] C++ -static-libmadc -o emit failed: $(tail -2 "$D/x.log" | tr '\n' ' ')"
fi

[ $rc -eq 0 ] && echo "macho_exe_dylib_gate: OK" || echo "macho_exe_dylib_gate: FAILURES"
exit $rc
