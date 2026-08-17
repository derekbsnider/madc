#!/usr/bin/env bash
# ufcs_gate.sh — UFCS is a --std=madc DIALECT feature, in BOTH directions, and
# it is absent from EVERY strict standard mode.
#
# The tests in tests/testufcs*.mad cover behaviour, and each ships a
# negative control — but each pins exactly ONE strict mode (--std=c++17). The
# claim this feature actually makes is stronger: every explicit --std=, C and
# C++ alike, is byte-identical to what it was before UFCS existed. That is the
# claim a future dialect feature is most likely to break silently, by gating
# itself on presents_as_cpp() or auto_includes_enabled() instead of
# ufcs_enabled() — both of which are true for more than just STD_MADC. One mode
# would keep passing its test while the other nineteen quietly changed.
#
# So this gate sweeps the whole --std= matrix. It needs no separate negative
# control: leg 1 asserts UFCS WORKS under the dialect, so reverting the feature
# fails the gate, and legs 2-3 assert it does not work anywhere else, so leaking
# it fails the gate too. Both directions of failure are covered.
#
# Run from the repo root (fulltest does).
set -u
cd "$(dirname "$0")/.."

ulimit -t 240 2>/dev/null

BIN=${MADC_BIN:-bin/madc}
if [ ! -x "$BIN" ]; then
    echo "ufcs_gate: missing $BIN"
    exit 1
fi

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Direction 1: dot -> free function. `p.magsq()` has no member to find.
cat > "$TMP/dot.mad" <<'EOF'
#include <stdio.h>
class Point { public: int x; int y; };
int magsq(Point &p) { return p.x * p.x + p.y * p.y; }
int main() { Point p; p.x = 3; p.y = 4; printf("%d\n", p.magsq()); return 0; }
EOF

# Direction 2: call -> member. No free `tally` is declared anywhere.
cat > "$TMP/call.mad" <<'EOF'
#include <stdio.h>
class Bag { public: int n; int tally() { return this->n * 3; } };
int main() { Bag b; b.n = 5; printf("%d\n", tally(b)); return 0; }
EOF

# Direction 1 on a NON-class receiver — the widest surface, so the one most
# likely to leak into a strict mode unnoticed.
#
# This probe is also the ONLY one that meaningfully tests the C modes. dot.mad
# and call.mad both declare a `class`, so under --std=c89 they are rejected for
# a reason that has nothing to do with UFCS; their C legs prove little on their
# own. prim.mad is valid C apart from the `n.twice()` itself, so its rejection
# under a C standard really is UFCS staying out of the dialect. Keep it that
# way: if this probe ever gains a C++-only construct, the C half of this gate
# silently stops testing anything.
cat > "$TMP/prim.mad" <<'EOF'
#include <stdio.h>
int twice(int n) { return n * 2; }
int main() { int n = 21; printf("%d\n", n.twice()); return 0; }
EOF

fail() { echo "ufcs_gate: $1"; exit 1; }

run_std() {	# run_std <file> <std-or-empty>; echoes "<rc>:<stdout>"
    local f=$1 std=$2 out rc
    if [ -z "$std" ]; then
	out=$( ( timeout 60 "$BIN" "$f" ) 2>/dev/null )
    else
	out=$( ( timeout 60 "$BIN" "--std=$std" "$f" ) 2>/dev/null )
    fi
    rc=$?
    echo "${rc}:${out}"
}

# ---- leg 1: the dialect. Default (no --std=) and explicit --std=madc both
# resolve UFCS, in both directions and on both receiver kinds.
for probe in "dot.mad 25" "call.mad 15" "prim.mad 42"; do
    set -- $probe
    f=$1 want=$2
    for std in "" madc; do
	got=$(run_std "$TMP/$f" "$std")
	label=${std:-<default>}
	case "$got" in
	    "0:$want") ;;
	    *) fail "$f under --std=$label: expected rc 0 and '$want', got '${got}'" ;;
	esac
    done
done
echo "  ok   UFCS resolves under the madc dialect (dot, call, non-class)"

# ---- legs 2 and 3: every explicit standard REJECTS all three, C and C++ alike.
# One entry per accepted spelling in Program::set_language_standard, minus the
# aliases (c90/cpp17/... select the same enumerator as their twin).
C_STDS="c78 c86 c88 c89 c90 c94 c95 c99 c c11 c17 c23"
CPP_STDS="c++98 c++03 c++ c++11 c++14 c++17 c++20 c++23 c++26"

for std in $C_STDS $CPP_STDS; do
    for f in dot.mad call.mad prim.mad; do
	got=$(run_std "$TMP/$f" "$std")
	rc=${got%%:*}
	case "$rc" in
	    0)   fail "$f COMPILED under --std=$std — UFCS leaked out of the dialect" ;;
	    124) fail "$f TIMED OUT under --std=$std (not a rejection)" ;;
	    *)   ;;	# any nonzero-but-not-timeout is the rejection we want
	esac
    done
done
nc=$(echo $C_STDS | wc -w)
np=$(echo $CPP_STDS | wc -w)
echo "  ok   UFCS absent from every strict standard ($nc C + $np C++ modes x 3 probes)"

echo "ufcs_gate: OK"
exit 0
