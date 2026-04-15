#!/bin/bash
# Run all integration tests
PASS=0
FAIL=0
for t in tests/*.mad; do
    base=$(basename "$t")
    [ "$base" = "include_helper.mad" ] && continue
    if timeout 5 bin/madc "$t" > /dev/null 2>&1; then
        echo "PASS: $t"
        PASS=$((PASS+1))
    else
        echo "FAIL: $t"
        FAIL=$((FAIL+1))
    fi
done
echo ""
echo "Results: $PASS passed, $FAIL failed"
