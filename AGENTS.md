# Agent Instructions — Mad-C (madc)

This is the canonical briefing for any AI coding agent working on this
repository. It is agent-agnostic — the same content drives every tool.

## Tool setup

Every supported tool reads the same content, via whichever file it
looks for by convention:

| Tool                             | File read                         | Mechanism                                       |
|----------------------------------|-----------------------------------|-------------------------------------------------|
| **OpenAI Codex CLI** (`codex`)   | `AGENTS.md`                       | Native — no setup needed                        |
| **Claude Code**                  | `CLAUDE.md`                       | `@AGENTS.md` import (inlined automatically)     |
| **Gemini CLI** (`gemini`)        | `GEMINI.md`                       | `@AGENTS.md` import + fallback prose pointer    |
| **GitHub Copilot** (VS Code / web) | `.github/copilot-instructions.md` | Short pointer to `AGENTS.md`                    |
| **Aider**                        | `AGENTS.md` via `.aider.conf.yml` | `read: [AGENTS.md]` (shipped in this repo)      |
| **Cursor**                       | `.cursor/rules/project.mdc`       | `alwaysApply: true`, points to `AGENTS.md`      |
| **Windsurf / Codeium**           | `.windsurfrules`                  | Prose pointer to `AGENTS.md`                    |

`AGENTS.md` (this file) is the **single source of truth**. The others
are short stubs that import or reference it. A new tool that prefers
its own filename just needs another stub — never duplicate the content.

Also: Claude Code automatically loads every file under `.claude/rules/`
as project instructions. Non-Claude tools do not, which is why this
file indexes them explicitly in the "Rules" section below.

## Project summary

**madc** — "My jit-Assembled Dialect of C" — is a C-like scripting
language that JIT-compiles directly to x86-64 machine code using the
asmjit library. Programs are compiled and executed in-process with no
intermediate bytecode.

The "Mad" in Mad-C: mix functions from multiple programming languages
(PHP, Perl, Python, Ruby, JavaScript) in a single program via
namespaces.

Long-term goal: run the SMAUG 1.8 MUD codebase (~158k LOC, C89)
without gcc. See `docs/SMAUG_requirements.md` for the gap analysis.

## Shell command hygiene

**Never chain commands with `&&` or use shell variable substitution.**
Each shell invocation (Bash tool call, etc.) must be a single, simple
command. This prevents repeated permission prompts when running under
agents that prompt per-command.

## Build system

```bash
make -C src          # build bin/madc
make -C src clean    # remove all object files
make -C src test     # build and run doctest unit tests
make -C src fulltest # build + unit tests + all integration tests
```

Source lives in `src/`, headers in `include/`, output in `bin/` and
`obj/`.

Build requires `g++` with C++11 support and asmjit v1.14 installed at
`/usr/local/` (see "asmjit version notes" below).

## Architecture

| Component | File                       | Role                                                          |
|-----------|----------------------------|---------------------------------------------------------------|
| Lexer     | `src/lexer.cpp`            | Tokenizes `.mad` source; handles `#include`, `#load`           |
| Parser    | `src/parser.cpp`           | Builds AST; struct/class/namespace resolution                  |
| Compiler  | `src/compiler.cpp`         | Walks AST, emits x86 via asmjit; stream I/O, dlcall            |
| Typesafe  | `src/typesafe.cpp`         | Type-safe register helpers (Gp/Xmm moves, arithmetic)          |
| php::     | `src/ns_php.cpp`           | 36 PHP-style string + array functions                          |
| perl::    | `src/ns_perl.cpp`          | 21 Perl-style functions (chop, grep, glob, split)              |
| python::  | `src/ns_python.cpp`        | 16 Python-style functions (title, center, zfill, format)       |
| ruby::    | `src/ns_ruby.cpp`          | 12 Ruby-style functions (squeeze, tr, chars, rotate)           |
| js::      | `src/ns_js.cpp`            | 6 JS-style functions (base64, URL encoding, JSON)              |
| STL       | `src/ns_stl.cpp`           | STL container helpers: `vector<T>`, `map<K,V>`, `set<T>`, `list<T>` |
| Headers   | `include/madc.h`, `include/tokens.h`, `include/datadef.h`, `include/datatokens.h` | Core data structures |

