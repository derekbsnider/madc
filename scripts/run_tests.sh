#!/bin/bash
# Run all integration tests
PASS=0
FAIL=0
SKIP=0
for t in tests/*.mad; do
    base=$(basename "$t")
    [ "$base" = "include_helper.mad" ] && continue
    [ "$base" = "testcin.mad" ] && { SKIP=$((SKIP+1)); continue; }
    [ "$base" = "testargv.mad" ] && { SKIP=$((SKIP+1)); continue; }
    if timeout 5 bin/madc "$t" > /dev/null 2>&1; then
        PASS=$((PASS+1))
    else
        echo "FAIL: $t"
        FAIL=$((FAIL+1))
    fi
done
echo "$PASS passed, $FAIL failed, $SKIP skipped"
[ $FAIL -eq 0 ] || exit 1
