# madc bug backlog

Defects found while working on something else, filed so the current work
keeps moving and fixed later in a burndown the owner schedules.

**Owner pause, 2026-09-25, LIFTED 2026-09-28:** while the REPL arc's first
slice was built (plan §37), a defect found off the REPL's path was filed here
instead of being fixed on the spot. With §37 complete, the owner lifted the
pause, and this backlog is being burned down. A defect found now is fixed in
its own commit when it can be (`fix-what-you-find.md`). One that is not fixed
on the spot is filed here, so it stays tracked (owner, 2026-09-29).

- One entry per defect: kind, when and during what it was found, the reducer
  inline (`tmp/` is untracked), what gcc, clang and madc do, and the layer
  when known.
- **SILENT** entries (exit 0, wrong value) come first and are fixed first.
- A fix is its own commit, with the reducer promoted to `tests/` with both
  oracles and a `CHANGELOG.md` line. The same commit deletes the entry here.

All outputs were measured 2026-09-25 with `bin/madc` at `8ffd42b8a`, gcc 13
and clang 18. The madc flags are `--std=c17` for `.c` files and
`--std=c++17` for `.cpp` files.

## Silent wrong answers

Found 2026-09-29 while fixing the aggregate and member attribute readers
(c77129ab2, 6671bd11a). Measured that day with `bin/madc` at 6671bd11a,
gcc 13 and clang 18.

### B62. A dependent `alignas` on a member function template's local is dropped

What remains of B62 after slices 1-3 of
`docs/plans/2026-09-29-alignment-specifiers.md`, which made `_Alignas`,
`alignas` and `aligned` align members, variables and class heads.

```cpp
#include <cstdio>
#include <cstddef>
template<class U> struct Box
{
	U v;
	std::size_t f() { char k = 1; alignas(U) char c = 0; return __alignof__(c) + c + k - 1; }
	template<class V> std::size_t h() { alignas(V) char c = 0; return __alignof__(c) + c; }
};
struct alignas(16) W { char c; };
int main()
{
	Box<double> bd; Box<W> bw;
	std::printf("d4: %zu %zu %zu %zu\n", bd.f(), bw.f(), bd.h<W>(), bw.h<double>());
	return 0;
}
```

- g++ = clang++: `d4: 8 16 16 8`. madc: `d4: 8 16 1 1`.
- Where: a member function template's body is a parse-once (tsubst)
  pattern. The pattern's `alignas(V)` operand is dependent, so the reader
  contributes nothing and the local keeps its type's alignment. Fix shape
  (plan slice 4): the local keeps the operand's type query and emits
  `_Alignas((int)_Alignof(V))` (c2mir wants a signed constant), which the
  dependent `N_ALIGNOF` fold makes concrete (a type-id operand's alignof
  defers since B68's fix); `__alignof__(c)` defers with the constant part as
  its `measure_floor` (a251fcd0d).
- Measured 2026-09-29: no libstdc++ 13 or libc++ 18 header has this shape.
  `<atomic>`'s `alignas(_Tp) unsigned char __buf[sizeof(_Tp)]` sits in
  non-template members of class templates and in free function templates,
  both parsed per instantiation and already right (d4's first two columns).
- The fix carries a dependent operand through the declaration parser's
  `size_t` alignment plumbing (`parsing_decl_align`, `decl_align` /
  `object_align`, `consume_gnu_attributes`, `consume_object_attributes`,
  `apply_declaration_storage`, `push_declarator_list_tail`) and `Variable`.
  Owner ruling pending: a focused session for that change.

### B70. A class-scope static with an in-class initializer has no storage unless madc folds it to an integer

```cpp
#include <cstdio>
struct V { int a; int b; };
struct Y
{
	static constexpr unsigned long s[] = { 1, 2, 3 };
	static constexpr unsigned long n = sizeof(s) / sizeof(s[0]);
	static constexpr double d = 1.5;
	static constexpr const char *p = "hi";
	static constexpr V v = { 3, 4 };
	static inline double x = 2.5;
};
template<class T> struct A { static constexpr T v = T(3.5); };
int main()
{
	std::printf("py: %lu %zu %g %s %g %g %d\n", Y::n, sizeof(Y::s), Y::d, Y::p, Y::x, A<double>::v, A<int>::v);
	return 0;
}
```

- g++ = clang++: `py: 3 24 1.5 hi 2.5 3.5 3`. madc:
  `py: 0 8 6.89921e-310 (null) 6.95332e-310 6.21027e-310 0`. Every
  non-integral member reads garbage or `(null)`, and `A<int>::v` reads 0. `Y::v.b` is refused, and `Y::s[i]` desyncs the parse
  ("Expecting brace after function declaration").
- Found 2026-09-29 during B62, filed first as the unsized-array `sizeof`
  alone. Reducers: `tmp/b62/b70.cpp`, `b70a`–`b70f`.
- Not affected: out-of-class definitions (`const double Y::d = 1.5;`, the
  `const int Y::t[4]` array, a struct, a `char *`). All of them match g++
  (`tmp/b62/b70e.cpp`). An in-class integral constant that
  `capture_constant_initializer_value` folds (`static const int n = 5;`)
  also matches g++.
- Layer: the class body's static data member arm (TokenCLASS::parse,
  `has_inclass_init`) creates storage only for a member with NO in-class
  initializer. It folds an integral initializer and structurally skips
  everything else. `resolve_class_static_member_value` then finds neither
  storage nor a constant and returns `TokenInt(0)` typed as the member.
  That fallback is where every read goes wrong silently.
- The missing feature is C++17 inline variables at class scope. A
  `constexpr` or `inline` static data member is a definition
  ([dcl.inline]/1, [class.static.data]/4). It needs storage with its
  initializer, bound as linkonce (`vfLINKONCE`, which namespace-scope
  `inline` variables already use through `apply_declaration_storage`), and
  an unsized array bound deduced from its initializer.
- Open design points for the focused session:
  - Where the initializer is parsed: at the declaration, or on first use
    as g++ emits an inline variable only when it is odr-used.
  - The class-template path: the member initializer must be substituted per
    specialization.
  - The system-header blast radius: libstdc++ and libc++ declare many
    in-class-initialized statics that madc skips today.

### B65. A C++ class body drops its members' attributes

One construct per file (each alone), in a struct that needs the class
parser (it has a member function):

```cpp
struct C1 { int f() { return x; } char c; int x __attribute__((aligned(16))); };   // c4: sizeof
struct C2 { int f() { return x; } char c; int __attribute__((aligned(16))) x; };   // c5: sizeof
struct C3 { int f() { return x; } char c; int x __attribute__((packed)); };        // c6: sizeof
struct AS { int f() { return x; } char c; alignas(16) int x; };                    // c8: sizeof
```

- g++ = clang++: `c4: 32`, `c5: 32`, `c6: 5`, `c8: 32`. madc: `c4: 8`,
  `c6: 8`, `c8: 8` (SILENT); c5 refused, `Failed to find type when parsing
  function parameters`.
- The class's own attributes are laid out since 61e6739ab
  (`tests/testclassattributes`). A member's are not: the class parser's
  `skip_member_attributes` reads and discards them, and the cv reads before
  and after the member's type (`skip_cv_qualifier_tokens`) take no
  attribute group.
- Blocked on B77 and B78. Applying a member's attributes evaluates their
  operands, and libstdc++'s `__aligned_membuf` declares
  `alignas(__alignof__(_Tp2::_M_t)) unsigned char _M_storage[...]`, a
  qualified name through a nested data-only struct. With B77 open that
  refuses every `std::list` / node-handle use
  (`testforeachiter`, `testlateinstproto`, `testphpdumpiter`,
  `testptrcmpupcast`); with B78 open its alignment would be `int`'s.
- The fix, once both land: read the member groups with
  `consume_object_attributes` / `consume_cv_and_object_attributes` into a
  line alignment and packing, and lay out each data member through
  `apply_member_layout_attributes`, as `TokenSTRUCT::parse` does.
  `g++.dg/cpp0x/alignas5.C` then leaves the gxx-c++11 baseline.

### B78. A qualified non-static member in `sizeof` has `int`'s size

```cpp
#include <cstdio>
struct C { double t; char a[12]; int f() { return 0; } };
int main() { std::printf("q: %zu %zu\n", sizeof(C::t), sizeof(C::a)); return 0; }
```

- g++ = clang++: `q: 8 12`. madc: `q: 4 1` (SILENT). Reducer: `tmp/b65r/v7.cpp`.
- Where: `resolve_class_qualified_expression`'s no-object arm (a member
  named without an object, valid only in an unevaluated operand,
  [expr.prim.id]/2) pushes `TokenInt(0)` "typed as the member", but
  `TokenInt::setDataType` accepts only an integer or complex type, so a
  `double`, pointer, struct or array member stays `int`. `alignof` reads it
  the same way. The operand needs a token that carries any type, with the
  member's array extents and its own alignment.
