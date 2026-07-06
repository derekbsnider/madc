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

# --- case: bitfield (slice 3b) — named bitfield members reconstruct via
#     addBitField; a freeze-time round-trip layout check guards the ones we
#     can't rebuild (unnamed gaps), so what binds is always laid out right.
cat > tmp/fbgate_bitfield.h <<'EOF'
#ifndef FBGATE_BITFIELD_H
#define FBGATE_BITFIELD_H
struct Flags { unsigned a : 3; unsigned b : 5; int c; };
#endif
EOF
cat > tmp/fbgate_bitfield_producer.cpp <<'EOF'
#include <fbgate_bitfield.h>
int main() { struct Flags f; f.a = 0; return f.a; }
EOF
cat > tmp/fbgate_bitfield_consumer.cpp <<'EOF'
#include <fbgate_bitfield.h>
#include <cstdio>
int main()
{
    struct Flags f;
    f.a = 5;
    f.b = 20;
    f.c = 7;
    printf("bf=%u %u %d sz=%zu\n", f.a, f.b, f.c, sizeof(struct Flags));
    return 0;
}
EOF
run_case bitfield "bf=5 20 7 sz=8"

# --- case: class (slice 3c) — a non-polymorphic class hierarchy. Multiple
#     inheritance exercises a nonzero base subobject offset (B at +4) and an
#     upcast (`B *bp = &x`), so the reconstructed DataDefCLASS's members (verbatim,
#     inheritance-flattened) AND bases[] (offsets) must both be right. A / B are
#     promoted to classes by being used as bases; C is class-keyword. All three
#     bind from the type-table records with no header parse.
cat > tmp/fbgate_class.h <<'EOF'
#ifndef FBGATE_CLASS_H
#define FBGATE_CLASS_H
class A { public: int a; };
class B { public: int b; };
class C : public A, public B { public: int c; };
#endif
EOF
cat > tmp/fbgate_class_producer.cpp <<'EOF'
#include <fbgate_class.h>
int main() { C x; x.a = 0; return x.a; }
EOF
cat > tmp/fbgate_class_consumer.cpp <<'EOF'
#include <fbgate_class.h>
#include <cstdio>
int main()
{
    C x; x.a = 1; x.b = 2; x.c = 3;
    B *bp = &x;
    printf("sum=%d bb=%d sz=%zu\n", x.a + x.b + x.c, bp->b, sizeof(C));
    return 0;
}
EOF
run_case class "sum=6 bb=2 sz=12"

# --- case: method (inline-body save/load) — a class with INLINE method bodies.
#     The producer froze `Counter::get`/`add` bodies into the grove (Tree-1); bind
#     LOADS them (node_for deserialization, never re-parse) and emits them as
#     func-defs into the consumer's ONE module, exactly as a parsed inline method
#     would be. Proves the call links to a loaded body, not an undefined import.
cat > tmp/fbgate_method.h <<'EOF'
#ifndef FBGATE_METHOD_H
#define FBGATE_METHOD_H
class Counter {
public:
    int n;
    int get() { return n * 3; }
    void add(int d) { n += d; }
};
#endif
EOF
cat > tmp/fbgate_method_producer.cpp <<'EOF'
#include <fbgate_method.h>
int main() { Counter c; c.n = 0; c.add(1); return c.get(); }
EOF
cat > tmp/fbgate_method_consumer.cpp <<'EOF'
#include <fbgate_method.h>
#include <cstdio>
int main()
{
    Counter c;
    c.n = 0;
    c.add(5);
    printf("get=%d\n", c.get());
    return 0;
}
EOF
run_case method "get=15"

# --- case: fwd (transitive inline-body reachability) — an inline method whose
#     body calls a SIBLING method defined LATER in the class. Only `first` is
#     called by the consumer; `second` is reachable only transitively. The bind
#     emission must follow that reference (reachability fixpoint) and emit BOTH
#     bodies + their forward prototypes, or `second` is an undefined import / a
#     silent implicit-int miscompile. Guards the forward/mutual-reference path.
cat > tmp/fbgate_fwd.h <<'EOF'
#ifndef FBGATE_FWD_H
#define FBGATE_FWD_H
class Chain {
public:
    int n;
    int first() { return second() + 1; }
    int second() { return n * 10; }
};
#endif
EOF
cat > tmp/fbgate_fwd_producer.cpp <<'EOF'
#include <fbgate_fwd.h>
int main() { Chain c; c.n = 0; return c.first(); }
EOF
cat > tmp/fbgate_fwd_consumer.cpp <<'EOF'
#include <fbgate_fwd.h>
#include <cstdio>
int main() { Chain c; c.n = 4; printf("chain=%d\n", c.first()); return 0; }
EOF
run_case fwd "chain=41"

