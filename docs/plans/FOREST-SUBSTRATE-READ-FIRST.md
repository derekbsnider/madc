# FOREST / DATA-SUBSTRATE — READ THIS FULL DOCUMENT BEFORE DOING ANYTHING ELSE

**⚠️ STOP. If you are about to touch anything under the forest / freeze / bind /
type-table / substrate surface — or you just resumed after a compaction — read
this ENTIRE document first. It is self-contained on purpose. Do NOT start from
the scattered plan docs; they are background, linked at the very bottom. This one
file is the source of truth. Skimming this file is how every serious mistake in
this campaign happened.**

**Date:** 2026-07-05 · **Status:** AUTHORITATIVE, single-source. Supersedes the
"read-order" section of `2026-07-05-forest-phase6-HANDOFF.md`.

---

## 0. The five things that, if you get them wrong, you will cause damage

1. **madc NEVER LOWERS.** There is no lowering pass. The `cir_node` tree is the
   complete, high-level thing.
2. **`cir_node` *contains* c2mir's `node_t` as its base.** c2mir only reads the
   `node_t` fields — a keyhole. Everything else on the `cir_node` (the DataDef
   graph, tokens, provenance, the whole high-level structure) is **invisible to
   c2mir, NOT absent.** Tree-2 is handed to c2mir **unlowered**; c2mir just can't
   see the rest.
3. **Tree-1 (the forest ROM) has EVERYTHING.** If the freeze drops something,
   the freeze is BROKEN — fix the freeze, do not "reconstruct" or "re-derive" the
   dropped thing on load.
4. **There is ONE serialization machinery** (`madc::dis` snapshot container over
   `intern_table` / `arena` / `id_table` / `value_pool`). Never hand-roll a
   second serializer or a parallel format beside it.
5. **Serialize pointers AS IDS; swizzle back to pointers on LOAD.** Load never
   re-parses, never re-computes layout, never re-derives. It reads stored values
   verbatim and relinks ids → pointers.

If any change you are about to make violates one of these, it is wrong no matter
how small.

---

## 1. The mental model (the two trees)

- **Tree-1 — immutable ROM.** Parsed ONCE: every template pattern **and the whole
  header corpus**. Never mutated, never handed to c2mir. It is (a) the COPY
  SOURCE for template instantiation and (b) **the serialized form = the
  header-forest.** g++ analogue: `DECL_SAVED_TREE`, generalized to the corpus.
- **Tree-2 — mutable, per-TU.** Built by `tsubst` = **copy a Tree-1 subtree into
  FRESH `node_t`s + substitute template params.** Handed to c2mir, which writes
  its own sema (`node->attr`) onto the `node_t` base in place. NEVER re-parsed.

Parse-once is DONE and RELEASED (v0.33.0): templates instantiate by copy+subst,
never re-parse. That is what makes freezing Tree-1 *sufficient* — a loaded forest
can instantiate with no token text.

**c2mir hard constraint (why Tree-2 is fresh):** c2mir writes `attr` on every
`node_t` it traverses, so each instantiation needs its OWN `node_t` structure
(copy, never splice a live Tree-1 node_t; two instantiations never share one).
The back-reference to Tree-1 rides in the `cir_node` EXTENSION (invisible to
c2mir). "Some ROM, some RAM": the heavy madc info stays ROM in Tree-1 and is
*referenced by id*; only the `node_t` base + substituted fields are fresh RAM.

---

## 2. The one substrate (already built — USE IT, do not reinvent)

All of these are index-based specifically so they serialize / mmap **with zero
fixup** (dump the block, map it back, ids still valid):

- `include/madcdis/intern_table.h` — index-linked (NOT pointer-chained) string/
  identifier table: three contiguous blocks (byte arena / entry array / bucket
  array); the `id` IS the entry index; collisions chain by index. A
  `frozen_intern_table` gives a read-only view over the three serialized blocks.
- `include/madcdis/arena.h` — bump allocator; pointer-stable; drop-all on
  destruction. `TokenArena` HAS-A `arena`.
- `include/madcdis/id_table.h` — segmented stable `uint32`-id ⇄ object* registry.
  This IS the compiler's **type table** (`Program::type_id_for` / `type_from_id`,
  `project_types`). Interface: `add(T*)→id`, `get(id)→T*`, `base()`, `size()`.