- Found 2026-09-29 while tracing the B65 member regression. Core expression
  parser: a focused session.

### B76. A bit-field under packing never straddles its type's window

```c
#include <stdio.h>
#include <string.h>
#pragma pack(8)
struct P8 { char a : 4; int b : 30; char c; };
#pragma pack()
struct __attribute__((packed)) PK { char a : 4; short s : 14; char c; long l : 40; };
struct BF { char a : 4; int b : 30 __attribute__((packed)); };
int main(void)
{
	struct P8 p = { 0, 1, 0 };
	unsigned char raw[sizeof p];
	memcpy(raw, &p, sizeof p);
	struct PK k = { 1, -3, 7, 0x123456789aLL };
	struct BF f = { 2, 123456789 };
	printf("sb: %zu %zu %zu %d %d %lx %d\n", sizeof(struct P8), sizeof(struct PK), sizeof(struct BF), raw[0], k.s, (unsigned long)k.l, f.b);
	return 0;
}
```

- gcc = clang: `sb: 8 9 5 16 -3 123456789a 123456789`. madc:
  `sb: 12 13 8 0 -3 123456789a 123456789`. The values survive inside madc;
  the layout does not match gcc, so sizes, offsets and binary data differ.
- Found 2026-09-29 while fixing B63 (a member's own `packed`). Reducer:
  `tmp/b62/b76.c`.
- gcc's rule (stor-layout `place_field`): a bit-field may not span more
  windows of its declared type than the type itself, EXCEPT when it is
  packed or a `#pragma pack` is active. Under any pack (`pack(8)`
  included), the field starts at the next free bit.
  `DataDefSTRUCT::allocateBitField`'s SysV path applies the window rule
  unconditionally.
- Layer below: c2mir reads and writes a bit-field through one load/store of
  its declared type at its settled byte offset (`emit_scalar_assign`, the
  bit-field extract before `gen_unary_op`). A field spanning more bits than
  that type from its first byte (`int b : 30` at bit 4 spans 34) has no
  representation. The fix is a c2mir raise for a spanning access, then the
  allocator change. A packed bit-field MEMBER straddles the same way
  (`BF` above); only its alignment is laid out today.

### B64. A typedef's own `aligned(N)` is not modeled

```c
#include <stdio.h>
#include <stddef.h>
typedef __attribute__((aligned(16))) struct { char a; } L2;
typedef int AI __attribute__((aligned(16)));
typedef __attribute__((aligned(8))) int AJ;
typedef struct { char a; } __attribute__((aligned(16))) L3;
typedef struct S4 { char a; } L4 __attribute__((aligned(16)));
struct H { char c; L2 l; };
struct HI { char c; AI i; };
int main(void)
{
	printf("tp: %zu %zu %zu %zu %zu %zu\n", sizeof(L2), __alignof__(L2), sizeof(AI), __alignof__(AI), sizeof(AJ), __alignof__(AJ));
	printf("tq: %zu %zu %zu %zu %zu %zu\n", sizeof(L3), __alignof__(L3), sizeof(L4), __alignof__(L4), sizeof(struct S4), __alignof__(struct S4));
	printf("tr: %zu %zu %zu %zu\n", sizeof(struct H), offsetof(struct H, l), sizeof(struct HI), offsetof(struct HI, i));
	return 0;
}
```

- gcc = clang: `tp: 1 16 4 16 4 8`, `tq: 16 16 1 16 1 1`,
  `tr: 32 16 32 16`. madc: `tp: 16 16 4 4 4 4`, `tq: 16 16 1 1 1 1`,
  `tr: 32 16 8 4`. `L3` (the attribute on the struct itself) is right;
  every typedef-level alignment is wrong, and an object or member declared
  through such a typedef is laid out wrong (`struct HI`).
- Re-measured 2026-09-29 at b6d4a8896. Reducer: `tmp/b64/tp.c`.
- In gcc and clang the attribute qualifies the TYPEDEF: the alias is a
  variant of its base with a user alignment (gcc `build_variant_type_copy`
  + `TYPE_USER_ALIGN`), the base type's size and alignment are unchanged,
  and on a typedef `aligned` may also LOWER an alignment (`__m128_u`'s
  `aligned(1)`).
- Where: the typedef parser's prefix reader (parser.cpp ~53870) collects
  `prefix_align` and drops it for every non-aggregate typedef; the
  declarator-trailing `__attribute__((aligned(N)))` on a typedef name is
  consumed without effect. For an aggregate typedef the prefix is handed to
  `TokenSTRUCT::parse` as `typedef_prefix_align` and aligns the struct tag
  itself, so its size rounds to 16. (mingw `setjmp.h`'s `typedef
  _CRT_ALIGN(16) struct ...` is that seed's reason; its struct is already 16
  bytes, so the seed hides the gap there.)
- Fix shape: an aligned variant type, minted once per (base, alignment) like
  `getQualifiedType`'s one-`DataDefQUAL`-per-(base, cv), which reports the
  variant's alignment and is the base for everything else. Consumers:
  `alignof`, member layout (`addMember`), object alignment, `--emit=c11`
  (renders the typedef's attribute), the forest record (a new record kind).
  OWNER RULING PENDING: this is a type-system addition, a focused session
  like B62 and B70.

### B67. `scalar_storage_order` has no effect

```c
#include <stdio.h>
struct __attribute__((scalar_storage_order("big-endian"))) B { unsigned int x; };
int main(void)
{
	struct B b; b.x = 0x01020304u;
	unsigned char *p = (unsigned char *)&b;
	printf("sso: %02x %u\n", p[0], b.x);
	return 0;
}
```

- gcc: `sso: 01 16909060`. madc: `sso: 04 16909060`. clang 18 does not
  implement the attribute (it warns, prints `04`), so this is GNU-only.
- Where: the attribute is read and recorded
  (`DataDefSTRUCT::setReverseScalarStorage`, frozen as `DF_REVERSE_SCALAR`,
  used for bit-field placement in datadef.h), but nothing in
  `cir_builder.cpp` byte-swaps a scalar member's load or store.
- Disposition (2026-09-29): out of scope under the owner's clang scope
  filter (2026-07-19: a GNU-only feature that is hard to support through
  MIR and that clang does not implement is not required). The bit-field
  placement stays: it closes the gcc torture tests `20230630-2.c` and
  `20230630-4.c` (d85af3516). The scalar byte swap is not planned.

## Accepts invalid code

### B52. A namespace's member is found unqualified in C++ (`vector` without `std::`)

- Found 2026-09-27, while building D23's completion (plan §41.7a, slice 3),
  which must offer only what C++ lets a top-level entry write bare.

```cpp
#include <vector>
int main() { vector<int> v; v.push_back(1); return (int)v.size(); }
```

- g++ 13: `2:14: 'vector' was not declared in this scope`. madc
  `--std=c++17`: compiles and runs (exit 1). The madc dialect writes std's
  names bare by design (value-first), and C++ is not the dialect.
- Where: not traced. madc registers a namespace's objects and functions as
  globals too (the scratch probe `tmp/repl/d23/names_probe.cpp` lists
  `std::allocator_arg` as a global `allocator_arg`), and the recorded
  using-directives include libstdc++'s own `std::__debug`, kept without the
  scope it was written in (`active_using_namespaces`). Either may be the lookup
  that finds `vector`.

### B73. `alignas` accepts a comma-separated operand list in C++

```cpp
struct alignas(8, 16) S { char c; };
int main() { return (int)alignof(S); }
```

- Found 2026-09-29 during B62. g++ 13: `expected ')' before ',' token`;
  clang++ 18: `expected ')'`. madc: compiles, exit 16.
- Where: `Program::parse_alignment_specifier` reads a list, because a class
  template's head is re-read after its pack expands (`alignas(alignof(T)...)`
  over two types is `alignas(alignof(A), alignof(B))`). The source form needs
  one operand and an optional `...`.
- Fix shape (2026-09-29): no token marks a pack expansion's separators
  (`tfBRACKETED`/`tfOVERLOADED`/`tfSYNTHPOS` only), so the reader cannot
  tell the source form from the replay. Either the expander marks what it
  produces, or an alignment specifier's pack expands to one `alignas(...)`
  per element ([dcl.align]/4 reads it that way); then the reader takes one
  operand and refuses a comma.

## Refuses valid code

### B77. A data-only struct cannot qualify a name in an expression

```cpp
#include <cstdio>
struct S { double t; };
template<typename T> struct M { struct T2 { T t; }; unsigned char s[sizeof(T2::t)]; int f() { return 0; } };
int main() { std::printf("q: %zu %zu\n", sizeof(S::t), sizeof(M<double>)); return 0; }
```

- g++ = clang++: `q: 8 8`. madc: `Unknown namespace or class 'S'` (and
  `'T2'` for the nested one). Reducers: `tmp/b65r/v6.cpp`, `v2.cpp`.
- Where: `classify_qualifier_before_scope` asks
  `resolve_expression_class_scope`, which answers only a `DataDefCLASS`. A
  C++ struct with no member function, base or object member stays a
  `DataDefSTRUCT`, so the qualifier classifies as nothing and the
  namespace arm throws. The same name used as a type resolves.
- Found 2026-09-29 while tracing the B65 member regression. Core expression
  parser: a focused session, with B78.

### B74. `std::shared_ptr` does not compile: GCC atomic builtins are undeclared

```cpp
#include <memory>
#include <vector>
#include <cstdio>
int main()
{
	std::shared_ptr<std::vector<int>> q = std::make_shared<std::vector<int>>(3, 4);
	std::printf("sp1: %zu\n", q->size());
	return 0;
}
```

- Found 2026-09-29 while timing B69 against a libstdc++-heavy TU.
  g++ 13 = clang++ 18: `sp1: 3`. madc, every C++ mode: `--std=c++11`
  through `c++17` refuse at `bits/shared_ptr_base.h:322:28: use of
  undeclared identifier '__atomic_always_lock_free'`; `--std=c++20` at
  `bits/atomic_wait.h:144:26: ... '__builtin_ia32_pause'`. No test in
  `tests/` includes a `shared_ptr` use.
- Where: the GCC builtins libstdc++'s `_Lock_policy` default and
  `__detail::__thread_relax` spell are unknown to madc.
  `__atomic_always_lock_free(size, 0)` is a constant (true for 1, 2, 4 and
  8 bytes on x86-64). What follows the first builtin is not yet measured.
- Separately, in a TU that also uses `std::function` (tmp/b62/heavy.cpp),
  `p->size()` of an `auto p = std::make_shared<...>` in a `printf` argument
  list is refused at parse, `Malformed expression: 2 operands with no
  operator between them`; not yet reduced.

### B79. A class template with a fixed non-type parameter before a pack has no members

```cpp
#include <cstdio>
template<int A, int... N> struct Z { char k; };
int main() { Z<1, 2> z; z.k = 4; std::printf("z: %d\n", z.k); return 0; }
```

- g++ = clang++: `z: 4`. madc: `3:27: Unidentified member 'k' in 'Z_1_2'`.
  Reducer: `tmp/b71/f.cpp`.
- Where: `template_pack_real_instantiable` refuses a fixed NON-TYPE
  parameter unless `nontype_fixed_ok` (the won-partial-spec path), so the
  concrete demand replay (`instantiate_shell_origin_replay`) leaves the class
  an opaque shell. Its comment says the arg loop binds only type
  parameters; the loop does bind a fixed non-type argument (`token_subst`).
- Blast radius: `std::_Tuple_impl<size_t _Idx, typename... _Elements>` has
  the same shape, so admitting it changes how every `std::tuple` element
  instantiates. A focused session with the tuple tests and lanes.
- Found 2026-09-29 while fixing B71 (a lone non-type pack, fixed by the
  demand replay arming the value-pack gate).

### B3. A declarator after a type definition's `}` may not start with cv or `*`

- Found 2026-09-25, during the enum family. This is duplication family
  `type_definition_declarator_tail`: `TokenSTRUCT`, `TokenCLASS` and
  `TokenENUM` each read the tail after `}` their own way, and none of them
  accepts a leading `const` / `volatile` / `*`.

| Reducer | madc error (gcc and clang accept all of them) |
|---|---|
| `typedef enum { T1, T2 } const CT;` | `Expecting variable name or ';' after enum definition` |
| `typedef enum { U1, U2 } *PT;` | `Expecting alias name in typedef enum` |
| `struct S { int a; } const s = {1};` (block scope) | `... after struct definition` |
| `class C { public: int a; } *cp;` (C++) | `... after class definition` |
| `struct S { enum E { A, B } const e = B; };` (C++ member) | `... after enum definition` |
| `enum E { A, B } volatile *vp = 0;` (block scope) | `... after enum definition` |
| `enum E { A, B } const e = B;` (block scope) | `... after enum definition` |
| `enum G { G1, G2 } volatile gv = G2;` (file scope) | `... after enum definition` |

- Where: one tail reader for all three, handing the declarator to
  `parse_declarator` and the list to `push_declarator_list_tail`
  (`indirection.md`). It must leave a gate behind.

### B4. `#include <atomic>` is refused

