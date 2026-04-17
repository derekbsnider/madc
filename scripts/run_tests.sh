#!/bin/bash
# Run all integration tests. A few need explicit stdin or argv — driven
# by the case blocks below so we don't leave them on the skip list.
PASS=0
FAIL=0
SKIP=0
for t in tests/*.mad; do
    base=$(basename "$t")
    [ "$base" = "include_helper.mad" ] && continue

    case "$base" in
        testcin.mad)
            # testcin reads: string name, int age, then string a, string b.
            out=$(echo "Alice 42 hello world" | timeout 5 bin/madc "$t" 2>/dev/null)
            rc=$?
            if [ $rc -eq 0 ] && echo "$out" | grep -q "^name: Alice$" \
                             && echo "$out" | grep -q "^age: 42$" \
                             && echo "$out" | grep -q "^hello world$"; then
                PASS=$((PASS+1))
            else
                echo "FAIL: $t"
                FAIL=$((FAIL+1))
            fi
            continue
            ;;
        testargv.mad)
            out=$(timeout 5 bin/madc "$t" hello world 2>/dev/null)
            rc=$?
            if [ $rc -eq 0 ] && echo "$out" | grep -q "^argc: 3$" \
                             && echo "$out" | grep -q "^argv\[1\]: hello$" \
                             && echo "$out" | grep -q "^argv\[2\]: world$"; then
                PASS=$((PASS+1))
            else
                echo "FAIL: $t"
                FAIL=$((FAIL+1))
            fi
            continue
            ;;
    esac

    if timeout 5 bin/madc "$t" > /dev/null 2>&1; then
        PASS=$((PASS+1))
    else
        echo "FAIL: $t"
        FAIL=$((FAIL+1))
    fi
done
echo "$PASS passed, $FAIL failed, $SKIP skipped"
[ $FAIL -eq 0 ] || exit 1
