# ADR 0001 — The CIR / c2mir backend (a C-AST IR feeding MIR), not a direct-MIR retarget

- **Status:** Accepted
- **Date:** 2026-05-30
- **Supersedes:** the informal "stay on emit-c until parity" stance
  (KG `Decision{cir-default-backend-stays-emit-c-until-parity}`)
- **Context version:** v0.25.0 (develop)

## Context

madc's backend has gone through three shapes:

1. **asmjit (master, ≤ v0.24.0).** A complete typed-register code generator —
   madc's own type system, C ABI, struct/varargs lowering — with asmjit as the
   final x86-64 emit layer. It worked: ~419 integration pass, ~97.9% GCC-torture
   parity, and SMAUG 1.8 reaching startup → login → serpent combat. It was
   x86-only, roughly `-O0`-quality, and entirely hand-maintained.
2. **A Gecko parser + MIR transpiler experiment.** Removed.
3. **CIR / c2mir (develop, v0.25.0).** madc's parser builds a `cir_node` tree
   that **derives from c2mir's `node_t`** (the MC11-IR, "set in stone" — see
   `docs/rules/mc11-ir.md`); c2mir performs sema + C-ABI lowering + MIR
   generation; MIR JITs (and can interpret). asmjit and Gecko are removed —
   this is the **sole** backend. SMAUG 1.8 now **boots, runs as a live server,
   and is playable** on this path.

The move from asmjit to MIR was never in question (MIR brings an optimizer,
multiple architectures, and an interpreter that asmjit cannot). What was never
written down — and kept getting re-litigated — is the **choice of front-to-MIR
strategy**:

