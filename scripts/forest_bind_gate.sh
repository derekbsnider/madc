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
# This gate is the verified bind's keeper (K2's positional walk re-evaluates
# each recorded conditional, docs/plans/2026-10-18-verified-cxx-bind.md); every
# consumer binds verified since K3.
if [ ! -x "$BIN" ]; then
    echo "forest_bind_gate: missing $BIN"
    exit 1
fi

mkdir -p tmp

fail() { echo "forest_bind_gate: $1"; exit 1; }

# The subject compiles in the madc dialect — the packed corpus's profile. The
# fixtures are .c / .cpp files g++ compiles too (the oracle), and they ran in
# the madc dialect until split arc 1A.4 let a source's extension pick gnu17 /
# gnu++17; the subject pins --std=madc so every leg tests what it was written
# for (a later --std= on a leg's own line would still win).
SUBJECT=tmp/fbgate_madc
printf '#!/bin/sh\nexec "%s" --std=madc "$@"\n' "$PWD/$BIN" > "$SUBJECT"
chmod +x "$SUBJECT"
BIN=$SUBJECT

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

# --- case: need (glibc __need protocol vs the freeze) --------------------------
# The producer's header protocol-includes <stdarg.h> (__need___va_list live,
# glibc stdio.h's idiom). The serving belongs to the INCLUDER's unit and the
# freeze must form NO unit named stdarg.h — a husk unit there would satisfy
# the consumer's PLAIN #include <stdarg.h> by name and lose va_list/va_start
# (the packed-lane regression of 2026-08-10: 25 varargs/ns_madc tests). The
# consumer proves both halves: fbgate_need_valist (the serving replayed with
# the bound includer) AND va_start (the plain include still gets the full
# header).
cat > tmp/fbgate_need.h <<'EOF'
#ifndef FBGATE_NEED_H
#define FBGATE_NEED_H
#define __need___va_list
#include <stdarg.h>
typedef __gnuc_va_list fbgate_need_valist;
#endif
EOF
cat > tmp/fbgate_need_producer.cpp <<'EOF'
#include <fbgate_need.h>
int main() { fbgate_need_valist *p = 0; (void)p; return 0; }
EOF
cat > tmp/fbgate_need_consumer.cpp <<'EOF'
#include <fbgate_need.h>
#include <stdarg.h>
#include <cstdio>
static int sum(int n, ...)
{
    va_list ap;
    va_start(ap, n);
    int s = 0;
    while (n--)
        s += va_arg(ap, int);
    va_end(ap);
    return s;
}
int main() { fbgate_need_valist *p = 0; (void)p; printf("s=%d\n", sum(3, 10, 20, 12)); return 0; }
EOF
run_case need "s=42"

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

# --- case: fnptrbody (forest-carriers S1) — `typedef struct Tag {...} Alias;`
#     whose body carries FUNCTION-POINTER members. The fnptr members route the
#     aggregate through the CLASS parser (TokenCLASS::parse), whose typedef
#     branch historically skipped user_typedef_names — so the freeze's flat-
#     typedef walk never emitted the alias's DK_TYPEDEF record and a bound
#     consumer lost the name while live parse resolved it. This is darwin's
#     FILE shape (`typedef struct __sFILE { ... int (*_close)(void *); ... }
#     FILE;`), caught by the first packed hosted-macOS binaries.
cat > tmp/fbgate_fnptrbody.h <<'EOF'
#ifndef FBGATE_FNPTRBODY_H
#define FBGATE_FNPTRBODY_H
typedef long fbg_pos_t;
typedef struct __fbgZ {
    int _r;
    int (* _close)(void *);
    fbg_pos_t (* _seek)(void *, fbg_pos_t, int);
} FBGZ;
#endif
EOF
cat > tmp/fbgate_fnptrbody_producer.cpp <<'EOF'
#include <fbgate_fnptrbody.h>
int main() { FBGZ z; z._r = 0; return z._r; }
EOF
cat > tmp/fbgate_fnptrbody_consumer.cpp <<'EOF'
#include <fbgate_fnptrbody.h>
#include <cstdio>
int main()
{
    FBGZ z;
    z._r = 41;
    z._close = 0;
    printf("r=%d ptrnull=%d\n", z._r + 1, z._close == 0);
    return 0;
}
EOF
run_case fnptrbody "r=42 ptrnull=1"

# --- case: memberptr (v53) — members whose type is a POINTER TO MEMBER: a union
#     holding `void (fbg_undefined::*)()` over a class that is never defined
#     (libstdc++'s _Nocopy_types, the buffer std::function sizes itself from),
#     and an `int fbg_point::*` data member pointer. Before v53 the freeze wrote
#     no record for either type, so the union and the struct holding it dropped
#     at load and a bound consumer lost both names; the data member pointer
#     also pinned as a plain `long`. Ubuntu 22.04's packed forest dropped
#     std::function's _Function_base this way (libstdc++ 11's <algorithm>
#     reaches <functional>).
cat > tmp/fbgate_memberptr.h <<'EOF'
#ifndef FBGATE_MEMBERPTR_H
#define FBGATE_MEMBERPTR_H
class fbg_undefined;
union fbg_nocopy {
    void *obj;
    void (*fn)();
    void (fbg_undefined::*memfn)();
};
struct fbg_point { int x; int y; };
struct fbg_holder {
    fbg_nocopy u;
    int fbg_point::*field;
    int k;
};
#endif
EOF
cat > tmp/fbgate_memberptr_producer.cpp <<'EOF'
#include <fbgate_memberptr.h>
int main() { fbg_holder h; h.k = 0; return h.k; }
EOF
cat > tmp/fbgate_memberptr_consumer.cpp <<'EOF'
#include <fbgate_memberptr.h>
#include <cstdio>
int main()
{
    fbg_point p = { 3, 4 };
    fbg_holder h;
    h.u.obj = 0;
    h.field = &fbg_point::y;
    h.k = 5;
    printf("sizes %d %d %d y=%d k=%d\n", (int)sizeof(fbg_nocopy), (int)alignof(fbg_nocopy),
           (int)sizeof(fbg_holder), p.*h.field, h.k);
    return 0;
}
EOF
run_case memberptr "sizes 16 8 32 y=4 k=5"

# --- case: unionclass — a UNION WITH MEMBER FUNCTIONS (a class with union
#     layout: libstdc++'s _Any_data) held by value in a struct (_Function_base).
#     The recordability cache fenced every union-layout class, so the union and
#     every aggregate holding it dropped at load and a bound consumer lost both
#     names; Ubuntu 22.04's packed forest dropped std::function's _Function_base
#     this way. The restore keeps the union's overlap: the members' recorded
#     offsets, verbatim, with the class's union layout.
cat > tmp/fbgate_unionclass.h <<'EOF'
#ifndef FBGATE_UNIONCLASS_H
#define FBGATE_UNIONCLASS_H
union fbg_anydata {
    void *access() { return &pod[0]; }
    const void *access() const { return &pod[0]; }
    long unused;
    char pod[16];
};
struct fbg_fnbase {
    fbg_anydata functor;
    int (*manager)(int);
    int tag;
};
#endif
EOF
cat > tmp/fbgate_unionclass_producer.cpp <<'EOF'
#include <fbgate_unionclass.h>
int main() { fbg_fnbase b; b.tag = 0; return b.tag; }
EOF
cat > tmp/fbgate_unionclass_consumer.cpp <<'EOF'
#include <fbgate_unionclass.h>
#include <cstdio>
#include <cstring>
int main()
{
    fbg_fnbase b;
    std::memset(&b, 0, sizeof b);
    int v = 7;
    std::memcpy(b.functor.access(), &v, sizeof v);
    b.tag = 3;
    printf("sizes %d %d first=%d unused=%ld tag=%d\n", (int)sizeof(fbg_anydata),
           (int)sizeof(fbg_fnbase), *(int *)b.functor.access(), b.functor.unused, b.tag);
    return 0;
}
EOF
run_case unionclass "sizes 16 32 first=7 unused=7 tag=3"

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

# --- case: nestedenumfn (task #64) — a CLASS-NESTED enum reached only through a
#     member's type chain. [basic.scope.class]/1 makes the tag a MEMBER, so a
#     live parse deliberately keeps it OUT of datatype_map and the namespace
#     (TokenENUM::parse — it used to leak money_base::part to file scope). The
#     freeze's enum walk read datatype_map, so a nested tag got NO DK_ENUM record
#     while forest_serialize_type_id had already stamped it a project id: the
#     fn-ptr signature's param then pointed at a DK_NONE slot, the fn-ptr never
#     rebuilt, and pass 2 SILENTLY dropped the whole owning aggregate. That is
#     std::ios_base's `event_callback *__fn_` — it took the entire iostream
#     family with it, so a bound darwin (libc++) `cout << "hi"` died on
#     `struct has no member __vptr`. Members flatten from bases, so one lost
#     member kills every derived class too.
#     The `int event` local is the other half of the gate: the tag must still
#     NOT be visible at file scope after the restore.
cat > tmp/fbgate_nestedenumfn.h <<'EOF'
#ifndef FBGATE_NESTEDENUMFN_H
#define FBGATE_NESTEDENUMFN_H
class Callbacks {
public:
    enum event { erase_event, imbue_event, copyfmt_event };
    typedef void (*event_callback)(event, Callbacks &, int);
    int n;
    event_callback *fn;
};
#endif
EOF
cat > tmp/fbgate_nestedenumfn_producer.cpp <<'EOF'
#include <fbgate_nestedenumfn.h>
int main() { Callbacks c; c.n = 0; c.fn = 0; return c.n; }
EOF
cat > tmp/fbgate_nestedenumfn_consumer.cpp <<'EOF'
#include <fbgate_nestedenumfn.h>
#include <cstdio>
int main()
{
    int event = 7;			/* the nested tag must NOT leak here */
    Callbacks c;
    c.n = 1;
    c.fn = 0;
    Callbacks::event e = Callbacks::imbue_event;
    printf("n=%d e=%d ev=%d sz=%zu\n", c.n, (int)e, event, sizeof(Callbacks));
    return 0;
}
EOF
run_case nestedenumfn "n=1 e=1 ev=7 sz=16"

# --- case: ldouble (task #63) — a struct with `long double` members. Slot 18
#     (MADC_TYPEID_LONG_DOUBLE) was left "reserved" after real long double
#     landed in 114b13a8, so ddLDOUBLE had no PINNED id and the freeze minted
#     it a PROJECT id no record walk writes — a DK_NONE cross-reference. Both
#     the scalar member and the pointer member then failed to swizzle at bind
#     and were dropped, so a bound `s.lo` reported `Unidentified member 'lo'`
#     while the identical live parse printed the g++/clang answer. The same
#     loss family as nestedenumfn above, on a PRIMITIVE instead of an enum:
#     found by scripts/forest_pack_gate.sh's first real run, which reported
#     `ptr long double* (operand kind=0)` in the load probe.
cat > tmp/fbgate_ldouble.h <<'EOF'
#ifndef FBGATE_LDOUBLE_H
#define FBGATE_LDOUBLE_H
struct fbgate_ld { long double lo; long double *pp; int tag; };
#endif
EOF
cat > tmp/fbgate_ldouble_producer.cpp <<'EOF'
#include <fbgate_ldouble.h>
int main() { struct fbgate_ld s; s.tag = 0; return s.tag; }
EOF
cat > tmp/fbgate_ldouble_consumer.cpp <<'EOF'
#include <fbgate_ldouble.h>
#include <cstdio>
int main() {
    long double x = 2.5L;
    struct fbgate_ld s;
    s.lo = 1.5L; s.pp = &x; s.tag = 7;
    printf("ld=%.1f %.1f %d %d\n", (double)s.lo, (double)*s.pp, s.tag,
           (int)sizeof(struct fbgate_ld));
    return 0;
}
EOF
run_case ldouble "ld=1.5 2.5 7 32"

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

# --- case: declonlymt (v34: DECL-ONLY member templates) — libstdc++'s
#     __do_common_type_impl::_S_test SFINAE idiom in miniature: a plain struct
#     with body-LESS static member function templates whose DEPENDENT return
#     type carries the answer, consumed via `using type = decltype(_S_pick<A,
#     B>(0))` inside a class template that privately inherits the impl. A
#     body-less member template retains no decl tokens, so pre-v34 the freeze
#     emitted no CIR_TMPLK_MEMBER record for it; the thawed placeholder had no
#     member_template_return_tokens and decltype fell to the implicit 64-bit
#     return — common_type<A,B>'s base materialized as int64_t (the packed-lane
#     testcommontype failure, task #31). The consumer instantiates FRESH
#     specializations (never frozen), forcing the thawed resolution path.
cat > tmp/fbgate_declonlymt.h <<'EOF'
#ifndef FBGATE_DECLONLYMT_H
#define FBGATE_DECLONLYMT_H
template<typename T> struct fbg_success { typedef T type; };
struct fbg_impl {
    template<typename T, typename U>
    static fbg_success<U> _S_pick(int);
    template<typename T, typename U>
    static fbg_success<T> _S_pick(...);
};
template<typename A, typename B>
struct fbg_common : private fbg_impl {
    using type = decltype(_S_pick<A, B>(0));
};
#endif
EOF
cat > tmp/fbgate_declonlymt_producer.cpp <<'EOF'
#include <fbgate_declonlymt.h>
int main() { fbg_common<int, long>::type r; (void)r; return 0; }
EOF
cat > tmp/fbgate_declonlymt_consumer.cpp <<'EOF'
#include <fbgate_declonlymt.h>
#include <cstdio>
int main()
{
    using R = fbg_common<char, short>::type;	/* fbg_success<short> */
    using V = R::type;				/* short */
    printf("r=%zu v=%zu\n", sizeof(R), sizeof(V));
    return 0;
}
EOF
run_case declonlymt "r=1 v=2"

# --- case: fwdalias — an ALIAS to a specialization of a template that is only
#     DECLARED at the point of the alias. libc++'s __fwd/sstream.h + __fwd/
#     fstream.h shape in miniature: all four stream aliases are spelled ahead
#     of the definitions in <sstream>, and a header-only producer never USES
#     them, so the alias binds to an opaque concrete mint that nothing ever
#     completes. Two independent defects met here, and each one alone loses the
#     name for every consumer of the container:
#       1. the mint had no arena record (no completion hook -> no project
#          type-id -> outside the freeze sweep's domain), so the namespaced
#          alias walk skipped the alias entirely while the decl INDEX still
#          listed it -> "use of undeclared identifier 'ostringstream'";
#       2. the husk cannot complete on the consumer side (the replay reads
#          dependent_shell_origin, pointer-keyed parse scratch that does not
#          serialize) -> "Unidentified member 'str'".
#     Live parse of the same header resolves both, which is what made this a
#     bind-only defect. It reached a SHIPPED product on darwin, whose grove set
#     is the only one frozen from libc++.
#     This case gates (1) ONLY: it uses the alias where an INCOMPLETE type is
#     legal (a pointer), which is exactly what the alias declaration itself
#     promises. Completing the husk is defect (2), still open — when it lands,
#     tighten this consumer to touch a member and re-baseline the expectation.
cat > tmp/fbgate_fwdalias.h <<'EOF'
#ifndef FBGATE_FWDALIAS_H
#define FBGATE_FWDALIAS_H
namespace fbg {
template <typename T, typename U = int> class fbg_holder;	/* declaration only */
using fbg_alias = fbg_holder<char>;				/* -> opaque mint */
template <typename T, typename U> class fbg_holder {		/* definition later */
public:
    U v;
    U get() const { return v; }
};
}
#endif
EOF
cat > tmp/fbgate_fwdalias_producer.cpp <<'EOF'
#include <fbgate_fwdalias.h>
int main() { return 0; }		/* deliberately never uses fbg_alias */
EOF
cat > tmp/fbgate_fwdalias_consumer.cpp <<'EOF'
#include <fbgate_fwdalias.h>
#include <cstdio>
int main()
{
    fbg::fbg_alias *p = 0;		/* the NAME must resolve */
    printf("p=%d\n", p == 0 ? 1 : 0);
    return 0;
}
EOF
run_case fwdalias "p=1"

# --- case: statmem — a BOUND class's STATIC DATA MEMBER resolves to the
#     LIBRARY's symbol. `std::use_facet<F>(loc)` odr-uses `F::id`, whose storage
#     lives in the stdlib, not in the program. Live records that Variable while
#     parsing the class body, with the alias its OWN owner derives
#     (class_static_member_itanium_symbol -> the nested _ZNSt3__15ctypeIcE2idE);
#     a bound class never parses its body, and the restore re-derived the alias
#     with the NAMESPACE-variable owner over the flat storage key `Tag__member`,
#     yielding one identifier component (_ZSt14ctype_char__id) that no library
#     exports. Symptom depended on where the reference came from: a restored
#     grove body named the real symbol nothing had declared ("undeclared
#     identifier _ZNSt3__15ctypeIcE2idE" at locale:378 — the owner's x86-64 Mac),
#     while a consumer-side instantiation named the invented one ("import of
#     undefined item _ZSt14ctype_char__id"). Fixed by TRANSPORTING the
#     producer's alias (format v39), so no derivation runs on the consumer side.
#     Runs under -stdlib=libc++ because that flavor's facets are the ones a
#     shipped forest binds (darwin's grove set is frozen from libc++); the
#     libstdc++ forest reaches the same code and must stay green too.
if printf 'int main(){return 0;}\n' > tmp/fbgate_statmem_probe.cpp \
   && timeout 60 "$BIN" -stdlib=libc++ tmp/fbgate_statmem_probe.cpp >/dev/null 2>&1; then
    sm_snap="tmp/fbgate_statmem.msnap"
    sm_prod="tmp/fbgate_statmem_producer.cpp"
    sm_cons="tmp/fbgate_statmem_consumer.cpp"
    sm_bin="tmp/fbgate_statmem_clang"
    sm_vlog="tmp/fbgate_statmem_v.log"
    sm_exp="up=A"
    cat > "$sm_prod" <<'EOF'
#include <locale>
int main() { return 0; }		/* header-only producer: never uses a facet */
EOF
    # <locale> first: its closure carries <cstdio>. A <cstdio>-first consumer
    # declines under the verified check — stdio.h's `#ifndef _VA_LIST_DEFINED`
    # typedef arm was skipped at freeze (missing content, K2 of
    # docs/plans/2026-10-18-verified-cxx-bind.md) — and a C++ TU's first
    # decline stops binding (K1a), so the static member would never bind here.
    cat > "$sm_cons" <<'EOF'
#include <locale>
#include <cstdio>
int main()
{
    std::locale loc;
    /* use_facet<F> odr-uses F::id — the class-scope static data member whose
       storage the LIBRARY defines. */
    const std::ctype<char> &ct = std::use_facet<std::ctype<char> >(loc);
    printf("up=%c\n", ct.toupper('a'));
    return 0;
}
EOF
    if ! timeout 600 "$BIN" -stdlib=libc++ --freeze="$sm_snap" "$sm_prod" >/dev/null 2>&1; then
        fail "[statmem] --freeze (-stdlib=libc++) FAILED"
    fi
    [ -f "$sm_snap" ] || fail "[statmem] --freeze produced no container"
    sm_live=$(timeout 120 "$BIN" -stdlib=libc++ "$sm_cons" 2>/dev/null)
    [ "$sm_live" = "$sm_exp" ] || fail "[statmem] live-parse output '$sm_live' != '$sm_exp'"
    if command -v clang++ >/dev/null 2>&1; then
        if timeout 120 clang++ -stdlib=libc++ "$sm_cons" -o "$sm_bin" >/dev/null 2>&1; then
            sm_or=$("$sm_bin" 2>/dev/null)
            [ "$sm_or" = "$sm_exp" ] || fail "[statmem] clang++ -stdlib=libc++ output '$sm_or' != '$sm_exp'"
        fi
    fi
    sm_out=$(timeout 120 "$BIN" -stdlib=libc++ --forest-bind="$sm_snap" "$sm_cons" 2>/dev/null)
    [ "$sm_out" = "$sm_exp" ] \
        || fail "[statmem] bind output '$sm_out' != '$sm_exp' (static-member alias re-derived?)"
    # No silent live fall-through: the grove must actually have bound.
    timeout 120 "$BIN" -v -stdlib=libc++ --forest-bind="$sm_snap" "$sm_cons" >"$sm_vlog" 2>/dev/null
    if ! grep -aq "bound to grove unit" "$sm_vlog"; then
        rm -f "$sm_snap" "$sm_bin" "$sm_vlog"
        fail "[statmem] consumer did NOT bind the grove (live fall-through?)"
    fi
    rm -f "$sm_snap" "$sm_bin" "$sm_vlog" "$sm_prod" "$sm_cons" tmp/fbgate_statmem_probe.cpp
    echo "forest_bind_gate: [statmem] OK — a bound class's static data member resolves to the library symbol, output == live == clang++"
else
    rm -f tmp/fbgate_statmem_probe.cpp
    echo "forest_bind_gate: [statmem] SKIP — this build has no libc++ flavor"
fi

# --- case: flavorgate (v34: the stdlib FLAVOR is producer-config identity) ---
# A container frozen under the build's default flavor (libstdc++) must NOT
# bind into a -stdlib=libc++ compile — the corpus carries the WRONG stdlib's
# headers (binding served libstdc++'s <stddef.h> and tripped libc++ <cstddef>'s
# #error: the packed-lane testcommontype_libcxx failure). The v27 gate's
# response to a config mismatch is a SILENT live fall-through: output stays
# correct AND the -v trace shows no grove binding. Skips when this build has
# no libc++ flavor.
if printf 'int main(){return 0;}\n' > tmp/fbgate_flavor_probe.cpp \
   && timeout 60 "$BIN" -stdlib=libc++ tmp/fbgate_flavor_probe.cpp >/dev/null 2>&1; then
    fl_snap="tmp/fbgate_flavor.msnap"
    fl_vlog="tmp/fbgate_flavor_v.log"
    cat > tmp/fbgate_flavor.cpp <<'EOF'
#include <cstddef>
#include <cstdio>
int main() { size_t n = 7; printf("n=%zu\n", n); return 0; }
EOF
    if ! timeout 600 "$BIN" --freeze="$fl_snap" tmp/fbgate_flavor.cpp >/dev/null 2>&1; then
        fail "[flavorgate] --freeze (default flavor) FAILED"
    fi
    fl_out=$(timeout 60 "$BIN" --forest-bind="$fl_snap" -stdlib=libc++ tmp/fbgate_flavor.cpp 2>/dev/null)
    [ "$fl_out" = "n=7" ] || fail "[flavorgate] -stdlib=libc++ consumer output '$fl_out' != 'n=7' (flavor-mismatched container bound?)"
    timeout 60 "$BIN" -v --forest-bind="$fl_snap" -stdlib=libc++ tmp/fbgate_flavor.cpp >"$fl_vlog" 2>/dev/null
    if grep -aq "bound to grove unit" "$fl_vlog"; then
        rm -f "$fl_snap" "$fl_vlog"
        fail "[flavorgate] a libstdc++-flavor container BOUND into a -stdlib=libc++ compile (config gate missed the flavor)"
    fi
    # Control: the SAME-flavor consumer still binds (the gate is a mismatch
    # gate, not a flavor kill-switch).
    timeout 60 "$BIN" -v --forest-bind="$fl_snap" tmp/fbgate_flavor.cpp >"$fl_vlog" 2>/dev/null
    if ! grep -aq "bound to grove unit" "$fl_vlog"; then
        rm -f "$fl_snap" "$fl_vlog"
        fail "[flavorgate] the SAME-flavor consumer did not bind (over-wide gate?)"
    fi
    rm -f "$fl_snap" "$fl_vlog" tmp/fbgate_flavor.cpp tmp/fbgate_flavor_probe.cpp
    echo "forest_bind_gate: [flavorgate] OK — flavor-mismatched container falls through to live parse; same-flavor still binds"
else
    rm -f tmp/fbgate_flavor_probe.cpp
    echo "forest_bind_gate: [flavorgate] SKIP — this build has no libc++ flavor"
fi

# --- case: sharegate (owner 2026-10-11: the madc profile serves the C++ modes) ---
# The release pack freezes once, under madc, and labels that one profile for
# gnu++17 and c++17 (--freeze-also-std=): the madc dialect is C++17 plus
# extras the --std= gate keeps out of a C++ compile. A C++ consumer binds the
# madc profile — and still gets NONE of madc's extras: no auto-include
# (println undeclared), no madc keyword (var), no auto-main (a top-level
# statement is an error). Control: the same probes compile under madc against
# the same profile, so each probe really exercises its extra. An unlabelled
# standard (c++20) does not bind.
cat > tmp/fbgate_share.cpp <<'EOF'
#include <cstddef>
#include <cstdio>
int main() { size_t n = 7; printf("n=%zu\n", n); return 0; }
EOF
printf '#include <cstdio>\nint main() { println("x"); return 0; }\n' > tmp/fbgate_share_println.cpp
printf '#include <cstdio>\nint main() { var x = 1; printf("%%d\\n", (int)x); return 0; }\n' > tmp/fbgate_share_var.cpp
printf '#include <cstdio>\nprintf("x\\n");\n' > tmp/fbgate_share_nomain.cpp
sh_snap=tmp/fbgate_share.msnap
sh_log=tmp/fbgate_share.log
rm -f "$sh_snap"
timeout 600 "$BIN" --freeze="$sh_snap" --freeze-also-std=gnu++17 --freeze-also-std=c++17 \
    tmp/fbgate_share.cpp >"$sh_log" 2>&1 || fail "[sharegate] the madc --freeze-also-std FAILED: $(tail -n 3 "$sh_log")"
