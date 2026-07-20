#!/bin/bash
# Run all integration tests.
#
# Convention: each .mad test carries its own fixtures, so this runner
# stays generic. For a test tests/foo.mad, the runner will use:
#   tests/foo.flags  — whitespace-split compiler flags prepended before
#                      the source path
#   tests/foo.input  — redirected to stdin if present
#   tests/foo.argv   — each whitespace-separated word appended as argv
#   tests/foo.expect — if present, the test must exit 0 AND produce
#                      output that contains every line listed here as
#                      a substring. Otherwise exit-0 is enough.
#   tests/foo.expect_err — marks a compile-error test: madc must exit
#                      NONZERO (not a timeout) and its diagnostics
#                      (stderr) must contain every line listed here as
#                      a substring. The EXE pass skips these — the
#                      source does not compile by design.
#   tests/foo.expect_quiet — if present (content ignored), the JIT run
#                      must produce EMPTY stderr — locks diagnostic
#                      hygiene (no leaked warnings/errors) for tests
#                      that compile real system headers.
#
# No test names are hard-coded here.
#
# Options:
#   --exe   Also compile each test to a native executable and run it.
#           Failures are reported as "FAIL(exe): ..." separately.
#
# MADC_BIN (env): the madc binary to test (default bin/madc). Generic
# runner capability — lets the suite run against e.g. a forest-packed
# copy (tmp/madc_packed) without touching the tree's binary.
RUN_EXE=0
BACKEND_FLAG=""
MADC="${MADC_BIN:-bin/madc}"
while [ $# -gt 0 ]; do
    case "$1" in
        --exe) RUN_EXE=1; shift ;;
        --backend=*) BACKEND_FLAG="$1"; shift ;;
        *) break ;;
    esac
done

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")"; pwd -P)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.."; pwd -P)
EXE_LD_LIBRARY_PATH="$REPO_ROOT/lib:/usr/local/lib"

