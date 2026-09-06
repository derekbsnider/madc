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

**madc** — "My Advanced Dialect of C" — is a C-like scripting
language. madc parses source into a `cir_node` tree — a
c2mir-friendly C11 AST that serves as madc's IR — which c2mir then
compiles to MIR for execution. (The original asmjit x86-64 JIT and the
Gecko parser experiment were both removed; CIR → c2mir → MIR is now the
sole backend.)

The "Mad" in Mad-C: mix functions from multiple programming languages
(PHP, Perl, Python, Ruby, JavaScript) in a single program via
namespaces.

**North-star vision & invariants — read `docs/plans/madc-vision-and-invariants.md`.**
The long-term arc is madc as a *polyglot transpiler* (read source language/standard X,
emit target Y, through the one `cir_node`/MC11-IR — chosen because most languages are
themselves implemented in C/C++, so a faithful C/C++ AST is their common substrate).
That doc holds the **invariants I1–I8** and a "does this block the vision?" checklist;
every language change must satisfy them (no hardcoded standards/targets; every feature
gated via the `--std=`/`LanguageStd` enum + the keyword/feature registry; one IR, one
emitter; no special-casing). `docs/plans/cpp-support.md` is the compliance roadmap that
serves this vision.

Long-term goal: compile a realistic C89 codebase end-to-end. The
concrete test case is SMAUG 1.8 (~158k LOC). The actual port lives
in a separate repository, **[MadSMAUG](https://github.com/derekbsnider/MadSMAUG)**,
because the Diku / Merc / SMAUG license stack is distinct from
madc's own MPL 2.0 licence. `docs/SMAUG_requirements.md` in this
repo is the historical gap analysis that drove madc's Phase A–F.

## Top 10 Rules — internalize these before writing a single line

These are the rules that cause the most damage when violated. Every
agent must follow them without exception.

**They are GATED — assertion is not enough, you must show the work.**
Every commit touching `src/` or `include/` carries four trailers, and
`scripts/check-rule-trailers.sh` (in `fulltest`) fails without them:

```
Hypothesis: <what you believed was wrong, written BEFORE editing>      (#3)
Layer:      <the layer chain, and why the one you edited is deepest>   (#2,#5)
Searched:   <the grep you ran, the CONCEPT, and what came back>        (#4)
Oracle:     <what gcc/clang did on a reducer, and what madc did>       (#1)
```

`n/a — <reason>` is allowed; silence is not. If you cannot write
`Layer:` — the chain, and why yours is the deepest — **you are shimming;
stop and go lower.** A true principle is not a substitute for the
deepest layer. See `.claude/rules/rule-trailers.md`.

1. **GCC is canon.** Before fixing any codegen or runtime bug, run
   `gcc -S -fverbose-asm -O0` and study the output. madc must match
   GCC's behavior. No exceptions. (`gcc-parity.md`, `gcc-methodology.md`)

2. **Fix at the deepest layer.** Never shim a symptom at a higher
   layer when the root cause is lower. If a type is wrong, fix where
   the type originates. Shortcuts make for long delays.
   (`gcc-methodology.md`)

3. **Think twice, code once.** Form a hypothesis before editing.
   If it's wrong, stop and re-examine — don't chain speculative
   micro-fixes. One reasoned pass beats five iterative attempts.
   (`gcc-methodology.md`)

4. **Understand what exists before assuming it doesn't.** Read the
   relevant code before writing new code. If you think a feature,
   helper, or handling path is missing — search first. The codebase
   is large; something that looks absent is often already there under
   a different name or in a different file. Reinventing existing
   machinery creates duplication and divergence. **State the search and
   its result before introducing any new named helper** — the grep you
   ran, the *concept* you searched for, and what came back. A search
   leaves no trace when skipped, which is why it fails silently.
   Standing instances: balanced-delimiter scanning is `DelimDepth`
   (`delimiter-tracking.md`); path canonicalization for comparison is
   `canonical_path_for_compare()`; a library's platform spelling (the `lib`
   prefix, `.so` / `.dylib` / `.dll`, the real runtime image names) is
   `madc_module_library_spelling()` in `src/madc_modules.cpp` (gated by
   `check-one-library-spelling.sh`).
   (`pre-edit-checklist.md`, `design-principles.md`)

5. **Do not cross layer boundaries.** Parsers parse, compilers emit
   code, namespace files implement their own namespace. A fix that
   touches the wrong layer is a future bug. (`design-principles.md`)