sh_labels=$(timeout 60 "$BIN" --dump-forest="$sh_snap" | grep -c '^shared	')
[ "$sh_labels" = 2 ] || fail "[sharegate] the profile carries $sh_labels label(s), expected 2"
for sh_std in gnu++17 c++17; do
    sh_out=$(timeout 60 "$BIN" --std=$sh_std --forest-bind="$sh_snap" tmp/fbgate_share.cpp 2>/dev/null)
    [ "$sh_out" = "n=7" ] || fail "[sharegate] $sh_std consumer output '$sh_out' != 'n=7'"
    timeout 60 "$BIN" -v --std=$sh_std --forest-bind="$sh_snap" tmp/fbgate_share.cpp >"$sh_log" 2>/dev/null
    grep -aq "bound to grove unit" "$sh_log" \
        || fail "[sharegate] the $sh_std consumer did not bind the madc profile labelled for it"
done
timeout 60 "$BIN" -v --std=c++20 --forest-bind="$sh_snap" tmp/fbgate_share.cpp >"$sh_log" 2>/dev/null
if grep -aq "bound to grove unit" "$sh_log"; then
    fail "[sharegate] a c++20 consumer BOUND a profile not labelled for it"
fi
for sh_probe in println var nomain; do
    sh_src=tmp/fbgate_share_$sh_probe.cpp
    timeout 60 "$BIN" --forest-bind="$sh_snap" "$sh_src" >/dev/null 2>&1 \
        || fail "[sharegate] control: the madc compile of the $sh_probe probe failed (the probe does not exercise its extra)"
    timeout 60 "$BIN" -v --std=gnu++17 --forest-bind="$sh_snap" "$sh_src" >"$sh_log" 2>&1
    sh_rc=$?
    grep -aq "bound to grove unit" "$sh_log" \
        || fail "[sharegate] the gnu++17 $sh_probe probe never bound the profile (vacuous)"
    [ "$sh_rc" -ne 0 ] && [ "$sh_rc" -ne 124 ] \
        || fail "[sharegate] a gnu++17 compile bound to the madc profile got madc's $sh_probe extra (rc=$sh_rc)"
done
rm -f "$sh_snap" "$sh_log" tmp/fbgate_share.cpp tmp/fbgate_share_println.cpp \
      tmp/fbgate_share_var.cpp tmp/fbgate_share_nomain.cpp
echo "forest_bind_gate: [sharegate] OK — the madc profile binds for gnu++17 and c++17 with no madc extras; c++20 does not bind"

# --- case: rawgate (owner 2026-10-17: the C layer is "separate, uncompressed") ---
# --freeze-raw writes the profile with no codec — every segment, the MIR cache
# included — so a bind reads it in place. A .c consumer freezes raw under
# gnu17 (named: the subject wrapper pins madc, and a later --std= wins),
# the dump reports the profile with 0 codec segments, and a .c compile binds it
# with live-equal output. Control: the same freeze without --freeze-raw stores
# codec segments, so the dump's count is what the flag changed.
cat > tmp/fbgate_raw.c <<'EOF'
#include <stdio.h>
#include <string.h>
int main(void) { printf("len=%d\n", (int)strlen("raw")); return 0; }
EOF
rw_snap=tmp/fbgate_raw.msnap
rw_ctl=tmp/fbgate_raw_ctl.msnap
rw_log=tmp/fbgate_raw.log
rm -f "$rw_snap" "$rw_ctl"
timeout 600 "$BIN" --std=gnu17 --freeze="$rw_snap" --freeze-raw --freeze-mir-cache tmp/fbgate_raw.c >"$rw_log" 2>&1 \
    || fail "[rawgate] the --freeze-raw freeze FAILED: $(tail -n 3 "$rw_log")"
timeout 600 "$BIN" --std=gnu17 --freeze="$rw_ctl" --freeze-mir-cache tmp/fbgate_raw.c >"$rw_log" 2>&1 \
    || fail "[rawgate] control: the compressed freeze FAILED: $(tail -n 3 "$rw_log")"
rw_raw=$(timeout 60 "$BIN" --dump-forest="$rw_snap" | awk -F'\t' '$1 == "profile" { sub("compressed=", "", $5); print $5; exit }')
rw_cmp=$(timeout 60 "$BIN" --dump-forest="$rw_ctl" | awk -F'\t' '$1 == "profile" { sub("compressed=", "", $5); print $5; exit }')
[ "$rw_raw" = 0 ] || fail "[rawgate] the --freeze-raw profile stores '$rw_raw' codec segment(s), expected 0"
[ -n "$rw_cmp" ] && [ "$rw_cmp" -gt 0 ] \
    || fail "[rawgate] control: the compressed profile reports '$rw_cmp' codec segments (the dump cannot see the codec)"
rw_live=$(timeout 60 "$BIN" --std=gnu17 --no-forest-bind tmp/fbgate_raw.c 2>/dev/null)
[ "$rw_live" = "len=3" ] || fail "[rawgate] live output '$rw_live' != 'len=3'"
rw_out=$(timeout 60 "$BIN" --std=gnu17 --forest-bind="$rw_snap" tmp/fbgate_raw.c 2>/dev/null)
[ "$rw_out" = "$rw_live" ] || fail "[rawgate] raw-bound output '$rw_out' != live '$rw_live'"
timeout 60 "$BIN" -v --std=gnu17 --forest-bind="$rw_snap" tmp/fbgate_raw.c >"$rw_log" 2>/dev/null
grep -aq "bound to grove unit" "$rw_log" \
    || fail "[rawgate] the .c consumer did not bind the raw profile"
rm -f "$rw_snap" "$rw_ctl" "$rw_log" tmp/fbgate_raw.c
echo "forest_bind_gate: [rawgate] OK — a --freeze-raw profile stores 0 codec segments and a .c compile binds it, output == live"

# --- case: clayer (docs/plans/2026-10-18-c-pack-layer.md, J3) ---
# The release pack stacks its C layer (gnu17, raw) BELOW the madc profile, and
# only the madc profile carries the source arm (--freeze-sources=) and the AOT
# ledger (--freeze-ledger=). A .c compile binds the C layer and must still read
# a header the C layer lacks from the source arm, and link madc's runtime from
# the ledger — each from the profile that CARRIES it, never the bound one.
# The consumers name --std=gnu17 (the subject wrapper pins madc; a later
# --std= wins). Two programs, so each half is red on its own: a header that exists only in
# the pack (no file on disk), and a VLA under -static-libmadc (rt_vla).
cl_dir=tmp/fbgate_clayer
cl_stack=$cl_dir/stack.msnap
rm -rf "$cl_dir"
mkdir -p "$cl_dir"
cat > "$cl_dir/c_tu.c" <<'EOF'
#include <stdio.h>
int main(void) { return 0; }
EOF
cat > "$cl_dir/top_tu.mad" <<'EOF'
#include <stddef.h>
int main() { return 0; }
EOF
cat > "$cl_dir/only_in_pack.h" <<'EOF'
#define ONLY_IN_PACK 7
EOF
printf '/usr/include/madc_fbgate_only_in_pack.h\t%s\n' "$cl_dir/only_in_pack.h" \
    > "$cl_dir/sources.txt"
cat > "$cl_dir/src.c" <<'EOF'
#include <stdio.h>
#include <madc_fbgate_only_in_pack.h>
int main(void) { printf("pack=%d\n", ONLY_IN_PACK); return 0; }
EOF
cat > "$cl_dir/vla.c" <<'EOF'
#include <stdio.h>
static int total(int n)
{
	int a[n];
	int i, s = 0;
	for ( i = 0; i < n; i++ )
		a[i] = i * 7;
	for ( i = 0; i < n; i++ )
		s += a[i];
	return s;
}
int main(void) { printf("total=%d\n", total(4)); return 0; }
EOF
: > "$cl_stack"
timeout 600 "$BIN" --std=gnu17 --freeze-raw --freeze-append="$cl_stack" "$cl_dir/c_tu.c" \
    > "$cl_dir/freeze_c.log" 2>&1 \
    || fail "[clayer] the C layer freeze FAILED: $(tail -n 3 "$cl_dir/freeze_c.log")"
timeout 600 "$BIN" --std=madc "--freeze-sources=$cl_dir/sources.txt" \
    --freeze-ledger=src/rt/rt_vla.c --freeze-append="$cl_stack" "$cl_dir/top_tu.mad" \
    > "$cl_dir/freeze_top.log" 2>&1 \
    || fail "[clayer] the madc profile freeze FAILED: $(tail -n 3 "$cl_dir/freeze_top.log")"
cl_profiles=$(timeout 60 "$BIN" --dump-forest="$cl_stack" | awk -F'\t' '$1 == "profile"' | wc -l)
[ "$cl_profiles" -eq 2 ] || fail "[clayer] the stack holds $cl_profiles profile(s), expected 2"
# The source arm: the header lives only in the madc profile's source slot.
cl_out=$(timeout 60 "$BIN" --std=gnu17 --forest-bind="$cl_stack" "$cl_dir/src.c" 2>"$cl_dir/src.err")
[ "$cl_out" = "pack=7" ] \
    || fail "[clayer] a .c compile bound to the C layer did not read the source arm: '$cl_out' $(head -n 2 "$cl_dir/src.err")"
timeout 60 "$BIN" -v --std=gnu17 --forest-bind="$cl_stack" "$cl_dir/src.c" >"$cl_dir/src.log" 2>/dev/null
grep -aq "bound to grove unit" "$cl_dir/src.log" \
    || fail "[clayer] the .c compile did not bind the C layer"
# The ledger: rt_vla rides the madc profile above the bound C layer.
timeout 120 "$BIN" --std=gnu17 --forest-bind="$cl_stack" -static-libmadc -o "$cl_dir/vla" "$cl_dir/vla.c" \
    >"$cl_dir/vla.err" 2>&1 \
    || fail "[clayer] -static-libmadc refused a .c bound to the C layer: $(head -n 2 "$cl_dir/vla.err")"
cl_vla=$(timeout 60 "$cl_dir/vla" 2>/dev/null)
[ "$cl_vla" = "total=42" ] || fail "[clayer] the -static-libmadc binary printed '$cl_vla', expected 'total=42'"
rm -rf "$cl_dir"
echo "forest_bind_gate: [clayer] OK — a .c compile bound to a C layer below the madc profile reads that profile's source arm and links its ledger"

# --- case: division (docs/plans/2026-10-18-verified-cxx-bind.md, K0) ---
# Two packs (owner 2026-10-18): the C layer is C's alone; the mixed C/C++
# profile serves C++ and madc. "C++ should NEVER dip into the C-only pack" —
# by rule, not by the release stack's order. Here the C layer sits ON TOP, and
# each profile's source arm carries its own text for one header name. A C++
# compile that reads the header as text (no exact bind: --std=gnu++17 against
# a madc-frozen profile) must read the mixed profile's (WHICH=1), never the
# C layer's above it. The control: a C compile reads the C layer's (WHICH=2).
dv_dir=tmp/fbgate_division
dv_stack=$dv_dir/stack.msnap
rm -rf "$dv_dir"
mkdir -p "$dv_dir"
cat > "$dv_dir/cxx_text.h" <<'EOF'
#define WHICH 1
EOF
cat > "$dv_dir/c_text.h" <<'EOF'
#define WHICH 2
EOF
printf '/usr/include/madc_fbgate_division.h\t%s\n' "$dv_dir/cxx_text.h" > "$dv_dir/cxx_sources.txt"
printf '/usr/include/madc_fbgate_division.h\t%s\n' "$dv_dir/c_text.h" > "$dv_dir/c_sources.txt"
cat > "$dv_dir/top_tu.mad" <<'EOF'
#include <stddef.h>
int main() { return 0; }
EOF
cat > "$dv_dir/c_tu.c" <<'EOF'
#include <stddef.h>
int main(void) { return 0; }
EOF
cat > "$dv_dir/use.cpp" <<'EOF'
#include <stdio.h>
#include <madc_fbgate_division.h>
int main() { printf("which=%d\n", WHICH); return 0; }
EOF
cat > "$dv_dir/use.c" <<'EOF'
#include <stdio.h>
#include <madc_fbgate_division.h>
int main(void) { printf("which=%d\n", WHICH); return 0; }
EOF
: > "$dv_stack"
timeout 600 "$BIN" --std=madc "--freeze-sources=$dv_dir/cxx_sources.txt" \
    --freeze-append="$dv_stack" "$dv_dir/top_tu.mad" > "$dv_dir/freeze_top.log" 2>&1 \
    || fail "[division] the madc profile freeze FAILED: $(tail -n 3 "$dv_dir/freeze_top.log")"
timeout 600 "$BIN" --std=gnu17 --freeze-raw "--freeze-sources=$dv_dir/c_sources.txt" \
    --freeze-append="$dv_stack" "$dv_dir/c_tu.c" > "$dv_dir/freeze_c.log" 2>&1 \
    || fail "[division] the C layer freeze FAILED: $(tail -n 3 "$dv_dir/freeze_c.log")"
dv_profiles=$(timeout 60 "$BIN" --dump-forest="$dv_stack" | awk -F'\t' '$1 == "profile"' | wc -l)
[ "$dv_profiles" -eq 2 ] || fail "[division] the stack holds $dv_profiles profile(s), expected 2"
dv_c=$(timeout 60 "$BIN" --std=gnu17 --forest-bind="$dv_stack" "$dv_dir/use.c" 2>"$dv_dir/use_c.err")
[ "$dv_c" = "which=2" ] \
    || fail "[division] control: a C compile did not read the C layer's text above: '$dv_c' $(head -n 2 "$dv_dir/use_c.err")"
dv_cxx=$(timeout 60 "$BIN" --std=gnu++17 --forest-bind="$dv_stack" "$dv_dir/use.cpp" 2>"$dv_dir/use_cxx.err")
[ "$dv_cxx" = "which=1" ] \
    || fail "[division] a C++ compile read the C-only pack: '$dv_cxx' (want the mixed profile's which=1) $(head -n 2 "$dv_dir/use_cxx.err")"
rm -rf "$dv_dir"
echo "forest_bind_gate: [division] OK — with the C layer on top, a C++ compile reads only the mixed C/C++ profile; a C compile reads the C layer"

# --- case: allornone (docs/plans/2026-10-18-verified-cxx-bind.md, K1a) ---
# The restore installs each name a bound unit declares as the whole freeze
# defined it, ahead of every live token, and C++ resolves default arguments,
# class patterns and templates by name — so a C++ TU's system content is
# all-bound or all-live. fbgate_ab_b.h declines here (its FBG_AB_CFG arm was
# taken at freeze, under fbgate_ab_cfg.h) with nothing bound yet: binding
# stops for the TU, and fbgate_ab_a.h parses live too. Before: a.h bound over
# the live b.h, and the restore re-parsed a.h's default argument before b.h's
# fbg_ab_mode existed ("Unknown namespace or class 'fbg_ab_mode'" — <streambuf>
# over a live bits/ios_base.h in the knob-on packed suite). g++ and clang++:
# m=3 extra=20. Control: with fbgate_ab_cfg.h first nothing differs and a.h
# binds (m=3 extra=10).
ab_dir=tmp/fbgate_ab
rm -rf "$ab_dir"
mkdir -p "$ab_dir"
cat > "$ab_dir/fbgate_ab_cfg.h" <<'EOF'
#ifndef FBGATE_AB_CFG_H
#define FBGATE_AB_CFG_H
#define FBG_AB_CFG 1
#endif
EOF
cat > "$ab_dir/fbgate_ab_b.h" <<'EOF'
#ifndef FBGATE_AB_B_H
#define FBGATE_AB_B_H
struct fbg_ab_mode { enum { in = 1, out = 2 }; };
#ifdef FBG_AB_CFG
#define FBG_AB_EXTRA 10
#else
#define FBG_AB_EXTRA 20
#endif
#endif
EOF
cat > "$ab_dir/fbgate_ab_a.h" <<'EOF'
#ifndef FBGATE_AB_A_H
#define FBGATE_AB_A_H
#include <fbgate_ab_b.h>
struct fbg_ab_buf { int pick(int m = fbg_ab_mode::in | fbg_ab_mode::out) { return m; } };
#endif
EOF
cat > "$ab_dir/producer.cpp" <<'EOF'
#include <fbgate_ab_cfg.h>
#include <fbgate_ab_b.h>
#include <fbgate_ab_a.h>
int main() { return 0; }
EOF
cat > "$ab_dir/consumer.cpp" <<'EOF'
#include <fbgate_ab_b.h>
#include <fbgate_ab_a.h>
#include <stdio.h>
int main() { fbg_ab_buf b; printf("m=%d extra=%d\n", b.pick(), FBG_AB_EXTRA); return 0; }
EOF
cat > "$ab_dir/control.cpp" <<'EOF'
#include <fbgate_ab_cfg.h>
#include <fbgate_ab_a.h>
#include <stdio.h>
int main() { fbg_ab_buf b; printf("m=%d extra=%d\n", b.pick(), FBG_AB_EXTRA); return 0; }
EOF
ab_snap=$ab_dir/ab.msnap
timeout 600 "$BIN" --std=gnu++17 --freeze="$ab_snap" -I "$ab_dir" "$ab_dir/producer.cpp" \
    >"$ab_dir/freeze.log" 2>&1 || fail "[allornone] the --freeze FAILED: $(tail -n 3 "$ab_dir/freeze.log")"
ab_out=$(timeout 60 "$BIN" --std=gnu++17 --forest-bind="$ab_snap" -I "$ab_dir" "$ab_dir/consumer.cpp" 2>&1)
[ "$ab_out" = "m=3 extra=20" ] \
    || fail "[allornone] bound and live system content mixed: '$ab_out' (g++: 'm=3 extra=20')"
timeout 60 "$BIN" -v --std=gnu++17 --forest-bind="$ab_snap" -I "$ab_dir" "$ab_dir/consumer.cpp" >"$ab_dir/consumer.log" 2>/dev/null
grep -aq "binding stopped for this TU" "$ab_dir/consumer.log" \
    || fail "[allornone] the first decline did not stop binding for the C++ TU"
grep -aq "bound to grove unit.*fbgate_ab_a.h" "$ab_dir/consumer.log" \
    && fail "[allornone] fbgate_ab_a.h BOUND after the TU's first decline"
ab_ctl=$(timeout 60 "$BIN" --std=gnu++17 --forest-bind="$ab_snap" -I "$ab_dir" "$ab_dir/control.cpp" 2>&1)
[ "$ab_ctl" = "m=3 extra=10" ] || fail "[allornone] control output '$ab_ctl' != 'm=3 extra=10'"
timeout 60 "$BIN" -v --std=gnu++17 --forest-bind="$ab_snap" -I "$ab_dir" "$ab_dir/control.cpp" >"$ab_dir/control.log" 2>/dev/null
grep -aq "bound to grove unit.*fbgate_ab_a.h" "$ab_dir/control.log" \
    || fail "[allornone] control: fbgate_ab_a.h did not bind when nothing differs"
rm -rf "$ab_dir"
echo "forest_bind_gate: [allornone] OK — a C++ TU's first decline with nothing bound stops binding; with nothing different it binds"

# --- case: condrec (docs/plans/2026-10-18-verified-cxx-bind.md, K2, forest v68) ---
# The freeze records a unit's ITEM ORDER for the bind's positional walk: each
# edge's place among the unit's PP events, and each conditional it evaluated
# over an EXTERNAL macro — kind, outcome, single arm, the arm's events / edges /
# tokens, and a skipped single arm's body. fbgate_cr_u.h, frozen after
# fbgate_cr_cfg.h and fbgate_cr_pre.h: its guard (#ifndef, taken), #ifdef
# FBG_CR_CFG (taken, a prototype in the arm), #ifndef FBG_CR_GUARDED (skipped —
# pre.h defined it — its typedef arm captured), #if over FBG_CR_CFG with an
# #elif (taken, one define). The #elif after a taken arm is never evaluated.
cr_dir=tmp/fbgate_cr
rm -rf "$cr_dir"
mkdir -p "$cr_dir"
cat > "$cr_dir/fbgate_cr_cfg.h" <<'EOF'
#ifndef FBGATE_CR_CFG_H
#define FBGATE_CR_CFG_H
#define FBG_CR_CFG 1
#endif
EOF
cat > "$cr_dir/fbgate_cr_pre.h" <<'EOF'
#ifndef FBGATE_CR_PRE_H
#define FBGATE_CR_PRE_H
#define FBG_CR_GUARDED 9
#endif
EOF
cat > "$cr_dir/fbgate_cr_v.h" <<'EOF'
#ifndef FBGATE_CR_V_H
#define FBGATE_CR_V_H
#define FBG_CR_V 1
#endif
EOF
cat > "$cr_dir/fbgate_cr_u.h" <<'EOF'
#ifndef FBGATE_CR_U_H
#define FBGATE_CR_U_H
#define FBG_CR_A 1
#include <fbgate_cr_v.h>
#ifdef FBG_CR_CFG
int fbg_cr_on(void);
#endif
#ifndef FBG_CR_GUARDED
#define FBG_CR_GUARDED 2
typedef int fbg_cr_t;
#endif
#if defined FBG_CR_CFG && FBG_CR_A
#define FBG_CR_B 3
#elif 0
#define FBG_CR_B 4
#endif
#endif
EOF
cat > "$cr_dir/producer.c" <<'EOF'
#include <fbgate_cr_cfg.h>
#include <fbgate_cr_pre.h>
#include <fbgate_cr_u.h>
int main(void) { return 0; }
EOF
cr_snap=$cr_dir/cr.msnap
timeout 600 "$BIN" --std=gnu17 --freeze="$cr_snap" -I "$cr_dir" "$cr_dir/producer.c" \
    >"$cr_dir/freeze.log" 2>&1 || fail "[condrec] the --freeze FAILED: $(tail -n 3 "$cr_dir/freeze.log")"
timeout 60 "$BIN" --dump-forest="$cr_snap" > "$cr_dir/dump.txt" 2>&1 \
    || fail "[condrec] --dump-forest FAILED"
cr_u='[^	]*fbgate_cr_u\.h'
cr_want() {	# cr_want <label> <ERE over a dump line>
    grep -aEq "$2" "$cr_dir/dump.txt" \
	|| fail "[condrec] $1 — no dump line matches: $2"
}
cr_want "the edge to v.h sits after u.h's two defines" "^edgepos	${cr_u}	0	2$"
cr_want "the guard #ifndef: taken, single arm, the whole unit" \
    "^cond	${cr_u}	ifndef	1	1	ev=0-3	ed=0-1	arm=none	FBGATE_CR_U_H$"
cr_want "#ifdef FBG_CR_CFG: taken, single arm, no events" \
    "^cond	${cr_u}	ifdef	1	1	ev=2-2	ed=1-1	arm=none	FBG_CR_CFG$"
cr_want "#ifndef FBG_CR_GUARDED: skipped, single arm, its typedef arm captured" \
    "^cond	${cr_u}	ifndef	0	1	ev=2-2	ed=1-1	arm=captured	FBG_CR_GUARDED$"
cr_want "#if over FBG_CR_CFG: taken, an #elif follows, one define" \
    "^cond	${cr_u}	if	1	0	ev=2-3	ed=1-1	arm=none	defined FBG_CR_CFG && FBG_CR_A$"
grep -aEq "^cond	${cr_u}	elif	" "$cr_dir/dump.txt" \
    && fail "[condrec] an #elif after a taken arm was recorded — it is never evaluated"
rm -rf "$cr_dir"
echo "forest_bind_gate: [condrec] OK — the freeze records each edge's place and each external conditional's outcome, arm and body (v68)"

