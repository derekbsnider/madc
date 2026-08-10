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
    cat > "$sm_cons" <<'EOF'
#include <cstdio>
#include <locale>
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
// emits — the g++ COMDAT/ODR-use shape — on live and bind alike).
int main() {
    std::string s; s = "hello";
    if (std::hardware_destructive_interference_size != 64) return 1;
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
if ! grep -Eq "^in_place:[[:space:]]+bss" "$strbind_mir"; then
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
timeout 120 "$BIN" -v --forest-bind="$vec_snap" "$vec_cons" >"$vec_vlog" 2>&1
if ! grep -aq "bound to grove unit.*vector" "$vec_vlog"; then
    rm -f "$vec_snap" "$vec_gcc" "$vec_vlog"
    fail "[vecbind] consumer did NOT bind the <vector> grove (live fall-through?)"
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

echo "forest_bind_gate: GREEN — typedef + struct + nested + bitfield + class + method + fwd + ptr + ns + anon + declonlymt + flavorgate + strbind + strops + vecbind + vecnewspec + mapbind + mapnewspec + iobind + traitfold + subbind grove headers bound (no re-parse), output == live == g++"
exit 0
