# madc — C and C++ without the ceremony

**My Advanced Dialect of C** is a self-contained native utility language for a
zero-toolchain workflow: run C/C++-style code like a script without giving up
access to native C/C++ code and libraries. Use it for shell-style automation,
embed it in another application, or emit object files and executables from the
same source.

A basic madc program needs no project scaffolding, no separate compiler
invocation, and—under the default madc dialect—no explicit `main()`.

[Usage](docs/usage.md) · [Build](docs/build.md) ·
[Architecture](docs/architecture.md) · [Test status](docs/test-status.md) ·
[Changelog](CHANGELOG.md) · [Contributing](AGENTS.md)

## Why madc?

- **C/C++ as a scripting language.** Write top-level statements, add a shebang,
  and run the file directly. madc synthesizes `main()` when you do not provide
  one.
- **One tool from script to binary.** JIT-run a one-off utility, compile a
  multi-file project, or emit a native executable without moving to another
  language.
- **Direct native interoperability.** Call the C and C++ runtime directly.
  Add other installed libraries with familiar `-l` options, or load them
  dynamically when runtime selection is useful.
- **A built-in utility toolkit.** Mix familiar functions from PHP, Perl,
  Python, Ruby, JavaScript, and Rust through explicit namespaces.
- **Embeddable.** `libmadc` can compile files or strings, execute code, evaluate
  expressions, call madc functions, register native callbacks, and control
  access through host policies and allowlists.

Once installed, a basic script needs only the `madc` command—no external C/C++
compiler, build system, bytecode VM, or separate language runtime. Core utility
functions need no boilerplate headers; real system headers and native libraries
remain available whenever a program needs them.

## Quick start

After [building madc](docs/build.md), create `hello.mad`:

```c
#!/usr/bin/env madc

puts("Hello from madc");
```

Run it immediately:

```bash
bin/madc hello.mad
```

Or make it an executable script (with `madc` installed on your `PATH`):

```bash
chmod +x hello.mad
./hello.mad
```

The same source can become a native executable:

```bash
bin/madc -o hello hello.mad
./hello
```

Top-level statements are a madc dialect feature. Explicit `--std=c*` and
`--std=c++*` modes retain their standard language rules.

## Native access without bindings

The C runtime and active C++ standard library are already part of the madc
process. madc automatically supplies declarations for common library
functions, so there is no reason to load libc explicitly:

```c
puts("Hello from libc");
```

For another installed library, use the familiar linker-style `-l` option:

```bash
bin/madc -lfoo tool.mad             # load libfoo and run
bin/madc -lfoo -o tool tool.mad     # emit an executable linked to libfoo
```

For JIT execution, `-lfoo` loads the named shared library globally so its
symbols resolve normally. For native output, it becomes a library dependency
of the emitted program. The source can include the library's normal header and
call its API directly.

For cases that genuinely require runtime loading or an isolated namespace,
madc also provides `#load`, `dlopen`, `dlsym`, and `dlcall`:

```c
#load "libfoo.so" as foo;

foo::some_function();
```

This makes madc useful for native API exploration, systems utilities,
automation, and small tools without generated bindings or a separate wrapper.
When headers are included, madc parses the **real installed headers**. Its
`std::string` and stream objects are real standard-library objects, not
replacement shims. Both stdlib flavors are first-class: the full test suite
passes under libstdc++ and, via `-stdlib=libc++`, under libc++.

## From one file to a native project

Single-file scripts are the smallest madc program, but they are not a separate
execution model. The same compiler pipeline supports:

- JIT execution in the current process
- source inclusion with `#include`
- multi-translation-unit projects
- native object and executable output
- external library resolution at link time
- C11 source emission with `--emit=c11`

Multi-file projects use the standard `compile_commands.json` format produced by
CMake or tools such as `bear`:

```bash
bin/madc --project compile_commands.json          # compile, link, and run
bin/madc --project compile_commands.json -lfoo    # add an installed library
bin/madc --project compile_commands.json -o app   # emit one native executable
```

The flagship compatibility case is **SMAUG 1.8**: approximately 158,000 lines
of C89 across 51 translation units, running both as an in-process JIT program
and as a native executable.

## C and C++ language support

madc is designed for substantial source compatibility with real-world C and
C++, while adding optional utility-language features in its own dialect.
Current coverage includes:

