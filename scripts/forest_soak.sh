#!/usr/bin/env bash
# Item-3 REAL-TEST SOAK (close-out handoff): freeze+bind EVERY tests/*.mad in
# ONE capped run; the oracle is the LIVE run of the same test (state
# equivalence), with rc + stdout compared. Failures are recorded per test with
# their first error line so the burn-down classifies by STATE FAMILY, never
# per test. A test's fixtures are scripts/test_fixtures.sh's (the runner's
# reader); .helper + .mir_skip + .expect_err tests are skipped.
set -u
cd "$(dirname "$0")/.."
. scripts/test_fixtures.sh	# the ONE reader of a test's fixtures

BIN=bin/madc
OUT=tmp/soak
mkdir -p "$OUT"
rm -f "$OUT"/*.log "$OUT"/results.tsv

ulimit -t 7200 2>/dev/null

for t in tests/*.mad; do
    base=$(basename "$t" .mad)
    [ -f "tests/$base.helper" ] && continue

    if [ -f "tests/$base.mir_skip" ]; then
        printf '%s\tSKIP_MIRSKIP\n' "$base" >> "$OUT/results.tsv"
        continue
    fi
    if [ -f "tests/$base.expect_err" ]; then
        # A compile-error test has no state to freeze.
        printf '%s\tSKIP_ERRTEST\n' "$base" >> "$OUT/results.tsv"
        continue
    fi

    fixture_load tests "$base"
    tmo=$fx_timeout
    ftmo=$(( tmo * 10 + 120 ))	# freeze parses + serializes: generous cap
    btmo=$(( tmo * 3 + 60 ))
    # No .input: stdin is /dev/null, never the caller's terminal.
    input="${fx_input:-/dev/null}"
    snap="$OUT/$base.msnap"
    # Every leg runs the test as the suite does: its .env and the hermetic flag.
    run=(env "${fx_envs[@]}")
    madc=("$BIN" $FIXTURE_HERMETIC_FLAGS)

    # 1. LIVE (the oracle)
    live_out=$("${run[@]}" timeout "$tmo" "${madc[@]}" "${fx_flags[@]}" "$t" "${fx_args[@]}" < "$input" 2>/dev/null)
    live_rc=$?
    if [ $live_rc -eq 124 ]; then
        printf '%s\tLIVE_TIMEOUT\n' "$base" >> "$OUT/results.tsv"
        continue
    fi

    # 2. FREEZE
    "${run[@]}" timeout "$ftmo" "${madc[@]}" --freeze="$snap" "${fx_flags[@]}" "$t" "${fx_args[@]}" < "$input" > "$OUT/$base.freeze.log" 2>&1
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
    bind_out=$("${run[@]}" timeout "$btmo" "${madc[@]}" --forest-bind="$snap" "${fx_flags[@]}" "$t" "${fx_args[@]}" < "$input" 2> "$OUT/$base.bind.err")
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
