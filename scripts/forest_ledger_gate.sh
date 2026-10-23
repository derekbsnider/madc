#!/bin/bash
# forest_ledger_gate.sh — the AOT ledger / -static-libmadc gate
# (forest-carriers S5, docs/plans/2026-07-25-forest-carriers-plan.md).
#
# The promise under test: a C-lane program that needs madc's runtime
# machinery (try/catch, VLA scope exit) emits with -static-libmadc and runs
# with NO madc library present — because the runtime rode in on the AOT
# ledger this madc's forest container carries.
#
# The dev tree's bin/madc is not packed (the pack happens at release), so the
# gate freezes its OWN container, ledger and all, and points the compiles at
# it with --forest-bind=. That exercises exactly the production path: the same
# --freeze-ledger= the release pack uses, read back through the same
# discovery chain.
#
# Legs:
#   1  freeze a container WITH the ledger; --dump-forest reports it, and the
#      startup members' native objects beside its modules
#   2  baseline — the same program WITHOUT the flag keeps libmadc.so.0
#      (so leg 3 is proving the flag, not an accident)
#   3  try/catch: -static-libmadc drops the dependency, no __madc_* imports
#      remain, and the binary's output matches the JIT run
#   4  the emitted binary runs with an EMPTY library path (no madc anywhere)
#   5  VLA scope exit (the second ledger module) — baseline + same contract
#   6  Tier-B refusal: a script-lane program (madc `array` + php::, whose
#      helpers exist only as host C++ objects) refuses, NAMING symbols
#   6b the other side — a plain <iostream>/<string> program IS fully coverable:
#      it emits, keeps no libmadc dependency, and matches its JIT output
#   6c the same program WITHOUT the flag: its destructors' unwind personality
#      rides the image like libgcc's, so it needs no libmadc, and its -c
#      object links with plain g++
#   6d a g++ throw crossing a madc object's frame runs the frame's destructor
#      (the object's own personality copy; oracle: g++ alone)
#   7  no-ledger carrier: a container packed without --freeze-ledger= refuses
#      with the build-side message, never the Tier-B one
#   7b the startup object (B231): a -c merges the ledger's pre-generated
#      personality object, a no-ledger carrier generates it in-process, and
#      either .o links with plain g++ and runs leg 6d's destructor
#   8  -static-libmadc -c refuses at the CLI (the runtime merges at the link)
#   9  the .o LINK lane merges the ledger: two -c objects (one needing
#      try/catch, one needing VLA scope exit) link runtime-free, produce the
#      same output as the libmadc-linked baseline, and run on a clean machine.
#      Also the boundary: objects that kept their __madc_shim_* host-call
#      adapters refuse (value ABI = Tier B) naming -fno-eval-shims, and -r
#      still refuses, so the runtime rides the final link only
#  10  an image names only the libraries its imports reach, gcc's list
#      exactly (oracle: gcc --as-needed): plain C = libc alone, a sqrt call
#      adds libm, the .o link lane agrees, and a C++ image keeps its C++
#      runtime
set -u
cd "$(dirname "$0")/.."
MADC=${MADC_BIN:-bin/madc}
D=tmp/ledgergate
LEDGER_LIST=scripts/ledger_sources.txt
rc=0
pass() { echo "  ok   $1"; }
fail() { echo "  FAIL $1"; rc=1; }

rm -rf "$D"
mkdir -p "$D"

run() { ( ulimit -t 300; timeout 400 "$@" ); }

# --- fixtures -------------------------------------------------------------
cat > "$D/tu.mad" <<'EOF'
#include <stdio.h>
int main(void) { return 0; }
EOF

cat > "$D/trycatch.mad" <<'EOF'
#include <stdio.h>

int risky(int n)
{
	if ( n > 3 )
		throw "too big";
	return n * 2;
}

int main(void)
{
	int i;
	for ( i = 1; i < 6; i++ )
	{
		try {
			printf("risky(%d) = %d\n", i, risky(i));
		} catch (const char *e) {
			printf("caught: %s\n", e);
		}
	}
	return 0;
}
EOF

cat > "$D/vla.mad" <<'EOF'
#include <stdio.h>

static int total(int n)
{
	int a[n];
	int i, s = 0;
	for ( i = 0; i < n; i++ )
		a[i] = i * i;
	for ( i = 0; i < n; i++ )
		s += a[i];
	return s;
}

int main(void)
{
	printf("total(%d) = %d\n", 7, total(7));
	return 0;
}
EOF