# --- case: k2walk (docs/plans/2026-10-18-verified-cxx-bind.md, K2 step 3) ---
# The bind walks a root's closure in the freeze's item order and re-evaluates
# each recorded conditional in the consumer's macro state at that position.
# One freeze, each shape frozen with an external definer the consumer lacks —
# the three the knob-on packed suite declined on, plus the walk's undo:
#   pos   — kw_c.h's `#if !defined FBG_KW_A && !defined FBG_KW_B` #error was
#           frozen under kw_a.h; reached through kw_b.h it holds by position
#           (glibc bits/setjmp.h under pthread.h). kw_b.h binds.
#   dirs  — kw_w.h's directive-only `#ifndef FBG_KW_MIN` arm, writing two
#           names, was skipped (kw_lim.h first); the walk runs it (wchar.h's
#           WCHAR_MIN). kw_lim.h spells FBG_KW_MIN differently (`(0+1)`), but
#           no frozen token after the arm expands it, so the state difference
#           is inert. kw_w.h binds.
#   expanded — kw_x.h's `#ifndef FBG_KW_XV` arm was skipped (kw_xdef.h's
#           FBG_KW_XV 1 first) and kw_x.h's own frozen token after the arm
#           expanded that 1; here the arm makes it 2, so the bound token would
#           be wrong: kw_x.h declines and parses live.
#   decls — kw_time.h's `#ifndef FBG_KW_PID_DEFINED` typedef arm was skipped
#           (kw_types.h first); the walk runs its directives and keeps its
#           typedef (time.h's __pid_t_defined). kw_time.h binds.
#   undo  — kw_r.h's walk undefines the consumer's FBG_KW_X, then kw_s.h's
#           two-arm FBG_KW_CFG group flips and the root declines: the undo
#           log restores FBG_KW_X before the live parse.
# g++ and clang++: c=3 b=4 / min=1 max=9 / pid=6 / xv=2 / saw=1 s=20.
kw_dir=tmp/fbgate_kw
rm -rf "$kw_dir"
mkdir -p "$kw_dir"
cat > "$kw_dir/fbgate_kw_c.h" <<'EOF'
#ifndef FBGATE_KW_C_H
#define FBGATE_KW_C_H
#if !defined FBG_KW_A && !defined FBG_KW_B
# error "include fbgate_kw_c.h through fbgate_kw_a.h or fbgate_kw_b.h"
#endif
struct fbg_kw_c { int v; };
#define FBG_KW_C 3
#endif
EOF
cat > "$kw_dir/fbgate_kw_a.h" <<'EOF'
#ifndef FBGATE_KW_A_H
#define FBGATE_KW_A_H
#define FBG_KW_A 1
#include <fbgate_kw_c.h>
#endif
EOF
cat > "$kw_dir/fbgate_kw_b.h" <<'EOF'
#ifndef FBGATE_KW_B_H
#define FBGATE_KW_B_H
#define FBG_KW_B 1
#include <fbgate_kw_c.h>
#define FBG_KW_BV 4
#endif
EOF
cat > "$kw_dir/fbgate_kw_lim.h" <<'EOF'
#ifndef FBGATE_KW_LIM_H
#define FBGATE_KW_LIM_H
#define FBG_KW_MIN (0+1)
#define FBG_KW_MAX 9
#endif
EOF
cat > "$kw_dir/fbgate_kw_w.h" <<'EOF'
#ifndef FBGATE_KW_W_H
#define FBGATE_KW_W_H
struct fbg_kw_w { int v; };
#ifndef FBG_KW_MIN
/* These might also be defined in fbgate_kw_lim.h.  */
# define FBG_KW_MIN 1
# define FBG_KW_MAX 9
#endif
#endif
EOF
cat > "$kw_dir/fbgate_kw_types.h" <<'EOF'
#ifndef FBGATE_KW_TYPES_H
#define FBGATE_KW_TYPES_H
#ifndef FBG_KW_PID_DEFINED
typedef int fbg_kw_pid;
# define FBG_KW_PID_DEFINED
#endif
#endif
EOF
cat > "$kw_dir/fbgate_kw_time.h" <<'EOF'
#ifndef FBGATE_KW_TIME_H
#define FBGATE_KW_TIME_H
struct fbg_kw_tm { int v; };
#ifndef FBG_KW_PID_DEFINED
typedef int fbg_kw_pid;
# define FBG_KW_PID_DEFINED
#endif
#endif
EOF
cat > "$kw_dir/fbgate_kw_cfg.h" <<'EOF'
#ifndef FBGATE_KW_CFG_H
#define FBGATE_KW_CFG_H
#define FBG_KW_CFG 1
#endif
EOF
cat > "$kw_dir/fbgate_kw_s.h" <<'EOF'
#ifndef FBGATE_KW_S_H
#define FBGATE_KW_S_H
struct fbg_kw_s { int v; };
#ifdef FBG_KW_CFG
#define FBG_KW_SV 10
#else
#define FBG_KW_SV 20
#endif
#endif
EOF
cat > "$kw_dir/fbgate_kw_r.h" <<'EOF'
#ifndef FBGATE_KW_R_H
#define FBGATE_KW_R_H
#ifdef FBG_KW_X
#define FBG_KW_SAW 1
#else
#define FBG_KW_SAW 0
#endif
#undef FBG_KW_X
#include <fbgate_kw_s.h>
#endif
EOF
cat > "$kw_dir/fbgate_kw_xdef.h" <<'EOF'
#ifndef FBGATE_KW_XDEF_H
#define FBGATE_KW_XDEF_H
#define FBG_KW_XV 1
#endif
EOF
cat > "$kw_dir/fbgate_kw_x.h" <<'EOF'
#ifndef FBGATE_KW_X_H
#define FBGATE_KW_X_H
#ifndef FBG_KW_XV
# define FBG_KW_XV 2
#endif
static const int fbg_kw_xv = FBG_KW_XV;
#endif
EOF
cat > "$kw_dir/producer.cpp" <<'EOF'
#include <fbgate_kw_xdef.h>
#include <fbgate_kw_x.h>
#include <fbgate_kw_a.h>
#include <fbgate_kw_b.h>
#include <fbgate_kw_lim.h>
#include <fbgate_kw_w.h>
#include <fbgate_kw_types.h>
#include <fbgate_kw_time.h>
#define FBG_KW_X 1
#include <fbgate_kw_cfg.h>
#include <fbgate_kw_r.h>
int main() { return 0; }
EOF
cat > "$kw_dir/pos.cpp" <<'EOF'
#include <fbgate_kw_b.h>
#include <stdio.h>
int main() { fbg_kw_c c = { FBG_KW_C }; printf("c=%d b=%d\n", c.v, FBG_KW_BV); return 0; }
EOF
cat > "$kw_dir/dirs.cpp" <<'EOF'
#include <fbgate_kw_w.h>
#include <stdio.h>
int main() { printf("min=%d max=%d\n", FBG_KW_MIN, FBG_KW_MAX); return 0; }
EOF
cat > "$kw_dir/decls.cpp" <<'EOF'
#include <fbgate_kw_time.h>
#include <stdio.h>
int main() { fbg_kw_pid p = 6; printf("pid=%d\n", (int)p); return 0; }
EOF
cat > "$kw_dir/expanded.cpp" <<'EOF'
#include <fbgate_kw_x.h>
#include <stdio.h>
int main() { printf("xv=%d\n", fbg_kw_xv); return 0; }
EOF
cat > "$kw_dir/undo.cpp" <<'EOF'
#define FBG_KW_X 1
#include <fbgate_kw_r.h>
#include <stdio.h>
int main() { printf("saw=%d s=%d\n", FBG_KW_SAW, FBG_KW_SV); return 0; }
EOF
kw_snap=$kw_dir/kw.msnap
timeout 600 "$BIN" --std=gnu++17 --freeze="$kw_snap" -I "$kw_dir" "$kw_dir/producer.cpp" \
    >"$kw_dir/freeze.log" 2>&1 || fail "[k2walk] the --freeze FAILED: $(tail -n 3 "$kw_dir/freeze.log")"
kw_case() {	# kw_case <name> <expected output> <unit that binds, or "-" for a decline> <declining unit>
    if command -v g++ >/dev/null 2>&1; then
	g++ -std=gnu++17 -I "$kw_dir" -o "$kw_dir/$1.gxx" "$kw_dir/$1.cpp" 2>"$kw_dir/$1.gxx.err" \
	    || fail "[k2walk] $1: g++ compile FAILED: $(head -n 2 "$kw_dir/$1.gxx.err")"
	[ "$("$kw_dir/$1.gxx")" = "$2" ] || fail "[k2walk] $1: g++ output is not '$2' — the reducer is wrong"
    fi
    kw_out=$(timeout 60 "$BIN" --std=gnu++17 --forest-bind="$kw_snap" -I "$kw_dir" "$kw_dir/$1.cpp" 2>&1)
    [ "$kw_out" = "$2" ] || fail "[k2walk] $1: output '$kw_out' != '$2' (g++)"
    timeout 60 "$BIN" -v --std=gnu++17 --forest-bind="$kw_snap" -I "$kw_dir" "$kw_dir/$1.cpp" \
	>"$kw_dir/$1.log" 2>/dev/null
    if [ "$3" != "-" ]; then
	grep -aq "bound to grove unit.*$3" "$kw_dir/$1.log" \
	    || fail "[k2walk] $1: $3 did not bind — $(grep -a -m1 'DECLINE root' "$kw_dir/$1.log")"
    else
	grep -aq "DECLINE root.*$4" "$kw_dir/$1.log" \
	    || fail "[k2walk] $1: $4 did not decline — the undo was never exercised"
    fi
}
kw_case pos "c=3 b=4" "fbgate_kw_b.h"
kw_case dirs "min=1 max=9" "fbgate_kw_w.h"
kw_case decls "pid=6" "fbgate_kw_time.h"
kw_case expanded "xv=2" "-" "fbgate_kw_x.h"
kw_case undo "saw=1 s=20" "-" "fbgate_kw_r.h"
rm -rf "$kw_dir"
echo "forest_bind_gate: [k2walk] OK — the bind re-evaluates each frozen conditional at its position: an equal outcome binds, a skipped arm runs (directives in place, declarations kept), a decline undoes the walk"

# --- case: k2c (docs/plans/2026-10-18-verified-cxx-bind.md, K2c) ---
# A run arm that leaves a macro unlike the freeze's no longer declines by
# itself: the name joins the TU's differs-set and is checked wherever frozen
# content expands it, in this root and every later one.
#   restore  — k2c_own.h's `#ifndef FBG_K2C_N` arm was skipped (k2c_def.h's
#            FBG_K2C_N 7 came first); reached through k2c_top.h the walk runs
#            it (5), then k2c_top.h's next edge binds k2c_def.h's first
#            inclusion, which sets 7 again before k2c_top.h's frozen token
#            expands it (mingw wchar.h's `NULL 0LL`, then stddef.h). Binds.
#   carry    — k2c_arm.h's arm leaves FBG_K2C_M 5 and nothing restores it:
#            the root binds (nothing it froze expands the name); the next
#            root, k2c_user.h, froze an expansion of 7, so it declines.
#   declined — k2c_early.h's frozen token expands FBG_K2C_E right after its
#            arm: the root declines and parses live (5); k2c_user2.h froze
#            an expansion of 7 and declines too (k2c_plain.h bound first, so
#            binding goes on after the decline).
# g++ and clang++: top=7 / user=5 / early=5 user2=5.
kc_dir=tmp/fbgate_k2c
rm -rf "$kc_dir"
mkdir -p "$kc_dir"
cat > "$kc_dir/fbgate_k2c_def.h" <<'EOF'
#ifndef FBGATE_K2C_DEF_H
#define FBGATE_K2C_DEF_H
#undef FBG_K2C_N
#define FBG_K2C_N 7
#define FBG_K2C_M 7
#define FBG_K2C_E 7
#endif
EOF
cat > "$kc_dir/fbgate_k2c_own.h" <<'EOF'
#ifndef FBGATE_K2C_OWN_H
#define FBGATE_K2C_OWN_H
struct fbg_k2c_own { int v; };
#ifndef FBG_K2C_N
# define FBG_K2C_N 5
#endif
#endif
EOF
cat > "$kc_dir/fbgate_k2c_top.h" <<'EOF'
#ifndef FBGATE_K2C_TOP_H
#define FBGATE_K2C_TOP_H
#include <fbgate_k2c_own.h>
#include <fbgate_k2c_def.h>
static const int fbg_k2c_top = FBG_K2C_N;
#endif
EOF
cat > "$kc_dir/fbgate_k2c_arm.h" <<'EOF'
#ifndef FBGATE_K2C_ARM_H
#define FBGATE_K2C_ARM_H
struct fbg_k2c_arm { int v; };
#ifndef FBG_K2C_M
# define FBG_K2C_M 5
#endif
#endif
EOF
cat > "$kc_dir/fbgate_k2c_user.h" <<'EOF'
#ifndef FBGATE_K2C_USER_H
#define FBGATE_K2C_USER_H
static const int fbg_k2c_user = FBG_K2C_M;
#endif
EOF
cat > "$kc_dir/fbgate_k2c_plain.h" <<'EOF'
#ifndef FBGATE_K2C_PLAIN_H
#define FBGATE_K2C_PLAIN_H
struct fbg_k2c_plain { int v; };
#endif
EOF
cat > "$kc_dir/fbgate_k2c_early.h" <<'EOF'
#ifndef FBGATE_K2C_EARLY_H
#define FBGATE_K2C_EARLY_H
#ifndef FBG_K2C_E
# define FBG_K2C_E 5
#endif
static const int fbg_k2c_early = FBG_K2C_E;
#endif
EOF
cat > "$kc_dir/fbgate_k2c_user2.h" <<'EOF'
#ifndef FBGATE_K2C_USER2_H
#define FBGATE_K2C_USER2_H
static const int fbg_k2c_user2 = FBG_K2C_E;
#endif
EOF
cat > "$kc_dir/producer.cpp" <<'EOF'
#include <fbgate_k2c_def.h>
#include <fbgate_k2c_top.h>
#include <fbgate_k2c_arm.h>
#include <fbgate_k2c_user.h>
#include <fbgate_k2c_plain.h>
#include <fbgate_k2c_early.h>
#include <fbgate_k2c_user2.h>
int main() { return 0; }
EOF
cat > "$kc_dir/restore.cpp" <<'EOF'
#include <fbgate_k2c_top.h>
#include <stdio.h>
int main() { printf("top=%d\n", fbg_k2c_top); return 0; }
EOF
cat > "$kc_dir/carry.cpp" <<'EOF'
#include <fbgate_k2c_arm.h>
#include <fbgate_k2c_user.h>
#include <stdio.h>
int main() { printf("user=%d\n", fbg_k2c_user); return 0; }
EOF
cat > "$kc_dir/declined.cpp" <<'EOF'
#include <fbgate_k2c_plain.h>
#include <fbgate_k2c_early.h>
#include <fbgate_k2c_user2.h>
#include <stdio.h>
int main() { printf("early=%d user2=%d\n", fbg_k2c_early, fbg_k2c_user2); return 0; }
EOF
kc_snap=$kc_dir/k2c.msnap
timeout 600 "$BIN" --std=gnu++17 --freeze="$kc_snap" -I "$kc_dir" "$kc_dir/producer.cpp" \
    >"$kc_dir/freeze.log" 2>&1 || fail "[k2c] the --freeze FAILED: $(tail -n 3 "$kc_dir/freeze.log")"
kc_case() {	# kc_case <name> <expected output>: run it, leave its -v trace in <name>.log
    if command -v g++ >/dev/null 2>&1; then
	g++ -std=gnu++17 -I "$kc_dir" -o "$kc_dir/$1.gxx" "$kc_dir/$1.cpp" 2>"$kc_dir/$1.gxx.err" \
	    || fail "[k2c] $1: g++ compile FAILED: $(head -n 2 "$kc_dir/$1.gxx.err")"
	[ "$("$kc_dir/$1.gxx")" = "$2" ] || fail "[k2c] $1: g++ output is not '$2' — the reducer is wrong"
    fi
    kc_out=$(timeout 60 "$BIN" --std=gnu++17 --forest-bind="$kc_snap" -I "$kc_dir" "$kc_dir/$1.cpp" 2>&1)
    [ "$kc_out" = "$2" ] || fail "[k2c] $1: output '$kc_out' != '$2' (g++)"
    timeout 60 "$BIN" -v --std=gnu++17 --forest-bind="$kc_snap" -I "$kc_dir" "$kc_dir/$1.cpp" \
	>"$kc_dir/$1.log" 2>/dev/null
}
kc_bound() {	# kc_bound <case> <header>
    grep -aq "bound to grove unit.*$2" "$kc_dir/$1.log" \
	|| fail "[k2c] $1: $2 did not bind — $(grep -a -m1 'DECLINE root' "$kc_dir/$1.log")"
}
kc_declined() {	# kc_declined <case> <header> <macro>
    grep -aq "DECLINE root.*$2.*expands '$3'" "$kc_dir/$1.log" \
	|| fail "[k2c] $1: $2 did not decline where it expands $3"
}
kc_case restore "top=7"
kc_bound restore "fbgate_k2c_top\.h"
kc_case carry "user=5"
kc_bound carry "fbgate_k2c_arm\.h"
kc_declined carry "fbgate_k2c_user\.h" FBG_K2C_M
kc_case declined "early=5 user2=5"
kc_bound declined "fbgate_k2c_plain\.h"
kc_declined declined "fbgate_k2c_early\.h" FBG_K2C_E
kc_declined declined "fbgate_k2c_user2\.h" FBG_K2C_E
rm -rf "$kc_dir"
echo "forest_bind_gate: [k2c] OK — a run arm's differing macro is checked where frozen content expands it: restored first binds, an expansion of the frozen value declines, in this root and every later one"

# --- case: k2d (docs/plans/2026-10-18-verified-cxx-bind.md, K2d) ---
# mingw's stdio.h and wchar.h hold the same `#ifndef _WSTDIO_DEFINED` block —
# prototypes, inline functions, `#include <swprintf.inl>`. The freeze reads
# k2d_stdio.h first, so its block is taken and k2d_wchar.h's is skipped; a
# consumer reaching k2d_wchar.h first (through k2d_top.h) takes the other one.
#   include  — the walk runs k2d_wchar.h's skipped arm in place, and the arm's
#            `#include <fbgate_k2d_inl.h>` is an edge of the walk: it binds.
#   withdraw — k2d_top.h's next edge binds k2d_stdio.h, whose frozen arm the
#            walk skips: the declarations made inside it are withdrawn (the
#            arm already declared them live), so nothing is declared twice.
#   unbound  — an arm whose `#include` names a header the freeze never read:
#            the walk cannot bind it, the root declines and parses live.
#   stream   — madc dialect: the root's walk replays <iostream> (the ostream
#            guard), runs k2d_wchar.h's arm, whose `#include` binds, then
#            declines on a later conditional. The arm's include completes
#            no serving, so bits/value_stream is injected once, after the
#            live <iostream> — never mid-walk ahead of std::ostream.
# gcc and clang: inline=41 twice=80 / extra=3 / stream=41.
kd_dir=tmp/fbgate_k2d
rm -rf "$kd_dir"
mkdir -p "$kd_dir"
cat > "$kd_dir/fbgate_k2d_inl.h" <<'EOF'
#ifndef FBG_K2D_INL_H
#define FBG_K2D_INL_H
static inline int fbg_k2d_twice(int x) { return fbg_k2d_proto(x) * 2; }
#endif
EOF
cat > "$kd_dir/fbgate_k2d_stdio.h" <<'EOF'
#ifndef FBG_K2D_STDIO_H
#define FBG_K2D_STDIO_H
struct fbg_k2d_file { int fd; };
#ifndef FBG_K2D_BLOCK
#define FBG_K2D_BLOCK
int fbg_k2d_proto(int);
static inline int fbg_k2d_inline(int x) { return fbg_k2d_proto(x) + 1; }
#include <fbgate_k2d_inl.h>
#endif
#endif
EOF
cat > "$kd_dir/fbgate_k2d_wchar.h" <<'EOF'
#ifndef FBG_K2D_WCHAR_H
#define FBG_K2D_WCHAR_H
struct fbg_k2d_wfile { int fd; };
#ifndef FBG_K2D_BLOCK
#define FBG_K2D_BLOCK
int fbg_k2d_proto(int);
static inline int fbg_k2d_inline(int x) { return fbg_k2d_proto(x) + 1; }
#include <fbgate_k2d_inl.h>
#endif
#endif
EOF
cat > "$kd_dir/fbgate_k2d_top.h" <<'EOF'
#ifndef FBG_K2D_TOP_H
#define FBG_K2D_TOP_H
#include <fbgate_k2d_wchar.h>
#include <fbgate_k2d_stdio.h>
#endif
EOF
cat > "$kd_dir/fbgate_k2d_extra.h" <<'EOF'
#ifndef FBG_K2D_EXTRA_H
#define FBG_K2D_EXTRA_H
#ifdef FBG_K2D_WANT_EXTRA
#include <fbgate_k2d_unseen.h>
#endif
struct fbg_k2d_extra { int v; };
#endif
EOF
cat > "$kd_dir/fbgate_k2d_unseen.h" <<'EOF'
#ifndef FBG_K2D_UNSEEN_H
#define FBG_K2D_UNSEEN_H
static inline int fbg_k2d_unseen(void) { return 3; }
#endif
EOF
cat > "$kd_dir/fbgate_k2d_spice.h" <<'EOF'
#ifndef FBG_K2D_SPICE_H
#define FBG_K2D_SPICE_H
#define FBG_K2D_SPICE 1
#endif
EOF
cat > "$kd_dir/fbgate_k2d_osroot.h" <<'EOF'
#ifndef FBG_K2D_OSROOT_H
#define FBG_K2D_OSROOT_H
#include <iostream>
#include <fbgate_k2d_wchar.h>
#ifdef FBG_K2D_SPICE
typedef int fbg_k2d_spiced;
#endif
#endif
EOF
cat > "$kd_dir/producer.cpp" <<'EOF'
#include <fbgate_k2d_stdio.h>
#include <fbgate_k2d_top.h>
#include <fbgate_k2d_extra.h>
int main() { return 0; }
EOF
cat > "$kd_dir/producer_stream.mad" <<'EOF'
#include <fbgate_k2d_spice.h>
#include <fbgate_k2d_stdio.h>
#include <fbgate_k2d_osroot.h>
int main() { return 0; }
EOF
cat > "$kd_dir/stream.mad" <<'EOF'
#include <fbgate_k2d_osroot.h>
int fbg_k2d_proto(int x) { return x * 10; }
int main() { std::cout << "stream=" << fbg_k2d_inline(4) << std::endl; return 0; }
EOF
cat > "$kd_dir/withdraw.cpp" <<'EOF'
#include <fbgate_k2d_top.h>
#include <stdio.h>
int fbg_k2d_proto(int x) { return x * 10; }
int main() { printf("inline=%d twice=%d\n", fbg_k2d_inline(4), fbg_k2d_twice(4)); return 0; }
EOF
cat > "$kd_dir/unbound.cpp" <<'EOF'
#define FBG_K2D_WANT_EXTRA
#include <fbgate_k2d_extra.h>
#include <stdio.h>
int main() { printf("extra=%d\n", fbg_k2d_unseen()); return 0; }
EOF
kd_snap=$kd_dir/k2d.msnap
timeout 600 "$BIN" --std=gnu++17 --freeze="$kd_snap" -I "$kd_dir" "$kd_dir/producer.cpp" \
    >"$kd_dir/freeze.log" 2>&1 || fail "[k2d] the --freeze FAILED: $(tail -n 3 "$kd_dir/freeze.log")"
kd_case() {	# kd_case <name> <expected output>: run it, leave its -v trace in <name>.log
    if command -v g++ >/dev/null 2>&1; then
	g++ -std=gnu++17 -I "$kd_dir" -o "$kd_dir/$1.gxx" "$kd_dir/$1.cpp" 2>"$kd_dir/$1.gxx.err" \
	    || fail "[k2d] $1: g++ compile FAILED: $(head -n 2 "$kd_dir/$1.gxx.err")"
	[ "$("$kd_dir/$1.gxx")" = "$2" ] || fail "[k2d] $1: g++ output is not '$2' — the reducer is wrong"
    fi
    kd_out=$(timeout 60 "$BIN" --std=gnu++17 --forest-bind="$kd_snap" -I "$kd_dir" "$kd_dir/$1.cpp" 2>&1)
    [ "$kd_out" = "$2" ] || fail "[k2d] $1: output '$kd_out' != '$2' (g++)"
    timeout 60 "$BIN" -v --std=gnu++17 --forest-bind="$kd_snap" -I "$kd_dir" "$kd_dir/$1.cpp" \
	>"$kd_dir/$1.log" 2>/dev/null
}
kd_case withdraw "inline=41 twice=80"
grep -aq "bound to grove unit.*fbgate_k2d_top\.h" "$kd_dir/withdraw.log" \
    || fail "[k2d] include: fbgate_k2d_top.h did not bind — $(grep -a -m1 'DECLINE root' "$kd_dir/withdraw.log")"
grep -aq "run arm's #include <fbgate_k2d_inl\.h> binds unit" "$kd_dir/withdraw.log" \
    || fail "[k2d] include: the run arm's #include did not bind fbgate_k2d_inl.h"
