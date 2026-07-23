# Forest B3 — SLICE 1e/1f plan: aggregate + FuncDef write-through

**Date:** 2026-07-06
**Status:** READY TO IMPLEMENT (ground truth probed; hook points pinned)
**Prereqs DONE + pushed (origin/develop @ `5baf2c0f`):** 1a schema (`f0b21e0e`), 1b/1c pointer
write-through (`70d08752`), 1d reference+const write-through (`5baf2c0f`).
**Read first:** `docs/plans/2026-07-06-forest-b3-record-layout-DESIGN.md` (record schema),
`docs/plans/FOREST-SUBSTRATE-READ-FIRST.md` (RESUME banner), memory
`feedback_forest_load_never_reparse` (B3 SLICE blocks).

---

## 0. Where we are (one paragraph)

`Program` owns a `madc::dis::DefArena forest_arena` + a runtime `bool forest_arena_enabled`
(default OFF → `bin/madc` unchanged). The three **unary** derived-type funnels
(`getPointerType`/`getReferenceType`/`getConstType`) each write-through a `DK_PTR`/`DK_REF`/
`DK_CONST` record via `Program::forest_arena_record_unary(DataDef*)`, which stamps the derived
type a project id (its arena slot) and stores its operand as a **serialized type-id** (via the
one `forest_serialize_type_id` policy). Reads still go through the live DataDef fields
(`base_type` is the read-cache); the record is dual-populated, consumed by nobody yet. 1e/1f
extend the same pattern to the **aggregates** (struct/class) and **FuncDef** — the bulk of the
~548-site conversion. Then a later slice flips READS onto the record and deletes the
per-category freeze.

## 1. The model for aggregates (how 1e differs from 1c/1d)

A pointer is *created complete* (getPointerType = one call), so 1c/1d hook at creation. A
struct/class is *built incrementally* (members added, then finalized; class layout computed;
vtable groups built), so **1e hooks ONCE at the completion point**, when the aggregate's full
state is known.

**Per-aggregate, NON-recursive.** Unlike `test_cir_arena`'s `arena_ensure` spike (which
recursively encodes children to prove the schema), production records **each aggregate at its
own completion**. A member/base/method whose type is another project aggregate is stored as a
**type-id** (via `forest_serialize_type_id`) — NOT recursively recorded. That other aggregate
records itself when ITS parse completes. The id-addressed arena tolerates a cross-ref to a
not-yet-written slot (it fills in later). So there is **no recursion and no `seen` set** in
production — simpler than the spike. Resolve-first still applies *within* one aggregate: resolve
all child type-ids into a local vector FIRST, then append each contiguous payload run (members,
bases, methods, vbase pairs, vgroup recs + slot-id runs), so nested runs never interleave.

## 2. Hook points (pinned)

- **Struct** — `TokenSTRUCT::parse` (parser.cpp:22843): after `dds->finalize()`
  (**parser.cpp:23982**, "round up size to struct alignment"). Confirm the hook covers all
  three registration branches (self-registered ~24031, forward-declared completion ~24058,
  plain registered ~24069). If the forward-completion path fills an *existing* struct without
  re-running the 23982 finalize, add a hook there too — trace before wiring.
- **Class** — `TokenCLASS::parse` (parser.cpp:25543): immediately after the layout trio
  **parser.cpp:27134–27136** (`ddc->compute_layout(); ddc->apply_member_layout();
  ddc->build_vtable_groups();`). This is the single point where members/offsets, `bases`,
  `vbase_offset`, and `vtable_groups` are all final. One hook, one call.

Each hook: `if (forest_arena_enabled) forest_arena_record_aggregate(dds_or_ddc);`

## 3. The method to add

`void Program::forest_arena_record_aggregate(DataDefSTRUCT *sdd);` — defined in **madc_cir.cpp**
(next to `forest_arena_record_unary` + the static `forest_serialize_type_id` it reuses). It
mirrors the **aggregate branch of `test_cir_arena.cpp`'s `arena_ensure`** (already schema-proven,
incl. all three flattenings), but non-recursive:

1. `tid = type_id_for(sdd)` — the aggregate's project id (its arena slot).
2. Build a `defrec`: kind = `DK_CLASS` (if `dynamic_cast<DataDefCLASS*>`) else `DK_UNION`/
   `DK_STRUCT`; `name_id`/`canon_id`/`size`/`datatype`/`ns_id`; the struct layout scalars
   (`pack`/`max_align`/`tag_explicit_align`) + flags (`DF_UNION_LAYOUT`/`DF_IS_COMPLETE`/…).
3. **Members** run (`memberrec`): resolve every `members[i].second` to a type-id via
   `forest_serialize_type_id` FIRST, then append the run (name_id, type_id, typedef_id, offset,
   count, flags). Complete the member fields the schema already has (bitfield, origin, dims,
   vbase_idx, explicit_align, default-init/count-expr node refs) as coverage grows — the DESIGN
   doc §3.1 lists them; start with the subset the existing freeze serializes and widen.
