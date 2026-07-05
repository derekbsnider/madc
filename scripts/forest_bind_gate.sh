#!/usr/bin/env bash
# Phase 6 slice 2 gate: --forest-bind LOADS a grove header instead of parsing
# it. Freeze a container from a producer that includes ONLY a typedef header,
# then in a FRESH process bind that container while compiling a consumer that
# uses the header's typedef. The bound header is NOT tokenized — its `myint`
# resolves from the container's decl records (forest_restore_decls) and its
# `typedef int myint;` node is reconstructed into the c2mir tree. The consumer
# also includes <cstdio>, which is NOT in the container and therefore still
# live-parses: only the frozen header binds.
#
# The gate proves THREE things:
#   1. bind output == live-parse output == g++ output (correctness).
#   2. the include actually BOUND (a -v run shows "bound to grove unit"), so a
#      silent fall-through to live parse cannot pass this gate as a false green.
#   3. it works cross-process (freeze and bind are separate madc invocations).
#
# Run from the repo root (fulltest does). Fixtures are generated into tmp/
# (gitignored), like scripts/forest_pack.sh — the gate is self-contained.
set -u
cd "$(dirname "$0")/.."

ulimit -t 300 2>/dev/null

BIN=bin/madc
if [ ! -x "$BIN" ]; then
    echo "forest_bind_gate: missing $BIN"
    exit 1
fi

mkdir -p tmp
HDR=tmp/fbgate_helper.h
PRODUCER=tmp/fbgate_producer.cpp
CONSUMER=tmp/fbgate_consumer.cpp
SNAP=tmp/fbgate.msnap
GCCBIN=tmp/fbgate_gcc
EXP="y=45"

cat > "$HDR" <<'EOF'
/* forest_bind_gate fixture: a file-scope typedef whose underlying type is a
   primitive (its type-id is pinned, so it resolves cross-process). */
#ifndef FBGATE_HELPER_H
#define FBGATE_HELPER_H
typedef int myint;
#endif
EOF

# Producer: includes ONLY the typedef header, so the container's sole packed
# system unit is the header — <cstdio> is deliberately absent here.
cat > "$PRODUCER" <<'EOF'
#include <fbgate_helper.h>
int main() { myint x = 0; return x; }
EOF

# Consumer: uses the typedef AND prints via <cstdio> (which live-parses under
# --forest-bind, since it is not in the container).
cat > "$CONSUMER" <<'EOF'
#include <fbgate_helper.h>
#include <cstdio>
int main()
{
    myint x = 42;
    myint y = x + 3;
    printf("y=%d\n", y);
    return 0;
}
EOF

fail() { echo "forest_bind_gate: $1"; rm -f "$SNAP" "$GCCBIN"; exit 1; }

# 1. Freeze the producer into a standalone container.
if ! timeout 120 "$BIN" --freeze="$SNAP" "$PRODUCER" -I tmp >/dev/null 2>&1; then
    fail "--freeze FAILED"
fi
[ -f "$SNAP" ] || fail "--freeze produced no container"

# 2. Oracles: live-parse madc and g++ must agree with each other first.
live_out=$(timeout 60 "$BIN" "$CONSUMER" -I tmp 2>/dev/null)
[ "$live_out" = "$EXP" ] || fail "live-parse output '$live_out' != '$EXP'"

if command -v g++ >/dev/null 2>&1; then
    if timeout 120 g++ -I tmp "$CONSUMER" -o "$GCCBIN" >/dev/null 2>&1; then
        gcc_out=$("$GCCBIN" 2>/dev/null)
        [ "$gcc_out" = "$EXP" ] || fail "g++ output '$gcc_out' != '$EXP'"
    else
        fail "g++ compile FAILED"
    fi
fi

# 3. Bind in a FRESH process — clean run for exact output equality.
bind_out=$(timeout 60 "$BIN" --forest-bind="$SNAP" "$CONSUMER" -I tmp 2>/dev/null)
[ "$bind_out" = "$EXP" ] || fail "bind output '$bind_out' != '$EXP' (== g++)"

# 4. Prove the include actually BOUND (no silent live fall-through). The -v
#    trace prints "bound to grove unit N (<path>)" only when the grove path
#    fires and skips tokenization. Grep a file (the -v dump carries NUL bytes,
#    which a shell variable capture would strip with a warning).
VLOG=tmp/fbgate_verbose.log
timeout 60 "$BIN" -v --forest-bind="$SNAP" "$CONSUMER" -I tmp >"$VLOG" 2>/dev/null
if ! grep -q "bound to grove unit.*fbgate_helper.h" "$VLOG"; then
    rm -f "$VLOG"
    fail "consumer did NOT bind the grove header (live fall-through?)"
fi

rm -f "$SNAP" "$GCCBIN" "$VLOG"
echo "forest_bind_gate: GREEN — grove header bound (no re-parse), output == live == g++"
exit 0