grep -aq "withdraws 'fbg_k2d_inline'" "$kd_dir/withdraw.log" \
    || fail "[k2d] withdraw: the skipped frozen arm's fbg_k2d_inline was not withdrawn"
kd_case unbound "extra=3"
grep -aq "DECLINE root.*fbgate_k2d_extra\.h.*cannot bind" "$kd_dir/unbound.log" \
    || fail "[k2d] unbound: fbgate_k2d_extra.h did not decline on its unbindable #include"
# stream: the madc profile's own freeze (the subject's --std=madc).
kd_snap2=$kd_dir/k2d_stream.msnap
timeout 600 "$BIN" --freeze="$kd_snap2" -I "$kd_dir" "$kd_dir/producer_stream.mad" \
    >"$kd_dir/freeze_stream.log" 2>&1 || fail "[k2d] stream: the --freeze FAILED: $(tail -n 3 "$kd_dir/freeze_stream.log")"
if command -v g++ >/dev/null 2>&1; then
    cp "$kd_dir/stream.mad" "$kd_dir/stream_gxx.cpp"
    g++ -std=gnu++17 -I "$kd_dir" -o "$kd_dir/stream.gxx" "$kd_dir/stream_gxx.cpp" 2>"$kd_dir/stream.gxx.err" \
	|| fail "[k2d] stream: g++ compile FAILED: $(head -n 2 "$kd_dir/stream.gxx.err")"
    [ "$("$kd_dir/stream.gxx")" = "stream=41" ] || fail "[k2d] stream: g++ output is not 'stream=41' — the reducer is wrong"
fi
kd_out=$(timeout 60 "$BIN" --forest-bind="$kd_snap2" -I "$kd_dir" "$kd_dir/stream.mad" 2>&1)
[ "$kd_out" = "stream=41" ] || fail "[k2d] stream: output '$(printf '%s' "$kd_out" | head -n 2)' != 'stream=41' (g++)"
timeout 60 "$BIN" -v --forest-bind="$kd_snap2" -I "$kd_dir" "$kd_dir/stream.mad" >"$kd_dir/stream.log" 2>/dev/null
grep -aq "run arm's #include <fbgate_k2d_inl\.h> binds unit" "$kd_dir/stream.log" \
    || fail "[k2d] stream: the run arm's #include did not bind fbgate_k2d_inl.h"
grep -aq "DECLINE root.*fbgate_k2d_osroot\.h" "$kd_dir/stream.log" \
    || fail "[k2d] stream: fbgate_k2d_osroot.h did not decline on FBG_K2D_SPICE"
rm -rf "$kd_dir"
echo "forest_bind_gate: [k2d] OK — a run arm's #include is an edge of the walk (no serving completes there), and a frozen arm the walk skips withdraws its declarations"

# --- case: lexalike (docs/plans/2026-10-18-verified-cxx-bind.md, K2b) ---
# A run arm whose write differs in SPELLING from the freeze's state but lexes
# to the same tokens leaves the frozen content as it is. glibc's <wchar.h>
# defaults __attr_dealloc_fclose to empty when <stdio.h> has not set it to
# `__attr_dealloc (fclose, 1)`; the corpus froze <stdio.h> first, so a
# consumer of <wchar.h> alone runs the arm. madc reads a GNU attribute it does
# not model as no token, so both values lex to nothing and the root binds
# (before K2b it declined on the spelling). Oracle: gcc / g++ print n=3.
la_dir=tmp/fbgate_lexalike
rm -rf "$la_dir"
mkdir -p "$la_dir"
printf '#include <stdio.h>\n#include <wchar.h>\nint main(void) { return 0; }\n' > "$la_dir/c_tu.c"
printf '#include <cstdio>\n#include <cwchar>\nint main() { return 0; }\n' > "$la_dir/cxx_tu.cpp"
printf '#include <wchar.h>\n#include <stdio.h>\nint main(void) { printf("n=%%d\\n", (int)wcslen(L"abc")); return 0; }\n' > "$la_dir/use.c"
printf '#include <cwchar>\n#include <cstdio>\nint main() { std::printf("n=%%d\\n", (int)std::wcslen(L"abc")); return 0; }\n' > "$la_dir/use.cpp"
timeout 600 "$BIN" --std=gnu17 --freeze-raw --freeze="$la_dir/c.msnap" "$la_dir/c_tu.c" \
    > "$la_dir/freeze.log" 2>&1 \
    || fail "[lexalike] the C freeze FAILED: $(tail -n 3 "$la_dir/freeze.log")"
timeout 600 "$BIN" --std=gnu++17 --freeze="$la_dir/cxx.msnap" "$la_dir/cxx_tu.cpp" \
    > "$la_dir/freeze_cxx.log" 2>&1 \
    || fail "[lexalike] the C++ freeze FAILED: $(tail -n 3 "$la_dir/freeze_cxx.log")"
for la_case in "gnu17 c.msnap use.c wchar.h" "gnu++17 cxx.msnap use.cpp cwchar"; do
    read -r la_std la_snap la_src la_root <<<"$la_case"
    timeout 60 "$BIN" -v --std="$la_std" --forest-bind="$la_dir/$la_snap" "$la_dir/$la_src" \
        > "$la_dir/trace.log" 2>&1
    grep -aqE "bound to grove unit [0-9]+ \([^)]*/$la_root\)" "$la_dir/trace.log" \
        || fail "[lexalike] $la_std: <$la_root> did not bind ($(grep -a -m1 'DECLINE root' "$la_dir/trace.log"))"
    la_out=$(timeout 60 "$BIN" --std="$la_std" --forest-bind="$la_dir/$la_snap" "$la_dir/$la_src" 2>"$la_dir/run.err")
    [ "$la_out" = "n=3" ] \
        || fail "[lexalike] $la_std printed '$la_out', want 'n=3' $(head -n 2 "$la_dir/run.err")"
done
rm -rf "$la_dir"
echo "forest_bind_gate: [lexalike] OK — a run arm whose write lexes to the frozen state's tokens binds (glibc's __attr_dealloc_fclose, C and C++)"

# --- case: typedefguard (docs/plans/2026-10-18-verified-cxx-bind.md, K2b) ---
# glibc guards each off64_t typedef with `#if defined __USE_LARGEFILE64 &&
# !defined __off64_t_defined`, an #if whose arm defines the guard it tests.
# The corpus froze <stdio.h> first, so <stdio.h> took the arm and <unistd.h>
# skipped it. A consumer of <unistd.h> first takes <unistd.h>'s arm and skips
# <stdio.h>'s: the arm's own guard is live, the condition read without it is
# taken, so the arm is masked as a plain `#ifndef` guard is (before K2b the
# root declined). Oracle: gcc / g++ print o=5.
tg_dir=tmp/fbgate_typedefguard
rm -rf "$tg_dir"
mkdir -p "$tg_dir"
printf '#include <stdio.h>\n#include <unistd.h>\nint main(void) { return 0; }\n' > "$tg_dir/c_tu.c"
printf '#include <cstdio>\n#include <unistd.h>\nint main() { return 0; }\n' > "$tg_dir/cxx_tu.cpp"
printf '#include <unistd.h>\n#include <stdio.h>\nint main(void) { off64_t o = 5; printf("o=%%d\\n", (int)o); return 0; }\n' > "$tg_dir/use.c"
printf '#include <unistd.h>\n#include <cstdio>\nint main() { off64_t o = 5; std::printf("o=%%d\\n", (int)o); return 0; }\n' > "$tg_dir/use.cpp"
timeout 600 "$BIN" --std=gnu17 -D_GNU_SOURCE --freeze-raw --freeze="$tg_dir/c.msnap" "$tg_dir/c_tu.c" \
    > "$tg_dir/freeze.log" 2>&1 \
    || fail "[typedefguard] the C freeze FAILED: $(tail -n 3 "$tg_dir/freeze.log")"
timeout 600 "$BIN" --std=gnu++17 --freeze="$tg_dir/cxx.msnap" "$tg_dir/cxx_tu.cpp" \
    > "$tg_dir/freeze_cxx.log" 2>&1 \
    || fail "[typedefguard] the C++ freeze FAILED: $(tail -n 3 "$tg_dir/freeze_cxx.log")"
for tg_case in "gnu17 -D_GNU_SOURCE c.msnap use.c stdio.h" "gnu++17 - cxx.msnap use.cpp cstdio"; do
    read -r tg_std tg_def tg_snap tg_src tg_root <<<"$tg_case"
    [ "$tg_def" = "-" ] && tg_def=
    timeout 60 "$BIN" -v --std="$tg_std" $tg_def --forest-bind="$tg_dir/$tg_snap" "$tg_dir/$tg_src" \
        > "$tg_dir/trace.log" 2>&1
    grep -aqE "bound to grove unit [0-9]+ \([^)]*/$tg_root\)" "$tg_dir/trace.log" \
        || fail "[typedefguard] $tg_std: <$tg_root> did not bind ($(grep -a -m1 'DECLINE root' "$tg_dir/trace.log"))"
    tg_out=$(timeout 60 "$BIN" --std="$tg_std" $tg_def --forest-bind="$tg_dir/$tg_snap" "$tg_dir/$tg_src" 2>"$tg_dir/run.err")
    [ "$tg_out" = "o=5" ] \
        || fail "[typedefguard] $tg_std printed '$tg_out', want 'o=5' $(head -n 2 "$tg_dir/run.err")"
done
rm -rf "$tg_dir"
echo "forest_bind_gate: [typedefguard] OK — a taken #if arm skipped only by its own live guard is masked (glibc's off64_t typedef guard, C and C++)"

# --- case: overlap (docs/plans/2026-10-18-verified-cxx-bind.md, K1b) ---
# A C++ root that declines AFTER a bind parses live beside the bound units.
# When its live closure declares a name a bound unit declares (fbg_ov_fn in
# both fbgate_ov_a.h and fbgate_ov_b.h), bound and live content would mix on
# that name, so binding stops for the rest of the TU: the later fbgate_ov_c.h
# parses live. A disjoint decline (fbgate_ov_d.h declares only its own names)
# keeps binding: fbgate_ov_c.h binds. Both print g++'s v=8.
ov_dir=tmp/fbgate_overlap
rm -rf "$ov_dir"
mkdir -p "$ov_dir/inc"
printf '#ifndef FBG_OV_A_H\n#define FBG_OV_A_H\nint fbg_ov_fn(void);\nstruct fbg_ov_a { int a; };\n#endif\n' > "$ov_dir/inc/fbgate_ov_a.h"
printf '#ifndef FBG_OV_B_H\n#define FBG_OV_B_H\nint fbg_ov_fn(void);\n#ifdef FBG_OV_WIDE\n#define FBG_OV_B 5\n#else\n#define FBG_OV_B 4\n#endif\n#endif\n' > "$ov_dir/inc/fbgate_ov_b.h"
printf '#ifndef FBG_OV_D_H\n#define FBG_OV_D_H\nint fbg_ov_dfn(void);\n#ifdef FBG_OV_WIDE\n#define FBG_OV_B 5\n#else\n#define FBG_OV_B 4\n#endif\n#endif\n' > "$ov_dir/inc/fbgate_ov_d.h"
printf '#ifndef FBG_OV_C_H\n#define FBG_OV_C_H\nstruct fbg_ov_c { int c; };\n#endif\n' > "$ov_dir/inc/fbgate_ov_c.h"
printf '#include <fbgate_ov_a.h>\n#include <fbgate_ov_b.h>\n#include <fbgate_ov_d.h>\n#include <fbgate_ov_c.h>\nint main() { return 0; }\n' > "$ov_dir/producer.cpp"
for ov_case in "b stop" "d bind"; do
    read -r ov_hdr ov_want <<<"$ov_case"
    printf '#include <cstdio>\n#include <fbgate_ov_a.h>\n#define FBG_OV_WIDE 1\n#include <fbgate_ov_%s.h>\n#include <fbgate_ov_c.h>\nint fbg_ov_fn(void) { return 1; }\nint main() { struct fbg_ov_a a = { 2 }; struct fbg_ov_c c = { 1 }; std::printf("v=%%d\\n", FBG_OV_B + a.a + c.c + fbg_ov_fn() - 1); return 0; }\n' "$ov_hdr" > "$ov_dir/use_$ov_hdr.cpp"
done
timeout 600 "$BIN" --std=gnu++17 --freeze="$ov_dir/ov.msnap" -I "$ov_dir/inc" "$ov_dir/producer.cpp" \
    > "$ov_dir/freeze.log" 2>&1 \
    || fail "[overlap] the freeze FAILED: $(tail -n 3 "$ov_dir/freeze.log")"
for ov_case in "b stop" "d bind"; do
    read -r ov_hdr ov_want <<<"$ov_case"
    timeout 60 "$BIN" -v --std=gnu++17 --forest-bind="$ov_dir/ov.msnap" -I "$ov_dir/inc" "$ov_dir/use_$ov_hdr.cpp" \
        > "$ov_dir/trace.log" 2>&1
    grep -aqE "bound to grove unit [0-9]+ \([^)]*fbgate_ov_a\.h\)" "$ov_dir/trace.log" \
        || fail "[overlap] $ov_hdr: fbgate_ov_a.h did not bind first"
    grep -aqE "DECLINE root [^ ]*fbgate_ov_$ov_hdr\.h" "$ov_dir/trace.log" \
        || fail "[overlap] $ov_hdr: fbgate_ov_$ov_hdr.h did not decline (the leg needs a decline after a bind)"
    if [ "$ov_want" = stop ]; then
        grep -aq "binding stopped for this TU" "$ov_dir/trace.log" \
            || fail "[overlap] an overlapping decline after a bind did not stop binding"
        grep -aqE "bound to grove unit [0-9]+ \([^)]*fbgate_ov_c\.h\)" "$ov_dir/trace.log" \
            && fail "[overlap] fbgate_ov_c.h bound after an overlapping decline"
    else
        grep -aq "binding stopped for this TU" "$ov_dir/trace.log" \
            && fail "[overlap] a disjoint decline after a bind stopped binding"
        grep -aqE "bound to grove unit [0-9]+ \([^)]*fbgate_ov_c\.h\)" "$ov_dir/trace.log" \
            || fail "[overlap] fbgate_ov_c.h did not bind after a disjoint decline"
    fi
    ov_out=$(timeout 60 "$BIN" --std=gnu++17 --forest-bind="$ov_dir/ov.msnap" -I "$ov_dir/inc" "$ov_dir/use_$ov_hdr.cpp" 2>"$ov_dir/run.err")
    [ "$ov_out" = "v=8" ] || fail "[overlap] $ov_hdr printed '$ov_out', want 'v=8' $(head -n 2 "$ov_dir/run.err")"
done
rm -rf "$ov_dir"
echo "forest_bind_gate: [overlap] OK — a C++ decline after a bind whose live closure redeclares a bound name stops binding; a disjoint one keeps binding"

# --- case: progmacro (BUGS.md B211) ---
# A macro the program defines ahead of an include changes every token of that
# header it names. fbgate_pm_a.h spells fbg_pm_sec, frozen without the
# program's `#define fbg_pm_sec fbg_pm_mine`: its root declines and lexes from
# text, so the member is fbg_pm_mine, as gcc and g++ read it. fbgate_pm_b.h
# spells no program macro and binds. A define the program takes back
# (`#undef` before the include) changes nothing: fbgate_pm_a.h binds. Each
# prints gcc's v=8.
pm_dir=tmp/fbgate_progmacro
rm -rf "$pm_dir"
mkdir -p "$pm_dir/inc"
printf '#ifndef FBG_PM_A_H\n#define FBG_PM_A_H\nstruct fbg_pm { int fbg_pm_sec; };\n#endif\n' > "$pm_dir/inc/fbgate_pm_a.h"
printf '#ifndef FBG_PM_B_H\n#define FBG_PM_B_H\nstruct fbg_pm_b { int b; };\n#endif\n' > "$pm_dir/inc/fbgate_pm_b.h"
printf '#include <fbgate_pm_b.h>\n#include <fbgate_pm_a.h>\nint main(void) { return 0; }\n' > "$pm_dir/producer.c"
cp "$pm_dir/producer.c" "$pm_dir/producer.cpp"
pm_body='int main(void) { struct fbg_pm t = { 0 }; struct fbg_pm_b b = { 1 }; t.fbg_pm_sec = 7; printf("v=%%d\\n", t.%s + b.b); return 0; }\n'
printf "#define fbg_pm_sec fbg_pm_mine\n#include <fbgate_pm_b.h>\n#include <fbgate_pm_a.h>\n#include <stdio.h>\n$pm_body" fbg_pm_mine > "$pm_dir/use_c_decline.c"
printf "#define fbg_pm_sec fbg_pm_mine\n#undef fbg_pm_sec\n#include <fbgate_pm_b.h>\n#include <fbgate_pm_a.h>\n#include <stdio.h>\n$pm_body" fbg_pm_sec > "$pm_dir/use_c_back.c"
printf "#define fbg_pm_sec fbg_pm_mine\n#include <fbgate_pm_b.h>\n#include <fbgate_pm_a.h>\n#include <cstdio>\n$pm_body" fbg_pm_mine > "$pm_dir/use_cxx_decline.cpp"
timeout 600 "$BIN" --std=gnu17 --freeze="$pm_dir/c.msnap" -I "$pm_dir/inc" "$pm_dir/producer.c" \
    > "$pm_dir/freeze.log" 2>&1 \
    || fail "[progmacro] the C freeze FAILED: $(tail -n 3 "$pm_dir/freeze.log")"
timeout 600 "$BIN" --std=gnu++17 --freeze="$pm_dir/cxx.msnap" -I "$pm_dir/inc" "$pm_dir/producer.cpp" \
    > "$pm_dir/freeze.log" 2>&1 \
    || fail "[progmacro] the C++ freeze FAILED: $(tail -n 3 "$pm_dir/freeze.log")"
for pm_case in "use_c_decline.c gnu17 c decline" "use_c_back.c gnu17 c bind" "use_cxx_decline.cpp gnu++17 cxx decline"; do
    read -r pm_src pm_std pm_snap pm_want <<<"$pm_case"
    timeout 60 "$BIN" -v --std="$pm_std" --forest-bind="$pm_dir/$pm_snap.msnap" -I "$pm_dir/inc" "$pm_dir/$pm_src" \
        > "$pm_dir/trace.log" 2>&1
    grep -aqE "bound to grove unit [0-9]+ \([^)]*fbgate_pm_b\.h\)" "$pm_dir/trace.log" \
        || fail "[progmacro] $pm_src: fbgate_pm_b.h (no program macro) did not bind"
    if [ "$pm_want" = decline ]; then
        grep -aqE "DECLINE root [^ ]*fbgate_pm_a\.h .* spells the program's macro 'fbg_pm_sec'" "$pm_dir/trace.log" \
            || fail "[progmacro] $pm_src: fbgate_pm_a.h did not decline on the program's fbg_pm_sec"
    else
        grep -aqE "bound to grove unit [0-9]+ \([^)]*fbgate_pm_a\.h\)" "$pm_dir/trace.log" \
            || fail "[progmacro] $pm_src: fbgate_pm_a.h did not bind after the define was taken back"
    fi
    pm_out=$(timeout 60 "$BIN" --std="$pm_std" --forest-bind="$pm_dir/$pm_snap.msnap" -I "$pm_dir/inc" "$pm_dir/$pm_src" 2>"$pm_dir/run.err")
    [ "$pm_out" = "v=8" ] || fail "[progmacro] $pm_src printed '$pm_out', want 'v=8' $(head -n 2 "$pm_dir/run.err")"
done
rm -rf "$pm_dir"
echo "forest_bind_gate: [progmacro] OK — a unit spelling a macro the program wrote ahead of it declines its root (C and C++); one taken back binds"

# --- case: cenv (docs/plans/2026-10-18-c-pack-layer.md, J4) ---
# A C program sets feature-test macros ahead of its system includes as a
# matter of course — sqlite3.c defines NDEBUG and _GNU_SOURCE. The C layer
# froze those headers without them, so binding the frozen view hands such a
# program the wrong declarations: a live assert(), no mremap. The bind is
# verified: a root whose recorded conditionals take another arm here declines
# to a live parse. The control: a C program that sets no feature macro still
# binds.
ce_dir=tmp/fbgate_cenv
ce_snap=$ce_dir/c.msnap
rm -rf "$ce_dir"
mkdir -p "$ce_dir"
cat > "$ce_dir/c_tu.c" <<'EOF'
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>
int main(void) { return 0; }
EOF
cat > "$ce_dir/feat.c" <<'EOF'
#define _GNU_SOURCE
#define NDEBUG 1
#include <assert.h>
#include <stdio.h>
#include <sys/mman.h>
int main(void)
{
	assert(only_in_debug_builds);
	printf("mremap=%d\n", MREMAP_MAYMOVE);
	return 0;
}
EOF
cat > "$ce_dir/plain.c" <<'EOF'
#include <stdio.h>
int main(void) { printf("plain\n"); return 0; }
EOF
timeout 600 "$BIN" --std=gnu17 --freeze-raw --freeze="$ce_snap" "$ce_dir/c_tu.c" \
    > "$ce_dir/freeze.log" 2>&1 \
    || fail "[cenv] the C freeze FAILED: $(tail -n 3 "$ce_dir/freeze.log")"
# The oracle: gcc compiles feat.c and prints mremap=1.
ce_out=$(timeout 60 "$BIN" --std=gnu17 --forest-bind="$ce_snap" \
    "$ce_dir/feat.c" 2>"$ce_dir/feat.err")
[ "$ce_out" = "mremap=1" ] \
    || fail "[cenv] a C program's feature macros were ignored by the bind: '$ce_out' $(head -n 2 "$ce_dir/feat.err")"
timeout 60 "$BIN" -v --std=gnu17 --forest-bind="$ce_snap" \
    "$ce_dir/plain.c" > "$ce_dir/plain.log" 2>/dev/null
grep -aq "bound to grove unit" "$ce_dir/plain.log" \
    || fail "[cenv] a C program with no feature macro no longer binds the C layer"
rm -rf "$ce_dir"
echo "forest_bind_gate: [cenv] OK — a C compile's bind honours its feature macros (NDEBUG, _GNU_SOURCE); a plain C program still binds"

# --- case: cdefine (docs/plans/2026-10-20-front-end-speed-plan.md, B1) ---
# A command-line -D is a macro defined before the first line. The pack is
# frozen with none, so a -D could change what it holds -- unless no frozen
# header spells the name: then nothing the pack holds can expand it, and the
# profile serves the consumer (GCC's PCH rule: a macro defined before the PCH
# "must not affect the precompiled header, which usually means that they
# don't appear in the precompiled header at all"). The freeze writes every
# name the frozen units spell (SPELLED_NAMES); a -D of a spelled name keeps
# the decline. The oracle for both consumers is gcc / g++ (cd_unspelled
# prints v=7, cd_ndebug prints ndebug=ok).
cd_dir=tmp/fbgate_cdefine
rm -rf "$cd_dir"
mkdir -p "$cd_dir"
printf '#include <assert.h>\n#include <stdio.h>\nint main(void) { return 0; }\n' > "$cd_dir/c_tu.c"
printf '#include <cassert>\n#include <cstdio>\nint main() { return 0; }\n' > "$cd_dir/cxx_tu.cpp"
cat > "$cd_dir/unspelled.c" <<'EOF'
#include <stdio.h>
int main(void) { printf("v=%d\n", MADC_GATE_UNSPELLED); return 0; }
EOF
cat > "$cd_dir/unspelled.cpp" <<'EOF'
#include <cstdio>
int main() { std::printf("v=%d\n", MADC_GATE_UNSPELLED); return 0; }
EOF
cat > "$cd_dir/ndebug.c" <<'EOF'
#include <assert.h>
#include <stdio.h>
int main(void) { assert(0); printf("ndebug=ok\n"); return 0; }
EOF
timeout 600 "$BIN" --std=gnu17 --freeze-raw --freeze="$cd_dir/c.msnap" "$cd_dir/c_tu.c" \
    > "$cd_dir/freeze.log" 2>&1 \
    || fail "[cdefine] the C freeze FAILED: $(tail -n 3 "$cd_dir/freeze.log")"
timeout 600 "$BIN" --std=gnu++17 --freeze="$cd_dir/cxx.msnap" "$cd_dir/cxx_tu.cpp" \
    > "$cd_dir/freeze_cxx.log" 2>&1 \
    || fail "[cdefine] the C++ freeze FAILED: $(tail -n 3 "$cd_dir/freeze_cxx.log")"
cd_names=$("$BIN" --dump-forest="$cd_dir/c.msnap" 2>/dev/null | awk -F'\t' '$1=="spellednames"{print $2}')
[ -n "$cd_names" ] && [ "$cd_names" -gt 0 ] \
    || fail "[cdefine] the C profile carries no spelled-names table ('$cd_names')"
