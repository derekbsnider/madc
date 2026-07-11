#!/bin/bash
# forest_phase_bench.sh — append one timing-trend row for the forest phase
# work (Phase-2 rungs 1-4), so "is it improving" is a tracked file rather
# than a per-sitting claim.
#
#   bash scripts/forest_phase_bench.sh [note]
#
# Measures tests/testsubscript.mad (the phase plan's headline test):
#   live  = bin/madc          (develop -O0 dev binary, live parse)
#   bound = bin/madc-release  (packed -O2 release binary, forest bind)
# 5 runs each, median wall seconds; consumer-side deferred-body derivations
# counted via MADC_MTI_PROBE. Appends to docs/perf/forest-timings.tsv.
# load1 records the 1-min load average at measurement time — the NAS is
# shared, so confounded rows must be visible in the trend, not hidden.
set -e
cd "$(dirname "$0")/.."

TEST=tests/testsubscript.mad
OUT=docs/perf/forest-timings.tsv
NOTE="${1:-}"
LIVE=bin/madc
BOUND=bin/madc-release

if [ ! -x "$LIVE" ]; then
    echo "forest_phase_bench: $LIVE missing — build with: make -C src" >&2
    exit 1
fi
if [ ! -x "$BOUND" ]; then
    echo "forest_phase_bench: $BOUND missing — build with: make -C src release" >&2
    exit 1
fi
if ! timeout 30 "$BOUND" --dump-forest 2>/dev/null | grep -q '^forest	units='; then
    echo "forest_phase_bench: $BOUND carries no forest blob — re-run: make -C src release" >&2
    exit 1
fi

mkdir -p "$(dirname "$OUT")" tmp

median5() {
    local bin="$1"
    local t=()
    local i s e
    for i in 1 2 3 4 5; do
        s=$(date +%s.%N)
        timeout 60 "$bin" "$TEST" > /dev/null 2>&1 || true
        e=$(date +%s.%N)
        t+=("$(awk -v a="$s" -v b="$e" 'BEGIN{printf "%.3f", b-a}')")
    done
    printf '%s\n' "${t[@]}" | sort -n | sed -n 3p
}

derive_count() {
    local bin="$1"
    MADC_MTI_PROBE=_ timeout 60 "$bin" "$TEST" > /dev/null 2> tmp/bench_derive.txt || true
    grep -c "MTIPROBE derive.*found=1" tmp/bench_derive.txt || true
}

if [ ! -f "$OUT" ]; then
    printf '# forest phase timing trend — one row per measurement (append-only).\n' > "$OUT"
    printf '# live = bin/madc (develop -O0, live parse); bound = bin/madc-release (packed -O2, forest bind).\n' >> "$OUT"
    printf '# Medians of 5 on tests/testsubscript.mad; derives = consumer parse_deferred_lazy_body count.\n' >> "$OUT"
    printf '# load1 = 1-min loadavg at measurement (shared NAS — high load rows are confounded).\n' >> "$OUT"
    printf 'date\tcommit\tlive_s\tbound_s\tlive_derives\tbound_derives\tload1\tnote\n' >> "$OUT"
fi

LIVE_S=$(median5 "$LIVE")
BOUND_S=$(median5 "$BOUND")
LIVE_D=$(derive_count "$LIVE")
BOUND_D=$(derive_count "$BOUND")
LOAD1=$(awk '{print $1}' /proc/loadavg)
DATE=$(date +%F)
COMMIT=$(git rev-parse --short HEAD 2>/dev/null)

printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$DATE" "$COMMIT" "$LIVE_S" "$BOUND_S" "$LIVE_D" "$BOUND_D" "$LOAD1" "$NOTE" \
    >> "$OUT"
tail -1 "$OUT"
