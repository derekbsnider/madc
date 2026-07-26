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
#   6  Tier-B refusal: a C++ script-lane program refuses, NAMING symbols
#   7  no-ledger carrier: a container packed without --freeze-ledger= refuses
#      with the build-side message, never the Tier-B one
#   8  -static-libmadc -c refuses at the CLI (the runtime merges at the link)
#   9  -static-libmadc in the .o link lane refuses loudly (stated boundary)
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
run "$MADC" --forest-bind="$D/madc.forest" -static-libmadc -o "$D/cpp" \
	"$D/cpp.mad" > "$D/tierb.log" 2>&1
tierb_rc=$?
tierb=$(tr -d '\0' < "$D/tierb.log")
if [ $tierb_rc -eq 0 ]; then
	fail "Tier-B program refuses"
elif ! grep -q 'not on the AOT ledger (Tier B' <<<"$tierb"; then
	fail "Tier-B refusal uses the Tier-B message"
	sed -n '1,6p' "$D/tierb.log"
elif ! grep -qE '^    [A-Za-z_]' <<<"$tierb"; then
	fail "Tier-B refusal lists the offending symbols"
else
	pass "Tier-B program refuses loudly, naming symbols"
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

# --- leg 9: the .o link lane refuses (stated S5 boundary) -----------------
run "$MADC" -c -o "$D/tc.o" "$D/trycatch.c" > /dev/null 2>&1
if [ -f "$D/tc.o" ]; then
	run "$MADC" -static-libmadc -o "$D/tclink" "$D/tc.o" \
		> "$D/link.log" 2>&1
	if [ $? -ne 0 ] \
	   && grep -q 'not supported in the multi-object link lane' "$D/link.log"; then
		pass "the .o link lane refuses loudly"
	else
		fail "the .o link lane refuses loudly"
		sed -n '1,4p' "$D/link.log"
	fi
else
	fail "could not build a .o for the link-lane leg"
fi

if [ $rc -eq 0 ]; then
	echo "forest_ledger_gate: OK"
else
	echo "forest_ledger_gate: FAILED"
fi
exit $rc