# cd_check STD SNAP SRC WANT BIND -D... : the consumer prints WANT, and its
# trace shows a bind (BIND=1) or none (BIND=0).
cd_check() {
    local std=$1 snap=$2 src=$3 want=$4 bind=$5 out
    shift 5
    timeout 60 "$BIN" -v --std="$std" "$@" --forest-bind="$snap" "$src" \
        > "$cd_dir/trace.log" 2>&1
    if grep -aq "bound to grove unit" "$cd_dir/trace.log"; then
        [ "$bind" = 1 ] || fail "[cdefine] $std $* bound the profile (a header spells the name)"
    else
        [ "$bind" = 0 ] || fail "[cdefine] $std $* did not bind (no frozen header spells the name)"
    fi
    out=$(timeout 60 "$BIN" --std="$std" "$@" --forest-bind="$snap" "$src" 2>"$cd_dir/run.err")
    [ "$out" = "$want" ] \
        || fail "[cdefine] $std $* printed '$out', want '$want' $(head -n 2 "$cd_dir/run.err")"
}
cd_check gnu17 "$cd_dir/c.msnap" "$cd_dir/unspelled.c" "v=7" 1 -DMADC_GATE_UNSPELLED=7
cd_check gnu++17 "$cd_dir/cxx.msnap" "$cd_dir/unspelled.cpp" "v=7" 1 -DMADC_GATE_UNSPELLED=7
cd_check gnu17 "$cd_dir/c.msnap" "$cd_dir/ndebug.c" "ndebug=ok" 0 -DNDEBUG
rm -rf "$cd_dir"
echo "forest_bind_gate: [cdefine] OK — a -D no frozen header spells binds (C and C++); a -D of a spelled name (NDEBUG) keeps the decline and gcc's answer"

# --- case: dialectnobind (the dialect is builtin: a fragment never binds) ---
# A container that froze a dialect fragment's text (bits/ui_enums, which strict
# C++ parses too) never binds its unit: a madc consumer reads the dialect from
# the builtin table (output == the unbound run's), and a c++17 / gnu++17
# consumer that #includes the fragment parses its text
# (docs/plans/2026-10-17-dialect-builtin-registration.md, R4d).
printf '#include <bits/ui_enums>\nint main() { return 0; }\n' > tmp/fbgate_dialect.cpp
printf '#include <bits/ui_enums>\nint main() { return (int)ui::WEB; }\n' > tmp/fbgate_dialect_strict.cpp
printf 'int main() { println("{}", php::strtoupper("ok")); return 0; }\n' > tmp/fbgate_dialect.mad
db_snap=tmp/fbgate_dialect.msnap
db_log=tmp/fbgate_dialect.log
rm -f "$db_snap"
timeout 600 "$BIN" --std=madc --freeze="$db_snap" --freeze-also-std=gnu++17 --freeze-also-std=c++17 \
    tmp/fbgate_dialect.cpp >"$db_log" 2>&1 || fail "[dialectnobind] the madc freeze of <bits/ui_enums> FAILED: $(tail -n 3 "$db_log")"
db_text=$(timeout 60 "$BIN" tmp/fbgate_dialect.mad 2>&1)
db_out=$(timeout 60 "$BIN" --forest-bind="$db_snap" tmp/fbgate_dialect.mad 2>&1)
[ "$db_out" = "OK" ] && [ "$db_out" = "$db_text" ] \
    || fail "[dialectnobind] the madc consumer printed '$db_out' (unbound '$db_text', want 'OK')"
for db_std in c++17 gnu++17; do
    timeout 60 "$BIN" -v --std=$db_std --forest-bind="$db_snap" tmp/fbgate_dialect_strict.cpp >"$db_log" 2>&1
    db_rc=$?
    [ "$db_rc" -eq 3 ] \
        || fail "[dialectnobind] the $db_std compile of <bits/ui_enums> exited $db_rc (want ui::WEB = 3): $(tail -n 3 "$db_log")"
    if grep -aqE "bound to grove unit [0-9]+ \(bits/ui_enums\)" "$db_log"; then
        fail "[dialectnobind] a $db_std consumer BOUND the dialect fragment's unit"
    fi
done
rm -f "$db_snap" "$db_log" tmp/fbgate_dialect.cpp tmp/fbgate_dialect_strict.cpp tmp/fbgate_dialect.mad
echo "forest_bind_gate: [dialectnobind] OK — a madc compile reads the dialect from its table (output == unbound); c++17 and gnu++17 parse <bits/ui_enums>, never bind it"

# --- case: nsenum (a namespaced unscoped enumerator's identity) ---------------
# An unscoped enumerator inside a namespace is that namespace's member
# ([dcl.enum]/11). The freeze deduped globals by BARE name (fbb::X lost to
# fba::X) and the restore bound none to its namespace (fba::X "is not a
# member of namespace 'fba'"); both now key on live's identity — the
# namespace's member, else the program's name. A tagged enum's enumerator is
# also indexed under the tag (gtag::GA): that index is the tag's, never the
# enumerator's scope — GA stays a global.
cat > tmp/fbgate_nsenum.h <<'EOF'
#ifndef FBGATE_NSENUM_H
#define FBGATE_NSENUM_H
namespace fba { enum mode { X = 1, Y = 2 }; }
namespace fbb { enum kind { X = 7, Z = 9 }; }
enum gtag { GA = 5 };
#endif
EOF
cat > tmp/fbgate_nsenum_producer.cpp <<'EOF'
#include <fbgate_nsenum.h>
int main() { return 0; }
EOF
cat > tmp/fbgate_nsenum_consumer.cpp <<'EOF'
#include <stdio.h>
#include <fbgate_nsenum.h>
int main() { printf("%d %d %d %d %d %d\n", (int)fba::X, (int)fbb::X, (int)fba::Y, (int)fbb::Z, (int)GA, (int)gtag::GA); return 0; }
EOF
run_case nsenum "1 7 2 9 5 5"
rm -f tmp/fbgate_nsenum.h tmp/fbgate_nsenum_producer.cpp tmp/fbgate_nsenum_consumer.cpp

# --- case: armreplay (2026-10-11, forest v55: a skipped directive-only arm replays) ---
# A unit whose `#ifndef X` arm was skipped at freeze because an EARLIER corpus
# sibling defined X carries none of that arm (mingw stdio.h's NULL behind the
# win64 corpus's <cstddef>). A consumer that includes only that unit has no X.
# The freeze captures the arm into the `#ifndef X` condition record (v68; until
# v70 a CIR_BRANCH_DEP_PP_ARM tuple), and the bind's walk runs it through the
# live preprocessor when the consumer lacks X: the unit still BINDS, and
# the arm's nested conditional answers in the CONSUMER's macro state
# (FBG_LV_WIDE, which the producer never defined, selects 50). Negative
# controls: the v54 binary fails with "use of
# undeclared identifier 'FBG_MC_X'"; d939e2a09's verified bind printed v=52
# with fbgate_lv_b.h live-parsed, which the bound assertion refuses.
mkdir -p tmp/fbgate_lv
cat > tmp/fbgate_lv/fbgate_lv_a.h <<'EOF'
#ifndef FBGATE_LV_A_H
#define FBGATE_LV_A_H
#define FBG_MC_X 5
#endif
EOF
cat > tmp/fbgate_lv/fbgate_lv_b.h <<'EOF'
#ifndef FBGATE_LV_B_H
#define FBGATE_LV_B_H
#ifndef FBG_MC_X
# ifdef FBG_LV_WIDE
#  define FBG_MC_X 50	/* the consumer's choice */
# else
#  define FBG_MC_X 5
# endif
#endif
struct fbg_lv_s { int v; };
#endif
EOF
cat > tmp/fbgate_lv/fbgate_lv_c.h <<'EOF'
#ifndef FBGATE_LV_C_H
#define FBGATE_LV_C_H
struct fbg_lv_t { int w; };
#endif
EOF
cat > tmp/fbgate_lv_producer.cpp <<'EOF'
#include <fbgate_lv_a.h>
#include <fbgate_lv_b.h>
#include <fbgate_lv_c.h>
int main() { return 0; }
EOF
cat > tmp/fbgate_lv_consumer.cpp <<'EOF'
#define FBG_LV_WIDE 1
#include <fbgate_lv_c.h>
#include <fbgate_lv_b.h>
#include <cstdio>
int main() { fbg_lv_t t = { 2 }; fbg_lv_s s = { FBG_MC_X + t.w }; printf("v=%d\n", s.v); return 0; }
EOF
lv_snap=tmp/fbgate_lv.msnap
lv_log=tmp/fbgate_lv.log
rm -f "$lv_snap"
timeout 600 "$BIN" --freeze="$lv_snap" --freeze-also-std=c++17 -I tmp/fbgate_lv \
    tmp/fbgate_lv_producer.cpp >"$lv_log" 2>&1 || fail "[armreplay] the madc --freeze-also-std FAILED: $(tail -n 3 "$lv_log")"
lv_out=$(timeout 60 "$BIN" --std=c++17 --forest-bind="$lv_snap" -I tmp/fbgate_lv tmp/fbgate_lv_consumer.cpp 2>&1)
[ "$lv_out" = "v=52" ] || fail "[armreplay] c++17 consumer output '$lv_out' != 'v=52'"
timeout 60 "$BIN" -v --std=c++17 --forest-bind="$lv_snap" -I tmp/fbgate_lv tmp/fbgate_lv_consumer.cpp >"$lv_log" 2>/dev/null
grep -aq "bound to grove unit.*fbgate_lv_b.h" "$lv_log" \
    || fail "[armreplay] the unit with the skipped arm did not BIND for the c++17 consumer"
grep -aq "fbgate_lv_b.h replays its skipped '#ifndef FBG_MC_X' arm" "$lv_log" \
    || fail "[armreplay] the bound unit never replayed its skipped arm"
rm -rf "$lv_snap" "$lv_log" tmp/fbgate_lv tmp/fbgate_lv_producer.cpp tmp/fbgate_lv_consumer.cpp
echo "forest_bind_gate: [armreplay] OK — a skipped directive-only arm binds and replays in the consumer's macro state"

# --- case: armreplayc (2026-10-18, forest v67: an arm that reproduces the dep binds under C) ---
# A C consumer's bind is always verified, so the armreplay unit's want-defined
# dep DECLINED its root there — mingw stdio.h's WEOF and NULL, defined first by
# the C layer's earlier ctype.h / stddef.h roots, left <stdio.h> live-parsed in
# every win64 C compile. The walk runs the unit's captured arm (writing the
# name alone, over an object-like external; v67-v69 marked it
# CIR_BRANCH_DEP_ARM_REPLAYS) in the consumer's state and, when it yields the
# frozen value, binds (fbgate_ac_b.h binds, x=5 —
# its arm writes the name in two branches, mingw stdio.h's NULL shape, and
# answers by language). Since K2 (v69) an arm whose value DIFFERS from the
# definer's binds too when no frozen token after the arm expanded that value:
# fbgate_ac_d.h's FBG_AC_Y is 6 at freeze and its arm makes it 7 here, and the
# walk runs the arm (y=7, as gcc). The shape whose frozen token DID expand the
# value declines — [k2walk]'s `expanded`. Negative control: the v66 binary
# declines fbgate_ac_b.h too.
mkdir -p tmp/fbgate_ac
cat > tmp/fbgate_ac/fbgate_ac_a.h <<'EOF'
#ifndef FBGATE_AC_A_H
#define FBGATE_AC_A_H
#define FBG_AC_X 5
#define FBG_AC_Y 6
#endif
EOF
cat > tmp/fbgate_ac/fbgate_ac_b.h <<'EOF'
#ifndef FBGATE_AC_B_H
#define FBGATE_AC_B_H
#ifndef FBG_AC_X
# ifdef __cplusplus
#  define FBG_AC_X 0
# else
#  define FBG_AC_X 5
# endif
#endif
struct fbg_ac_s { int v; };
#endif
EOF
cat > tmp/fbgate_ac/fbgate_ac_d.h <<'EOF'
#ifndef FBGATE_AC_D_H
#define FBGATE_AC_D_H
#ifndef FBG_AC_Y
#define FBG_AC_Y 7
#endif
struct fbg_ac_d { int w; };
#endif
EOF
cat > tmp/fbgate_ac_producer.c <<'EOF'
#include <fbgate_ac_a.h>
#include <fbgate_ac_b.h>
#include <fbgate_ac_d.h>
int main(void) { return 0; }
EOF
cat > tmp/fbgate_ac_consumer.c <<'EOF'
#include <fbgate_ac_b.h>
#include <fbgate_ac_d.h>
#include <stdio.h>
int main(void) { struct fbg_ac_s s = { FBG_AC_X }; struct fbg_ac_d d = { FBG_AC_Y }; printf("x=%d y=%d\n", s.v, d.w); return 0; }
EOF
ac_snap=tmp/fbgate_ac.msnap
ac_log=tmp/fbgate_ac.log
rm -f "$ac_snap"
timeout 600 "$BIN" --std=gnu17 --freeze="$ac_snap" -I tmp/fbgate_ac \
    tmp/fbgate_ac_producer.c >"$ac_log" 2>&1 || fail "[armreplayc] the madc --freeze FAILED: $(tail -n 3 "$ac_log")"
ac_out=$(timeout 60 "$BIN" --std=gnu17 --forest-bind="$ac_snap" -I tmp/fbgate_ac tmp/fbgate_ac_consumer.c 2>&1)
[ "$ac_out" = "x=5 y=7" ] || fail "[armreplayc] gnu17 consumer output '$ac_out' != 'x=5 y=7'"
timeout 60 "$BIN" -v --std=gnu17 --forest-bind="$ac_snap" -I tmp/fbgate_ac tmp/fbgate_ac_consumer.c >"$ac_log" 2>/dev/null
grep -aq "bound to grove unit.*fbgate_ac_b.h" "$ac_log" \
    || fail "[armreplayc] the unit whose arm reproduces its dep did not BIND for the C consumer"
grep -aq "fbgate_ac_b.h replays its skipped '#ifndef FBG_AC_X' arm" "$ac_log" \
    || fail "[armreplayc] the bound unit never replayed its skipped arm"
grep -aq "fbgate_ac_d.h replays its skipped '#ifndef FBG_AC_Y' arm" "$ac_log" \
    || fail "[armreplayc] the unit whose arm sets a value no frozen token expanded did not replay it"
rm -rf "$ac_snap" "$ac_log" tmp/fbgate_ac tmp/fbgate_ac_producer.c tmp/fbgate_ac_consumer.c
echo "forest_bind_gate: [armreplayc] OK — under C, a skipped arm replays: it reproduces its dep, or sets a value no frozen token expanded"

# --- case: replreuse (2026-10-11: each unit's tree gets its own forest nodes) ---
# A frozen inline body the program's main uses, touched again by a REPL entry
# (madc -i), a later translation unit of the same c2m session. The segment's
# per-record memo handed the entry the SAME nodes the first unit's tree had
# already linked in and c2mir had already checked, so the entry's tree held
# them twice: "repeated declaration a/b/s", or garbage error nodes (libc++'s
# inline operator<<(ostream&, const char*) on darwin: 21 "cir error: do",
# testrepl_cout). CirFrozenForest::begin_compile now drops the memo per
# compile and the entry materializes its own copy. Negative control: the
# pre-fix binary prints "repeated declaration a" and 3 c2mir check errors.
mkdir -p tmp/fbgate_repl
cat > tmp/fbgate_repl/fbgate_repl.h <<'EOF'
#ifndef FBGATE_REPL_H
#define FBGATE_REPL_H
inline int fbg_add(int a, int b) { int s = a; s += b; return s; }
template<class T> inline T fbg_twice(T v) { T r = v; r += v; return r; }
#endif
EOF
cat > tmp/fbgate_repl_producer.mad <<'EOF'
#include <fbgate_repl.h>
int main() { return 0; }
EOF
cat > tmp/fbgate_repl_consumer.mad <<'EOF'
#include <fbgate_repl.h>
#include <cstdio>
int main() { printf("main %d %d\n", fbg_add(1, 2), fbg_twice(5)); return 0; }
EOF
printf 'fbg_add(3, 4)\nfbg_twice(21)\n' > tmp/fbgate_repl_input.txt
rr_snap=tmp/fbgate_repl.msnap
rr_log=tmp/fbgate_repl.log
rm -f "$rr_snap"
timeout 600 "$BIN" --freeze="$rr_snap" -I tmp/fbgate_repl tmp/fbgate_repl_producer.mad \
    >"$rr_log" 2>&1 || fail "[replreuse] the freeze FAILED: $(tail -n 3 "$rr_log")"
rr_out=$(timeout 60 "$BIN" --forest-bind="$rr_snap" -I tmp/fbgate_repl -i tmp/fbgate_repl_consumer.mad \
    < tmp/fbgate_repl_input.txt 2>&1)
[ "$rr_out" = "$(printf 'main 3 10\n7\n42')" ] \
    || fail "[replreuse] REPL output '$rr_out' != 'main 3 10 / 7 / 42'"
timeout 60 "$BIN" -v --forest-bind="$rr_snap" -I tmp/fbgate_repl -i tmp/fbgate_repl_consumer.mad \
    < tmp/fbgate_repl_input.txt >"$rr_log" 2>/dev/null
grep -aq "bound to grove unit.*fbgate_repl.h" "$rr_log" \
    || fail "[replreuse] the consumer never bound the frozen header (vacuous)"
rm -rf "$rr_snap" "$rr_log" tmp/fbgate_repl tmp/fbgate_repl_producer.mad tmp/fbgate_repl_consumer.mad tmp/fbgate_repl_input.txt
echo "forest_bind_gate: [replreuse] OK — a REPL entry re-touching a frozen body gets its own nodes"

# --- case: replate (2026-10-15: a REPL entry's later #include binds) ---
# A header an interactive entry includes AFTER the session's first decl
# restore BINDS from the grove, and its records register: the restore runs
# again for every entry that bound a unit, registering what the wider closure
# admits (forest_restore_decls' per-record state). The struct's members, the
# class template and the inline function all resolve. Negative controls: the
# one-shot restore left the late unit's records out ("Unidentified member
# 'year'"), and its successor declined the bind so the header's text parsed —
# the output is right there, so the bound line below is what fails it.
mkdir -p tmp/fbgate_late
cat > tmp/fbgate_late/fbgate_late_a.h <<'EOF'
#ifndef FBGATE_LATE_A_H
#define FBGATE_LATE_A_H
inline int fbg_late_one() { return 1; }
#endif
EOF
cat > tmp/fbgate_late/fbgate_late_b.h <<'EOF'
#ifndef FBGATE_LATE_B_H
#define FBGATE_LATE_B_H
struct fbg_late_rec { int year; int day; };
template<class T> struct fbg_late_box { T v; T get() const { return v; } };
inline int fbg_late_sum(int a, int b) { return a + b; }
#endif
EOF
cat > tmp/fbgate_late_producer.mad <<'EOF'
#include <fbgate_late_a.h>
#include <fbgate_late_b.h>
int main() { fbg_late_box<int> b; b.v = fbg_late_one(); return b.get() - 1; }
EOF
cat > tmp/fbgate_late_consumer.mad <<'EOF'
#include <fbgate_late_a.h>
#include <cstdio>
int main() { printf("main %d\n", fbg_late_one()); return 0; }
EOF
cat > tmp/fbgate_late_input.txt <<'EOF'
#include <fbgate_late_b.h>
fbg_late_rec r; r.year = 5; printf("year=%d\n", r.year);
fbg_late_box<int> b; b.v = 7; printf("box=%d sum=%d\n", b.get(), fbg_late_sum(2, 3));
EOF
lt_snap=tmp/fbgate_late.msnap
lt_log=tmp/fbgate_late.log
rm -f "$lt_snap"
timeout 600 "$BIN" --freeze="$lt_snap" -I tmp/fbgate_late tmp/fbgate_late_producer.mad \
    >"$lt_log" 2>&1 || fail "[replate] the freeze FAILED: $(tail -n 3 "$lt_log")"
lt_out=$(timeout 60 "$BIN" --forest-bind="$lt_snap" -I tmp/fbgate_late -i tmp/fbgate_late_consumer.mad \
    < tmp/fbgate_late_input.txt 2>&1)
[ "$lt_out" = "$(printf 'main 1\nyear=5\nbox=7 sum=5')" ] \
    || fail "[replate] REPL output '$lt_out' != 'main 1 / year=5 / box=7 sum=5'"
timeout 60 "$BIN" -v --forest-bind="$lt_snap" -I tmp/fbgate_late -i tmp/fbgate_late_consumer.mad \
    < tmp/fbgate_late_input.txt >"$lt_log" 2>/dev/null
grep -aq "bound to grove unit.*fbgate_late_a.h" "$lt_log" \
    || fail "[replate] the consumer never bound its first header (vacuous)"
grep -aq "bound to grove unit.*fbgate_late_b.h" "$lt_log" \
    || fail "[replate] the entry's later #include did not BIND (its text parsed)"
rm -rf "$lt_snap" "$lt_log" tmp/fbgate_late tmp/fbgate_late_producer.mad tmp/fbgate_late_consumer.mad tmp/fbgate_late_input.txt
echo "forest_bind_gate: [replate] OK — a REPL entry's later #include binds and its records register"

# --- case: reprogate (2026-10-11: a freeze is reproducible) ---
# --dump-forest's `content` line digests the frozen corpus with the producer
# config left out. Two freezes of one TU under standards that parse it alike
# (gnu++17, c++17) print the same line — the freeze no longer walks its
# pointer-keyed pack maps in heap-address order (the pack's units once came
# out in a different order run to run). Negative control: a c++20 freeze
# takes other branches and must print a different line, so equal lines are
# not an artifact of the digest.
cat > tmp/fbgate_repro.cpp <<'EOF'
#include <cstddef>
#include <cstdio>
#include <cstdlib>
int main() { size_t n = 7; printf("n=%zu\n", n); return EXIT_SUCCESS; }
EOF
rp_content() {	# rp_content <std> -> the content line of a fresh freeze
    rm -f tmp/fbgate_repro.msnap
    timeout 600 "$BIN" --std="$1" --freeze=tmp/fbgate_repro.msnap tmp/fbgate_repro.cpp >/dev/null 2>&1 \
        || fail "[reprogate] the $1 --freeze FAILED"
    timeout 60 "$BIN" --dump-forest=tmp/fbgate_repro.msnap | grep '^content	'
}
rp_gnu=$(rp_content gnu++17)
rp_cxx17=$(rp_content c++17)
rp_cxx20=$(rp_content c++20)
[ -n "$rp_gnu" ] || fail "[reprogate] the dump printed no content line"
[ "$rp_gnu" = "$rp_cxx17" ] || fail "[reprogate] gnu++17 and c++17 froze one TU differently ($rp_gnu / $rp_cxx17)"
[ "$rp_gnu" != "$rp_cxx20" ] || fail "[reprogate] control: a c++20 freeze printed the same content (the digest is vacuous)"
rm -f tmp/fbgate_repro.msnap tmp/fbgate_repro.cpp
echo "forest_bind_gate: [reprogate] OK — gnu++17 and c++17 freeze one TU identically; c++20 differs"

# --- case: strbind (v12 corpus — the FIRST std:: library class bound end-to-end) ---
#     Freeze the REAL system <string>, then bind it while compiling a consumer that
#     default-CONSTRUCTS a std::string, ASSIGNS a C-string to it, SIZES it, and (at
#     scope exit) DESTROYS it. Before v12 the freeze skipped the class's ctors/dtor/
#     operators, so binding `std::string s;` failed "no matching constructor"; v12
#     serializes them (CIR_METHF_CTOR/DTOR) so cdd->ctors, the "~" dtor method_map
#     key, and operator= are restored, and the restored dtor is registered in
#     funcdef_map so the scope-exit cleanup declares it `void`. This is a RUNTIME-
#     CORRECTNESS gate (output == live == g++), NOT byte-identity: the whole <string>
#     TU is not yet MIR-identical (bind lacks the header's inline global vars +
#     __madc_global_init, and synthesizes trivial dtors live doesn't emit — both
#     separate whole-TU-emission slices). What it proves: a corpus class BINDS from
#     the forest (no re-parse) and the bound program constructs/assigns/sizes/destroys
#     correctly, cross-process.
strbind_prod="tmp/fbgate_strbind_producer.cpp"
strbind_cons="tmp/fbgate_strbind_consumer.cpp"
strbind_snap="tmp/fbgate_strbind.msnap"
strbind_gcc="tmp/fbgate_strbind_gcc"
strbind_vlog="tmp/fbgate_strbind_v.log"
cat > "$strbind_prod" <<'EOF'
#include <string>
int main() { std::string s; s = "hi"; return (int)s.size(); }
EOF
cat > "$strbind_cons" <<'EOF'
#include <string>
#include <cstdio>
// The consumer REFERENCES hardware_destructive_interference_size and in_place so
// the v14/v16 restoration asserts below stay meaningful under the rung-3
// referenced-surface filter (an UNreferenced system-header global no longer
// emits — the g++ COMDAT/ODR-use shape — on live and bind alike). The scalar
// is odr-used by ADDRESS: a VALUE read of a constexpr scalar folds to 64 on
// live and bind alike (not an odr-use), so the value check alone would leave
// the global unemitted on both sides and the v14 assert would test nothing.
int main() {
    std::string s; s = "hello";
    if (std::hardware_destructive_interference_size != 64) return 1;
    if (!&std::hardware_destructive_interference_size) return 1;
    if (!&std::in_place) return 1;
    printf("len=%d\n", (int)s.size());
    return 0;
}
EOF
if ! timeout 180 "$BIN" --freeze="$strbind_snap" "$strbind_prod" >/dev/null 2>&1; then
    fail "[strbind] --freeze <string> FAILED"