# The .o link-lane pair (leg 9): main needs try/catch, the helper needs VLA
# scope exit — the two ledger modules, reached from two separate objects.
cat > "$D/linkmain.mad" <<'EOF'
#include <stdio.h>

int vlasum(int n);

static int risky(int n)
{
	if ( n > 3 )
		throw "too big";
	return n * 2;
}

int main(void)
{
	int i;
	for ( i = 1; i < 6; i++ )
	{
		try {
			printf("risky(%d) = %d\n", i, risky(i));
		} catch (const char *e) {
			printf("caught: %s\n", e);
		}
	}
	printf("vlasum(7) = %d\n", vlasum(7));
	return 0;
}
EOF

cat > "$D/linkhelp.mad" <<'EOF'
int vlasum(int n)
{
	int a[n];
	int i, s = 0;
	for ( i = 0; i < n; i++ )
		a[i] = i * i;
	for ( i = 0; i < n; i++ )
		s += a[i];
	return s;
}
EOF

# Leg 6 needs a program that genuinely needs the C++ SCRIPT-LANE runtime — the
# thing the Tier-B message names. The madc `array` (madc::value) + php:: lane is
# exactly that: its helpers live only as host-toolchain C++ objects
# (madarray_construct/destruct, __php_array_*), so no ledger can cover them.
#
# This used to be an <iostream>/<string> program, which refused only by
# accident: madc sized an EMPTY struct 0 bytes, so passing an empty
# iterator-category tag BY VALUE (input_iterator_tag, the tag-dispatch idiom)
# became a zero-length block argument, and MIR's x86-64 block-arg path emits
# `mir.arg_memcpy` for any size that is not 1..16. That MIR internal — not any
# madc runtime symbol — was the single "uncovered" symbol. Once empty
# aggregates got their [class]/4 size of 1 the copy became a register move, the
# program became fully coverable, and the leg failed. Leg 6b now locks that
# capability in so it cannot silently regress.
cat > "$D/tierb.mad" <<'EOF'
#include <iostream>
#include <string>
#include <ns_php>
using namespace std;
int main()
{
	array a;
	php::array_push(a, "alpha");
	cout << a[0] << endl;
	return 0;
}
EOF

cat > "$D/cpp.mad" <<'EOF'
#include <iostream>
#include <string>
int main() { std::string s = "hi"; std::cout << s << std::endl; return 0; }
EOF

if ! LEDGER_SOURCES=$(bash scripts/select_ledger_sources.sh linux "$LEDGER_LIST"); then
	echo "forest_ledger_gate: could not select ledger sources" >&2
	exit 1
fi
LEDGER_ARGS=()
while IFS= read -r src; do
	[ -n "$src" ] || continue
	LEDGER_ARGS+=("--freeze-ledger=$src")
