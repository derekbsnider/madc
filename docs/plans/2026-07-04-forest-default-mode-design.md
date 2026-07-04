# Forest Default Mode — B4 design (parse-time grove binding)

**Date:** 2026-07-04 · **Status:** DESIGN (the operative B4 design for the
data-substrate governing plan §5 / forest execution plan Phase 4)

> **Design-owner directive (2026-07-04):** use of the frozen forest is the
> **DEFAULT mode of operation**. madc ships with its standard-header forest
> appended to the binary; `#include <system header>` binds a frozen grove
> instead of parsing; live parse is the *fallback*, not the norm. Groves
> load on demand — nothing but the directory and closure tables is read up
> front (confirmed 2026-07-04 alongside B3: connectors/anchors are the
> keyframe nodes other units connect through, and the same directory-keyed
> binding serves C++20 `import`).

**Reads with (settled; do not re-derive):**
- `2026-06-09-embedded-header-forest-design.md` — the mental model
  (availability vs use, materialize-on-resolve, system/project split,
  Clang/Roslyn verdicts). This doc turns that model into madc-concrete
  machinery.
- `2026-06-13-embedded-ast-frontend-design.md` — dev/prod mode contract,
  per-segment codec, versioning, pack qualification. §6's "no macro
  fingerprint check" is REFINED here (§6 below) because forest-default
  raises the safety bar: we add a *branch-relevant macro set* auto-fallback.
- `2026-06-22-embedded-header-forest-execution-plan.md` — SETTLED format
  decisions + Phase 4 scope. B1–B3 landed (@0b1e618a, @b62089ad, @75830eec).
- `2026-07-04-data-substrate-first-customer-PLAN.md` — governing order;
  landing history.

---

## 0. What B3 left standing (the gap this design closes)

B3 proved the CONTAINER story end-to-end: per-unit groves, on-demand
connector loading, cross-process closure (string pool, positions, type
names, libs), context-hash pin, append-to-binary + `/proc/self/exe` loader.
A frozen *module tree* runs in a fresh process in 82 ms where live
parse+run costs 1.66 s.

What it deliberately did NOT do: let the **parser** consume a grove. Today
`#include <vector>` still lexes and decl-parses the whole closure —
the measured **flat ~1.9 s decl-parse tax** (plus ~0.5–1.0 s lex). The
compiler state that work produces is *parser-side*: `datatype_map` /
`struct_map` / `namespace_map` DataDefs and FuncDefs, Tree-1 template
patterns, and the macro tables. B4 = make `#include` bind the grove and
produce that state **lazily, per used declaration**.

## 1. The core decision: what a grove carries per exported name

Three candidate architectures were weighed:

**(a) Serialized decl records (the Clang PCH shape).** Freeze the DataDef /
FuncDef object graphs as typed records; deserialize on lookup. Fastest per
use — and the largest new surface in the compiler: every field of every
`DataDef` subclass becomes wire format, drifts with every parser change,
and needs its own identity/merge discipline (Clang's hardest, still-buggy
area 15 years in — the 06-09 research verdict). Rejected as the *first*
rung: cost and drift risk are out of proportion to what the numbers demand.

**(b) Frozen token slices + lazy per-decl parse (CHOSEN for B4).** The
grove carries the unit's **post-PP token stream** (the `.madh` serialization
madc has shipped since Phase 1) plus a **decl index**: exported name →
{kind, token-slice range}. Binding installs names; the FIRST USE of a name
nested-parses just its slice through the **normal decl-parse path**,
producing the DataDef/FuncDef/Tree-1 pattern exactly as today. Nothing new
learns to build parser state; the forest only changes *when* and *how much*
of it gets built.
- The lazy machinery already exists in production twice over:
  `lazy_map`/`lazy_resolve`/`lazy_resolve_type` (parser.cpp:12146+) is
  precisely "name known, registration deferred to first use", and
  `parse_deferred_lazy_body` (driven by the cir_builder fixpoint,
  cir_builder.cpp:18186) is precisely "nested parse of a saved token range
  mid-build". B4 generalizes both, replacing `LazyEntry{header,kind}`'s
  int-keyed hardcoded `add_xxx()` builders with data-driven forest refs.
- **Parse-once compliance:** a lazy decl parse is the ONLY parse of that
  decl in the process — the legal category (`parse-once.md`: the remaining
  body parses are the ones that are the only parse). Instantiation still
  never re-parses (Tree-1 patterns are built by the slice parse and then
  tsubst as today).
- **Cost model:** the 1.9 s tax exists because live parse builds
  *everything in the closure*; a real TU uses a small fraction. Used-decl
  parsing is proportional to the use set, not the closure. The remaining
  per-use cost is bounded and amortizes within a compile.