fi
[ -f "$strbind_snap" ] || fail "[strbind] --freeze produced no container"
strbind_live=$(timeout 60 "$BIN" "$strbind_cons" 2>/dev/null)
[ "$strbind_live" = "len=5" ] || fail "[strbind] live-parse output '$strbind_live' != 'len=5'"
if command -v g++ >/dev/null 2>&1; then
    if timeout 120 g++ "$strbind_cons" -o "$strbind_gcc" >/dev/null 2>&1; then
        strbind_gcc_out=$("$strbind_gcc" 2>/dev/null)
        [ "$strbind_gcc_out" = "len=5" ] || fail "[strbind] g++ output '$strbind_gcc_out' != 'len=5'"
    else
        fail "[strbind] g++ compile FAILED"
    fi
fi
strbind_bind=$(timeout 60 "$BIN" --forest-bind="$strbind_snap" "$strbind_cons" 2>/dev/null)
[ "$strbind_bind" = "len=5" ] || fail "[strbind] bind output '$strbind_bind' != 'len=5' (== live == g++)"
# Prove <string> actually BOUND from the container (no silent live fall-through).
# The -v dump carries NUL bytes, so grep -a (treat as text) on a file.
timeout 60 "$BIN" -v --forest-bind="$strbind_snap" "$strbind_cons" >"$strbind_vlog" 2>&1
if ! grep -aq "bound to grove unit.*string" "$strbind_vlog"; then
    rm -f "$strbind_snap" "$strbind_gcc" "$strbind_vlog"
    fail "[strbind] consumer did NOT bind the <string> grove (live fall-through?)"
fi
# v14: the bound <string> emits its scalar-const file-scope globals as data items,
# byte-identically to live (`hardware_destructive_interference_size: u64 64`). This
# is a whole-TU byte-identity SLICE (not the full TU, which still differs on
# in_place's {} value-init + the synth-dtor set): assert the scalar global's value
# is serialized + restored cross-process, not silently dropped.
strbind_mir="tmp/fbgate_strbind_mir.log"
MADC_DUMP_MIR=1 timeout 60 "$BIN" --forest-bind="$strbind_snap" "$strbind_cons" >/dev/null 2>"$strbind_mir"
if ! grep -Eq "hardware_destructive_interference_size:[[:space:]]+u64[[:space:]]+64" "$strbind_mir"; then
    rm -f "$strbind_snap" "$strbind_gcc" "$strbind_vlog" "$strbind_mir"
    fail "[strbind] bound <string> did NOT emit scalar-const global hardware_destructive_interference_size=64 (v14 regressed)"
fi
# Synth-dtor overshoot (#20): a live compile registers each restored class's own dtor
# (class_own_dtor non-NULL) and synthesizes NO Cls___dtor for it. When the freeze
# dropped a bodyless/symbolless inline dtor, the restored class looked dtor-less and
# bind synthesized a spurious trivial dtor a live compile never emits (e.g.
# _Save_errno___dtor — the consumer never touches _Save_errno). Serializing the dtor
# declaration-only fixed it. Assert the overshoot stays gone: bind must NOT emit a
# _Save_errno___dtor func-def (a class the consumer does not use).
if grep -Eq "^_Save_errno___dtor:[[:space:]]+func" "$strbind_mir"; then
    rm -f "$strbind_snap" "$strbind_gcc" "$strbind_vlog" "$strbind_mir"
    fail "[strbind] bound <string> emitted a spurious _Save_errno___dtor (synth-dtor overshoot #20 regressed)"
fi
# v16: the in_place global (an empty tag class with a `T x{}` value-init) used to be
# DROPPED — the flush ctors.empty() guard skipped it (its ctor was never serialized),
# so bind emitted neither `in_place: bss` nor `export in_place`, unlike live. v16
# serializes the class-global INITIALIZER FORM (VALUE_INIT/COPY_TEMP) + DataDefCLASS::
# nvsize, and stops pushing restored classes as dkStruct TopDecls (so their struct defs
# emit via Pass 0.5's class_member_list — with the empty-class `char __pad0[1]` — like
# live), so in_place restores + __madc_global_init's body is byte-identical to live.
# The global is a namespace-scope DEFINITION, so its emitted symbol is the
# Itanium _ZSt8in_place (live and bind alike — the bind carries the producer's
# symbol verbatim); a bare `in_place:` here means the bind dropped the alias.
if ! grep -Eq "^_ZSt8in_place:[[:space:]]+bss" "$strbind_mir"; then
	rm -f "$strbind_snap" "$strbind_gcc" "$strbind_vlog" "$strbind_mir"
	fail "[strbind] bound <string> did NOT restore the in_place global var (v16 regressed)"
fi
# RC2: a bound header restores its free-function DECLARATIONS, so the consumer's
# printf call resolves the real `int printf(const char *, ...)` signature and its
# extern proto emits TYPED (i32 return + typed pointer first param) exactly like
# live — never the dlsym implicit-variadic fallback (`i64, ...`), which mis-reads
# a signed-int return as a 64-bit long (the bsearch_skill_exact bug class).
if ! grep -Eq "proto[[:space:]]+i32, u64:U0_p0, \.\.\." "$strbind_mir"; then
    rm -f "$strbind_snap" "$strbind_gcc" "$strbind_vlog" "$strbind_mir"
    fail "[strbind] bound printf did NOT get its typed proto (free-function restore / RC2 regressed)"
fi
# #23 CLOSED: the WHOLE bound-<string> TU is byte-identical to a live parse.
# Requires (a) restored methods registered as funcdef_map[method-id] + program
# Variable (parseFunction's tail) so Pass 0.75 emits the ctor/dtor typed protos
# at live's sorted positions, and (b) SYSTEM-header forest bodies materializing
# inside the materialize_and_lower fixpoint (the loaded equivalent of a deferred
# lazy body) so the late tag-ctor/allocator-dtor definitions land AFTER main in
# fixpoint order, exactly like live. Any MIR-dump divergence is a regression.
strbind_live_mir="tmp/fbgate_strbind_live_mir.log"
MADC_DUMP_MIR=1 timeout 60 "$BIN" "$strbind_cons" >/dev/null 2>"$strbind_live_mir"
if ! diff -q "$strbind_live_mir" "$strbind_mir" >/dev/null 2>&1; then
    diff "$strbind_live_mir" "$strbind_mir" | head -40 >&2
    rm -f "$strbind_snap" "$strbind_gcc" "$strbind_vlog" "$strbind_mir" "$strbind_live_mir"
    fail "[strbind] bound <string> MIR dump is NOT byte-identical to live (#23 regressed; divergence above)"
fi
rm -f "$strbind_snap" "$strbind_gcc" "$strbind_vlog" "$strbind_mir" "$strbind_live_mir"
echo "forest_bind_gate: [strbind] OK — std::string bound from <string> grove (no re-parse); whole-TU MIR byte-identical to live (#23), output == live == g++"

# --- case: quietbind — a bound container adds NOTHING to a consumer's stderr ---
#     The flush re-derives a restored function's default arguments from their
#     frozen token runs, and a FREE function's re-derive waits until this TU
#     registers the function (forest_settle_param_defaults). A container frozen
#     from a <string> producer holds basic_string<wchar_t>'s iterator-pair
#     constructor instance — a compiler-derived identity, deferred until named
#     — whose `= allocator<wchar_t>()` names a product no consumer declares
#     until something promotes it. Re-derived at bind, EVERY consumer (a C file
#     with only <stdio.h> among them) printed "use of undeclared identifier
#     'allocator_wchar_t'" (stringfwd.h:71), and the packed release failed
#     every .expect_quiet test. The <string> consumer constructs std::string
#     and std::wstring from iterator pairs, taking the defaulted allocator: the
#     promoted instance's default re-derives then, and resolves.
qb_prod="tmp/fbgate_quietbind_producer.cpp"
qb_snap="tmp/fbgate_quietbind.msnap"
qb_c="tmp/fbgate_quietbind_c.c"
qb_cpp="tmp/fbgate_quietbind_cpp.cpp"
qb_str="tmp/fbgate_quietbind_str.cpp"
qb_gcc="tmp/fbgate_quietbind_gcc"
qb_err="tmp/fbgate_quietbind.err"
qb_clean() { rm -f "$qb_snap" "$qb_gcc" "$qb_err"; }
cat > "$qb_prod" <<'QBEOF'
#include <string>
#include <cstdio>
int main() { std::string s("x"); std::puts(s.c_str()); return 0; }
QBEOF
cat > "$qb_c" <<'QBEOF'
#include <stdio.h>
int main(void) { puts("c"); return 0; }
QBEOF
cat > "$qb_cpp" <<'QBEOF'
#include <cstdio>
int main() { std::puts("cpp"); return 0; }
QBEOF
cat > "$qb_str" <<'QBEOF'
#include <string>
#include <cstdio>
int main() {
    const char a[] = "abc";
    const wchar_t w[] = L"wxyz";
    std::string s(a, a + 3);
    std::wstring ws(w, w + 4);
    std::printf("s=%s ws=%d\n", s.c_str(), (int)ws.size());
    return 0;
}
QBEOF
if ! timeout 180 "$BIN" --freeze="$qb_snap" "$qb_prod" >/dev/null 2>&1; then
    qb_clean
    fail "[quietbind] --freeze <string> FAILED"
fi
[ -f "$qb_snap" ] || fail "[quietbind] --freeze produced no container"
if command -v g++ >/dev/null 2>&1; then
    if ! timeout 120 g++ "$qb_str" -o "$qb_gcc" >/dev/null 2>&1; then
        qb_clean
        fail "[quietbind] g++ compile FAILED"
    fi
    qb_gcc_out=$("$qb_gcc" 2>/dev/null)
    if [ "$qb_gcc_out" != "s=abc ws=4" ]; then
        qb_clean
        fail "[quietbind] g++ output '$qb_gcc_out' != 's=abc ws=4'"
    fi
fi
for qb in "$qb_c:c" "$qb_cpp:cpp" "$qb_str:s=abc ws=4"; do
    qb_src="${qb%%:*}"
    qb_exp="${qb#*:}"
    qb_live=$(timeout 60 "$BIN" "$qb_src" 2>/dev/null)
    if [ "$qb_live" != "$qb_exp" ]; then
        qb_clean
        fail "[quietbind] live-parse output of $qb_src '$qb_live' != '$qb_exp'"
    fi
    qb_out=$(timeout 60 "$BIN" --forest-bind="$qb_snap" "$qb_src" 2>"$qb_err")
    if [ "$qb_out" != "$qb_exp" ]; then
        qb_clean
        fail "[quietbind] bind output of $qb_src '$qb_out' != '$qb_exp' (== live)"
    fi
    if [ -s "$qb_err" ]; then
        sed 's/\x1b\[[0-9;]*m//g' "$qb_err" | head -6 >&2
        qb_clean
        fail "[quietbind] binding the <string> container wrote to $qb_src's stderr (above)"
    fi
done
qb_clean
echo "forest_bind_gate: [quietbind] OK — a bound <string> container leaves a C, a C++ and a <string> consumer's stderr empty; output == live == g++"

# --- case: strops (widening: restored-method OVERLOAD fidelity) ---
# A consumer exercising an overload SET on a bound class: append has 9 parsed
# overloads (const string&, const char*, initializer_list<char>, ...).
# findMethodOverload derives the hidden-__this skip from the method Variable's
# Method::owner_class — restored methods carried data==NULL, so every overload
# ranked at the wrong arity and resolution fell to the LAST method_map slot:
# s.append("!!") mis-picked append(initializer_list<char>) and failed
# "no matching constructor for call to 'initializer_list_char(char*)'".
# The restore now attaches Method(owner_class) at materialization (live parity
# with parseFunction's tail) and the flush shares the ONE Variable with
# tkProgram scope, exactly like a live parse.
strops_prod="tmp/fbgate_strops_producer.cpp"
strops_cons="tmp/fbgate_strops_consumer.cpp"
strops_snap="tmp/fbgate_strops.msnap"
strops_gcc="tmp/fbgate_strops_gcc"
cat > "$strops_prod" <<'EOF'
#include <string>
int main() { std::string s; s = "hi"; s += "!"; s.append("xy"); return (int)s.size() + (int)s.length(); }
EOF
cat > "$strops_cons" <<'EOF'
#include <string>
#include <cstdio>
int main() { std::string s; s = "hello"; s += " world"; s.append("!!"); printf("len=%d c0=%c\n", (int)s.length(), s.c_str()[0]); return 0; }
EOF
if ! timeout 180 "$BIN" --freeze="$strops_snap" "$strops_prod" >/dev/null 2>&1; then
    fail "[strops] --freeze <string> FAILED"
fi
[ -f "$strops_snap" ] || fail "[strops] --freeze produced no container"
strops_live=$(timeout 60 "$BIN" "$strops_cons" 2>/dev/null)
[ "$strops_live" = "len=13 c0=h" ] || fail "[strops] live-parse output '$strops_live' != 'len=13 c0=h'"
if command -v g++ >/dev/null 2>&1; then
    if timeout 120 g++ "$strops_cons" -o "$strops_gcc" >/dev/null 2>&1; then
        strops_gcc_out=$("$strops_gcc" 2>/dev/null)
        [ "$strops_gcc_out" = "len=13 c0=h" ] || fail "[strops] g++ output '$strops_gcc_out' != 'len=13 c0=h'"
    else
        fail "[strops] g++ compile FAILED"
    fi
fi
strops_bind=$(timeout 60 "$BIN" --forest-bind="$strops_snap" "$strops_cons" 2>/dev/null)
[ "$strops_bind" = "len=13 c0=h" ] || fail "[strops] bind output '$strops_bind' != 'len=13 c0=h' (== live == g++; overload fidelity regressed?)"
strops_live_mir="tmp/fbgate_strops_live_mir.log"
strops_bind_mir="tmp/fbgate_strops_bind_mir.log"
MADC_DUMP_MIR=1 timeout 60 "$BIN" "$strops_cons" >/dev/null 2>"$strops_live_mir"
MADC_DUMP_MIR=1 timeout 60 "$BIN" --forest-bind="$strops_snap" "$strops_cons" >/dev/null 2>"$strops_bind_mir"
if ! diff -q "$strops_live_mir" "$strops_bind_mir" >/dev/null 2>&1; then
    diff "$strops_live_mir" "$strops_bind_mir" | head -40 >&2
    rm -f "$strops_snap" "$strops_gcc" "$strops_live_mir" "$strops_bind_mir"
    fail "[strops] bound <string> overload-set MIR dump is NOT byte-identical to live (divergence above)"
fi
rm -f "$strops_snap" "$strops_gcc" "$strops_live_mir" "$strops_bind_mir"
echo "forest_bind_gate: [strops] OK — overload set (append/operator+=/c_str) resolves on a bound class; whole-TU MIR byte-identical to live, output == live == g++"

# --- case: vecbind (widening slice 2: template-NAME state) ---
# The corpus blocker: a bound <vector> consumer failed at PARSE — "use of
# undeclared identifier 'vector'" — because the instantiation PRODUCT class was
# in the arena but the template NAME was not. v20 serializes the parser's
# template pattern maps (TemplateDef token bodies in the .madh record form +
# params/flags/ns), the class-scope name maps (type_aliases / static member
# types / const values), canonical_cpp_spelling (arg-spelling identity — the
# instantiation-key memo), the producer's instantiated __mti / __ns_*__oN
# DEFINITIONS (bodied free functions with forest bodies riding the m&l
# fixpoint), and the extern-decl index (loaded bodies' runtime/library callee
# declarations load VERBATIM). An exact-match consumer memo-hits the restored
# product and runs correctly. Output-correctness + item-SET identity gate;
# whole-TU byte-identity (the residual is proto/label numbering order) is the
# follow-on, like strbind before #23.
vec_prod="tmp/fbgate_vec_producer.cpp"
vec_cons="tmp/fbgate_vec_consumer.cpp"
vec_snap="tmp/fbgate_vec.msnap"
vec_gcc="tmp/fbgate_vec_gcc"
vec_vlog="tmp/fbgate_vec_v.log"
cat > "$vec_prod" <<'EOF'
#include <vector>
int main() { std::vector<int> v; v.push_back(7); return v[0] + (int)v.size(); }
EOF
cat > "$vec_cons" <<'EOF'
#include <vector>
#include <cstdio>
int main() { std::vector<int> v; v.push_back(3); v.push_back(4); int s = 0; for (int i = 0; i < (int)v.size(); i++) s += v[i]; printf("sum=%d\n", s); return 0; }
EOF
if ! timeout 300 "$BIN" --freeze="$vec_snap" "$vec_prod" >/dev/null 2>&1; then
    fail "[vecbind] --freeze <vector> FAILED"
fi
[ -f "$vec_snap" ] || fail "[vecbind] --freeze produced no container"
vec_live=$(timeout 120 "$BIN" "$vec_cons" 2>/dev/null)
[ "$vec_live" = "sum=7" ] || fail "[vecbind] live-parse output '$vec_live' != 'sum=7'"
if command -v g++ >/dev/null 2>&1; then
    if timeout 120 g++ "$vec_cons" -o "$vec_gcc" >/dev/null 2>&1; then
        vec_gcc_out=$("$vec_gcc" 2>/dev/null)
        [ "$vec_gcc_out" = "sum=7" ] || fail "[vecbind] g++ output '$vec_gcc_out' != 'sum=7'"
    else
        fail "[vecbind] g++ compile FAILED"
    fi
fi
vec_bind=$(timeout 120 "$BIN" --forest-bind="$vec_snap" "$vec_cons" 2>/dev/null)
[ "$vec_bind" = "sum=7" ] || fail "[vecbind] bind output '$vec_bind' != 'sum=7' (== live == g++; template-name restore regressed?)"
# Prove <vector> actually BOUND from the container (no silent live fall-through).
timeout 120 "$BIN" -v --show-stats --forest-bind="$vec_snap" "$vec_cons" >"$vec_vlog" 2>&1
if ! grep -aq "bound to grove unit.*vector" "$vec_vlog"; then
    rm -f "$vec_snap" "$vec_gcc" "$vec_vlog"
    fail "[vecbind] consumer did NOT bind the <vector> grove (live fall-through?)"
fi
# Demand-driven derived-function restore must be observable on this exact-match
# consumer: the producer's cached instantiation products are deferred, the
# called subset activates, and a dead remainder stays unregistered.
if ! grep -aEq "forest functions \.{4} [0-9]+ eager \+ [1-9][0-9]* derived deferred; [1-9][0-9]* activated / [1-9][0-9]* remain" "$vec_vlog"; then
    rm -f "$vec_snap" "$vec_gcc" "$vec_vlog"
    fail "[vecbind] derived forest functions did not defer + activate on demand"
fi
# Item-SET identity vs live: every func/export/import present in live must be
# present in bind and vice versa (emission ORDER may still differ — the
# whole-TU byte-identity follow-on; a SET divergence is a real regression:
# a missing __mti/__ns_*__oN definition, a dropped method, a spurious synth).
vec_live_mir="tmp/fbgate_vec_live_mir.log"
vec_bind_mir="tmp/fbgate_vec_bind_mir.log"
MADC_DUMP_MIR=1 timeout 120 "$BIN" "$vec_cons" >/dev/null 2>"$vec_live_mir"
MADC_DUMP_MIR=1 timeout 120 "$BIN" --forest-bind="$vec_snap" "$vec_cons" >/dev/null 2>"$vec_bind_mir"
vec_sets_diverge=0
for vkind in "func" "export" "import"; do
    if [ "$vkind" = "func" ]; then
        vec_live_set=$(grep -aE "^[A-Za-z_][A-Za-z0-9_.$]*:[[:space:]]+func" "$vec_live_mir" | cut -d: -f1 | sort -u)
        vec_bind_set=$(grep -aE "^[A-Za-z_][A-Za-z0-9_.$]*:[[:space:]]+func" "$vec_bind_mir" | cut -d: -f1 | sort -u)
    else
        vec_live_set=$(grep -aE "^[[:space:]]+$vkind[[:space:]]" "$vec_live_mir" | awk '{print $2}' | sort -u)
        vec_bind_set=$(grep -aE "^[[:space:]]+$vkind[[:space:]]" "$vec_bind_mir" | awk '{print $2}' | sort -u)
    fi
    if [ "$vec_live_set" != "$vec_bind_set" ]; then
        echo "[vecbind] $vkind SET diverges (live vs bind):" >&2
        diff <(echo "$vec_live_set") <(echo "$vec_bind_set") | head -20 >&2
        vec_sets_diverge=1
    fi
done
if [ "$vec_sets_diverge" != "0" ]; then
    rm -f "$vec_snap" "$vec_gcc" "$vec_vlog" "$vec_live_mir" "$vec_bind_mir"
    fail "[vecbind] bound <vector> item sets diverge from live (see above)"
fi
rm -f "$vec_gcc" "$vec_vlog" "$vec_live_mir" "$vec_bind_mir"
echo "forest_bind_gate: [vecbind] OK — std::vector<int> bound from <vector> grove (template-name state restored, no re-parse); func/export/import sets == live, output == live == g++"

# --- case: vecnewspec (new-specialization instantiation from restored tokens) ---
# The consumer instantiates a specialization the producer NEVER built
# (vector<long> from a vector<int> producer): template-name resolution, the
# ns-fn-template placeholder chain (std::_Destroy), member-template patterns
# (_Destroy_aux::__destroy), enums (std::align_val_t), out-of-line member
# definitions (vector.tcc _M_realloc_insert), overload-set ranking (aligned
# operator new) and Itanium-bound throw helpers all run from restored state.
# Reuses the vecbind producer snapshot ($vec_snap — freeze once, bind twice).
vec_ns_cons="tmp/fbgate_vec_ns_consumer.cpp"
vec_ns_gcc="tmp/fbgate_vec_ns_gcc"
cat > "$vec_ns_cons" <<'EOF'
#include <vector>
#include <cstdio>
int main() { std::vector<long> w; w.push_back(40); w.push_back(2); long t = 0; for (int i = 0; i < (int)w.size(); i++) t += w[i]; printf("t=%ld n=%d\n", t, (int)w.size()); return 0; }
EOF
vec_ns_live=$(timeout 120 "$BIN" "$vec_ns_cons" 2>/dev/null)
[ "$vec_ns_live" = "t=42 n=2" ] || fail "[vecnewspec] live-parse output '$vec_ns_live' != 't=42 n=2'"
if command -v g++ >/dev/null 2>&1; then
    if timeout 120 g++ "$vec_ns_cons" -o "$vec_ns_gcc" >/dev/null 2>&1; then
        vec_ns_gcc_out=$("$vec_ns_gcc" 2>/dev/null)
        [ "$vec_ns_gcc_out" = "t=42 n=2" ] || fail "[vecnewspec] g++ output '$vec_ns_gcc_out' != 't=42 n=2'"
    else
        fail "[vecnewspec] g++ compile FAILED"
    fi
fi
vec_ns_bind=$(timeout 120 "$BIN" --forest-bind="$vec_snap" "$vec_ns_cons" 2>/dev/null)
[ "$vec_ns_bind" = "t=42 n=2" ] || fail "[vecnewspec] bind output '$vec_ns_bind' != 't=42 n=2' (== live == g++; new-spec instantiation from restored tokens regressed?)"
rm -f "$vec_snap" "$vec_ns_cons" "$vec_ns_gcc"
echo "forest_bind_gate: [vecnewspec] OK — NEW specialization (vector<long>) instantiated from restored tokens, output == live == g++"

