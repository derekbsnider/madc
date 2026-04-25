# madc — Mad-C Programming Language

**My jit-Assembled Dialect of C** — a C-like scripting language that JIT-compiles directly to x86-64 machine code using [asmjit](https://asmjit.com/). No bytecode, no interpreter, no separate compilation step.

The "Mad" in Mad-C: mix functions from multiple programming languages in a single program.

> **Contributing / using an AI agent?** Start with [`AGENTS.md`](AGENTS.md)
> — it's the canonical briefing for every agent (Claude Code, Codex CLI,
> Gemini CLI, Copilot, Cursor, Aider, Windsurf). Rules are in
> [`.claude/rules/`](.claude/rules/), with reasoning in
> [`docs/rules/`](docs/rules/). Cross-agent session flow lives in
> [`docs/agent-handoff.md`](docs/agent-handoff.md).

---

## Quick Start

```bash
# Build
make -C src

# Run a program
bin/madc tests/testint.mad

# Run with debug trace
bin/madc -v tests/testint.mad

# Build first, then run a command against the fresh binary
scripts/build_then.sh bin/madc tests/testint.mad
```

## Multi-file Projects

Single-file programs are the default. For larger projects, the convention is
a top-level file named after the application (e.g. `smaug.mad`, `mygame.mad`)
that `#include`s the rest in the right order, with `int main()` last:

```c
// smaug.mad
#include "config.mad"
#include "mud.mad"
#include "tables.mad"
#include "comm.mad"
// ... other source files ...
#include "main.mad"   // contains int main()
```

Run the whole project with `bin/madc smaug.mad`. `#include "file.mad"` works
at the lexer level — filenames resolve relative to the including file, nested
includes are supported, and repeated includes are skipped within the same
compile.

---

## Language Features

- **Data types:** `int8_t`–`int64_t`, `uint8_t`–`uint64_t`, `float`, `double`, `char`, `string`, `array`
- **Typed containers:** `vector<int>`, `map<string, int>`, `set<string>`, `list<int>` — also as `std::vector<int>` etc.
- **Streams:** `cout`, `cerr`, `cin`, `stringstream`, `ifstream`, `ofstream`, `fstream`
- **Control flow:** `if`/`else`, `for`, `while`, `do`/`while`, `switch`/`case`/`default`
- **Range-based for:** `for (string name : names) { ... }` — works with array and vector
- **Ternary operator:** `condition ? true_expr : false_expr`
- **Functions:** user-defined with return values and parameters
- **Multiple return values:** `return q, r;` and `q, r := divide(17, 5);` (Go-style)
- **Function pointers:** `auto fn = my_func; fn(args);`
- **Lambdas:** `[](int a, int b) { return a + b; }` with `[&]` capture by reference
- **`defer`:** Go-style deferred execution at scope exit (LIFO order)
- **`auto` keyword:** type inference for function pointer and lambda declarations
- **`:=` short declaration:** `x := 42;` with type inference from RHS
- **`register` keyword:** explicitly register-only variables (never written to memory)
- **User-defined structs:** `struct Point { int x; int y; };`
- **Classes with methods:** `class Counter { int count; void inc() { count = count + 1; } };`
- **Namespaces:** `std::cout`, `madc::regex_match()`, `php::explode()`, `perl::grep()`, `python::title()`, `ruby::tr()`, `js::btoa()`
- **Regex:** `madc::regex_match()`, `madc::regex_search()`, `madc::regex_replace()`
- **Input:** `cin >> name >> age;` reads from stdin
- **`#include`:** `#include "file.mad"` for source inclusion
- **`using`:** `using namespace std;` or `using std::cout;`
- **`#load`:** `#load "libfoo.so" as foo;` for dynamic library loading
- **`dlopen`/`dlsym`/`dlcall`:** first-class dynamic linking
- **File I/O:** `ifstream`/`ofstream` with `open`, `close`, `good`, `eof`, `getline`
- **Subscript operator:** `a[0]`, `nums[i]`, `ages["key"]`
- **Escape sequences:** `\n`, `\t`, `\r`, `\\`, `\"`, `\0`

### Multi-Language Namespaces

The signature feature of madc — use the best functions from each language:

