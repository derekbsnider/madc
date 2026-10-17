#!/bin/bash
# check-pack-seed-parity.sh — a seeded bind restores only what its bound
# closure reaches (pack slices design §8, docs/plans/2026-10-13-pack-slices-
# design.md — the closure-only invariant), and a dialect record only what a
# lookup demands (docs/plans/2026-10-16-dialect-per-name-plan.md, A7).
#
#   bash scripts/check-pack-seed-parity.sh [madc-binary [madc args...]]
#
# Default binary: bin/madc-release (its libmadc carries the pack). Extra args
# reach every compile, e.g. `bin/madc --forest-bind=PACK` for a dev freeze.
#
# Each workload compiles with MADC_SEED_PARITY_PROBE=1, which prints, after
# each materialize generation, one `SEEDPAR <kind> <name> v=<verdict> [<role>]
# <via>` line per record restored so far; the gate reads the last block. <via>
# is how the bind reached the record: `demanded` — asked for by record (a
# dialect lookup's demand), `listed` — in a visited seed list (a bound unit's:
# what its decl index declares, its parse created or its instantiations
# required; or the always list), `carried` — a reference pull, a
# function-local class admitted by structure, an admitted owner's dependent,
# or `unreached`. <role> marks a record a dialect fragment's decl index names:
# `anchor` (a global with storage — the bind restores it) or `dialect` (any
# other). PASS requires:
#   1. no `unreached` line: every restored record's declaring, creating or
#      requiring unit is bound, or a bound record carried it (design §4,
#      "Pulls"). A v=-1 line (a name only unbound units declare) is reported,
#      not failed: an instantiation's static member rides the units requiring
#      its product (design §4);
#   2. no `dialect listed` line: a dialect record restores only as demanded,
#      carried or an anchor;
#   3. a workload with a <name>.restored file beside it restores at most that
#      many records — a number that may only fall (lower it when it does);
#   4. NEGATIVE CONTROLS — each workload printed SEEDPAR lines (a run that
#      bound nothing would pass 1 vacuously; only a program with no #include
#      may bind nothing); the checks flag a synthetic unreached line and a
#      synthetic listed dialect line; and a dialect program compiled with the
#      withholding off (--forest-touch-log, the pack's layout pass: no unit
#      activates, every dialect record restores with its unit) is flagged.
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
dialect_listed() { grep -a ' dialect listed$' "$1" || true; }

# probe <program> <out.seed> [madc args...] — the last generation's block:
# every record restored, judged by the final filter.
probe() {
	local p="$1" seed="$2"
	shift 2
	MADC_SEED_PARITY_PROBE=1 timeout 300 "$BIN" --no-config "${EXTRA[@]}" "$@" "$p" \
		>/dev/null 2>"$seed.err"
	awk '/^SEEDPAR-GEN$/ { n = 0; delete L; next }
		/^SEEDPAR / { L[++n] = $0 } END { for (i = 1; i <= n; ++i) print L[i] }' \
		"$seed.err" | LC_ALL=C sort > "$seed"
}

W=tmp/seed_parity
rm -rf "$W"
mkdir -p "$W"

printf '%s\n' 'SEEDPAR tag ok v=1 listed' 'SEEDPAR nsbind std::pulled v=-1 carried' \
	'SEEDPAR global std::p_int__id v=-1 listed' 'SEEDPAR tag std::stray v=0 unreached' \
	'SEEDPAR func __ns_php_count v=1 dialect demanded' \
	'SEEDPAR tag madc::SysInfo v=1 dialect carried' \
	'SEEDPAR global madc::__madc_run_child v=1 anchor listed' \
	'SEEDPAR func __ns_php_trim v=1 dialect listed' > "$W/control"
if [ "$(unreached "$W/control")" != 'SEEDPAR tag std::stray v=0 unreached' ]; then
	echo "check-pack-seed-parity: FAIL — the negative control's unreached line was not flagged exactly" >&2
	exit 1
fi
if [ "$(dialect_listed "$W/control")" != 'SEEDPAR func __ns_php_trim v=1 dialect listed' ]; then
	echo "check-pack-seed-parity: FAIL — the negative control's listed dialect line was not flagged exactly" >&2
	exit 1
fi

# Withholding off: the dialect units restore whole, so their records are
# reached as listed — the probe must mark them and the check must see them.
printf 'int main()\n{\n    println("{}", php::strtoupper("ok"));\n    return 0;\n}\n' \
	> "$W/withholding_off.mad"
probe "$W/withholding_off.mad" "$W/withholding_off.seed" --forest-touch-log="$W/touch.log"
if [ -z "$(dialect_listed "$W/withholding_off.seed")" ]; then
	echo "check-pack-seed-parity: FAIL — negative control: a dialect program compiled" \
	     "with the withholding off (--forest-touch-log) restored no dialect record as" \
	     "listed (a binary whose probe marks no dialect record?)" >&2
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
	if grep -avq ' \(listed\|carried\|demanded\|unreached\)$' "$W/$b.seed"; then
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
	bad=$(dialect_listed "$W/$b.seed")
	if [ -n "$bad" ]; then
		echo "check-pack-seed-parity: FAIL — $b: dialect records restored by a seed list, not by demand:" >&2
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
		"carried=$(grep -ac ' carried$' "$W/$b.seed") demanded=$(grep -ac ' demanded$' "$W/$b.seed")" \
		"dialect=$(grep -ac ' dialect [a-z]*$' "$W/$b.seed") anchors=$(grep -ac ' anchor [a-z]*$' "$W/$b.seed")" \
		"outside-closure=$(grep -ac ' v=-1 ' "$W/$b.seed")"
done
if [ "$fail" -ne 0 ]; then
	echo "check-pack-seed-parity: FAIL (work files in $W)" >&2
	exit 1
fi
echo "check-pack-seed-parity: PASS"
