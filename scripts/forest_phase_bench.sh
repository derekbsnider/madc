#!/bin/bash
# forest_phase_bench.sh — append one timing-trend row for the forest phase
# work (Phase-2 rungs 1-4), so "is it improving" is a tracked file rather
# than a per-sitting claim.
#
#   bash scripts/forest_phase_bench.sh [note]
#   bash scripts/forest_phase_bench.sh --compare <bin> [<bin> ...]
#
# --compare measures ARBITRARY binaries side by side and appends NOTHING to
# the trend files. The trend answers "is this tree improving over time"; the
# comparison answers "which of these binaries is faster, right now" — e.g. an
# archived tmp/release-bins/madc-release-vX.Y.Z against a candidate build.
# Both questions share ONE measurement implementation (metrics5 / ir_count /
# derive_count) so a methodology change lands in a single place; only the
# lane labelling differs, and the trend's lane names keep their meaning.
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
COMPARE=0
if [ "${1:-}" = "--compare" ]; then
    COMPARE=1
    shift
    [ $# -ge 1 ] || { echo "forest_phase_bench: --compare needs at least one binary" >&2; exit 2; }
    # The workload is overridable for a comparison ONLY. The trend's whole
    # value is that every row measures the same thing, so trend mode keeps
    # testsubscript nailed down; a comparison legitimately asks about other
    # workloads (a small script's startup latency answers a different
    # question than a template-heavy TU, and users feel the small one).
    TEST="${MADC_BENCH_TEST:-$TEST}"
    [ -f "$TEST" ] || { echo "forest_phase_bench: no such workload: $TEST" >&2; exit 2; }
fi
NOTE="${1:-}"
LIVE=bin/madc
BOUND=bin/madc-release

if [ "$COMPARE" = 0 ] && [ ! -x "$LIVE" ]; then
    echo "forest_phase_bench: $LIVE missing — build with: make -C src" >&2
    exit 1
fi
if [ "$COMPARE" = 0 ] && [ ! -x "$BOUND" ]; then
    echo "forest_phase_bench: $BOUND missing — build with: make -C src release" >&2
    exit 1
fi
if [ "$COMPARE" = 0 ] \
   && ! timeout 30 "$BOUND" --dump-forest 2>/dev/null | grep -q '^forest	units='; then
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

# rusage metrics for one lane: 5 runs, per-run getrusage(RUSAGE_CHILDREN)
# deltas. Prints: wall_s cpu_s maxrss_kb minflt majflt nivcsw (medians;
# maxrss is the max). Wall alone is untrustworthy on this shared host —
# neighbor contention changes IPC without touching guest loadavg — so the
# trend records the CPU cost, the memory high-water, and two per-row
# confounder flags (majflt = cold cache/IO; nivcsw = box was busy).
metrics5() {
    python3 - "$1" "$TEST" <<'PYEOF'
import resource, subprocess, sys, time
bin_, test = sys.argv[1], sys.argv[2]
rows = []
prev = resource.getrusage(resource.RUSAGE_CHILDREN)
for _ in range(5):
    t0 = time.monotonic()
    try:
        subprocess.run([bin_, test], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=60)
    except subprocess.TimeoutExpired:
        pass
    wall = time.monotonic() - t0
    cur = resource.getrusage(resource.RUSAGE_CHILDREN)
    rows.append((wall,
                 (cur.ru_utime - prev.ru_utime) + (cur.ru_stime - prev.ru_stime),
                 cur.ru_maxrss,
                 cur.ru_minflt - prev.ru_minflt,
                 cur.ru_majflt - prev.ru_majflt,
                 cur.ru_nivcsw - prev.ru_nivcsw))
    prev = cur
med = lambda xs: sorted(xs)[len(xs) // 2]
print("%.3f %.3f %d %d %d %d" % (
    med([r[0] for r in rows]), med([r[1] for r in rows]),
    max(r[2] for r in rows), med([r[3] for r in rows]),
    med([r[4] for r in rows]), med([r[5] for r in rows])))
PYEOF
}

# Deterministic work anchor: callgrind instruction count (Ir). Immune to
# host/box state entirely — the one field that answers "did the code get
# slower" across days. ~50x slowdown, so opt-in: MADC_BENCH_IR=1.
ir_count() {
    local bin="$1"
    if [ "${MADC_BENCH_IR:-0}" != "1" ]; then
        echo "-"
        return
    fi
    timeout 900 valgrind --tool=callgrind --callgrind-out-file=tmp/bench_cg.out \
        "$bin" "$TEST" > /dev/null 2>&1 || true
    awk '/^summary:/ {print $2; found=1} END {if (!found) print "-"}' tmp/bench_cg.out
}

if [ "$COMPARE" = 1 ]; then
    # One moment, N binaries, identical workload. Nothing is appended to the
    # trend files: these rows describe OTHER trees (an archived release, a
    # candidate build), and the trend's lane names mean one tree's two lanes.
    #
    # ir is the field that decides. Wall time on this host moves ±15% with
    # neighbour contention and no loadavg signal, so a wall-clock A/B can
    # invert a real difference; the callgrind instruction count cannot.
    # Run with MADC_BENCH_IR=1 whenever the answer matters.
    echo "# workload: $TEST"
    printf 'binary\twall_s\tcpu_s\tmaxrss_kb\tminflt\tmajflt\tnivcsw\tir\tderives\tunits\n'
    for bin in "$@"; do
        if [ ! -x "$bin" ]; then
            echo "forest_phase_bench: not executable: $bin" >&2
            exit 1
        fi
        units=$(timeout 30 "$bin" --dump-forest 2>/dev/null \
                | sed -n 's/^forest	units=\([0-9]*\).*/\1/p' | head -1)
        [ -n "$units" ] || units="-"
        printf '%s\t%s\t%s\t%s\n' \
            "$bin" "$(metrics5 "$bin" | tr ' ' '\t')" \
            "$(ir_count "$bin")" "$(derive_count "$bin")	$units"
    done
    exit 0
fi

if [ ! -f "$OUT" ]; then
    printf '# forest phase timing trend — one row per measurement (append-only).\n' > "$OUT"
    printf '# live = bin/madc (develop -O0, live parse); bound = bin/madc-release (packed -O2, forest bind).\n' >> "$OUT"
    printf '# Medians of 5 on tests/testsubscript.mad; derives = consumer parse_deferred_lazy_body count.\n' >> "$OUT"
    printf '# load1 = 1-min loadavg at measurement (shared NAS — high load rows are confounded).\n' >> "$OUT"
    printf 'date\tcommit\tlive_s\tbound_s\tlive_derives\tbound_derives\tload1\tnote\n' >> "$OUT"
fi

MOUT=docs/perf/forest-metrics.tsv
if [ ! -f "$MOUT" ]; then
    printf '# forest phase metrics trend — one row per lane per measurement (append-only).\n' > "$MOUT"
    printf '# lane: live = bin/madc (-O0, live parse); bound = bin/madc-release (packed -O2, forest bind).\n' >> "$MOUT"
    printf '# wall_s/cpu_s = median of 5 (cpu = child ru_utime+ru_stime); maxrss_kb = max of 5.\n' >> "$MOUT"
    printf '# minflt/majflt/nivcsw = per-run medians; majflt>0 or high nivcsw marks a confounded row.\n' >> "$MOUT"
    printf '# ir = callgrind instruction count (deterministic work anchor; "-" unless MADC_BENCH_IR=1).\n' >> "$MOUT"
    printf '# Wall alone is unreliable on this shared host: neighbor contention shifts IPC ±15%% with\n' >> "$MOUT"
    printf '# no guest loadavg signal (proven 2026-07-17: the 0.519 binary re-measured 0.620 at load 0.42).\n' >> "$MOUT"
    printf 'date\tcommit\tlane\twall_s\tcpu_s\tmaxrss_kb\tminflt\tmajflt\tnivcsw\tir\tload1\tnote\n' >> "$MOUT"
fi

LIVE_M=$(metrics5 "$LIVE")
BOUND_M=$(metrics5 "$BOUND")
LIVE_S=$(echo "$LIVE_M" | awk '{print $1}')
BOUND_S=$(echo "$BOUND_M" | awk '{print $1}')
LIVE_D=$(derive_count "$LIVE")
BOUND_D=$(derive_count "$BOUND")
LIVE_IR=$(ir_count "$LIVE")
BOUND_IR=$(ir_count "$BOUND")
LOAD1=$(awk '{print $1}' /proc/loadavg)
DATE=$(date +%F)
COMMIT=$(git rev-parse --short HEAD 2>/dev/null)

# Legacy wide row (schema unchanged — history stays one file, one format).
printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$DATE" "$COMMIT" "$LIVE_S" "$BOUND_S" "$LIVE_D" "$BOUND_D" "$LOAD1" "$NOTE" \
    >> "$OUT"
# Long-format metric rows, one per lane.
echo "$LIVE_M" | awk -v d="$DATE" -v c="$COMMIT" -v ir="$LIVE_IR" -v l="$LOAD1" -v n="$NOTE" \
    '{printf "%s\t%s\tlive\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n", d, c, $1, $2, $3, $4, $5, $6, ir, l, n}' \
    >> "$MOUT"
echo "$BOUND_M" | awk -v d="$DATE" -v c="$COMMIT" -v ir="$BOUND_IR" -v l="$LOAD1" -v n="$NOTE" \
    '{printf "%s\t%s\tbound\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n", d, c, $1, $2, $3, $4, $5, $6, ir, l, n}' \
    >> "$MOUT"
tail -1 "$OUT"
tail -2 "$MOUT"
