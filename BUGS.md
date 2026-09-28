# madc bug backlog

Defects found while working on something else, filed so the current work
keeps moving and fixed later in a burndown the owner schedules.

**Owner pause, 2026-09-25, LIFTED 2026-09-28:** while the REPL arc's first
slice was built (plan §37), a defect found off the REPL's path was filed here
instead of being fixed on the spot. With §37 complete, the owner lifted the
pause: a defect found now is fixed in its own commit (`fix-what-you-find.md`),
never added here, and this backlog is being burned down.

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

### B12. A C file-scope declaration with no type specifier is dropped

- Found 2026-09-25, while measuring C89's implicit-int reading for §41.2a
  slice 2b.

```c
#include <stdio.h>
int y;
y = 4;
int main(void) { printf("%d\n", y); return 0; }
```

- gcc `-std=c89`: `4`, warning "data definition has no type or storage
  class". gcc `-std=c17`: `4`, plus "type defaults to 'int'". clang
  `-std=c89`: `4`. clang `-std=c17`: error, "ISO C99 and later do not support
  implicit int". madc `--std=c89` and `--std=c17`: `0`, silently.
- Undeclared, `x = 3;` at file scope is madc's "use of undeclared identifier
  'x'" where gcc declares `int x = 3`.
- Where: a file-scope `name = e;` under `knr_supported()` parses as an
  expression (parseExprStmt), and parse_toplevel sends the result to
  `tp->statements`, which nothing lowers. The implicit-int reading is a
  declaration with no declaration-specifiers (C89 6.5; gcc extends it to
  data definitions). It belongs in parseStatementBody's identifier arm,
  beside the implicit-int function definition (`name(params) { body }`).
- Keep the session's reading when fixing it: in an interactive entry, a
  declared name's `y = 4;` is an assignment statement (D3), never a
  redeclaration. The implicit-int reading is for file mode, and for an
  undeclared name.

### B13. `int(f(3));` in a function body is read as a declaration

- Found 2026-09-25, while probing top-level cast statements for §41.2a
  slice 2b. The same happens at an interactive entry's top level.

```cpp
#include <cstdio>
int log_v = 0;
int step(int d) { log_v = log_v * 10 + d; return d; }
int main() { int(step(3)); printf("%d\n", log_v); }
```

- g++, clang++: `3`. madc: `0`, silently. The call never runs.
- `int(step(3))` cannot be a declaration: a parenthesized declarator is
  `( declarator )`, and `step(3)` is not one, since `3` is no
  parameter-declaration ([stmt.ambig], [dcl.ambig.res]). So it is a
  functional cast whose operand is the call.
