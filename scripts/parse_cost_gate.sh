#!/usr/bin/env bash
# parse_cost_gate.sh — the profiling MERGE GATE (owner 2026-10-08): the work
# madc does to compile a fixed set of workloads, measured in INSTRUCTIONS
# (callgrind Ir — deterministic, unlike wall time), ratcheted against a
# recorded baseline. A growth past the tolerance is RED; an improvement past
# it asks for the baseline to be re-recorded DOWNWARD so the gain is kept.
#
#   bash scripts/parse_cost_gate.sh                 # check (exit 1 on red)
#   bash scripts/parse_cost_gate.sh --record        # re-record (refuses growth)
#   bash scripts/parse_cost_gate.sh --record --accept-increase='<reason>'
#   MADC_BIN=bin/madc-release (default) — the shipped -O2 binary
#
# Workloads live in scripts/parse_cost/: <name>.<ext> + <name>.expect (its
# exact stdout). Each runs as a LIVE parse (--no-forest-bind: the parser and
# instantiator are what is measured); cxx_stl also runs through the packed
# forest, the path a default C++ compile takes. The baseline records the
# toolchain it was measured on (compiler, libc, libstdc++ headers, valgrind):
# a different toolchain parses different headers, so a mismatch REFUSES the
# comparison instead of passing or failing on someone else's numbers.
# Every run is capped (timeout + ulimit -t).
set -u
cd "$(dirname "$0")/.." || exit 9
DIR=scripts/parse_cost
BASE=$DIR/baseline.tsv
BIN=${MADC_BIN:-bin/madc-release}
TOL=${PARSE_COST_TOL:-0.5}	# percent (repeat runs agree to ~1e-8)
record=0; reason=""
for a in "$@"; do
	case "$a" in
	--record) record=1 ;;
	--accept-increase=*) reason="${a#--accept-increase=}" ;;
	*) echo "parse_cost_gate: unknown argument '$a'" >&2; exit 2 ;;
	esac
done
command -v valgrind > /dev/null || { echo "parse_cost_gate: valgrind not installed" >&2; exit 2; }
[ -x "$BIN" ] || { echo "parse_cost_gate: no $BIN (make -C src release)" >&2; exit 2; }

fingerprint() {
	local cxx libc hdr vg
	cxx=$(g++ -dumpfullversion 2>/dev/null)
	libc=$(ldd --version 2>/dev/null | head -1 | awk '{print $NF}')
	hdr=$(ls -d /usr/include/c++/[0-9]* 2>/dev/null | sort -V | tail -1)
	vg=$(valgrind --version 2>/dev/null)
	echo "g++-$cxx glibc-$libc ${hdr##*/} $vg"
}

SP=$(mktemp -d "${TMPDIR:-/tmp}/parse_cost.XXXXXX")
trap 'rm -rf "$SP"' EXIT

# measure <row> <source> <expect> [madc flags...] -> "<Ir>" or "FAIL <why>"
measure() {
	local row="$1" src="$2" exp="$3"; shift 3
	local out="$SP/$row.out" cg="$SP/$row.cg" rc ir
	( ulimit -t 900; timeout 900 valgrind --tool=callgrind --callgrind-out-file="$cg" \
		"$BIN" "$@" "$src" > "$out" 2> "$SP/$row.err" )
	rc=$?
	[ "$rc" -eq 0 ] || { echo "FAIL rc=$rc"; return; }
	cmp -s "$out" "$exp" || { echo "FAIL output differs from $exp"; return; }
	ir=$(grep -m1 '^summary:' "$cg" | awk '{print $2}')
	[ -n "$ir" ] || { echo "FAIL no callgrind summary"; return; }
	echo "$ir"
}