```c
#!/usr/bin/env madc

int main()
{
    // PHP-style string splitting and joining
    string csv = "alice,bob,charlie";
    string delim = ",";
    array names;
    php::explode(names, delim, csv);
    php::sort(names);
    string sorted;
    php::implode(sorted, delim, names);
    cout << sorted << endl;             // alice,bob,charlie

    // Perl-style regex grep
    array matches;
    string pat = "^a";
    perl::grep(matches, pat, names);    // apple, avocado

    // Python-style string formatting
    string title = "hello world";
    python::title(title);
    cout << title << endl;              // Hello World

    // Ruby-style string transforms
    string s = "aabbccdd";
    ruby::squeeze(s);                   // "abcd"

    // JavaScript base64
    string encoded;
    js::btoa(encoded, s);
    cout << encoded << endl;            // YWJjZA==

    // Regex
    int m = madc::regex_match(s, "[a-d]+");
    cout << m << endl;                  // 1

    return 0;
}
```

### Available Namespaces

| Namespace | Functions | Focus |
|-----------|-----------|-------|
| [`php::`](docs/language/ns-php.md) | 36 | String manipulation, array operations (explode, implode, sort) |
| [`perl::`](docs/language/ns-perl.md) | 21 | chop/chomp, grep (regex), glob, split (regex)/join, array ops |
| [`python::`](docs/language/ns-python.md) | 16 | Title case, alignment (center/ljust/rjust/zfill), format |
| [`ruby::`](docs/language/ns-ruby.md) | 12 | squeeze, tr (transliterate), chars, rotate, compact |
| [`js::`](docs/language/ns-js.md) | 6 | Base64 (btoa/atob), URL encoding, parseInt, JSON stringify |
| `std::` | 5 | cin, cout, cerr, endl, for_each |
| `madc::` | 4 | array, regex_match, regex_search, regex_replace |

Plus `#load` for any shared library via dlopen.

---

## Building

Requires:
- `g++` with C++11 support
- asmjit v1.14 installed at `/usr/local/` (see [`docs/build.md`](docs/build.md))

```bash
make -C src           # build bin/madc
make -C src clean     # clean objects
make -C src test      # run unit tests
```

---

## Testing

```bash
# Run unit + integration tests
make -C src fulltest

# Build first, then run one integration test through the batch runner
scripts/build_then.sh bash scripts/run_tests.sh tests/testint.mad
```

**Current status: 197 integration tests pass. 48 unit tests pass (25 datadef + 23 IR). (`make -C src fulltest`)**

(`testcin.mad` and `testargv.mad` are driven by `scripts/run_tests.sh` — it
feeds them stdin and argv respectively and asserts on their output.)

---

## Documentation

| Doc | Contents |
|-----|----------|
| [`docs/usage.md`](docs/usage.md) | Language reference, CLI flags |
| [`docs/language/ns-php.md`](docs/language/ns-php.md) | php:: namespace reference |
| [`docs/language/ns-perl.md`](docs/language/ns-perl.md) | perl:: namespace reference |
| [`docs/language/ns-python.md`](docs/language/ns-python.md) | python:: namespace reference |
| [`docs/language/ns-ruby.md`](docs/language/ns-ruby.md) | ruby:: namespace reference |
| [`docs/language/ns-js.md`](docs/language/ns-js.md) | js:: namespace reference |
| [`docs/language/modern/`](docs/language/modern/) | Range-for, function pointers, lambdas, defer |
| [`docs/language/switch.md`](docs/language/switch.md) | Switch/case/default statement |
| [`docs/language/input-operator.md`](docs/language/input-operator.md) | cin >> input operator |
| [`docs/language/class-methods.md`](docs/language/class-methods.md) | Class methods with this pointer |
| [`docs/language/regex.md`](docs/language/regex.md) | Regex functions |
| [`docs/language/multiple-returns.md`](docs/language/multiple-returns.md) | Go-style multiple return values |
| [`docs/language/ternary-operator.md`](docs/language/ternary-operator.md) | Ternary operator |
| [`docs/build.md`](docs/build.md) | Build requirements, asmjit setup |
| [`docs/architecture.md`](docs/architecture.md) | Compiler internals |
| [`docs/testing.md`](docs/testing.md) | Test guide |
| [`docs/test-status.md`](docs/test-status.md) | Per-test results |
| [`docs/agent-handoff.md`](docs/agent-handoff.md) | Cross-agent hand-off workflow and source-of-truth rules |
| [`AGENTS.md`](AGENTS.md) | Agent briefing — project rules, architecture, multi-tool setup |
| [`docs/rules/`](docs/rules/) | Reasoning behind each rule in `.claude/rules/` |
| [`CHANGELOG.md`](CHANGELOG.md) | Change history |