- C integer and floating-point types, structs, pointers, functions, control
  flow, preprocessor inclusion, and early C23 features
- C++ classes, constructors and destructors, RAII, access control, inheritance,
  virtual dispatch, RTTI, exceptions, operator overloading, and templates
- real `std::string`, streams, containers, references, lambdas, `constexpr`,
  `noexcept`, and standard-gated language features
- range-based `for`, function pointers, `auto`, `:=`, `defer`, multiple return
  values, and `rust::match`

See the [usage guide](docs/usage.md) for the language surface and
[architecture guide](docs/architecture.md) for lowering, ABI, and compiler
implementation details.

## Multi-language utility namespaces

The “Mad” in Mad-C also reflects its ability to combine familiar utility
functions from several language ecosystems in one program.

| Namespace | Focus |
|---|---|
| [`php::`](docs/language/ns-php.md) | String and array utilities, including split, join, and sort |
| [`perl::`](docs/language/ns-perl.md) | Regex-oriented grep, glob, split, join, chop, and chomp |
| [`python::`](docs/language/ns-python.md) | String formatting, alignment, title case, and zero fill |
| [`ruby::`](docs/language/ns-ruby.md) | Transliteration, squeeze, character, rotation, and compact operations |
| [`js::`](docs/language/ns-js.md) | Base64, URL encoding, integer parsing, and JSON utilities |
| [`rust::`](docs/language/ns-rust.md) | String, collection, option-like, and `match` utilities |
| [`ui::`](docs/language/ns-ui.md) | The data-hub surface: worlds, projections, verbs, and `ui::prompt` |
| `std::` | C++ strings, streams, containers, conversions, and algorithms |
| `madc::` | Native madc types and services, including regular expressions |

Namespace precedence can be selected explicitly with `prefer` or
`#pragma prefer`; see [namespace precedence](docs/language/prefer.md).

## Embedding with libmadc

The public C API in [`include/madc_api.h`](include/madc_api.h) exposes a
reusable engine and program model. A host application can:

- compile or execute a file or source string
- evaluate a translation unit, function body, or expression
- invoke compiled functions and inspect or update globals
- register native host functions
- save or load object files and emit executables
- configure namespaces, process access, dynamic linking, execution limits,
  diagnostics, and allowlists

Install the embedding library and public headers with:

```bash
sudo make -C src install-libmadc
```

See the [libmadc unit tests](tests/unit/test_libmadc_program.cpp) for working
API examples.

## Architecture

madc uses one source-preserving compiler representation:

```text
source → cir_node / MC11-IR → c2mir → MIR → JIT or native output
```

The `cir_node` tree derives from c2mir's `node_t` and retains its originating
tokens. The same representation can execute in-process, render C11, and feed
object or executable generation. There is no bytecode interpreter or parallel
second compiler backend.

See [docs/architecture.md](docs/architecture.md) for deeper
implementation details; the MIR/c2mir backend library is maintained
in-tree at `third_party/mir`.

## Current Release

The current release is **v0.97.0** — the madcide interaction arc and
the carrier elegance arc. madcide gains its session/client seam
(`class IdeSession` + a TUI client over the facts/keytable protocol —
the first slice of the IDE-as-API-gateway architecture), a ^N modes
palette, and a full **vi modal personality** as pure profile data
(`profiles/neovim.keys`: @normal/@insert scopes, counts,
operator+motion grammar, the `:` command line, showmode/showcmd). The
dialect completes its literal story: keyed literals
(`var m = { "a": 1 }`), PHP's append accessor (`rows[] = expr`), brace
literals as expressions (`rows[] = { "act": "colon" };`,
`rows.push({...})`), and subscripts that dispatch on a var index's
live kind (`m[kn]` keys by string, indexes by number) — closing a
silent address-as-index defect and deleting over a hundred lines of
row-building ceremony from madcide itself. c-testsuite conformance is
COMPLETE at 220/220 (`--std=gnu11`), and two new parser walls turned
formerly-silent wrong answers (the associative-literal hang class,
operand juxtaposition) into loud refusals.

Branch state: v0.97.0 is released on `develop`; v0.95.2 remains the
`master` promotion, with public binaries published for Linux
(deb/rpm), Windows x86-64, and macOS (Apple Silicon + Intel).

Latest validated results (the v0.97.0 merge-wave battery):