# --- case: ptr (pointer-member serialization) — a struct whose members are all
#     NON-pinned pointers: a scalar pointer (double*), a pointer to a SIBLING
#     aggregate (struct Point*), and a SELF-referential pointer (struct Node*).
#     None is a pinned pointer slot (void*/char*/int*), so before v9 the freeze
#     bailed on the first such member and dropped the whole struct — a bound member
#     access then failed "Unidentified member". v9 records each as a derived-type
#     record (CIR_TYPEK_POINTER, ref0 = pointee typeid) and the load fixpoint
#     reconstructs them (the self-reference resolves because Node is allocated
#     before its self-pointer). Proves pointer members bind cross-process, the
#     primitive the std::string / std::vector corpus classes were blocked on.
cat > tmp/fbgate_ptr.h <<'EOF'
#ifndef FBGATE_PTR_H
#define FBGATE_PTR_H
struct Point { int x; int y; };
struct Node { int v; double *dp; struct Node *next; struct Point *pp; };
#endif
EOF
cat > tmp/fbgate_ptr_producer.cpp <<'EOF'
#include <fbgate_ptr.h>
int main() { struct Node n; n.v = 0; n.dp = 0; n.next = 0; n.pp = 0; return n.v; }
EOF
cat > tmp/fbgate_ptr_consumer.cpp <<'EOF'
#include <fbgate_ptr.h>
#include <cstdio>
int main()
{
    struct Point p; p.x = 3; p.y = 4;
    struct Node a, b;
    double d = 2.5;
    a.v = 10; a.dp = &d; a.next = &b; a.pp = &p;
    b.v = 20; b.dp = 0; b.next = 0; b.pp = 0;
    printf("v=%d dv=%.1f nv=%d px=%d sz=%zu\n",
           a.v, *a.dp, a.next->v, a.pp->x, sizeof(struct Node));
    return 0;
}
EOF
run_case ptr "v=10 dv=2.5 nv=20 px=3 sz=32"

# --- case: ns (namespace-qualified type restoration) — a struct defined inside a
#     user namespace. Before v10 the loaded type registered ONLY in the flat
#     struct_map/datatype_map, so a bound `N::P` failed "Unknown namespace 'N'". v10
#     stamps each record's defining namespace (reverse-walked from
#     namespace_datatype_map at freeze, the verbatim source) and restore registers it
#     into namespace_map + namespace_datatype_map, so the qualified name resolves.
#     This is the primitive std::string / std::vector (namespace std) restoration
#     builds on — with the template-instantiation naming as the remaining follow-on.
cat > tmp/fbgate_ns.h <<'EOF'
#ifndef FBGATE_NS_H
#define FBGATE_NS_H
namespace N { struct P { int x; int y; }; }
#endif
EOF
cat > tmp/fbgate_ns_producer.cpp <<'EOF'
#include <fbgate_ns.h>
int main() { N::P p; p.x = 0; return p.x; }
EOF
cat > tmp/fbgate_ns_consumer.cpp <<'EOF'
#include <fbgate_ns.h>
#include <cstdio>
int main() { N::P p; p.x = 7; p.y = 9; printf("s=%d\n", p.x + p.y); return 0; }
EOF
run_case ns "s=16"

# --- case: anon (anonymous-aggregate serialization) — a struct with an ANONYMOUS
#     UNION member. addAnonymousAggregate flattens the union's members into the
#     parent for name lookup but keeps the grouping in anonymous_aggregates so
#     emission re-nests a real `union{..}`; the freeze used to DROP that grouping
#     (and skip the whole struct via has_anon_aggregate), so a naive bind laid the
#     flattened members out sequentially and the union overlap was lost — a SILENT
#     miscompile. v11 serializes anonymous_aggregates (nameless sub-aggregate as its
#     own record + a group slice) + the layout scalars, so the overlap is faithful.
#     The consumer WRITES via `i` and READS via `buf` (overlap) — a size-only check
#     would miss the bug; this asserts the actual shared storage.
cat > tmp/fbgate_anon.h <<'EOF'
#ifndef FBGATE_ANON_H
#define FBGATE_ANON_H
struct S { int tag; union { int i; char buf[8]; }; };
#endif
EOF
cat > tmp/fbgate_anon_producer.cpp <<'EOF'
#include <fbgate_anon.h>
int main() { struct S s; s.tag = 0; s.i = 0; return s.tag; }
EOF
cat > tmp/fbgate_anon_consumer.cpp <<'EOF'
#include <fbgate_anon.h>
#include <cstdio>
int main()
{
    struct S s;
    s.tag = 7;
    s.i = 0x41424344;			/* write via the union member i */
    printf("t=%d b0=%c b1=%c sz=%zu\n", s.tag, s.buf[0], s.buf[1], sizeof(struct S));
    return 0;
}
EOF
run_case anon "t=7 b0=D b1=C sz=12"

echo "forest_bind_gate: GREEN — typedef + struct + nested + bitfield + class + method + fwd + ptr + ns + anon grove headers bound (no re-parse), output == live == g++"
exit 0
