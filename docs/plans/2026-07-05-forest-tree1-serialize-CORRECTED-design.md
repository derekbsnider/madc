# Forest = serialized Tree-1 (two-tree Phase 6) — CORRECTED execution design

**Date:** 2026-07-05 · **Status:** DESIGN (awaiting sign-off before code)
**Supersedes:** `2026-07-04-forest-default-mode-design.md` (its token-slice + re-parse
rung is RETIRED — it reopened a settled decision; see "How it went wrong").
**Governing architecture:** `2026-06-23-two-tree-cir-materialize-from-ast-PLAN.md`
§2 (the two trees), §6 Phase 6 (serialize Tree-1). This doc is the execution
design for that Phase 6, nothing new.

---

## 0. THE INVARIANT (never violate)

**NEVER RE-PARSE.** Forest bind = LOAD the immutable Tree-1 grove + `tsubst`
(copy + substitute) into a per-TU Tree-2 → c2mir. No `parseStatement` at bind
time, no token re-lex, no re-derivation. If a name doesn't resolve after load,
that is a load-fidelity bug to fix — never a reparse to trigger.
(See memory `feedback_forest_load_never_reparse`.)

## 1. Architecture recap (grounded in the settled plan — do not re-derive)

- **Tree-1 — immutable ROM.** Parsed ONCE: every template pattern + the whole
  embedded-header corpus. Never mutated, never handed to c2mir. It is the COPY
  SOURCE for instantiation and **the serialized form = the header-forest**
  (two-tree §2).
- **Tree-2 — mutable per-TU.** Handed to c2mir (which mutates the `node_t` base
  in place). Built by `tsubst` = copy a Tree-1 subtree into FRESH `node_t`s +
  substitute params (two-tree §2, §11).
- **"Some ROM, some RAM" (§2 SETTLED-3):** c2mir sees only the `node_t` base, so
  Tree-2's `node_t` structure is private per instantiation; the heavy madc info
  (the `DataDef` detail, the pattern back-ref) stays ROM in Tree-1 and is
  *referenced*.
- Compile a TU = parse the user's code against the restored header symbols +
  `tsubst` the Tree-1 patterns it uses → Tree-2 → c2mir. The header is never
  re-parsed.

## 2. What is built vs. the actual gap

**Built (two-tree Phases 1–5, RELEASED v0.33.0):** Tree-1 in-memory patterns +
`tsubst` (templates instantiate by copy+substitute, no re-parse). B1–B3 froze the
Tree-1 cir **`node_t` skeleton + string/value pools + `typeid→name` closure**.
That is why `--run-frozen` works: it hands the frozen `node_t` straight to c2mir
and **never parses**, so it needs no madc `DataDef`s.

**The gap (this is Phase 6):** binding a header into a *user* compile is different
from `--run-frozen` — the user's code must be **parsed** against the header's
symbols (`std::vector` must resolve to its template; `size_t` to its type). Per §2
"some ROM, some RAM," Tree-1's `DataDef`/symbol graph is currently **referenced by
id, live-only — not serialized**. Cross-process, those ids resolve NULL. So:
- the parser can't resolve header names in user code (no symbol tables), and
- `tsubst` over a *loaded* Tree-1 can't read `datadef()`.
Both are fixed by the same thing: **serialize Tree-1's `DataDef`/symbol graph and
restore the symbol tables on load.**

