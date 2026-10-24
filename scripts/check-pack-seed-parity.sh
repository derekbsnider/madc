#!/bin/bash
# check-pack-seed-parity.sh — a seeded bind restores only what its bound
# closure reaches (pack slices design §8, docs/plans/2026-10-13-pack-slices-
# design.md — the closure-only invariant).
#
#   bash scripts/check-pack-seed-parity.sh [madc-binary [madc args...]]
#
# Default binary: bin/madc-release (its libmadc carries the pack). Extra args
# reach every compile, e.g. `bin/madc --forest-bind=PACK` for a dev freeze.
#
# Each workload compiles with MADC_SEED_PARITY_PROBE=1, which prints, after
# each materialize generation, one `SEEDPAR <kind> <name> v=<verdict> <via>`
# line per record restored so far, under a `SEEDPAR-GEN <forest role>`
# header; the gate reads each forest's last block. <via> is how the bind
# reached the record: `listed` — in a visited seed list (a bound unit's: what its decl index declares, its parse
# created or its instantiations required; or the always list), `carried` — a
# reference pull, a function-local class admitted by structure, an admitted
# owner's dependent, or `unreached`. PASS requires:
#   1. no `unreached` line: every restored record's declaring, creating or
#      requiring unit is bound, or a bound record carried it (design §4,
#      "Pulls"). A v=-1 line (a name only unbound units declare) is reported,
#      not failed: an instantiation's static member rides the units requiring
#      its product (design §4);
#   2. a workload with a <name>.restored file beside it restores at most that
#      many records — a number that may only fall (lower it when it does);
#   3. NEGATIVE CONTROLS — each workload printed SEEDPAR lines (a run that
#      bound nothing would pass 1 vacuously; only a program with no #include
#      may bind nothing); the check flags a synthetic unreached line.
# The workloads: the parse-cost ladder that binds the pack (scripts/parse_cost/
# *.cpp, *.mad) and one TU per single-header rung of the design's targets.
set -u
cd "$(dirname "$0")/.." || exit 2
BIN="${1:-bin/madc-release}"
[ $# -gt 0 ] && shift
EXTRA=("$@")

if [ ! -x "$BIN" ]; then
	echo "check-pack-seed-parity: FAIL — no binary at $BIN" >&2
	exit 1
fi

# The invariants' readers: the lines of a sorted last block that break them.
unreached() { grep -a ' unreached$' "$1" || true; }

# probe <program> <out.seed> [madc args...] — the last generation's block:
# every record restored, judged by the final filter.
probe() {
	local p="$1" seed="$2"
	shift 2
	MADC_SEED_PARITY_PROBE=1 timeout 300 "$BIN" --no-config "${EXTRA[@]}" "$@" "$p" \
		>/dev/null 2>"$seed.err"
	awk '/^SEEDPAR-GEN/ { k = NF > 1 ? $2 : "system"; n[k] = 0; next }
		/^SEEDPAR / { L[k, ++n[k]] = $0 }
		END { for (f in n) for (i = 1; i <= n[f]; ++i) print L[f, i] }' \
		"$seed.err" | LC_ALL=C sort > "$seed"
}

W=tmp/seed_parity
rm -rf "$W"
mkdir -p "$W"

printf '%s\n' 'SEEDPAR tag ok v=1 listed' 'SEEDPAR nsbind std::pulled v=-1 carried' \
	'SEEDPAR global std::p_int__id v=-1 listed' 'SEEDPAR tag std::stray v=0 unreached' \
	> "$W/control"
if [ "$(unreached "$W/control")" != 'SEEDPAR tag std::stray v=0 unreached' ]; then
	echo "check-pack-seed-parity: FAIL — the negative control's unreached line was not flagged exactly" >&2
	exit 1
fi

for h in cstddef cstdio limits type_traits vector; do
	printf '#include <%s>\nint main() { return 0; }\n' "$h" > "$W/rung_$h.cpp"
done
progs=("$W"/rung_*.cpp)
for f in scripts/parse_cost/*.cpp scripts/parse_cost/*.mad; do
	[ -f "$f" ] && progs+=("$f")
done

fail=0
for p in "${progs[@]}"; do
	b=$(basename "$p")
	probe "$p" "$W/$b.seed"
	n=$(wc -l < "$W/$b.seed")
	if [ "$n" -eq 0 ] && ! grep -q '^[[:space:]]*#[[:space:]]*include' "$p"; then
		# A zero-include dialect program binds no unit until the prelude
		# is one (design §10 part 4).
		echo "check-pack-seed-parity: $b binds no unit (no #include)"
		continue
	fi
	if [ "$n" -eq 0 ]; then
		echo "check-pack-seed-parity: FAIL — $b restored nothing: the run did not bind the pack" >&2
		fail=1
		continue
	fi
	if grep -avq ' \(listed\|carried\|unreached\)$' "$W/$b.seed"; then
		echo "check-pack-seed-parity: FAIL — $b: probe lines carry no reach label (a binary older than the gate?)" >&2
		fail=1
		continue
	fi
	bad=$(unreached "$W/$b.seed")
	if [ -n "$bad" ]; then
		echo "check-pack-seed-parity: FAIL — $b: restored records no seed list holds and nothing carried:" >&2
		printf '%s\n' "$bad" | head -10 >&2
		fail=1
	fi
	cap="${p%.*}.restored"
	if [ -f "$cap" ]; then
		c=$(cat "$cap")
		if [ "$n" -gt "$c" ]; then
			echo "check-pack-seed-parity: FAIL — $b restored $n records, more than $cap's $c (it may only fall)" >&2
			fail=1
		elif [ "$n" -lt "$c" ]; then
			echo "check-pack-seed-parity: $b restored $n records, fewer than $cap's $c — lower it to $n to keep the gain"
		fi
	fi
	echo "check-pack-seed-parity: $b restored=$n listed=$(grep -ac ' listed$' "$W/$b.seed")" \
		"carried=$(grep -ac ' carried$' "$W/$b.seed")" \
		"outside-closure=$(grep -ac ' v=-1 ' "$W/$b.seed")"
done
if [ "$fail" -ne 0 ]; then
	echo "check-pack-seed-parity: FAIL (work files in $W)" >&2
	exit 1
fi
echo "check-pack-seed-parity: PASS"