- `include/madcdis/value_pool` — deduping `uint32` handles over wide-literal
  limbs (128-bit literals etc.). Same three-block zero-fixup discipline.
- `include/madcdis/snapshot.h` (`src/madcdis_snapshot.cpp`) — **THE container:**
  header / 16-aligned segment frames (zstd/zlib per-segment) / directory / footer
  (magic + directory offset). Two placements: a standalone file, or **appended to
  the `madc` binary** (footer read from EOF via `/proc/self/exe`). This is the
  ONE format family for the forest blob, the `.madh` PCH, and `snapshot://`.

`cir_freeze.{h,cpp}` sits ON TOP of this: it flattens the live `cir_node` DLIST
tree into POD records + a child-index pool and stages them as snapshot segments.

---

## 3. The type table (the DataDef identity + serialization spine)

From `docs/plans/2026-06-12-type-table-value-abi-design.md` §2 (the doc I skipped
that caused the confusion):

```
id 0                  invalid / "no type"
[1, 0x100)            primitives — pinned ABI constants, process-invariant,
                      resolve in ANY process with no table
[0x100, 0x01000000)   SYSTEM segment — "types from the embedded system forest,
                      frozen at forest build time, identical across runs."
                      THIS SEGMENT IS THE FOREST'S.
[0x01000000, 2^32)    project segment — per-Program user typedefs/structs/
                      classes/enums; deterministic registration order
```

- `uint32 typeid` is the canonical type identity. Every `DataDef` carries
  `type_id` (`datadef.h`), stamped at registration.
- §6.4 (a *later campaign*, i.e. THIS work): **"P4 forest type-refs serialize as
  ids + table segments."** §2 AOT constraint: **"serialize the table so ids
  survive a save/load round trip."**
- **So the DataDef graph is serialized by serializing the TABLE**, with every
  pointer field written as an id (member/base type → typeid; names → intern
  handle) and swizzled back on load. NOT by a bespoke format, NOT by re-deriving
  from the lowered view (there is no lowered view — see §0.1/§0.2).

---

## 4. What is already built and gated (do not redo)

- **Track A (substrate spine), DONE:** `frozen_intern_table` + the snapshot
  container (`62c1b91b`); `value_pool` + 128-bit literals (`7d7c0e5d`); wide-fold
  spine (`956e7030`).
- **B1 (`0b1e618a`):** every `cir_node` EXTENSION pointer is now position-
  independent — `datadef` → `datadef_id` (typeid), `char*` → intern handle,
  `tree1_origin` → `cir_ref{seg,idx}` behind the one `madc_cir_node_for(ref)`
  resolver. Raw pointers remain ONLY in the c2mir-visible `node_t` base (op links,
  `u.s`, `attr`), which map to handles at freeze time. **This is the "cir_node
  half" of the swizzle. It proves the pattern.**
- **B2 (`b62089ad`):** single-segment freeze/thaw — flatten the tree to POD
  records + child pool → snapshot segment → load → resolve-on-touch →
  materialize real pointer-linked `cir_node`s at the c2mir edge.
- **B3 (`75830eec`):** multi-segment forest, append-to-binary, `/proc/self/exe`
  loader, context-hash pin, **cross-process closure** (container carries its own
  intern pool + positions + libs + the typeid→**name** closure). A FRESH process
  runs a frozen program with no parse. `--run-frozen` MEASURED 20× vs live.
- **B4a (`54aff2ce`):** grove payload v2 (per-unit token slices, a decl index,
  PP-export events, include edges) + pack-time recording + oracles. **NOTE: the
  token-slice + decl-index-as-token-ranges parts are the RE-PARSE approach and
  are being retired — see §6.**

**The gap B3 explicitly left:** the typeid→**name** closure serializes each
DataDef's id and NAME and **nothing else** (`cir_freeze.cpp`, the `type_names`
loop). So cross-process `madc_type_from_id` returns NULL for forest types — the
DataDefs' members/offsets/bases/methods/vtable were **left behind.** That is the
bug this campaign fixes (§5).

---