6. **Targeted tests per change; `make -C src fulltest` per MERGE WAVE.**
   Incremental commits run the new/affected tests; the full battery runs
   ONCE at the release/merge gate (or for genuinely suite-wide blast
   radius). Never re-run suites on already-green content. No
   JIT-green-EXE-broken, no EXE-green-JIT-broken. (`testing-fulltest.md`)

7. **No hard-coding specifics into general machinery.** Test runner:
   no per-test case branches. Parser: no string comparisons against
   user names. Use data lookups, type predicates, filename conventions.
   (`design-principles.md`)

8. **Commit early; never clobber uncommitted work.** Feature branches
   off `develop`. Keep `develop` stable. Never `git checkout` over
   uncommitted work — use `#ifdef` guards or `git stash`.
   (`branching.md`, `feature-guards.md`)

9. **Bare rules in `.claude/rules/`, reasoning in `docs/rules/`.**
   Never duplicate content between the two. If a rules file needs to
   say "because", move that to the docs file. (`docs-vs-rules.md`)

10. **Pre-edit checklist: trace, search, identify.** Before modifying
    any source file: trace the data flow for every variable the fix
    touches, search the file for existing handling of the same pattern,
    and identify where modified values are written back. If any check
    reveals an unknown, read more code before editing.
    (`pre-edit-checklist.md`)

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

When the task is clearly limited to core `madc` / `libmadc` / parser /
compiler work and does not touch `madcdat` or shared storage-facing
surfaces, prefer `./configure --enable-madcdat=no` first to shrink the
rebuild/test footprint. Re-enable `madcdat` before final validation when
working on storage/federation code or any shared surface that may affect it.

Source lives in `src/`, headers in `include/`, output in `bin/` and
`obj/`.

Build requires `clang++` (or `g++`) with C++11 support. **MIR (libmir +
c2mir) lives IN this repository at `third_party/mir`** — a subtree import
of the former fork, maintained as ordinary madc source (edit + commit it
like any file; no pin, no lockstep, no separate release). It is NOT stock
upstream MIR: it carries native C99 `_Complex`,
`__attribute__((cleanup))`, the scope-depth auto-local layout fix, the
struct/union statement-expression copy-out fix, ≤16-byte SIMD/vector
(`vector_size`/`ext_vector_type`) support, the Mach-O executable writer,
and the SysV-varargs / `_Complex` / `_Alignas` ABI fixes the CIR backend
depends on. `make -C src` builds libmir + c2m itself, into
`obj/mir/<variant>` (never inside the subtree). `vnmakarov/mir` is the
true upstream; `github.com/derekbsnider/mir` is the historical former
downstream and the transport for upstream PR branches only (see
`.claude/rules/build.md` and
`docs/plans/mir-into-madc-repo-2026-08-11.md`).

## Architecture

| Component | File                       | Role                                                          |
|-----------|----------------------------|---------------------------------------------------------------|
| Lexer       | `src/lexer.cpp`          | Tokenizes `.mad` source; handles `#include`, `#load`           |
| Parser      | `src/parser.cpp`         | Builds AST; struct/class/namespace resolution                  |
| CIR builder | `src/cir_builder.cpp`    | Lowers the AST to a `cir_node` tree (c2mir-friendly C11 AST), the IR fed to c2mir → MIR |
| php::     | `src/ns_php.cpp`           | 36 PHP-style string + array functions                          |
| perl::    | `src/ns_perl.cpp`          | 20 Perl-style functions (chop, grep, glob, split)              |
| python::  | `src/ns_python.cpp`        | 15 Python-style functions (title, center, zfill, format)       |
| ruby::    | `src/ns_ruby.cpp`          | 12 Ruby-style functions (squeeze, tr, chars, rotate)           |
| js::      | `src/ns_js.cpp`            | 6 JS-style functions (base64, URL encoding, JSON)              |
| rust::    | `src/ns_rust.cpp`          | 18 Rust-style string + array helpers (plus `rust::match`)      |
| madc::    | `src/ns_madc.cpp`          | Runtime eval API (`eval_*`, contexts) + the `madc::sys` object |
| Headers   | `include/madc.h`, `include/tokens.h`, `include/datadef.h`, `include/datatokens.h` | Core data structures |

Execution flow: `madc.cpp` → lexer → parser → CIR builder (`cir_node`)
→ c2mir → MIR execute.

## Backend note