- Found 2026-09-25. The release pack log's 70 `atomic_base.h` errors are the
  same failure.

```cpp
#include <atomic>
int main() { return 0; }
```

- g++, clang++: accept. madc: `inc2.cpp:2:3: error: Expecting variable name or
  ';' after class definition`. The position is reported in the including
  file, not in the header, which is a second defect.
- Probably B3's family, since it's the same error from the class tail. Check
  that first.

### B5. An enum defined inside a parameter, a C struct member, `sizeof` or a cast

- Found 2026-09-25, during the enum family.

| Reducer (C) | madc error (gcc and clang accept) |
|---|---|
| `int f(enum {X1, X2} a) { return a; }` | `Expecting enum tag in parameter type` |
| `struct S { int a; enum N { N1, N2 }; };` then `N2` | `Expecting enum tag in struct member type` |
| `int s = sizeof(enum {Z1, Z2});` | `Unexpected keyword in expression` |
| `int c = (enum {W1, W2})1;` | `use of undeclared identifier 'W1'` |

- Where: each reader expects an elaborated `enum TAG` and doesn't hand an
  `enum {` definition to `TokenENUM::parse`.

### B6. A function typedef returning an enum is refused

- Found 2026-09-25, in glibc's `nss.h`.

```c
enum T { T1, T2 };
typedef enum T F(void);
F g;
enum T g(void) { return T2; }
int main(void) { return g() - 1; }
```