4. **Class-only** (when `DataDefCLASS`): flags (`DF_HAS_VTABLE`/`DF_HAS_VPTR_SLOT`/
   `DF_FROM_SYSTEM_HDR`/`DF_HAS_USER_CTOR`/`DF_HAS_USER_DTOR`), layout scalars
   (`nvsize`/`class_align`/`own_block_off`), then the four runs from the spike:
   - **bases** (`baserec`): base type-id + offset + flags (virtual/primary/access).
   - **methods** (`methodrec`): name_id + `func_id` = `forest_serialize_type_id(methods[i]->type)`
     (the method's FuncDef, whose OWN record is populated in 1f — cross-ref by id now).
   - **vbase_offset** (`vbaserec`): flatten the pointer-keyed `std::map<DataDefCLASS*,size_t>` to
     a sorted `(class_id, offset)` run.
   - **vtable_groups** (`vgrouprec` + slot-id run): two-level slice — append each group's slot
     name-id run, then the `vgrouprec` run.
5. `forest_arena.set_def_at(tid, r)`.

Reuse `forest_serialize_type_id` for **every** cross-ref (member/base/method/vbase/vgroup-owner)
— one policy, no parallel encoder. The named-keyed maps (method_map, type_aliases, …) are cold
and come in a follow-on; start with the structural graph the spike + existing freeze already cover.

## 4. Gating

- **test_cir_arena** — a new PARSE-DRIVEN case (this is the honest test of the HOOK, not just the
  method): tokenize+parse a small program defining a struct + a class (with a base, a member, a
  method) with `forest_arena_enabled = true`, then assert `prog.forest_arena` has a `DK_STRUCT`
  and a `DK_CLASS` record at the aggregates' project-id slots, with the member/base/method runs
  populated and cross-refs = the operands' type-ids. (Pattern for parse: see
  `test_cir_freeze.cpp` `build_two_file_module` — `make_shared<Program>()` → `tokenize` →
  `parse`.) Optionally also a direct-call case on a manually-built class (like the existing
  DK_CLASS schema case) for the encoder in isolation.
- **fulltest** must stay **680/0/0/16**; all forest gates **byte-identical to live** (flag off ⇒
  zero behavioral change ⇒ byte-identical by construction). No new warnings.
- Commit as `feat(forest): B3 slice 1e — aggregate write-through (DK_STRUCT/DK_CLASS)`.

## 5. Risks / subtleties to trace before editing

- **Struct forward-completion path** (~24058): does it re-finalize (so the 23982 hook fires) or
  fill an existing struct in place? If the latter, its own hook is needed. TRACE first.
- **Re-recording:** confirm an aggregate is completed **once** (not re-finalized / re-laid-out
  later). If a class can be re-`compute_layout`'d (e.g. template instantiation, late completion),
  the hook fires again → `set_def_at` overwrites the same slot (idempotent, fine) — but verify no
  half-built intermediate is recorded.
- **Methods reference FuncDefs** not-yet-recorded until 1f — fine, `methodrec.func_id` is just a
  type-id; the `DK_FUNC` record fills in during 1f. No ordering constraint (id-addressed arena).
- **Anonymous sub-aggregates** (`anonymous_aggregates`) + the name-keyed maps are follow-on
  coverage — the existing freeze handles them (v11); mirror that AFTER the structural graph lands.
- **Polymorphic vtable / typeinfo** stays an explicit separate boundary (per the memory) — not
  folded into 1e.

## 6. After 1e — 1f + the flip

- **1f — FuncDef → `DK_FUNC`.** FuncDef is the one class with **no builder funnel** (writes are
  scattered across the 6 hot functions). Add a thin `FuncDefBuilder` (DESIGN doc §4c) OR hook the
  FuncDef-completion points; record return + params run (schema already proven). This is where the
  ~64 `parameters` mutations concentrate.
- **The flip (later slice, NOT 1e/1f).** Once all write-throughs are in + tested, switch the
  freeze to DUMP `forest_arena` (already populated during parse) and LOAD to `mmap` + bind, then
  DELETE `materialize_types` + `cir_forest_fill_type_records` + the per-category restore + the 3
  delete-segments, and flip READS onto the record. `#23` whole-TU byte-identity closes by
  construction. This is the payoff — gate hardest here.

---

*Ground truth probed 2026-07-06: hook points parser.cpp:23982 (struct finalize) + 27134–27136
(class layout trio); encoder reference `cir_forest_record_aggregate` madc_cir.cpp:1138; schema +
traversal proven by `tests/unit/test_cir_arena.cpp` `arena_ensure`. Guard = the runtime
`forest_arena_enabled` flag (owner-approved over #ifdef; parser.o is shared).*
