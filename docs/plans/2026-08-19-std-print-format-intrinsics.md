# std::print / std::println / std::format as madc intrinsics

**Status:** SLICES 1+2 EXECUTED 2026-08-20 (engine @ `rt_format:` commit,
intrinsics + lowering @ the slice-2 commit; ships in v0.92.0). Phase 2
(vformat/runtime strings) and the residues below remain open.

## Executed notes (what the plan didn't know)

- The format string reaches the lowering as the `__literal__<text>`
  const char* Variable (addLiteral), not a TokenStr — the bytes are the
  name suffix, a convention three other builder sites already read.
- The by-value std::string return walks the call into class-return
  ELISION lanes that emit the callee symbol directly, bypassing the
  translate_expr intercept: `object_call_temp_addr` (operand shapes)
  now delegates to lower_format_call; the decl-init same-class elision
  declines intrinsics. Any future intrinsic returning a class BY VALUE
  must audit `git grep call_target_emit_name` the same way.
- Oracle findings: expected strings can embed NUL ("{:5c}" of 0 — the
  fixture rows carry explicit lengths); libstdc++ 13's "{:#.0f}" of
  DBL_MAX returns a corrupt buffer (byte 0 NUL, digits shifted;
  tmp/fmt/maxf.cpp) — cell excluded with evidence, engine keeps printf's
  correct output. Candidate upstream GCC report.
- 'c' presentation accepts exactly char's SIGNED range (-42 → byte
  \326, 255 throws); precision-less 'a'/'A' must be built bit-exactly
  from the double (printf %a prints denormals un-normalized).
- libstdc++ 13 treats char strings as BYTE strings (width counts bytes,
  precision truncates mid-UTF-8 — the 3-byte € probe) — pinned as-is.

## Residues (open, named)

1. **`var s = std::format(...)` errors loudly** — madc::value has no
   std::string ctor/assign ingestion overload (madarray_construct_* has
   cstr/int/real/bool/value only). Follow-up: add the std::string lane.
2. **Phase 2**: std::vformat / runtime format strings (the engine
   already runs the same grammar at runtime by construction).
3. Width/precision from arguments (`{:{}}`) — refused with a named
   message; cheap once wanted.
4. `#include <format>` / `<print>` SPELLINGS in madc mode (embedded
   provider stubs) — deferred; the bare identifiers are the surface.
5. `std::println()` zero-arg (bare newline) — the declaration requires
   a format string; add the overload if wanted.
6. No locale `L`, chrono/ranges formatting, wide output, or user
   `formatter<T>` — refused by name (parity: mostly ill-formed anyway).

**Status:** PLANNED (owner approved 2026-08-19: "yes, let's do it").
**Owner constraint (verbatim intent):** these are "just part of madc without
being slowed down by having madc parsing all kinds of expensive c++ headers" —
NO `#include <format>` / `#include <print>`, no libstdc++ `<format>` parse,
zero header cost. The C++23 spellings must simply work in a `.mad` program.

## Architecture — proven precedents, composed

The repo already proves every structural piece:

1. **Variadic declaration-only intrinsic**: `php::var_dump` is
   `template<class... Ts> void var_dump(const Ts &...vs);` — defined nowhere,
   carried by `FuncDef::inline_builtin_kind`, forest-serializable
   (`cir_freeze.h` extra_id). The front end (packs, `sizeof...`, call-site
   arity) needs nothing new.
2. **Compiler-generated body + runtime output primitives**: `cir_dump.cpp`
   "approach A" — the call site knows every concrete argument type, so the
   compiler generates the code; the runtime carries only primitives
   (`src/rt/rt_dump.c`). A program that never formats pays nothing.
3. **Host-implemented value rendering**: the `cout << value` contract
   (src/madc_value.cpp) and the tagged-value dispatch from the php dump
   campaign serve `{}` on a `value`/`var` argument.

## The pieces, in order of size

### 1. `src/rt/rt_format.c` — the format-spec mini-language engine (long pole)

One C89 implementation of the std format-spec grammar
(`[[fill]align][sign][#][0][width][.precision][type]` over
`b B c d o x X a A e E f F g G s p` plus `{}` defaults), beside `rt_dump.c`.