- gcc, clang: accept, and the program exits 0. madc: `2:17: error: expected ','
  or ';' before '(' token`.

### B11. A script's top-level block does not see the script's `:=` names

- Found 2026-09-25, while making a top-level block run (plan §41.2a slice 2).
  An interactive session is unaffected, because an entry's `:=` declares a
  global there.

```c
y := 1;
{ println("{}", y); }
```

- Expected: `1`, as for the same two lines inside a written `main`. madc:
  `2:17: error: use of undeclared identifier 'y'`.
- Where: `parseStatement`'s `{` arm pushes a parentless compound when the
  compound stack is empty. `parse_substatement` already chains an `if`/`for`
  arm's block to the synthesized main (`script_lookup_scope`); a bare block
  needs the same chain, and `{` needs to arm `parsing_script_statement`
  (`file_scope_statement_starter`) for it to apply.

### B17. A class template's static data member is never defined

- Found 2026-09-26, during plan §41.2a slice 3 (measured at `431ee5ef1`).
  A session is affected the same way.

```cpp
#include <stdio.h>
template <class T> struct K { static int n; };
template <class T> int K<T>::n = 5;
int k1 = ++K<int>::n;
int main() { int k2 = ++K<int>::n; printf("%d %d\n", k1, k2); return 0; }
```

- g++: `6 7`. madc: `MIR error: import of undefined item _ZN1KIiE1nE`.
- Where: the out-of-line definition of a template's static data member is
  never instantiated for `K<int>`. It should be emitted linkonce, since every
  TU using `K<int>::n` emits it.
- Wider (measured 2026-09-29 at `9c9b96d02`): a non-type parameter's
  (`template<int N> int B<N>::w = N;`, B<7>::w) and an explicit
  specialization's (`template<> int A<char>::w = 3;`, a concrete definition
  that instantiates nothing) are undefined imports too (`_ZN1AIcE1wE`).
  This blocks the test for KG Gap `qualified_template_id_compound_assign_statement`.

### B18. A file-scope lambda global is never defined

- Found 2026-09-26, during slice 3 (measured at `431ee5ef1`). A session is
  affected the same way.

```cpp
#include <stdio.h>
auto inc = [](int x) { return x + 1; };
int r1 = inc(2);
int main() { printf("%d %d\n", r1, inc(3)); return 0; }
```

- g++: `3 4`. madc: `MIR error: import of undefined item inc`.

### B20. A script's top-level `S::count = 3;` is refused

- Found 2026-09-26, during slice 3. An interactive entry takes it as a
  statement since `9d71ce881`, which is gated on the REPL's parse mode.

```c
struct S { static int count; };
int S::count = 0;
S::count = 3;
println(S::count);
```

- Expected: `3`, as for the same statement inside a written `main`. madc:
  `3:1: error: Qualified member definition requires a return type`.
- Where: `parseStatement`'s file-scope `Type ::` branch. For script mode it
  needs the same route as an entry's (`entry_top_level_statement_at`), gated
  on the madc dialect's main source.

### B21. `--emit=c11` calls a synthesized base-object destructor before declaring it

- Found 2026-09-26, while checking slice 3's destructor change against the
  emitted C (measured at `68bd5c327`; the order predates it).

```cpp
#include <stdio.h>
int dv = 0;
struct V { int x; ~V() { dv++; } };
struct L : virtual V {};
struct R : virtual V {};
struct J : L, R {};
int main() { { J j; } printf("dv=%d\n", dv); return 0; }
```

- g++: `dv=1`. `madc --emit=c11` then gcc `-std=c11 ... -lstdc++`: `dv=1`,
  since gcc accepts the implicit declaration. clang `-std=c11`: `error: call
  to undeclared function '_ZN1RD2Ev'`, and the same for `_ZN1LD2Ev`.
  `_ZN1JD2Ev`'s body calls both, and Pass 1.6 defines J's first, with no
  prototype ahead.
- Where: Pass 1.45 prototypes only a vtable's destructor slots. Every
  synthesized destructor another synthesized body calls needs one too.

### B22. Two synthesized per-TU definitions collide across modules

- Found 2026-09-26 by reading the code, in slice 3's audit of every
  `N_FUNC_DEF` the builder synthesizes. Neither is reproduced yet, and
  neither is on the REPL's path.
- `__madc_cyg_exit_thunk` (`-finstrument-functions`) is a strong definition
  with one fixed name in every TU, so two such objects collide at link. It
  should be linkonce, like its siblings.
- `__madc_flvmar_<N>` (the flavor-marshalling thunks, libc++ flavor only) is
  named by a per-builder counter, so two modules can define the same name
  with different bodies. linkonce would be wrong here. It needs a name
  derived from what it marshals, or internal linkage.

### B23. A cast of a `<cmath>` call, then a binary operator, is refused

- Found 2026-09-26, while testing the session's includes. It fails in file
  mode too, measured at `abd480473`.

```cpp
#include <cmath>
int main()
{
    int a = (int)std::floor(4.5) + 3;
    return a - 7;
}
```

- g++, clang++: accept, exit 0. madc: `4:34: error: Malformed expression: 2
  operands with no operator between them`. The same for `std::sqrt`.
- Passes: `(int)(std::floor(4.5)) + 3`, `(int)sqrt(16.0) + 3`, and a
  header-free `(int)N::f(4.0) + 3`, where `f` is a plain namespace function
  or a `using ::g;` redeclaration with an overload.
