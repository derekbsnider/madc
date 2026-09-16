#!/usr/bin/env bash
# mangle_abi_gate.sh — the checked-in Itanium oracle (tests/unit/mangle_oracle.inc)
# must be exactly what g++ AND clang++ emit for tests/abi/mangle_corpus.cpp TODAY.
#
# Why a gate: the .inc is the truth tests/unit/test_mangle.cpp replays against
# madc's Itanium encoders. If the corpus grows and the .inc is not regenerated,
# the new shape has no oracle row and the unit test's every-row-consumed check
# cannot see it (it only sees EXTRA rows). If the oracle compilers change what
# they emit, the contract moved. Either way the two must be re-agreed, never
# hand-edited — this gate regenerates into tmp/ and diffs the ROWS (the .inc
# header records compiler versions, which legitimately differ per host).
#
# Lanes:
#   drift — regenerate → row-diff against the checked-in .inc            (phase 0)
#   (phase 5 adds the end-to-end lane: madc --std=c++20 -c corpus → nm, set-equal
#    to the oracle — the madc↔g++ ABI parity acceptance of the design's §8.)
#
# Host requirements: both canon compilers (the container and the Mac runner
# have them); LP64 (the corpus's std::size_t row is `m` — the wine lane runs
# run_tests.sh, not fulltest, so an LLP64 host never reaches here).
#
# Run from the repo root (fulltest does).
set -u
cd "$(dirname "$0")/.."
ulimit -t 120 2>/dev/null

fail() { echo "mangle_abi_gate: $1"; exit 1; }

inc=tests/unit/mangle_oracle.inc
[ -f "$inc" ] || fail "no $inc — run scripts/gen_mangle_oracle.sh on the container and scp it back"

fresh="$PWD/tmp/mangle_oracle_fresh.inc"
rm -f "$fresh"
bash scripts/gen_mangle_oracle.sh "$fresh" || fail "the oracle could not be regenerated (see above)"

rows() { grep '^{' "$1"; }
if ! cmp -s <(rows "$inc") <(rows "$fresh"); then
	echo "mangle_abi_gate: the checked-in oracle DRIFTED from what g++/clang++ emit today (< checked-in, > fresh):"
	diff <(rows "$inc") <(rows "$fresh") | head -40
	fail "regenerate tests/unit/mangle_oracle.inc on the container (scripts/gen_mangle_oracle.sh), scp it back, update the encoder checks — never hand-edit it"
fi
n=$(rows "$inc" | grep -c .)
[ "$n" -ge 100 ] || fail "only $n oracle rows — the corpus no longer exercises the shapes (non-vacuity floor 100)"
echo "mangle_abi_gate: [drift] OK — $n oracle rows; g++ and clang++ agree with the checked-in oracle"
echo "mangle_abi_gate: OK"
