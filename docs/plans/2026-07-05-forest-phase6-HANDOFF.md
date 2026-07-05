# Forest Phase 6 — HANDOFF (READ THIS FIRST, before any forest/bind work)

**Date:** 2026-07-05 · **Branch:** develop · **Status:** ACTIVE — slices 1a+1b+2 landed.

> This handoff exists because of a real post-compaction drift on 2026-07-05: an
> agent resumed, followed a design doc that had quietly reopened a *settled*
> decision, and built ~600 lines of the WRONG thing (re-parsing) before the
> design owner caught it across several rounds. This document is the antidote.
> **Do not skip the "THE INVARIANT" and "DRIFT TRAP" sections.**

---

## THE INVARIANT (never violate)

**The forest is the serialized Tree-1 ROM. Bind = LOAD the grove + RECONSTRUCT
the parser's symbol tables from typed decl records + `tsubst`. NEVER RE-PARSE a
header at bind time.** No `parseStatement`, no token re-lex, no re-derivation. If
a name won't resolve after load, that is a LOAD-FIDELITY bug to fix — never a
re-parse to add.

## READ FIRST, in this order (do not act before you have)

1. **This handoff.**
2. `docs/plans/2026-07-05-forest-tree1-serialize-CORRECTED-design.md` — the
   corrected Phase-6 execution design (the plan of record).