Execution flow: `madc.cpp` → lexer → parser → compiler → JIT execute.

## asmjit version notes

The project uses the **manually installed** asmjit v1.14 at
`/usr/local/` (NOT the apt-installed package at
`/lib/x86_64-linux-gnu/`). The Makefile explicitly passes
`-L/usr/local/lib -Wl,-rpath,/usr/local/lib` to link the right one.
Headers are at `/usr/local/include/asmjit/`.

Key v1.14 migration points (full list in `docs/rules/asmjit-api.md`):

- `BaseReg::kTypeGp*` → `RegType::kGp*`
- `BaseReg::kGroupVec` / `kGroupGp` → `RegGroup::kVec` / `kGp`
- `ConstPool::kScopeLocal` → `ConstPoolScope::kLocal`
- `CallConv::kIdHost` → `CallConvId::kCDecl`
- `cc.call(target, sig)` → `cc.invoke(&node, target, sig)`
- `Imm::i64()` → `Imm::value()`
- `Operand::isEqual()` → `Operand::equals()`
- `FormatOptions::kFlag*` → `FormatFlags::k*`

## Testing

```bash
bin/madc tests/testint.mad        # run a single integration test
make -C src test                  # run unit tests (doctest)
make -C src fulltest              # unit + all integration tests
```

Integration tests live in `tests/*.mad`. Unit tests in `tests/unit/`.
Per-test stdin / argv / expected-output fixtures live next to each
`.mad` file via the filename convention in
`.claude/rules/test-fixtures.md`. Current pass counts live in
`docs/test-status.md` and `README.md` — never hard-coded in these
instructions.

Skip `tests/include_helper.mad` when running by hand; it is included
by `testinclude.mad`, not a standalone test.

## Key design notes

- **Stream methods** use type-specific wrappers (`ifstream_good`,
  `ofstream_good`) because `std::ios` is a virtual base class —
  casting `void*` to `ios*` gives the wrong pointer offset.
- **String parameters** are pass-by-reference. `voperand()` creates a
  bare Gp register for `vfPARAM` non-numeric vars; `cleanup()` skips
  their destruction.
- **`dtSTRING → dtCHARptr` coercion** happens automatically in
  `TokenCallFunc::compile()` via `string_cstr()` when a function
  expects `const char*`.
- **MadValue / MadArray** — tagged union + container for PHP-style
  mixed-type arrays. Used internally by php:: array functions.
- **dlopen functions** use variadic calling: 0 declared params,
  actual args passed based on compile-time types. String args
  auto-coerce to `const char*`.
- **Class methods** receive a hidden `__this` pointer as their first
  parameter. `this.member` compiles as an offset from that pointer.
- **Multi-return functions** use a hidden `__retbuf` parameter — a
  pointer to caller-allocated stack memory where return values are
  written.
- **Ternary operator** uses a stack-slot merge: both branches of
  `cond ? a : b` write to the same stack location, avoiding phi
  nodes. See `docs/rules/ternary.md` for why.

## Rules

Every rule lives in `.claude/rules/` as a bare list of imperatives —
no reasoning, no long explanations. The "why" for each rule is in a
sibling file under `docs/rules/` with the same base name.

**You must read the rules that apply to your task.** Skimming them is
not optional. Follow them unless the user explicitly overrides one.

### Cross-cutting (apply to every change)

| Rule                                             | Scope                                         |
|--------------------------------------------------|-----------------------------------------------|
| [design-principles.md](.claude/rules/design-principles.md) | Separation of concerns, high cohesion / low coupling, OOP, no hard-coding specifics into general machinery |
| [docs-vs-rules.md](.claude/rules/docs-vs-rules.md) | Bare rules in `.claude/rules/`, reasoning in `docs/rules/` |
| [code-style.md](.claude/rules/code-style.md)     | C++11, tabs, header guards, naming            |
| [helper-methods.md](.claude/rules/helper-methods.md) | Extract ad-hoc checks into named helpers  |
| [feature-guards.md](.claude/rules/feature-guards.md) | `#ifdef FEATURE_NAME` for in-progress code |