- The same family, found 2026-09-26 while writing D10's tests: `int k =
  std::signbit(-0.0) && (-0.0) == 0;` after `#include <cmath>` gives
  `expected label name after '&&'` (it reads GNU's `&&label`). g++ gives 1.
- Where: not traced. The cast's operand is read by `parseCastExpression`,
  the one owner. What differs is `<cmath>`'s `std::` overload set.

### B28. A compound literal of a two-dimensional array is refused

- Found 2026-09-26, while re-entering D10's shown values (§41.4a slice 2):
  the session shows `int grid[2][2]` as `(int[2][2]){ { 1, 2 }, { 3, 4 } }`,
  and madc refuses that text entered again.

```c
#include <stdio.h>
int main(void)
{
	int k = (int[2][2]){ { 1, 2 }, { 3, 4 } }[1][0];
	printf("%d\n", k);
	return 0;
}
```

- gcc, clang `-std=c17 -Wall`: `3`. madc `--std=c17`: `excess elements in
  scalar initializer` twice (from c2mir), then `cir_compile failed`. A 1-D
  `(int[3]){ 4, 5, 6 }[2]` works.
- Where: not traced. The compound literal's type reaches c2mir as a
  one-dimensional array, which fits madc's flattened array storage, so the
  inner braces initialize scalars.

### B30. Two initializer-list constructions of standard containers are refused

- Found 2026-09-26, while testing D10's container display.

```cpp
#include <cstdio>
#include <map>
#include <string>
#include <vector>
int main()
{
	std::map<int, int> m = { { 1, 10 }, { 2, 20 } };
	std::vector<std::string> vs = { "x", "y" };
	printf("%zu %zu\n", m.size(), vs.size());
	return 0;
}
```

- g++: `2 2`. madc `--std=c++17`, one error per line:
  - line 7: `constructor argument coercion cycle (no viable converting
    constructor)`;
  - line 8: `no matching constructor for call to 'vector_std____cxx11__…'`,
    naming the vector's constructor with one `char*` argument.
- `std::vector<int> v = { 1, 2, 3 }` works, and a map filled by assignment
  shows fine.
- Where: not traced. Both are the initializer-list constructor over an
  element that needs its own conversion: a pair from a braced pair, and a
  string from a literal.

### B31. `std::vector`'s `operator==` is refused

- Found 2026-09-26, while re-entering D10's shown containers.

```cpp
#include <cstdio>
#include <vector>
int main()
{
	std::vector<int> v = { 1, 2, 3 };
	std::vector<int> w = std::vector<int>{ 1, 2, 3 };
	int a = w == v;
	int b = (std::vector<int>{ 1, 2, 3 }) == v;
	printf("%d %d\n", a, b);
	return 0;
}
```

- g++: `1 1`. madc `--std=c++17`: `tsubst: skipped body with no tsubst
  coverage (re-parse fallback deleted) @.../bits/stl_algobase.h:1167`.
- Where: the parse-once spine (parse-once.md) has no KIND coverage for the
  body `operator==` reaches through `std::equal`. That is a loud refusal by
  design, never a wrong answer.

### B27. A function template defined after its first use is called by its bare name

- Found 2026-09-26, while testing D27's stubs with a template declared in
  one entry and defined in a later one.

```cpp
#include <cstdio>
template<class T> T tf(T x);
int u() { return tf(21); }
template<class T> T tf(T x) { return x * 2; }
int main() { printf("%d\n", u()); return 0; }
```

- g++, clang++: `42`. madc `--std=c++17`: `MIR error: import of undefined
  item tf`. The call names the template, `tf`, where g++ names the
  specialization, `_Z2tfIiET_S0_`, and instantiates it at the end of the TU
  ([temp.point]/7).
- In a session, clang-repl-18 and -20 fail `u()` at its first use
  ("Symbols not found: [ _Z2tfIiET_S0_ ]"), because a later input does not
  instantiate for an earlier one. madc fails at the same point, under the
  wrong name.
- Where: not traced. The call's callee is resolved while the template has
  no definition, and it is never re-resolved to the specialization.

### B38. A structured binding of an array is refused

- Found 2026-09-27, while looking for an array copy for D12 to reuse (plan
  §41.6a).

```cpp
#include <stdio.h>
int arr[2] = {1, 2};
int main() { auto [x, y] = arr; arr[0] = 9; printf("%d %d\n", x, y); return 0; }
```

- g++ 13: `1 2` ([dcl.struct.bind]/1: the array is copied element by
  element). madc `--std=c++17`: "Expecting integer constant expression" at
  `3:20`.
- D12's slice 2 builds the element-by-element array copy that this binding
  needs.

### B41. A function name after `==` and before `;` is called

- Found 2026-09-27, while gating D12's prerequisite A (plan §41.6a): an
  entry's shown function pointer compared with the function.

```c
#include <stdio.h>
int g(int v) { return v; }
int main(void) { int (*p)(int) = g; int k = p == g; printf("%d\n", k); return 0; }
```

- gcc 13 and clang 18: `1`. madc `--std=c17` and `--std=c++17`: `3:51: too
  few arguments` (c2mir), after "comparison of integer with a pointer".
  `p == g,` and `(p == g)` are right, and so is `g == p;`.
- Where: the function-name arm of `parseExpression` decides decay or call by
  the next token. A `;` decays only with an empty operator stack (so
  `cout << endl;` keeps its call), and here the stack holds `==`. The rule
  C gives is not a token rule: a function designator decays unless it is the
  operand of `&` or `sizeof`, or the callee of a call (C11 6.3.2.1p4).

### B44. A namespace-scope lambda's init-capture is refused

- Found 2026-09-27, while testing D12's result names in a lambda capture.

```cpp
#include <stdio.h>
auto l = [c = 42] { return c; };
int main() { printf("%d\n", l()); return 0; }
```

- g++ 13 and clang++ 18: `42` ([expr.prim.lambda.capture]/6: an init-capture
  declares its own variable, so it needs no enclosing one). madc
  `--std=c++17`: "lambda capture 'c' does not name an enclosing variable" at
  `2:11`.

### B45. A `std::map` is not copied, nor brace-initialized at file scope

- Found 2026-09-27, while keeping a shown `std::map` as D12's result (plan
  §41.6a).

```cpp
#include <stdio.h>
#include <map>
std::map<int,int> m = { { 1, 10 } };
std::map<int,int> m2 = m;
int main() { std::map<int,int> l = m; printf("%d %d\n", m2[1], l[1]); return 0; }
```

- g++ 13: `10 10`. madc `--std=c++17`: "constructor argument coercion cycle
  (no viable converting constructor)" at `3:25`, then "no matching
  constructor for call to 'map_int32_t_int32_t_…(std::map<…>)'" at `4:8` and
  `5:21`: the copy constructor is not found, at file and block scope.
- Where: the CIR's constructor choice (`cir error`), not traced. D12 keeps no
  such object until its slice 2.

### B46. `std::move(x) + 0` is refused, and a first `std::move` shows nothing

- Found 2026-09-27, while tracing why an entry's `std::move(v)` showed
  nothing (plan §41.6a, "Built, slice 1", D).

```cpp
#include <stdio.h>
#include <utility>
int main() { int x = 3; int k = std::move(x) + 0; printf("%d\n", k); return 0; }
```

- g++ 13: `3`. madc `--std=c++17`: "Malformed expression: 2 operands with
  no operator between them" at the `+`, at file and block scope.
  `auto y = std::move(x);`, `sizeof(std::move(x))` and a user function
  returning `T&&` are right.
- In a REPL entry, the first use of `std::move(x)` shows nothing, and
  `std::move(v)` of a vector shows only its type word; a later use shows the
  value. `std::forward<T>(v)` does the same.
- Where: not traced. The call's value type around std::move's first
  instantiation (its return type is `remove_reference<T>::type&&`).

### B47. A C-style cast to a qualified template's reference type is refused

- Found 2026-09-27, while testing casts at an entry's top level (plan §41.6a).

```cpp
#include <vector>
int main() { std::vector<int> v = {1}; (std::vector<int>&&)v; return 0; }
```

- g++ 13: compiles. madc `--std=c++17`: "use of undeclared identifier
  'std'" at the cast's `std`, at block scope and at an entry's top level.

### B48. `std::vector<int>(3, 7)` fails in the parse-once instantiation

- Found 2026-09-27, while testing functional casts at an entry's top level
  (plan §41.6a, E).

```cpp
#include <stdio.h>
#include <vector>
int main() { printf("%zu\n", std::vector<int>(3, 7).size()); return 0; }
```

- g++ 13: `3`. madc `--std=c++17`: "cir error: parse-once internal: tsubst
  bailed on the covered instantiation ... of std::vector::vector<_InputIterator,
  __anon_tparam0> [why: tsubst: unresolved dependent member call]" at
  `stl_vector.h:709`. The error names the iterator-range constructor
  template, where g++ calls the count-and-value constructor. Not traced.

### B49. A file-scope compound literal's address is refused as an initializer

- Found 2026-09-27, while checking where an entry's compound literal lives,
  for D12's slice 2 (plan §41.6a).

```c
#include <stdio.h>
int *p = (int[]){ 11, 22 };
int main(void) { printf("%d\n", p[0] + p[1]); return 0; }
```

