# madc — Mad-C Programming Language

**My jit-Assembled Dialect of C** — a C-like scripting language that JIT-compiles directly to x86-64 machine code using [asmjit](https://asmjit.com/). No bytecode, no interpreter, no separate compilation step.

The "Mad" in Mad-C: mix functions from multiple programming languages in a single program.

> **Contributing / using an AI agent?** Start with [`AGENTS.md`](AGENTS.md)
> — it's the canonical briefing for every agent (Claude Code, Codex CLI,
> Gemini CLI, Copilot, Cursor, Aider, Windsurf). Rules are in
> [`.claude/rules/`](.claude/rules/), with reasoning in
> [`docs/rules/`](docs/rules/).

---

## Quick Start

```bash
# Build
make -C src

# Run a program
bin/madc tests/testint.mad

# Run with debug trace
bin/madc -v tests/testint.mad
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
at the lexer level — filenames resolve relative to the including file, and
nested includes are supported.

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
```

**Current status: 96 integration tests pass. 25 unit tests pass. (`make -C src fulltest`)**

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
| [`AGENTS.md`](AGENTS.md) | Agent briefing — project rules, architecture, multi-tool setup |
| [`docs/rules/`](docs/rules/) | Reasoning behind each rule in `.claude/rules/` |
| [`CHANGELOG.md`](CHANGELOG.md) | Change history |

---

## Current Release

**v0.8.0** (2026-04-17) — SMAUG Phase E complete + Phase F start. C fixed arrays (1D + multi-dim + global), brace initializer lists, string-literal char-array init, `char *msg = "literal"` sugar, struct initializer lists, array-of-structs init, chained `->`/`.` member access, `str.length()` / `.size()` methods, crash handler with backtrace, `__FILE__`/`__LINE__`, raw-pointer `ptr[i]` subscript, `struct tm`/`timeval`/`fd_set` with glibc-matching layouts + `FD_*` macros + end-to-end `select()`, self-referencing structs, three-word compound types, multi-variable declarations, `stdin`/`stdout`/`stderr`, for-loop comma expressions, forward decl + definition. The SMAUG 1.8 port begins in the separate [MadSMAUG](https://github.com/derekbsnider/MadSMAUG) repo (`hashstr.mad` first target). 88 integration + 25 unit tests pass.

### Recent Releases

- **v0.8.0** — SMAUG Phase E complete + Phase F start: C arrays, brace init, struct interop, select(), MadSMAUG port begins
- **v0.7.0** — SMAUG Phase D: va_list/stdarg.h, vsprintf helpers, -rdynamic, for-loop increment fix
- **v0.6.0** — SMAUG Phase A/B/C: C pointer system, macros, compound types, enum, static, typedef
- **v0.5.0** — Phase 4 prep: 38 embedded headers, dlsym fallback, struct alignment, sizeof, argc/argv

## Roadmap

| Phase | Goal | Status |
|-------|------|--------|
| **Phase 1** | Foundation: verbose flag, char literals, struct fix, register, doctest | **Complete** |
| **Phase 2** | User-defined structs/classes, namespaces, #include, using | **Complete** |
| **Phase 3** | php::/perl::/python::/ruby::/js:: namespaces, dlopen, MadArray | **Complete** |
| **Phase 3.5** | Modern language features: range-for, function pointers, lambdas, defer, STL containers | **Complete** |
| **Phase 3.5+** | switch, cin, class methods, regex, multi-return, ternary, namespace scoping | **Complete** |
| **Phase 4 prep** | C preprocessor, 39 embedded headers, struct alignment, sizeof, argc/argv | **Complete** |
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