rows=()
for src in "$DIR"/*.cpp "$DIR"/*.c "$DIR"/*.mad; do
	[ -e "$src" ] || continue
	name=$(basename "$src"); name="${name%.*}"
	rows+=("$name.live|$src|--no-forest-bind")
	[ "${src##*.}" = cpp ] && rows+=("$name.forest|$src|")
done

fp=$(fingerprint)
declare -A now
red=0
for r in "${rows[@]}"; do
	IFS='|' read -r row src flags <<< "$r"
	name="${row%.*}"
	# shellcheck disable=SC2086
	now[$row]=$(measure "$row" "$src" "$DIR/$name.expect" $flags)
	case "${now[$row]}" in FAIL*) echo "RED  $row: ${now[$row]}"; red=$((red + 1)) ;; esac
done

base_fp=$(awk -F'\t' '$1=="# toolchain"{print $2}' "$BASE" 2>/dev/null)
if [ "$record" -eq 1 ]; then
	[ "$red" -eq 0 ] || { echo "parse_cost_gate: not recording — $red workload(s) failed"; exit 1; }
	grew=""
	if [ "$base_fp" = "$fp" ]; then
		for r in "${rows[@]}"; do
			row="${r%%|*}"
			b=$(awk -F'\t' -v k="$row" '$1==k{print $2}' "$BASE")
			[ -n "$b" ] || continue
			awk -v n="${now[$row]}" -v b="$b" -v t="$TOL" 'BEGIN{exit !(n > b*(1+t/100))}' \
				&& grew="$grew $row"
		done
	fi
	if [ -n "$grew" ] && [ -z "$reason" ]; then
		echo "parse_cost_gate: refusing to record growth in:$grew"
		echo "  (fix it, or --accept-increase='<why the extra work is the price of correctness>')"
		exit 1
	fi
	{
		printf '# parse-cost baseline (scripts/parse_cost_gate.sh): callgrind Ir per workload\n'
		printf '# toolchain\t%s\n' "$fp"
		printf '# recorded\t%s (the commit is the ledger row parse-cost)\n' "$(date -u +%Y-%m-%d)"
		[ -n "$reason" ] && printf '# accepted-increase\t%s\n' "$reason"
		for r in "${rows[@]}"; do
			row="${r%%|*}"
			printf '%s\t%s\n' "$row" "${now[$row]}"
		done
	} > "$BASE"
	echo "parse_cost_gate: recorded $BASE"
	cat "$BASE"
	exit 0
fi

[ -f "$BASE" ] || { echo "parse_cost_gate: no baseline — run with --record"; exit 1; }
if [ "$base_fp" != "$fp" ]; then
	echo "parse_cost_gate: REFUSED — baseline toolchain '$base_fp' != this host '$fp'"
	echo "  (re-record on the build host the battery runs on)"
	exit 1
fi
better=0
for r in "${rows[@]}"; do
	row="${r%%|*}"
	case "${now[$row]}" in FAIL*) continue ;; esac
	b=$(awk -F'\t' -v k="$row" '$1==k{print $2}' "$BASE")
	if [ -z "$b" ]; then
		echo "NEW  $row ${now[$row]} (not in the baseline — re-record)"
		red=$((red + 1))
		continue
	fi
	verdict=$(awk -v n="${now[$row]}" -v b="$b" -v t="$TOL" 'BEGIN{
		d = (n - b) * 100 / b
		printf "%s %+.2f%%", (d > t ? "RED " : (d < -t ? "GAIN" : "OK  ")), d }')
	printf '%s %-16s %15s  (baseline %s)\n' "${verdict%% *}" "$row" "${now[$row]}" "$b ${verdict#* }"
	case "$verdict" in RED*) red=$((red + 1)) ;; GAIN*) better=$((better + 1)) ;; esac
done
[ "$better" -gt 0 ] && echo "parse_cost_gate: $better workload(s) improved past ${TOL}% — re-record (--record) to keep the gain"
echo "parse_cost_gate: $red red of ${#rows[@]} (tolerance ${TOL}%)"
[ "$red" -eq 0 ]