- gcc 13 and clang 18: `33`. A compound literal outside a function body has
  static storage (C11 6.5.2.5p5), so its address is an address constant.
  madc `--std=c17`: "initializer of non-auto or thread local object should
  be a constant expression or address" (c2mir), and the same at an entry's
  top level under C and C++. Related: B35 (the literal is typed as a
  pointer).

### B56. A lambda inside a member cannot name the class's members

- Found 2026-09-28, while making a default member initializer's error an
  error (B43). g++.dg `lambda-nsdmi2.C` and `lambda-nsdmi5.C` compiled only
  because madc dropped their initializers. They are in the gxx baseline
  until this is fixed.

```cpp
#include <cstdio>
struct bug {
	int a = 5;
	int *f() { return [&]{ return &a; }(); }
	int g() { return [] { return decltype(a)(); }(); }
};
int main() { bug b; printf("%d %d\n", b.f() == &b.a, b.g()); return 0; }
```

- g++ 13, clang++ 18: `1 0`. madc `--std=c++17`: "use of undeclared
  identifier 'a'" in both lambdas. `[this]{ return this->a; }` works, since
  `this` resolves through the compound chain to the method's `__this`.
- The same happens in a default member initializer (`int *b = [&]{ return
  &a; }();`).
- Where: a lambda's body compound carries the lambda's own Method, whose
  owner class is NULL. About 30 parser sites read the member context as
  `compounds.top()->method->owner_class` (parseExpr_identifierArm's member
  arms, parsePostfixChain's head, `class_scope_hides_unqualified_name`,
  `current_method_class_has_member`...). One owner must walk the compound
  chain to the enclosing member function, as `access_context_class` does for
  access checks, and allow it when the lambda can capture `this` (a
  capture-default or an explicit `this`) or the name is in an unevaluated
  operand ([expr.prim.id]/2). A lambda that captures nothing and names a
  member outside an unevaluated operand stays an error. A focused session
  (owner rule, core parser).

### B57. The GNU legacy trait `__has_nothrow_constructor` is undeclared

- Found 2026-09-28, with B56. g++.dg `noexcept62.C` compiled only because
  madc dropped its initializer. It is in the gxx baseline until this is
  fixed.

```cpp
struct T {
	template <bool N> struct S { S() noexcept(N) {} };
	int h() { return __has_nothrow_constructor(S<true>); }
};
int main() { T t; return t.h() ? 0 : 1; }
```

- g++ 13: exits 0. clang++ 18 accepts it with a deprecation warning.
  madc `--std=c++17`: "use of undeclared identifier
  '__has_nothrow_constructor'".
- Where: the type traits are dispatched by name at three sites in
  parser.cpp (`__is_nothrow_constructible` at ~15550, ~16692, ~37722). The
  legacy `__has_*` spellings belong in that dispatch, converted once to the
  trait they mean. The three string ladders should become one enum at the
  boundary (enum-over-strings.md).

### B66. `_Alignas` above 16 is refused by c2mir

- Found 2026-09-29 with B62-B65; measured with `bin/madc` at 6671bd11a.

```c
#include <stdio.h>
struct L { char c; int x __attribute__((aligned(32))); };
int main(void) { printf("a32: %zu %zu\n", sizeof(struct L), __alignof__(struct L)); return 0; }
```

- gcc = clang: `a32: 64 32`. madc: `2:24: unsupported alignmnent`,
  `cir_compile failed` (c2mir's check, on the `_Alignas(32)` madc emits).
- Where: c2mir's `invalid_alignment`
  (`third_party/mir/c2mir/<target>/c<target>-code.c`) accepts only 0, 1, 2,
  4, 8 and 16, on every target. A local aligned to 32 needs a realigned
  frame in MIR, so this may be a floor change
  (`lowering-vs-raising.md` Tier 2/3), not only the check.

## Diagnostics

### B7. An undeducible function-template call dies in MIR without a location

- Found 2026-09-25, while fixing the juxtaposed-operand bug (`2bcd34fc8`).

```cpp
template <int N> int t() { return N; }
int main() { return t(); }        // and: return t<1 2>();
```

- g++: `2:22: no matching function for call to 't()'`, and for the second
  form `parse error in template argument list`. clang++: `no matching
  function for call to 't'`, then `expected '>'`. madc: `MIR error: import
  of undefined item t` with no line.
- Where: `instantiate_namespace_fn_template_for_call` returns NULL, and
  `parseCallFunc` keeps the body-less placeholder. For `t<1 2>`,
  `capture_call_template_args` bails silently on the non-type argument it
  can't fold. The fix must stay quiet in dependent parses, unevaluated
  operands and SFINAE contexts.

### B8. The caret is misdrawn on a line with a tab or a multi-byte character

- Found 2026-09-25, while measuring diagnostics for D26 (plan §42). This is
  D26 part 1. Part 2, which cites the token's start column, rides the next
  merge wave.

```c
int main(void) {
	int x = 1 foo; return x; }
```

- gcc: `2:19`, with the caret under `foo`, the tab expanded and `^~~`.
- madc: `2:14`. The line is printed with its tab raw, and the caret is
  placed by counting bytes as spaces, so it lands 7 columns left of `foo`.
  With `"éé"` earlier on the line, it lands 2 columns right.
- Where: the caret renderer. It should expand tabs, count screen width and
  underline the token.

### B9. madc wording where gcc and clang name the missing token

- Found 2026-09-25.
- `int main() { g() return 0; }`
  - gcc: `expected ';' before 'return'`;
  - clang: `expected ';' after expression`;
  - madc: `2:23: Unexpected keyword in expression`.
- The same happens to the juxtaposition messages for `static_assert`,
  subscript, `switch` and the conditional operator. They are refused at the
  right position, except the subscript, but in madc's own wording.

### B16. A C file-scope initializer that is not constant is refused by c2mir, not the front end

- Found 2026-09-25, while building the entry transaction's JIT half (plan
  §41.3).

```c
int f(void) { return 7; }
int r = f();
int main(void) { return r; }
```