PASS=0
FAIL=0
TIMEOUTS=0
SKIP=0
EXE_PASS=0
EXE_FAIL=0
for t in tests/*.mad; do
    base=$(basename "$t" .mad)
    [ "$base" = "include_helper" ] && continue

    input_file="tests/$base.input"
    argv_file="tests/$base.argv"
    expect_file="tests/$base.expect"
    expect_err_file="tests/$base.expect_err"
    expect_quiet_file="tests/$base.expect_quiet"
    flags_file="tests/$base.flags"
    mir_skip_file="tests/$base.mir_skip"
    timeout_file="tests/$base.timeout"

    # Skip tests marked as not transpilable (MIR is the default backend)
    if [ -f "$mir_skip_file" ]; then
        SKIP=$((SKIP+1))
        continue
    fi

    args=()
    flags=()
    [ -f "$flags_file" ] && read -r -a flags < "$flags_file"
    [ -f "$argv_file" ] && read -r -a args < "$argv_file"
    # Per-test wall-clock cap (seconds); default 10. A `.timeout` fixture overrides
    # it for tests that legitimately need longer — e.g. a real-libstdc++-header
    # compile (no PCH yet) parses the full <iostream> closure. The default build
    # is -O0 (optimization is a last-lap switch), so header-heavy compiles need
    # headroom. Generic filename convention, never a per-test branch in the runner.
    tmo=10
    [ -f "$timeout_file" ] && read -r tmo < "$timeout_file"

    if [ -f "$expect_err_file" ]; then
        # Compile-error test: capture stderr — the diagnostics ARE the
        # expected output.
        if [ -f "$input_file" ]; then
            out=$(timeout "$tmo" "$MADC" $BACKEND_FLAG "${flags[@]}" "$t" "${args[@]}" < "$input_file" 2>&1)
        else
            out=$(timeout "$tmo" "$MADC" $BACKEND_FLAG "${flags[@]}" "$t" "${args[@]}" 2>&1)
        fi
        rc=$?
        ok=1
        timed_out=0
        if [ $rc -eq 124 ]; then
            ok=0
            timed_out=1
        elif [ $rc -eq 0 ]; then
            ok=0
        else
            while IFS= read -r line; do
                [ -z "$line" ] && continue
                if ! grep -qF -- "$line" <<< "$out"; then
                    ok=0
                    break
                fi
            done < "$expect_err_file"
        fi
    else
        # stderr goes to a scratch file so an .expect_quiet fixture can
        # assert it is empty; without the fixture it is simply discarded.
        errf="/tmp/madc_test_stderr_${base}_$$"
        if [ -f "$input_file" ]; then
            out=$(timeout "$tmo" "$MADC" $BACKEND_FLAG "${flags[@]}" "$t" "${args[@]}" < "$input_file" 2>"$errf")
        else
            out=$(timeout "$tmo" "$MADC" $BACKEND_FLAG "${flags[@]}" "$t" "${args[@]}" 2>"$errf")
        fi
        rc=$?

        ok=1
        timed_out=0
        if [ $rc -eq 124 ]; then
            ok=0
            timed_out=1
        elif [ $rc -ne 0 ]; then
            ok=0
        else
            if [ -f "$expect_file" ]; then
                while IFS= read -r line; do
                    [ -z "$line" ] && continue
                    # Each expected line must appear somewhere in the output.
                    if ! grep -qF -- "$line" <<< "$out"; then
                        ok=0
                        break
                    fi
                done < "$expect_file"
            fi
            if [ $ok -eq 1 ] && [ -f "$expect_quiet_file" ] && [ -s "$errf" ]; then
                echo "NOISY(stderr): $t"
                ok=0
            fi
        fi
        rm -f "$errf"
    fi

    if [ $ok -eq 1 ]; then
        PASS=$((PASS+1))
    else
        if [ $timed_out -eq 1 ]; then
            echo "TIMEOUT: $t"
            TIMEOUTS=$((TIMEOUTS+1))
        else
            echo "FAIL: $t"
            FAIL=$((FAIL+1))
        fi
    fi

    # EXE pass: compile to native and run
    if [ $RUN_EXE -eq 1 ] && [ $ok -eq 1 ] && [ ! -f "$expect_err_file" ]; then
        exe_path="/tmp/madc_test_exe_${base}"
        # -o BEFORE the fixture flags: a positional .json manifest (project
        # auto-detect) ends madc's flag parsing — everything after it is the
        # program's argv, so a trailing -o would never reach madc.
        if "$MADC" -o "$exe_path" "${flags[@]}" "$t" >/dev/null 2>&1; then
            if [ -f "$input_file" ]; then
                exe_out=$(env LD_LIBRARY_PATH="$EXE_LD_LIBRARY_PATH" timeout 5 "$exe_path" "${args[@]}" < "$input_file" 2>/dev/null)
            else
                exe_out=$(env LD_LIBRARY_PATH="$EXE_LD_LIBRARY_PATH" timeout 5 "$exe_path" "${args[@]}" 2>/dev/null)
            fi
            exe_rc=$?
            exe_ok=1
            if [ $exe_rc -ne 0 ]; then
                exe_ok=0
            elif [ -f "$expect_file" ]; then
                while IFS= read -r line; do
                    [ -z "$line" ] && continue
                    if ! grep -qF -- "$line" <<< "$exe_out"; then
                        exe_ok=0
                        break
                    fi
                done < "$expect_file"
            fi
            if [ $exe_ok -eq 1 ]; then
                EXE_PASS=$((EXE_PASS+1))
            else
                echo "FAIL(exe): $t"
                EXE_FAIL=$((EXE_FAIL+1))
            fi
            rm -f "$exe_path"
        else
            echo "FAIL(exe-build): $t"
            EXE_FAIL=$((EXE_FAIL+1))
        fi
    fi
done
echo "$PASS passed, $FAIL failed, $TIMEOUTS timed out, $SKIP skipped"
if [ $RUN_EXE -eq 1 ]; then
    echo "EXE: $EXE_PASS passed, $EXE_FAIL failed (of $PASS JIT-passing tests)"
fi
[ $FAIL -eq 0 ] || exit 1
[ $TIMEOUTS -eq 0 ] || exit 1
