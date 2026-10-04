# madc bug backlog

Defects found while working on something else, filed so the current work
keeps moving and fixed later in a burndown the owner schedules.

**Owner pause, 2026-09-25, LIFTED 2026-09-28:** while the REPL arc's first
slice was built (plan §37), a defect found off the REPL's path was filed here
instead of being fixed on the spot. With §37 complete, the owner lifted the
pause, and this backlog is being burned down. A defect found now is fixed in
its own commit when it can be (`fix-what-you-find.md`). One that is not fixed
on the spot is filed here, so it stays tracked (owner, 2026-09-29).

**Owner, 2026-09-30:** the REPL + learning-IDE release comes first. A defect
on the release's path, or blocking it, is fixed; one found off that path is
filed here and the work returns to the release. The burn-down resumes when the
owner schedules it.

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

### B168. `__DATE__` / `__TIME__` are madc's own build date, not the compile's

```c
#include <stdio.h>
int main(void) { printf("%s\n", __DATE__); return 0; }
```

- C11 6.10.8.1: the date of translation of the source file. gcc 13 and
  clang 18 (2026-10-04): `Oct  4 2026`. The v0.101.0 release madc
  (`tmp/release-bins/v0.101.0`) the same day: `Oct  3 2026` — the day madc
  itself was built: `src/lexer.cpp` sets `define_map["__DATE__"]` from the
  C++ compiler's own `__DATE__` (and `__TIME__` likewise).
- Consumers: Help ▸ About's copyright year (`product_about`) reads
  `__DATE__`; it is right only while madc and the product are built in the
  same year.
- Not a one-line fix: the frozen forest is keyed by the predefined-macro
  table (the context hash and the producer config), so a per-compile date
  must stay out of that key (gcc's `SOURCE_DATE_EPOCH` is the reproducible-
  build convention to honour too).
- Found 2026-10-04 writing Help ▸ About.

### B156. `__is_same` is false for a namespace-scope scalar typedef and its type

```cpp
#include <type_traits>
#include <cstddef>
#include <cstdio>
namespace w { typedef int i; }
int main() {
	std::printf("%d %d %d %d\n", (int)std::is_same<std::size_t, unsigned long>::value,
		    (int)std::is_same<w::i, int>::value,
		    (int)std::is_same<std::ptrdiff_t, long>::value,
		    (int)__is_same(std::size_t, unsigned long));
	return 0;
}
```

- g++ 13 and clang++ 18 (libstdc++ and libc++): `1 1 1 1`. madc
  (2026-10-03, libstdc++): `0 0 0 0`; under `-stdlib=libc++`: `1 0 1 1`.
  v0.100.1 gives the same answers.
- Layer: `TraitTypeArg::same_as` (src/parser.cpp) compares `dd->name`. The
  namespace-scope arm of `TokenTYPEDEF::parse`'s `finish_alias` mints an
  alias dd named for the typedef, linked to its type by `scalar_alias_of`.
  `proven_scalar_identity` walks that link; `same_as` does not.
- Found 2026-10-03 while fixing the libc++ `add_volatile` regression
  (tests/testvolatiletypetraitscxx). Off the release path (shipped in
  v0.100.1), filed per owner 2026-09-30.

### B157. libc++: `std::vector::push_back(std::move(x))` copies instead of moving

```cpp
#include <vector>
#include <utility>
#include <cstdio>
struct S { int v; S(int x) : v(x) {}
	S(const S &o) : v(o.v) { std::printf("copy "); }
	S(S &&o) : v(o.v) { std::printf("move "); } };
int main() { std::vector<S> v; v.reserve(4); S s(1);
	v.push_back(std::move(s)); std::printf("| ");
	v.emplace_back(std::move(s)); std::printf("| ");
	v.push_back(s); std::printf("\n"); return 0; }
```

- clang++ 18 `-stdlib=libc++`: `move | move | copy`. madc `-stdlib=libc++`
  (2026-10-03): `copy | move | copy`. libstdc++ is correct. v0.100.1 libc++
  prints the same as madc today (tests/testtsubstargcategory's `vec:` line:
  `copy copy copy`; expected `move copy move`).
- The emitted C's `main` calls `vector<S>::push_back` (the `const_reference`
  overload) for the xvalue; the `value_type&&` overload (`push_back__o2`)
  is never emitted. Both out-of-line definitions bind their own overload
  (`MADC_OOL_PROBE=vector`). `std::allocator_traits::construct` and
  `std::__construct_at` forward an xvalue correctly, and a user class
  template with the same `const_reference` / `value_type&&` pair resolves
  correctly. Layer not yet traced: the call-site overload choice between
  libc++'s two declarations.
- Found 2026-10-03 in the libc++ release-tier lane. Off the release path
  (shipped in v0.100.1), filed per owner 2026-09-30.
  tests/testtsubstargcategory carries `.libcxx_skip` until this is fixed.

### B150. win64 `--emit=c11` renders `packed` as `#pragma pack(1)`, which mingw lays out differently

```c
#include <stdio.h>
struct __attribute__((packed)) A1 { char a : 4; int : 0; char c; };
int main(void) { printf("%zu %zu\n", sizeof(struct A1), _Alignof(struct A1)); return 0; }
```

- x86_64-w64-mingw32 gcc 13 under wine: `4 4`. madc's win64 JIT gives
  `4 4` once the MS zero-width record-alignment fix is in (2026-10-02).
  madc's win64 `--emit=c11` output wraps the struct in `#pragma pack(push, 1)`
  with no attribute. mingw gcc lays that form out as `2 1`, like
  `#pragma pack(1) struct C1 {...}`, so the emitted C disagrees with the JIT.
- Under SysV the two spellings lay out alike (gcc and clang: `5 1` for both),
  so Linux output is unaffected.
- Layer: the settled aggregate layout record carries only the effective
  `pack` (`DataDefSTRUCT::pack`, 1 for the attribute), and
  `cir_emit_c.cpp`'s `emit_pack_push` renders every pack as the pragma. The
  record would need to say whether the pack came from `#pragma pack`
  (`pragma_pack`) or the `packed` attribute.
- Found 2026-10-02 while fixing the MS zero-width alignment
  (tests/testmszerowidthalign). Off the release path, filed per owner
  2026-09-30.

### B147. `noexcept(std::__relocate_a(...))` answers false, and `--freeze` answers it true

```cpp
#include <vector>
#include <string>
#include <cstdio>
int main() {
	std::string *p = nullptr;
	int *q = nullptr;
	std::allocator<std::string> a;
	std::allocator<int> b;
	std::printf("%d %d\n", (int)noexcept(std::__relocate_a(p, p, p, a)),
		(int)noexcept(std::__relocate_a(q, q, q, b)));
	return 0;
}
```

- g++ 13 and clang++ 18 (`-std=c++17`): `1 1`. madc (2026-10-02, at fda3b3019
  with the subscript-operator fix in the tree): `0 0`, exit 0.
- In `tests/testsubscript.mad`, `-v` (the `noexcept_eval: call` diagnostic)
  shows `__relocate_a`'s specification as false (`spec=0`, no deferred
  condition) in a plain compile and as true (`spec=1`) under `--freeze`. So
  the two parses of one TU disagree, and a bound consumer loads the frozen
  answer. Program output is unaffected: it is `vector`'s relocation path
  (`_S_use_relocate()`) that differs.