- gcc `-std=c17`: `2:9: error: initializer element is not constant`. clang:
  `2:9: error: initializer element is not a compile-time constant`. madc
  `--std=c17`: exit 1, but with c2mir's `2:10: initializer of non-auto or
  thread local object should be a constant expression or address`, then
  `cir_compile failed`. That is not a madc diagnostic: no `error:`, the
  column is one past the expression, and nothing is recorded on the Program.
  An interactive entry records only "the entry did not compile".
- Where: C11 6.7.9p4. The front end should refuse a non-constant
  initializer of a static-storage object in C, where it already folds the
  constant ones. C++ runs it as dynamic initialization instead, and madc
  does that correctly.
- `test_cir`'s "a tree compiles after an earlier tree failed" uses this
  input to make c2mir refuse a tree. Once the front end refuses it, that
  test needs another input only c2mir refuses.

### B33. A missing right operand is refused at the declaration's `=`

- Found 2026-09-27, while building the REPL's front end (D20, plan §41.5a).
  The same happens in file mode, so it is not the REPL's.

```c
int x = 1;
int y = x +;
```

- gcc 13: `2:12: expected expression before ';' token`, with the caret
  under the `;`. clang: `2:12: expected expression`.
- madc `--std=c17`: `2:7: Missing operand`, with the caret under the `=`.
  `x + ;`, `(x +)` and `x * ;` are refused at `2:7` too. The refusal is
  right; its position and wording are not.
- Where: `popOperator`'s two "Missing operand" throws (`Throw(to)`, where
  `to` is the operator being popped). Either the popped operator carries the
  initializer's position, or the operator being cited is the wrong one.
  Reducer: `tmp/repl/d20/missop.c`.

### B34. A redefinition is cited two columns before its name

- Found 2026-09-27, while measuring loaded-file statics for D20's slice 2
  (plan §41.5a). File mode does the same.

```c
int t = 5;
int t = 6;
int main(void) { return t; }
```

- gcc 13: `2:5: redefinition of 't'`, the caret under `t`.
- madc `--std=c17`: `2:3: redefinition of 't'`, with the caret under the
  `t` of `int`. The session's `int f(void)` redefinition is cited at `1:6`,
  on the `f`, so functions are right.
- Where: `Program::declare_object`'s redefinition refusal (D18,
  `bd3c56500`) throws at `where`, the token its caller passes, and that
  token is not the declarator-id. Reducer: `tmp/repl/d20s2/redef.c`.

### B36. An entry spells a class type with its default template arguments

- Found 2026-09-27, while measuring which values D12 can copy (plan §41.6a).
  Its silent half is fixed (2026-09-28): a `std::unique_ptr` built from a
  pointer held null.

```cpp
#include <memory>
std::unique_ptr<int> p(new int(3));
```

- An entry's `p` shows its type as
  `std::unique_ptr<int32_t,std::default_delete<int32_t>>`. The source's
  spelling is `std::unique_ptr<int>`, without the default argument (and
  `int`, not madc's `int32_t`).
- Where: not traced. The entry's value display spells the instance's
  canonical name.

### B39. A missing `;` after an `auto` declaration is cited inside the next line

- Found 2026-09-27, while tracing why an entry's `auto x = 5` shows nothing
  (plan §41.6a, B there).

```cpp
auto x = 5
int main() { return x; }
```

- g++ 13: `2:1: expected ',' or ';' before 'int'`. madc `--std=c++17`: `2:8:
  Malformed expression: 2 operands with no operator between them`.
- Where: the `auto` arm of `parseDeclaration` parses its initializer with
  the expression engine, which reads on into `int main`. A declarator's
  initializer ends where the next token cannot continue it.

### B51. An unknown directive is refused as "unexpected token type 7"

- Found 2026-09-27, while building D23's completion (plan §41.7a, slice 3),
  whose probe after `#` the lexer kept as tokens.

```c
#xylo
int main(void) { return 3; }
```

- gcc 13: `1:2: error: invalid preprocessing directive #xylo`. clang 18:
  `invalid preprocessing directive`. madc `--std=c17`: `1:5: unexpected
  token type 7`, refused (exit 1).
- Where: the lexer's directive dispatch has no arm for an unknown name in an
  active region (only a skipped block consumes the line, lexer.cpp's "rest
  of line for unknown directives"), so `#` and the name reach the parser as
  tokens. A null directive (`#` alone) is valid and must stay accepted.

### B53. C23's own keywords are not keywords under `--std=c23`

- Found 2026-09-27, while giving `?name` a keyword's provenance (plan
  §41.8a): `?constexpr` in a `--std=c23` session is "not declared".

```c
constexpr int x = 5;
int main(void) { return x; }
```

- gcc 13 `-std=c2x`: accepted, exit 5. madc `--std=c23`: `1:9: use of
  undeclared identifier 'constexpr'`, refused (exit 1).
- Where: `Program::add_keywords` (lexer.cpp) reserves `constexpr`,
  `thread_local`, `alignas`, `alignof`, `nullptr`, `static_assert`, `bool`,
  `true` and `false` only through `cpp_keyword_active` (the madc dialect or
  C++). C23 has them too: the standards' lists (`src/madc_keywords.cpp`)
  record each one's C standard (`c_since`), which a C-side gate can read.
  `typeof`, `typeof_unqual` and `_BitInt` are C23's alone.

### B55. Tab inserts nothing in madcide's editor

- Found 2026-09-28, while building item 9 slice 3 (plan §41.9a): the REPL
  tab's gui case showed tab going to the shared focus owner, which cycles
  focus. Fixed for fields that ask for tab (the `tabkey` hint, 82108777b:
  the terminal and the REPL input). The editor's own node does not ask.
- Reducer: open a file in madcide (any client), put the caret mid-line and
  press Tab. Expected (JOE, pico, vim's insert mode, VS Code): a tab (or the
  profile's indent) goes in at the caret. madcide: the buffer is unchanged.
  With a menu bar or a panel composed, the key becomes an invisible focus
  cycle. With one focusable it reaches `edit_key`
  (`tools/texteditor/editor_events.inc`), which has no tab arm.
- Fix: an insert-tab / indent command in the command table, bound by the
  profiles (data, never a hard-coded key), and the `tabkey` hint on the
  editor window's node while it has the keyboard. Pin it in testmadcide
  and a tests/gui case.

## Open questions

### B10. `__builtin_types_compatible_p` in C++

```cpp
enum E { A };
int main() { return __builtin_types_compatible_p(enum E, int); }
```

- g++ and clang++ refuse the builtin in C++ entirely. madc accepts it for
  builtin types, but its operand reader resolves `enum TAG` only through
  `find_c_enum_tag`, so this gives `Invalid first type in
  __builtin_types_compatible_p`.
- Decide: refuse it in C++, as gcc does, or read C++ enum tags.

### B40. Every function type answers `is_void()`

- Found 2026-09-27, while tracing why an entry's final function name showed
  nothing (plan §41.6a, A).
- `FuncDef`'s `DataDef` base is default-constructed, so its type tag is 0,
  `dtVOID`, and `DataDef::is_void()` (`rawtype() == dtVOID`) is true for
  every function type. No user-visible effect was found beyond the show: an
  overload set over two function-pointer parameters resolves right, and
  `std::is_void<decltype(f)>` is 0.
- The show now asks of the value it displays, a function's pointer
  ([conv.func]). The owner (`DataDef::is_void`, gated) is where the fix
  belongs, and it reaches overload ranking: `score_arg_to_param`'s
  non-class pointee arm ranks a function-pointer pointee against any other
  as a `void *` conversion when either answers `is_void()`
  (`cir_builder.cpp`, the `[conv.ptr]` arm). A focused session: give a
  function type its own tag, then run the lanes.

## Duplication families (divergent, open)

A divergent family is a live bug. Consolidating one leaves a gate in
`fulltest`.

B58–B61 are one family, filed per delimiter at the owner's request
(2026-09-28; B58, `<`, closed 2026-09-29 — every angle counter and
hand-split argument list is on the owners, and the split has its own gate
marker): hand-rolled balanced-delimiter counters that
`delimiter-tracking.md` forbids and `check-one-delim-tracker.sh` reports
GREEN over. The gate's two markers are a counter NAME (`*angle*`,
`*paren*`, `*square*`, `*brace*`) and a raw `'('` character scan, so a
token scan (`id() == TokenID::tkLT` … `++depth`) with any other counter
name is invisible to it. Found with a name-independent sweep: an equality
test on a delimiter that increments a counter, then a test on a delimiter
that decrements the same counter (either way round, so a backward walk
counts). The gate now carries that marker and holds the ratchet at the
count; each migration lowers it. A function that counts several
delimiters is listed under each. Line numbers are at `3c2c531a7`, in `src/parser.cpp`
unless stated. The owners already exist: `DelimDepth` with
`delim_scan_step` (index scans) and `Program::delimStepStream` (stream
scans), `peek_after_balanced_template_id_from`,
`capture_balanced_group_tokens` and `outofline_declarator_param_arity`.

### B59. `(`: twelve hand-rolled paren counters

