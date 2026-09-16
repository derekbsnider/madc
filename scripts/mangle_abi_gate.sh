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
#   interop  — `--interop` (phase 2; fulltest runs it): the madc↔g++ LINK lane
#              over tests/abi/interop_*. Lane A: madc -c defines the header's
#              class members, g++ compiles a TU that constructs / calls /
#              overrides / deletes them, g++ links the two, the program's
#              output must equal the all-g++ build's. Lane B: the reverse
#              (g++ defines, madc uses). Plus the ALIEN-SYMBOL check: every
#              function madc's object defines must be one g++'s object of
#              the same definitions defines too — the gate that keeps the
#              internal Class__method / __oN spellings out of a C++ object
#              for good. Carries its own negative control.
#   (phase 5 adds the end-to-end lane: madc --std=c++20 -c corpus → nm, set-equal
#    to the oracle — the madc↔g++ ABI parity acceptance of the design's §8.)
#
# Host requirements: both canon compilers (the container and the Mac runner
# have them); LP64 (the corpus's std::size_t row is `m` — the wine lane runs
# run_tests.sh, not fulltest, so an LLP64 host never reaches here). The
# interop lane needs bin/madc (MADC_BIN overrides) and nm.
#
# Run from the repo root (fulltest does).
set -u
cd "$(dirname "$0")/.."
ulimit -t 180 2>/dev/null

fail() { echo "mangle_abi_gate: $1"; exit 1; }

selftest=0
interop=0
for arg in "$@"; do
	case "$arg" in
	--selftest) selftest=1;;
	--interop)  interop=1;;
	*) fail "unknown argument: $arg (only --selftest / --interop)";;
	esac
done

# ---- the interop lane (its own exit path: the drift lane below needs the
#      oracle .inc, which the interop lane does not) --------------------------
if [ "$interop" = 1 ]; then
	MADC_BIN="${MADC_BIN:-bin/madc}"
	[ -x "$MADC_BIN" ] || fail "[interop] no madc binary at $MADC_BIN (MADC_BIN overrides)"
	command -v g++ >/dev/null || fail "[interop] g++ not found"
	command -v nm  >/dev/null || fail "[interop] nm not found"
	abi=tests/abi
	out="$PWD/tmp/interop"
	rm -rf "$out"; mkdir -p "$out"
	# nm's defined FUNCTION symbols (T/W/t), stripped of madc's own runtime
	# machinery and of main. Both sides pass through this one filter, so a
	# Mach-O underscore prefix cancels out.
	defined_funcs() {
		nm --defined-only "$1" | awk '$2 ~ /^[TWt]$/ {print $3}' \
			| grep -v '^_*__madc\|^_*main$\|^_*_Z[0-9]*__madc' | sort -u
	}
	# The alien-symbol check as a function so the negative control can feed
	# it a doctored list: every madc symbol must be in the g++ set.
	alien_check() {  # $1 = madc symbol list, $2 = g++ symbol list
		local alien
		alien=$(comm -23 "$1" "$2")
		[ -z "$alien" ] || { echo "$alien"; return 1; }
		return 0
	}

	# Lane A oracle + lane B oracle: the same definer, both users, all g++.
	g++ -std=c++20 -c -o "$out/gxx_def.o" $abi/interop_gxx_def.cpp \
		|| fail "[interop] g++ could not compile the definer TU"
	g++ -std=c++20 -c -o "$out/gxx_use.o" $abi/interop_gxx_use.cpp \
		|| fail "[interop] g++ could not compile the user TU"
	g++ -std=c++20 -x c++ -c -o "$out/gxx_use_b.o" $abi/interop_madc_use.mad \
		|| fail "[interop] g++ could not compile the reverse-lane user TU (it must be valid C++)"
	g++ -o "$out/oracle_a" "$out/gxx_def.o" "$out/gxx_use.o" \
		|| fail "[interop] the all-g++ lane-A program did not link"
	g++ -o "$out/oracle_b" "$out/gxx_def.o" "$out/gxx_use_b.o" \
		|| fail "[interop] the all-g++ lane-B program did not link"
	"$out/oracle_a" > "$out/oracle_a.out" || fail "[interop] the all-g++ lane-A oracle failed to run"
	"$out/oracle_b" > "$out/oracle_b.out" || fail "[interop] the all-g++ lane-B oracle failed to run"

	# Lane A: madc defines (default --std=madc presents as C++), g++ uses.
	"$MADC_BIN" -c -o "$out/madc_def.o" $abi/interop_madc_def.mad \
		|| fail "[interop] madc could not compile the definer TU to an object"
	defined_funcs "$out/madc_def.o" > "$out/madc_def.syms"
	defined_funcs "$out/gxx_def.o"  > "$out/gxx_def.syms"
	if ! alien_check "$out/madc_def.syms" "$out/gxx_def.syms" > "$out/alien.txt"; then
		echo "mangle_abi_gate: [interop] madc's object defines function symbols g++ never would (internal spellings leaked into a C++ object):"
		sed 's/^/    /' "$out/alien.txt"
		fail "[interop] ALIEN SYMBOLS — every member of a user class must emit its Itanium name"
	fi
	g++ -o "$out/lane_a" "$out/madc_def.o" "$out/gxx_use.o" \
		|| fail "[interop] lane A did not LINK — g++'s user TU references symbols madc's definer object does not define (see the undefined references above)"
	"$out/lane_a" > "$out/lane_a.out" || fail "[interop] lane A ran but failed (rc=$?)"
	cmp -s "$out/lane_a.out" "$out/oracle_a.out" \
		|| { diff "$out/oracle_a.out" "$out/lane_a.out" | head -20; fail "[interop] lane A output differs from the all-g++ build (< oracle, > madc-defined)"; }
	echo "mangle_abi_gate: [interop] lane A OK — madc-defined class linked into a g++ program, output identical ($(grep -c . "$out/madc_def.syms") madc symbols, none alien)"

	# Lane B: g++ defines, madc (--std=c++20 this time) uses.
	"$MADC_BIN" --std=c++20 -c -o "$out/madc_use.o" $abi/interop_madc_use.mad \
		|| fail "[interop] madc could not compile the user TU to an object"
	g++ -o "$out/lane_b" "$out/gxx_def.o" "$out/madc_use.o" \
		|| fail "[interop] lane B did not LINK — madc's user TU imports symbols g++'s definer does not export"
	"$out/lane_b" > "$out/lane_b.out" || fail "[interop] lane B ran but failed (rc=$?)"
	cmp -s "$out/lane_b.out" "$out/oracle_b.out" \
		|| { diff "$out/oracle_b.out" "$out/lane_b.out" | head -20; fail "[interop] lane B output differs from the all-g++ build (< oracle, > madc-using)"; }
	echo "mangle_abi_gate: [interop] lane B OK — madc program linked against a g++-defined class, output identical"

	# NEGATIVE CONTROL: the alien check must bite on an internal spelling.
	{ cat "$out/madc_def.syms"; echo "Counter__add"; } | sort -u > "$out/doctored.syms"
	if alien_check "$out/doctored.syms" "$out/gxx_def.syms" > /dev/null; then
		fail "[interop] NEGATIVE CONTROL FAILED — an injected Counter__add passed the alien-symbol check"
	fi
	echo "mangle_abi_gate: [interop] negative control OK — an internal spelling is caught"
	echo "mangle_abi_gate: [interop] OK"
	[ "$selftest" = 1 ] || exit 0
fi

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