- Layer: not yet traced. The instantiated declarator's `noexcept(noexcept(
  std::__relocate_a_1(...)))` folds to false at the declarator parse
  (`fold_nontype_template_arg`) in a plain compile, and to true in the
  `--freeze` compile of the same TU; what the forest recording changes on
  that path is the open question.
- Found 2026-10-02 while fixing the `[subbind]` bind gate. Off the release
  path, filed per owner 2026-09-30.

### B144. A cast to an lvalue reference deduces a forwarding reference as an rvalue

```cpp
#include <cstdio>
#include <utility>
void f(int &) { puts("L"); }
void f(int &&) { puts("R"); }
template<class T> void g(T &&t) { f(std::forward<T>(t)); }
int main() {
	int x = 1;
	g(static_cast<int &>(x));
	g(static_cast<int &&>(x));
	g((int &)x);
	return 0;
}
```

- g++ 13 and clang++ 18 (`-std=c++17`): `L R L`. madc (`--std=c++17`,
  2026-10-02): `R R R`, exit 0.
- Layer: deduction's lvalue test, `fn_template_call_arg_is_lvalue`
  (`src/parser.cpp`), is a second value-category reader beside
  `Program::argument_value_category`. It sends a `TokenCast` to
  `is_addressable_expression`, which answers false for every cast, so
  [temp.deduct.call]/3 deduces `T = int` for an lvalue. `argument_value_category`
  reads the cast's kind (`static_cast<T&>` an lvalue) correctly. The fix is to
  make the deduction reader defer to the one reader. The two also differ on a
  string literal (an lvalue there, an rvalue here).
- Found 2026-10-02 while fixing the vecbind gate (substituted call arguments'
  value category). Off the release path, filed per owner 2026-09-30.

### B145. A member template's direct-initialization picks its constructor once, for every instantiation

```cpp
#include <utility>
#include <cstdio>
struct S {
	int v;
	S(int x) : v(x) {}
	S(const S &o) : v(o.v) { printf("copy "); }
	S(S &&o) : v(o.v) { printf("move "); }
};
struct Relay {
	template<class T> void make(T &&t) { S s(std::forward<T>(t)); (void)s; }
};
template<class T> void free_make(T &&t) { S s(std::forward<T>(t)); (void)s; }
int main() {
	Relay r;
	S a(3);
	printf("member: ");
	r.make(a);
	r.make(std::move(a));
	printf("\nfree: ");
	free_make(a);
	free_make(std::move(a));
	printf("\n");
	return 0;
}
```

- g++ 13 and clang++ 18: `member: copy move` / `free: copy move`. madc
  (2026-10-02): `member: move move` / `free: copy move`, exit 0. It moves
  from an lvalue.
- Layer: a member template's body is lowered once as a Tree-1 pattern and
  copied per instance. `translate_block` defers a local declaration's
  construction (the decl marker that `tsubst_relower_deferred_construction`
  relowers) only when its ctor args hold a pack expansion
  (`tsubst_args_have_pack_expansion`). A non-pack dependent argument
  (`std::forward<T>(t)`) gets its constructor picked in the PATTERN, where
  `argument_value_category`'s identity-forward arm reads the dependent `T` as a
  non-reference (an xvalue). `S(S&&)` is baked in for every `T`. The fix is to
  defer the construction whenever an argument is type-dependent ([temp.dep.expr]:
  overload resolution of a dependent call happens at instantiation).
- The functional-cast form `return A(std::forward<U>(u)).v;` in a member
  template, and `w = A(std::forward<U>(u)).v;` in a constructor template's
  body, also move from an lvalue argument. `U` itself deduces correctly
  (`std::is_lvalue_reference<U>::value` is 1), and
  `A(static_cast<U &&>(u)).v` copies (2026-10-02).
- Found 2026-10-02 with B144. Off the release path, filed per owner 2026-09-30.

### B121. A const object binds the non-const reference overload

```cpp
#include <utility>
#include <cstdio>
struct W { int v; };
static const char *w(W &) { return "W&"; }
static const char *w(const W &) { return "const W&"; }
static const char *r(W &&) { return "W&&"; }
static const char *r(const W &&) { return "const W&&"; }
template<class T> struct X { T v; };
template<class T> const char *f(X<T> &) { return "X&"; }
template<class T> const char *f(const X<T> &) { return "const X&"; }
int main()
{
    W a{1};
    const W ca{2};
    X<int> x{1};
    const X<int> cx{2};
    printf("%s %s %s %s %s %s\n", w(a), w(ca), r(std::move(a)),
           r(std::move(ca)), f(x), f(cx));
    return 0;
}
```

- g++ 13 = clang++ 18 (`-std=c++17`): `W& const W& W&& const W&& X& const X&`.
  madc (`--std=c++17`, at `17d464b8a` and after `b84cb6334`):
  `W& W& W&& W&& X& X&`, exit 0.
- A non-const `T&` / `T&&` binds no const object ([dcl.init.ref]/5). C++
  `const` is not in madc's types (`modeled_cv()` carries const in C only), so
  the binding rank (`copy_move_ref_binding_rank`'s `arg_const`) reads every
  object as non-const. Layer: the const-qualified-type model
  (`DataDefQUAL` for C++ const, FEATURE_CONST_TYPES), not the rankers.
- Same root, seen 2026-10-04 in the REPL: `const char *p = "Test";` then
  `%whos` lists `p` with Type `char *` under `--std=c++17` and `--std=madc`
  (the Variables view too), `const char *` under `--std=c17`. The binding's
  type is spelled from the type, which carries no C++ const.

### B102. A namespace-scope object's destructor never runs at exit

```cpp
#include <cstdio>
struct D { ~D() { printf("dtor\n"); } };
D g;
int main() { printf("main\n"); return 0; }
```

- g++ 13 and clang++ 18: `main` / `dtor` ([basic.start.term]/1: an object
  of static storage duration is destroyed when `main` returns, in reverse
  order of construction). madc (`--std=c++17`, `bin/madc` at `409fceb08`),
  JIT and `-o` executable alike: `main` only. Exit 0. `--emit=c11` holds the
  destructor's body, but nothing registers it.
- Found 2026-10-01 building the plugin `source` transport (plan §41.11a
  step 5): what `madc::code_close` must do with opened code's static objects
  depends on it.
- Layer (suspected): `__madc_global_init`'s dynamic initialization
  (`CirBuilder`, src/cir_builder.cpp) constructs each class-typed global and
  queues no destruction. The g++ model: each construction is followed by
  `__cxa_atexit(dtor, &obj, __dso_handle)`, and `dlclose` runs that DSO's
  entries through `__cxa_finalize`. When the fix lands, `code_close`
  (src/madc_program.cpp) must run its code's entries before it frees the
  code, as `dlclose` does, or the exit-time handlers call freed code.

### B81. A mem-initializer flattens a nested braced list, losing its nesting

```cpp
#include <cstdio>
struct FB { int i; int j; };
struct P : FB { P() : FB{ 1, 2 } { } };        // FB becomes a (ctor-less) class
struct AR { int a[3]; char c; };
struct DB : AR { DB() : AR{ { 61, 62 }, 'z' } { } };
struct O { FB f; int k; };
struct PO : O { PO() : O{ { 1, 2 }, 3 } { } };
int main()
{
	DB db; PO po;
	std::printf("b81: %d %d %d %c | %d %d %d\n", db.a[0], db.a[1], db.a[2], db.c,
		po.f.i, po.f.j, po.k);
	return 0;
}
```

A plain-struct member takes the flat list field by field (SILENT):

```cpp
#include <cstdio>
struct PS2 { int a, b; };
struct Q2 { PS2 p; int n; };
struct H2 { Q2 q; H2() : q{ { 1 }, 2 } { } };
int main() { H2 h; std::printf("ps: %d %d %d\n", h.q.p.a, h.q.p.b, h.q.n); return 0; }
```

- g++ 13 = clang++ 18: `ps: 1 0 2`. madc (`--std=c++17`): `ps: 1 2 <garbage>`
  (SILENT). A fully braced inner list (`q{ {1, 2}, 3 }`) comes out right.
- For a ctor-less CLASS subobject (the first reducer):
  g++ 13 = clang++ 18: `b81: 61 62 0 z | 1 2 3`. madc (`--std=c++17`):
  `cir error: nested braced list in a mem-initializer of aggregate 'AR' is
  not supported` (LOUD).
- Layer: the mem-initializer parser (`Program::collect_braced_init_args`,
  src/parser.cpp) flattens a nested braced list into ONE scalar sequence, so
  `AR{ {61, 62}, 'z' }` reaches CIR as `61, 62, 'z'`. The flattening is lossy
  (`AR{ {61}, 'z' }` and `AR{ 61, 'z' }` arrive identical), so no CIR reading
  can recover the nesting. The parser records the loss
  (`CtorInitializer::nested_list_flattened`), and `class_direct_init_stmts`
  refuses such a list for a ctor-less class rather than brace-elide over it —
  elision over the flat `61, 62, 'z'` would store `a[2] = 'z'`, silently.
  `class_aggregate_init` walks nested lists as `TokenStructLit` (the
  declaration lanes keep them). Before the B80 fix the whole initializer was
  dropped silently.
- A duplication family (KG `DupFamily braced_init_list_reading`): five
  positions read a braced-init-list. Three keep the nesting —
  `read_struct_lit` (a lambda local to `parse_declaration_body`),
  `parse_compound_struct_lit`, the `respell_braced_list_*` owner — and two
  diverge: this flattener, and `parse_ctor_args_list` (`T{...}`,
  `new T{...}`), which refuses a nested list ("Nested braced-init-list in a
  constructor argument list is not supported (yet)").
- Fix: hoist `read_struct_lit` into one `Program` member, adopt it at the
  mem-initializer and ctor-args positions, delete `collect_braced_init_args`
  and the `nested_list_flattened` refusal; the plain-struct member arm
  (`aggregate_member_init_stmts`) then reads the nesting too. A core parser
  change: its own focused session (owner, 2026-09-13).
- Found 2026-09-29 while fixing B80.

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
- Was blocked on B77 and B78, both fixed now (B78 `86b36877b`, B77
  2026-10-02). Applying a member's attributes evaluates their
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

### B163. C++ keywords are accepted as variable names under `--std=c++NN`

```cpp
int main(void) { int new = 3; return new - 3; }
```

- g++ 13 and clang++ 18 (`-std=c++17`): `expected unqualified-id before
  'new'`, exit 1; the same for `int class = 3;`. madc `--std=c++17`
  (2026-10-03): compiles and runs, exit 0, for `new` and for `class`.
  Under `--std=c17` they are ordinary identifiers, as gcc has them.
- Layer not yet traced: the C++ keyword set reaches the declarator-id
  reader as an identifier under a C++ standard.
- Found 2026-10-03 while choosing a reducer for madcide's `--std=`. Off
  the item in hand (owner rule 2026-09-13: parser behaviour changes get
  their own session), filed per owner 2026-09-30.

### B158. libc++: returning an lvalue `std::unique_ptr` (a deleted copy) is accepted

```cpp
#include <memory>
std::unique_ptr<int> bad(std::unique_ptr<int> &p)
{
	return p;
}
int main() { return 0; }
```

- clang++ 18 `-stdlib=libc++`: `error: call to implicitly-deleted copy
  constructor of 'std::unique_ptr<int>'`. g++ 13: `use of deleted
  function`. madc rejects it under libstdc++ (`use of deleted copy
  constructor`, tests/testdeletedcopyreturn). madc `-stdlib=libc++`
  (2026-10-03): compiles, exit 0. Every archived release since v0.72.0
  behaves the same.
- libc++ declares the copy constructor implicitly deleted (a user move
  constructor, [class.copy.ctor]/6), where libstdc++ spells `= delete`.
  Layer not yet traced: the return-statement copy-initialization check
  sees no deleted constructor in the implicit case.
- Found 2026-10-03 in the libc++ release-tier lane. Off the release path,
  filed per owner 2026-09-30. tests/testdeletedcopyreturn carries
  `.libcxx_skip` until this is fixed.

### B101. A cast from a pointer to a narrower integer type is accepted: `(int)p`, and `(long)p` on Windows

```cpp
typedef bool (*fn)(long);
bool f(long x) { return x > 0; }
int main()
{
	fn p = f;
	int n = (int)p;
	return n == 0;
}
```

- g++ 13: `error: cast from 'fn' {aka 'bool (*)(long int)'} to 'int' loses
  precision [-fpermissive]`; clang++ 18: `error: cast from pointer to
  smaller type 'int' loses information` ([expr.reinterpret.cast]/4: a
  pointer converts only to an integral type large enough to hold it). madc
  (`--std=c++17`, `bin/madc` at `2e77c5504`): compiles, runs, exit 0, the
  pointer truncated to 32 bits. In C, gcc and clang warn
  (`-Wpointer-to-int-cast`, on by default); madc says nothing.
- On Windows `long` is 32 bits (LLP64), so `(long)p` is the same cast. The
  madcide plugin registry kept command handlers and library handles that
  way; on Linux it worked, and under wine a plugin library's symbol lookups
  went through a truncated handle and found nothing. Found 2026-10-01
  bringing the plugin `library` transport to Windows (plan §41.11a step 4);
  the registry now keeps them as `int64_t`, in its own commit.
- Layer (suspected): the cast's semantic check (`TokenCast`,
  `include/madc.h`, read by `Program::parseCastExpression`): nothing
  compares a pointer operand's width with an integer target's. A core
  parser change: its own focused session (owner, 2026-09-13).

### B95. `std::string s = 5;` is accepted, then fails at link without a position

```cpp
#include <string>
#include <cstdio>
std::string s = 5;
int main() { printf("size=%zu\n", s.size()); return 0; }
```

- g++: `b95.cpp:3:17: error: conversion from 'int' to non-scalar type
  'std::string' ... requested`. clang++: `b95.cpp:3:13: error: no viable
  conversion from 'int' to 'std::string'`. madc (`--std=c++17`): no front-end
  diagnostic; the link refuses `MIR error: import of undefined item
  basic_string_char_..._o15` (exit 1), citing no source position.
- Found 2026-09-30 while filing B94 (the same declaration with a valid
  initializer is B94's reducer).
- Layer (suspected, unmeasured): copy-initialization from `int` selects a
  `basic_string` constructor (`_o15`) that [dcl.init]/17.6.3 does not allow
  (no converting constructor takes an `int`), and the selected overload has
  no definition to link.

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

### B171. `std::map<std::string, T>::find("literal")` is refused: the heterogeneous `find<_Kt>` is chosen

```cpp
#include <stdio.h>
#include <map>
#include <string>
int main()
{
	std::map<std::string, int> m;
	m["cy"] = 1;
	printf("%d %d\n", m.find("cy")->second, (int)(m.find("zz") == m.end()));
	return 0;
}
```

- g++ 13 and clang++ 18 (`-std=c++17`): `1 1`. madc `--std=c++17` (at
  55cb04451, and v0.101.0 alike): `cir error: parse-once internal: tsubst
  bailed on the covered instantiation '..._find__o2__mti__...' of
  std::map::find<_Kt> [why: tsubst: unresolved dependent member body]` at
  `stl_map.h:1225`, then `1 untranslatable node(s); not compiling`.
- `find(std::string("cy"))`, `count(std::string("cy"))` and `at("bo")` work.
- Where: overload resolution. C++14's `template<typename _Kt> auto
  find(const _Kt&) -> decltype(_M_t._M_find_tr(__x))` exists only for a
  transparent comparator: `_M_find_tr`'s `__has_is_transparent_t<_Compare,
  _Kt>` default fails for `std::less<std::string>`, so g++ drops the
  template by SFINAE and calls `find(const key_type&)` through the
  converting constructor. madc keeps the template (an exact match for
  `const char (&)[3]`), and the instantiation then bails.
- Found 2026-10-04 testing `std::map<std::string, int>` after B170.

### B167. `#include <windows.h>` is refused on win64: absent from the forest, and in C++