- Most skip to the matching `)`. `DelimDepth` answers that the same way
  for parens, so few of these diverge today. They are still copies, and a
  paren skip that also has to step over a `{`, `[` or template-id does not
  see it.
- Done (2026-09-29): the three helpers other code called.
  `paren_close_index` and `tsubst_matching_close` are deleted; their
  callers ask `balanced_group_close`. `consume_balanced_parenthesized_suffix`
  tracks its group with `delimStepStream`. The loops below move onto
  `balanced_group_close` (index scans) or `delimStepStream` (stream scans).
- Done (2026-09-29): the GNU-extension skippers (`consume_gnu_attributes`,
  `skip_gnu_asm_statement`'s nine loops, `parse_gnu_vector_size_attribute`,
  `consume_typedef_gnu_attributes`) track their groups on the stream
  tracker; the asm skipper's loops are the new
  `Program::consume_through_open_parens`.
- Done: the static_assert consumers (`consume_deferred_static_assert_statement`,
  `consume_class_static_assert_declaration`, whose message comma is now
  "directly inside the parens", no other group open) and
  `fold_if_constexpr_condition` (collecting an operator-id's tail too).
- Done: `peek_param_list_spelling` (an operator-id's tail is spelled too)
  and `next_parenthesized_type_is_compound_literal`, on the stream tracker.
- Done: `struct_body_needs_class_parser_from` walks on one tracker (its
  brace axis the member level, its square axis the dimensions, the
  attribute skip `balanced_group_close`), with B60's site there.
- Done (2026-09-29): the aggregate attribute readers. The struct parser's
  `consume_attribute`, the nested arms' `consume_nested_attributes` and
  `consume_anonymous_aggregate_open` ask `Program::consume_aggregate_attributes`
  (over `consume_gnu_attributes`); the opener's "does a `{` follow"
  is `balanced_group_close` on the stored stream. Three copies of the rule
  had diverged: the nested one knew only `packed`, never read the groups
  after `}`, and the named arm dropped what it read (SILENT layouts, fixed
  with tests/testnestedaggregateattrs and testclassanonaggregateattrs).
- Done (2026-09-29): a member's own trailing attribute is read by
  `consume_gnu_attributes`, like its leading one. The skip had dropped
  `aligned(N)` (SILENT layouts, fixed with tests/testmembertrailingattr).
- Sites: `skipped_friend_operator_definition` 49145;
  `skipped_friend_defaulted_comparison` 49180;
  `TokenCLASS::parse` 51066; `skip_constraint_expression` 57283 (the
  `skip_balanced` lambda, also used for `{`);
  `skipped_template_body_is_inline_identity_refcast` 60141;
  `resolve_fn_template_return_by_key` 67162;
  `try_parse_implicit_int_function_definition` 70031;
  `parseFunction` 71510, 71566; `paren_group_is_function_def` 73423;
  `parse_optional_init_statement` 52777 (also `[` `{`);
  `instantiate_fn_template_binding` 63622 (backward).
- No failing reducer yet. Each migration is behaviour-preserving for
  parens, and the suite is its oracle.

### B60. `[`: one hand-rolled square-bracket counter

- Done: `try_parse_vla_row_sizeof` (`balanced_group_close`) and the two
  bound scanners `bracket_dim_uses_runtime_value` /
  `bracket_dim_has_constant_fold_query` (DelimDepth entered inside the `[`).
- Done: `evaluate_requires_expression_constant`'s requirement collector
  (with B61's two there).
- Site: `parseFunction` 71407.
- The two `bracket_dim_*` scanners read an array bound. Measured when
  they moved (2026-09-29): `int a[b[1]]`, `int c[b[b[0]] + 1]` and
  `int[sizeof(int[2])]` size as gcc and clang do (`nd: 20 24 32`).
- No failing reducer yet.

### B61. `{`: ten hand-rolled brace counters

- Done: `evaluate_requires_expression_constant`'s requirement collector
  and its compound requirement's `{ E }` close (`balanced_group_close`; an
  unclosed brace is now "not satisfied", where the old walk built an
  inverted range).
- Sites: `parseExpr_operatorArm` 43000; `collect_compound_body_tokens` 48375;
  `TokenCLASS::parse` 50599; `skip_constraint_expression` 57285;
  `skip_template_nonclass_declaration` 57379;
  `materialize_pattern_local_class` 65736; `TokenTEMPLATE::parse` 69128;
  `parseFunction` 71409; `parse_declaration_body` 74599;
  `src/madc.cpp` 133 `find_closing_brace`.
- `find_closing_brace` scans raw source text, with string, character and
  comment states. Its owner is `SpellingDelimDepth`
  (`include/spelling_delim.h`), or a stated exclusion like the gate's
  `validate_expression_source`, decided when it moves.
- No failing reducer yet.

- `type_definition_declarator_tail`: see B3.
- `gnu_attribute_spelling_readers`: `TokenSTRUCT`'s `consume_attribute`
  lambda, `consume_nested_attributes`, `consume_anonymous_aggregate_open`
  and `consume_typedef_gnu_attributes` compare attribute spellings by hand.
  They should use `consume_gnu_attributes_naming`. No failing reducer yet.
- `call_argument_loop`: `parseCallFunc` and `parseCallMethod` carry twin
  argument loops.
- `call_argument_shaping` (found 2026-09-26, with the by-value `var` fix):
  - Ten call lanes each carry the same argument ladder: a reference formal
    to `object_arg_addr`, a by-value object formal to `object_arg_value`, a
    scalar reference to `ref_param_arg_addr`, then the scalar arms.
  - They agree today: the by-value test is `by_value_class_formal` in all
    ten. The ladder itself is still copied, and some copies carry extra
    arms (the char-pointer coercion, the complex lowering).
- `external_ctor_param_shape` (found 2026-09-26, with L3):
  - `ctor_call_assemble`'s external-constructor arm declares the extern's
    parameters by hand: a real as `long long`, a non-char pointer as
    `char *`. The owner is `native_param_shape`.
  - The retbuf copy lane had the same copy. It returned a garbage real for
    `var f() { return 2.5; }`, and L3 moved it onto the owner.
  - No failing reducer yet for this arm. The carrier's constructor rows get
    Pass 0.75's typed prototype first.
- `member_default_construct_loop` (found 2026-09-28, with the base-owned
  members fix):
  - `class_member_construct` (the user-ctor prologue) and
    `append_member_default_constructs` (the implicit and inherited ctors)
    each default-construct a class's members.
  - Both skip a member a base's constructor built, through
    `member_constructed_by_base`. They still build each member differently:
    `class_ctor_call_addr` against `complete_object_construct_stmts`, which
    differ on virtual bases. No failing reducer yet.
- `retbuf_memberwise_copy` (found 2026-09-28, with the returned-prvalue
  fix):
  - The owner of the implicit copy/move is
    `implicit_copy_construct_from_addr`. `class_copy_construct_into_retbuf`
    keeps a loop of its own for what the owner refuses: a polymorphic
    class, and a class whose user copy constructor overload selection
    missed.
  - That loop copies members only: it runs no base's constructor and never
    moves. No failing reducer yet.

## Carried from earlier hand-offs

These are still open as of UPDATE 103. Their reducers are in the knowledge
graph (offline while the NAS is down) and in `claude_status.json`
(`live_handoff_previous`, `known_pre_existing_gaps`). Give each one a full
entry above when it's next touched.

- Copy elision into heap and array elements.
- `thread_local` is process-wide (a missing MIR TLS feature).
- Statics and globals are not destroyed at exit.
- No `std::bad_array_new_length`.
- No user conversion operators to scalars.
- `__sync_*` builtins.
- The aggregate-predicate duplication family.
- `u8` prefix and UCN validity.
