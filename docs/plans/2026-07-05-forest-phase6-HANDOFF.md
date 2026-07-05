# Forest Phase 6 — HANDOFF (READ THIS FIRST, before any forest/bind work)

**Date:** 2026-07-05 · **Branch:** develop · **Status:** ACTIVE — slices 1a+1b landed.

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

## PARKED — DO NOT MERGE

Branch `feature/forest-b4b-bind-claude` (commits `df7241f0`, `d15b341d`,
`4c1f29f4`) is the WRONG-TURN token-reparse bind. It stays as a **salvage
reference only**. Reusable pieces for the correct path (lift, don't merge):
forest-open-at-parse-time (`ensure_bind_forest`), the reverse directory
(`CirFrozenForest::find_unit` / `_unit_by_name`), the `--forest-bind[=path]` flag,
and PP-install + DAG-walk (`forest_install_pp`, `forest_bind_include`). DROP:
`parse_forest_slice`, the token slices, and the cascade hooks in
`find_template`/`lazy_resolve_type`/`resolve_namespaced_type_token`.

## NEXT — slice 2 (the next unit of work)

**Wire `#include` of a forest header to LOAD + restore, then a fresh-process run
== g++.** Concretely:
- Lift the salvage above onto develop (open the forest at parse time; `find_unit`;
  `--forest-bind` flag; PP-install along the DAG — that part was correct).
- On a system `#include` that names a forest unit: install PP exports + call
  `forest_restore_decls` for that unit's decls, and **return without tokenizing
  the header** (the `#include` handler is in lexer.cpp `Program::_getToken`, the
  `is_system && !is_include_next` block ~line 2673+; today's paths: filesystem at
  ~2975, embedded at ~2949).
- Gate proof: `bin/madc --forest-bind=<container> prog.cpp` where prog uses the
  header's typedef == live parse == g++, and a FRESH-process `--run`/`--freeze`
  round-trip.

Then, in order: **struct/class** (needs system-segment type-ids —
`[MADC_TYPEID_PRIMITIVE_END, MADC_TYPEID_PROJECT_BASE)` is RESERVED for the
forest, `madc_type_from_id` returns NULL there today; that's where serialized
aggregate types get stable ids) → **`TemplateDef` patterns + tsubst-from-loaded-
Tree-1** → **flip forest to default + DELETE the token-reparse path** (B4a token
slices/decl-index + the parked B4b branch; `-Wunused-function` confirms the cut).

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
