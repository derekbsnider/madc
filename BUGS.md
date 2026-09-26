# madc bug backlog

Defects found while working on something else, filed so the current work
keeps moving and fixed later in a burndown the owner schedules.

**Owner pause, 2026-09-25:** until the REPL arc makes real progress (plan
§41.2 onward), a defect found off the REPL's path is filed here instead of
being fixed on the spot. A defect that blocks the current REPL step is still
fixed at once. The `fix-what-you-find.md` rule is unchanged; this pause sets
it aside for now and lifts when the owner says so. The whole backlog is then
burned down in a dedicated session.

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

### B1. A C++ anonymous enum's enumerator past `int` is typed `int`

- Found 2026-09-25, while giving C enumerators their enum's type (`ba4b85774`).
  The gxx lane's `cpp0x/enum17.C` covers it too.

```cpp
#include <stdio.h>
enum { BIG = 0x100000000 };
int main() { printf("%zu %d\n", sizeof(BIG), (int)(BIG >> 32)); return 0; }
```

- g++, clang++: `8 1`. madc: `4 1`, because the enumerator is typed `int`.
  The folded value is kept, so only its type is wrong.
- Where: [dcl.enum]/5 gives an enumerator the enum's type after the closing
  brace. madc's retype at the `TokenENUM` close runs in C only, and the C++
  anonymous path keeps `int`.

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

### B26. A new-expression skips a class's default member initializers

- Found 2026-09-26, while testing D27's stubs with a virtual function (the
  session's `mk()->g()` gave 0). It fails in file mode too.

```cpp
#include <cstdio>
struct A { int n = 3; };
int main()
{
	A *a = new A;
	A *b = new A();
	A *c = new A{};
	printf("%d %d %d\n", a->n, b->n, c->n);
	return 0;
}
```

- g++, clang++: `3 3 3`. madc `--std=c++17`: `0 0 0`, silently. `A s;` on
  the stack gives 3, and so does a polymorphic class's stack object.
- Where: not traced. A class with default member initializers and no
  user-declared constructor has a non-trivial implicit default constructor
  ([class.default.ctor]/3), and the new-expression must call it
  ([expr.new]/22). The stack declaration's path applies the initializers,
  and the new-expression's does not.

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
