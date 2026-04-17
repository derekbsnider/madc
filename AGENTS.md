# Agent Instructions

This file is the entry point for any AI coding agent (Claude Code,
Cursor, Aider, Copilot Chat, Codex, etc.) working on this repository.
The same rules apply regardless of which agent is running.

## Before you start

Read `CLAUDE.md` in the repository root. It is the canonical project
briefing — build commands, architecture overview, asmjit setup, and
shell-command hygiene apply to every agent. Despite the filename, its
contents are not Claude-specific.

## Rules directory

Every rule lives in `.claude/rules/`. Each file is a bare list of
imperatives — no reasoning, no long explanations. When the "why"
matters, it's in a sibling file under `docs/rules/` with the same
base name.

**You must read the rules that apply to your task.** Skimming them is
not optional. Follow them unless the user explicitly overrides one.

### Cross-cutting (apply to every change)

| Rule                                             | Scope                                         |
|--------------------------------------------------|-----------------------------------------------|
| [design-principles.md](.claude/rules/design-principles.md) | Separation of concerns, high cohesion / low coupling, OOP, no hard-coding specifics into general machinery |
| [docs-vs-rules.md](.claude/rules/docs-vs-rules.md) | Rules vs docs separation — bare rules in `.claude/rules/`, reasoning in `docs/rules/` |
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
- Index the new rule in this AGENTS.md.

## When in doubt

- Trace what's broken first. Don't rapid-cycle fixes.
- Prefer small, self-contained commits that each fix one thing.
- Commit early. Never run `git checkout` on files with uncommitted
  work — use feature guards (`#ifdef FEATURE_NAME`) or `git stash`.
- Read related rule(s) before editing compiler internals.