- Where: `datatype_statement_starts_functional_expr` (parser.cpp) decides
  a type-headed statement is an expression only for its cases (a)–(e). A
  group whose first declarator-id is followed by a non-declarator `(` list
  falls through to parseDeclaration. As a declaration, madc reads
  `int step(3)`, a variable `step` initialized to 3, which shadows the
  function (an earlier line calling `step` failed with "called object is
  not a function").

### B24. A weak function definition is emitted strong

- Found 2026-09-26, while measuring ld's weak and strong rule for D27. It
  is not on the REPL's path: D27's session stubs set their binding in MIR
  directly.

```c
/* w1.c */
#include <stdio.h>
__attribute__((weak)) int f(void) { return 1; }
int main(void) { printf("%d\n", f()); return 0; }
/* w2.c */
int f(void) { return 2; }
```

- gcc, clang (`w1.c w2.c`): `2`. madc `--project` over both TUs, in
  either order: `1`, silently. With a third function `int (*get(void))(void)
  { return f; }` in w2.c and `fa == get()` in w1.c, gcc says `1` and madc
  `0`: two addresses for one function.
- Where: the MIR dump has no `weak f` line, so the builder drops the
  attribute on a function definition (`var_decl` carries `linkonce` for a
  C++ inline variable only). `--project` then keeps both copies, since it
  permits func redefinition (last copy wins), and each TU's own calls bind to
  its own copy. Two layers: the builder must emit the binding, and MIR's
  loader must let a strong definition replace a weak one (D27 adds that
  rule, adopting the weak definition's address).

### B35. An array compound literal is typed as a pointer

- Found 2026-09-27, while measuring what an entry shows for D12 (plan
  §41.6a). File mode does the same.

```c
#include <stdio.h>
int main(void)
{
	printf("%zu %zu\n", sizeof((int[]){7,8}), sizeof((int[3]){0}));
	return 0;
}
```

- gcc 13: `8 12`. madc `--std=c17`: `8 8`, silently. `(int[3]){0}` has the
  type `int[3]` (C11 6.5.2.5p4), and madc gives it a pointer's.
- An entry's `(int[]){7,8}` shows `(int *) 0x7ffd...` under every standard,
  where an `int a[2]` shows its elements.
- Where: the parser wraps the literal in `TokenCast(T *, literal)` when it
  builds it ("Array compound literals decay to pointer", parser.cpp ~42830),
  so every reader, sizeof included, sees the pointer. The literal's own type
  is a synthetic `__compound_array` struct. The faithful shape is the
  literal typed `T[N]`, decaying where it is used (`array_decay_pointer`).
  That is a parse-tree change of its own.
- Its storage sibling is fixed (2026-09-28): `(int[3]){0}` held ONE element,
  since the literal was emitted unsized, and a store to p[2] overwrote the
  next object.

### B36. A file-scope `std::unique_ptr` is not initialized

- Found 2026-09-27, while measuring which values D12 can copy (plan §41.6a).

```cpp
#include <memory>
#include <stdio.h>
std::unique_ptr<int> p(new int(3));
int main() { printf("%d\n", p ? *p : -1); return 0; }
```

- g++ 13: `3`. madc `--std=c++17`: `-1`, silently. The same global written
  `std::unique_ptr<int> q = std::make_unique<int>(5);` crashes (SIGSEGV)
  before `main`'s first statement, where g++ prints 5.
- An entry's `std::unique_ptr<int> p(new int(3));` then `p` shows
  `std::unique_ptr<int32_t,std::default_delete<int32_t>>{ ._M_t = { ._M_t =
  { ._M_head_impl = (int *) nullptr } } }`. The spelling is wrong too: the
  source's is `std::unique_ptr<int>`, without the default argument.
- Where: not traced. The constructor's argument never reaches the global's
  construction.

### B50. `&arr` is typed as a pointer to the element, not to the array

- Found 2026-09-27, while building D12's slice 2 (plan §41.6a), whose array
  result is a pointer to the array.

```c
#include <stdio.h>
int arr[3] = { 1, 2, 3 };
int main(void) { printf("%zu %zu %d\n", sizeof *&arr, sizeof &arr, (*&arr)[2]); return 0; }
```

- gcc 13: `12 8 3`. madc `--std=c17` and `--std=c++17`: `4 8 3`, silently.
  `&arr` has the type `int (*)[3]` (C11 6.5.3.2p3, [expr.unary.op]/3), so
  `*&arr` is the array and `&arr + 1` steps 12 bytes.
- Where: `Program::build_address_of` types a named operand from its
  Variable's type (`addressof_result_type(tv->var.type)`), and madc stores an
  array Variable flattened, its element as its type. The array an operand
  denotes is `array_operand_type`'s answer. The fix changes the type of
  every `&array`: a focused session, with the lanes (owner rule, core
  changes). D12's array results wait on it.

## Accepts invalid code

### B2. A stray top-level `}` is accepted

- Found 2026-09-25, while building the REPL input classifier.

```c
int main(void) { return 0; }
}
```

- gcc: `expected identifier or '(' before '}' token`. clang: `extraneous
  closing brace ('}')`. madc: compiles, and the program exits 0.
- Where: the `tkClBrc` arm of `parseStatementBody`. When `compounds` is
  empty it should throw clang's wording. `extern "C"` blocks and namespace
  bodies consume their own `}`, so they are unaffected as far as checked.

### B14. `:=` is accepted under every C and C++ standard

- Found 2026-09-25, while checking which of the statement starters matter to
  a C session (§41.2a slice 2b).

```c
int main(void) { x := 3; return x; }
```

- gcc `-std=c17`: "expected expression before '=' token" and "'x'
  undeclared". clang: "use of undeclared identifier 'x'" and "expected
  expression". madc `--std=c17` and `--std=c++17`: compiles, and the program
  exits 3. madc-dialect syntax is silently accepted (invariant I8).
- In a `--std=c17` session, `int g = 0;` then `g := 3;` is accepted and
  leaves `g` at 0. The `:=` declares a global whose initializing statement
  no one runs, since only madc's statement starter arms `:=`.
- Where: the lexer makes `tkColEq` under every standard (lexer.cpp, the
  `':'` arm), and parseStatementBody's `:=` arm has no dialect gate. The gate
  is the madc dialect (`language_std == STD_MADC`), in one predicate beside
  `ufcs_enabled()`. Under C and C++, `:` then `=` should lex as two tokens.

### B15. A `for`-init declaration is accepted under `--std=c89`

- Found 2026-09-25, in a `--std=c89` session probe for §41.2a slice 2b.

```c
int main(void) { int s = 0; for (int i = 0; i < 3; i++) s += i; return s; }
```

- gcc `-std=c89` (with or without `-pedantic-errors`): "'for' loop initial
  declarations are only allowed in C99 or C11 mode". clang `-std=c89`: a
  `-Wgcc-compat` warning, and it compiles. madc `--std=c89`: compiles, exit 3.
- Where: the `for` statement's init clause (`TokenFOR::parse`) takes a
  declaration under every standard. C99 6.8.5.3 added it; before C99 the
  clause is an expression. The gate is `language_std` below `STD_C99` in the
  C range (C78 to C95).

### B37. Copying a `std::unique_ptr` is accepted, and crashes

- Found 2026-09-27, while measuring which values D12 can copy (plan §41.6a).

```cpp
#include <memory>
#include <stdio.h>
int main() { std::unique_ptr<int> p(new int(3)); auto q = p; printf("%d\n", *q); return 0; }
```

- g++ 13: "use of deleted function 'std::unique_ptr<...>::unique_ptr(const
  std::unique_ptr<...>&)'". madc `--std=c++17`: compiles, then SIGSEGV.
- Where: not traced. A deleted copy constructor is dropped from the class's
  constructors, and `has_deleted_copy_ctor` records it for the
  `__is_constructible` trait. Whether it is set for `unique_ptr`, and whether
  a declaration's initialization reads it, is the first thing to check.

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

## Refuses valid code

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

### B19. An out-of-line definition of a namespace member is refused

- Found 2026-09-26, during slice 3 (measured at `431ee5ef1`). A session is
  affected the same way.

```cpp
namespace N { int f(); }
int N::f() { return 11; }
int main() { return N::f() - 11; }
```

- g++, clang++: accept, exit 0. madc: `2:5: error: Unknown C++ declarator
  scope 'N'`.
- Where: `parseDeclaration`'s qualified declarator-id resolves its scope with
  `resolve_qualified_class_owner`, which knows classes only.

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

### B25. A declared function returning a function pointer is prototyped `long long`

- Found 2026-09-26, while writing the weak reducer for D27 (B24).

```c
int (*get(void))(void);
int one(void);
int same(void) { return one == get(); }
int call(void) { return get()(); }
```

- gcc, clang `-c -Wall -Wextra`: clean. madc `--std=c17`: `3:30: warning --
  comparison of integer with a pointer`. `--emit=c11` prints `extern long
  long get(void);`, and `get()()` is then a call of a `long long`. The MIR
  proto is `i64`, which happens to work on x86-64.
- With the definition later in the same TU, madc is right (`1 1`, no
  warning). Only a declaration that no definition follows is mistyped.
- Where: not traced. The front end already mistypes it (the warning is
  parse time), and the builder's prototype follows.

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

### B42. `auto` from an array deduces its element type

- Found 2026-09-27, while measuring the type D12's result would take (plan
  §41.6a).