3. `docs/plans/2026-06-23-two-tree-cir-materialize-from-ast-PLAN.md` §2 (Tree-1
   ROM / Tree-2) and §6 **Phase 6** ("serialize Tree-1 … load, instantiate
   without re-parsing"). This is the GOVERNING architecture — the forest IS
   Tree-1.
4. Memory `feedback_forest_load_never_reparse`.

## DRIFT TRAP — do NOT follow this doc

`docs/plans/2026-07-04-forest-default-mode-design.md` is **SUPERSEDED**. Its §1
chose "frozen token slices + re-parse," which contradicts two-tree §6 AND the
parse-once campaign ("g++ doesn't re-parse"). It is the drift source. Do not
implement from it. (Its PP-export/edges/branch-macro ideas are fine — those are
"load, not re-derive" — but the token-slice + `parse_forest_slice` bind is dead.)

## WHY (the one-line rationale that proves the invariant)

`tsubst` reads each Tree-1 node's `datadef()`. A Tree-1 loaded in a fresh process
has `datadef_id`s that resolve NULL cross-process. So instantiating a *loaded*
Tree-1 is impossible unless the `DataDef`/symbol graph is restored — which is why
the freeze header itself names this "the parser-resume slice (B4+)." The
requirement falls out of the architecture; it is not a preference.

---

## LANDED (develop, each gated green)

- **Design + correction:** `0f18123e` (corrected design doc).
- **Slice 1a — `c8bdd7ba`:** typed decl-record format
  (`cir_forest_decl_record` {name_id,kind,type_id,ns_id,aux} +
  `SNAP_KIND_CIR_DECL_RECORDS` seg + `CIR_FOREST_FORMAT_VERSION` 2→3), freeze
  collector `cir_forest_collect_decls` (madc_cir.cpp, walks `user_typedef_names`),
  reader `CirFrozenForest::decl_records()`. Unit-test round-trip proves a
  typedef's PRIMITIVE type-id survives freeze+reopen and resolves with no Program.
- **Slice 1b — `25406c81`:** `Program::forest_restore_decls(CirFrozenForest&)`
  (parser.cpp, near `lazy_resolve_type`) registers records into `datatype_map`
  (+`namespace_datatype_map` for scoped) + `user_typedef_names`. Unit test proves
  a FRESH Program resolves a header typedef after restore with NO header parse.
- `test_cir_freeze`: 14 cases / 194 assertions.
- **Slice 2 — wire `#include` → bind (LOAD, no re-parse).** Lifted the salvage
  onto develop (see PARKED): `CirFrozenForest::find_unit` + `_unit_by_name`
  (cir_freeze), the `--forest-bind[=path]` flag (madc.cpp), and
  `ensure_bind_forest`/`forest_unit_for_include`/`forest_bind_include`/
  `forest_install_pp` + the `_getToken` bind hook (lexer.cpp). On a system
  `#include` that names a frozen unit: install PP-exports along the include DAG,
  call `forest_restore_decls` ONCE per compile, and **return without tokenizing
  the header**. Crux fix: `forest_restore_decls` now also pushes a `dkTypedef`
  `TopDecl` (exactly what the live `record_typedef` does, parser.cpp ~22689) so
  the `typedef int myint;` node is reconstructed into the c2mir tree — otherwise
  the parser resolves the alias but c2mir reports "unknown type". Proven
  cross-process by `scripts/forest_bind_gate.sh` (wired into fulltest after
  `forest_selfexe_gate`): freeze a typedef-header container, then in a FRESH
  process `--forest-bind` a consumer that uses the typedef (and live-parses
  `<cstdio>`, which is NOT in the container) → output == live == g++, and the
  `-v` trace proves the header BOUND (no silent live fall-through).
- **Decl restore is forest-GLOBAL for now** (records carry no unit attribution):
  the first grove bind restores every typedef decl record once. Per-unit
  attribution pairs naturally with the struct/class slice (which adds
  system-segment type-ids + a `unit_id` on the record). Fine for typedefs.

## PARKED — DO NOT MERGE

Branch `feature/forest-b4b-bind-claude` (commits `df7241f0`, `d15b341d`,
`4c1f29f4`) is the WRONG-TURN token-reparse bind. It stays as a **salvage
reference only**. Its load-only pieces WERE LIFTED onto develop in slice 2
(`9ee21881`): forest-open-at-parse-time (`ensure_bind_forest`), the reverse
directory (`CirFrozenForest::find_unit` / `_unit_by_name`), the
`--forest-bind[=path]` flag, and PP-install + DAG-walk (`forest_install_pp`,
`forest_bind_include`). Its re-parse pieces were DROPPED and never lifted:
`parse_forest_slice`, the token slices, and the cascade hooks in
`find_template`/`lazy_resolve_type`/`resolve_namespaced_type_token`. Nothing else
is left to salvage from that branch — it can be deleted once struct/class lands.

## NEXT — slice 3: struct/class (the next unit of work)

Slice 2 (`#include` → bind, typedefs) is DONE and gated. Slice 3 = restore
`DataDefSTRUCT`/`DataDefCLASS` (members + access + offsets + methods + bases +
vtable) so a bound header's aggregate types resolve + emit into the c2mir tree,
same LOAD-not-reparse shape.
- **Type-id blocker:** aggregate/project types resolve NULL cross-process. The
  system segment `[MADC_TYPEID_PRIMITIVE_END, MADC_TYPEID_PROJECT_BASE)` is
  RESERVED for the forest (`madc_type_from_id` returns NULL there today); this
  slice assigns forest aggregate types stable system-segment ids at freeze and
  adds a reader-side resolver. That is the real new depth (slice 2 rode on pinned
  PRIMITIVE ids, which resolve in any process with zero extra machinery).
- **Reconstruction:** extend the decl record (or add typed struct/member records)
  so `forest_restore_decls` rebuilds the `DataDefSTRUCT` + pushes a `dkStruct`
  `TopDecl` (mirroring the live `record_struct`, parser.cpp ~22664) — the
  aggregate analogue of slice 2's `dkTypedef` push.
- **Per-unit attribution** naturally lands here: give each decl record a
  `unit_id` so restore can be per-bound-unit instead of forest-global.

Then, in order: **`TemplateDef` patterns + tsubst-from-loaded-Tree-1** → **flip
forest to default + DELETE the token-reparse path** (B4a token slices/decl-index +
the parked B4b branch; `-Wunused-function` confirms the cut).

## GATE (every commit — two-tree §8; never perf-gate)

- `make -C src` clean, no new warnings.
- `make -C src fulltest` green (warning ratchet, tsubst flag-on baseline,
  `forest_selfexe_gate`, both forest oracles, `test_cir_freeze`).
- gcc.c-torture failset byte-identical to the 51-name baseline.
- `--emit=c11` byte-identical on representative TUs.
- **Cross-process:** freeze in one process, load+compile+run in a FRESH process,
  output == g++.

## CURRENT TYPE-ID NUANCE (so the next slice isn't surprised)

Primitive type-ids are pinned and resolve in ANY process (that's why slice 1
works with zero extra machinery). Aggregate/project types resolve NULL
cross-process. The struct/class slice must assign forest aggregate types
**system-segment** ids and add a reader-side resolver for them — that's the next
real depth after slice 2, not slice 2 itself.
