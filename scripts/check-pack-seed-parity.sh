#!/bin/bash
# check-pack-seed-parity.sh — a seeded bind restores what the whole-arena walk
# restores, less only what no bound unit declares (pack slices design §8,
# docs/plans/2026-10-13-pack-slices-design.md).
#
#   bash scripts/check-pack-seed-parity.sh [madc-binary [madc args...]]
#
# Default binary: bin/madc-release (its libmadc carries the pack). Extra args
# reach every compile, e.g. `bin/madc --forest-bind=PACK` for a dev freeze.
#
# Each workload compiles twice with MADC_SEED_PARITY_PROBE=1, which prints,
# after each materialize generation, one `SEEDPAR <kind> <name> v=<verdict>`
# line per record restored so far (the gate reads the last block): once seeded (the bind visits the bound units' seed lists), once as
# the whole walk (MADC_FOREST_WHOLE_WALK=1, the oracle). PASS requires:
#   1. the seeded lines are a sub-multiset of the whole walk's;
#   2. every whole-only line carries v<=0 — a name no bound unit declares
#      (registration drops a -1; a 0 is an undecided record whose point of
#      creation is an unbound unit) — or is a `product` / `instance` (a
#      template-id class product, a function-template instance): those are
#      placed with the units whose parse required them, not by their
#      template's name, so the whole walk keeps ones no bound unit asked for
#      (design §4, the differences the design accepts);
#   3. NEGATIVE CONTROL — each workload printed SEEDPAR lines in both runs (a
#      run that bound nothing would pass 1 and 2 vacuously); only a program
#      with no #include may bind nothing.
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

W=tmp/seed_parity
rm -rf "$W"
mkdir -p "$W"
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
	MADC_SEED_PARITY_PROBE=1 timeout 300 "$BIN" --no-config "${EXTRA[@]}" "$p" \
		>/dev/null 2>"$W/$b.seed.err"
	MADC_SEED_PARITY_PROBE=1 MADC_FOREST_WHOLE_WALK=1 timeout 300 "$BIN" --no-config \
		"${EXTRA[@]}" "$p" >/dev/null 2>"$W/$b.whole.err"
	# The last generation's block: every record restored, judged by the
	# final filter.
	last_block() { awk '/^SEEDPAR-GEN$/ { n = 0; delete L; next }
		/^SEEDPAR / { L[++n] = $0 } END { for (i = 1; i <= n; ++i) print L[i] }' "$1"; }
	last_block "$W/$b.seed.err" | LC_ALL=C sort > "$W/$b.seed"
	last_block "$W/$b.whole.err" | LC_ALL=C sort > "$W/$b.whole"
	ns=$(wc -l < "$W/$b.seed")
	nw=$(wc -l < "$W/$b.whole")
	if [ "$ns" -eq 0 ] && [ "$nw" -eq 0 ] && ! grep -q '^[[:space:]]*#[[:space:]]*include' "$p"; then
		# A zero-include dialect program binds no unit until the prelude
		# is one (design §10 part 4).
		echo "check-pack-seed-parity: $b binds no unit (no #include)"
		continue
	fi
	if [ "$ns" -eq 0 ] || [ "$nw" -eq 0 ]; then
		echo "check-pack-seed-parity: FAIL — $b restored nothing (seeded $ns, whole $nw lines): the run did not bind the pack" >&2
		fail=1
		continue
	fi
	extra=$(LC_ALL=C comm -23 "$W/$b.seed" "$W/$b.whole")
	missing=$(LC_ALL=C comm -13 "$W/$b.seed" "$W/$b.whole")
	bad=$(printf '%s\n' "$missing" | grep -a ' v=1$' \
		| grep -av '^SEEDPAR \(product\|instance\) ' || true)
	nmiss=$(printf '%s' "$missing" | grep -ac '^SEEDPAR' || true)
	if [ -n "$extra" ]; then
		echo "check-pack-seed-parity: FAIL — $b: the seeded bind restored records the whole walk did not:" >&2
		printf '%s\n' "$extra" | head -10 >&2
		fail=1
	fi
	if [ -n "$bad" ]; then
		echo "check-pack-seed-parity: FAIL — $b: records a BOUND unit declares are missing from the seeded bind:" >&2
		printf '%s\n' "$bad" | head -10 >&2
		fail=1
	fi
	echo "check-pack-seed-parity: $b seeded=$ns whole=$nw undeclared-only=$nmiss"
done
if [ "$fail" -ne 0 ]; then
	echo "check-pack-seed-parity: FAIL (work files in $W)" >&2
	exit 1
fi
echo "check-pack-seed-parity: PASS"