**(c) The ladder (adopted as the trajectory).** (b) is rung one. If
profiling later shows hot per-use parse cost, individual decl KINDS
escalate to serialized records *behind the same index* — the index entry
grows a "payload form" tag, exactly the per-segment-codec escape-hatch
discipline. Tree-1 pattern bodies additionally freeze as **cir trees on the
B3 machinery** (rung B4d below) so repeat instantiations copy from frozen
trees. One container, several segment kinds, no parallel formats
(`no-parallel-implementations.md`).

## 2. Grove payload v2 (extends the B3 container; no format break)

Per unit, ADDED segment kinds (B3's records/children/connectors/positions
stay for the module-cache surface and B4d):

| Segment | Content |
|---|---|
| `SNAP_KIND_CIR_UNIT_TOKENS` | the unit's post-PP token stream, `.madh` record form (`madc_pch::write_madh` payload as a segment body) |
| `SNAP_KIND_CIR_DECL_INDEX` | exported name → {name_id, kind, slice_begin, slice_end, aux} — names include operators (ADL) and namespace qualification; a name with N overloads has N entries (a bind materializes the full overload set) |
| `SNAP_KIND_CIR_PP_EXPORTS` | the macro delta this unit itself defines/undefines (`#undef` = tombstone), name_id → MacroDef body text; composed along include edges at bind |
| `SNAP_KIND_CIR_UNIT_EDGES` | this unit's `#include` edges (directory unit ids) — SETTLED #4: an include is a reference to another segment, never inlined |
| `SNAP_KIND_CIR_BRANCH_MACROS` | container-global: the set of macro names the pack-time PP *consulted in conditionals* anywhere in the closure (see §6) |

`anchor_idx` (reserved in B3) points at the unit's decl-index segment
entry point — the keyframe a parse-time `#include` (or C++20 `import`;
the directory key is an interned unit-name spelling, not intrinsically a
path) binds to.

The pack-time parser records decl boundaries during its normal parse (it
knows them trivially); no second scanner exists.

## 3. Bind semantics (`#include <X>` under forest-default)

1. Resolve the include to a unit-name key; look it up in the forest
   directory. **Miss → live parse of that header** (per-include fallback;
   third-party/user headers always take the live path — the 06-13 dev/prod
   contract).
2. Install the PP exports: walk the unit's include DAG (visited set),
   applying each unit's macro delta in order. User code after the include
   sees the same macro surface live parsing would produce.
3. Arm name lookup **miss-driven**: the unit (with its DAG) joins a
   forest-lookup chain consulted by the parser's existing unknown-name
   hooks (`lazy_resolve` / `lazy_resolve_type`). No per-name eager
   registration — `#include` stays O(depth of DAG), not O(names).
4. Nothing materializes. Availability only (06-09 model).

## 4. Materialize-on-use

- Parser hits an unknown name → forest chain → decl-index hit →
  **nested parse of the token slice** (tokens re-materialize from the
  frozen stream through the `push_precompiled_header_tokens` rebind path,
  lexer.cpp:2152) → the normal decl-parse builds the DataDef / FuncDef /
  Tree-1 pattern → memoized (slice parsed once per process).
- **Cascade:** the slice parse itself hitting unknown names recurses
  through the same hooks (the typedef/lexer-hack context resolves the same
  way). Termination: finite closure + per-slice in-progress marks.
  Reentrancy has the lazy-body existence proof; it is the top
  implementation risk (§8).
- Overload sets: a name hit materializes ALL its index entries (overload
  resolution needs the full candidate set). ADL: operator names are
  ordinary index entries under their namespace.
- Templates: the slice parse saves the pattern (Tree-1 recipe) as today;
  instantiation is tsubst, never re-parse.
- MC11-IR law: materialized decls' cir nodes carry origin tokens created
  by the slice parse — full provenance for everything that exists
  in-process; unused decls never exist at all (that is the optimization).

## 5. Default-mode contract and fallbacks

| Situation | Behavior |
|---|---|
| madc binary carries a forest, pin matches | groves bind by default |
| no appended blob (dev build, stripped copy) | live parse, silent |
| context-hash pin mismatch | whole forest OFF, loud one-line notice, live parse (never mis-thaw — B3 semantics) |
| include not in the directory | that header live-parses; forest stays on for others |
| CLI `-D`/`-U` naming a **branch-relevant macro** (§6) | whole forest OFF for system units, one-line notice, live parse |
| explicit opt-out flag | live parse everything (flag name decided at B4c; today's `--no-embedded-headers` keeps its shim-bypass meaning until the shim-retirement track converges the two) |

Fallback is always the FULL live path that exists today — one parser, one
decl-parse implementation; the forest only short-circuits it
(`no-parallel-implementations.md` is satisfied because the lazy slice
parse IS the production decl parser on a narrower input).

## 6. The PP contract (refining 06-13 §6 for default-on)

The pack parses the closure ONCE under the **pinned predefine set — the
same set the live PP uses by default** (`src/predefined_macros.cpp`), so
default-forest behavior equals default-live behavior by construction.

06-13 §6 said "no runtime macro-fingerprint check" — acceptable for an
opt-in mode, too sharp an edge for a DEFAULT: real code sets feature
macros (`_GNU_SOURCE`, `_FILE_OFFSET_BITS`) and silently serving a
mismatched grove would be wrong-not-slow. Refinement, cheap and precise:
at pack time the PP records **which macro names its conditionals actually
consulted** (`SNAP_KIND_CIR_BRANCH_MACROS`); at startup, any CLI/user
`-D`/`-U`/pre-include `#define` naming one of them disables the forest
(loud, one line) and falls back to live parse. An app-local
`-DMYAPP_DEBUG` doesn't intersect and costs nothing. Include-order
variance beyond the packed canonical order remains out of contract
(documented; the escape is the same fallback).

## 7. Pack pipeline + qualification (execution-plan Phase 4, unchanged in
substance)

