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