asmjit (the original x86-64 JIT) was removed. CIR → c2mir → MIR is the
sole backend; the MIR library (libmir + c2mir) is in-tree at
`third_party/mir`.

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

A test with a sibling `tests/<name>.helper` fixture (e.g.
`include_helper.mad`, project-mode TU sources) is a compilation unit of
another test — runners skip it; skip it when running by hand too.

## Duplication audit

`/dupaudit` (`.claude/commands/dupaudit.md`) is a **recon** pass for *semantic*
duplication — N sites implementing one rule where at least one differs. That
divergence is the bug; the redundancy is only the cost. It is not a clone
detector: the case that hurts here shares no text (six angle-bracket scanners,
one guarded, a sixth unguarded copy written two days *after* the fix landed in
the first).

Run it **before merging a feature branch, scoped to the subsystem the feature
touched** — that is where new copies are born. Findings are recorded as
`DupFamily` nodes in `madc-knowledge` so later sweeps re-check instead of
rediscovering, and every family that gets consolidated leaves a gate in
`fulltest` so it cannot regrow. Non-Claude tools: read the command file and
follow its steps directly.

## Session hand-off

Cross-agent work in this repo uses
[`docs/agent-handoff.md`](docs/agent-handoff.md) as the hand-off
playbook.

- `claude_status.json` is the canonical current snapshot.
- `madc-knowledge` is the authoritative project-memory source.
- `claude_status.json`, `docs/plans/ROADMAP.md`, and `CHANGELOG.md` are mirrored
  repo surfaces that must stay in sync with it.
- `docs/test-status.md` is the detailed test baseline.
- Live git state and actual build/test results remain operational truth
  for branch, working tree, and validation.

Read and update those artifacts per `docs/agent-handoff.md` when
passing work between agents.

## Key design notes

- **`std::string` is a real class** (g++ canon): string literals are
  `const char*` (`ddCHARptr`); `string` ingests them via its real
  constructors/operators, resolved mangled-direct against libstdc++
  (no wrapper shims — those were deleted).
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
- **Ternary operator** `cond ? a : b` lowers to a C11 conditional in the
  `cir_node` tree; c2mir/MIR own the branch/merge codegen.

## Rules

Every rule lives in `.claude/rules/` as a bare list of imperatives —
no reasoning, no long explanations. The "why" for each rule is in a
sibling file under `docs/rules/` with the same base name.

**You must read the rules that apply to your task.** Skimming them is
not optional. Follow them unless the user explicitly overrides one.
When two rules could both apply, the lower-numbered priority tier
(P1 → P4) wins.

Claude Code auto-loads every file under `.claude/rules/` each turn.
Other tools need the index below. The line counts are a guard against
rule bloat — run `scripts/rule_stats.sh` and update the "Total rule
footprint" line below if any rule grows or shrinks noticeably.

### P1 — Safety and process (never violate)

Highest priority. Violating these can destroy work, break the git
history, or spam agent-permission prompts. Apply them unconditionally.

| Rule                                             | Lines | Scope                                          |
|--------------------------------------------------|------:|------------------------------------------------|
| [branching.md](.claude/rules/branching.md)       |    37 | Feature branches off `develop`, agent-owned `-claude` / `-codex` WIP branches, stable `develop`; lane-freshness push gate; OWNER LAW: every platform lane's FULL suite green before master (`check --release`) |
| [feature-guards.md](.claude/rules/feature-guards.md) |   9 | `#ifdef FEATURE_NAME` for in-progress code; never `git checkout` over uncommitted work |
| [docs-vs-rules.md](.claude/rules/docs-vs-rules.md) |   20 | Bare rules in `.claude/rules/`, reasoning in `docs/rules/` — never duplicate content |
| [session-handoff.md](.claude/rules/session-handoff.md) |   19 | KG-first hand-off flow, hypothesis-first execution, concise hand-off note |
| [knowledge-graph.md](.claude/rules/knowledge-graph.md) |   14 | KG as authoritative project memory, mirrored back into repo files |
| [scratch-files.md](.claude/rules/scratch-files.md) |     8 | All scratch / temp / reducer files go in `tmp/` (gitignored) — never in `tests/` or repo root |
| [rule-trailers.md](.claude/rules/rule-trailers.md) |    28 | **Show the Top 5 work, don't assert it.** Every `src/`/`include/` commit carries `Hypothesis:` / `Layer:` / `Searched:` / `Oracle:`; gated by `check-rule-trailers.sh`. Can't write `Layer:`? You're shimming |