done <<<"$LEDGER_SOURCES"
if [ ${#LEDGER_ARGS[@]} -eq 0 ]; then
	echo "forest_ledger_gate: $LEDGER_LIST selects no Linux sources"
	exit 1
fi
LEDGER_MODULE_COUNT=${#LEDGER_ARGS[@]}

# --- leg 1: freeze a container carrying the ledger ------------------------
if run "$MADC" "${LEDGER_ARGS[@]}" --freeze-append="$D/madc.forest" "$D/tu.mad" \
	> "$D/freeze.out" 2> "$D/freeze.err"; then
	pass "freeze with ledger"
else
	fail "freeze with ledger (rc=$?)"
	sed -n '1,20p' "$D/freeze.err"
	exit $rc
fi
dump=$(run "$MADC" --dump-forest="$D/madc.forest" 2>/dev/null | tr -d '\0')
LEDGER_SUMMARY_PREFIX=$'ledger\tmodules='"$LEDGER_MODULE_COUNT"$'\tsymbols='
if grep -Fq "$LEDGER_SUMMARY_PREFIX" <<<"$dump"; then
	pass "--dump-forest reports the ledger"
else
	fail "--dump-forest reports the ledger"
	grep -a '^ledger' <<<"$dump" | head -3
fi
for src in "${LEDGER_ARGS[@]}"; do
	LEDGER_MODULE_PREFIX=$'ledgermod\t'"${src#--freeze-ledger=}"$'\t'
	if grep -Fq "$LEDGER_MODULE_PREFIX" <<<"$dump"; then
		pass "ledger carries ${src#--freeze-ledger=}"
	else
		fail "ledger carries ${src#--freeze-ledger=}"
	fi
done
if grep -q $'^crtobj\tcrt:' <<<"$dump"; then
	pass "ledger carries the startup objects"
else
	fail "ledger carries the startup objects"
fi

# --- shared helper: emit + inspect ----------------------------------------
# $1 = source, $2 = out name, $3.. = extra madc flags. Sets NEEDED/UND/OUT.
# UND counts the STRONG __madc_* imports: a weak reference pulls no archive
# member and is null when nothing defines it (ld; oracle: gcc -static-libgcc
# images keep crtbegin's weak _ITM_* / __gmon_start__ undefined) — the unwind
# personality's view of the throw side in a program that never throws.
emit_and_inspect()
{
	local src=$1 out=$2; shift 2
	run "$MADC" --forest-bind="$D/madc.forest" "$@" -o "$D/$out" "$src" \
		> "$D/$out.emit" 2>&1
	EMIT_RC=$?
	NEEDED=$(readelf -d "$D/$out" 2>/dev/null | grep -ao 'libmadc[^]]*')
	UND=$(readelf --dyn-syms -W "$D/$out" 2>/dev/null \
	      | awk '$7=="UND" && $5!="WEAK"{print $8}' | grep -a '^__madc_' | sort -u)
}

# --- leg 2: baseline — no flag keeps the dependency -----------------------
emit_and_inspect "$D/trycatch.mad" base
if [ $EMIT_RC -eq 0 ] && [ -n "$NEEDED" ]; then
	pass "baseline keeps libmadc.so.0 (the flag is what changes it)"
else
	fail "baseline keeps libmadc.so.0 (rc=$EMIT_RC needed='$NEEDED')"
fi

# --- leg 3: try/catch, -static-libmadc ------------------------------------
emit_and_inspect "$D/trycatch.mad" tc -static-libmadc
if [ $EMIT_RC -ne 0 ]; then
	fail "try/catch -static-libmadc emit (rc=$EMIT_RC)"
	sed -n '1,15p' "$D/tc.emit"
elif [ -n "$NEEDED" ]; then
	fail "try/catch image still needs '$NEEDED'"
elif [ -n "$UND" ]; then
	fail "try/catch image still imports: $(tr '\n' ' ' <<<"$UND")"
else
	pass "try/catch -static-libmadc: no madc library, no __madc_* imports"
fi
jit=$(run "$MADC" "$D/trycatch.mad" 2>/dev/null)
aot=$(run "$D/tc" 2>/dev/null)
if [ -n "$aot" ] && [ "$jit" = "$aot" ]; then
	pass "try/catch output matches the JIT run"
else
	fail "try/catch output matches the JIT run"
	diff <(echo "$jit") <(echo "$aot") | head -6
fi

# --- leg 4: run it with no madc library reachable at all ------------------
clean=$(env -u LD_LIBRARY_PATH -i LD_LIBRARY_PATH=/nonexistent \
	timeout 60 "$PWD/$D/tc" 2>&1)
if [ "$clean" = "$jit" ]; then
	pass "runs with an empty library path (clean machine)"
else
	fail "runs with an empty library path (clean machine)"
	echo "$clean" | head -4
fi

# --- leg 5: VLA scope exit (the second ledger module) ---------------------
# Baseline first, for the same reason as leg 2: a VLA program that did not
# need the runtime would satisfy the -static-libmadc assertions vacuously.
emit_and_inspect "$D/vla.mad" vlabase
if [ $EMIT_RC -eq 0 ] && [ -n "$NEEDED" ]; then
	pass "VLA baseline is genuinely runtime-needing"
else
	fail "VLA baseline is genuinely runtime-needing (rc=$EMIT_RC needed='$NEEDED')"
fi
emit_and_inspect "$D/vla.mad" vla -static-libmadc
vjit=$(run "$MADC" "$D/vla.mad" 2>/dev/null)
vaot=$(run "$D/vla" 2>/dev/null)
if [ $EMIT_RC -eq 0 ] && [ -z "$NEEDED" ] && [ -z "$UND" ] \
   && [ -n "$vaot" ] && [ "$vjit" = "$vaot" ]; then
	pass "VLA -static-libmadc: runtime-free and output-identical"
else
	fail "VLA -static-libmadc (rc=$EMIT_RC needed='$NEEDED' und='$UND')"
	sed -n '1,10p' "$D/vla.emit"
fi

# --- leg 5b: two ledger modules sharing a static-inline header pair --------
# A madc-mode println/format program pulls rt_format AND rt_dump together;
# both TUs carry rt_dump.h's static-inline helpers. With C internal linkage
# dropped (the pre-fix front-end bug) the second module died at MIR load with
# "func madc_dump_frame_col is prohibited for redefinition". The pair loading,
# linking runtime-free, and running proves function statics stay TU-local
# through the ledger (FuncDef::internal_linkage -> N_STATIC -> no export).
cat > "$D/fmtpair.mad" <<'EOF'
int main() { println("pair {}", 7); return 0; }
EOF
emit_and_inspect "$D/fmtpair.mad" fmtpair -static-libmadc
fjit=$(run "$MADC" "$D/fmtpair.mad" 2>/dev/null)
faot=$(run "$D/fmtpair" 2>/dev/null)
if grep -q "prohibited for redefinition" "$D/fmtpair.emit"; then
	fail "format+dump ledger pair: static-inline redefinition is back"
	sed -n '1,6p' "$D/fmtpair.emit"
elif [ $EMIT_RC -eq 0 ] && [ -z "$NEEDED" ] && [ -z "$UND" ] \
   && [ -n "$faot" ] && [ "$fjit" = "$faot" ]; then
	pass "format+dump ledger pair: statics stay module-local"
else
	fail "format+dump ledger pair (rc=$EMIT_RC needed='$NEEDED' und='$UND')"
	sed -n '1,10p' "$D/fmtpair.emit"
fi

# --- leg 6: Tier-B refusal names the symbols ------------------------------
# The JIT run first: a program that did not actually work would refuse for
# reasons that have nothing to do with the ledger (the same
# prove-the-baseline discipline legs 2/5/9 use).
tierb_jit=$(run "$MADC" "$D/tierb.mad" 2>/dev/null)
if [ "$tierb_jit" = "alpha" ]; then
	pass "Tier-B fixture is a working program"
else
	fail "Tier-B fixture is a working program (got '$tierb_jit')"
fi
run "$MADC" --forest-bind="$D/madc.forest" -static-libmadc -o "$D/tierb" \
	"$D/tierb.mad" > "$D/tierb.log" 2>&1
tierb_rc=$?
tierb=$(tr -d '\0' < "$D/tierb.log")
if [ $tierb_rc -eq 0 ]; then
	fail "Tier-B program refuses"
elif ! grep -q 'not on the AOT ledger (Tier B' <<<"$tierb"; then
	fail "Tier-B refusal uses the Tier-B message"
	sed -n '1,6p' "$D/tierb.log"
elif ! grep -qE '^    [A-Za-z_]' <<<"$tierb"; then
	fail "Tier-B refusal lists the offending symbols"
elif ! grep -q 'madarray_\|__php_' <<<"$tierb"; then
	fail "Tier-B refusal names the script-lane runtime symbols"
	sed -n '1,6p' "$D/tierb.log"
else
	pass "Tier-B program refuses loudly, naming symbols"
fi

# --- leg 6b: a plain C++ program IS coverable ------------------------------
# The other side of the same contract. Nothing in <iostream>/<string> needs the
# script-lane runtime, so -static-libmadc must cover it completely: no libmadc
# dependency, no __madc_* imports, and the same output as the JIT run. This
# became true when empty aggregates got their C++ size of 1 (see the fixture
# comment above); asserting it keeps the regression that would undo it visible.
cpp_jit=$(run "$MADC" "$D/cpp.mad" 2>/dev/null)
emit_and_inspect "$D/cpp.mad" cpp -static-libmadc
cpp_aot=$(run "$D/cpp" 2>/dev/null)
if [ $EMIT_RC -ne 0 ]; then
	fail "plain C++ program emits under -static-libmadc (rc=$EMIT_RC)"
	sed -n '1,10p' "$D/cpp.emit"
elif [ -n "$NEEDED" ]; then
	fail "plain C++ -static-libmadc image still needs '$NEEDED'"
elif [ -n "$UND" ]; then
	fail "plain C++ -static-libmadc image still imports: $(tr '\n' ' ' <<<"$UND")"
elif [ -z "$cpp_aot" ] || [ "$cpp_jit" != "$cpp_aot" ]; then
	fail "plain C++ -static-libmadc output matches the JIT run"
	diff <(echo "$cpp_jit") <(echo "$cpp_aot") | head -6
else
	pass "plain C++ program is fully ledger-coverable"
fi

# --- leg 6c: the same program WITHOUT the flag needs no madc library -------
# A C++ frame with a destructor names madc's unwind personality (B205), the
# role __gxx_personality_v0 plays for g++. It rides every image and object
# the way libgcc's personality does, so a program whose only runtime need is
# its destructors' unwinding keeps no libmadc dependency and its -c object
# links with plain g++ (B229: 0.106.0 shipped needing both).
emit_and_inspect "$D/cpp.mad" cppdyn
# The personality's references into the exception runtime are weak: null in
# a program that never throws (ld's undefined weak), so only a STRONG
# __madc_* import is a dependency.
UND=$(readelf --dyn-syms -W "$D/cppdyn" 2>/dev/null \
      | awk '$7=="UND" && $5!="WEAK"{print $8}' | grep -a '^__madc_' | sort -u)
cppdyn_aot=$(run "$D/cppdyn" 2>/dev/null)
run "$MADC" --forest-bind="$D/madc.forest" -c -o "$D/cppdyn.o" "$D/cpp.mad" \
	> "$D/cppdyn_o.emit" 2>&1
obj_rc=$?
g++ -o "$D/cppdyn_gxx" "$D/cppdyn.o" > "$D/cppdyn_gxx.link" 2>&1
gxx_rc=$?
cppdyn_gxx=$(run "$D/cppdyn_gxx" 2>/dev/null)
if [ $EMIT_RC -ne 0 ]; then
	fail "plain C++ program emits without -static-libmadc (rc=$EMIT_RC)"
	sed -n '1,10p' "$D/cppdyn.emit"
elif [ -n "$NEEDED" ]; then
	fail "plain C++ image without -static-libmadc needs '$NEEDED'"
elif [ -n "$UND" ]; then
	fail "plain C++ image without -static-libmadc imports: $(tr '\n' ' ' <<<"$UND")"
elif [ -z "$cppdyn_aot" ] || [ "$cppdyn_aot" != "$cpp_jit" ]; then
	fail "plain C++ image output matches the JIT run"
elif [ $obj_rc -ne 0 ] || [ $gxx_rc -ne 0 ]; then
	fail "plain C++ -c object links with plain g++ (madc rc=$obj_rc, g++ rc=$gxx_rc)"
	sed -n '1,6p' "$D/cppdyn_o.emit" "$D/cppdyn_gxx.link"
elif [ "$cppdyn_gxx" != "$cpp_jit" ]; then
	fail "plain C++ -c object linked by g++ matches the JIT run"
else
	pass "plain C++ program needs no madc library: image and g++-linked object"
fi

# --- leg 6d: a g++ throw crossing a madc object's frame runs its destructor -
# The object's own personality copy runs the frame's cleanup for a foreign
# exception, as g++ destroys a frame's objects for any exception. Oracle: the
# same two files built by g++ alone.
cat > "$D/xframe.cpp" <<'EOF'
#include <cstdio>
struct D { ~D() { std::puts("dtor"); } };
void g();
void f() { D d; g(); std::puts("not reached"); }
EOF
cat > "$D/xmain.cpp" <<'EOF'
#include <cstdio>
void g() { throw 1; }
void f();
int main() { try { f(); } catch (int) { std::puts("caught"); } return 0; }
EOF
g++ -c -o "$D/xframe_g.o" "$D/xframe.cpp" && g++ -c -o "$D/xmain.o" "$D/xmain.cpp" \
	&& g++ -o "$D/xoracle" "$D/xframe_g.o" "$D/xmain.o"
xoracle=$(run "$D/xoracle" 2>/dev/null)
run "$MADC" --forest-bind="$D/madc.forest" --std=gnu++17 -c -o "$D/xframe_m.o" \
	"$D/xframe.cpp" > "$D/xframe.emit" 2>&1
xobj_rc=$?
g++ -o "$D/xmixed" "$D/xframe_m.o" "$D/xmain.o" > "$D/xmixed.link" 2>&1
xlink_rc=$?
xmixed=$(run "$D/xmixed" 2>/dev/null)
if [ "$xoracle" != "$(printf 'dtor\ncaught')" ]; then
	fail "foreign-throw oracle (g++ alone) prints dtor, caught (got '$xoracle')"
elif [ $xobj_rc -ne 0 ] || [ $xlink_rc -ne 0 ]; then
	fail "foreign-throw madc object links with plain g++ (madc rc=$xobj_rc, g++ rc=$xlink_rc)"
	sed -n '1,6p' "$D/xframe.emit" "$D/xmixed.link"
elif [ "$xmixed" != "$xoracle" ]; then
	fail "a g++ throw through a madc frame runs its destructor (got '$xmixed')"
else
	pass "a g++ throw through a madc object's frame runs its destructor"
fi

# --- leg 7: a carrier with no ledger gets the BUILD-side message ----------
run "$MADC" --freeze-append="$D/noledger.forest" "$D/tu.mad" \
	> /dev/null 2>&1
run "$MADC" --forest-bind="$D/noledger.forest" -static-libmadc \
	-o "$D/nl" "$D/trycatch.mad" > "$D/noledger.log" 2>&1
nl_rc=$?
nl=$(tr -d '\0' < "$D/noledger.log")
if [ $nl_rc -eq 0 ]; then
	fail "no-ledger carrier refuses"
elif ! grep -q "needs this madc's AOT ledger, and no carrier provided one" <<<"$nl"; then
	fail "no-ledger carrier uses the build-side message"
	sed -n '1,6p' "$D/noledger.log"
elif grep -q 'Tier B' <<<"$nl"; then
	fail "no-ledger carrier must NOT blame Tier B"
else
	pass "no-ledger carrier refuses with the build-side message"
fi

# --- leg 7b: the startup object comes from the ledger, never the text -----
# A -c of leg 6d's frame with a destructor merges the ledger's pre-generated
# personality object (B231); a carrier with no ledger generates the same
# object in-process. Either object links with plain g++ and runs the frame's
# destructor for the foreign throw (oracle: g++ alone, leg 6d).
crt_leg()
{
	local carrier=$1 out=$2 want=$3
	run "$MADC" --forest-bind="$carrier" --std=gnu++17 -v -c -o "$D/$out.o" \
		"$D/xframe.cpp" > "$D/$out.log" 2>&1
	local mrc=$?
	g++ -o "$D/$out" "$D/$out.o" "$D/xmain.o" > "$D/$out.link" 2>&1
	local lrc=$?
	local got
	got=$(run "$D/$out" 2>/dev/null)
	if [ $mrc -ne 0 ] || [ $lrc -ne 0 ]; then
		fail "startup object ($want): -c and a g++ link (madc rc=$mrc, g++ rc=$lrc)"
		grep -a -m3 -E 'error|madc:' "$D/$out.log" "$D/$out.link"
	elif ! tr -d '\0' < "$D/$out.log" | grep -aq "^crt: crt:[a-z_]*: object $want"; then
		fail "startup object: $want"
		tr -d '\0' < "$D/$out.log" | grep -a '^crt: ' | head -3
	elif [ "$got" != "$xoracle" ]; then
		fail "startup object ($want): g++-linked output matches g++ alone (got '$got')"
	else
		pass "startup object: $want, links with g++, runs the destructor"
	fi
}
crt_leg "$D/madc.forest" crt_ledger "from the ledger"
crt_leg "$D/noledger.forest" crt_generated "generated"

# --- leg 8: -c refuses (the runtime merges at the final link) -------------
run "$MADC" -static-libmadc -c -o "$D/tc.o" "$D/trycatch.mad" \
	> "$D/dashc.log" 2>&1
if [ $? -ne 0 ] && grep -q 'requires a linked native output' "$D/dashc.log"; then
	pass "-static-libmadc -c refuses at the CLI"
else
	fail "-static-libmadc -c refuses at the CLI"
	sed -n '1,4p' "$D/dashc.log"
fi

# --- leg 9: the .o LINK lane merges the ledger ----------------------------
# The runtime is carried as MIR modules, so in this lane it is generated into
# its own relocatable and merged into the same builder as the inputs. Two
# objects, so a per-object merge (or a missed one) cannot pass.
# -fno-eval-shims: these objects are headed for a standalone link, so they
# leave out the host-call adapters whose value-ABI imports are Tier B.
run "$MADC" -fno-eval-shims -c -o "$D/lm.o" "$D/linkmain.mad" > "$D/lm.log" 2>&1
run "$MADC" -fno-eval-shims -c -o "$D/lh.o" "$D/linkhelp.mad" > "$D/lh.log" 2>&1
if [ ! -f "$D/lm.o" ] || [ ! -f "$D/lh.o" ]; then
	fail "could not build the .o pair for the link-lane leg"
	sed -n '1,4p' "$D/lm.log" "$D/lh.log"
else
	# Baseline first (same discipline as legs 2/5): the link must genuinely
	# need the runtime, or the -static-libmadc assertions are vacuous.
	run "$MADC" -o "$D/lbase" "$D/lm.o" "$D/lh.o" > "$D/lbase.log" 2>&1
	lbase_rc=$?
	lbase_needed=$(readelf -d "$D/lbase" 2>/dev/null | grep -ao 'libmadc[^]]*')
	if [ $lbase_rc -eq 0 ] && [ -n "$lbase_needed" ]; then
		pass ".o link baseline is genuinely runtime-needing"
	else
		fail ".o link baseline is genuinely runtime-needing (rc=$lbase_rc needed='$lbase_needed')"
		sed -n '1,4p' "$D/lbase.log"
	fi
	run "$MADC" --forest-bind="$D/madc.forest" -static-libmadc \
		-o "$D/lstatic" "$D/lm.o" "$D/lh.o" > "$D/lstatic.log" 2>&1
	lst_rc=$?
	lst_needed=$(readelf -d "$D/lstatic" 2>/dev/null | grep -ao 'libmadc[^]]*')
	lst_und=$(readelf --dyn-syms -W "$D/lstatic" 2>/dev/null \
		  | awk '$7=="UND" && $5!="WEAK"{print $8}' | grep -a '^__madc_' | sort -u)
	if [ $lst_rc -ne 0 ]; then
		fail ".o link lane -static-libmadc emit (rc=$lst_rc)"
		sed -n '1,15p' "$D/lstatic.log"
	elif [ -n "$lst_needed" ]; then
		fail ".o link image still needs '$lst_needed'"
	elif [ -n "$lst_und" ]; then
		fail ".o link image still imports: $(tr '\n' ' ' <<<"$lst_und")"
	else
		pass ".o link lane -static-libmadc: no madc library, no __madc_* imports"
	fi
	# The baseline binary IS the oracle: same objects, same link, the only
	# difference is where the runtime came from.
	lbase_out=$(run "$D/lbase" 2>/dev/null)
	lst_out=$(run "$D/lstatic" 2>/dev/null)
	if [ -n "$lst_out" ] && [ "$lbase_out" = "$lst_out" ]; then
		pass ".o link lane output matches the libmadc-linked baseline"
	else
		fail ".o link lane output matches the libmadc-linked baseline"
		diff <(echo "$lbase_out") <(echo "$lst_out") | head -6
	fi
	lst_clean=$(env -u LD_LIBRARY_PATH -i LD_LIBRARY_PATH=/nonexistent \
		    timeout 60 "$PWD/$D/lstatic" 2>&1)
	if [ "$lst_clean" = "$lbase_out" ]; then
		pass ".o link image runs with an empty library path"
	else
		fail ".o link image runs with an empty library path"
		echo "$lst_clean" | head -4
	fi
	# The other half of the contract: objects that KEPT their host-call
	# adapters cannot be covered (the value ABI is Tier B), and the refusal
	# must name the flag that fixes it — not leave the user guessing.
	run "$MADC" -c -o "$D/lmshim.o" "$D/linkmain.mad" > /dev/null 2>&1
	run "$MADC" -c -o "$D/lhshim.o" "$D/linkhelp.mad" > /dev/null 2>&1
	run "$MADC" --forest-bind="$D/madc.forest" -static-libmadc \
		-o "$D/lshim" "$D/lmshim.o" "$D/lhshim.o" > "$D/lshim.log" 2>&1
	shim_rc=$?
	shim_log=$(tr -d '\0' < "$D/lshim.log")
	if [ $shim_rc -eq 0 ]; then
		fail "shim-carrying objects refuse under -static-libmadc"
	elif ! grep -q 'madc_value_' <<<"$shim_log"; then
		fail "shim refusal names the value-ABI symbols"
		sed -n '1,6p' "$D/lshim.log"
	elif ! grep -q -- '-fno-eval-shims' <<<"$shim_log"; then
		fail "shim refusal names -fno-eval-shims as the fix"
		sed -n '1,6p' "$D/lshim.log"
	else
		pass "shim-carrying objects refuse, naming the symbols and the fix"
	fi
	# -r keeps refusing: the runtime belongs in the image, not in an object
	# that a later link would duplicate it from.
	run "$MADC" -static-libmadc -r -o "$D/lcomb.o" "$D/lm.o" "$D/lh.o" \
		> "$D/lr.log" 2>&1
	if [ $? -ne 0 ] && grep -q 'requires a linked native output' "$D/lr.log"; then
		pass "-static-libmadc -r still refuses at the CLI"
	else
		fail "-static-libmadc -r still refuses at the CLI"
		sed -n '1,4p' "$D/lr.log"
	fi
fi

# --- leg 10: an image names only the libraries its imports reach -----------
# gcc's --as-needed link is the oracle: a C program's image names libc.so.6
# alone and a sqrt call adds libm.so.6 — madc's implicit C++ runtime and libm
# stay only where an import reaches them (a C++ program keeps its libstdc++).
cat > "$D/c_plain.c" <<'EOF'
#include <stdio.h>
int main(void) { puts("plain"); return 0; }
EOF
cat > "$D/c_math.c" <<'EOF'
#include <math.h>
#include <stdio.h>
int main(void) { volatile double x = 2.0; printf("%.3f\n", sqrt(x)); return 0; }
EOF
needed_list() { readelf -dW "$1" 2>/dev/null | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | tr '\n' ' '; }
# $1 = source, $2 = madc flags (word-split), $3 = gcc's extra flags: madc's
# image names exactly gcc's libraries, runs, and prints gcc's output.
as_needed_leg()
{
	local src=$1 flags=$2 gflags=$3 n
	n=$(basename "${src%.c}")
	# shellcheck disable=SC2086
	run "$MADC" $flags -o "$D/$n.madc" "$src" > "$D/$n.emit" 2>&1
	local mrc=$?
	# shellcheck disable=SC2086
	gcc -o "$D/$n.gcc" "$src" $gflags > "$D/$n.gcc.log" 2>&1
	local mneed gneed mout gout
	mneed=$(needed_list "$D/$n.madc")
	gneed=$(needed_list "$D/$n.gcc")
	mout=$(env -u LD_LIBRARY_PATH -i LD_LIBRARY_PATH=/nonexistent timeout 60 "$PWD/$D/$n.madc" 2>&1)
	gout=$(run "$D/$n.gcc" 2>&1)
	if [ $mrc -ne 0 ]; then
		fail "$n: madc -o $flags (rc=$mrc)"
		sed -n '1,6p' "$D/$n.emit"
	elif [ "$mneed" != "$gneed" ]; then
		fail "$n: image names '$mneed', gcc's names '$gneed'"
	elif [ -z "$gout" ] || [ "$mout" != "$gout" ]; then
		fail "$n: output with an empty library path '$mout', gcc's '$gout'"
	else
		pass "$n: names gcc's libraries ($gneed) and runs with no madc library"
	fi
}
as_needed_leg "$D/c_plain.c" "" ""
as_needed_leg "$D/c_math.c" "-lm" "-lm"
# The .o link lane names the same set (the same rule, cir_elf_needed).
run "$MADC" -fno-eval-shims -c -o "$D/c_plain.o" "$D/c_plain.c" > "$D/c_plain_o.log" 2>&1
run "$MADC" -o "$D/c_plain_link" "$D/c_plain.o" > "$D/c_plain_link.log" 2>&1
link_need=$(needed_list "$D/c_plain_link")
plain_gcc_need=$(needed_list "$D/c_plain.gcc")
if [ -n "$link_need" ] && [ "$link_need" = "$plain_gcc_need" ]; then
	pass ".o link lane names gcc's libraries ($plain_gcc_need)"
else
	fail ".o link lane names '$link_need', gcc's names '$plain_gcc_need'"
	sed -n '1,4p' "$D/c_plain_o.log" "$D/c_plain_link.log"
fi
# The other side: a C++ image that imports from the C++ runtime keeps it.
run "$MADC" -o "$D/cpp_needed" "$D/cpp.mad" > "$D/cpp_needed.log" 2>&1
cpp_need=$(needed_list "$D/cpp_needed")
if grep -q 'libstdc++\.so\|libc++\.so' <<<"$cpp_need"; then
	pass "a C++ image keeps its C++ runtime ($cpp_need)"
else
	fail "a C++ image keeps its C++ runtime (names '$cpp_need')"
	sed -n '1,4p' "$D/cpp_needed.log"
fi

if [ $rc -eq 0 ]; then
	echo "forest_ledger_gate: OK"
else
	echo "forest_ledger_gate: FAILED"
fi
exit $rc