```cpp
#include <stdio.h>
int a[3] = {1,2,3};
auto q = a;
int main() { auto lq = a; printf("%zu %d %d\n", sizeof lq, q[1], lq[2]); return 0; }
```

- g++ 13: `8 2 3` (`auto` decays an array to a pointer, [temp.deduct.call]/2).
  madc `--std=c++17`: "assigning pointer without cast to integer", then
  "subscripted value is neither array nor pointer nor vector" at both uses.
  Block scope does the same.
- Where: `deduce_expr_type` answers `operand_value_datadef`, which gives an
  array variable's element type (madc stores an array flattened, its element
  in the Variable's type). The array-to-pointer and function-to-pointer
  conversions of the deduction are missing.

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

## Runtime hangs

### B54. A byte channel closed under a parked `chan_select` hangs the waiter on Linux

- Found 2026-09-28, while building item 9 slice 2 (plan §41.9a): the same
  mechanism hung a pump parked on a session through a restart, fixed there
  by `taskio::handle_closing`. `madc::channel::close()` does not call it yet.

```c
long waited = -2;
void waiter(long c, long done)
{
    var cases;
    cases[0] = c;
    var v;
    waited = madc::chan_select(v, cases);
    var one = 1;
    madc::chan_send(done, one);
}
int main()
{
    madc::channel ch("exec://sleep 2");
    long c = madc::chan_readable(ch);
    long done = madc::chan_make(1);
    go waiter(c, done);
    madc::sleep_ms(100);	// the waiter parks on the pipe
    ch.close();
    var got;
    madc::chan_recv(got, done);
    println("waiter returned {}", waited);
    return 0;
}
```

- Expected (the poll() path, macOS): the closed descriptor reports POLLNVAL,
  the waiter rescans, finds its only case dead, and `chan_select` returns -1.
  madc on Linux: hangs (killed by `timeout 15`, exit 124). The reactor
  watches every descriptor with epoll, which forgets a closed one silently.
- Fix: `channel::close()` (`src/madc_channel_object.cpp`) calls
  `madc::taskio::handle_closing(read_wait_handle(), read_wait_kind())`
  before closing its endpoint, as `SessionClient::stop()` does; the reducer
  becomes a test.

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