- Linux JIT: **1247 passed / 0 failed / 0 timed out / 9 skipped**
- native EXE lane **1193/0**, OBJ lane **1193/0**; packed suite **1247/0/0/9**
- Colossal Cave Adventure parity: **3 fragments + 94 whole reference logs
  byte-identical** to the original C game (a permanent fulltest gate)
- c-testsuite conformance: **220/220, baseline empty** (C mode,
  `--std=gnu11`)
- all three pack lanes green under the degradation gate: Linux and Win64 at
  93 tolerated pack parse errors with zero load-side losses, macOS at 64 per
  arch (six entries raised with a stated reason: the new juxtaposition wall
  made a pre-existing instantiation-body misparse LOUD — the fix is banked),
  and every listed header verified present as a container unit
- headerless (no headers on disk anywhere): Linux **1220/0/0/36** —
  the only lane that can see an artifact fail to serve a standard
  header from its own frozen corpus
- macOS on real Apple-Silicon hardware: **8 passed / 3 failed** — exact
  leg-for-leg parity with the standing known-opens (groves `os.str()`
  husk, the value intrinsic, the exec:// channel), not regressions;
  both arches packed at 835 units with the Mach-O release verifier
  green and the degradation leg at zero admitted-record losses
- packed Win64 under persistent Wine **1197/0/0/59** on the freshly
  rebuilt PE; on genuine Windows 11 (v0.92.1 run) all seven battery
  legs pass, including compiling a C translation unit on a host with
  no toolchain installed
- **zero compiler warnings on every build lane**, enforced by `-Werror`

### Recent Releases

- [v0.97.0](docs/release-notes/v0.97.0.md) — the madcide interaction
  arc (gateway seam, modes palette, the vi modal personality as
  profile data) + the carrier elegance arc (keyed literals, `rows[] =`
  append, literal expressions, live-kind subscripts); c-testsuite
  220/220 COMPLETE.
- [v0.96.0](docs/release-notes/v0.96.0.md) — the variadic-class arc:
  `bin/madc examples/embed_hello.cpp` compiles and RUNS; libmadc
  embedding fixes; two forest-artifact fixes (ranked-ctor symbol
  stamp, husk canonical-path re-include).
- [v0.95.2](docs/release-notes/v0.95.2.md) — the v0.95 download tag:
  darwin clang-lane conformance (override sweep) + the six assets.
- [v0.95.1](docs/release-notes/v0.95.1.md) — win64 `--project`
  first-call crash fixed (lazy-gen gated to eager on win64).
- [v0.95.0](docs/release-notes/v0.95.0.md) — the `ui::` namespace +
  Colossal Cave Adventure fully playable (94/94 logs byte-identical);
  cold startup 829 ms → ~150 ms class; the zero-include contract.

Older release notes live in [docs/release-notes/](docs/release-notes/).

## Building from source

Everything madc needs from source lives in this repository — MIR (the
JIT/codegen library, madc's maintained downstream of
[vnmakarov/mir](https://github.com/vnmakarov/mir)) is included at
`third_party/mir` and builds automatically. One clone is enough:

```bash
git clone https://github.com/derekbsnider/madc.git
cd madc

autoreconf -fi
./configure
make -C src
```

Useful targets:

```bash
make -C src release     # optimized, stripped, packed CLI
make -C src fulltest    # unit, integration, and release gates
sudo make -C src install
```

See [docs/build.md](docs/build.md) for dependencies, build modes, optional
features, installation paths, and release packaging.

## Documentation

- [Usage and CLI](docs/usage.md)
- [Build and installation](docs/build.md)
- [Compiler architecture](docs/architecture.md)
- [Testing guide](docs/testing.md)
- [Current test status](docs/test-status.md)
- [Modern language features](docs/language/modern/)
- [Regular expressions](docs/language/regex.md)
- [Multiple return values](docs/language/multiple-returns.md)
- [Data storage and federation plan](docs/plans/data-storage-federation.md)
- [Change history](CHANGELOG.md)

## Contributing

Start with [`AGENTS.md`](AGENTS.md). It is the canonical project briefing for
human contributors and coding agents. Repository rules live in
[`.claude/rules/`](.claude/rules/), with supporting reasoning in
[`docs/rules/`](docs/rules/) and cross-agent handoff guidance in
[`docs/agent-handoff.md`](docs/agent-handoff.md).

## License

madc is licensed under the [Mozilla Public License 2.0](LICENSE).
