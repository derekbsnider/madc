# madc Project Build Driver — Design Spec

- **Date:** 2026-06-08
- **Status:** Design approved in conversation; awaiting written-spec review.
- **Branch (origin of work):** `feature/realhdr-parse-gaps2-claude` (Codex base
  `ecc92e4`). New work will branch off this per the branching rule.
- **Supersedes/relates:** the SMAUG umbrella build model
  (`MadSMAUG/src/SMAUG.mad` + `MadSMAUG.sh`); ADR 0001 (CIR/c2mir is the sole
  backend); the develop→master parity track (object emit is a parity item, not
  in this v1).

---

## 1. Problem

SMAUG is built today via a hand-authored **umbrella translation unit**,
`SMAUG.mad`, which `#include`s ~50 `.c` files plus headers into a *single* TU,
injects `main()`, and `#define`s real functions to no-ops
(`bug`/`ch_printf`/`pager_printf`). This "makeshift Makefile" has three
structural problems:

1. **Single-TU compilation is unfaithful.** Everything shares one namespace; a
   typedef/macro/`static` in one `.c` bleeds into the next. The real SMAUG
   `bug()` never compiles — it is stubbed out.
2. **Error locations are wrong.** Because the whole program is one umbrella TU,
   madc misattributes included-file line numbers to the umbrella (the observed
   bogus `SMAUG.mad:1261` for a construct that actually lives in an
   `upstream_src/*.c`). This makes diagnosing failures painful.
3. **It is a per-project shim.** Every real C/C++ codebase would need its own
   hand-written umbrella. That does not scale and is not the polyglot-transpiler
   direction.

The real SMAUG `Makefile` is plain separate compilation: each `.c` → `.o` via
`gcc -c` (with `-DSMAUG -DREGEX -DREQUESTS`), then `gcc -o smaug *.o`.

**Goal:** madc reads a project's real build description, compiles each
translation unit independently *in one madc process*, links the resulting MIR
modules, and JIT-executes the linked program — no umbrella, no per-file process,
no `.o` files on disk. Faithful compilation, real per-file error locations, and
a foundation that generalizes to any project ("other sorts of project files").

---

## 2. Goals / Non-goals

**v1 goals**
- A `ProjectManifest` abstraction: the list of TUs + their per-file flags + a
  minimal link description.
- A `compile_commands.json` reader that populates the manifest.
- A multi-TU engine: one process, fresh front-end state per TU, one MIR context,
  multiple modules, one link, JIT-run `main`.
- Real per-TU `file:line` error reporting.
- A small test fixture proving cross-TU calls + file-static isolation through
  madc (the spike, but end-to-end through the product path).

**v1 non-goals (explicitly deferred — see §9 Roadmap)**
- Writing real object code (`.o`) to disk — JIT run only in v1.
- A general GNU Makefile interpreter.
- A native `.madproj` format.
- Other-ecosystem readers (Cargo, package.json, etc.).
- Parallel/incremental rebuild, caching, dependency tracking.
- Auto-detecting the manifest in the cwd.

---

## 3. Feasibility (proven before design)

The engine floor was verified, not assumed:

- **c2mir + MIR multi-module link works.** A two-TU spike compiled+linked+ran via
  the fork's `c2m`: `c2m spike_a.c spike_b.c -eg` → `a_secret=100 from_b=300`,
  with two same-named file-scope `static int secret` correctly isolated and
  cross-TU calls resolving both directions. This is c2mir's native serial mode.
- **The c2m driver pattern** (`mir/c2mir/c2mir-driver.c:716–823`): one
  `MIR_init()`, one `c2mir_init()`, then a loop calling `c2mir_compile(ctx, …)`
  per source with an incrementing `module_num` (one module per TU into the shared
  context), then link + run. madc mirrors this with `c2mir_compile_tree` (a
  pre-built cir_node tree) instead of `c2mir_compile` (lexing from source).
- **madc's gap is wiring, not capability.** `madc_cir_execute`
  (`src/madc_cir.cpp:118–239`) already does the *single-TU* version of exactly
  this: one `Program` → `CirBuilder::translate_module` → one tree →
  `c2mir_compile_tree` → one module → `MIR_load_module` →
  `MIR_link(ctx, …, cir_import_resolver)` → find `main` → `MIR_gen` → run. The
  multi-TU engine runs the build-a-module body N times into the same context,
  loads all modules, then performs the **single** `MIR_link` already present.

