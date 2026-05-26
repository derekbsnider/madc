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
#
# No test names are hard-coded here.
#
# Options:
#   --exe   Also compile each test to a native executable and run it.
#           Failures are reported as "FAIL(exe): ..." separately.
RUN_EXE=0
BACKEND_FLAG=""
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
    flags_file="tests/$base.flags"

    args=()
    flags=()
    [ -f "$flags_file" ] && read -r -a flags < "$flags_file"
    [ -f "$argv_file" ] && read -r -a args < "$argv_file"

    if [ -f "$input_file" ]; then
        out=$(timeout 5 bin/madc $BACKEND_FLAG "${flags[@]}" "$t" "${args[@]}" < "$input_file" 2>/dev/null)
    else
        out=$(timeout 5 bin/madc $BACKEND_FLAG "${flags[@]}" "$t" "${args[@]}" 2>/dev/null)
    fi
    rc=$?

    ok=1
    timed_out=0
    if [ $rc -eq 124 ]; then
        ok=0
        timed_out=1
    elif [ $rc -ne 0 ]; then
        ok=0
    elif [ -f "$expect_file" ]; then
        while IFS= read -r line; do
            [ -z "$line" ] && continue
            # Each expected line must appear somewhere in the output.
            if ! grep -qF -- "$line" <<< "$out"; then
                ok=0
                break
            fi
        done < "$expect_file"
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
    if [ $RUN_EXE -eq 1 ] && [ $ok -eq 1 ]; then
        exe_path="/tmp/madc_test_exe_${base}"
        if bin/madc "${flags[@]}" -o "$exe_path" "$t" >/dev/null 2>&1; then
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