Shell-command hygiene (single commands, no `&&` chains) is a P1 rule
too; it's stated in the "Shell command hygiene" section of this file.

### P2 — Cross-cutting design (apply to every change)

These shape every edit. If a change doesn't honour them, it's wrong
no matter how small.

| Rule                                             | Lines | Scope                                          |
|--------------------------------------------------|------:|------------------------------------------------|
| [design-principles.md](.claude/rules/design-principles.md) |  59 | Separation of concerns, high cohesion / low coupling, OOP, no hard-coding specifics into general machinery |
| [pre-edit-checklist.md](.claude/rules/pre-edit-checklist.md) |  19 | Trace data flow, search for existing handling, identify write-back target — before every edit (Top 10 Rule #10) |
| [cpp-first-api.md](.claude/rules/cpp-first-api.md) |  24 | Design embedding and `libmadc` APIs as C++ first; keep C shims thin and late; extern-C exports are the C-host API only — script-facing namespace publics resolve mangled-direct, with two named exceptions (CIR-emitted machinery, compiler-implemented publics like `php::print_r`) |
| [helper-methods.md](.claude/rules/helper-methods.md) |  12 | Extract ad-hoc checks into named helpers      |
| [fix-what-you-find.md](.claude/rules/fix-what-you-find.md) |   21 | A defect you DISCOVER is yours to fix — "pre-existing" is not a disposition; silent wrong answers jump the queue; the fix ships a reducer |
| [no-parallel-implementations.md](.claude/rules/no-parallel-implementations.md) | 22 | One implementation per concern; A/B scaffolding expires; tests use production entry points; cap every test run |
| [parse-once.md](.claude/rules/parse-once.md)     |    24 | New C++ support resolves on the parse-once generic spine (g++ tsubst model), NEVER via re-parse; re-parse is a transitional fallback slated for deletion at suite-wide burndown=0; every change moves the `[why:]` fallback count down or flat |
| [code-style.md](.claude/rules/code-style.md)     |     6 | C++11, tabs, header guards, DBG                |
| [value-first.md](.claude/rules/value-first.md)   |    30 | madc-dialect code: ZERO includes/`using`/`std::` (bare print/println/format; auto-include reaches user modules); var/value over std::string; missing capability = fix the CARRIER/compiler, never spell around it |
| [dialect-lean.md](.claude/rules/dialect-lean.md) |    35 | OWNER LAW: the `--std=madc` surface (prelude fragments included) never depends on C++ system header parsing or std::string; interop conveniences behind the stdlib guards; polyglot publics need lean PRIMARY forms; gated by `check-dialect-lean.sh` |
| [enum-over-strings.md](.claude/rules/enum-over-strings.md) | 15 | Enums (not chars/strings) for type/category discriminators; convert C-string node names to enums at the boundary |
| [thread-safety.md](.claude/rules/thread-safety.md) | 22 | OWNER LAW: every language addition STATES its thread-safety contract (C++ stdlib convention default); shared mutation routes through the hub/verbs; no new bare mutable globals |

### P3 — Build, test, and validation (gate "done")

Must be satisfied before a change is considered complete. A change
that fails any of these is not merged.

| Rule                                             | Lines | Scope                                          |
|--------------------------------------------------|------:|------------------------------------------------|
| [build.md](.claude/rules/build.md)               |    15 | `make -C src`, the in-tree MIR subtree model   |
| [testing-fulltest.md](.claude/rules/testing-fulltest.md) | 11 | Targeted tests per change; `make -C src fulltest` once per merge wave |
| [testing.md](.claude/rules/testing.md)           |    32 | Integration + unit test conventions            |
| [test-fixtures.md](.claude/rules/test-fixtures.md) |  16 | Per-test `.input` / `.argv` / `.expect` files; runner stays generic |

### P4 — Compiler internals (apply when touching that area)

Area-specific rules. Read the ones relevant to the code you're
editing — don't try to memorize all of them.

| Rule                                             | Lines | Scope                                          |
|--------------------------------------------------|------:|------------------------------------------------|
| [mc11-ir.md](.claude/rules/mc11-ir.md)           |    30 | **SET IN STONE.** `cir_node` = MC11-IR: derives from c2mir `node_t` (c2mir sees lowered) AND carries originating tokens + parse tree + file/line/col (madc sees high-level). It is BOTH; render targets share the `--std=` enum |
| [delimiter-tracking.md](.claude/rules/delimiter-tracking.md) | 33 | **ONE tracker for `(` `[` `{` `<`.** Never hand-roll `++paren_depth` / `--angle_depth` / `>>`-splitting — use `DelimDepth` + `delim_scan_step()` / `delimStepStream()`. Whether a `<` opens is a NAME question ([temp.names]/3) answered by the Program's lookup predicates; every stream scan carries the Program handle. Gated by `check-one-delim-tracker.sh` |
| [backend-strategy.md](.claude/rules/backend-strategy.md) | 30 | **Forward trajectory (ADR 0001).** c2mir/C-AST IR is the sole backend; direct-MIR is a scalpel for runtime internals + REPL/debug tier; `--emit=c11` is first-class; CIR coverage parity gates promotion to master |
| [lowering-vs-raising.md](.claude/rules/lowering-vs-raising.md) | 39 | Where a missing feature gets fixed: Tier 1 lower/resolve in madc (default) · Tier 2 raise c2mir for semantic primitives · Tier 3 raise MIR for floor gaps (SIMD). Verify c2mir's real surface — stmt-exprs/_Generic/_Complex are supported |
| [gcc-methodology.md](.claude/rules/gcc-methodology.md) | 44 | Compare with `gcc -S -fverbose-asm` first, fix at deepest layer, operator self-determination |
| [clang-methodology.md](.claude/rules/clang-methodology.md) | 46 | Same methodology against clang, the co-equal canon; cross-check lowering with both gcc and clang |
| [debug.md](.claude/rules/debug.md)               |    18 | `DBG(x)` macro usage and rules                 |
| [c11-transpiler.md](.claude/rules/c11-transpiler.md) | 70 | `--emit=c11` lowers every C++ feature to strict C11; c2mir limits, lowering patterns, emission hygiene |
| [embedded-headers.md](.claude/rules/embedded-headers.md) |  67 | `include/madc/` headers, lazy registration, `#load`, real return types (signed `int` libc fns) |
| [gcc-parity.md](.claude/rules/gcc-parity.md)     |    15 | GCC as a reference baseline (verbose `-fverbose-asm` disassembly) for codegen / type / runtime parity |
| [clang-parity.md](.claude/rules/clang-parity.md) |    16 | clang as the co-equal reference baseline (second lowering opinion); both gcc and clang are canon |

### Total rule footprint

- **34 rules, 993 lines** in `.claude/rules/` (per `scripts/rule_stats.sh`).
- **This file (AGENTS.md): ~414 lines** — loaded by Claude via
  `@AGENTS.md` in `CLAUDE.md`, read directly by Codex / Gemini / etc.
- **Grand total loaded by Claude Code per turn: ~1415 lines.**

Rule bloat ages: if any tier exceeds a few hundred lines, split the
heaviest rule into a narrower sub-rule or move more content into the
sibling `docs/rules/` reasoning file. Refresh these counts by running
`scripts/rule_stats.sh` and updating this section.

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
| `MadSMAUG/`       | Symlink (gitignored) to the external [MadSMAUG](https://github.com/derekbsnider/MadSMAUG) repo — the SMAUG 1.8 port. Kept separate due to the Diku / Merc / SMAUG license stack. |
| `scripts/`        | `run_tests.sh`, `gen_embedded_headers.sh`, `psed.sh`  |

## When a rule needs to change

- Edit both the rule file (`.claude/rules/<name>.md`) and the
  reasoning file (`docs/rules/<name>.md`) in the same commit.
- If you're adding a new rule, create both files. Start with the bare
  rule; move any "because" / "why" / "historically" sentences into
  the docs file.
- Index the new rule in this file's "Rules" section.

## When in doubt

- A bug you find is a bug you own. There is no other maintainer — see
  `fix-what-you-find.md`. Do not stop at proving it predates your change.
- Trace what's broken first. Don't rapid-cycle fixes — when a bug is
  hard, stop and form a hypothesis before editing.
- Prefer small, self-contained commits that each fix one thing.
- Commit early. Never run `git checkout` on files with uncommitted
  work — use feature guards (`#ifdef FEATURE_NAME`) or `git stash`.
- Read the related rule(s) before editing compiler internals.
- Run the targeted tests per change and `make -C src fulltest` once per
  merge wave; a green merge-wave battery is part of "done."

## Help / feedback

- Issues: https://github.com/derekbsnider/madc/issues
- Contributions welcome — see `docs/` for language reference and
  architecture details before opening a PR.
