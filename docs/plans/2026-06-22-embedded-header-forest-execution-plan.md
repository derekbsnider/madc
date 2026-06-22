# Embedded Header Forest — Execution Plan (measurement-gated)

**Date:** 2026-06-22
**Status:** PLAN
**Goal:** Make stdlib-heavy compiles **FAST** by loading a pre-parsed, embedded
**AST forest** instead of re-parsing the libstdc++/glibc closure on every compile.
**Fence:** This is the optimization pass that follows the current correctness work
(real-header parsing, shim retirement, the develop→master CIR-parity gate). Do not
start building it before Phase 0 passes and the correctness gate is met.

---

## Relationship to existing docs (read, do not duplicate)

- **Prior-art / mental model:** `2026-06-09-embedded-header-forest-design.md`
  (Clang PCH/modules, Roslyn green/red, salsa durability, serialization verdicts).
  The literature sweep is DONE — do not re-research it.
- **Full-pipeline refactor + phasing:** `2026-06-09-frontend-representation-refactor.md`
  (P0–P6; this plan is the forest-specific slice of P3/P4/P5).
- **Packaging / on-disk format:** `2026-06-13-embedded-ast-frontend-design.md`.
- **Type-identity substrate:** `2026-06-12-type-table-value-abi-design.md`
  (the uint32 typeid table — a hard prerequisite, see Phase 1).

This plan adds two things the design docs lacked: (a) the **format decisions
settled 2026-06-22** (below), and (b) a **measurement gate** in front, so we don't
fund the expensive AST-forest half if a cheaper token-level PCH already wins.

---

## SETTLED 2026-06-22 — do not re-litigate

1. **The live, c2mir-visible tree stays pointer-based `node_t`.** No wholesale
   conversion of the live IR into a pointerless handle-arena. (Repeatedly set aside;
   it stays set aside.)
2. **Payload = pre-parsed AST subtrees** (the forest), **one segment per header
   file** — i.e. **save state *after* parsing**, load it, and proceed from there.
   This is the decision, not token-level PCH: parsing the template-heavy libstdc++
   closure is the dominant front-end cost (the reason PCH/modules exist at all);
   tokenizing is not the bottleneck, so freezing the token stream would leave the
   real cost on the table. Save the post-parse `cir_node` state and resume.
3. **On-disk container:** zstd-compressed segments **appended to the end of the
   `madc` binary**, followed by a **segment directory**, then a **fixed footer**
   (magic + byte-offset of the directory). Found at runtime via
   `readlink(/proc/self/exe)` → `mmap` → read footer from the end → follow to the
   directory. No magic → no blob → fall back to live parse.
4. **Per-file segment granularity.** `#include` = a **reference to another
   segment**, never inlined (gives dedup of shared `bits/*` headers + lazy
   decompress). A **shared zstd trained dictionary** offsets small-frame ratio loss.
5. **Addressing = `(segment_id, node_index)`.** A segment is a **flat array of
   fixed-size node records**; `offset` is a **node index** (`base[seg] + idx*sizeof`).
   All variable-length data (strings, wide-int literals) lives in **shared pools**,
   referenced by handle — never inline in the node block. The reference is stored as
   **two `uint32`s behind ONE inline `resolve(ref)` accessor** — NOT hand-packed
   bits. (No overflow cliff; zstd eats the slack on disk; pack to one `uint32` later
   behind the same accessor if profiling demands — the accessor is the single
   chokepoint, same discipline as `call_emit_symbol`.)
6. **No separate connector-node KIND.** A "connector" is simply a reference whose
   `segment_id != current`. Resolving a reference whose segment is not yet loaded
   **triggers load+decompress+register-base**. Uniform; load-on-demand falls out.
7. **Two layers (green/red).** Cold forest = `(seg,offset)` handles (lookup+add per
   deref — fine for the cold availability layer). Materialized / c2mir-visible =
   **real pointers**, swizzled on resolve **at the c2mir boundary**. Only *used*
   nodes materialize; the hot path stays raw pointers.