```c
#include <windows.h>
int main(void) { return 0; }
```

- x86_64-w64-mingw32-gcc / g++ 13 (`-std=c17`, `-std=c++17`): compile, exit 0.
- C under wine is FIXED (2026-10-04, `tests/testsysheaderreinclude`): every
  mingw header had been classified as user code — the system include dirs
  were compared with a `/` appended to a Windows canonical spelling — so
  wtypes.h's nested second inclusion was skipped and objidl.h met an
  undeclared `CLIPFORMAT`. `madc.exe --std=c17` now compiles the full
  `<windows.h>` (0 errors) and runs Win32 calls JIT and as a built .exe,
  matching mingw-gcc.
- Two gaps remain, measured 2026-10-04 with the release set's `madc.exe`:
  1. PACKAGING: `<windows.h>` is not in `scripts/forest_pack_headers_windows.txt`,
     so the release's forest does not carry it. On genuine Windows (no
     headers on disk) the release set's `madc.exe` refuses it with
     `Failed to open include file: windows.h`. Under wine the container's
     mingw headers answer through `Z:` and hide this.
  2. C++: `madc.exe --std=c++17` on the reducer: exit 1. Two gcc builtin
     gaps were on its path and are fixed (`__builtin_ia32_sfence`,
     `tests/testia32sfence`; the `__sync_*` family, `tests/testsyncbuiltins`):
     920 errors became 382. What stops it now is B169: mingw's `<intrin.h>`
     includes gcc's own `<x86intrin.h>` (for `__GNUC__` >= 4.9), and the first
     error is `ia32intrin.h:41:10: use of undeclared identifier
     '__builtin_ia32_bsrsi'`. The rest follow it (`FILETIME`,
     `LPDEBUG_EVENT`, ... not types): an error inside one inline body loses
     the enclosing `extern "C"` block's later declarations (winnt.h opens
     `extern "C" {`, reaches the intrinsics, then declares its types).
  - Packaging (1) waits on (2): the Windows forest pack compiles its header
    list as ONE C++ translation unit (`scripts/forest_pack_windows.sh`), so
    `windows.h` joins the list only when C++ parses it quietly.
- Found 2026-10-04 writing the reducer for the Windows window-loop stop.

### B169. gcc's x86 intrinsic headers are refused in C++ (`<x86intrin.h>`: 4477 errors)

```cpp
#include <x86intrin.h>
int main() { return 0; }
```

- g++ 13 and clang++ 18 (`-std=c++17`, x86-64 Linux): compile, exit 0.
  madc `--std=c++17` (Linux, 2026-10-04): exit 1, 4477 parse errors, the
  first `ia32intrin.h:41:10: use of undeclared identifier
  '__builtin_ia32_bsrsi'`. madc `--std=c17` on the same file: exit 0 (C's
  implicit declarations carry the calls; nothing reaches them).
- gcc's intrinsic headers (`/usr/lib/gcc/x86_64-linux-gnu/13/include/*intrin*.h`)
  call `__builtin_ia32_*` 7582 times, most of them vector builtins over
  `vector_size` types; madc declares only `__builtin_ia32_sfence` (mapped to a
  runtime helper). Mapping them one by one is not the fix; this is the SIMD
  track's surface (KG Feature `SIMD vector types`, Decision
  `simd_raise_mir_upstream`).
