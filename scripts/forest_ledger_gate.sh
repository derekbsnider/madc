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
#   1  freeze a container WITH the ledger; --dump-forest reports it
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
#   7  no-ledger carrier: a container packed without --freeze-ledger= refuses
#      with the build-side message, never the Tier-B one
#   8  -static-libmadc -c refuses at the CLI (the runtime merges at the link)
#   9  the .o LINK lane merges the ledger: two -c objects (one needing
#      try/catch, one needing VLA scope exit) link runtime-free, produce the
#      same output as the libmadc-linked baseline, and run on a clean machine.
#      Also the boundary: objects that kept their __madc_shim_* host-call
#      adapters refuse (value ABI = Tier B) naming -fno-eval-shims, and -r
#      still refuses, so the runtime rides the final link only
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
cat > "$D/tu.c" <<'EOF'
#include <stdio.h>
int main(void) { return 0; }
EOF

cat > "$D/trycatch.c" <<'EOF'
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

cat > "$D/vla.c" <<'EOF'
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
cat > "$D/linkmain.c" <<'EOF'
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

cat > "$D/linkhelp.c" <<'EOF'
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

LEDGER_ARGS=()
while read -r src; do
	LEDGER_ARGS+=("--freeze-ledger=$src")
done < <(grep -vE '^[[:space:]]*(#|$)' "$LEDGER_LIST")
if [ ${#LEDGER_ARGS[@]} -eq 0 ]; then
	echo "forest_ledger_gate: $LEDGER_LIST lists no sources"
	exit 1
fi

# --- leg 1: freeze a container carrying the ledger ------------------------
if run "$MADC" "${LEDGER_ARGS[@]}" --freeze-append="$D/madc.forest" "$D/tu.c" \
	> "$D/freeze.out" 2> "$D/freeze.err"; then
	pass "freeze with ledger"
else
	fail "freeze with ledger (rc=$?)"
	sed -n '1,20p' "$D/freeze.err"
	exit $rc
fi
dump=$(run "$MADC" --dump-forest="$D/madc.forest" 2>/dev/null | tr -d '\0')
if grep -q '^ledger	modules=2	symbols=' <<<"$dump"; then
	pass "--dump-forest reports the ledger"
else
	fail "--dump-forest reports the ledger"
	grep -a '^ledger' <<<"$dump" | head -3
fi
for src in "${LEDGER_ARGS[@]}"; do
	if grep -q "^ledgermod	${src#--freeze-ledger=}	" <<<"$dump"; then
		pass "ledger carries ${src#--freeze-ledger=}"
	else
		fail "ledger carries ${src#--freeze-ledger=}"
	fi
done

# --- shared helper: emit + inspect ----------------------------------------
# $1 = source, $2 = out name, $3.. = extra madc flags. Sets NEEDED/UND/OUT.
emit_and_inspect()
{
	local src=$1 out=$2; shift 2
	run "$MADC" --forest-bind="$D/madc.forest" "$@" -o "$D/$out" "$src" \
		> "$D/$out.emit" 2>&1
	EMIT_RC=$?
	NEEDED=$(readelf -d "$D/$out" 2>/dev/null | grep -ao 'libmadc[^]]*')
	UND=$(readelf --dyn-syms -W "$D/$out" 2>/dev/null \
	      | awk '$7=="UND"{print $8}' | grep -a '^__madc_' | sort -u)
}

# --- leg 2: baseline — no flag keeps the dependency -----------------------
emit_and_inspect "$D/trycatch.c" base
if [ $EMIT_RC -eq 0 ] && [ -n "$NEEDED" ]; then
	pass "baseline keeps libmadc.so.0 (the flag is what changes it)"
else
	fail "baseline keeps libmadc.so.0 (rc=$EMIT_RC needed='$NEEDED')"
fi

# --- leg 3: try/catch, -static-libmadc ------------------------------------
emit_and_inspect "$D/trycatch.c" tc -static-libmadc
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
jit=$(run "$MADC" "$D/trycatch.c" 2>/dev/null)
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
emit_and_inspect "$D/vla.c" vlabase
if [ $EMIT_RC -eq 0 ] && [ -n "$NEEDED" ]; then
	pass "VLA baseline is genuinely runtime-needing"
else
	fail "VLA baseline is genuinely runtime-needing (rc=$EMIT_RC needed='$NEEDED')"
fi
emit_and_inspect "$D/vla.c" vla -static-libmadc
vjit=$(run "$MADC" "$D/vla.c" 2>/dev/null)
vaot=$(run "$D/vla" 2>/dev/null)
if [ $EMIT_RC -eq 0 ] && [ -z "$NEEDED" ] && [ -z "$UND" ] \
   && [ -n "$vaot" ] && [ "$vjit" = "$vaot" ]; then
	pass "VLA -static-libmadc: runtime-free and output-identical"
else
	fail "VLA -static-libmadc (rc=$EMIT_RC needed='$NEEDED' und='$UND')"
	sed -n '1,10p' "$D/vla.emit"
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

# --- leg 7: a carrier with no ledger gets the BUILD-side message ----------
run "$MADC" --freeze-append="$D/noledger.forest" "$D/tu.c" \
	> /dev/null 2>&1
run "$MADC" --forest-bind="$D/noledger.forest" -static-libmadc \
	-o "$D/nl" "$D/trycatch.c" > "$D/noledger.log" 2>&1
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

# --- leg 8: -c refuses (the runtime merges at the final link) -------------
run "$MADC" -static-libmadc -c -o "$D/tc.o" "$D/trycatch.c" \
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
run "$MADC" -fno-eval-shims -c -o "$D/lm.o" "$D/linkmain.c" > "$D/lm.log" 2>&1
run "$MADC" -fno-eval-shims -c -o "$D/lh.o" "$D/linkhelp.c" > "$D/lh.log" 2>&1
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
		  | awk '$7=="UND"{print $8}' | grep -a '^__madc_' | sort -u)
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
	run "$MADC" -c -o "$D/lmshim.o" "$D/linkmain.c" > /dev/null 2>&1
	run "$MADC" -c -o "$D/lhshim.o" "$D/linkhelp.c" > /dev/null 2>&1
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

if [ $rc -eq 0 ]; then
	echo "forest_ledger_gate: OK"
else
	echo "forest_ledger_gate: FAILED"
fi
exit $rc
