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

madc builds multi-file projects from a standard **`compile_commands.json`**
compilation database (the format CMake emits with
`-DCMAKE_EXPORT_COMPILE_COMMANDS=ON`, or `bear -- make` generates from any
Makefile build): each translation unit compiles separately, the modules are
linked in-process, and the entry point runs.

```bash
bin/madc --project compile_commands.json          # compile all TUs, link, run
bin/madc compile_commands.json                    # .json implies --project
bin/madc --project compile_commands.json -lcrypt  # resolve libs at link time
bin/madc --project compile_commands.json -o app   # single native ELF from all TUs
```

This is how the flagship test case runs: SMAUG 1.8 (~158k lines of C89,
51 translation units) boots both as a multi-TU JIT run and as a single
~5 MB native executable.

For small projects, `#include` composition still works — one top-level file
that `#include`s the rest, with `int main()` last, run as a single
translation unit. Filenames resolve relative to the including file, nested
includes are supported, and repeated includes are skipped within the same
compile.

---

## Language Features

- **Data types:** `int8_t`–`int64_t`, `uint8_t`–`uint64_t`, `float`, `double`, `char`, `std::string`, `array`
- **Typed containers:** `vector<T>`, `map<K,V>`, `set<T>`, `list<T>` — real template instantiations (also as `std::vector<T>` etc.)
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
- **Classes:** the full C++ class model — see **C++ support** below
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

### C++ support

madc is a C/C++ dialect: C++ lowers Cfront-style to strict C11 on the
one `cir_node` IR (`--emit=c11` is a first-class output), and madc
parses the **real glibc/libstdc++ headers** — `#include <vector>`,
`<string>`, `<iostream>` read the actual installed headers, not
embedded shims. The current model (all suite-tested):

- **Classes:** constructors/destructors with RAII (destructor injection
  at every scope exit, return, and unwind path), const methods, static
  members, access control, `friend`, conversion operators, and operator
  overloading — including `<=>` three-way comparison with the standard
  synthesis of the relational operators
- **Inheritance:** single, multiple, and virtual inheritance on the
  Itanium model (vtables, vtable thunks, virtual bases constructed
  once), virtual and pure-virtual methods, virtual destructors, RTTI —
  `typeid` and `dynamic_cast`, including cross-casts in MI hierarchies
- **Templates:** class, function, member, and variable templates via
  real monomorphization on the parse-once spine (the g++ tsubst model:
  the pattern parses once, instantiations substitute over the saved
  tree — never re-parsed); `std::move`/`std::forward` instantiate from
  the real headers
- **`std::string` and streams are real libstdc++ objects**, called
  mangled-direct through Itanium symbols — no wrapper layer
- **Exceptions:** `try`/`catch`/`throw` lowered to setjmp/longjmp with
  type-dispatched catch; destructors run during unwinding
- **Lambdas** with `[&]` capture; references (including rvalue
  references in template instantiation)
- **Specifiers:** `constexpr` (folded; `if constexpr` discards the dead
  branch), `consteval`/`constinit` (accepted), `inline` with real C++
  vague linkage — identical per-TU copies (template instantiations,
  inline/in-class bodies, vtables, typeinfo, C++17 inline variables)
  emit `STB_WEAK` and merge at native or external links, with inline
  variables' dynamic inits once-guarded — plus `inline namespace`,
  `alignas`, `noexcept`, `auto` return deduction
- Version-gated via `--std=`: each keyword activates at its introducing
  standard through the language-standard registry

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
  (branch **`develop`**, pinned at the commit in [`MIR_COMMIT`](MIR_COMMIT) —
  that file is the single source of truth), built at `/workspace/mir`. madc
  links `libmir.a` + c2mir from there. This is **not** upstream MIR: the fork
  carries native C99 `_Complex`, `__attribute__((cleanup))`, ≤16-byte
  SIMD/vector support, direct ELF emission + DWARF + PIC, and the ABI/codegen
  fixes the CIR backend depends on. The fork's `develop` tracks madc's
  `develop` and its `master` tracks madc's `master`; fork releases are tagged
  `v<upstream-base>-madc.<madc-version>` (the release madc depends on is named
  in [`MIR_VERSION`](MIR_VERSION)).

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

