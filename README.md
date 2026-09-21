# madc — C and C++ without the ceremony

**My Advanced Dialect of C** is a self-contained native utility language for a
zero-toolchain workflow: run C/C++-style code like a script without giving up
access to native C/C++ code and libraries. Use it for shell-style automation,
embed it in another application, or emit object files and executables from the
same source.

A basic madc program needs no project scaffolding, no separate compiler
invocation, and—under the default madc dialect—no explicit `main()`.

[Usage](docs/usage.md) · [Build](docs/build.md) · [madcide](docs/madcide.md) ·
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
madc also provides `import name as ns;` (the C++20 `import` made whole —
interface and library, spelled for the target by the module map, so no
`.so` / `.dylib` / `.dll` appears in the source), `dlopen`, `dlsym`, and
`dlcall`:

```c
import c as libc;

libc::abs(-42);
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
- real `std::string`, streams, containers, references, lambdas **with capture
  lists**, `constexpr`, `noexcept`, and standard-gated language features
- **pointers to members** — types and values, `&C::m`, `.*`, `->*`, and calls
  through a bound member pointer
- **Itanium ABI compatibility**: every C++ symbol madc defines emits its real
  mangled name, so a madc object file links against g++- and clang-built code
  and free functions overload by parameter type; by-value class parameters and
  returns follow the Itanium calling convention
- range-based `for`, function pointers, `auto`, `:=`, `defer`, multiple return
  values, and `rust::match`

**madc does not grade its own homework.** Conformance is measured against
gcc's own testsuite and the third-party c-testsuite, and the numbers — with
their scope and caveats — are published in
[`docs/conformance-coverage.md`](docs/conformance-coverage.md): C at
**99.2%** of the in-scope gcc c-torture execute set under `--std=c17` and
**220/220** on c-testsuite; C++ at **80.0%** (C++98) and **75.1%** (C++11) of
the compile-clean `g++.dg` subset, with C++14/17/20 measured alongside.

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

## madcide and the Nexus — the IDE *is* the running compiler

`madcide` is not an editor that shells out to a compiler. The live parse in
memory **is** the compilation, so diagnostics, the outline and the emitted
views are projections of real compiler data rather than a second model that
can drift.

```bash
madcide file.mad                              # terminal (the default grid)
madcide file.mad --gui                        # native window: platform menu bar, file dialogs
madcide file.mad --line                       # ex / edlin line mode over stdin
madcide file.mad -c "check"                   # no UI at all — the exit status is the verdict
madcide file.mad --serve 127.0.0.1:7777       # a session other clients attach to
madcide file.mad --mcp                        # the MCP seat: the IR as a graph
madcide file.mad --lsp --serve 127.0.0.1:0    # the LSP face
madcide file.mad --attach                     # find the session holding this file
```

One composer and one client loop serve every face, so the terminal rendering
is byte-identical to the window's.

**A session, not a process.** A second window is a second *client* of the same
document, with its own caret and its own View; presence carets shift through
one anchor registry as anyone types. The editor region is a real split tree,
tool panes are chrome slots, and layouts persist beside the project manifest.
A View can re-represent the same document as source, MC11, C11 or C++ in
place — put source left and MC11 right and the carets track each other
through the emitter's correlation map.

**Every change is an event.** The change log is an append-only journal that
replays to any point, checkpoints, compacts, and surfaces as an `event:N`
history View.

**The Nexus — the IR as a graph agents edit.** Beyond the human faces,
madcide exposes the *live IR* as a node-addressed graph over MCP: an agent
queries structure, edits through validated verbs instead of text patches,
walks history with PAST verbs over git, and proposes changes under a
permission tier. LSP, a VS Code extension, attach and session discovery ride
the same session — one process, several faces.

See [`docs/madcide.md`](docs/madcide.md) and the design documents under
[`docs/plans/`](docs/plans/).

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

The current release is **v0.100.0**, the Nexus release. madcide stops being
a desktop application and becomes a **session**: a window is a *client*, so
several windows share one document with their own carets and presence, the
editor region is a real split tree whose layouts persist beside the manifest,
and a View re-represents the same document as source, MC11, C11 or C++ in
place — source left, MC11 right, carets tracking through the emitter's
correlation map. Every change is an event in an append-only journal that
replays, checkpoints and compacts. The session is reachable over an `api`
transport with permission tiers, a headless `--serve`, a `ws` window on the
same port, an MCP seat, an LSP face, a VS Code extension, attach and
discovery. Beyond the human faces sits **the Nexus**: the live IR as a
node-addressed graph an agent queries and edits through validated verbs
rather than text patches, with history verbs over git and a propose tier.
Underneath, the async I/O reactor gained a Windows backend and libgit2 became
the `madcgit` module rather than a vendored subtree.

The same release makes C++ conformance a **measured** number. Every C++
symbol madc defines now emits its real Itanium mangled name, so a madc object
file links against g++- and clang-built code; expression SFINAE, pointers to
members, real lambda captures, inheriting constructors and prvalue reference
binding landed behind it. The `g++.dg` ratchet lane was built in this window
and driven from **1169 (60%) to 1464 / 1950 (75.1%)**. That work then found
three standard-C regressions — including a compiler SIGSEGV — which are fixed,
returning gcc c-torture to 1611/1624 with zero regressions against v0.99.2.

Branch state: v0.100.0 is released on `develop`; the `master` promotion
follows the release-tier lane ledger (every platform lane's FULL suite green
on this content), with public binaries built by CI for Linux
(deb/rpm/tarball), Windows x86-64, and macOS (Apple Silicon + Intel), each
shipping the platform webview library beside the binaries.

Latest validated results — the full release tier, every lane green on one
commit (`7488a39cf`, 2026-09-21). Measured conformance against third-party
suites is published separately in
[`docs/conformance-coverage.md`](docs/conformance-coverage.md):

- Linux JIT: **1540 passed / 0 failed / 0 timed out / 9 skipped**; native EXE lane **1444/0**, OBJ lane
  **1444/0**; packed suite **1540/0/0/9**; headerless (no headers on
  disk anywhere) **1506/0/0/43**; doctest **186/186**
- the GUI stage under Xvfb (webview, the web editor, the madcide workbench,
  split with both clicks, menu bar, the dialogs, the panel, Run into the
  Terminal with no input, the resizable panel): **19/19 JIT, 19/19 EXE, 19/19 OBJ**
- Windows: packed Win64 under persistent Wine **1480/0/0TO/69skip**
  (`verify_pe_release` OK, 234 units); the FULL suite on genuine Windows 11
  **1480/0/0TO/69skip**
- **C conformance**: gcc `c-torture/execute` **1611/1624 in scope (99.2%)**
  under `--std=c17`, ratcheted against a baseline of pre-existing failures;
  c-testsuite **220/220, baseline empty** (`--std=gnu11`)
- **C++ conformance**: gcc's own `g++.dg` compile-clean subset —
  C++98 **308/385 (80.0%)**, C++11 **1464/1950 (75.1%)**, C++14 **206/417**,
  C++17 **128/308**, C++20 **346/653**
- macOS cross release on both architectures: 836 units, Mach-O release
  verifier (including the C-linkage authority) and the exe/dylib gate green;
  the FULL suite on GitHub's mac runners: arm64 **1520/0/0TO/29skip**,
  Intel **1521/0/0TO/28skip**
- the libc++ flavor suite (macOS's library on Linux hardware): **jit 1533/0/0TO/16skip, EXE/OBJ 1437/0**
- Colossal Cave Adventure parity: **3 fragments + 94 whole reference logs
  byte-identical** to the original C game (a permanent fulltest gate)
- **zero compiler warnings on every build lane**, enforced by `-Werror`

### Recent Releases

- [v0.100.0](docs/release-notes/v0.100.0.md) — **the Nexus release**:
  madcide becomes a multi-client session (a window is a client, presence,
  split-tree Views, an event-sourced change log, correlation maps) reachable
  over api / ws / MCP / LSP / VS Code / attach; the live IR becomes a graph
  agents edit by verbs; Itanium symbol mangling and the C++ feature work
  behind a conformance lane driven 60% → 75.1%; three standard-C regressions
  found and fixed, and suites tiered by time to run.
- [v0.99.2](docs/release-notes/v0.99.2.md) — the owner's hands-on round
  on the polished local IDE, the last polish before the master GUI release:
  output streams into the Terminal with no keystroke (the window's wait is
  the scheduler's wait), the Build menu's `^B` rows, a dialog's Close that
  closes and leaves nothing behind, resizable panel and sidebar.
- [v0.99.1](docs/release-notes/v0.99.1.md) — the owner's first round with
  the desktop application: the prompts as dialogs (quick input / confirm
  with buttons), a click picks the window, the status bar as chrome, and
  "(^C aborts)" true in every profile.
- [v0.99.0](docs/release-notes/v0.99.0.md) — madcide is a desktop
  application: the GUI chrome milestone (native menu bar from one
  command/menu data file, native file dialogs, status chrome, the JOE
  split as a window stack, mouse caret/selection, the incremental web
  editor, event enums) + five carrier fixes.
- [v0.98.0](docs/release-notes/v0.98.0.md) — the macOS full-suite
  release: the whole suite green on both mac runner arches after the
  darwin burndown (by-value class ABI, distinct wide char types,
  target-shaped long double / va_list, libc++ `<list>`); 128-bit SIMD
  in MIR on x86-64 + aarch64 with the vector ABI gated against the
  platform compiler; Apple stack-argument packing; `-w`; every
  platform lane's full suite gates master.
- [v0.97.0](docs/release-notes/v0.97.0.md) — the madcide interaction
  arc (gateway seam, modes palette, the vi modal personality as
  profile data) + the carrier elegance arc (keyed literals, `rows[] =`
  append, literal expressions, live-kind subscripts); c-testsuite
  220/220 COMPLETE.

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
- [madcide — the IDE](docs/madcide.md) (terminal and window; profiles, projects, Build/Run, the panel and the Terminal)
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