# --- case: mapbind (exact-match <map> bind: the v22 map burn-down) ---
# A bound <map> consumer exercises the state families vector never touched:
# member-template ctor PLACEHOLDERS restored verbatim at their saved __oN ranks
# (the CIR_TMPLK_MEMBER flush HYDRATES them instead of re-minting a shifted
# family), bodied member-template INSTANTIATIONS (pair(...)__oN — loaded
# _Rb_tree bodies call them directly), a dtor referenced ONLY via a
# scope-local's cleanup attribute inside a LOADED body (_Auto_node's __z —
# cir_collect_cleanup_attr_fns), and class-scope name resolution through
# restored type_aliases (pair<iterator,bool>). Freeze + bind the SAME file.
map_src="tmp/fbgate_map_src.cpp"
map_snap="tmp/fbgate_map.msnap"
map_gcc="tmp/fbgate_map_gcc"
map_vlog="tmp/fbgate_map_v.log"
cat > "$map_src" <<'EOF'
#include <map>
#include <cstdio>
int main() { std::map<int,int> m; m[1] = 41; m[2] = 1; int s = 0; for (std::map<int,int>::iterator it = m.begin(); it != m.end(); ++it) s += it->second; printf("sum=%d\n", s); return 0; }
EOF
map_live=$(timeout 120 "$BIN" "$map_src" 2>/dev/null)
[ "$map_live" = "sum=42" ] || fail "[mapbind] live-parse output '$map_live' != 'sum=42'"
if command -v g++ >/dev/null 2>&1; then
    if timeout 120 g++ "$map_src" -o "$map_gcc" >/dev/null 2>&1; then
        map_gcc_out=$("$map_gcc" 2>/dev/null)
        [ "$map_gcc_out" = "sum=42" ] || fail "[mapbind] g++ output '$map_gcc_out' != 'sum=42'"
    else
        fail "[mapbind] g++ compile FAILED"
    fi
fi
if ! timeout 300 "$BIN" --freeze="$map_snap" "$map_src" >/dev/null 2>&1; then
    fail "[mapbind] --freeze <map> FAILED"
fi
[ -f "$map_snap" ] || fail "[mapbind] --freeze produced no container"
map_bind=$(timeout 120 "$BIN" --forest-bind="$map_snap" "$map_src" 2>/dev/null)
[ "$map_bind" = "sum=42" ] || fail "[mapbind] bind output '$map_bind' != 'sum=42' (== live == g++; member-template method restore / cleanup-attr callee collection regressed?)"
# Prove <map> actually BOUND from the container (no silent live fall-through).
timeout 120 "$BIN" -v --forest-bind="$map_snap" "$map_src" >"$map_vlog" 2>&1
if ! grep -aq "bound to grove unit.*map" "$map_vlog"; then
    rm -f "$map_snap" "$map_gcc" "$map_vlog"
    fail "[mapbind] consumer did NOT bind the <map> grove (live fall-through?)"
fi
rm -f "$map_vlog"
echo "forest_bind_gate: [mapbind] OK — exact-match <map> bind (member-template ranks + bodies, cleanup-attr dtor), output == live == g++"

# --- case: mapnewspec (new map specialization from restored tokens) ---
# map<long,long> from a map<int,int> producer: a fresh _Rb_tree instantiation
# resolves std::piecewise_construct by QUALIFIED name through the restored
# global's v22 namespace binding (namespace_map[ns][name]), and instantiates
# through the hydrated member-template patterns. KNOWN NUANCE (recorded in the
# close-out handoff): the fresh instantiation binds pair(...)__o7's reference
# params to a derived-node pointer without live's materialized base-ptr
# conversion temp (2 benign c2mir pointer warnings on stderr; stdout — the
# gate's oracle — matches live == g++).
map_ns_cons="tmp/fbgate_map_ns_consumer.cpp"
map_ns_gcc="tmp/fbgate_map_ns_gcc"
cat > "$map_ns_cons" <<'EOF'
#include <map>
#include <cstdio>
int main() { std::map<long,long> m; m[10] = 40; m[20] = 2; long s = 0; for (std::map<long,long>::iterator it = m.begin(); it != m.end(); ++it) s += it->second; printf("t=%ld n=%d\n", s, (int)m.size()); return 0; }
EOF
map_ns_live=$(timeout 120 "$BIN" "$map_ns_cons" 2>/dev/null)
[ "$map_ns_live" = "t=42 n=2" ] || fail "[mapnewspec] live-parse output '$map_ns_live' != 't=42 n=2'"
if command -v g++ >/dev/null 2>&1; then
    if timeout 120 g++ "$map_ns_cons" -o "$map_ns_gcc" >/dev/null 2>&1; then
        map_ns_gcc_out=$("$map_ns_gcc" 2>/dev/null)
        [ "$map_ns_gcc_out" = "t=42 n=2" ] || fail "[mapnewspec] g++ output '$map_ns_gcc_out' != 't=42 n=2'"
    else
        fail "[mapnewspec] g++ compile FAILED"
    fi
fi
map_ns_bind=$(timeout 180 "$BIN" --forest-bind="$map_snap" "$map_ns_cons" 2>/dev/null)
[ "$map_ns_bind" = "t=42 n=2" ] || fail "[mapnewspec] bind output '$map_ns_bind' != 't=42 n=2' (== live == g++; global ns binding / member-template hydration regressed?)"
rm -f "$map_snap" "$map_src" "$map_gcc" "$map_ns_cons" "$map_ns_gcc"
echo "forest_bind_gate: [mapnewspec] OK — NEW specialization (map<long,long>) instantiated from restored state, output == live == g++"

# --- case: iobind (<iostream>: polymorphic classes + extern-ref globals) ---
# `std::cout << 7 << std::endl` from a bound forest: vtable-carrying classes
# restore (greatest-fixpoint closure admits the basic_ios<->basic_ostream
# pointer cycle; DK_FPTR fn-ptr member records resolve _Callback_list), cout
# restores as a CIR_GLOBALF_EXTERN_REF (vfEXTERN Variable + Itanium
# _ZSt4cout alias + `extern` TopDecl), and the W2 manipulator surface
# (free_operator_overloads) re-derives from the restored patterns so endl
# binds mangled-direct (_ZSt4endl...) instead of the placeholder symbol.
io_src="tmp/fbgate_io_src.cpp"
io_snap="tmp/fbgate_io.msnap"
io_gcc="tmp/fbgate_io_gcc"
io_vlog="tmp/fbgate_io_v.log"
cat > "$io_src" <<'EOF'
#include <iostream>
int main() { std::cout << 7 << std::endl; return 0; }
EOF
io_live=$(timeout 180 "$BIN" "$io_src" 2>/dev/null)
[ "$io_live" = "7" ] || fail "[iobind] live-parse output '$io_live' != '7'"
if command -v g++ >/dev/null 2>&1; then
    if timeout 120 g++ "$io_src" -o "$io_gcc" >/dev/null 2>&1; then
        io_gcc_out=$("$io_gcc" 2>/dev/null)
        [ "$io_gcc_out" = "7" ] || fail "[iobind] g++ output '$io_gcc_out' != '7'"
    else
        fail "[iobind] g++ compile FAILED"
    fi
fi
if ! timeout 600 "$BIN" --freeze="$io_snap" "$io_src" >/dev/null 2>&1; then
    fail "[iobind] --freeze <iostream> FAILED"
fi
[ -f "$io_snap" ] || fail "[iobind] --freeze produced no container"
io_bind=$(timeout 180 "$BIN" --forest-bind="$io_snap" "$io_src" 2>/dev/null)
[ "$io_bind" = "7" ] || fail "[iobind] bind output '$io_bind' != '7' (== live == g++; polymorphic-class / extern-ref / manipulator restore regressed?)"
# Prove <iostream> actually BOUND from the container (no silent live fall-through).
timeout 180 "$BIN" -v --forest-bind="$io_snap" "$io_src" >"$io_vlog" 2>&1
if ! grep -aq "bound to grove unit.*iostream" "$io_vlog"; then
    rm -f "$io_snap" "$io_gcc" "$io_vlog"
    fail "[iobind] consumer did NOT bind the <iostream> grove (live fall-through?)"
fi
rm -f "$io_snap" "$io_src" "$io_gcc" "$io_vlog"
echo "forest_bind_gate: [iobind] OK — <iostream> bound (polymorphic classes + extern-ref cout + mangled-direct endl), output == live == g++"

# --- case: traitfold — a trait-call NTTP base must NOT constant-fold at
#     CAPTURE time. The freeze's pattern-capture parse of gcc13's
#     `is_assignable : __bool_constant<__is_assignable(_Tp,_Up)>` folded the
#     trait with UNBOUND params to 0 and froze false_type as the pattern's
#     base — every bound is_assignable<To,From> then read ::value == 0
#     regardless of its args (testtraitassign, packed leg). The local trait
#     fold now REFUSES dependent args (read_local_type_arg), deferring to
#     instantiation. Negative control: the packed battery failed exactly this
#     shape before the refusal guard landed.
cat > tmp/fbgate_traitfold.h <<'EOF'
#ifndef FBGATE_TRAITFOLD_H
#define FBGATE_TRAITFOLD_H
template<bool __v> struct fbg_bool_constant { static const bool value = __v; };
template<typename _Tp, typename _Up>
struct fbg_is_assignable
    : public fbg_bool_constant<__is_assignable(_Tp, _Up)> { };
#endif
EOF
cat > tmp/fbgate_traitfold_producer.cpp <<'EOF'
#include <fbgate_traitfold.h>
int main() { return 0; }
EOF
cat > tmp/fbgate_traitfold_consumer.cpp <<'EOF'
#include <fbgate_traitfold.h>
#include <cstdio>
int main() {
    printf("%d %d %d\n",
	   (int)fbg_is_assignable<int&, int&&>::value,
	   (int)fbg_is_assignable<int, int>::value,
	   (int)fbg_is_assignable<int&, int&>::value);
    return 0;
}
EOF
run_case traitfold "1 0 1"

# --- case: nothrowfold — a noexcept(...) over a DEPENDENT operand must NOT
#     constant-fold at CAPTURE time (traitfold's twin, 2026-10-11). The
#     capture parse read gcc13's `__is_nt_destructible_impl`'s
#     `decltype(__test<_Tp>(0))` with `_Tp` a placeholder: the pseudo-
#     destructor `~_Tp()` named no class, so `noexcept(declval<_Tp&>().~_Tp())`
#     answered 1 as for a scalar and the pattern froze integral_constant<bool,
#     true> — every bound is_nothrow_destructible<X> read 1, a noexcept(false)
#     destructor included (testnoexceptderefdtor, packed leg). The walk now
#     refuses a dependent operand, and a refusal poisons the capture (the class
#     stays on the parse lane). Negative control: the pre-fix binary binds
#     "1 1 1".
cat > tmp/fbgate_nothrowfold.h <<'EOF'
#ifndef FBGATE_NOTHROWFOLD_H
#define FBGATE_NOTHROWFOLD_H
#include <type_traits>
struct fbg_do_nt_destructible {
    template<typename _Tp>
    static std::integral_constant<bool, noexcept(std::declval<_Tp&>().~_Tp())>
    __test(int);
    template<typename>
    static std::false_type __test(...);
};
template<typename _Tp>
struct fbg_nt_destructible_impl : public fbg_do_nt_destructible {
    typedef decltype(__test<_Tp>(0)) type;
};
template<typename _Tp>
struct fbg_is_nt_destructible : public fbg_nt_destructible_impl<_Tp>::type { };
#endif
EOF
cat > tmp/fbgate_nothrowfold_producer.cpp <<'EOF'
#include <fbgate_nothrowfold.h>
int main() { return 0; }
EOF
cat > tmp/fbgate_nothrowfold_consumer.cpp <<'EOF'
#include <fbgate_nothrowfold.h>
#include <cstdio>
struct Thr { ~Thr() noexcept(false) {} };
struct Quiet { ~Quiet() {} };
int main() {
    printf("%d %d %d\n",
	   (int)fbg_is_nt_destructible<Thr>::value,
	   (int)fbg_is_nt_destructible<Quiet>::value,
	   (int)fbg_is_nt_destructible<int>::value);
    return 0;
}
EOF
run_case nothrowfold "0 1 1"

# --- case: explicitctor — a restored class keeps its constructor's `explicit`
#     (2026-10-11, forest v54). The forest never recorded FuncDef::is_explicit:
#     a frozen reverse_iterator<__normal_iterator<int*, vector<int>>> came back
#     with `explicit reverse_iterator(iterator_type)` as a CONVERTING ctor, so
#     inside std::operator==(const reverse_iterator&, const reverse_iterator&)
#     the body's `__x.base() == __y.base()` ranked that very operator viable
#     for two __normal_iterator operands and recursed until the stack ran out
#     (testvectoriterpairctor, packed leg). The ctor's explicitness now rides
#     the method record (MF_EXPLICIT) and the class-pattern method payload.
#     The producer instantiates W<It<int>> so the consumer restores it;
#     W<It<long>> is a new specialization. Negative control: the v53 binary's
#     bound consumer dies of SIGSEGV.
cat > tmp/fbgate_explicitctor.h <<'EOF'
#ifndef FBGATE_EXPLICITCTOR_H
#define FBGATE_EXPLICITCTOR_H
namespace fbgB {
template<class T> struct It { T v; };
template<class T> bool operator==(const It<T> &a, const It<T> &b) { return a.v == b.v; }
}
namespace fbgA {
template<class I> struct W {
    I cur;
    W() {}
    explicit W(I x) : cur(x) {}
    I base() const { return cur; }
};
template<class I> bool operator==(const W<I> &x, const W<I> &y) { return x.base() == y.base(); }
}
inline int fbg_explicitctor_touch() { fbgA::W<fbgB::It<int> > w; return w.cur.v; }
#endif
EOF
cat > tmp/fbgate_explicitctor_producer.cpp <<'EOF'
#include <fbgate_explicitctor.h>
int main() { return 0; }
EOF
cat > tmp/fbgate_explicitctor_consumer.cpp <<'EOF'
#include <fbgate_explicitctor.h>
#include <cstdio>
int main() {
    fbgB::It<int> i = { 1 }, j = { 2 };
    fbgA::W<fbgB::It<int> > a(i), b(j);
    fbgB::It<long> k = { 3 }, l = { 3 };
    fbgA::W<fbgB::It<long> > c(k), d(l);
    printf("%d %d %d\n", (int)(a == b), (int)(a == a), (int)(c == d));
    return 0;
}
EOF
run_case explicitctor "0 1 1"

# --- case: deletedctor — deleted copy/move metadata must survive freeze/bind.
#     FuncDef keeps the two special-member declarations distinct because its
#     DataDefREF parameter type intentionally erases `&` versus `&&`. The
#     consumer exercises both positive and negative constructibility results
#     after the class declarations are restored from the grove.
cat > tmp/fbgate_deletedctor.h <<'EOF'
#ifndef FBGATE_DELETEDCTOR_H
#define FBGATE_DELETEDCTOR_H
struct FbgCopyDeleted {
	int n;
	FbgCopyDeleted() : n(1) { }
	FbgCopyDeleted(const FbgCopyDeleted&) = delete;
	FbgCopyDeleted(FbgCopyDeleted&&) = default;
};
struct FbgMoveDeleted {
	int n;
	FbgMoveDeleted() : n(2) { }
	FbgMoveDeleted(const FbgMoveDeleted&) = default;
	FbgMoveDeleted(FbgMoveDeleted&&) = delete;
};
#endif
EOF
cat > tmp/fbgate_deletedctor_producer.cpp <<'EOF'
#include <fbgate_deletedctor.h>
int main() { return 0; }
EOF
cat > tmp/fbgate_deletedctor_consumer.cpp <<'EOF'
#include <fbgate_deletedctor.h>
#include <cstdio>
int main() {
	printf("%d %d %d %d\n",
	       (int)__is_constructible(FbgCopyDeleted, FbgCopyDeleted&),
	       (int)__is_constructible(FbgCopyDeleted, FbgCopyDeleted&&),
	       (int)__is_constructible(FbgMoveDeleted, FbgMoveDeleted&),
	       (int)__is_constructible(FbgMoveDeleted, FbgMoveDeleted&&));
	return 0;
}
EOF
run_case deletedctor "0 1 1 0"

# --- case: constcopy — a bound class's `const X&` copy constructor stays one.
#     A restored parameter keeps its recorded const flag (paramrec
#     PF_CONST_PARAM -> FuncDef::const_params), which the copy constructor's
#     selection reads: without it a member's `X(const X&)` cannot bind the
#     const source of an implicit memberwise copy, no constructor is chosen,
#     and the member is bit-copied — exit 0 with "v=1 live=1 after=-1" here
#     (std::string as such a member: a double free, tests/testimplcopy).
cat > tmp/fbgate_constcopy.h <<'EOF'
#ifndef FBGATE_CONSTCOPY_H
#define FBGATE_CONSTCOPY_H
struct FbgCounted {
	int *live;
	int v;
	FbgCounted(int *l, int x) : live(l), v(x) { ++*live; }
	FbgCounted(const FbgCounted &o) : live(o.live), v(o.v + 100) { ++*live; }
	~FbgCounted() { --*live; }
};
#endif
EOF
cat > tmp/fbgate_constcopy_producer.cpp <<'EOF'
#include <fbgate_constcopy.h>
int main() { return 0; }
EOF
cat > tmp/fbgate_constcopy_consumer.cpp <<'EOF'
#include <fbgate_constcopy.h>
#include <cstdio>
struct Holder { FbgCounted c; int n; Holder(int *l, int k) : c(l, k), n(k) { } ~Holder() { } };
int main() {
	int live = 0;
	{
		Holder a(&live, 1);
		Holder b(a);	// implicit copy: FbgCounted(const FbgCounted&) runs
		printf("v=%d live=%d", b.c.v, live);
	}
	printf(" after=%d\n", live);
	return 0;
}
EOF
run_case constcopy "v=101 live=2 after=0"

# --- case: subbind (THE OWNER'S BAR: a REAL integration test on the forest) ---
# tests/testsubscript.mad (string/array subscripting, <string> + <map> whole)
# freeze+bind == live == its .expect fixture. The last family that flipped it:
# v23 DEFAULT ARGUMENTS — a restored method's `= _Alloc()` / `= npos` /
# `= io_errc::stream` re-derives param_defaults from the captured raw-token
# runs (paramrec.def_tok_*), re-run through parseExpression at the flush
# inside the owner's class + namespace scope. Without it `string greet =
# "hello"` found no matching basic_string ctor.
sub_snap="tmp/fbgate_sub.msnap"
sub_vlog="tmp/fbgate_sub_v.log"
sub_test="tests/testsubscript.mad"
[ -f "$sub_test" ] || fail "[subbind] tests/testsubscript.mad missing"
sub_live=$(timeout 180 "$BIN" "$sub_test" 2>/dev/null)
[ -n "$sub_live" ] || fail "[subbind] live-parse produced no output"
if ! timeout 600 "$BIN" --freeze="$sub_snap" "$sub_test" >/dev/null 2>&1; then
    fail "[subbind] --freeze testsubscript.mad FAILED"
fi
[ -f "$sub_snap" ] || fail "[subbind] --freeze produced no container"
sub_bind=$(timeout 180 "$BIN" --forest-bind="$sub_snap" "$sub_test" 2>/dev/null)
[ "$sub_bind" = "$sub_live" ] || fail "[subbind] bind output differs from live (default-arg / restored-method state regressed?)"
while IFS= read -r line; do
    [ -z "$line" ] && continue
    printf '%s\n' "$sub_bind" | grep -qF -- "$line" \
        || fail "[subbind] expected line missing from bind output: $line"
done < tests/testsubscript.expect
# Prove the headers actually BOUND from the container (no silent live fall-through).
timeout 180 "$BIN" -v --forest-bind="$sub_snap" "$sub_test" >"$sub_vlog" 2>&1
if ! grep -aq "bound to grove unit" "$sub_vlog"; then
    rm -f "$sub_snap" "$sub_vlog"
    fail "[subbind] consumer did NOT bind the grove (live fall-through?)"
fi
rm -f "$sub_snap" "$sub_vlog"
echo "forest_bind_gate: [subbind] OK — OWNER'S BAR: tests/testsubscript.mad freeze+bind == live == .expect (default arguments restored)"

# --- case: redecl — the v40 mixed bind/live redeclaration seam (task #57) ----
# The wchar.h/stdio.h FILE shape. A shared `#ifndef FBG_T_DEFINED` block lives
# in BOTH a.h and b.h; c.h re-declares the typedef (C11 6.7p3: a typedef may be
# redefined to the same type — the C spelling of cstdio's `using ::fbgT` re-export).
# Both corpora are C TUs: a mixed bind/live TU is C's alone — its restored
# structs and typedefs merge with a live twin — while a C++ TU's system content
# is all-bound or all-live (K1a, docs/plans/2026-10-18-verified-cxx-bind.md):
# corpus 1 compiled as C++ stops binding at its first decline (checked last).
# Corpus 1 (sub-block guard MASKED-BIND + tolerance): the freeze parses a.h's
# copy first, so b.h's frozen `#ifndef FBG_T_DEFINED` arm was SKIPPED and
# captured. The consumer includes b.h FIRST: the bind walk (K2) takes that arm
# here, runs it — its define leaves FBG_T_DEFINED as the freeze held it — and
# b.h binds with the arm's struct fbg_T + typedef fbgT parsed LIVE. Then c.h:
# a.h's taken `#ifndef FBG_T_DEFINED` arm is skipped here, and it is a guard
# a.h itself defines whose state already matches, while a.h's file is NOT on
# the live-tokenized record ⇒ the MASKED-BIND arm: a.h BINDS (never pruned —
# that would lose its non-shared content; never declined — that cascades)
# with the arm's events skipped, and the declarations made inside the skipped
# arm are WITHDRAWN (K2d): gcc declares nothing there, and b.h's run arm
# already declared struct fbg_T + typedef fbgT live.
# Corpus 2 (real include-once guard, genuine live evidence): a.h consults
# FBG_SPICE (defined at freeze by spice.h, absent live) so root a.h declines
# and live-parses — its file IS on the live-tokenized record. c.h then binds
# with a.h pruned on its REAL guard (the honest-evidence prune); same
# tolerance tail. The pair pins the prune/inert-bind boundary.
# Corpus 3 (a restored twin with no arm in it): d.h consults FBG_SPICE and
# declines, so it parses live; e.h binds and restores its own typedef fbgU
# beside d.h's live one. The main parse must TOLERATE the twin
# (DataDef::denotes_same_type, C11 6.7p3), never throw.
cat > tmp/fbgate_redecl_a.h <<'EOF'
#ifndef FBG_REDECL_A_H
#define FBG_REDECL_A_H
#ifndef FBG_T_DEFINED
#define FBG_T_DEFINED
struct fbg_T { int x; int y; };
typedef struct fbg_T fbgT;
#endif
#ifdef FBG_SPICE
typedef int fbg_spice_seen;
#endif
#endif
EOF
cat > tmp/fbgate_redecl_b.h <<'EOF'
#ifndef FBG_REDECL_B_H
#define FBG_REDECL_B_H
#ifndef FBG_T_DEFINED
#define FBG_T_DEFINED
struct fbg_T { int x; int y; };
typedef struct fbg_T fbgT;
#endif
static int fbg_b_use(fbgT *p) { return p ? p->x : 0; }
#endif
EOF
cat > tmp/fbgate_redecl_c.h <<'EOF'
#ifndef FBG_REDECL_C_H
#define FBG_REDECL_C_H
#include <fbgate_redecl_a.h>
typedef struct fbg_T fbgT;
#endif
EOF
cat > tmp/fbgate_redecl_spice.h <<'EOF'
#ifndef FBG_REDECL_SPICE_H
#define FBG_REDECL_SPICE_H
#define FBG_SPICE 1
#endif
EOF
cat > tmp/fbgate_redecl_e.h <<'EOF'
#ifndef FBG_REDECL_E_H
#define FBG_REDECL_E_H
struct fbg_U;
typedef struct fbg_U fbgU;
static int fbg_e_use(fbgU *p);
#endif
EOF
cat > tmp/fbgate_redecl_d.h <<'EOF'
#ifndef FBG_REDECL_D_H
#define FBG_REDECL_D_H
struct fbg_U { int x; int y; };
typedef struct fbg_U fbgU;
#ifdef FBG_SPICE
typedef int fbg_spice_seen_d;
#endif
#endif
EOF
cat > tmp/fbgate_redecl3_producer.c <<'EOF'
#include <fbgate_redecl_spice.h>
#include <fbgate_redecl_e.h>
#include <fbgate_redecl_d.h>
int main() { return 0; }
EOF
cat > tmp/fbgate_redecl3_consumer.c <<'EOF'
#include <fbgate_redecl_d.h>
#include <fbgate_redecl_e.h>
#include <stdio.h>
static int fbg_e_use(fbgU *p) { return p->x + p->y; }
int main() { fbgU t; t.x = 5; t.y = 2; printf("v=%d\n", fbg_e_use(&t)); return 0; }
EOF
cat > tmp/fbgate_redecl_producer.c <<'EOF'
#include <fbgate_redecl_c.h>
#include <fbgate_redecl_b.h>
int main() { return 0; }
EOF
cat > tmp/fbgate_redecl_consumer.c <<'EOF'
#include <fbgate_redecl_b.h>
#include <fbgate_redecl_c.h>
#include <stdio.h>
int main() { fbgT t; t.x = 5; t.y = 2; printf("v=%d\n", fbg_b_use(&t) + t.y); return 0; }
EOF
cat > tmp/fbgate_redecl2_producer.c <<'EOF'
#include <fbgate_redecl_spice.h>
#include <fbgate_redecl_c.h>
int main() { return 0; }
EOF
cat > tmp/fbgate_redecl2_consumer.c <<'EOF'
#include <fbgate_redecl_a.h>
#include <fbgate_redecl_c.h>
#include <stdio.h>
int main() { fbgT t; t.x = 5; t.y = 2; printf("v=%d\n", t.x + t.y); return 0; }
EOF
rd_snap="tmp/fbgate_redecl.msnap"
rd_snap2="tmp/fbgate_redecl2.msnap"
rd_snap3="tmp/fbgate_redecl3.msnap"
rd_vlog="tmp/fbgate_redecl_v.log"
rd_vlog2="tmp/fbgate_redecl2_v.log"
rd_gcc="tmp/fbgate_redecl_gcc"
for c in tmp/fbgate_redecl_consumer.c tmp/fbgate_redecl2_consumer.c tmp/fbgate_redecl3_consumer.c; do
    rd_live=$(timeout 60 "$BIN" --std=gnu17 "$c" -I tmp 2>/dev/null)
    [ "$rd_live" = "v=7" ] || fail "[redecl] live-parse of $c gave '$rd_live' != 'v=7'"
    if command -v gcc >/dev/null 2>&1; then
        if timeout 120 gcc -I tmp "$c" -o "$rd_gcc" >/dev/null 2>&1; then
            rd_gcc_out=$("$rd_gcc" 2>/dev/null)
            [ "$rd_gcc_out" = "v=7" ] || fail "[redecl] gcc output for $c '$rd_gcc_out' != 'v=7'"
        else
            fail "[redecl] gcc compile of $c FAILED"
        fi
    fi