**How it went wrong (for the record):** `2026-07-04-forest-default-mode-design.md`
reopened this settled Phase 6, weighed "serialized decl records" vs "frozen token
slices + re-parse," and chose re-parse — contradicting the two-tree plan's Phase 6
AND its standing rule ("SUPERSEDES scratch-token re-parse — g++ doesn't
re-parse"). B4a built the token slices; B4b built the re-parse bind. Both are
retired here.

## 3. The approach (Phase 6, as designed)

1. **Serialize Tree-1's decl graph** — `DataDefCLASS`/`DataDefSTRUCT`/`FuncDef`/
   `TemplateDef` + the symbol-table entries — on the substrate primitives already
   built for exactly this (segmented `u32` type-id table + intern tables + the
   snapshot container), position-independent (ids, never pointers), pinned by the
   existing context-hash (a format change → rebuild the forest, which the build
   step does anyway).
2. **Bind = LOAD + restore + reconnect.** Restore
   `datatype_map`/`struct_map`/`namespace_datatype_map`/`template_map`/`funcdef_map`
   from the serialized decl graph; reconnect Tree-1 patterns; PP-install macros
   (already done — that part of B4a/B4b was correct). No parse.
3. **Compile.** Parse the user TU against the restored symbols; `tsubst` the used
   Tree-1 patterns → Tree-2 → c2mir. (This is the already-working Phase 1–5 path,
   now fed from a *loaded* Tree-1 instead of a live one.)
4. **Delete** the token-slice (B4a) + re-parse (B4b) infrastructure once step 1–3
   cover the corpus (the two-tree §5 deletion discipline; `-Wunused-function`
   confirms the cut).

## 4. THE one open sub-decision (your call — it's the deep part §4 always flagged)

*How* to get Tree-1's `DataDef` graph across the process boundary:

- **(a) Direct typed decl records.** Serialize each `DataDef`/`FuncDef`/
  `TemplateDef`'s fields as wire format; rebuild the objects + symbol tables on
  load. Fastest load, most direct. Cost: the decl object graph becomes a wire
  format (drift with parser changes) — mitigated by the context-hash pin (drift =
  forced forest rebuild) and by the corpus being a single curated set (no
  cross-module identity/merge problem). **This is Rung 1 of the substrate vision**
  ("move `datatype_map`/`DataDef`s onto the mmap-serializable type-id primitive").
- **(b) Enrich the frozen tree + rebuild-from-AST.** Make the freeze capture the
  high-level structure lowering currently drops (methods/access/bases/template
  bodies), then reconstruct `DataDef`s by walking the thawed tree on load. Less
  duplication of "what a decl is," but it re-adds to the freeze the exact
  high-level info that lowering discards, and the reconstruction walker is itself
  substantial.

**Recommendation: (a).** It's the vision's own Rung 1, the pin handles the drift
risk, and it's the most direct "load, don't re-derive." But this is the load-
bearing decision, so it's yours to confirm.

## 5. Sequencing — smallest gated vertical first (no big-bang at the last lap)

1. **Prove the loop on ONE simple decl** (a file-scope `typedef`, then a free
   function): serialize → fresh process → load → user code that uses it resolves
   + runs, output == g++. This exercises serialize+restore end-to-end on the real
   container with the smallest surface.
2. **`DataDefSTRUCT`/`DataDefCLASS`** (members + access + offsets + methods + bases
   + vtable).
3. **`TemplateDef` patterns** + `tsubst` from a *loaded* Tree-1.
4. **Flip forest to default + delete the re-parse path.**

## 6. GATE (every commit — the proven safety net; two-tree §8)

Correctness only, never perf-gate:
- `make -C src` clean, no new warnings.
- `make -C src fulltest` green at the current baseline (679/0/0/16 + all unit
  suites + oracles + `forest_selfexe_gate`).
- gcc.c-torture failset byte-identical to the 51-name baseline (0 timeouts).
- `--emit=c11` byte-identical on the representative TUs.
- **Cross-process:** freeze in one process, load+compile+run in a FRESH process,
  output == g++ (the B3 discipline, extended to the parser-resume path).

## 7. Retirement (what B4a/B4b leaves behind)

- **DELETE:** the token slices (`SNAP_KIND_CIR_UNIT_TOKENS`), the decl-index-as-
  token-ranges (`SNAP_KIND_CIR_DECL_INDEX`), `parse_forest_slice`, and the cascade
  hooks in `find_template`/`lazy_resolve_type`/`resolve_namespaced_type_token`
  (B4b branch, unmerged).
- **KEEP (correct + reusable):** PP-export restore (`SNAP_KIND_CIR_PP_EXPORTS`) +
  edges + branch-macros (macro restore + DAG walk are "load, not re-derive"), the
  container format + pools + `typeid→name` closure, the `CirFrozenForest` reverse
  directory (`find_unit`), the `--forest-bind` flag, and forest-open-at-parse-time.

---

## Acceptance (what "Phase 6 done" means)

A stock binary with the header-forest appended compiles a real
`<iostream>/<string>/<vector>` program **by loading Tree-1 and instantiating** —
zero re-parse of any packed header — output == g++, `--show-stats` header cost
collapsed to load + `tsubst`, every gate in §6 green in a FRESH process.
