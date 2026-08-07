#!/bin/bash
# macho_obj_gate.sh — the MH_OBJECT `.o` gate (Mach-O axis B step 4,
# docs/plans/2026-07-25-macho-arm64-plan.md).
#
# The promise under test: `madc -c` for a Mach-O target writes a REAL
# relocatable object — one Apple's own linker accepts and links correctly,
# and one madc reads back so its own link lane produces the same image the
# from-source lane does.
#
# Two independent authorities, both on Linux:
#   * ld64.lld + the macOS SDK: an external linker that never saw MIR must
#     accept the object AND resolve its relocations to the right targets.
#     Running the result is the owner's Mac (every darwin leg's RUN gate).
#   * madc itself: `-c` then link must equal the direct `-o` emit byte for
#     byte apart from the code-signature identifier (the output basename) —
#     the capture is the same, so any difference is a read-back defect.
#
# Legs (per arch, arm64 + x86_64):
#   1  -c writes an MH_OBJECT llvm-otool parses, with the expected sections
#   2  ld64.lld links it against the SDK
#   3  the linked image's pool slots resolve to the right targets (a rebased
#      text address, a bss address, a bound import)
#   4  mixed link: a clang-compiled TU calls into the madc-compiled one
#   5  round trip: -c then madc-link == the direct emit (disassembly, pool)
#   6  two-TU merge through MIR_object_read: symbols unify across objects
#   7  -r merge: two .o -> one .o that BOTH linkers still accept, and whose
#      image matches the direct two-TU link
#   8  a global constructor's init entry rides __mod_init_func, and both
#      linkers keep the section (dyld runs the initializer from it)
#
# The cross madcs, ld64.lld-18 / llvm-*-18 and the macOS SDK are container
# artifacts, so the gate SKIPS (rc 0) when any is missing and says which.
# MACOS_SDK overrides the SDK path.
set -u
cd "$(dirname "$0")/.."
D=tmp/machoobjgate
SDK=${MACOS_SDK:-/workspace/sdk/MacOSX.sdk}
rc=0
pass() { echo "  ok   $1"; }
fail() { echo "  FAIL $1"; rc=1; }
skip() { echo "macho_obj_gate: SKIP ($1)"; exit 0; }

run() { ( ulimit -t 300; timeout 400 "$@" ); }

for t in ld64.lld-18 llvm-otool-18 llvm-nm-18 llvm-objdump-18 clang-18; do
	command -v "$t" >/dev/null 2>&1 || skip "$t not installed"
done
[ -d "$SDK" ] || skip "no macOS SDK at $SDK (set MACOS_SDK)"
for m in bin/madc-arm64-macos bin/madc-x86-64-macos; do
	[ -x "$m" ] || skip "$m not built (make -C src cross-arm64-macos cross-x86-64-macos)"
done

rm -rf "$D"
mkdir -p "$D"

# --- fixtures -------------------------------------------------------------
# rich.c exercises every relocation the capture can emit: a switch table
# (pool ABS64 against .text), string/array data (pool ABS64 against .data),
# a bss global (pool ABS64 against .bss), an import (pool ABS64 undefined),
# and text references into the pool (PC32 on x86-64, adrp/ldr+add page pairs
# on arm64, with and without an addend).
cat > "$D/rich.c" <<'EOF'
#include <stdio.h>
static const char *names[] = {"zero", "one", "two", "three"};
int counter;
static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
static int pick(int k)
{
	switch (k) {
	case 0: return 10;
	case 1: return 20;
	case 2: return 30;
	case 3: return 40;
	default: return -1;
	}
}
int compute(int k) { counter += pick(k); return counter; }
int main(void)
{
	printf("obj: %s %s %d fib(10)=%d\n", names[1], names[3],
	       compute(2) + compute(1), fib(10));
	return 28;
}
EOF

cat > "$D/parta.c" <<'EOF'
int shared_counter;
static const char tag[] = "A";
const char *a_tag(void) { return tag; }
int helper(int x) { shared_counter += x; return x * 3 + 1; }
EOF

cat > "$D/partb.c" <<'EOF'
#include <stdio.h>
extern int helper(int);
extern const char *a_tag(void);
extern int shared_counter;
int main(void)
{
	int r = helper(5) + helper(2);
	printf("two-TU: %s %d %d\n", a_tag(), r, shared_counter);
	return 0;
}
EOF

# A global constructor is the only way an object gets a __mod_init_func
# section (C file-scope initializers are constant by definition), so this is
# the fixture for that section's relocation and section TYPE.
cat > "$D/ctor.mad" <<'EOF'
#include <stdio.h>

class Counter
{
public:
	int n;
	Counter() { n = 7; }
};

Counter g;

int main()
{
	printf("ctor: %d\n", g.n);
	return 0;
}
EOF

cat > "$D/mlib.c" <<'EOF'
int madc_side(int x) { return x * 3 + 1; }
EOF

cat > "$D/mmain.c" <<'EOF'
#include <stdio.h>
extern int madc_side(int);
int clang_side(int x) { return x + 7; }
int main(void) { printf("mixed: %d %d\n", madc_side(5), clang_side(5)); return 0; }
EOF