**Conclusion:** no fork work is required for the engine. The work is madc-side
orchestration plus the manifest front end.

---

## 4. Architecture

```
 compile_commands.json ──▶ [ ManifestReader ] ──▶ ProjectManifest ──▶ [ Engine ] ──▶ JIT run
   (future: Makefile,                                  (TUs+flags+link)     │
    .madproj, …)                                                            └─(future sinks: .o)
```

Three replaceable edges around one core:
- **Front end (reader):** turns a foreign build description into a manifest.
- **Core (engine):** consumes the manifest, produces and links MIR modules.
- **Back end (sink):** what to do with the linked program (v1: JIT run).

---

## 5. Components

### 5.1 ProjectManifest (central data model)
A plain C++ struct, the single contract between readers and the engine.

```
struct ProjectTU {
    std::string file;                 // absolute or working_dir-relative
    std::string working_dir;          // for resolving relative #includes
    std::vector<std::string> defines; // e.g. {"SMAUG", "REGEX=1"}
    std::vector<std::string> include_dirs;
    std::string std_option;           // e.g. "c89"/"gnu89"; empty = default
};

struct ProjectManifest {
    std::vector<ProjectTU> tus;
    std::string entry;        // entry symbol, default "main"
    std::string output_name;  // informational in v1 (no .o written)
};
```

The manifest is dumb data. It models a build, not a build system.

### 5.2 Manifest readers (pluggable front ends)
- **v1: `compile_commands.json`.** Parse the JSON array; for each entry read
  `file`, `directory`, and `command`/`arguments`. Extract `-D`, `-I`, `-std=`
  into the `ProjectTU`. Ignore flags madc does not consume (warnings, `-O`,
  `-g`, `-c`, `-o`). A small, dependency-light JSON parse (reuse any existing
  madc JSON facility if present; otherwise a minimal local parser — confirm
  during implementation, do not add a third-party dep without cause).
- **Known gap — link description.** `compile_commands.json` has no link rule
  (which TUs form which executable, link flags). v1 convention: **all TUs in the
  manifest form one program; the entry is `main`.** A real link section arrives
  with the Makefile reader (§9).
- **Future readers** (§9) populate the same `ProjectManifest`: a simple-Makefile
  subset, a native `.madproj`, other ecosystems.

### 5.3 Multi-TU engine
A new entry point alongside `madc_cir_execute` (do not fork the single-file path
into divergence — factor shared steps so there is one module-building helper used
by both; honor no-parallel-implementations).

