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
