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
#   drift    — regenerate → row-diff against the checked-in .inc         (phase 0)
#   selftest — `--selftest` (what fulltest runs): drift, PLUS the negative
#              control — a copy of the corpus with one extra shape must come
#              back as a row diff. A gate that cannot be shown to fail is not
#              a gate; this proves the comparison bites on every run.
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
ulimit -t 180 2>/dev/null

fail() { echo "mangle_abi_gate: $1"; exit 1; }

selftest=0
case "${1:-}" in --selftest) selftest=1;; "") ;; *) fail "unknown argument: $1 (only --selftest)";; esac

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

if [ "$selftest" = 1 ]; then
	# NEGATIVE CONTROL: the same pipeline over a corpus with ONE more shape
	# must produce exactly one more row than the checked-in oracle, and the
	# row-diff must see it. If it cannot, the drift lane above proved nothing.
	probe="$PWD/tmp/mangle_oracle_selftest.cpp"
	drifted="$PWD/tmp/mangle_oracle_selftest.inc"
	rm -f "$probe" "$drifted"
	cp tests/abi/mangle_corpus.cpp "$probe"
	echo 'void mangle_gate_selftest_drift(int, const char *) {}' >> "$probe"
	MANGLE_CORPUS="$probe" bash scripts/gen_mangle_oracle.sh "$drifted" >/dev/null \
		|| fail "[selftest] the probe corpus did not regenerate — the negative control could not run"
	nd=$(rows "$drifted" | grep -c .)
	[ "$nd" -eq $((n + 1)) ] || fail "[selftest] NEGATIVE CONTROL FAILED — the probe corpus has one extra shape but yielded $nd rows against $n"
	if cmp -s <(rows "$inc") <(rows "$drifted"); then
		fail "[selftest] NEGATIVE CONTROL FAILED — an extra corpus shape produced no row diff; the gate cannot see drift"
	fi
	echo "mangle_abi_gate: [selftest] OK — one extra corpus shape is detected as drift ($n → $nd rows)"
fi
echo "mangle_abi_gate: OK"
