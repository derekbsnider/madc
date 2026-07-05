# Forest Phase 6 — HANDOFF (READ THIS FIRST, before any forest/bind work)

**Date:** 2026-07-05 · **Branch:** develop · **Status:** ACTIVE — slices 1a+1b+2+3a+3b landed.

> ⚠️ **POST-COMPACTION, DO THIS FIRST (owner-directed 2026-07-05) — a
> PARSER-track item, before resuming the forest track (3c):**
> Root-cause and fix the live-parse failure where a struct reached through the
> **system-include / auto-include prelude** path rejects `__int128_t`:
> `parser.cpp:23185` throws *"Expecting type in struct definition, got
> '__int128_t'"*. Exploratory repro (NOT yet minimal): `tmp/link.h` =
> `struct Node { int val; struct Node *next; };`, then
> `bin/madc tmp/rA2.cpp -I tmp` where rA2 = `#include <link.h>` +
> `int printf(const char*,...);` + a `main` using `struct Node`. Note: the error
> line ("99") is in the flattened prelude, NOT the 5-line header — so the
> self-ref pointer / `ns::X` shapes are likely RED HERRINGS; the real trigger is
> `__int128_t` in a prelude/system struct. **Method (gcc-methodology.md):**
> minimize past the include noise to the true trigger → confirm blast radius
> (does it block real corpus headers?) → `gcc -S`/`clang` compare → fix at the
> DEEPEST layer (likely: recognize `__int128_t`/`__int128` as a type spelling in
> the struct-body type parser), gated vs gcc+clang. This is a prerequisite for
> the corpus (a bind can't beat a live parse). It is NOT a forest regression —
> the failing path runs zero forest code (bind hook gated off), the throw is in
> code this session never touched (git -L 383628f6..HEAD clean), and the torture
> failset stayed byte-identical. Only AFTER this: resume forest 3c below.
>
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
  the first grove bind restores every decl record once. Per-unit attribution is
  a later refinement (a `unit_id` on the record). Fine for the curated container.
- **Slice 3a — plain struct/union bind (system-segment type-ids).** Format v3→4:
  `cir_forest_decl_record` gains `member_begin`/`member_count`; new
  `SNAP_KIND_CIR_STRUCT_MEMBERS` segment = a flat u32 `[name_id,type_id,count]`
  member stream (`cir_forest_struct_member`). Freeze (`cir_forest_collect_decls`)
  scans `top_decls` for `dkStruct`/`dkUnion`, assigns each forest aggregate a
  stable **system-segment id** (`[0x100,0x01000000)`, freeze-local — NOT the
  process project id the frozen node tree uses), and serializes members whose
  type is a pinned primitive or an already-assigned forest aggregate; anything
  richer (a `DataDefCLASS` = methods/bases/vtable, anon aggregates, bitfields, a
  member of an unserializable type) is skipped (bind falls back). Restore
  (`forest_restore_decls`) rebuilds the `DataDefSTRUCT` by DRIVING `addMember`
  (identical to the live struct parser → offsets/size/align regenerate
  byte-for-byte), `finalize()`, registers in `struct_map`, and pushes a
  `dkStruct`/`dkUnion` `TopDecl` so `cir_builder::struct_def` emits it. A local
  `restored_by_sysid` map links member references to earlier forest aggregates —
  the restored struct is otherwise a normal project DataDef (no global
  `type_from_id` change). `forest_bind_gate` [struct] case (mixed-type padding +
  a union + `sizeof`) proves bind == live == g++ cross-process; `test_cir_freeze`
  15 cases / 221 assertions.
- **Slice 3a scope limits (documented, safe under the opt-in flag):** global
  scope only (`ns_id=0` — namespaced structs would restore into the global
  `struct_map`; guard/handle when the real corpus is frozen); typedef-of-struct
  alias not yet restored (the struct itself is, via its own `pdkStruct` record).
- **3a also reaches nested VALUE-aggregate members** for free (`restored_by_sysid`
  + definition order) — locked by the `forest_bind_gate` [nested] case.
- **Slice 3b — bitfield members + layout self-validation.** Format v4→5: the
  member stream went from 3-u32 triples to 4-u32 records (`+bit_width`), and the
  decl record gained `orig_size`. A named bitfield serializes its width and
  restores via `addBitField` (mirrors the live parser → bit-offsets regenerate).
  The load-bearing safety: `cir_forest_plain_struct_faithful` (madc_cir.cpp) does
  a **freeze-time round-trip** — replays the members-only reconstruction on a
  scratch struct and requires it reproduce the original's size + every member
  offset + every bitfield bit-offset; a struct that fails (unnamed-bitfield gaps
  — absent from `members`, so they shift later bit-offsets WITHOUT changing size,
  which a size-only check misses — plus `#pragma pack`, reverse storage, explicit
  alignment) is NOT serialized. Restore also re-checks `orig_size` as a
  cross-process backstop. Net: what binds is always laid out right; what we can't
  faithfully rebuild binds cleanly-lacks-it (loud error at use), NEVER wrong
  output. `forest_bind_gate` [bitfield] case + `test_cir_freeze` (positive: Flags
  a@0/b@3; negative: an unnamed-gap `Gap` is refused at freeze) → 16 cases / 245
  assertions.

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

## NEXT — slice 3c: DataDefCLASS (the next real unit of work)

Slices 3a + 3b are DONE and gated. What forest bind now covers: typedefs, plain
structs/unions (mixed types, padding, `sizeof`), nested VALUE-aggregate members,
and NAMED bitfields — all cross-process == live == g++, never re-parsing.
Probing (2026-07-05) mapped the rest of the struct surface; what remains is
either parser-blocked or is 3c:
- **Pointer-to-struct and namespaced structs are BLOCKED by pre-existing
  live-parse bugs, NOT forest gaps** — a separate parser-correctness track:
    - `namespace ns { struct X; }` + `ns::X v;` → live madc "Unexpected keyword
      in expression" (qualified-type declarator gap).
    - self-ref `struct Node { int v; struct Node *next; };` → live madc emits a
      spurious "Expecting type in struct definition, got '__int128_t'" and even
      the producer FREEZE fails. Fix these in the parser FIRST; only then is
      forest support for those shapes gateable (bind can't beat live).
  Do NOT try to fix live-parse bugs inside the forest slices.
- **Unnamed-bitfield / packed / reverse-storage / explicitly-aligned structs**
  are refused at freeze by the round-trip guard (never mis-laid-out); supporting
  them means serializing the extra layout state — low urgency.

So the next REAL value is **3c — `DataDefCLASS`** (methods + access + bases +
vtable): serialize the class's method set / base list / vtable layout,
reconstruct the `DataDefCLASS` (mirror the struct→class promotion +
`class_struct_def` emission), push the `TopDecl`. This is where a bound
`<string>` / `<vector>` class becomes resolvable — the corpus payoff. Start it in
a FRESH session (it is the big slice; compact first).

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