## 5. THE CURRENT TASK — make the freeze complete (serialize the whole type table)

**Bug:** the freeze drops DataDef content (§4). **Fix:** serialize each project/
system DataDef's COMPLETE content as a type-table segment, pointer fields as ids,
**verbatim**; swizzle on load into real DataDef objects so `madc_type_from_id`
resolves; bind points the parser's symbol tables at them.

**LANDED `1b2d7171` (2026-07-05): the mechanism + typedef/struct/union coverage.**
Format v6, freeze serializes the complete type table (full content, ids), load
swizzles to complete DataDef objects VERBATIM, bind points symbol tables at them;
the parallel `decl_records`/`struct_members` format + `finalize`-regeneration are
retired. Verbatim load means unnamed-bitfield-gap/packed structs now bind
correctly (the old rebuild refused them). Gated green (fulltest 679/0/0/16 +
forest_bind_gate cross-process + torture 50-name byte-identical).

**LANDED (2026-07-05): `CIR_TYPEK_CLASS` — class DATA LAYOUT.** Freeze serializes a
non-polymorphic class's members (verbatim, inheritance-flattened) + direct bases
(`cir_forest_type_base`: subobject offset / access / is_primary) + flags
(`from_system_header`/`has_user_ctor`/`has_user_dtor`) + size/align; load allocates
a `DataDefCLASS` and swizzles base ids → loaded `DataDefCLASS*` verbatim; restore
registers it into `struct_map` + `datatype_map` + a `dkStruct` TopDecl. A class
carrying state this slice does not serialize (a vtable / polymorphic, a union
layout, or a virtual base) is skipped WHOLE → loud error at use, never mis-link.
Two freeze subtleties handled: (a) `TokenCLASS::parse` registers classes in
`struct_map` only (NOT `top_decls`, unlike structs), and (b) lazy id-stamping is
not definition order — so classes are enumerated from `struct_map` via a
dependency-driven **fixpoint** (bases/member-types recorded before their users).
The shared `cir_forest_serialize_members`/`_bases`/`_record_aggregate` helpers back
both the struct and class paths. Gated green (fulltest 679/0/0/16 +
`forest_bind_gate` `class` case: MI hierarchy binds cross-process, `sum=6 bb=2
sz=12` == live == g++, incl. nonzero base offset + upcast; `test_cir_freeze` 17/17).
**LANDED (2026-07-05): class METHOD DECLARATIONS (slice 3d, format v7).** Freeze
serializes each non-virtual method's decl — mangled call name (`Counter__get`),
display name (`get`), return typeid, explicit-param typeids, `emit_symbol`, and
flags (const/varargs/void-params/static) — into the type record (`method_begin`/
`method_count` + `cir_forest_type_method`). Load rebuilds a `FuncDef`+`Variable`,
reconstructing the hidden `__this` (param 0 of a non-static method) as a pointer to
the owning class, and attaches it to `method_map`/`methods`; `forest_restore_decls`
needs no change (methods ride the class object). Ctors/dtors/operators/templates and
methods with an unserializable return/param are skipped individually (not bailing
the class). A member call on a bound class now **RESOLVES**. Gated: `test_cir_freeze`
slice-3d unit test (get/add reconstruct with correct signatures + `__this`);
`forest_bind_gate` still green; fulltest green.

**The linking layering (the runnable payoff — NEXT):** a resolved call still needs a
DEFINITION.
- **External / system methods** (the corpus): `emit_symbol` names a real libstdc++
  Itanium symbol — the decl alone links (no body). BLOCKED on serializing the corpus
  classes at all, which need **pointer-member serialization** (std::string/vector have
  pointer members; the current member pass bails on non-primitive/non-recorded types).
  So: pointer-member serialization → the corpus classes serialize → their method
  decls link externally.