- `make` gains a pack step after `bin/madc` links: drive the just-built
  compiler over the packaged header set under the pinned config →
  `cir_freeze_forest` v2 payloads → `--freeze-append` onto `bin/madc`.
  Build remains hermetic: pack failure = build failure; a madc without a
  forest still works (fallback row 2).
- **Shared trained zstd dictionary** lands here (per-file frames are now
  numerous and small; requires HAVE_ZSTD + ZDICT; the per-segment codec
  field already carries the flip).
- **`madc -dM` oracle** lands here (B4a): dump the effective macro table;
  parity harness vs `gcc -dM -E` under the pinned set, and forest-bind vs
  live-parse macro-table identity — the gate §6 needs.
- Toolchain re-pin stays a **manual qualification event** with automated
  watching (06-13 §8 verbatim).
- System-segment typeid occupancy `[0x100, 0x01000000)` stays FENCED:
  DataDefs are built in-process by the slice parse and stamp project-
  segment ids as today; frozen system ids become meaningful only with
  serialized decl records (ladder rung (c)) — record the reasoning, do not
  build it speculatively.

## 8. Risks

1. **Parser reentrancy** (a slice parse suspending a slice parse, from
   arbitrary lookup points). Existence proof: `parse_deferred_lazy_body`
   nests today. Mitigation: one shared nested-parse entry, in-progress
   marks per slice, cycle = loud error (never silent partial state).
2. **Decl-boundary fidelity** at pack time (multi-name decls, linkage
   blocks, `#pragma`, template guards). Gate: the B4a index-parity oracle
   (every name live parse registers, the index carries — diffed
   mechanically over the whole packed closure).
3. **Materialization ordering effects** (globals, static init, inline
   emission): bounded by the used set, which is exactly the reachability
   discipline the DCE + lazy-body work already enforces.
4. **PP compositionality** (macro deltas along the DAG, `#undef`
   tombstones): gated by the -dM identity harness, forest-bind vs live.
5. **Memory**: token segments decompress per unit on first bind and stay
   (provenance); they are the same bytes live parsing would have held as
   token objects, minus everything never bound.

## 9. Slicing and gates

- **B4a — format v2 + pack driver + oracles** (no consumption yet):
  pack-time decl-boundary recording, token/index/PP-export/edges segments,
  `-dM` + index-parity oracles green over the packed closure. Gates:
  standard battery (fulltest incl. forest_selfexe_gate, torture failset,
  emit-C corpus, SMAUG) untouched — this slice only writes containers.
- **B4b — bind + materialize-on-use behind a flag**: the forest-lookup
  chain, PP-export install, nested slice parse. Gates: real
  `<iostream>/<string>/<vector>` programs == live parse == g++ with the
  flag on; `--show-stats` shows decl-parse collapsing to the used set;
  full battery with the flag OFF byte-identical.
- **B4c — DEFAULT flip**: forest-on by default (make pack step ships it),
  §5 fallback matrix live, §6 branch-macro guard live. Gates: the ENTIRE
  suite runs under forest-default; torture failset byte-identical; SMAUG
  (C89 headers benefit too) boots; perf numbers published in the landing
  block (`--show-stats` before/after — the ~1.9 s decl-parse + lex tax vs
  grove binding).
- **B4d — Tree-1 pattern freeze (instantiate-from-frozen)**: pattern cir
  bodies as B3 cir segments keyed by pattern identity; tsubst copies from
  thawed trees; kills the remaining first-use parse cost for template-
  heavy code. Optional rung, profiled first.

Every slice: two-commit convention, landing block in the governing plan,
mirrors + KG synced at milestones.

## 10. Acceptance (what "B4 done" means)

A stock `bin/madc` compiles and runs a real `<iostream>/<string>/<vector>`
program **by default** with no live parse of any packed header, output ==
g++, with `--show-stats` decl-parse+lex reduced from ~2.5–2.9 s flat to
grove-bind + used-decl parses (target: tens of ms for typical TUs —
measured, published), and every fallback row in §5 behaving as specified.