### Build, branching, testing

| Rule                                             | Scope                                         |
|--------------------------------------------------|-----------------------------------------------|
| [build.md](.claude/rules/build.md)               | `make -C src`, asmjit v1.14 at `/usr/local/` |
| [branching.md](.claude/rules/branching.md)       | Feature branches off `develop`, `develop` → `master` for releases |
| [testing.md](.claude/rules/testing.md)           | Integration + unit test conventions           |
| [testing-fulltest.md](.claude/rules/testing-fulltest.md) | `make -C src fulltest` after every change |
| [test-fixtures.md](.claude/rules/test-fixtures.md) | Per-test `.input` / `.argv` / `.expect` files; runner stays generic |

### Compiler internals

| Rule                                             | Scope                                         |
|--------------------------------------------------|-----------------------------------------------|
| [asmjit-api.md](.claude/rules/asmjit-api.md)     | asmjit v1.14 API dos / don'ts                 |
| [debug.md](.claude/rules/debug.md)               | `DBG(x)` macro usage and rules                |
| [regdp-reset.md](.claude/rules/regdp-reset.md)   | Reset `regdp` before sub-compiles in loops / conditionals |
| [struct-compiler.md](.claude/rules/struct-compiler.md) | `addOffset` vs `setOffset`, string-member lifecycle |
| [class-methods.md](.claude/rules/class-methods.md) | Name mangling, `__this`, unqualified member access |
| [multi-return.md](.claude/rules/multi-return.md) | `__retbuf` injection, multi-return call sites |
| [ternary.md](.claude/rules/ternary.md)           | Ternary parsing + stack-slot merge pattern    |
| [embedded-headers.md](.claude/rules/embedded-headers.md) | `include/madc/` headers, lazy registration, `#load` |

## Directory layout

| Path              | Purpose                                               |
|-------------------|-------------------------------------------------------|
| `src/`            | Lexer, parser, compiler, namespace implementations     |
| `include/`        | Headers — `madc.h`, `tokens.h`, `datadef.h`, etc.     |
| `include/madc/`   | Embedded standard / POSIX headers (baked into binary) |
| `tests/`          | `.mad` integration tests + `.input` / `.argv` / `.expect` fixtures |
| `tests/unit/`     | doctest C++ unit tests                                |
| `.claude/rules/`  | Bare rules (this index's subjects)                    |
| `docs/rules/`     | Reasoning behind each rule                            |
| `docs/`           | User / language documentation                         |
| `MadSMAUG/`       | SMAUG 1.8 port target                                 |
| `scripts/`        | `run_tests.sh`, `gen_embedded_headers.sh`, `psed.sh`  |

## When a rule needs to change

- Edit both the rule file (`.claude/rules/<name>.md`) and the
  reasoning file (`docs/rules/<name>.md`) in the same commit.
- If you're adding a new rule, create both files. Start with the bare
  rule; move any "because" / "why" / "historically" sentences into
  the docs file.
- Index the new rule in this file's "Rules" section.

## When in doubt

- Trace what's broken first. Don't rapid-cycle fixes — when a bug is
  hard, stop and form a hypothesis before editing.
- Prefer small, self-contained commits that each fix one thing.
- Commit early. Never run `git checkout` on files with uncommitted
  work — use feature guards (`#ifdef FEATURE_NAME`) or `git stash`.
- Read the related rule(s) before editing compiler internals.
- Run `make -C src fulltest` after each change; a green test run is
  part of "done."

## Help / feedback

- Issues: https://github.com/derekbsnider/madc/issues
- Contributions welcome — see `docs/` for language reference and
  architecture details before opening a PR.
