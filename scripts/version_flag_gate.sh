#!/usr/bin/env bash
# version_flag_gate.sh — `madc --version` reports the VERSION file, and the
# three spellings of the version agree.
#
# madc shipped for its whole life with no --version flag: the argument fell
# through to the source-file arm, so `madc --version` printed
# "--version:0:0: error: Failed to open file" and exited 1. It was found on a
# release artifact during the v0.82.0 macOS validation, and the way the mac
# battery had been asking — COMPILING a program that prints madc::sys.version —
# is the tell that the CLI had no answer.
#
# The version reaches three places from ONE source (../VERSION, threaded in as
# -DMADC_VERSION_STR; src/Makefile makes the version-consuming objects depend
# on that file). This gate asserts all three agree, because a bump that moves
# only some of them is the exact failure that dependency exists to prevent:
#   1. the CLI flag            madc --version
#   2. the preprocessor macro  MADC_VERSION
#   3. the runtime object      madc::sys.version
#
# Run from the repo root (fulltest does).
set -u
cd "$(dirname "$0")/.."

ulimit -t 120 2>/dev/null

BIN=${MADC_BIN:-bin/madc}
if [ ! -x "$BIN" ]; then
    echo "version_flag_gate: missing $BIN"
    exit 1
fi

WANT=$(cat VERSION 2>/dev/null)
if [ -z "$WANT" ]; then
    echo "version_flag_gate: VERSION file is empty or missing"
    exit 1
fi

fail() { echo "version_flag_gate: $1"; exit 1; }

# 1. The flag itself — both spellings, exit 0, first line "madc <version>".
for flag in --version -V; do
    out=$(timeout 60 "$BIN" "$flag" 2>&1); rc=$?
    [ $rc -eq 0 ] || fail "[$flag] exited $rc (output: $out)"
    first=$(printf '%s\n' "$out" | head -1)
    [ "$first" = "madc $WANT" ] \
        || fail "[$flag] printed '$first', VERSION says '$WANT'"
done

# 2 + 3. The macro and the runtime object, from a compiled program — the two
# other spellings, checked against the same VERSION.
mkdir -p tmp
cat > tmp/version_gate.mad <<'EOF'
#include <ns_madc>
#include <stdio.h>
int main()
{
    printf("macro=%s\n", MADC_VERSION);
    printf("sys=%s\n", madc::sys.version);
    return 0;
}
EOF
prog=$(timeout 120 "$BIN" tmp/version_gate.mad 2>&1); rc=$?
if [ $rc -ne 0 ]; then
    rm -f tmp/version_gate.mad
    fail "the MADC_VERSION / madc::sys.version probe failed (rc=$rc): $prog"
fi
rm -f tmp/version_gate.mad
got_macro=$(printf '%s\n' "$prog" | sed -n 's/^macro=//p')
got_sys=$(printf '%s\n' "$prog" | sed -n 's/^sys=//p')
[ "$got_macro" = "$WANT" ] \
    || fail "MADC_VERSION is '$got_macro', VERSION says '$WANT' (stale object?)"
[ "$got_sys" = "$WANT" ] \
    || fail "madc::sys.version is '$got_sys', VERSION says '$WANT' (stale object?)"

echo "version_flag_gate: OK — --version, -V, MADC_VERSION and madc::sys.version all report $WANT"