**Current status (v0.51.0): 756 integration tests pass (0 failing, 0 timed out, 9 skipped) through the live binary, the packed release binary in BOTH carrier shapes (embedded and sidecar), and the thin CLI of the shared shape (library-carried forest); the native `--exe` lane is at 740/0. With `-static-libmadc` an emitted binary carries the madc runtime it needs and runs with no madc library installed. gcc.c-torture stands at 1614/1685 with zero standard-C failures — every remaining failure is a classified GNU-extension roadmap item ([`docs/parity/failset-classification.md`](docs/parity/failset-classification.md)). SMAUG 1.8 boots, runs as a live server, and is playable — both as a multi-TU JIT run and as a single native ELF. `cir_node → c2mir → MIR → JIT` is the sole backend (built against the [madc MIR fork](https://github.com/derekbsnider/mir)). (`make -C src fulltest`)**

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

**v0.51.0** — **forest carriers S6** (`madc.ini`): the final carriers
slice, and with it the track is complete. madc optionally reads one
`madc.ini`, and settings now resolve **CLI > environment > madc.ini >
baked defaults** — the rule the discovery chain had reserved its last
arm for. Keys: `std` (default dialect), `forest` (a frozen-forest
container, discovery **arm 5**), `include` (repeatable, searched after
every `-I`), and `cpu-limit` / `mem-limit`. Lookup is `./madc.ini` →
`$XDG_CONFIG_HOME/madc/madc.ini` → `<sysconfdir>/madc.ini`, and the
**first existing file wins outright** — configs are never merged, because
a merged chain makes "why is this setting on?" unanswerable. Relative
paths resolve against the config file's own directory. The parser is
**strict**: an unknown key, a foreign section, or a non-numeric limit is
a hard error naming file:line and the accepted keys — `mem-limit = 8G`
says so instead of quietly arming an 8 MB guard. New `--config=<file>`
(that file is the whole search, and it must load) and `--no-config`.
The reader itself is **schema-blind substrate**: it owns the format while
each consumer registers its own keys, the same split
`madcdis/snapshot.h` makes as a content-blind container — so
`config_file("madcdat")` reads `madcdat.ini` with madcdat's keys and its
own diagnostics, and no second parser is ever needed. Reading a config
file stays a `madc(1)` feature: **libmadc never consults one**, since a
file that can redirect where the compiler loads frozen state from is an
attack surface for a sandboxed host, and `configure
--disable-config-file` removes madc's lookup entirely. Also fixed: the
installed `madcdis/snapshot.h` did not compile downstream (`madc_pch.h`,
which its public signatures need, was never installed), and
`docs/build.md` still documented **asmjit** — the JIT removed several
releases ago — as a build requirement. Suites: fulltest **756/0/0/9**
with the new 39-check `forest_config_gate`, `--exe` **740/0**, packed
arbiter **756/0/0/9**. The MIR fork is unchanged (`1.0-madc.0.47.0`
remains the pinned release).

**Branch state:** `develop` is at v0.51.0; `master` is at v0.48.0
(promoted 2026-07-25 — the Mach-O milestone promote, per the owner's
ride-with-S3 decision). The
[MIR fork](https://github.com/derekbsnider/mir)'s `master` tracks
madc's `master` in lockstep (fast-forwarded to the same
`1.0-madc.0.47.0` pin); fork releases pair with madc's (see
[`MIR_VERSION`](MIR_VERSION)).

### Recent Releases

- **v0.51.0** — forest-carriers S6 (`madc.ini`), completing the carriers track: optional config file with the precedence rule CLI > environment > madc.ini > baked defaults; keys `std` / `forest` (discovery arm 5) / `include` (repeatable) / `cpu-limit` / `mem-limit`; lookup `./madc.ini` → `$XDG_CONFIG_HOME/madc/madc.ini` → `<sysconfdir>/madc.ini` with the first existing file winning outright (never merged); relative paths resolve against the config file's directory; STRICT parsing (unknown key / malformed line / non-numeric limit = hard error naming file:line and the accepted keys); new `--config=<file>` and `--no-config`; the reader is schema-blind substrate reusable by madcdat and madcdis-based tools (consumers register their own keys, same split `madcdis/snapshot.h` makes as a content-blind container); libmadc never reads a config file and `--disable-config-file` removes madc's lookup; suite + pack hermeticity via `--no-config`; also fixed the installed `madcdis/snapshot.h` not compiling downstream (`madc_pch.h` now installed) and rewrote `docs/build.md`, which still documented asmjit; `forest_config_gate` (39 checks / 18 legs) in fulltest; fulltest 756/0/0/9, `--exe` 740/0, packed arbiter 756/0/0/9; fork unchanged (1.0-madc.0.47.0)
- **v0.50.0** — forest-carriers S5 (`-static-libmadc` / AOT ledger): madc's C-lane runtime becomes dual-build C11 sources (`src/rt/`) compiled BOTH into libmadc by the host build and into MIR ledger modules by madc-via-c2mir at pack time, carried in a new optional forest-container segment read independently of the grove bind; `-static-libmadc` (alias `-static`) pulls the needed modules before the link so the emitted image runs with no madc library — the unlock that makes try/catch AOT possible on Mach-O; distinct build-side vs Tier-B refusals; two cover-analysis fixes (copy-relocated libc data; host-vs-target probing on cross builds); `forest_ledger_gate` (14 checks) in fulltest; fulltest 756/0/0/9, `--exe` 740/0, packed arbiter 756/0/0/9, product path emits a zero-libmadc binary that runs under an empty library path; fork unchanged (1.0-madc.0.47.0)
- **v0.49.0** — forest-carriers S4 (shared shape): forest-in-library discovery arm (`dladdr` → the libmadc image; `<lib>.forest` sidecar behind it; IMAGE arms never gated by `enable_external_forest`, so a sandboxed strict host still binds); `--enable-shared` thin-CLI configure axis with the release pack targeting `lib/libmadc.so` (133 KB CLI + 11.5 MB packed library, 240 units); forest knob family on the public embedding API (`enable_forest_bind` / `forest_missing` / `enable_external_forest` + `allow_external_forest` clamped under `system_locked`); `Program::forest_bind_enabled` folded into `RegistrationPolicy`; `forest_library_gate` in fulltest (9 legs incl. the `enable_external_forest=false` negative test S3 owed); thin-CLI parity 756/0/0/9 and `--enable-shared` product arbiter 756/0/0/9; fork unchanged (1.0-madc.0.47.0)
- **v0.48.0** — forest-carriers S3 (discovery): ordered carrier probe chain (self-image → `<exe>.forest` sidecar → `$MADC_FOREST`; S4/S6 slots reserved); `--with-forest=embedded|sidecar|none` configure axis (`forest_pack.sh --sidecar`, hosted darwin sidecar shape, `make install` sidecar); failure-policy knobs in the RegistrationPolicy family (silent/loud/strict + `enable_external_forest`); config-mismatch fall-through never a notice; `forest_sidecar_gate` in fulltest; Linux arbiter through both carriers 756/0/0/9, Mac 7/7 legs both arches; fork unchanged (1.0-madc.0.47.0)
- **v0.47.0** — forest-carriers S2 (emitted-pack): `--pack-forest=<container>` embeds a frozen container in emitted native executables — ELF trailer / Mach-O `__MADC,__forest` section laid by the fork writer inside the emit-time signature (fork seam `MIR_object_exec_params.extra_*`; no re-signer on the product path); host-neutral Mach-O file-probe read-back; full native loop (freeze → pack-emit → AMFI → read-back) green on Apple hardware both arches; `forest_emitpack_gate` in fulltest; fulltest 756/0/0/9 + packed 240 units, `--exe` 740/0; fork release 1.0-madc.0.47.0

## Roadmap

Canonical roadmap: [`docs/plans/ROADMAP.md`](docs/plans/ROADMAP.md). The phases
below chart the language build-out. They were originally achieved on the
**old asmjit/Gecko backend** (since removed) and have been **fully
re-established — and far surpassed — on the `cir_node` → c2mir → MIR
path**: the CIR parity gate was met with zero standard-C failures
outstanding, and the suite stands at the counts in the Testing section
above.

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
| **C++ Model** | Classes, inheritance, vtables, exceptions | **Complete and extended on CIR** — full Itanium class model (MI + virtual bases, RTTI/`dynamic_cast`, vtable thunks), templates via real monomorphization, `<=>`, SJLJ exceptions + unwinding, real libstdc++ headers, vague linkage (see *C++ support* above) |
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