- Windows: mingw's `<intrin.h>` includes `<x86intrin.h>`, and winnt.h
  reaches it, so this is what stops C++ `<windows.h>` (B167).
- Found 2026-10-04 tracing the C++ form of B167.

### B164. The forest pack's header compile refuses libstdc++ 13 template bodies (28 errors, the build stays green)

`make -C src release` runs `scripts/forest_pack.sh --image
lib/release/libmadc.so bin/madc-release`, which compiles
`tmp/forest_pack_tu.cpp` (the `scripts/forest_pack_headers.txt` set) and
prints 28 errors from GCC 13's headers before packing anyway (rc=0):

- `bits/basic_string.h:1085` `_Alloc_traits::max_size(...)`: "use of
  undeclared identifier 'max_size'" (8 times): a static member reached
  through a dependent typedef.
- `bits/fstream.tcc:184` `_M_file.open(__s, __mode)`: "undeclared
  identifier '__mode'", the enclosing member function's own parameter.
  The same shape: `istream.tcc:713` `eofbit` and `locale_facets_nonio.tcc:725`
  `__state`.
- `bits/basic_string.tcc:191/242` `~_Guard() { _M_guarded->_M_dispose(); }`:
  "'_M_dispose' is a private member", where `_Guard` is a LOCAL class of a
  `basic_string` member function, which has that function's access
  ([class.local]/3, [class.access]/2). `fstream.tcc:263` `__fb->_M_mode`:
  "'_M_mode' is a protected member of basic_filebuf".
- g++ 13 and clang++ 18 compile these headers without a diagnostic. The
  count is 28 in this log (2026-10-03, `tmp/logs/build-rel.log`) and in an
  earlier release build's (`rb-release-qb.log`); the v0.101.0 macOS build
  printed 34. The suite is green on the packed binary, so whatever the
  pack keeps of these bodies is either unused or re-instantiated cleanly.