# Link with Apple's linker; $1 = arch, $2 = output, rest = inputs.
ld64() {
	local arch=$1 out=$2
	shift 2
	run ld64.lld-18 -arch "$arch" -platform_version macos 12.0 12.0 \
		-syslibroot "$SDK" -lSystem -o "$out" "$@" >"$D/ld.log" 2>&1
}

for arch in arm64 x86_64; do
	case $arch in
	arm64) MADC=bin/madc-arm64-macos; TRIPLE=arm64-apple-macos12;;
	*) MADC=bin/madc-x86-64-macos; TRIPLE=x86_64-apple-macos12;;
	esac
	echo "macho_obj_gate [$arch]"
	P=$D/$arch
	mkdir -p "$P"

	# --- leg 1: -c writes a parseable MH_OBJECT with our sections
	run "$MADC" -c -o "$P/rich.o" "$D/rich.c" >"$P/c.log" 2>&1
	if [ $? -ne 0 ]; then
		fail "[1] -c failed: $(tail -1 "$P/c.log")"
		continue
	fi
	hdr=$(llvm-otool-18 -h "$P/rich.o" 2>&1)
	case "$hdr" in
	*error*) fail "[1] llvm-otool rejects the object: $hdr";;
	*) pass "[1] MH_OBJECT parses";;
	esac
	secs=$(llvm-otool-18 -l "$P/rich.o" 2>/dev/null | grep -c \
		-e "sectname __text" -e "sectname __mir_addrpool" -e "sectname __data")
	[ "$secs" = "3" ] && pass "[1] __text / __mir_addrpool / __data present" \
		|| fail "[1] expected 3 known sections, found $secs"

	# --- leg 2: Apple's linker accepts it
	if ld64 "$arch" "$P/prog.ld" "$P/rich.o"; then
		pass "[2] ld64.lld links the object"
	else
		fail "[2] ld64.lld refused: $(tail -2 "$D/ld.log" | tr '\n' ' ')"
	fi

	# --- leg 3: the pool slots ld64 wrote point at the right things
	if [ -f "$P/prog.ld" ]; then
		# each wanted target is a section's [addr, addr+size) range
		range() {
			llvm-otool-18 -l "$P/prog.ld" 2>/dev/null | awk -v s="sectname $1" '
				index($0, s) {f = 1}
				f && $1 == "addr" {a = $2}
				f && $1 == "size" {print a, $2; exit}'
		}
		pool=$(llvm-otool-18 -s __DATA_CONST __mir_addrpool "$P/prog.ld" 2>/dev/null)
		hits=0
		for sect in __text __bss; do
			set -- $(range "$sect")
			# llvm-otool dumps section bytes as 4-byte WORDS on arm64 and
			# single BYTES on x86-64 -- normalize both to a byte stream
			# before rebuilding the 8-byte slots.
			printf '%s' "$pool" | python3 -c '
import sys, re
want, size = int(sys.argv[1], 16), int(sys.argv[2], 16)
data = bytearray()
for t in sys.stdin.read().split():
    if re.fullmatch(r"[0-9a-f]{2}", t):
        data += bytes([int(t, 16)])
    elif re.fullmatch(r"[0-9a-f]{8}", t):
        data += int(t, 16).to_bytes(4, "little")