8. **Context-hash PIN** in the directory header: hash of `{toolchain, --std,
   layout-affecting -D/target, search paths}`. Checked once, before anything else.
   Mismatch → **reject the blob, parse live**. Never silently use a mismatched
   forest. (Today's `compiler_hash` is a static string — replacing it is in scope.)

---

## Phase 0 — Baseline numbers (capture the *before*; not a go/no-go)

The forest direction is settled (save state after parse). Phase 0 is NOT a
"should we build it" gate — it exists to (a) record the **before** number so we can
prove the speedup, and (b) confirm early that **load+materialize actually beats
parse**, i.e. the swizzle/materialize cost doesn't eat the win.

### MEASURED 2026-06-22 (`--show-stats`, this is the empirical baseline)

| Phase | minimal `#include <iostream>` + 1 line | `testsubscript` (`<iostream>/<vector>/<map>/<string>`) |
|---|---|---|
| input read | 2.0 MB / 0.004 s | 2.4 MB / 0.007 s |
| lex | 0.54 s | 0.97 s |
| **decl-parse (PCH-cacheable)** | **1.92 s** | **1.98 s** |
| instantiate | 0.89 s | 1.49 s |
| parse total | 2.80 s | 3.47 s |
| c2mir compile | 0.005 s | 0.023 s |
| execute | ~0 | ~0 |

**Verdict (no further gating needed):**
- **Parse dominates** (~78% of wall); lex is a fifth of it; c2mir/exec are noise.
- **decl-parse is ~flat (1.92 → 1.98 s) regardless of the program** — it is *fixed
  header-closure re-parse tax* paid on every compile. That ~1.9 s is the forest's
  direct prize (amortized to a load).
- Token-only PCH would recover only the 0.5–0.97 s of lex and leave the ~1.9 s
  decl-parse on the table → confirms freezing the **post-parse** state, not tokens.
- instantiate (0.89 s fixed + grows with use) is the secondary prize (frozen
  instantiations / the landed lazy-body memo).

| # | Measurement | How | Use |
|---|---|---|---|
| 0.1 | Phase split of a real stdlib-heavy compile | Instrument the existing pipeline; bucket wall into **lex+PP / parse / instantiate / codegen** | The baseline; confirms parse is the big bucket (expected) and sizes the prize |
| 0.2 | `.madh` baseline | Compile a stdlib-heavy TU **cold vs. with the existing `.madh` token PCH** | Shows how little tokenizing-only saves — i.e. why the *parse* state is the one worth freezing |
| 0.3 | Mechanism check | **decompress+fixup+materialize ONE frozen header** vs. **re-parse it** | Confirms load `<<` parse. If materialization erodes it, fix the materialize path (Phase 2) before scaling — this is a tuning signal, not a kill switch |

These are cheap (days) and run alongside Phase 1; they do not block starting Phase 1.

---

## Phase 1 — serializable `cir_node` references (the prerequisite)

A subtree can only be frozen if every reference it holds is position-independent.
Today `cir_node` holds raw pointers; convert each reference class:

- **type (`datadef`) → typeid** — consume the `2026-06-12` uint32 type-table
  identity layer (its system segment is the forest's frozen id space).
- **literal value → value-pool handle** — P0 (the `__int128` slices landed); extend
  coverage to all literal consumers.
- **tree links → `(seg, node_index)`** behind the single `resolve()` accessor.
- **provenance / origin tokens →** interned/handle-referenced, or dropped on the
  codegen path (kept for transpile/`--emit` — they are payload there, not cold debt).

**Gate:** `make fulltest` green; gcc torture-ALONE byte-identical to baseline;
`--emit=c11` byte-identical before/after (proves the references stayed c2mir-blind).

---

## Phase 2 — single-segment freeze / thaw

Prove the mechanism on ONE header before scaling.

- **Serialize:** one header's parsed subtree → flat node-record array + offset refs +
  pool handles → zstd block.
- **Load:** decompress to arena → register segment base → resolve-on-touch →
  materialize to real pointers at the c2mir edge.
- **Oracle:** a header loaded-from-frozen produces a `cir_node` tree **structurally
  identical** to re-parsing it (round-trip identity).

**Gate:** round-trip identity holds; that one header compiles + runs == g++.

---

## Phase 3 — multi-segment forest + connectors

- Per-file segments for the stdlib closure; `(seg,offset)` cross-references; the
  segment base table; load-on-resolve crossing connectors.
- The on-disk container: segment directory + footer + **append-to-binary**;
  `/proc/self/exe` + mmap loader (Step 1 of the design).
- **Context-hash pin + reject/reparse** (replaces the static `compiler_hash`).
- **Shared zstd trained dictionary** for the per-file frames.

**Gate:** real `<iostream>`/`<string>`/`<vector>` compile + run == g++ with the
forest mmap'd; `madc -dM` macro parity vs `gcc -dM`; SMAUG C89 soak unaffected.

---

## Phase 4 — pack pipeline + qualification gate

- **Build/install time:** generate the target predefine set once (shared with the
  runtime TU's PP); pre-parse the stdlib closure under the pinned config → freeze →
  segment → set per-segment codec → compress → embed (`xxd -i` static array **or**
  ICU-style mmap'd sidecar; defer to a packaging flag).
- **Deliberate pin, not auto-track:** re-pinning a toolchain release is a
  qualification event — automate the *watching* (a PR that reports what the frontend
  chokes on), keep the *gate* manual.

---

## Hard prerequisites (outside this plan, must land first)

- **Real headers must parse cleanly** — the freestanding compiler-header set
  (bucket 1/2) + the GCC-impersonation surface (predefined macros + `__builtin_*` +
  the `__is_*`/`__has_*` oracles). You cannot freeze a token/AST stream of headers
  that don't yet parse. This is the near-term correctness deliverable.
- **The `2026-06-12` type-table identity layer** (typeids) — Phase 1 depends on it.

---

## Risks / standing discipline

- **Compression ⟂ zero-copy** → two layers (chosen: decompress-to-arena +
  materialize-to-pointer). zstd segments are never resolved in place.
- **Memoize on canonical/global id, not use-site** (Clang's hardest bug class).
- **Pin the context; reject-and-reparse on mismatch** — a layout-affecting `-D`/
  target against a mismatched forest is *wrong*, not just slow.
- **Don't let materialization cost eat the parse savings** — Phase 0.3 measures it
  up front; re-measure at Phase 2 on the real load path.
- **Fenced behind correctness/parity.** This is the optimization pass; it does not
  jump the develop→master parity gate.
