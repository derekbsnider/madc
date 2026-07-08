#!/usr/bin/env bash
# Item-3 REAL-TEST SOAK (close-out handoff): freeze+bind EVERY tests/*.mad in
# ONE capped run; the oracle is the LIVE run of the same test (state
# equivalence), with rc + stdout compared. Failures are recorded per test with
# their first error line so the burn-down classifies by STATE FAMILY, never
# per test. Fixture conventions mirror scripts/run_tests.sh (.flags/.input/
# .argv/.timeout; include_helper + .mir_skip + .expect_err skipped).
set -u
cd "$(dirname "$0")/.."

BIN=bin/madc
OUT=tmp/soak
mkdir -p "$OUT"
rm -f "$OUT"/*.log "$OUT"/results.tsv

ulimit -t 7200 2>/dev/null

for t in tests/*.mad; do
    base=$(basename "$t" .mad)
    [ "$base" = "include_helper" ] && continue

    if [ -f "tests/$base.mir_skip" ]; then
        printf '%s\tSKIP_MIRSKIP\n' "$base" >> "$OUT/results.tsv"
        continue
    fi
    if [ -f "tests/$base.expect_err" ]; then
        # A compile-error test has no state to freeze.
        printf '%s\tSKIP_ERRTEST\n' "$base" >> "$OUT/results.tsv"
        continue
    fi

    args=()
    flags=()
    [ -f "tests/$base.flags" ] && read -r -a flags < "tests/$base.flags"
    [ -f "tests/$base.argv" ]  && read -r -a args  < "tests/$base.argv"
    tmo=10
    [ -f "tests/$base.timeout" ] && read -r tmo < "tests/$base.timeout"
    ftmo=$(( tmo * 10 + 120 ))	# freeze parses + serializes: generous cap
    btmo=$(( tmo * 3 + 60 ))
    input="tests/$base.input"
    snap="$OUT/$base.msnap"

    # 1. LIVE (the oracle)
    if [ -f "$input" ]; then
        live_out=$(timeout "$tmo" "$BIN" "${flags[@]}" "$t" "${args[@]}" < "$input" 2>/dev/null)
    else
        live_out=$(timeout "$tmo" "$BIN" "${flags[@]}" "$t" "${args[@]}" 2>/dev/null)
    fi
    live_rc=$?
    if [ $live_rc -eq 124 ]; then
        printf '%s\tLIVE_TIMEOUT\n' "$base" >> "$OUT/results.tsv"
        continue
    fi

    # 2. FREEZE
    if [ -f "$input" ]; then
        timeout "$ftmo" "$BIN" --freeze="$snap" "${flags[@]}" "$t" "${args[@]}" < "$input" > "$OUT/$base.freeze.log" 2>&1
    else
        timeout "$ftmo" "$BIN" --freeze="$snap" "${flags[@]}" "$t" "${args[@]}" > "$OUT/$base.freeze.log" 2>&1
    fi
    frc=$?
    if [ $frc -eq 124 ]; then
        printf '%s\tFREEZE_TIMEOUT\n' "$base" >> "$OUT/results.tsv"
        rm -f "$snap"; continue
    fi
    if [ ! -f "$snap" ]; then
        printf '%s\tFREEZE_FAIL\trc=%s\t%s\n' "$base" "$frc" \
            "$(grep -am1 'error' "$OUT/$base.freeze.log" | tr '\t' ' ')" >> "$OUT/results.tsv"
        continue
    fi

    # 3. BIND (compare rc + stdout to LIVE)
    if [ -f "$input" ]; then
        bind_out=$(timeout "$btmo" "$BIN" --forest-bind="$snap" "${flags[@]}" "$t" "${args[@]}" < "$input" 2> "$OUT/$base.bind.err")
    else
        bind_out=$(timeout "$btmo" "$BIN" --forest-bind="$snap" "${flags[@]}" "$t" "${args[@]}" 2> "$OUT/$base.bind.err")
    fi
    brc=$?
    if [ $brc -eq 124 ]; then
        printf '%s\tBIND_TIMEOUT\n' "$base" >> "$OUT/results.tsv"
        rm -f "$snap"; continue
    fi
    if [ $brc -ne $live_rc ]; then
        printf '%s\tBIND_RC\tlive=%s bind=%s\t%s\n' "$base" "$live_rc" "$brc" \
            "$(grep -am1 'error' "$OUT/$base.bind.err" | tr '\t' ' ')" >> "$OUT/results.tsv"
        rm -f "$snap"; continue
    fi
    if [ "$bind_out" != "$live_out" ]; then
        printf '%s\n' "$live_out" > "$OUT/$base.live.out"
        printf '%s\n' "$bind_out" > "$OUT/$base.bind.out"
        printf '%s\tBIND_DIFF\n' "$base" >> "$OUT/results.tsv"
        rm -f "$snap"; continue
    fi
    printf '%s\tOK\n' "$base" >> "$OUT/results.tsv"
    rm -f "$snap" "$OUT/$base.freeze.log" "$OUT/$base.bind.err"
done

echo "=== SOAK SUMMARY ==="
cut -f2 "$OUT/results.tsv" | sort | uniq -c | sort -rn
echo "=== NON-OK BY CLASS ==="
grep -v $'\tOK$' "$OUT/results.tsv" | sort -t$'\t' -k2 | head -80
