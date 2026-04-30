#!/bin/bash
# Run all integration tests.
#
# Convention: each .mad test carries its own fixtures, so this runner
# stays generic. For a test tests/foo.mad, the runner will use:
#   tests/foo.input  — redirected to stdin if present
#   tests/foo.argv   — each whitespace-separated word appended as argv
#   tests/foo.expect — if present, the test must exit 0 AND produce
#                      output that contains every line listed here as
#                      a substring. Otherwise exit-0 is enough.
#
# No test names are hard-coded here.
PASS=0
FAIL=0
TIMEOUTS=0
SKIP=0
for t in tests/*.mad; do
    base=$(basename "$t" .mad)
    [ "$base" = "include_helper" ] && continue

    input_file="tests/$base.input"
    argv_file="tests/$base.argv"
    expect_file="tests/$base.expect"

    args=()
    [ -f "$argv_file" ] && read -r -a args < "$argv_file"

    if [ -f "$input_file" ]; then
        out=$(timeout 5 bin/madc "$t" "${args[@]}" < "$input_file" 2>/dev/null)
    else
        out=$(timeout 5 bin/madc "$t" "${args[@]}" 2>/dev/null)
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
done
echo "$PASS passed, $FAIL failed, $TIMEOUTS timed out, $SKIP skipped"
[ $FAIL -eq 0 ] || exit 1
[ $TIMEOUTS -eq 0 ] || exit 1