Flow:
1. `MIR_init()`; `c2mir_init()`; `MIR_gen_init()`; one `c2m_ctx` via `cir_init`.
2. **For each `ProjectTU`:**
   a. A **fresh `Program`** configured with this TU's defines/include-dirs/std.
   b. `Program::tokenize(file)` then `parse()` — independent front-end state.
   c. `CirBuilder builder(c2m); tree = builder.translate_module(prog);`
   d. Validity gate (`cir_report_errors`) — report with the TU's real
      `file:line` (tokens already carry file/line/col per MC11-IR; this is where
      the bogus-attribution class of bug is fixed).
   e. `c2mir_compile_tree(ctx, c2m, tree, "<file>")` → one module per TU
      (distinct module name, mirroring c2m's `module_num++`).
3. After all TUs: `MIR_load_module` each module; **one**
   `MIR_link(ctx, MIR_set_gen_interface, cir_import_resolver)`.
4. Locate `entry` (`main`) across modules; `MIR_gen`; run with argc/argv.
5. Teardown (mirror existing finish ordering).

**Open item to verify during implementation (the through-madc spike answers it):**
that `CirBuilder`/`c2m` hold no cross-TU state that breaks a *second*
`c2mir_compile_tree` in the same context. c2mir resets per TU by design (c2m
relies on it); madc's wrappers must not defeat that. If a fresh `c2m_ctx` per TU
is needed instead of one shared ctx, that is a small change and still links
because modules accumulate in the shared `MIR_context`.

### 5.4 Per-TU state isolation
A fresh `Program` per TU is the isolation mechanism — correct C TU semantics for
free. Macros, typedefs, and file-scope `static` cannot leak between TUs. Embedded
headers and lazy registration re-run per `Program`, exactly as a real build
re-includes headers per TU. SMAUG's real `bug()` compiles; the umbrella's no-op
`#define`s are gone.

### 5.5 Output sinks
- **v1: in-process JIT run** (the only sink). Same `MIR_gen` + call path as
  today's single-file execute.
- Future sinks in §9.

---

## 6. CLI surface

```
madc --project <compile_commands.json> [program-args…]
```

Reads the manifest, builds + links all TUs, JIT-runs `main` with the trailing
args. Explicit flag only in v1 (no cwd auto-detection). This replaces
`MadSMAUG.sh`'s `exec madc SMAUG.mad` once SMAUG has a manifest.

---

## 7. Data flow (one invocation)

```
--project file ─▶ read JSON ─▶ ProjectManifest{tus[], entry}
   for tu in tus:  Program(tu.flags).tokenize+parse ─▶ CirBuilder ─▶ tree
                   ─▶ c2mir_compile_tree(ctx, "<tu.file>")  [module_i]
   load module_0..n ─▶ MIR_link(ctx) ─▶ MIR_gen(main) ─▶ run(argc,argv)
```

---

## 8. Testing

- **Start small (mirror the spike *through madc*).** Fixture: 2–3 small TUs +
  a hand-written `compile_commands.json`; assert cross-TU calls resolve, two
  same-named file-scope `static`s stay isolated, and stdout matches. This is the
  first deliverable and the engine's proof.
- **Per the test-fixtures rule:** the project-mode test is discovered by
  convention; no per-test branches in the runner. Cap every run
  (`ulimit -t` + `timeout`).
- **Gates unchanged:** `make -C src fulltest` (534/4 baseline) and
  gcc.c-torture (1566/31/57/1) must not regress. New project-mode tests are
  additive.
- **SMAUG only after the small fixture is green.** Generate SMAUG's
  `compile_commands.json` (`bear -- make` against the upstream tree, or derive
  from the Makefile's `C_FILES`/`C_FLAGS`), then `madc --project` it. Real
  per-file errors will localize the remaining parse/codegen gaps far better than
  the umbrella did. Single runs, no loops; random port.

---

## 9. Roadmap (explicitly out of v1)

- **Link description:** a Makefile-subset reader (or `.madproj` link section)
  that captures which TUs form which target and the link flags.
- **More readers:** simple GNU Makefile subset; native `.madproj`; eventually
  other ecosystems (the polyglot angle).
- **Real object code to disk (`.o`)** — a capability the asmjit backend on
  *master* already had (real ELF emit); it regressed when asmjit was removed, so
  this is a **parity recovery item, not a new feature**, and must be rebuilt on
  the CIR/MIR path (no asmjit revival — backend-strategy / no-parallel-impl).
  - *Route A (no fork):* `--emit=c11` per TU → `cc -c` → ELF `.o` + native link.
  - *Route B (fork, later):* native MIR→ELF object emitter; the old asmjit
    object writer (in the `/workspace/madc-asmjit` worktree) is a *design
    reference*, not code to port.
  - The `ProjectManifest` already carries the link info both routes need, so
    designing it in now is free.
- **Performance/UX:** incremental rebuild + caching, parallel compile, manifest
  auto-detection.

---

## 10. Risks / open questions

1. **CirBuilder/c2m cross-TU state** (§5.3) — verified by the through-madc spike;
   fallback is fresh `c2m_ctx` per TU.
2. **Lazy registration / embedded-header globals** (e.g. `cout`, `stderr`) across
   multiple `Program`s in one process — confirm each fresh `Program` re-registers
   cleanly and the import resolver still resolves runtime symbols across modules.
3. **Duplicate-symbol diagnostics** — two non-static externs with the same name in
   different TUs should produce a clear link error, not a crash. Confirm
   `MIR_link` behavior and surface it.
4. **Entry selection** — exactly one `main` expected; diagnose zero/multiple.
5. **Flag fidelity** — which `-D`/`-I`/`-std` forms appear in real
   `compile_commands.json` `command` strings vs `arguments` arrays; handle both.

---

## 11. Why this fits the project's invariants

- **One IR, one backend** (ADR 0001 / backend-strategy): the engine is pure
  orchestration over the existing CIR→c2mir→MIR path; no new codegen, no revived
  asmjit.
- **MC11-IR** tokens carry file/line/col — the design *uses* that to fix error
  attribution rather than working around it.
- **Polyglot north star:** a manifest abstraction with pluggable readers is the
  generalizable shape for "read source project X" — SMAUG's `compile_commands.json`
  is just the first reader.
- **No hard-coding specifics:** the engine never special-cases SMAUG; it consumes
  a manifest. SMAUG's manifest is data.
```