- Written ONCE, shared by both consumers (no-parallel-implementations):
  the CIR builder calls it at compile time to parse/validate the literal;
  the same code runs at runtime for phase-2 `std::vformat` / non-literal
  format strings.
- Float `{}` default is **shortest-round-trip** (`to_chars` semantics), NOT
  a fixed `%g` precision. C89-portable implementation: `snprintf("%.{p}g")`
  for p = 1…17, first string that `strtod`s back to identical bits. This is
  equivalent BY SPEC (`to_chars` general format is defined in terms of `%g`
  at shortest round-trip precision). No `-std=` bump needed anywhere.
- Sizing: flat scalar formatting, no recursive structure walk — smaller
  than the print_r/var_dump arc.

### 2. Compile-time format-string checking — free by construction

C++23 requires invalid literal format strings to be compile errors (real
libstdc++ does consteval gymnastics). We get it with zero consteval
machinery because the compiler IS the implementation: the CIR builder
parses the literal, checks each replacement field against the concrete
argument type (`{:d}` on a double, too few/many args, bad spec), and
diagnoses at compile time. Manual indexing (`{0}`/`{1}`) and `{{`/`}}`
escapes included.

### 3. CIR lowering

Split the literal into text runs + replacement fields; emit direct calls to
typed runtime primitives (int-with-spec, double-with-spec, string field,
char, bool, pointer) building into a growing buffer.

- `std::format` returns a real `std::string` (madc has the real class).
- `std::print` writes bytes to stdout — `vprint_nonunicode` semantics
  (stdio, NOT cout).
- `std::println(fmt, ...)` = `print` + trailing `'\n'`; `std::println()`
  (no args, C++26 courtesy g++ ships) = bare newline — decide at
  implementation time against the g++ oracle, default = parity.
- A `value`/`var` argument dispatches on its tag through one value-taking
  primitive — `std::println("{}", somevar)` works on madc's own type,
  consistent with the `cout << value` contract.

### 4. Registration — always-included madc-mode intrinsics

The three names register into namespace `std` at parser init under
`STD_MADC` only (the `value`/`var` registration model): declaration-only
variadic FuncDefs tagged `inline_builtin_kind` = `std_format` /
`std_print` / `std_println`. Zero include cost, zero parse cost. Strict
`--std=c++NN` lanes stay pristine (there the names come from real headers
someday, behind the concepts/consteval long pole — out of scope here).

### 5. Oracle + tests

- g++ 13.3 (container) compiles `<format>` under `-std=c++23` → direct
  oracle for `std::format` expectations.
- `<print>` is GCC 14+ (absent on container; clang 18 shares libstdc++ 13
  headers) → oracle for print/println is a g++-13 shim using
  `std::format` + `fwrite`, which is the standard's own definition.
- Tests: `tests/testformat.mad`, `tests/testprint.mad` (+ value-arg
  shapes), `.expect` generated from the oracle.

## Coverage boundary at first ship (report the SHAPE TABLE, not "done")

- **Covered:** built-in arithmetic types, `bool`, `char`, pointers,
  `const char*`, `std::string`, `value`/`array`; fill/align/sign/#/0/
  width/precision; all standard type presentations listed above;
  positional `{n}`; `{{`/`}}`.
- **NOT covered initially:** user-defined `formatter<T>` (calls on
  unformattable types get a clear compile diagnostic — parity: ill-formed
  in real C++ too), locale `L` forms, chrono/ranges formatting, wide
  output, runtime format strings (`std::vformat` — phase 2, the engine
  already runs at runtime by construction), width/precision from `{}`
  argument slots (decide in-slice; cheap once args marshal).
- **Deliberately NOT proposed:** madc-dialect magic (`{}` on any user
  struct via a generated member-walking formatter, print_r-style). Easy on
  this architecture but a C++-parity deviation → needs explicit owner
  approval; default off.

## Sequencing

After the v0.92.0 bare-`cout << value` release. Slices:
1. `rt_format.c` engine + unit tests against the g++-13 `<format>` oracle
   outputs (engine-level fixtures).
2. Intrinsic registration + CIR lowering for literal format strings;
   integration tests; compile-diagnostic tests (`.expect_err`).
3. Phase 2 (separate): `std::vformat` / runtime strings; `FILE*`/`ostream`
   overloads of print if wanted.

Ships WITH a release per cadence law.
