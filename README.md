# madc — Mad-C Programming Language

**My Advanced Dialect of C** — a C-like scripting language built around the **MC11-IR**: a `cir_node` AST tree that *derives from* [c2mir](https://github.com/vnmakarov/mir)'s `node_t`, so it feeds c2mir → MIR and executes in-process — no bytecode, no separate compilation step. The same tree also carries its originating tokens, so it renders back to source (`.c` / `.mc11`) and forward to other targets from one representation.

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

Optional storage backends are now being wired for configure-time
feature detection. When Autotools is installed, the intended flow is:

```bash
autoreconf -fi
./configure --with-bdb --with-gdbm --with-qdbm --with-sqlite3
make
```

`--with-qdbm` is intended for the Villa API layer, and each backend is
optional rather than required for a core `madc` build.

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

- **Data types:** `int8_t`–`int64_t`, `uint8_t`–`uint64_t`, `float`, `double`, `char`, `std::string`, `array`
- **Typed containers:** `vector<int>`, `map<string, int>`, `set<string>` — also as `std::vector<int>` etc.
- **Streams:** `std::cout`, `std::cerr`, `std::cin`, `std::stringstream`, `std::ifstream`, `std::ofstream`, `std::fstream`
- **Control flow:** `if`/`else`, `for`, `while`, `do`/`while`, `switch`/`case`/`default`, `rust::match`
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
- **Namespaces:** `std::cout`, `madc::regex_match()`, `php::explode()`, `perl::grep()`, `python::title()`, `ruby::tr()`, `js::btoa()`, `rust::trim()`
- **Dialect precedence:** `prefer rust, php, c;` or `#pragma prefer rust, php, c`
- **Regex:** `madc::regex_match()`, `madc::regex_search()`, `madc::regex_replace()`
- **Input:** `std::cin >> name >> age;` reads from stdin
- **`#include`:** `#include "file.mad"` for source inclusion
- **`using`:** `using namespace std;` or `using std::cout;` imports std names into the unqualified surface
- **`#load`:** `#load "libfoo.so" as foo;` for dynamic library loading
- **`dlopen`/`dlsym`/`dlcall`:** first-class dynamic linking
- **File I/O:** `ifstream`/`ofstream` with `open`, `close`, `good`, `eof`, `getline`
- **Subscript operator:** `a[0]`, `nums[i]`, `ages["key"]`
- **Escape sequences:** `\n`, `\t`, `\r`, `\\`, `\"`, `\0`
- **C23 coverage (early wave):** `_Bool`, `0b...`, `_Static_assert` / `static_assert`, `alignof` / `_Alignof`, `typeof` / `typeof_unqual`, `nullptr`, digit separators (`1'000'000`)

### Multi-Language Namespaces

The signature feature of madc — use the best functions from each language:

```c
#!/usr/bin/env madc
#include <iostream>
#include <string>
using namespace std;

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
| [`rust::`](docs/language/ns-rust.md) | 18 | trim/contains/replace, split/join, first/last/get, push/pop |
| `std::` | 9 + types | cin, cout, cerr, endl, getline, string conversions, for_each, stream/string types |
| `madc::` | 4 | array, regex_match, regex_search, regex_replace |

Plus `#load` for any shared library via dlopen.

---

## Building

Requires:
- `clang++` (or `g++`) with C++11 support
- The **madc MIR fork** — [github.com/derekbsnider/mir](https://github.com/derekbsnider/mir)
  (branch **`develop`**, pinned at commit `4aa628b` — see [`MIR_COMMIT`](MIR_COMMIT)),
  built at `/workspace/mir`. madc links `libmir.a` + c2mir from there. This is
  **not** upstream MIR: the fork carries native C99 `_Complex`,
  `__attribute__((cleanup))`, and the ABI/codegen fixes the CIR backend depends
  on. The fork's `develop` tracks madc's `develop` (and `master`↔`master` once
  madc reaches parity).

```bash
# Build the MIR fork first (libmir + c2mir):
git clone -b develop https://github.com/derekbsnider/mir /workspace/mir
git -C /workspace/mir checkout "$(cat MIR_COMMIT)"   # pin to the verified commit
make -C /workspace/mir

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

**Current status (v0.34.0, develop): 695 integration tests pass (0 failing, 0 timed out, 16 skipped) through both the live and packed release binaries. Every forest gate is green, including bound-surface `[subbind]`; the current reducer corpus is 308 pack drops. The gcc.c-torture promote gate and formally skipped cases are tracked in [`docs/parity/failset-classification.md`](docs/parity/failset-classification.md). SMAUG 1.8 boots, runs as a live server, and is playable. `cir_node → c2mir → MIR → JIT` is the sole backend (built against the [madc MIR fork](https://github.com/derekbsnider/mir)). (`make -C src fulltest`)**

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
| [`docs/language/ns-rust.md`](docs/language/ns-rust.md) | rust:: namespace reference |
| [`docs/language/prefer.md`](docs/language/prefer.md) | Namespace precedence directive |
| [`docs/language/modern/`](docs/language/modern/) | Range-for, function pointers, lambdas, defer |
| [`docs/language/switch.md`](docs/language/switch.md) | Switch/case/default statement |
| [`docs/language/rust-match.md`](docs/language/rust-match.md) | `rust::match` (integer patterns, OR-arms, `_` wildcard) |
| [`docs/language/input-operator.md`](docs/language/input-operator.md) | cin >> input operator |
| [`docs/language/class-methods.md`](docs/language/class-methods.md) | Class methods with this pointer |
| [`docs/language/regex.md`](docs/language/regex.md) | Regex functions |
| [`docs/language/multiple-returns.md`](docs/language/multiple-returns.md) | Go-style multiple return values |
| [`docs/language/ternary-operator.md`](docs/language/ternary-operator.md) | Ternary operator |
| [`docs/build.md`](docs/build.md) | Build requirements + the madc MIR fork |
| [`docs/plans/data-storage-federation.md`](docs/plans/data-storage-federation.md) | Exploratory `madcdat` storage/federation design (`madc::DataSource` stays core, `DataSet<T>`, `Relation<A,B>`, automatic mapping, SQL/GQL front-ends, current `--enable-madcdat` build gate, future separate `libmadcdat` boundary) |
| [`docs/architecture.md`](docs/architecture.md) | Compiler internals |
| [`docs/testing.md`](docs/testing.md) | Test guide |
| [`docs/test-status.md`](docs/test-status.md) | Per-test results |
| [`docs/agent-handoff.md`](docs/agent-handoff.md) | Cross-agent hand-off workflow and source-of-truth rules |
| [`AGENTS.md`](AGENTS.md) | Agent briefing — project rules, architecture, multi-tool setup |
| [`docs/rules/`](docs/rules/) | Reasoning behind each rule in `.claude/rules/` |
| [`CHANGELOG.md`](CHANGELOG.md) | Change history |

---

## Current Release

**v0.34.0 (2026-07-11)** — the pack-time drain release: Phase 2 rung 1 of the
embedded-header-forest work. A `--freeze` parse drains every deferred header
body to fixpoint and freezes fully-evaluated state, guarded by a pack-side
c2mir check gate (fork API `c2mir_check_tree`, pinned `062dd97`) with
error-tolerant reverts — a parser gap in a body nobody calls can never poison
a pack. The packed binary runs the whole integration suite **681/681** and
beats the previous -O2 live-parse wall on the headline test. Dev and packed
binaries now coexist (`bin/madc` / `bin/madc-release`), and
`docs/perf/forest-timings.tsv` tracks the timing trend per release. See
[`docs/release-notes/v0.34.0.md`](docs/release-notes/v0.34.0.md).

CIR baseline (2026-07-14): **695 integration pass / 0 fail / 0 timeout /
16 skip** (packed suite 695/695; zero known reds, all forest gates GREEN) and **gcc.c-torture 1567
of 1652 in-scope (95.0%) at `-O1` and `-O2`** — the develop→master promote
gate is **all 41 remaining standard-C failures fixed (≥1608)**, per the
user-signed failset classification audit
([`docs/parity/failset-classification.md`](docs/parity/failset-classification.md)):
33 gcc-internal/torture-only tests are formally skipped
(`docs/parity/torture-skip-manifest.txt`) and 14 real-world GNU extensions
are roadmap items, not gate blockers. In-process `eval`/exec runs on the CIR
JIT (`CirJitSession`); the REPL and native AOT output remain deferred.

**Branch state:** `develop` carries v0.34.0 (CIR backend). `master` still holds
the v0.24.0 asmjit/Gecko backend at full C89 coverage (419 pass / 0 fail) —
develop is **not** promoted to master until the CIR path reaches feature parity.

### Current Release

**v0.34.0** is the pack-time drain release: Phase 2 rung 1 of the
embedded-header-forest work is closed. Deferred header bodies drain to
fixpoint at pack time and freeze fully evaluated, validated by a pack-side
c2mir check gate with error-tolerant reverts (drain failures revert to
on-use derivation; eager-parsed source functions revert to a body-span
carry — restoring packed `std::stoi`/`std::stod`). First fully-green
fulltest of the drain era (681/0/0/16 + every ratchet + bind gate 18/18 +
both oracles) and the first-ever **packed suite 681/681**. The build now
keeps `bin/madc` (dev -O0) and `bin/madc-release` (packed -O2) side by
side, with the timing trend tracked in `docs/perf/forest-timings.tsv`.

### Recent Releases

- **v0.34.0** — Pack-time deferred-body drain (rung 1): pack-side c2mir check gate (fork `c2mir_check_tree` @062dd97), error-tolerant reverts incl. body-span carry (packed stoi/stod restored), emission split + trap prebind; packed suite 681/681; dual dev/packed binaries; timing trend TSV; fulltest 681/0/0/16
- **v0.33.0** — Parse-once campaign complete: seven copy-time KINDs (incl. the SET wall), first-skip flipped to production, re-parse machinery deleted (−123 lines); burndown 312/0; map-iteration for-init SIGSEGV fixed (+`testmapiter`); typed dtor externs make emit-C gcc-clean on containers; fulltest 677/0/0/16
- **v0.32.0** — Rung-1 interning capstone: `TokenIdent::str` dropped (4-byte interned spelling_id; −43% token string ctors, −3.3% instructions); `Variable::name_sid` + finalize caches (−6.6% instructions); tsubst burndown root-caused to the dependent-member-type KIND
- **v0.31.0** — Tag-arithmetic encoding retired (structural derivation only); `madc::dis` substrate primitives + `datatype_map` re-key; `-O2` default front-end speedup; c2mir warnings 97 → 0; lambda `[&]` capture; fulltest 673/0/0/16
- **v0.30.0** — Set wall cleared: real-libstdc++ `std::set`/`std::map` (incl. `std::map<std::string,std::string>`) compile and run on the default C++17 path (eight container bugs fixed); real 16-byte `__int128`; embedded-header-forest plan; fulltest 669/0/0/18, torture failset byte-identical to baseline

## Roadmap

Canonical roadmap: [`docs/plans/ROADMAP.md`](docs/plans/ROADMAP.md). The phases
below were achieved on the **old asmjit/Gecko backend** (now removed); the
current focus is re-establishing them on the `cir_node` → c2mir → MIR path
(integration baseline 227/193/56).

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
| **SMAUG F** | Language gaps surfaced by porting SMAUG 1.8. Port itself lives in [MadSMAUG](https://github.com/derekbsnider/MadSMAUG) | **Complete** (v0.13.0 — playable end-to-end) |
| **GCC Parity** | GCC torture test suite compatibility | **v0.20.0** — 1536/1685 (91.2%); std namespace cleanup, std::vector |
| **C++ Model** | Classes, inheritance, vtables, exceptions | **v0.21.0** — ctors/dtors, operators, refs, new/delete, inheritance, vtables, SJLJ exceptions + unwinding |
| **Phase 4** | `libmadc.so` embedding API | **In progress** — §4.1 state split + structured diagnostics + engine-owned IO + full logging stack landed; §4.2 now ships `madc::value` and `madc::error` at `include/libmadc/` |

---

## Architecture

```
Source (.mad / .mc11 / C / C++)
    |
    v src/lexer.cpp       — tokenize source (#include, #load handled here)
    |
    v src/parser.cpp      — build the token parse tree (file/line/col retained)
    |
    v src/cir_builder.cpp — build the MC11-IR: a cir_node AST tree derived from
    |                       c2mir's node_t, each node carrying its originating
    |                       tokens + parse subtree
    |
    v c2mir               — consume the lowered C11 view of the MC11-IR
    |
    v MIR                 — execute in-process (native object/executable later)
```

### MC11-IR — the one intermediate representation (SET IN STONE)

The primary in-memory representation is the **Mad-enhanced-C11 IR (MC11-IR)** —
the `cir_node` AST tree. It is **both** lowered and high-level by construction:

- `cir_node` **derives from c2mir's `node_t`**, so c2mir consumes the lowered
  C11 view of the tree directly (classes already struct + functions, etc.).
- Every `cir_node` also **carries its originating lexed tokens and parse
  subtree**, with `file`/`line`/`column`, so madc retains the original
  high-level structure — not reconstructed from comments, but kept.

So one tree serves c2mir (lowered) and madc (high-level) at once. The `.mc11`
text form (C11 + `madc`-namespaced pragmas) is the on-disk serialization of the
extra info; render targets (C11, MC11, C++, madc) share the `--std=` language
enum and pick which view of the one IR to emit. See
[`docs/rules/mc11-ir.md`](docs/rules/mc11-ir.md).

Namespace implementations: `src/ns_php.cpp`, `src/ns_perl.cpp`, `src/ns_python.cpp`, `src/ns_ruby.cpp`, `src/ns_js.cpp`, `src/ns_stl.cpp`
