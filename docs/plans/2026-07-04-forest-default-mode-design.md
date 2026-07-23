# Forest Default Mode — B4 design (parse-time grove binding)

**Date:** 2026-07-04 · **Status:** ✅ B4a–B4c LANDED; B4d optional-parked
(profile-gated). Audit 2026-07-19 (task #60), superseding the stale DESIGN
banner:

> **B4a** landed with the format-v2/pack/oracles/build-modes slice (see the
> governing plan's landing history). **B4b** landed @df7241f0 (B4b.1
> parse-time grove availability + include recognition, flag-gated) →
> @9ee21881 (Phase-6 slice 2: `#include` of a forest header LOADS + restores,
> no re-parse). **B4c** landed @561cce34 (2026-07-08): embedded-forest bind is
> the DEFAULT (madc.cpp — silent fallback to live parse when no blob/pin
> mismatch; `--no-forest-bind` is the opt-out/A-B lever; freeze modes
> excluded), and the packed release suite — the arbiter, 717/0/0/16 as of the
> audit — runs the ENTIRE suite under forest-default on the stripped `-O2`
> binary, which is exactly B4c's gate.
>
> **§10 acceptance measured (release binary, `<iostream>` TU, 2026-07-19):**
> default bind lexes 19 tokens, parse 0.001 s, decl-parse 0.000 s, total
> in-process 0.194 s; `--no-forest-bind` live lexes 150,246 tokens, parse
> 0.239 s, decl-parse 0.099 s, total 0.441 s. The decl-parse/lex tax is GONE
> from the default lane; the remaining bind-lane cost (~0.14 s, attributed to
> the lex bucket) is blob load + grove binding — that overhead plus the live
> lane's class-instantiate profile (186 parse / 383 cache on this TU) is the
> input for the **B4d go/no-go profile** (B4d = tsubst-from-frozen-pattern
> trees; stays parked until a workload shows it pays).
>
> Original design text below is retained for the B4d rung and the fallback
> matrix (§5) / branch-macro guard (§6) reference semantics.

(the operative B4 design for the
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
`-DMYAPP_DEBUG` doesn't intersect and costs nothing.

### Canonical include order (design-owner refinement, 2026-07-04)

There IS a known proper order for system-header includes, so order
variance is not fenced out of contract — it is **normalized to the
canonical order**, a positive spec instead of a documented edge:

- **The pack order is an explicit, versioned artifact** (recorded in the
  container directory, not an accident of closure traversal): the known
  proper system order — features/config headers first, foundational type
  headers before their consumers, the same discipline glibc's internal
  ordering encodes.
- **Normalization is semantics-preserving for conforming code by the
  standards themselves:** C++ requires standard headers to be includable
  in any order ([res.on.headers]); POSIX likewise for its modern header
  set. The only order-sensitive channel left — a *branch-relevant* macro
  defined between two system includes — is exactly what the
  `SNAP_KIND_CIR_BRANCH_MACROS` guard already catches (loud fallback).
- **Bind semantics under normalization:** macro exports still install at
  each `#include` point in program order (user code between includes sees
  the surface it would see live), but system-unit *interdependencies*
  always compose in canonical DAG order — so any user ordering of system
  includes yields the same bound surface as the proper ordering.
- **This rides the existing auto-include machinery, not new invention:**
  madc already auto-includes system headers from a symbol→header trigger
  (`Program::auto_include_standard_identifier`,
  `pending_auto_include_headers`, madc.h:2690/2573) and — the mechanical
  key — the lexer already supports **id-level token-buffer reorder at
  cursor 0** built for auto-include injection (madc.h:1310/1346).
  Auto-SORTING user system-include directives into canonical order is the
  same reorder pass with an order key from the pack directory. Auto-sort
  and auto-include become two faces of one normalization: missing system
  includes can be injected, present ones ordered, both keyed by the same
  table.

## 7. Pack pipeline + qualification (execution-plan Phase 4, unchanged in
substance)

- The build gains a pack step after `bin/madc` links: drive the just-built
  compiler over the packaged header set under the pinned config →
  `cir_freeze_forest` v2 payloads → `--freeze-append` onto `bin/madc`.
  Build remains hermetic: pack failure = build failure; a madc without a
  forest still works (fallback row 2).

### Build modes (design-owner directive, 2026-07-04)

Three make modes, formalizing what is half-present today:

| Mode | Flags | Strip | Forest |
|---|---|---|---|
| `make` (develop, the default) | `-O0` — unchanged; the documented "-O0 during development, optimize algorithms first" contract stands | no | optional (pack on demand; absence = fallback row 2) |
| `make debug` | `-O0 -ggdb` | no | optional |
| `make release` | `-O2` — the Makefile's own "last-lap switch, flipped once the front-end is doing the work properly"; B4 IS that last lap | **yes, BEFORE append** | packed + appended, always |

- **Strip-before-append is mandatory ordering, not preference:** `strip`
  rewrites the ELF image and discards trailing non-ELF bytes — stripping
  after the append would destroy the blob. Release pipeline:
  compile `-O2` → link → `strip` → pack (freeze the closure with the
  just-built, just-stripped binary) → append → verify (EOF footer
  readable, pin matches, smoke run). The append step depends on the
  binary target, so any relink re-packs — a stale blob cannot survive a
  rebuild.
- **Strip vs the JIT resolver:** `bin/madc` links `-rdynamic` because
  `cir_import_resolver` dlsyms host symbols out of the executable's
  DYNAMIC symbol table. Plain `strip` removes `.symtab` but preserves
  `.dynsym`, so default stripping is safe; aggressive variants that touch
  dynamic sections are forbidden. The release gate must include a
  dlsym-heavy smoke run on the stripped+appended binary —
  `forest_selfexe_gate.sh` already is exactly that.
- **Per-mode object trees** (`obj/<mode>/`): `-MMD` dependency files track
  headers, not flags — today's `debug:` target rebuilding into the same
  `obj/` silently mixes `-g` and non-`-g` objects. Modes must never share
  objects.
- **`NDEBUG` stays OFF in release initially** (behavior identity with the
  tested develop build); revisit only after the full suite runs green on
  the release binary.
- libmadc.so strips normally in release (no appended blob on the .so);
  the MIR fork library's own optimization level is checked at
  implementation (pin discipline unchanged).
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

- **B4a — format v2 + pack driver + oracles + build modes** (no
  consumption yet): pack-time decl-boundary recording,
  token/index/PP-export/edges segments, the canonical-order artifact in
  the directory, `-dM` + index-parity oracles green over the packed
  closure; the `make release`/`make debug` mode scaffolding (per-mode
  object trees, strip-before-append ordering, release pack+append+verify).
  Gates: standard battery (fulltest incl. forest_selfexe_gate, torture
  failset, emit-C corpus, SMAUG) untouched on the develop build — this
  slice only writes containers — plus the release-binary smoke
  (stripped+appended self-exe run).
- **B4b — bind + materialize-on-use behind a flag**: the forest-lookup
  chain, PP-export install, nested slice parse. Gates: real
  `<iostream>/<string>/<vector>` programs == live parse == g++ with the
  flag on; `--show-stats` shows decl-parse collapsing to the used set;
  full battery with the flag OFF byte-identical.
- **B4c — DEFAULT flip**: forest-on by default (the pack step ships it),
  §5 fallback matrix live, §6 branch-macro guard + canonical-order
  normalization live (the auto-include reorder pass gains the order key).
  Gates: the ENTIRE suite runs under forest-default; the full suite ALSO
  runs green on the `make release` binary (stripped, `-O2`, forest
  appended); torture failset byte-identical; SMAUG (C89 headers benefit
  too) boots; perf numbers published in the landing block from the
  RELEASE build (`--show-stats` before/after — the ~1.9 s decl-parse +
  lex tax vs grove binding).
- **B4d — Tree-1 pattern freeze (instantiate-from-frozen)**: pattern cir
  bodies as B3 cir segments keyed by pattern identity; tsubst copies from
  thawed trees; kills the remaining first-use parse cost for template-
  heavy code. Optional rung, profiled first.

Every slice: two-commit convention, landing block in the governing plan,
mirrors + KG synced at milestones.

## 10. Acceptance (what "B4 done" means)

A stock **`make release` `bin/madc`** — `-O2`, stripped, forest appended —
compiles and runs a real `<iostream>/<string>/<vector>` program **by
default** with no live parse of any packed header, output == g++, with
`--show-stats` decl-parse+lex reduced from ~2.5–2.9 s flat to grove-bind +
used-decl parses (target: tens of ms for typical TUs — measured, published
from the release build), every fallback row in §5 behaving as specified,
and the full suite green in both develop and release modes.