- **Path A — retarget master:** keep madc's existing typed codegen and ABI
  logic; change only the emit layer from asmjit instructions to **MIR via the
  API**. (Effectively: resurrect master's middle/back, target MIR.)
- **Path B — current develop:** lower everything to a **C AST (c2mir
  `node_t`)** and let **c2mir** do sema + ABI + MIR codegen.

Note both paths share madc's parser as the front end; the real fork is *whose
middle-end and back-end run* — madc's (A) or c2mir's (B).

The decision turns on **what madc is**. madc is **a C-based dialect** — a
superset-ish of C with C++ support and conventions borrowed from other
languages (php/perl/python/ruby/js namespaces). Crucially, **all of that lives
inside the C/C++ semantic universe**: C++ is implemented by lowering to C
(Cfront), and the borrowed conventions are *themselves* C/C++ (they ship as
`ns_*.cpp` libraries; multi-syntax is skin-deep — lexer/parser only, AST/IR
unchanged). And the backend itself — MIR + c2mir — **is written in C.** It is C
all the way down.

## Decision

**c2mir / the C-AST IR is madc's primary backend.** Direct-MIR-via-API is *not*
the backend; it is a scoped **scalpel** for runtime internals and the
interpreter/REPL/debug tier (the "hybrid seam," below). The MC11-IR design (the
`cir_node` tree being c2mir's `node_t` plus retained tokens/positions) stands.

## Rationale (identity-first)

1. **The "C11 ceiling" is not a ceiling for what madc actually is.** The single
   biggest theoretical advantage of direct-MIR is escaping C semantics
   (stack-switching coroutines, a non-C++ object model, etc.). madc does not
   want those — its semantics *are* C/C++. So that advantage does not pay off.
   And the ceiling is *higher than bare C11* anyway: per `mir/c2mir/README.md`,
   c2mir already exposes the dynamic-language-JIT machinery as GNU-style
   extensions — **labels-as-values (computed goto), statement expressions,
   `__builtin_jcall`/`__builtin_jret` (fast interp↔JIT switching), overflow
   builtins, and the `__builtin_prop_*` lazy-BB-versioning builtins.** Most of
   what looked like "direct-MIR-only" is reachable through c2mir.
2. **C++ makes c2mir the natural backend, via Cfront.** The proven way to
   implement C++ is to lower it to C and hand it to a C compiler (Cfront). The
   c11-transpiler lowering (classes→struct+vtable, templates→instantiation,
   references→pointers, RAII dtor injection, exceptions→SJLJ) targets **C11 — a
   well-specified intermediate language** — and c2mir *is* that C compiler.
   Direct-MIR would force the C++ lowering to target MIR, which is lower and
   less specified. So the hard part of madc is *easier and more proven* on B.
3. **The "Mad" conventions are backend-neutral.** Namespaces are C/C++ libs;
   multi-syntax is skin-deep. Zero pressure toward direct-MIR.
4. **C all the way down — including the backend.** MIR + c2mir is lean C
   (~23K + ~55K LOC). Consequences: (a) the fork we carry is **C, in madc's own
   wheelhouse** — patching c2mir is the exact work madc developers already do,
   unlike owning an LLVM (C++) or Cranelift (Rust); (b) it opens a coherent
   **self-hosting endgame** — madc compiles C, MIR/c2mir *is* C and already
   self-bootstraps, so "madc compiles the backend it runs on" is reachable in
   principle (needs high C conformance — the same conformance the SMAUG goal
   already drives).
5. **A second compiler ecosystem for free (B-exclusive).** Because the IR is a
   C AST, `--emit=c11` makes madc a **source-to-source compiler**: the same IR
   has three outputs — JIT (MIR), interpret (MIR interp), and **portable C11
   source for any C toolchain**. This:
   - lifts the portability ceiling from MIR's 5 architectures to *anything with
     a C compiler* (Cortex-M, AVR, exotic embedded, WASM via emscripten, vendor
     compilers);
   - lifts the optimization ceiling above MIR's lean JIT for AOT/production
     (`gcc -O3`/`clang -O3`);
   - composes with the whole language (C++ and conventions, already lowered to
     C11), not just the C subset;
   - *escapes c2mir's own hard limits* — SIMD/`vector_size`, inline asm,
     `wchar_t`, and `_Complex` (which forced our fork) are native in gcc/clang,
     so emit-C can be *more* capable than our JIT in those areas.
   Direct-MIR **structurally cannot** do this — MIR is one-way to machine code.

## Alternatives considered

- **A — retarget master's codegen onto MIR (direct-MIR).** Genuinely survives
  the analysis on a few axes: lower JIT/REPL latency (no C-sema pass),
  dependency independence (no c2mir fork), and finer MIR↔source debug control.
  The "MIR features C lacks" axis is **narrower than it first appears**: per
  `mir/c2mir/README.md`, c2mir already exposes computed goto, statement
  expressions, and the lightweight interp↔JIT calls (`__builtin_jcall`/`jret`)
  as extensions — so what's *genuinely* direct-MIR-only is **native multiple
  return** (no C syntax — we emulate with `__retbuf`) and **guaranteed tail
  calls**. **But** all of these are *runtime/implementation* arguments, not
  *language* arguments — and madc would
  then **own the C ABI per target** (master had only x86-64 SysV; c2mir ships
  per-arch ABIs for free) and own C-conformance correctness. Rejected as the
  *primary* backend; its surviving value is captured by the hybrid seam.
- **A2 — pure transpile (emit C text, re-parse with c2mir's parser).** Adds a
  text round-trip with no benefit over building `node_t` directly. Rejected for
  the hot path; retained only as the `--emit=c11` output mode.
- **Hybrid (adopted alongside B).** c2mir for all *language* lowering; drop to
  direct-MIR-via-API only for selected **runtime internals** (dispatch
  trampolines, exception runtime, multi-return shims) and the **interpreter /
  debug / REPL tier**, all emitted into the same MIR module and linked together.

## The hybrid seam (what may bypass c2mir, and why)

Allowed to be emitted as direct MIR (or run via MIR interp), because they are
implementation mechanics rather than user-language semantics:

- the **interpreter / REPL / debug tier** (MIR interp on the same module);
- runtime **trampolines / dispatch** and other glue where a C expression is
  pure impedance;
- constructs where MIR is strictly better and C only approximates (e.g. genuine
  multi-return) — *as an optimization of an already-working C-lowered path,
  never as the only path*.

User-visible language features stay on the c2mir path so they also flow through
`--emit=c11`.

## Consequences (accepted trade-offs)

- **We maintain a c2mir fork** (`github.com/derekbsnider/mir`, branch
  `develop`, pinned by `MIR_COMMIT`) — native `_Complex`, cleanup attributes,
  and the ABI/codegen fixes the CIR backend depends on. Accepted: it is C, and
  it is the backend we depend on (see ADR-adjacent KG
  `Decision{madc_mir_fork_dependency}`).
- **We drive c2mir's `node_t` the way its own parser would** — a fragile seam
  that has surfaced internal-convention bugs (abstract-param `N_TYPE`, fn-ptr
  node ordering). Accepted; mitigated by treating c2mir as canon and reducing
  to minimal `.c` reducers when a tree is rejected. Concretely: the develop hot
  path enters via a **fork-added `c2mir_compile_tree(ctx, c2m, tree, module)`**
  (`madc_cir.cpp`), injecting a pre-built `node_t` and bypassing c2mir's
  preprocessor + PEG parser. c2mir's *documented* library API is source-text
  via `c2mir_compile(getc_func)` (still used by the older `madc_mir_backend`
  path and by `--emit=c11`). So the coupling includes an **API surface we added
  to the fork**, not just patches — a real maintenance fact, and a deliberate
  one (reuse c2mir's sema + MIR-gen, skip its parser since madc has its own).
- **c2mir's explicit non-features** (per its README): VLA (we lower →
  `__builtin_alloca`), `_Complex` (our fork adds it natively), atomics, and
  thread-local storage. Anything madc needs there is lowering or fork work.
- **SIMD is a *floor* gap, and is roadmapped as a raise** (Track 1.6, KG
  `Decision{simd_raise_mir_upstream}`). MIR itself has no vector type/insns, so
  real SIMD-in-JIT means adding a minimal generic-vector extension to **MIR**
  (+ per-target codegen) and a c2mir `vector_size` front-end — **designed for
  upstream** (it enables MIR's own WASM→MIR goal, since WASM has SIMD).
  Interim: scalarize for the JIT, emit-C → gcc/clang for real SIMD (AOT).
- **~2× compile latency** vs direct-MIR. Accepted for batch/JIT; the
  interactive tier is addressed by the hybrid interpreter seam.
- **Coverage regression during the transition** (master 419 pass → develop's
  CIR baseline, climbing back; 486 integration pass / 4 fail / 1 timeout after
  the 2026-06-05 retire-std-hardcoding merge). The *destination* is right; the
  cost was the rebuild. **develop is not promoted to master until the CIR path
  reaches feature parity** (KG `Decision{promote_parity_gate}`).
- **Everything must be expressible as C11 (+ c2mir extensions).** Accepted —
  it is, by madc's definition; and the emit-C path can target gcc/clang where
  c2mir's own limits would otherwise bind.

## References

- `docs/rules/mc11-ir.md` — the MC11-IR definition (set in stone).
- `docs/rules/c11-transpiler.md` — the C++→C11 lowering rules (Cfront).
- MIR project: <https://github.com/vnmakarov/mir>; madc fork:
  <https://github.com/derekbsnider/mir>.
- "The MIR C interpreter and JIT compiler" (Red Hat, 2021).
- KG: `Decision{promote_parity_gate}`, `Decision{madc_mir_fork_dependency}`,
  `Metric{test_status}`, `Milestone{smaug_boots, smaug_combat_works}`.