slots = [int.from_bytes(data[i:i + 8], "little") for i in range(0, len(data) - 7, 8)]
sys.exit(0 if any(want <= v < want + max(size, 1) for v in slots) else 1)
' "$1" "$2" && hits=$((hits + 1))
		done
		[ "$hits" = "2" ] && pass "[3] pool slots resolve into __text and __bss" \
			|| fail "[3] pool slots did not resolve ($hits/2 found)"
		binds=$(llvm-objdump-18 --macho --bind "$P/prog.ld" 2>/dev/null | grep -c _printf)
		[ "$binds" -ge 1 ] && pass "[3] the import slot became a dyld bind" \
			|| fail "[3] no bind entry for _printf"
	fi

	# --- leg 4: mixed link with a clang TU, both directions of the call
	run "$MADC" -c -o "$P/mlib.o" "$D/mlib.c" >/dev/null 2>&1
	clang-18 --target="$TRIPLE" -isysroot "$SDK" -O1 -c -o "$P/mmain.o" \
		"$D/mmain.c" >/dev/null 2>&1
	if ld64 "$arch" "$P/mixed" "$P/mmain.o" "$P/mlib.o"; then
		if llvm-nm-18 "$P/mixed" 2>/dev/null | grep -q "_madc_side"; then
			pass "[4] clang TU links against the madc object"
		else
			fail "[4] mixed image lacks _madc_side"
		fi
	else
		fail "[4] mixed link refused: $(tail -2 "$D/ld.log" | tr '\n' ' ')"
	fi

	# --- leg 5: read-back round trip == the direct emit
	run "$MADC" -o "$P/from_o" "$P/rich.o" >"$P/link.log" 2>&1
	if [ $? -ne 0 ]; then
		fail "[5] madc link of its own .o failed: $(tail -1 "$P/link.log")"
	else
		run "$MADC" -o "$P/direct" "$D/rich.c" >/dev/null 2>&1
		llvm-objdump-18 --macho -d --no-show-raw-insn "$P/from_o" 2>/dev/null \
			| tail -n +2 > "$P/d1"
		llvm-objdump-18 --macho -d --no-show-raw-insn "$P/direct" 2>/dev/null \
			| tail -n +2 > "$P/d2"
		if cmp -s "$P/d1" "$P/d2"; then
			pass "[5] .o path disassembles identically to the direct emit"
		else
			fail "[5] disassembly differs: $(diff "$P/d1" "$P/d2" | head -4 | tr '\n' ' ')"
		fi
		llvm-otool-18 -s __DATA_CONST __mir_addrpool "$P/from_o" 2>/dev/null \
			| tail -n +2 > "$P/p1"
		llvm-otool-18 -s __DATA_CONST __mir_addrpool "$P/direct" 2>/dev/null \
			| tail -n +2 > "$P/p2"
		cmp -s "$P/p1" "$P/p2" && pass "[5] pool contents identical" \
			|| fail "[5] pool contents differ"
	fi

	# --- leg 6: two-TU merge through the reader
	run "$MADC" -c -o "$P/parta.o" "$D/parta.c" >/dev/null 2>&1
	run "$MADC" -c -o "$P/partb.o" "$D/partb.c" >/dev/null 2>&1
	run "$MADC" -o "$P/two" "$P/parta.o" "$P/partb.o" >"$P/two.log" 2>&1
	if [ $? -ne 0 ]; then
		fail "[6] two-TU link failed: $(tail -1 "$P/two.log")"
	else
		syms=$(llvm-nm-18 "$P/two" 2>/dev/null)
		miss=""
		for s in _helper _a_tag _main _shared_counter; do
			case "$syms" in *"$s"*) ;; *) miss="$miss $s";; esac
		done
		[ -z "$miss" ] && pass "[6] cross-object symbols unified" \
			|| fail "[6] merged image missing:$miss"
	fi

	# --- leg 7: -r merge stays linkable by both linkers, same image
	run "$MADC" -r -o "$P/merged.o" "$P/parta.o" "$P/partb.o" >"$P/r.log" 2>&1
	if [ $? -ne 0 ]; then
		fail "[7] -r merge failed: $(tail -1 "$P/r.log")"
	else
		run "$MADC" -o "$P/m1" "$P/merged.o" >/dev/null 2>&1 \
			&& pass "[7] madc links the merged .o" \
			|| fail "[7] madc refused the merged .o"
		ld64 "$arch" "$P/m2" "$P/merged.o" \
			&& pass "[7] ld64.lld links the merged .o" \
			|| fail "[7] ld64.lld refused the merged .o: $(tail -2 "$D/ld.log" | tr '\n' ' ')"
		if [ -f "$P/m1" ] && [ -f "$P/two" ]; then
			llvm-objdump-18 --macho -d --no-show-raw-insn "$P/m1" 2>/dev/null \
				| tail -n +2 > "$P/r1"
			llvm-objdump-18 --macho -d --no-show-raw-insn "$P/two" 2>/dev/null \
				| tail -n +2 > "$P/r2"
			cmp -s "$P/r1" "$P/r2" \
				&& pass "[7] merged-.o image == direct two-TU link" \
				|| fail "[7] merged-.o image differs from the direct link"
		fi
	fi

	# --- leg 8: a global ctor's init entry rides __mod_init_func, and BOTH
	# linkers keep it (ld64 has to accept S_MOD_INIT_FUNC_POINTERS from us,
	# or dyld would never run the initializer)
	if run "$MADC" -c -o "$P/ctor.o" "$D/ctor.mad" >"$P/ctor.log" 2>&1; then
		if llvm-otool-18 -l "$P/ctor.o" 2>/dev/null \
			| grep -q "sectname __mod_init_func"; then
			pass "[8] the ctor's init entry lands in __mod_init_func"
		else
			fail "[8] no __mod_init_func section in the object"
		fi
		run "$MADC" -o "$P/ctorprog" "$P/ctor.o" >/dev/null 2>&1 \
			&& pass "[8] madc links the ctor object" \
			|| fail "[8] madc refused the ctor object"
		if ld64 "$arch" "$P/ctorprog.ld" "$P/ctor.o"; then
			llvm-otool-18 -l "$P/ctorprog.ld" 2>/dev/null \
				| grep -q "sectname __mod_init_func" \
				&& pass "[8] ld64 keeps __mod_init_func in the image" \
				|| fail "[8] ld64 dropped __mod_init_func"
		else
			fail "[8] ld64 refused the ctor object: $(tail -2 "$D/ld.log" | tr '\n' ' ')"
		fi
	else
		fail "[8] -c of a ctor TU failed: $(tail -1 "$P/ctor.log")"
	fi
done

[ $rc -eq 0 ] && echo "macho_obj_gate: OK" || echo "macho_obj_gate: FAILURES"
exit $rc