---

## Current Release

**v0.12.0** (2026-04-25) — SMAUG Phase F front-edge wave. Thirteen parser / lexer / compiler fixes surfaced while probing MadSMAUG translation units one at a time. Highlights: pointer-typed `*(ptr ± N)` / `*(p = ptr + N)`, single-pass macro parameter substitution (no more cascade through later param names), `extern` libc late-bind via dlsym, `*++p` not eating trailing binops, fn-ptr-member access not mis-firing through pending operators, constant-expression `<<`/`>>`/`&`/`|`/`^`, char literals inside function-like macro args, `struct {...} *p, *q;` declarators, `type const *p` / `int * const *xpp` interleaved CV-qualifier+star chains, `vfADDRTAKEN` pointer + `&ptr->member`, and a sweep converting compound-assign / inc-dec raw `throw` sites to token-aware `Throw(...)`. 197 integration + 48 unit tests (25 datadef + 23 IR) passing.

### Recent Releases

- **v0.12.0** — SMAUG Phase F front-edge wave: 13 parser/lexer/compiler fixes covering 9 MadSMAUG TUs (mud_prog.c, news.c, stances.c, tables.c, act_info.c, act_obj.c, boards.c, misc.c, update.c)
- **v0.11.0** — SMAUG Phase F front-edge resumption: umbrella compiles + runs end-to-end; `goto`/labels, struct-copy, `*p++=`, `(*p).m`, `expr[i].m`, compound-assign on subscripts, realpath includes, class-as-ident
- **v0.10.1** — Typed-register IR cleanup completion: stream/multi-return/direct-call cleanup, final compiler-site IR store/load ports, dead `typesafe.cpp` surface pruned
- **v0.10.0** — Typed-register IR scaffolding + bottom-up migration (Stages 0 – 3c): 15 shared compile-site helpers, ~880 lines removed from `compiler.cpp`, three latent bugs fixed
- **v0.9.1** — Silent codegen bug roll-up: ternary to Mem, shared literals, `int = -N`, fn-ptr casts, constant `case`, postfix chains
- **v0.9.0** — SMAUG Phase F continues: `->` on call results, Mem-backed arithmetic, unified control-flow parens, fn-ptr typedefs + decay, struct interop expansion, address-taken locals, assign-as-expression

## Roadmap

| Phase | Goal | Status |
|-------|------|--------|
| **Phase 1** | Foundation: verbose flag, char literals, struct fix, register, doctest | **Complete** |
| **Phase 2** | User-defined structs/classes, namespaces, #include, using | **Complete** |
| **Phase 3** | php::/perl::/python::/ruby::/js:: namespaces, dlopen, MadArray | **Complete** |
| **Phase 3.5** | Modern language features: range-for, function pointers, lambdas, defer, STL containers | **Complete** |
| **Phase 3.5+** | switch, cin, class methods, regex, multi-return, ternary, namespace scoping | **Complete** |
| **Phase 4 prep** | C preprocessor, 40 embedded headers, struct alignment, sizeof, argc/argv | **Complete** |
| **SMAUG A/B/C** | Pointers, `->`, casts, `&`, macros, unsigned/enum/static/typedef | **Complete** |
| **SMAUG D** | `va_list`/`<stdarg.h>`, variadic helpers, for-loop fix | **Complete** |
| **SMAUG E** | Fixed arrays, brace init, struct/array-of-struct init, chained member access, struct tm/timeval/fd_set, select() | **Complete** (v0.8.0) |
| **SMAUG F** | Language gaps surfaced by porting SMAUG 1.8. Port itself lives in [MadSMAUG](https://github.com/derekbsnider/MadSMAUG) | **In progress** (hashstr.mad first file ported) |
| **Phase 4** | `libmadc.so` embedding API | Planned |

---

## Architecture

```
Source (.mad file)
    |
    v src/lexer.cpp      — tokenize source (#include, #load handled here)
    |
    v src/parser.cpp     — build AST, namespace resolution, type registration
    |
    v src/compiler.cpp   — walk AST, emit x86-64 via asmjit
    |
    v JIT execute        — run machine code in-process
```

Namespace implementations: `src/ns_php.cpp`, `src/ns_perl.cpp`, `src/ns_python.cpp`, `src/ns_ruby.cpp`, `src/ns_js.cpp`, `src/ns_stl.cpp`