- **Inline user methods**: the body was emitted by the PRODUCER into the grove (e.g.
  `int Counter__get(Counter*){...}`), but `--forest-bind` currently emits only the
  consumer's own tree, so a bound inline method is an `undefined MIR import`. Needs
  **grove function-body emission on bind** (pull the bound unit's function-definition
  nodes from the grove into the consumer's c2mir tree — the forest's "load the
  pre-parsed body, don't re-parse" promise).

  **VERIFIED-FEASIBLE DESIGN (2026-07-05, ready to build — the recommended NEXT):**
  - Confirmed: method bodies partition into the HEADER unit (decl-index shows
    `Counter__get`/`add` under `fm.h`, `main` under the `.cpp` unit); a function
    definition is `N_FUNC_DEF`; a module is built with `c2mir_new_node(c2m, N_MODULE)`
    + `c2mir_op_append` (see `CirBuilder::translate_module`, cir_builder.cpp:17770);
    the model is `CirJitSession::build_frozen` (madc_cir.cpp:443 — open forest with
    the session `c2m`, materialize, `cir_compile`, `load_and_link`); multi-module
    linking already exists (madc_cir.cpp:1246).
  - HOOK: in `CirJitSession::build` (madc_cir.cpp:418), after `build_tu_module` and
    BEFORE `load_and_link`, if `prog->forest_chain` is non-empty (bind occurred):
    open a SECOND `CirFrozenForest` on the same image WITH THE SESSION `c2m`
    (`bind_forest` was opened `c2m=NULL` at parse, for decls only — node-tree
    materialization needs a real `c2m`); materialize the bound units' TOP-LEVEL decls
    (types + `N_FUNC_DEF`s); synthesize `N_MODULE(N_LIST(decls))`; `MIR_load_module`
    it so the existing `MIR_link` resolves `Counter__get` (grove-defined) as the
    consumer's import.
  - RISKS to design against: (1) node→unit origin mapping to filter the root's
    `top_list` by bound unit (or iterate each bound unit's segment records) — must
    NOT emit the producer's `main`; (2) the struct type MUST ride in the grove module
    (the body accesses members); (3) cross-module: grove module + consumer module each
    get a module-local `struct Counter`, symbol `Counter__get` defined-once (grove) /
    imported (consumer) — verify MIR links cleanly (THE highest-risk part).
  - GATE: `tmp/fm_consumer.cpp` (a `Counter{int n; int get(); void add(int);}`
    header) binds + runs `get=15` == live == g++; add a `forest_bind_gate` method
    case; fulltest; torture unaffected (forest-only).

Pick either linking path next; both build on the now-landed decl serialization.
The inline path (grove-body-emission) has the fully-worked design above; the corpus
path is blocked on pointer-member serialization first.

NOTE (freeze fidelity, orthogonal): the `struct`-keyword-with-base `sizeof` bug was
FIXED (`6f008d0c`) — see the memory. Residual: exact vbase-diamond member offsets vs
g++ (tail-padding reuse, task #8), no failing test.

Concretely (the shape, for the class widening and any further types):
- `cir_freeze.h`: `cir_forest_type_record` (id, kind, name_id, spelling_id, size,
  align, flags, ref0, member/base slices) + `cir_forest_type_member` (verbatim
  offset/count/access/origin/bitfield) + `cir_forest_type_base`. Replaces the
  name-only closure.
- `cir_freeze.cpp` **serializer:** for each type, dispatch on `basetype()` /
  subclass and write full content — names → intern ids, member/base types →
  typeids.
- `cir_freeze.cpp` **loader:** two passes — allocate all DataDef objects (so ids
  are known), then swizzle (relink member/base typeids → the loaded DataDef*s,
  primitives via `madc_type_from_id`), filling member vectors **verbatim**. A
  forest-local `map<typeid, DataDef*>` is the swizzle table.
- `parser.cpp forest_restore_decls`: register the loaded DataDefs by name into
  `struct_map` / `datatype_map` / `namespace_datatype_map` / `user_typedef_names`
  (and, for emission, `struct_map` — Pass 0.5 iterates it; structs also push a
  `dkStruct` TopDecl for Pass 0). **No rebuild, no `finalize`, no re-parse.**
- **RETIRE:** `cir_forest_decl_record`, `struct_members`, `cir_forest_collect_decls`
  (madc_cir.cpp), `cir_forest_plain_struct_faithful`, and the layout-regeneration
  in the old `forest_restore_decls` struct branch — the parallel format and the
  re-derivation.

**Coverage order (one mechanism, widened — NOT a parallel format each time):**
typedef → struct/union → class (data layout: members verbatim, bases, `has_vtable`
/`has_vptr_slot`/`from_system_header` flags, size/align). Class METHOD-CALL
dispatch needs external libstdc++ symbols (the class's methods bind to real
Itanium symbols; madc emits no bodies for `from_system_header`/`is_externally_defined`
classes) — that is a follow-on after the type/layout is complete, not a blocker
for the type serialization.

---

## 6. Retirement (the re-parse turn that was wrong)

`2026-07-04-forest-default-mode-design.md` chose "frozen token slices + re-parse"
and is **SUPERSEDED** — re-parse contradicts parse-once and §0.5. Retire, once the
type serialization covers the corpus:
- token slices (`SNAP_KIND_CIR_UNIT_TOKENS`), decl-index-as-token-ranges
  (`SNAP_KIND_CIR_DECL_INDEX`), any `parse_forest_slice` / cascade hooks.
- The B4b branch `feature/forest-b4b-bind-claude` (unmerged) — reference only.

KEEP: PP-export restore + include edges + branch-macros (macro restore + DAG walk
= "load, not re-derive"), the container/pools/closure, `CirFrozenForest::find_unit`,
the `--forest-bind` flag, forest-open-at-parse-time.

---

## 7. INVARIANTS (never violate — these are the whole point)

1. **Never re-parse at bind/load.** No `parseStatement`, no token re-lex. A lookup
   miss after load is a load-fidelity bug to FIX, never a re-parse to add.
2. **Never re-derive.** No `finalize()`/`compute_layout()` re-run on load — offsets
   and layout are serialized and loaded verbatim.
3. **Never a parallel format.** One serializer, the substrate. If you find yourself
   adding a `struct cir_forest_*_record` beside the type table, stop — extend the
   table serialization.
4. **The freeze must be COMPLETE.** If it drops something, fix the freeze.
5. **Fix at the deepest layer** (gcc/clang methodology); no name-keyed special cases.

---

## 8. GATE (every commit — correctness only, never perf-gate)

- `make -C src` clean, **no new warnings**.
- `make -C src fulltest` green at baseline **679/0/0/16** + all unit suites +
  ratchets + `forest_selfexe_gate` + the forest oracles.
- gcc.c-torture failset **byte-identical to `docs/parity/torture-failset-current.txt`**
  (currently 50 names after the `__int128_t` fix), 0 timeouts.
- `--emit=c11` byte-identical on representative TUs.
- **Cross-process:** freeze in one process, load+compile+run in a FRESH process,
  output == g++ (`scripts/forest_bind_gate.sh`, wired into fulltest).

---

## 9. Working discipline (repo-specific, easy to forget after a compaction)

- Branch `develop`; commit via `git commit -F -` heredoc; **stage files
  explicitly — never `git add -A`**; the untracked `mir-debug-support.md` is NOT
  ours, never stage it.
- No chained `&&` shell commands; no sleep/poll loops (use `run_in_background`);
  scratch/temp files under `tmp/` (gitignored) or `$CLAUDE_JOB_DIR/tmp`.
- Do NOT `/promote` develop→master.
- Commit trailers: `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>` and
  the `Claude-Session:` line.

---

## 10. Background source docs (NOT required reading — this file subsumes them)

Only open these if you need a detail this file omits:
- `2026-06-23-two-tree-cir-materialize-from-ast-PLAN.md` — §2 two trees, §6 phases,
  the g++ model. Phases 0–5 COMPLETE (v0.33.0); Phase 6 = this work.
- `2026-06-12-type-table-value-abi-design.md` — §2 segments, §6.4 "table segments",
  §2 AOT "serialize the table". **The doc whose §2/§6.4 are the swizzle mechanism.**
- `2026-06-13-embedded-ast-frontend-design.md` — "arena + u32 index handles, no
  internal pointers"; segmented mmap / zero-copy.
- `2026-07-04-data-substrate-first-customer-PLAN.md` — the governing track order
  (A → B → C); §0 "one serialization machinery"; landing history B1–B4a.
- `feedback_forest_load_never_reparse` (memory) — the one-line invariant.

**If this file and a background doc disagree, this file wins — then reconcile.**
