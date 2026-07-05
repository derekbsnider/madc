#!/usr/bin/env bash
# Phase 6 bind gate: --forest-bind LOADS a grove header instead of parsing it.
# For each case: freeze a container from a producer that includes ONLY the
# grove header, then in a FRESH process bind that container while compiling a
# consumer that USES the header's decl. The bound header is never tokenized —
# its decls resolve from the container's records (forest_restore_decls) and the
# corresponding node is reconstructed into the c2mir tree. The consumer also
# includes <cstdio>, which is NOT in the container and therefore still
# live-parses: only the frozen header binds.
#
# Cases:
#   typedef  (slice 2)  — `typedef int myint;`   resolves cross-process.
#   struct   (slice 3a) — `struct Point{int x,y;}` reconstructed (members +
#                          layout) and emitted; a system-segment type-id.
#
# Each case proves: bind output == live-parse output == g++ output; the include
# actually BOUND (a -v run shows "bound to grove unit", so a silent live
# fall-through cannot false-green it); and it works cross-process.
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

fail() { echo "forest_bind_gate: $1"; exit 1; }

# run_case <tag> <expected-output>
# Expects tmp/fbgate_<tag>.h / _producer.cpp / _consumer.cpp already written.
run_case() {
    tag="$1"; exp="$2"
    hdr="tmp/fbgate_${tag}.h"
    prod="tmp/fbgate_${tag}_producer.cpp"
    cons="tmp/fbgate_${tag}_consumer.cpp"
    snap="tmp/fbgate_${tag}.msnap"
    gccbin="tmp/fbgate_${tag}_gcc"
    vlog="tmp/fbgate_${tag}_v.log"

    # 1. Freeze the producer into a standalone container.
    if ! timeout 120 "$BIN" --freeze="$snap" "$prod" -I tmp >/dev/null 2>&1; then
        fail "[$tag] --freeze FAILED"
    fi
    [ -f "$snap" ] || fail "[$tag] --freeze produced no container"

    # 2. Oracles: live-parse madc and g++ must agree with each other first.
    live_out=$(timeout 60 "$BIN" "$cons" -I tmp 2>/dev/null)
    [ "$live_out" = "$exp" ] || fail "[$tag] live-parse output '$live_out' != '$exp'"
    if command -v g++ >/dev/null 2>&1; then
        if timeout 120 g++ -I tmp "$cons" -o "$gccbin" >/dev/null 2>&1; then
            gcc_out=$("$gccbin" 2>/dev/null)
            [ "$gcc_out" = "$exp" ] || fail "[$tag] g++ output '$gcc_out' != '$exp'"
        else
            fail "[$tag] g++ compile FAILED"
        fi
    fi

    # 3. Bind in a FRESH process — clean run for exact output equality.
    bind_out=$(timeout 60 "$BIN" --forest-bind="$snap" "$cons" -I tmp 2>/dev/null)
    [ "$bind_out" = "$exp" ] || fail "[$tag] bind output '$bind_out' != '$exp' (== g++)"

    # 4. Prove the include actually BOUND (no silent live fall-through). The -v
    #    trace prints "bound to grove unit N (<path>)" only when the grove path
    #    fires and skips tokenization. Grep a file (the -v dump carries NUL
    #    bytes, which a shell variable capture would strip with a warning).
    timeout 60 "$BIN" -v --forest-bind="$snap" "$cons" -I tmp >"$vlog" 2>/dev/null
    if ! grep -q "bound to grove unit.*fbgate_${tag}.h" "$vlog"; then
        rm -f "$snap" "$gccbin" "$vlog"
        fail "[$tag] consumer did NOT bind the grove header (live fall-through?)"
    fi

    rm -f "$snap" "$gccbin" "$vlog"
    echo "forest_bind_gate: [$tag] OK — grove header bound (no re-parse), output == live == g++"
}

# --- case: typedef (slice 2) -------------------------------------------------
cat > tmp/fbgate_typedef.h <<'EOF'
#ifndef FBGATE_TYPEDEF_H
#define FBGATE_TYPEDEF_H
typedef int myint;
#endif
EOF
cat > tmp/fbgate_typedef_producer.cpp <<'EOF'
#include <fbgate_typedef.h>
int main() { myint x = 0; return x; }
EOF
cat > tmp/fbgate_typedef_consumer.cpp <<'EOF'
#include <fbgate_typedef.h>
#include <cstdio>
int main() { myint x = 42; myint y = x + 3; printf("y=%d\n", y); return 0; }
EOF
run_case typedef "y=45"

# --- case: struct (slice 3a) — mixed-type padding + a union + sizeof, so the
#     reconstruction's layout (addMember/finalize) must match g++ byte-for-byte.
cat > tmp/fbgate_struct.h <<'EOF'
#ifndef FBGATE_STRUCT_H
#define FBGATE_STRUCT_H
struct Mix { char c; int i; double d; };
union Blob { int i; double d; char c; };
#endif
EOF
cat > tmp/fbgate_struct_producer.cpp <<'EOF'
#include <fbgate_struct.h>
int main() { struct Mix m; union Blob b; m.c = 0; b.i = 0; return 0; }
EOF
cat > tmp/fbgate_struct_consumer.cpp <<'EOF'
#include <fbgate_struct.h>
#include <cstdio>
int main()
{
    struct Mix m;
    m.c = 'A';
    m.i = 100;
    m.d = 2.5;
    printf("smix=%zu sblob=%zu c=%d i=%d\n",
           sizeof(struct Mix), sizeof(union Blob), m.c, m.i);
    return 0;
}
EOF
run_case struct "smix=16 sblob=8 c=65 i=100"

# --- case: nested (slice 3a reach) — a struct with a by-value struct member.
#     The freeze assigns Inner a system id, then Outer's `in` member references
#     it; restore's restored_by_sysid map links them (definition order), so a
#     value-aggregate member reconstructs without re-parse.
cat > tmp/fbgate_nested.h <<'EOF'
#ifndef FBGATE_NESTED_H
#define FBGATE_NESTED_H
struct Inner { int a; int b; };
struct Outer { struct Inner in; int c; };
#endif
EOF
cat > tmp/fbgate_nested_producer.cpp <<'EOF'
#include <fbgate_nested.h>
int main() { struct Outer o; o.c = 0; return o.c; }
EOF
cat > tmp/fbgate_nested_consumer.cpp <<'EOF'
#include <fbgate_nested.h>
#include <cstdio>
int main()
{
    struct Outer o;
    o.in.a = 2;
    o.in.b = 3;
    o.c = 4;
    printf("n=%d sz=%zu\n", o.in.a + o.in.b + o.c, sizeof(struct Outer));
    return 0;
}
EOF
run_case nested "n=9 sz=12"

echo "forest_bind_gate: GREEN — typedef + struct + nested grove headers bound (no re-parse), output == live == g++"
exit 0