- Layer not yet traced: the pack's priming compile of out-of-line member
  bodies (parameters and dependent-base names not in scope; a local class
  without its enclosing member's access).
- Found 2026-10-03 rebuilding the container's release binary for the
  chthonia GUI. Off the item in hand, filed per owner 2026-09-30.

### B162. `&(c += 4)` / `&(a = b)` on a class is refused although the operator returns a reference

```cpp
#include <iostream>
struct Counter { int n; Counter &operator+=(int k) { n += k; return *this; } };
int main()
{
    Counter c = { 1 };
    Counter *q = &(c += 4);
    std::cout << "compound: " << (q == &c) << " " << c.n << std::endl;
    return 0;
}
```

- g++ 13 and clang++ 18 (`-std=c++17`): `compound: 1 5`, exit 0. madc
  (2026-10-03, after f380392f3): `expecting addressable expression after
  '&'`, exit 1. The implicit copy assignment returns `T &` too, so `&(a = b)`
  on any class is the same case.
- Layer: `Program::is_addressable_expression` → `builtin_operator_yields_lvalue`
  answers `=` / `@=` for built-in operands only ("a class … operand assigns
  through its operator=, whose return type decides" — nothing decides it).
  f380392f3 records `TokenOperator::resolved_reference` for operators in
  `object_operator_symbol`'s table, which has no assignment operators.
- The operator-spelling table exists twice: the parser's
  `object_operator_symbol` (unary + binary, no assignment) and the CIR's
  `binop_overload_symbol` (`src/cir_builder.cpp`, assignment + compound +
  bitwise + logical, no unary). The fix starts by consolidating them into one
  owner; adding assignment operators to the parser's typing then changes how
  every class `=`, `@=`, `&`, `|`, `&&` expression is typed, so it is its own
  focused session (owner rule, 2026-09-13).
- Found 2026-10-03 fixing the REPL's `cout << x << endl` entry
  (tests/testaddr_opref covers the stream and prefix `++` forms).

### B154. A namespace-qualified variable template is refused inside a function body

```cpp
namespace ns {
template <class T>
inline constexpr bool ev = sizeof(T) > 1;
}
static_assert(ns::ev<int>, "x");
int main() { constexpr bool b = ns::ev<int>; return b ? 0 : 1; }
```

- g++ 13 and clang++ 18 (`-std=c++20`): exit 0. madc (2026-10-03): the
  `static_assert` passes; the use in `main` is refused, `'ev' is not a
  member of namespace 'ns'`, exit 1. The unqualified form (no namespace)
  compiles. v0.100.1 refuses it the same way.
- Layer not yet traced: the qualified-name arm of the function-body
  expression parser misses the namespace's variable templates, which the
  constant-expression reader finds.
- Found 2026-10-02 while fixing the libc++ `testifconstexpr` regression
  (tests/testvartemplaterequires). Off the release path, filed per owner
  2026-09-30.

### B155. A requires-expression in a variable template's initializer is refused when used at run time

```cpp
template <class T>
inline constexpr bool ev = sizeof(T) > 1 || requires { (T*)nullptr; };
int main() { return ev<int> ? 0 : 1; }
```

- g++ 13 and clang++ 18 (`-std=c++20`): exit 0. madc (2026-10-03): `use of
  undeclared identifier 'requires'`, exit 1. v0.100.1 compiled it and
  returned 1 (a silent wrong answer); today's refusal is loud.
- The same initializer in a `static_assert` folds correctly
  (tests/testvartemplaterequires). Layer: the runtime expression parser has
  no requires-expression primary; the constant-expression reader has one.
- Found 2026-10-02 with B154. Off the release path, filed per owner
  2026-09-30.

### B159. libc++: a `std::tuple` of two or more elements cannot be constructed

```cpp
#include <tuple>
#include <cstdio>
int main() { std::tuple<int, double> t(3, 2.5);
	std::printf("%d %g\n", std::get<0>(t), std::get<1>(t)); return 0; }
```

- clang++ 18 `-stdlib=libc++`: `3 2.5`. madc `-stdlib=libc++` (2026-10-03):
  `cir error: no matching constructor for call to '__tuple_impl<...>(
  __tuple_indices<0,1>, __tuple_types<int32_t,double>, __tuple_indices<>,
  __tuple_types<>, int32_t*, double*)'` at libc++ `tuple:587`. A
  one-element `std::make_tuple(3)` works. libstdc++ is correct.
- `__tuple_impl`'s constructor is a variadic template over two index packs
  and two type packs; the arguments arrive as pointers (`int32_t*`), so the
  forwarding `_Up&&...` parameters are not matched. Layer not yet traced.
- tests/testmaketuple and tests/testrvaluereftype fail this way under
  libc++ on every archived release since v0.72.0; both carry
  `.libcxx_skip` until this is fixed. Found 2026-10-03 in the libc++
  release-tier lane, filed per owner 2026-09-30.

### B160. libc++: `std::vector`'s iterator-pair constructor does not compile

```cpp
#include <vector>
#include <cstdio>
int main() { std::vector<int> r(3, 7); std::vector<int> c(r.begin(), r.end());
	std::printf("%zu %d\n", c.size(), c[0]); return 0; }
```

- clang++ 18 `-stdlib=libc++`: `3 7`. madc `-stdlib=libc++` (2026-10-03):
  c2mir refuses the instantiated libc++ bodies: `pointer to incomplete type
  as an operand of +` (`__memory/uninitialized_algorithms.h:579`) and
  `incompatible types in assignment to an arithmetic type lvalue`
  (`__memory/construct_at.h:52`). libstdc++ is correct.
- Layer not yet traced: an iterator type in `__uninitialized_allocator_copy`
  substitutes as an incomplete type.
- tests/testvectoriterpairctor fails this way under libc++ on every
  archived release since v0.72.0 and carries `.libcxx_skip` until this is
  fixed. Found 2026-10-03 in the libc++ release-tier lane, filed per owner
  2026-09-30.

### B151. C11 `_Noreturn` is refused as a function specifier

```c
void exit(int);
_Noreturn void f(void) { exit(0); }
int main(void) { f(); return 1; }
```

- gcc 13 (`-std=c17`) compiles and exits 0. madc (2026-10-02, `--std=c17`):
  `use of undeclared identifier '_Noreturn'`; `_Noreturn static void` gives
  the same error, and `static _Noreturn void` gives `Expecting type after
  'static'`. `<stdnoreturn.h>`'s `noreturn` therefore fails too.
- No test in `tests/` uses `_Noreturn`. glibc's own declarations use
  `__attribute__((__noreturn__))`, which parses.
- Layer: `_Noreturn` is in the keyword registry (`src/madc_keywords.cpp`,
  `KW_C("_Noreturn", STD_C11)`), but the declaration-specifier reader has no
  function-specifier arm for it (C11 6.7.4: it may appear anywhere among the
  specifiers, like `inline`).
- Found 2026-10-02 while adding the embedded `<stdnoreturn.h>`
  (tests/testfreestandingheaders checks the macro only). Off the release path,
  and a core-parser change, filed per owner 2026-09-30.

### B152. `offsetof` refuses a type defined in its operand

```c
#include <stddef.h>
int printf(const char *, ...);
int main(void) { printf("%zu\n", offsetof(struct { char c; int i; }, i)); return 0; }
```

- gcc 13 and clang 18 (`-std=c17`): `4`. madc (2026-10-02):
  `Unexpected keyword in expression` at the `struct`.
- C11 7.19p3 does not exclude a defined type (C23 7.21p3 makes it undefined),
  and both canon compilers accept it.
- Layer: not yet traced. The message suggests `offsetof`'s first operand is
  read as an expression rather than a type-name that may carry a
  struct-or-union-specifier (a guess).
- Found 2026-10-02 with B151. Off the release path, filed per owner
  2026-09-30.

### B153. Packed binary, `--project`, `<string>` in two TUs: SIGSEGV under forest bind

```cpp
// m.cpp
#include <cstdio>
int fa(); int fb();
int main() { printf("%d %d\n", fa(), fb()); return 0; }
// a.cpp
#include <string>
int fa() { std::string s("abc"); return (int)s.size(); }
// b.cpp
#include <string>
int fb() { std::string s("hello"); return (int)s.size(); }
```

- `cc.json` names the three TUs; `madc-release --project cc.json`.
- g++ 13 and clang++ 18: `3 5`. Packed `bin/madc-release` (2026-10-02):
  SIGSEGV, core dumped. `--no-forest-bind` prints `3 5`.
- Release archive (`scripts/release_bins.sh`): v0.92.0 through v0.94.0
  print `3 5`; v0.95.0 through v0.100.0 crash. It entered between v0.94.0
  (caae16af6) and v0.95.0 (36fc42fc5), the cold-startup release.
- Stack: `madc_thaw_member_template` ←
  `instantiate_member_ctor_template_candidate` ←
  `parse_functional_type_expression` ← `rebuild_forest_param_defaults` ←
  `flush_forest_pending_globals` ← `Program::tokenize` ←
  `madc_project_execute`. The second TU's bind thaws a member constructor
  template; layer not yet traced.
- tests/testprojectmtiorder runs with `--no-forest-bind` until this is
  fixed. Found 2026-10-02 in the seam battery's packed lanes. Off the
  release path (shipped since v0.95), filed per owner 2026-09-30.

### B148. A constructor template's mem-initializer that reads a member of a class temporary crashes c2mir

```cpp
#include <cstdio>
struct A { int v; A(int x) : v(x) {} A(const A &o) : v(o.v + 100) {} };
struct R { int w; template<class U> R(U &u) : w(A(u).v) {} };
int main() { A a(1); R r(a); printf("%d\n", r.w); return 0; }
```

- g++ 13 and clang++ 18: `101`. madc (2026-10-02, at 4bad47e26 plus the
  instance-symbol fix): SIGSEGV inside `c2mir_compile_tree`, exit 139.
  v0.99.2 refused it loudly instead (`undeclared identifier __madc_objtmp_1`,
  `request for member v in something not a structure or union`).
- The same mem-initializer in a non-template constructor
  (`P(A &u) : w(A(u).v) {}`) prints `101`. A forwarding form,
  `template<class U> S(U &&u) : w(A(std::forward<U>(u)).v) {}`, is refused
  with `lvalue required as unary & operand` instead of crashing.
- Layer: not yet traced. The class temporary in a member template's
  mem-initializer (the `__madc_objtmp` the v0.99.2 message names) has no
  declaration in the instantiated constructor's tree.
- Found 2026-10-02 while probing value categories in member constructor
  templates (the cross-TU instance-symbol fix). Off the release path, filed per
  owner 2026-09-30.

### B149. `std::map::emplace` leaves an undefined MIR import

```cpp
#include <cstdio>
#include <map>
int main() { std::map<int, int> m; m.emplace(3, 4); printf("%zu %d\n", m.size(), m[3]); return 0; }
```

- g++ 13 and clang++ 18: `1 4`. madc (2026-10-02, at 4bad47e26 and at
  v0.99.2): `MIR error: import of undefined item
  map_int32_t_int32_t_std__less_..._emplace`, exit 1. The same happens with
  `std::map<std::string, std::string>` and lvalue arguments.
- `std::vector::emplace_back(s)` with an lvalue works (B146 is the converting
  `emplace_back(3)`). No test in `tests/` calls `map::emplace`.
- Layer: not yet traced. `map::emplace` is a variadic member template that
  forwards to `_M_t._M_emplace_unique`; the call is bound to the placeholder
  symbol, and no instance is defined.
- Found 2026-10-02 with B148. Off the release path, filed per owner 2026-09-30.

### B146. `std::vector<S>::emplace_back(3)` is refused: tsubst bails on the converting placement construction

```cpp
#include <vector>
#include <cstdio>
struct S {
	int v;
	S(int x) : v(x) {}
	S(const S &o) : v(o.v) {}
	S(S &&o) : v(o.v) {}
};
int main() {
	std::vector<S> v;
	v.emplace_back(3);
	std::printf("%d %d\n", (int)v.size(), v[0].v);
	return 0;
}
```

- g++ 13 and clang++ 18 (`-std=c++17`): `1 3`. madc (2026-10-02): `cir error:
  parse-once internal: tsubst bailed on the covered instantiation
  '__new_allocator_S__construct__mti' of std::__new_allocator::construct<_Up,_Args...>
  [why: tsubst: dependent-arg object construction]` at `new_allocator.h:190`,
  nothing compiled. `push_back(S(1))` and `push_back(s)` work.
- Layer: `::new((void*)__p) _Up(std::forward<_Args>(__args)...)` with
  `_Up = S`, `_Args = int`. At instantiation time the unexpanded
  `TokenPackExpansion` (type `_Args*`) reaches `CirBuilder::object_arg_addr`'s
  materializing tail, which refuses a dependent-typed argument
  (`MADC_XTEST_DEPARG_DEBUG=1`: `[DEPARG-HIT] arg=TokenPackExpansion dd=_Args*
  target=S in=__new_allocator_S__construct__mti`). The same `construct` lowers
  for `push_back`'s copy and move (`_Args = S` / `const S&`); the converting
  `S(int)` case fails. It is present before and after the vecbind-gate fix.
- Found 2026-10-02 with B144. Off the release path, filed per owner 2026-09-30.

### B143. `&ns::C::m` is refused: the address-of arm walks no class after a namespace

```cpp
#include <cstdio>
namespace ns { struct C { int k; int f() { return 0; } }; }
int main() { int ns::C::*pk = &ns::C::k; ns::C c; c.k = 3; std::printf("%d\n", c.*pk); return 0; }
```

- g++ 13 (`-std=c++17`): `3`. madc (`--std=c++17`, 2026-10-02):
  `error: 'C' is not a member of namespace 'ns'` at `&ns::C::k`, exit 1. The
  value side (`sizeof(ns::C::k)`) resolves; a data-only `ns::N` fails the
  same way.
- Layer: `Program::parseAddressOfExpression`'s qualified-name arm reads ONE
  qualifier before the member; after a namespace it walks only a template-id
  (`&ns::Tmpl<args>::m`), never a class name. The value side's walk
  (the identifier arm's `ns::C::` blocks into `resolve_qualified_scope_expression`)
  is the owner to share.
- Found 2026-10-02 while fixing B77. Off the release path, filed per owner
  2026-10-02 (merge after B77).

### B142. c2m refuses a wide string literal for a `wchar_t` array, and an overlong string

```c
#include <wchar.h>
#include <stdio.h>
wchar_t w[3] = L"ab";
char s[2] = "abc";
int main(void) { printf("%d %d %c\n", (int)w[0], (int)w[2], s[1]); return 0; }
```

- gcc 13 = clang 18 (`-std=c17`): `97 0 b` (a warning for `s`, its excess
  dropped). madc (whose parser places both through `Program::fit_char_array`)
  prints `97 0 b`. The in-tree c2m driver (`obj/mir/host/c2m x.c -eg`) refuses
  both in `check_initializer`: "assignment of incompatible value" for `w`,
  "string is too long for array initializer" for `s`.
- Layer: c2mir `check_initializer` (a string literal initializing an array
  of a wide character type; C's drop of the excess, C11 6.7.9p14 + p2).
- Found 2026-10-02 beside the string-literal fit (`d39f231ea`). The c2m driver
  is the self-hosting compiler; madc never hands c2mir either shape.

### B140. C: excess elements in an initializer list are refused

```c
#include <stdio.h>
int a[2] = { 1, 2, 3 };
struct S { int x, y; } s = { 4, 5, 6 };
int main(void) { int l[1] = { 7, 8 }; printf("%d %d | %d %d | %d\n", a[0], a[1], s.x, s.y, l[0]); return 0; }
```

- gcc 13 = clang 18 (`-std=c17`): three warnings ("excess elements in array
  initializer", "... in struct initializer"), the excess dropped:
  `1 2 | 4 5 | 7`. g++/clang++ refuse it ("too many initializers").
- madc (`--std=c17`, 2026-10-02, `f055ae418` + the string-literal fit): `a`
  and `l` refused by the
  parser, "Too many initializers for array (expected 2)" / "(expected 1)"; the
  struct alone reaches c2mir and is refused there, "excess elements in
  array/struct/union initializer" (c2mir's `check_initializer`).
- C11 6.7.9p2 makes the excess a constraint violation; gcc's C dialect
  diagnoses it as a warning and drops it. madc's string-literal fit already
  does (`Program::fit_char_array`: drop + `report_warning`); the list case is
  refused at two layers instead — the parser's array capacity check in
  `parse_declaration_body` and c2mir's struct/union walk.
- The fix must keep madc's own static-data writer from writing past the
  array (drop the excess at the layer that places it), keep C++ refusing, and
  note that madc renders no warnings in batch mode (only the REPL renders
  recorded diagnostics), so a gcc-default warning is invisible.
- Found 2026-10-02 beside the string-literal fit. Off the release path, filed
  per owner 2026-09-30.

### B126. A polyglot public refuses a temporary for a read-only `value &` input

```cpp
int main() {
	println("{}", php::array_key_exists("a", { "a": 1 }));
	return 0;
}
```

- PHP: `array_key_exists("a", ["a" => 1])` is `true`. madc: `error: no matching
  overload for 'php::array_key_exists' with argument types (char*, madc::value)`.
- The dialect headers spell read-only carrier inputs `value &` (about 150
  parameters over `ns_php` / `ns_perl` / `ns_python` / `ns_ruby` / `ns_js` /
  `ns_rust` / `ns_madc`). [dcl.init.ref]/5 binds a prvalue only to a const
  lvalue reference, so a temporary is refused wherever an overload set ranks
  the binding (a one-candidate call still binds it). `php::array_push` is
  already `const value &` (its kind-preserving overload).
- Layer: the header signatures, through each runtime `extern "C"` entry (`const
  value *`, in `include/madc/ns_*.h` and `src/ns_*.cpp` together). Output
  parameters (`array &out`, the array a call mutates) stay `value &`.

### B115. A function template over a `const T&...` pack is refused: `eat()`, `eat(1)`

```cpp
#include <cstdio>
template<typename... T> int eat(const T&...) { return (int)sizeof...(T); }
int main() { printf("%d %d %d\n", eat(), eat(1), eat(1, 2.5)); return 0; }
```

- g++ 13 = clang++ 18 (`-std=c++17`): `0 1 2`. madc (`--std=c++17`, at
  `f6c808cc1` and with B112's fix): `MIR error: import of undefined item
  eat`, exit 1.
- `MADC_FNTPL_PROBE=eat` on g++.dg/cpp0x/variadic32.C: the empty pack's
  instantiation body throws (`Failed to find type when parsing function
  parameters`), and the instance registered for `eat(1)` (`eat__o2`, stamped
  `eat<int>`) is emitted with no parameters. The gxx-c++11 lane counts
  variadic32 and variadic33 as passing because it stops at `--emit=c11`.
- Found 2026-10-01 while validating B112. Off the release path, filed per
  owner 2026-09-30.
- Layer: not yet traced: the pack-expansion parameter declarator of an
  instantiated `const T&...` (empty and one-element packs).

### B107. `sizeof(typename C<T>::m)` outside a template is refused: "Expecting identifier"

```cpp
#include <cstdio>
template<typename T> struct traits {};
template<typename T> struct traits<T *> { typedef int category; };
int main() { traits<int *>::category c = 3; printf("p1 %d %zu\n", c, sizeof(typename traits<int *>::category)); return 0; }
```

- g++ 13 and clang++ 18 (`-std=c++17`): `p1 3 4`. madc: `4:109: error:
  Expecting identifier` at the `typename` inside `sizeof(...)`.
- C++11 allows `typename` before a qualified name outside a template
  ([temp.res]/5). The same operand in a template's default argument
  (`enable_if<sizeof(typename traits<It>::category) != 0>`) is refused too,
  which there surfaces as a lost candidate: `no matching constructor`.
- Found 2026-10-01 during B96's recon. Off the release path, filed per owner
  2026-09-30.
- Layer (suspected): `sizeof`'s type-id operand reader (`parse_type_id` is
  the type-id owner, `indirection.md`) does not take a `typename`-prefixed
  qualified name.

### B100. A `const var &` parameter refuses a text prvalue: `take("x")`, `take(format(...))`

```cpp
void take(const var &v)
{
    println("got {}", v);
}
int main()
{
    const char *s = "lit";
    take(s);			// binds
    take("direct");		// refused
    take(format("f{}", 1));	// refused
    return 0;
}
```

- madc (`--std=madc`, `bin/madc` at `67660bc79`): `9:14: assignment of
  incompatible value` and `10:13: ...` from c2mir's check, `cir_compile
  failed`. A named `const char *` binds.
- g++ 13 and clang++ 18, with a class `value` constructible from `const
  char *` in its place: `got lit` / `got direct` / `got f1`. madc
  `--std=c++17` on that same C++ prints the same three lines, so the gap is
  the carrier's own temporary, not reference binding in general.
- Found 2026-10-01 writing the plugin API's `ide::set` (plan §41.11a step 4),
  which gains `ui::set`'s overloads (`const char *`, integer, bool, real)
  beside `const var &`, as the engine's bag writes have.
- Layer: cir's reference-binding owner, `ref_param_arg_addr` →
  `ref_param_arg_addr_from_value` (src/cir_builder.cpp): a carrier referent
  with a converted temp from a text prvalue is emitted as an assignment
  into the carrier's storage, which C has no form for; it needs the
  carrier's construction from text (the path a named `const char *` takes).

### B98. A value literal cannot nest a brace list: `{ "rows": {} }` is refused

```cpp
int main()
{
    var x = { "rows": {} };		// an empty array as a field's value
    var y = { "a": { "b": 1 } };	// an object in an object
    var z = { { 1, 2 }, { 3 } };	// an array of arrays
    return 0;
}
```

- madc (`--std=madc`): each literal is refused, `error: Nested brace list in
  a value literal is not supported (yet)`, at the inner `{`. JSON, a
  JavaScript object literal (`{ rows: [] }`) and PHP (`['rows' => []]`)
  all nest. Spelling the inner value as a named `var` first (`var none =
  {}; var x = { "rows": none };`) compiles.
- The refusal is deliberate and loud (`Program::parse_ctor_args_list`,
  src/parser.cpp): a `{`-headed element never reaches `parseExpression`,
  which has no brace-head reading. The inner list's target is the carrier
  itself (a `var` element), so no constructor selection is needed to type
  it.
- Found 2026-09-30 writing `tests/testmadcide_contrib.mad` section 10 (plan
  §41.11a step 3d), which spells the inner array as a named `var` meanwhile.
- Layer: the carrier list's element reader. A carrier list's `{` element
  should be read as a nested carrier literal (the same reader, recursive).
  A core parser change: its own focused session (owner, 2026-09-13).

### B74. `std::make_shared` does not compile: its tag constructor is not a candidate

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

- g++ 13 = clang++ 18: `sp1: 3`. madc (`--std=c++17`): `cir error: no
  matching constructor for call to 'shared_ptr_std__vector_...(std::
  _Sp_alloc_shared_tag<std::allocator<void>>, int32_t*, int32_t*)' @
  bits/shared_ptr.h:1009:23`. Reducer: `tmp/b74/s1.cpp`.
- Where: `std::allocate_shared` calls the private constructor template
  `template<typename _Alloc, typename... _Args> shared_ptr(
  _Sp_alloc_shared_tag<_Alloc>, _Args&&...)` with the forwarded arguments;
  that template is not among the candidates. Not yet reduced below
  libstdc++.
- The GCC `__atomic_*` builtins it needed first are implemented
  (include/atomic_builtins.h). `--std=c++20` still stops earlier, at
  `bits/atomic_wait.h:144:26: use of undeclared identifier
  '__builtin_ia32_pause'` (the spin-wait hint).
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

### B161. libc++: copying a `std::unique_ptr` is reported as "no matching constructor"

```cpp
#include <memory>
int main()
{
	std::unique_ptr<int> p(new int(3));
	auto q = p;
	return *q;
}
```

- clang++ 18 `-stdlib=libc++`: `call to implicitly-deleted copy constructor
  of 'std::unique_ptr<int>'`. madc rejects it under libstdc++ with `use of
  deleted copy constructor` (tests/testdeletedcopyunique). madc
  `-stdlib=libc++` (2026-10-03) rejects it too, as `cir error: no matching
  constructor for call to 'unique_ptr_int32_t_...'`, which names neither
  the deleted constructor nor the user's type spelling (B108).
- Probably the same root as B158: libc++'s copy constructor is implicitly
  deleted (a user move constructor), where libstdc++ spells `= delete`.
  Not yet traced.
- tests/testdeletedcopyunique carries `.libcxx_skip` until this is fixed.
  Found 2026-10-03 in the libc++ release-tier lane, filed per owner
  2026-09-30.

### B114. A call whose function-template deduction fails is reported as an undefined MIR import

```cpp
#include <iterator>
int main() { int a[3] = { 1, 2, 3 }; std::__iterator_category(a); return 0; }
```

- g++ 13: `2:62: error: no matching function for call to
  '__iterator_category(int [3])'` (`const _Iter&` deduces `int [3]`, and
  `iterator_traits<int [3]>` has no `iterator_category`). clang++ 18:
  `2:38: error: no matching function for call to '__iterator_category'`.
- madc (`--std=c++17`, at `f6c808cc1` and with B112's fix): `MIR error:
  import of undefined item __ns_std___iterator_category`, exit 1, with no
  source position.
- The same for a set with specializations: `long x = 5; std::max(x, 0L);
  std::max(x, 0);` (g++: `no matching function for call to 'max(long int&,
  int)'`). Since B112's fix, `std::max(x, 0)` no longer binds `max<long>`;
  it is refused with `import of undefined item __ns_std_max`.
- Found 2026-10-01 during B111's recon. Off the release path, filed per owner
  2026-09-30.
- madc's deduction lane also reports "failed" for shapes it does not model
  (an empty pack, a non-type explicit argument; g++.dg/cpp0x variadic32,
  nontype4), so a failed deduction alone is not a "no candidate" verdict.
- Layer (suspected): a one-member set (the placeholder) never reaches the
  ranker's strict no-viable verdict (`find_namespace_function_overload`
  returns early below two members), so `CirBuilder::call_target_variable`
  falls back to the placeholder. The verdict needs the deduction lane to
  tell a genuine failure ([temp.deduct]) from a shape it does not model.

### B108. "no matching constructor" spells an argument's type as madc's internal name: `int32_t`, and an array as its element

```cpp
struct V { V(unsigned long, const int &) {} };
int main() { int arr[2] = { 7, 8 }; V y(arr, arr + 2); return 0; }
```

- g++ 13: `2:41: error: invalid conversion from 'int*' to 'long unsigned
  int'` (the one candidate, the array argument decayed to `int*`). clang++
  18: `2:39: error: no matching constructor for initialization of 'V'`.
- madc (`--std=c++17`, at `8f6df07b2`): `cir error: no matching constructor
  for call to 'V(int32_t, int32_t*)'`: `int` is spelled `int32_t`, and the
  array argument is spelled as its element type (neither `int [2]` nor the
  decayed `int*`).
- Found 2026-10-01 during B96's recon. Off the release path, filed per owner
  2026-09-30.
- Layer (suspected): the message's argument list is spelled from the
  flattened `datadef()` with the internal type names; the source's names and
  `Program::array_operand_type` are the owners (`indirection.md`; owner rule
  "show the source's name").

### B106. A class template's missing dependent member type is reported as an internal ClassPattern error at the instantiation

```cpp
template <typename T>
struct B
{
  typename T::U u;
};
struct E { int x; };
B<E> b;
int main() { return 0; }
```

- g++ 13: `4:17: error: no type named 'U' in 'struct E'`, with the
  instantiation at line 7 as a note. clang++ 18: `4:15: error: no type named
  'U' in 'E'`, the same note.
- madc (`--std=c++11`, at `f48f62bfb`): `7:1: error: internal: basic
  ClassPattern could not resolve type 1 kind=10 name='U' operand=2
  owner='(null)' in 'B'`, then `1 parse error(s) — compilation refused`. The
  refusal is right; the words are the resolver's state, and the position is
  the instantiation's, not the member's.
- Found 2026-10-01 while reducing the g++.dg/cpp0x/implicit7.C crash (the
  dangling throw position, fixed in f48f62bfb). Off the release path, filed
  per owner 2026-09-30.
- Layer: the ClassPattern type resolver (`src/parser.cpp`, the
  `pgm.Throw(binding.location) << "internal: basic ClassPattern could not
  resolve type "` arm). A `typename T::U` that names no type of the
  substituted class is [temp.res]'s user-facing error, cited at the member's
  declarator with the instantiation as a note.

### B105. `tests/testmadcide` built as a Windows executable hangs under wine after `colon-exec-ok`

```text
WINEDEBUG=-all WINEPATH='Z:\workspace\madc\bin' wine bin/madc-hosted-x86-64-windows.exe -o tmp/t.exe tests/testmadcide.mad
wine tmp/t.exe        # times out (120 s); the JIT run of the same test passes under wine
```

- Measured 2026-10-01 at `c30f723ff`, and the same with the tools of
  `fbfd79670` (the last green wine64 seam) on today's engine. The output stops
  after `colon-exec-ok` (`IdeSession::term_exec`'s `rqCMD`, a `system()` of
  `echo colon-exec-ok`), so `vi-load:` and every pin after it are missing;
  `build-out:`, `build-stop:` and `view1-node:` are missing earlier in the run.
- Not gated anywhere: the wine64 lane runs the suite JIT only
  (`remote_build.sh`'s wine stage has no `--exe`), and the genuine Windows lane
  runs `madc.exe` on each test. Off the release path, filed per owner
  2026-09-30.
- Layer: not yet traced (the executable's `system()` under wine, or the
  terminal and build pieces of the IDE's Windows executable).

### B104. An enumerator declared twice in one scope is accepted, and the later one wins

```c
#include <stdio.h>
enum provenance { pvNONE = 0, pvCOMPILER, pvGIT, pvEVENT };
enum again { pxA = 0, pxB, pvEVENT };
int main(void) { printf("pvEVENT=%d\n", (int)pvEVENT); return 0; }
```

- gcc 13 (`-std=c17`): `error: redeclaration of enumerator 'pvEVENT'`; g++ 13:
  `error: 'pvEVENT' conflicts with a previous declaration`; clang 18:
  `error: redefinition of enumerator 'pvEVENT'` (C11 6.7.2.2 with 6.7/3;
  [dcl.enum], [basic.scope.declarative]). madc (`bin/madc` at `89868e2de`,
  `--std=c17`, `--std=c++17` and the dialect alike): compiles, prints
  `pvEVENT=2`, exit 0. A SILENT wrong answer: the second declaration
  renumbers every later use of the name.
- Found 2026-10-01: plan §41.11a step 7's `enum plugin_verb` reused
  `pvNONE` and `pvEVENT` from `enum provenance`, and `graph.history`'s event
  rows came out labelled `record` (`tests/testgraphpast`, the batch
  checkpoint). The madcide names are fixed (`plv*`, and `ide_pmode`'s
  `lmNONE`, which collided with `lsp_method`'s at the same value), and
  `check-madcide-enums.sh` rule 7 refuses an enumerator declared twice in
  madcide's sources until the compiler does.
- Layer (suspected): the enumerator's declaration into its scope (the enum
  body's reader in the parser) adds the name without a lookup of an earlier
  declaration in the same scope. A parser change, for its own session.

### B103. A C function declared with `()` is spelled `int (void)`, not `int ()`

```c
int f();
int g(void);
%type f
%type g
```

- gcc 13 and clang 18 (`-std=c17`, the type in an `-Wint-conversion`
  diagnostic of `int x = f;`): `int ()` for `f` (no prototype, C17 6.7.6.3/14)
  and `int (void)` for `g`. madc's REPL (`--std=c17`, `bin/madc` at
  `d2e801a72`): `int (void)` for both. chthonia's Variables view shows
  `main int (void)` for `int main() {…}` the same way. Display only; exit 0.
- Found 2026-10-01 writing the Variables view's gate (plan §41.11a step 6).
- Layer (suspected): `TypeSpeller::parameter_list`
  (src/madc_type_spelling.cpp) writes an empty C list as `void`, because the
  function type records no difference between `()` and `(void)`. The fix
  needs that bit where the declarator reader builds the function type
  (`Program::parse_declarator`), so it is a parser change for its own
  session; under C23 both spellings are `int (void)`.

### B94. A declaration whose type is a qualified typedef name takes the typedef's header position

```text
#include <string>
std::string s = "hello";
?s
```

- Before 2026-09-30, madc's REPL (`--std=c++17`) said `Defined:   @
  /usr/include/c++/13/bits/stringfwd.h:77`, where the object is `REPL[2]:1`,
  and dropped `s` from `%whos`. SILENT then: `static std::string t = "x";`,
  `t += "y";`, `t` showed `"x"` (the session read `t` as a header's static,
  so each later unit had its own copy) where cling's rule and madc's own
  (a unit's statics are session names, plan §41.5a) give `"xy"`. An
  unqualified typedef (`size_t n = 3;`) was right.
- SILENT outside the REPL too: an ordinary C++ program's unreferenced
  `std::string s = make("s");` or `std::size_t n = count("n");` never ran
  its initializer (g++ and clang++ run it). The emitter's system-origin
  verdict (`td_system`) read the header's file, so the object became
  emit-if-referenced, as a header's own global is.
- Worked around 2026-09-30 (plan §41.11a step 3d): `record_global_top_decl`
  records the parser's position as an annotation (`TopDecl::parse_file` /
  `parse_line` / `parse_column`), and `Program::top_decl_position` uses it
  when the origin token is another file's. It is the one reader of a
  TopDecl's place: `?`, the unit-statics rule, the emitter's system-origin
  verdict and c2mir node position, the graph API's global node. Gates:
  `tests/testglobal_qualified_typedef_init.mad`; `test_repl_session`, "a
  qualified typedef's object is the entry's own (B94)";
  `scripts/check-one-top-decl-position.sh` (fulltest).
- What remains: the declaration's type token `tb` in `parseDeclaration` is
  the registered typedef's token (the header's) for a qualified name, where
  an unqualified name carries its use site, and every `TokenDecl` /
  `TopDecl` position copied from it, and the emitted global's `+madc`
  origin token, still cite the header (the D10 show's
  run placement already falls back to the entry's end for the same reason,
  `show_entry_value`). The qualified type-name reader should yield a
  use-site token (`clone_origin`), as the unqualified one does; then the
  annotation and the fallback go. A core parser change: its own focused
  session (owner, 2026-09-13).

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

## madcide (the editor)

### B88. vi NORMAL mode's Enter and Backspace edit the buffer

- Found 2026-09-30, while placing B55's Tab in the vi arm. In NORMAL mode
  the vi arm (`IdeSession::vi_event`) lets every key but a pending esc fall
  through to the shared `edit_key`, so Enter inserts a line break and
  Backspace deletes the byte before the caret. vim's NORMAL-mode Enter
  moves to the first non-blank of the next line and Backspace moves left;
  neither edits. (Arrows and Del already match vim: Del deletes the
  character under the cursor, as `x` does.) Measured headless on `ab`,
  caret 1, NORMAL mode: Enter leaves `a\nb`, Backspace leaves `b`.
- Fix: NORMAL mode's Enter and Backspace are motions (@normal data, as the
  printable commands are), never the edit core's text keys.
- Off the release path (owner 2026-09-30: the neovim personality's key
  review is an open question to the owner); filed.

### B165. In a built madcide, `{madc}` and the test runner name madcide itself

- `madc::compiler_path()` is the running executable
  (`madc_self_exe_path()`). Under the JIT (`madc tools/madcide/madcide.mad`)
  that is the madc CLI. In the packaged madcide (`madcide.exe`, the Linux
  package's `madcide`, chthonia) it is madcide, which does not take madc's
  command line. Two consumers pass it a madc command line:
  - `build_subst`'s `{madc}` (`tools/madcide/madcide_core.inc`), for
    manifest-declared commands such as `{madc} -o {base} {path}`;
  - the test runner (`tools/madcide/madcide_tests.inc`,
    `env MADC_BIN={self} ... run_tests.sh`), which would run every test
    through madcide.
- Run itself is not affected: since the run-child fix (MADC_RUN_CHILD,
  `include/madc_run_child.h`) every program on the engine serves its own Run
  child, so Run never needs a madc CLI.
- Open for the owner: these two need a madc CLI, which is not the running
  program. The installation ships one (`bin/madc.exe` beside
  `libmadc-0.dll`; the standalone chthonia package ships libmadc and madc,
  plugin design §9.3). Should a packaged madcide name the installation's
  madc for these two? Or should they refuse there? The 2026-08-27 ruling
  (never exec a madc) covers Build and Run, not these.
- Found 2026-10-03 while fixing the Windows Run recursion (the audit of
  `madc_self_exe_path()` callers).

## Build and packaging

### B172. A native macOS `make -C src` links the modules with GNU ld's flags

- `src/madcgit.mk` and `src/madcmark.mk` choose their arm by MODE: the
  hosted darwin MODEs get `-dynamiclib`, `-install_name` and `-load_hidden`.
  A native macOS build in the default MODE (`develop`, which is what the
  Homebrew formula's `make -C src` runs) falls to the Linux arm:
  `-Wl,-soname` and `-Wl,--exclude-libs,ALL`, which Apple's ld64 does not
  take, and a `.so` name.
- Not measured: there was no mac run. The brew lane is Linux-only
  (`scripts/brew_lane.sh`, linuxbrew), and the Mac bottle is held.
- Fix: one module-link owner keyed on the host's object format (ELF vs
  Mach-O), not on MODE, so a native darwin build links a module as the
  hosted darwin arm does. The name is not the problem: macOS `dlopen`
  ignores the extension, and a module is only ever opened, never linked
  with `-l`, so modules can be spelled `lib<module>.so` on both Linux and
  macOS (owner, 2026-10-04). That spelling moves
  `madc_module_library_spelling`, its gate, the module makefiles and the
  macOS packager together.
- Found 2026-10-04 while adding cmark-gfm to the Homebrew formula (the
  madcmark module).

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

### B166. No lane runs tests as Windows executables

- The win64 lanes (`remote_build.sh wine`, `headerless-win`,
  `scripts/win_suite.sh` on genuine Windows) run the JIT pass only; the
  `--exe` / `--obj` passes run on Linux (`exeobj`). So a defect that only a
  Windows native image shows has no lane. The Run recursion did: from
  7225fa035 (2026-09-08) a Windows executable that called `parse_run` or
  opened `madcrun://` (madcide.exe included) spawned itself with
  `--run-frozen=`. `testparserunfrozen` runs on every domain, and its win64
  `--exe` pass was red throughout (measured 2026-10-03: JIT 1/0, EXE 0/1),
  but nothing ran that pass.
- The owner's 2026-10-03 lane rule is "one runner pass per lane (--exe
  --obj)". Should the win64 full-suite lane run `--exe --obj` under wine?
  That costs the seam roughly one more win64 suite of wall time. Until it
  does, the Windows Run tests (`testparserunfrozen`, `testprojectrun`,
  `testmadcide`'s `run-window`) are run by hand with
  `MADC_BIN=bin/madc-hosted-x86-64-windows.exe MADC_WRAPPER=wine
  MADC_SKIP_EXT='win64 wine64' run_tests.sh --exe <names>`.
- Found 2026-10-03 with B165.

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

### B90. A declarator's top-level cv, restated three times

- Found 2026-09-30 while fixing B89 (a `const char *&` parameter refused as
  read-only). The rule: the leading cv run plus the base's east cv when no
  `*` intervenes, else the cv after the last `*`. Its owner is
  `Program::declarator_written_cv` (every bit the source wrote), and
  `declarator_object_cv` is that owner masked to `modeled_cv()`. B89's fix
  moved the three C++ const-flag readers onto it: the parameter loop
  (`FuncDef::const_params`, the read-only marking), the type-trait operand
  (`TraitTypeArg::referent_const`) and a local declaration's
  `decl_is_const`.
- Three copies build a TYPE from the same rule and remain, in
  `src/parser.cpp` at `068cf83c4` + the B89 fix:
  - `parse_type_id` (~30887), as `own_cv` and `top_cv`;
  - the typedef alias's `post_cv` (~54385);
  - `parse_declarator`'s reference referent `ref_cv` (~54920).
- Divergent on paper: the typedef and reference copies test `ptr_depth == 0`
  alone, and the owner tests `ptr_depth == 0 && nested_stars == 0` (a
  parenthesized `*`). No failing reducer yet.
- Fix: each asks `declarator_written_cv` and applies its own mask. Then a
  gate marker (`const_after_star ? cvCONST` outside the owner) at zero.

### B91. Where a user's configuration lives, in two programs

- Found 2026-09-30 in the bundle work's dupaudit. The compiler's
  `config_file::search_paths` (`src/madc_config_file.cpp`, madc.ini) reads
  `$XDG_CONFIG_HOME/<app>`, else `~/.config/<app>`. madcide's
  `madcide_config_dir` (`tools/madcide/madcide_plugins.inc`, settings.json
  and `plugins/`) reads `MADCIDE_CONFIG_DIR`, then the same two, and
  `%APPDATA%/madcide` on Windows before `~/.config`.
- Divergent on Windows: with no `XDG_CONFIG_HOME`, madc.ini is looked for
  under `$HOME/.config`, and madcide's configuration under `%APPDATA%`.
- Fix: one engine owner (`madc::user_config_dir(app)`, also served to the
  dialect), which both read. The Windows rule is the owner's to decide
  (`%APPDATA%` is the platform's convention).

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