done
timeout 120 "$BIN" --std=gnu17 --freeze="$rd_snap" tmp/fbgate_redecl_producer.c -I tmp >/dev/null 2>&1 \
    || fail "[redecl] --freeze corpus 1 FAILED"
timeout 120 "$BIN" --std=gnu17 --freeze="$rd_snap2" tmp/fbgate_redecl2_producer.c -I tmp >/dev/null 2>&1 \
    || fail "[redecl] --freeze corpus 2 FAILED"
timeout 120 "$BIN" --std=gnu17 --freeze="$rd_snap3" tmp/fbgate_redecl3_producer.c -I tmp >/dev/null 2>&1 \
    || fail "[redecl] --freeze corpus 3 FAILED"
# Corpus 1: b.h binds through its replayed arm; a.h's sub-block-guard
# mismatch MASKED-BINDS (never pruned, never declined) and withdraws the
# skipped arm's declarations; output still exact.
rd_bind=$(timeout 60 "$BIN" --std=gnu17 --forest-bind="$rd_snap" tmp/fbgate_redecl_consumer.c -I tmp 2>/dev/null)
[ "$rd_bind" = "v=7" ] || fail "[redecl] corpus-1 bind output '$rd_bind' != 'v=7'"
timeout 60 "$BIN" -v --std=gnu17 --forest-bind="$rd_snap" tmp/fbgate_redecl_consumer.c -I tmp >"$rd_vlog" 2>&1
grep -aq "fbgate_redecl_b.h replays its skipped '#ifndef FBG_T_DEFINED' arm (its declarations parse live)" "$rd_vlog" \
    || fail "[redecl] root b.h did NOT replay its skipped arm — the live side of the seam is gone"
grep -aq "bound to grove unit.*fbgate_redecl_b.h" "$rd_vlog" \
    || fail "[redecl] corpus-1 b.h did NOT bind"
grep -aq "fbgate_redecl_a.h '#ifndef FBG_T_DEFINED' arm skipped — its guard is live (masked bind)" "$rd_vlog" \
    || fail "[redecl] corpus-1 a.h's sub-block guard did NOT take the masked-bind arm"
grep -aq "fbgate_redecl_a.h already present" "$rd_vlog" \
    && fail "[redecl] corpus-1 a.h was PRUNED without live-tokenize evidence (over-claim regressed)"
grep -aq "DECLINE root tmp/fbgate_redecl_c.h" "$rd_vlog" \
    && fail "[redecl] corpus-1 root c.h DECLINED — the masked-bind arm did not bind"
grep -aq "bound to grove unit.*fbgate_redecl_c.h" "$rd_vlog" \
    || fail "[redecl] corpus-1 c.h did NOT bind"
for nm in fbg_T fbgT; do
    grep -aq "fbgate_redecl_a.h withdraws '$nm'" "$rd_vlog" \
	|| fail "[redecl] corpus-1 a.h's skipped arm did NOT withdraw $nm (K2d)"
done
grep -aq "forest_restore_decls: typedef fbgT" "$rd_vlog" \
    && fail "[redecl] corpus-1 restored the withdrawn fbgT twin"
grep -aq "Identifier already defined" "$rd_vlog" \
    && fail "[redecl] corpus 1 hit the redeclaration throw"
# Corpus 2: real-guard prune fires. (Under C the freeze indexes c.h's repeated
# typedef under a.h, its first declarer, so this corpus restores no twin —
# corpus 1 pins the twin tolerance; this one pins the prune.)
rd_bind2=$(timeout 60 "$BIN" --std=gnu17 --forest-bind="$rd_snap2" tmp/fbgate_redecl2_consumer.c -I tmp 2>/dev/null)
[ "$rd_bind2" = "v=7" ] || fail "[redecl] corpus-2 bind output '$rd_bind2' != 'v=7'"
timeout 60 "$BIN" -v --std=gnu17 --forest-bind="$rd_snap2" tmp/fbgate_redecl2_consumer.c -I tmp >"$rd_vlog2" 2>&1
grep -aq "DECLINE root tmp/fbgate_redecl_a.h" "$rd_vlog2" \
    || fail "[redecl] corpus-2 root a.h did NOT decline (FBG_SPICE dep lost?)"
grep -aq "fbgate_redecl_a.h already present ('FBG_REDECL_A_H' guard live)" "$rd_vlog2" \
    || fail "[redecl] corpus-2 a.h was NOT pruned on its REAL include-once guard"
grep -aq "bound to grove unit.*fbgate_redecl_c.h" "$rd_vlog2" \
    || fail "[redecl] corpus-2 c.h did NOT bind"
grep -aq "Identifier already defined" "$rd_vlog2" \
    && fail "[redecl] corpus 2 hit the redeclaration throw"
# Corpus 3: d.h declines and parses live; e.h binds and restores its fbgU twin.
rd_bind3=$(timeout 60 "$BIN" --std=gnu17 --forest-bind="$rd_snap3" tmp/fbgate_redecl3_consumer.c -I tmp 2>/dev/null)
[ "$rd_bind3" = "v=7" ] || fail "[redecl] corpus-3 bind output '$rd_bind3' != 'v=7'"
timeout 60 "$BIN" -v --std=gnu17 --forest-bind="$rd_snap3" tmp/fbgate_redecl3_consumer.c -I tmp >"$rd_vlog2" 2>&1
grep -aq "DECLINE root tmp/fbgate_redecl_d.h" "$rd_vlog2" \
    || fail "[redecl] corpus-3 root d.h did NOT decline (FBG_SPICE dep lost?)"
grep -aq "bound to grove unit.*fbgate_redecl_e.h" "$rd_vlog2" \
    || fail "[redecl] corpus-3 e.h did NOT bind"
grep -aq "forest_restore_decls: typedef fbgU" "$rd_vlog2" \
    || fail "[redecl] corpus-3 restored fbgU twin never registered — the collision precondition is gone"
grep -aq "Identifier already defined" "$rd_vlog2" \
    && fail "[redecl] corpus 3 hit the redeclaration throw (denotes_same_type tolerance regressed)"
# Corpus 1 as C++ (its own C++ freeze — a C++ compile never reads a C profile,
# K0): nothing declines now, so the TU binds as the C one does (K1a's stop is
# [allornone]'s).
cp tmp/fbgate_redecl_producer.c tmp/fbgate_redecl_producer_cxx.cpp
cp tmp/fbgate_redecl_consumer.c tmp/fbgate_redecl_consumer_cxx.cpp
timeout 120 "$BIN" --std=gnu++17 --freeze="$rd_snap" tmp/fbgate_redecl_producer_cxx.cpp -I tmp >/dev/null 2>&1 \
    || fail "[redecl] --freeze corpus 1 (C++) FAILED"
rd_cxx=$(timeout 60 "$BIN" --std=gnu++17 --forest-bind="$rd_snap" tmp/fbgate_redecl_consumer_cxx.cpp -I tmp 2>/dev/null)
[ "$rd_cxx" = "v=7" ] || fail "[redecl] corpus-1 C++ consumer output '$rd_cxx' != 'v=7'"
timeout 60 "$BIN" -v --std=gnu++17 --forest-bind="$rd_snap" tmp/fbgate_redecl_consumer_cxx.cpp -I tmp >"$rd_vlog" 2>&1
grep -aq "bound to grove unit.*fbgate_redecl_c.h" "$rd_vlog" \
    || fail "[redecl] corpus-1 C++ consumer did NOT bind c.h"
grep -aq "DECLINE root" "$rd_vlog" \
    && fail "[redecl] corpus-1 C++ consumer declined: $(grep -a -m1 'DECLINE root' "$rd_vlog")"
rm -f "$rd_snap" "$rd_snap2" "$rd_snap3" "$rd_gcc" "$rd_vlog" "$rd_vlog2" tmp/fbgate_redecl_consumer_cxx.cpp tmp/fbgate_redecl_producer_cxx.cpp
echo "forest_bind_gate: [redecl] OK — (C) replayed arm live + masked bind withdraws the skipped arm's declarations; honest-evidence prune pinned; a declined root's live twin of a restored typedef tolerated (denotes_same_type); output == live == gcc; (C++) binds the same"

# --- case: husk — missing-content recovery (task #57; K2's arm replay) -------
# The mingw stdlib.h -> errno.h shape in miniature. pre.h defines the shared
# sub-block guard BEFORE first including husk.h, so that frozen unit contains
# only its file guard: its typedef/value block is absent (a skipped arm). root.h
# later records an edge to that already-included husk beside an independently
# bindable sibling. On the consumer the shared guard is absent. Binding the
# frozen husk as frozen would silently lose content; declining the WHOLE root
# throws away the sibling and root too. The bind walk (K2) takes the arm the
# freeze skipped: it runs it in place — its guard define leaves the state the
# freeze held, its new FBG_HUSK_VALUE was undefined there — and its typedef
# parses live, while root + sibling + husk bind. This recovery is
# correctness-critical.
cat > tmp/fbgate_husk_pre.h <<'EOF'
#ifndef FBG_HUSK_PRE_H
#define FBG_HUSK_PRE_H
#define FBG_HUSK_SHARED_GUARD 1
#include <fbgate_husk.h>
#endif
EOF
cat > tmp/fbgate_husk.h <<'EOF'
#ifndef FBG_HUSK_H
#define FBG_HUSK_H
#ifndef FBG_HUSK_SHARED_GUARD
#define FBG_HUSK_SHARED_GUARD 1
typedef int fbg_husk_t;
#define FBG_HUSK_VALUE 7
#endif
#endif
EOF
cat > tmp/fbgate_husk_sibling.h <<'EOF'
#ifndef FBG_HUSK_SIBLING_H
#define FBG_HUSK_SIBLING_H
typedef int fbg_husk_sibling_t;
static int fbg_husk_sibling(void) { return 5; }
#endif
EOF
cat > tmp/fbgate_husk_root.h <<'EOF'
#ifndef FBG_HUSK_ROOT_H
#define FBG_HUSK_ROOT_H
#include <fbgate_husk_sibling.h>
#include <fbgate_husk.h>
typedef int fbg_husk_root_t;
#endif
EOF
cat > tmp/fbgate_husk_producer.cpp <<'EOF'
#include <fbgate_husk_pre.h>
#include <fbgate_husk_root.h>
int main() { return 0; }
EOF
cat > tmp/fbgate_husk_consumer.cpp <<'EOF'
#include <fbgate_husk_root.h>
#include <cstdio>
int main()
{
    fbg_husk_t n = FBG_HUSK_VALUE;
    fbg_husk_root_t r = 0;
    fbg_husk_sibling_t s = fbg_husk_sibling();
    printf("v=%d\n", n + r + s);
    return 0;
}
EOF
hs_snap="tmp/fbgate_husk.msnap"
hs_gcc="tmp/fbgate_husk_gcc"
hs_vlog="tmp/fbgate_husk_v.log"
hs_exp="v=12"
hs_live=$(timeout 60 "$BIN" tmp/fbgate_husk_consumer.cpp -I tmp 2>/dev/null)
[ "$hs_live" = "$hs_exp" ] || fail "[husk] live-parse output '$hs_live' != '$hs_exp'"
if command -v g++ >/dev/null 2>&1; then
    if timeout 120 g++ -I tmp tmp/fbgate_husk_consumer.cpp -o "$hs_gcc" >/dev/null 2>&1; then
	hs_or=$("$hs_gcc" 2>/dev/null)
	[ "$hs_or" = "$hs_exp" ] || fail "[husk] g++ output '$hs_or' != '$hs_exp'"
    else
	fail "[husk] g++ compile FAILED"
    fi
fi
timeout 120 "$BIN" --freeze="$hs_snap" tmp/fbgate_husk_producer.cpp -I tmp >/dev/null 2>&1 \
    || fail "[husk] --freeze FAILED"
[ -f "$hs_snap" ] || fail "[husk] --freeze produced no container"
hs_bind=$(timeout 60 "$BIN" --forest-bind="$hs_snap" tmp/fbgate_husk_consumer.cpp -I tmp 2>/dev/null)
[ "$hs_bind" = "$hs_exp" ] \
    || fail "[husk] bind output '$hs_bind' != '$hs_exp'"
timeout 60 "$BIN" -v --forest-bind="$hs_snap" tmp/fbgate_husk_consumer.cpp -I tmp >"$hs_vlog" 2>&1
grep -aq "fbgate_husk.h replays its skipped '#ifndef FBG_HUSK_SHARED_GUARD' arm (its declarations parse live)" "$hs_vlog" \
    || fail "[husk] did NOT replay the husk's skipped arm"
grep -aq "DECLINE root.*fbgate_husk_root.h" "$hs_vlog" \
    && fail "[husk] declined the whole root"
grep -aq "bound to grove unit.*fbgate_husk_root.h" "$hs_vlog" \
    || fail "[husk] root did NOT bind"
grep -aq "forest_restore_decls: typedef fbg_husk_sibling_t" "$hs_vlog" \
    || fail "[husk] sibling decl did NOT restore"
grep -aq "forest_restore_decls: typedef fbg_husk_root_t" "$hs_vlog" \
    || fail "[husk] root decl did NOT restore"
grep -aq "forest_restore_decls: typedef fbg_husk_t" "$hs_vlog" \
    && fail "[husk] restored the frozen husk decl instead of using live tokens"
rm -f "$hs_snap" "$hs_gcc" "$hs_vlog"
echo "forest_bind_gate: [husk] OK — the missing content is the husk's skipped arm, replayed live; root + sibling + husk bound; output == live == g++"

# --- case: silbody — a static-inline header fn passed BY ADDRESS through a
# bound template (task #57, the mingw strtof/__stoa shape). fbg_strtof is a
# static __inline__ DEFINITION carrying a BLOCK-SCOPE prototype of its
# delegate (mingw stdlib.h's exact layout); fbg_stof passes &fbg_strtof as a
# template call argument. At bind, both ride forest bodies; the arg-ref
# collector must route the address-referenced sibling through
# referenced_funcs so its DEFINITION materializes (it is not an extern DECL,
# so the extern-index lookup alone drops it: "undeclared identifier").
cat > tmp/fbgate_silbody.h <<'EOF'
#ifndef FBG_SILBODY_H
#define FBG_SILBODY_H
static __inline__ float fbgsb_conv(const char *s, char **e)
{
    float fbgsb_delegate(const char *, char **);
    return fbgsb_delegate(s, e);
}
template <typename _TRet, typename _Ret = _TRet>
_Ret fbgsb_stoa(_TRet (*conv)(const char *, char **), const char *s)
{
    char *e = 0;
    return conv(s, &e);
}
inline float fbgsb_stof(const char *s)
{
    return fbgsb_stoa(&fbgsb_conv, s);
}
#endif
EOF
cat > tmp/fbgate_silbody_producer.cpp <<'EOF'
#include <fbgate_silbody.h>
int main() { return 0; }
EOF
cat > tmp/fbgate_silbody_consumer.cpp <<'EOF'
#include <fbgate_silbody.h>
#include <cstdio>
float fbgsb_delegate(const char *s, char **e) { (void)s; (void)e; return 9.0f; }
int main() { printf("t=%d\n", (int)fbgsb_stof("2.5")); return 0; }
EOF
sb_snap="tmp/fbgate_silbody.msnap"
sb_gcc="tmp/fbgate_silbody_gcc"
sb_live=$(timeout 60 "$BIN" tmp/fbgate_silbody_consumer.cpp -I tmp 2>/dev/null)
[ "$sb_live" = "t=9" ] || fail "[silbody] live-parse output '$sb_live' != 't=9'"
if command -v g++ >/dev/null 2>&1; then
    if timeout 120 g++ -I tmp tmp/fbgate_silbody_consumer.cpp -o "$sb_gcc" >/dev/null 2>&1; then
        sb_or=$("$sb_gcc" 2>/dev/null)
        [ "$sb_or" = "t=9" ] || fail "[silbody] g++ output '$sb_or' != 't=9'"
    else
        fail "[silbody] g++ compile FAILED"
    fi
fi
timeout 120 "$BIN" --freeze="$sb_snap" tmp/fbgate_silbody_producer.cpp -I tmp >/dev/null 2>&1 \
    || fail "[silbody] --freeze FAILED"
[ -f "$sb_snap" ] || fail "[silbody] --freeze produced no container"
sb_bind=$(timeout 60 "$BIN" --forest-bind="$sb_snap" tmp/fbgate_silbody_consumer.cpp -I tmp 2>/dev/null)
[ "$sb_bind" = "t=9" ] || fail "[silbody] bind output '$sb_bind' != 't=9' (address-referenced static-inline sibling not materialized?)"
rm -f "$sb_snap" "$sb_gcc"
echo "forest_bind_gate: [silbody] OK — address-referenced static-inline sibling materializes through the bind, output == live == g++"

# --- case: secvptr — a class with a SECONDARY polymorphic base restored from the
#     grove, whose INLINE ctor is loaded and emitted. The struct emitter names
#     the secondary vptr fields from the layout-time secondary_vptr_owners list;
#     the ctor stamps them from vtable_groups. Only the groups are frozen, so a
#     restored class refills the list from them (secondary_vptr_owners_from_groups).
#     Without the refill the bound `struct C` lacked `__vptr_16` and the loaded
#     ctor's stamp of it was a c2mir "struct has no member" error (libc++
#     basic_iostream<char>, the darwin-host istringstream family). `pb->fb()`
#     dispatches through the secondary vptr, so a wrong stamp is a wrong answer.
cat > tmp/fbgate_secvptr.h <<'EOF'
#ifndef FBGATE_SECVPTR_H
#define FBGATE_SECVPTR_H
struct FbgA { long a; FbgA() : a(1) {} virtual ~FbgA() {} virtual int fa() { return 10; } };
struct FbgB { long b; FbgB() : b(2) {} virtual ~FbgB() {} virtual int fb() { return 20; } };
struct FbgC : FbgA, FbgB { long c; FbgC() : c(3) {} int fb() { return 21; } };
#endif
EOF
cat > tmp/fbgate_secvptr_producer.cpp <<'EOF'
#include <fbgate_secvptr.h>
int main() { FbgC x; return x.fa(); }
EOF
cat > tmp/fbgate_secvptr_consumer.cpp <<'EOF'
#include <fbgate_secvptr.h>
#include <cstdio>
int main()
{
    FbgC x;
    FbgB *pb = &x;
    printf("fb=%d fa=%d sum=%ld sz=%d\n", pb->fb(), x.fa(), x.a + x.b + x.c, (int)sizeof(FbgC));
    return 0;
}
EOF
run_case secvptr "fb=21 fa=10 sum=6 sz=40"

# --- case: friendgrant — a class whose PRIVATE member is read by its hidden-friend
#     operators (one plain, one a TEMPLATE-HEAD friend), restored from the grove.
#     The friendship grants (friend_function_names / friend_class_names) are
#     parse-time access state; before v43 they were never frozen, so a hoisted
#     hidden-friend body instantiated against the restored class was refused its
#     private read, the instantiation failed silently, and the operator fell to
#     the raw C operator (libc++ <iomanip>'s __iom_t5 inserter, testiomanip on
#     the darwin runner — pack-bound only). Both operators must bind and answer
#     exactly as live and g++ do.
cat > tmp/fbgate_friendgrant.h <<'EOF'
#ifndef FBGATE_FRIENDGRANT_H
#define FBGATE_FRIENDGRANT_H
class FbfA {
	int n;
public:
	FbfA(int v) : n(v) {}
	friend int operator+(const FbfA &a, int k) { return a.n + k; }
	template <class T>
	friend T operator*(const FbfA &a, T k) { return a.n * k; }
};
#endif
EOF
cat > tmp/fbgate_friendgrant_producer.cpp <<'EOF'
#include <fbgate_friendgrant.h>
int main() { FbfA a(1); return a + 1; }
EOF
cat > tmp/fbgate_friendgrant_consumer.cpp <<'EOF'
#include <fbgate_friendgrant.h>
#include <cstdio>
int main()
{
    FbfA a(7);
    printf("plus=%d times=%d\n", a + 1, a * 2);
    return 0;
}
EOF
run_case friendgrant "plus=8 times=14"

# --- case: patternalias — a class template partial specialization whose
#     class-scope alias is `decltype` of a call on the template parameter
#     (`using A = decltype(addr_of(declv<It>()))` — libc++'s
#     __unwrap_iter_impl<_Iter,true>::_ToAddressT), frozen from the grove and
#     instantiated by the consumer for NEW arguments. The freezer's class-pattern
#     CAPTURE parse resolved that decltype against the placeholder `It`, the
#     deduction failed, and the alias was captured as a CONCRETE int64 — so every
#     pattern materialization (the frozen exemplar and each consumer
#     instantiation) typed `A` as long long: c2mir `invalid operand types of -`
#     in std::vector copies on the darwin runner (4 tests). Live and g++ agree
#     on the per-instantiation type; the bound consumer must too.
cat > tmp/fbgate_patternalias.h <<'EOF'
#ifndef FBGATE_PATTERNALIAS_H
#define FBGATE_PATTERNALIAS_H
template<class T> T* fbpa_addr_of(T* p) { return p; }
template<class T> T fbpa_declv();
template<class It, bool = true> struct FbpaImpl;
template<class It>
struct FbpaImpl<It, true> {
	using A = decltype(fbpa_addr_of(fbpa_declv<It>()));
	static A un(It i) { return fbpa_addr_of(i); }
};
#endif
EOF
cat > tmp/fbgate_patternalias_producer.cpp <<'EOF'
#include <fbgate_patternalias.h>
int main() { char c = 'x'; char *p = FbpaImpl<char*>::un(&c); return *p == 'x' ? 0 : 1; }
EOF
cat > tmp/fbgate_patternalias_consumer.cpp <<'EOF'
#include <fbgate_patternalias.h>
#include <cstdio>
int main()
{
    int x = 1; long y = 2;
    int *p = FbpaImpl<int*>::un(&x);
    long *q = FbpaImpl<long*>::un(&y);
    printf("%d %ld\n", *p, *q);
    return 0;
}
EOF
run_case patternalias "1 2"

# NOTE: this tally is hand-maintained, and that bit me — the [ldouble] case ran
# and passed while the summary still said 25/25, so the line UNDERSTATED
# coverage. Worse in the other direction: deleting a case would leave this line
# still claiming it runs. Deriving it from run_case would need the ~12 bespoke
# cases below to register too; until then, update it when you add a case.
echo "forest_bind_gate: GREEN 47/47 — typedef + struct + nested + bitfield + class + method + fwd + ptr + nestedenumfn + ldouble + ns + anon + declonlymt + flavorgate + strbind + strops + vecbind + vecnewspec + mapbind + mapnewspec + iobind + traitfold + nothrowfold + explicitctor + armreplay + armreplayc + division + allornone + condrec + k2walk + replreuse + deletedctor + constcopy + subbind + redecl + husk + silbody grove headers bound (missing content replayed from the skipped arm), output == live == g++ + secvptr + friendgrant + patternalias + quietbind + memberptr + unionclass"
exit 0
