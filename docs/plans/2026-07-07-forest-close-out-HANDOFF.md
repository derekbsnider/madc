# FOREST CLOSE-OUT — the ordered worklist to CLOSE the embedded-header-forest plan

**Date:** 2026-07-07 · **Owner directive:** bring
`2026-06-22-embedded-header-forest-execution-plan.md` to a CLOSE. It has blocked
every other madc track for five weeks. This document is the execution order; the
`FOREST-SUBSTRATE-READ-FIRST.md` RESUME banner stays the mechanism reference.
**Read both IN FULL before touching anything. Do not re-derive either.**

---

## DEFINITION OF DONE (the plan's own bars — when these hold, the plan CLOSES)

1. **Phase-3 gate (from the plan, verbatim):** real `<iostream>` / `<string>` /
   `<vector>` compile + run == g++ with the forest mmap'd; `madc -dM` macro
   parity vs `gcc -dM`; SMAUG C89 soak unaffected.
2. **Owner's bar:** real tests run on the forest —
   `bin/madc --freeze=S tests/testsubscript.mad` then
   `bin/madc --forest-bind=S tests/testsubscript.mad` == live == `.expect`.
3. **Phase 4:** build-time pack of the stdlib closure under the pinned config →
   **append to `bin/madc`** (machinery exists: B3 + `/proc/self/exe` +
   `forest_selfexe_gate`) → compiles bind from the embedded forest **by
   default, no flags**.
4. **Lazy defrost (standing plan, 06-22 + 06-13 docs):** only what a compile
   NEEDS materializes. Make the ONE restore path demand-keyed — the eager
   whole-container `forest_restore_decls` defrost is the gap. NO parallel lazy
   path beside the eager one.
5. **The number:** measured compile-time win on the `--project` SMAUG 51-TU
   build + `--show-stats` before/after, recorded in the plan doc, which is then
   stamped CLOSED.

## STATUS AGAINST THE PLAN (evidence, 2026-07-07)

- Phases 0–3 mechanism: **LANDED** (B1/B2/B3; append-to-binary + selfexe gate
  green; context-hash pin in). Phase-2 oracle exceeded: whole bound-`<string>`
  TU **byte-identical** to live (#23). New-specialization instantiation from
  restored tokens **works** (`vector<long>` from a `vector<int>` producer,
  `t=42 n=2` == live == g++, gate `[vecnewspec]`, 14/14).
- **`<map>` — GREEN (v22, 2026-07-07 second sitting): exact-match bind AND a
  new `map<long,long>` specialization run == live == g++ (gates `[mapbind]` +
  `[mapnewspec]`).** The first sitting's 4 fixes (wip @b2903b72: class-scope
  flush re-run; local_emit_name invariant; incomplete-empty `_Tuple_impl_1`;
  funcdef_files origin classification) + the v22 batch. **The "freeze-time
  body completeness" hypothesis was WRONG** — the snapshot already carried
  every needed body (or_probe showed `__o7` with `body=1`); all three
  undefined imports were LOAD-side:
  1. `cir_freeze.cpp` dropped every DF_IS_MEMBER_TEMPLATE methodrec ("the v6
     rule") — so the bodied instantiations (pair/tuple ctor `__oN`) AND the
     decl-only placeholders never restored; now VERBATIM at their saved ranks.
  2. The CIR_TMPLK_MEMBER flush re-ran register_skipped_class_template_function,
     re-MINTING a rank-shifted placeholder family (`__o6/__o7/...` collided
     with the restored ranks). Now it HYDRATES the restored placeholder
     (funcdef_map[record key]) via the extracted stamp_member_template_pattern
     — the ONE derivation shared with the live registration; full re-run stays
     as the missing-placeholder fallback.
  3. `_Auto_node`'s dtor is referenced ONLY via a scope-local's cleanup
     attribute inside a LOADED body — live registers the dtor at the lowering
     site a pre-built body never runs. `cir_collect_cleanup_attr_fns` now
     collects cleanup refs at the two FOREST materialization sites (live
     collector untouched — torture byte-identical by construction).
  4. (newspec) `'piecewise_construct' is not a member of namespace 'std'` —
     the restored global lacked its NAMESPACE binding; v22 records ns_id on
     cir_forest_global_record and the flush reproduces live's
     namespace_map[ns][name] registration. Format v21→v22.
  The 1b "has NO func-def in the partition" diagnostic remains useful but the
  151 listed symbols are mostly never-ODR-used funcdef entries that cleanly
  lack — NOT the map blocker. KNOWN NUANCE (measured, not blocking): a fresh
  map<long,long> instantiation binds pair(...)__o7's reference params to the
  derived node pointer without live's materialized base-ptr conversion temp —
  2 benign c2mir pointer warnings on the newspec bind (stdout == live == g++;
  live is warning-free). Same residual class as vec's proto/label numbering;
  fix = restored-method param-type fidelity at the construction site.
- **`<iostream>` FAILS — polymorphic classes** (the load selection still
  excludes DF_HAS_VTABLE/vptr/vbase classes — the v6 rule). Closing it =
  serialize/restore vtable + typeinfo + virtual-dispatch state from the arena
  records (the records already store vgroups/vbase offsets; the LOAD selection
  and the emission-side state are the work). This is the LONG POLE.

## EXECUTION ORDER (strict; one batch each; do not reorder)

1. **MAP:** burn down `pair<>` on `tmp/s4_map_prod.cpp` until exact-match bind
   == live; then a newspec map consumer (`map<long,long>` from an int
   producer); add gate case `[mapbind]` (+ newspec variant) to
   `forest_bind_gate.sh`.
2. **IOSTREAM (polymorphic):** widen the load selection + state for
   vtable-carrying classes; reducer = freeze/bind
   `#include <iostream> int main(){ std::cout << 7 << std::endl; }`; gate case
   `[iobind]`. Success flips `tests/testsubscript.mad` (owner's bar test).
   **MEASURED STARTING POINT (2026-07-07, reducer tmp/s5_io.cpp + tmp/s5_io.msnap):**
   the FREEZE already handles the whole 185-unit `<iostream>` corpus (34,236
   records); the bind fails at parse with `use of undeclared identifier 'cout'`.
   Two known state families behind it: (a) `ostream`/`ios_base` etc. are
   DF_HAS_VTABLE classes — the load selection (cir_freeze.cpp materialize, the
   v6 polymorphic exclusion) fences them; the arena records ALREADY carry
   vgroups/vbase offsets, so the work is load-side selection + whatever
   emission-side vtable/typeinfo state a live parse holds; (b) `std::cout` is a
   vfEXTERN global (a reference to the libstdc++ object, NOT a header-defined
   definition) — `cir_forest_fill_globals` explicitly skips vfEXTERN
   (madc_cir.cpp ~1196), so extern-global references are their own (small)
   restore category. Burn down with the map discipline: reducer-iterate,
   fix = state into the substrate, gates ONCE per batch.
   **IMPLEMENTATION MAP (traced 2026-07-07 — the save side is ALREADY COMPLETE;
   the load is mechanical):** the aggregate encoder (madc_cir.cpp ~929-1041)
   records own_block_off, the vbase run (vbaserec: class_id/offset, sorted),
   and the vgroup run (vgrouprec: owner_id/this_offset/slot-name-id run/
   addr_point — mirrors DataDefCLASS::VtableGroup datadef.h:895 exactly).
   LOAD work: (1) drop the vtable/vptr/vbase exclusions in the closure builder
   (cir_freeze.cpp ~1354-1367; keep DF_UNION_LAYOUT fenced); (2) pass-2 class
   restore adds own_block_off + has_vtable/has_vptr_slot (from flags) +
   cdd->vbase_offset[swizzled class]=offset + rebuilt vtable_groups (slot names
   from the pool) — base-before-derived ordering already covers virtual bases;
   (3) the extern-global category: a vfEXTERN class-typed global records as a
   reference (new CIR_GLOBALF_EXTERN_REF gflag bit, still v22 — v22 shipped
   this session, no container predates it), and the flush rebuilds live's
   extern-cout Variable shape (MEASURE live's cout Variable first: flags +
   storage_alias_name/Itanium binding — do not guess); (4) reducer to `7` ==
   live == g++, gate [iobind], batch gates once. For `cout << 7` the calls are
   non-virtual mangled-direct into libstdc++ (operator<<(int), endl) — madc-
   generated virtual DISPATCH is NOT on the reducer's path; vgroups/vbases are
   restored for state fidelity, not exercised by this reducer.
   **PROGRESS (2026-07-07 tail of session — steps 1+2 LANDED, gated):** the
   closure fence is dropped (vtable/vptr/vbase classes admitted; union-layout
   stays fenced) and pass 2 restores has_vtable/has_vptr_slot/own_block_off +
   vbase_offset + vtable_groups (get_word for slot-id runs). Gated: bind gate
   16/16 (strbind/strops whole-TU byte-identity UNREGRESSED — the widening
   reaches every bind case), test_cir_freeze 31/546, test_cir_arena 11/316.
   RESULT: polymorphic classes DO restore (ios_base__failure with base +
   methods), BUT the core trio — ios_base / basic_ios<char> /
   basic_ostream<char> — is STILL dropped by the member-chain fixpoint, so
   `cout` stays undeclared. **THE BLOCKER IS MEASURED (the new permanent
   `materialize closure: DROPPED <name> (member <m>)` -v diagnostic, the
   load-side twin of the freeze-completeness probe): ONE member family —
   FUNCTION-POINTER members — blocks the whole hierarchy.** ios_base drops on
   `_M_callbacks` (`_Callback_list*` whose `_M_fn` is a fn-ptr), and
   basic_ios/basic_ostream/basic_istream/basic_iostream all drop on the same
   chain; also `__jmp_buf_tag.__jmpbuf`, pthread cleanup structs (`__routine`),
   `_IO_cookie_io_functions_t.read`. Fn-ptr TYPES have no arena write-through:
   the getPointerType funnel records object pointees, but a pointer whose
   pointee is a FuncDef has no record → forest_serialize_type_id fails → the
   member chain fails. FIX SHAPE (state into the substrate, machinery exists):
   serialize a fn-ptr type as DK_PTR with ref0 = a DK_FUNC record of the
   signature (forest_arena_record_func already encodes FuncDefs); find where
   fn-ptr member DataDefs are born (the DataDefFUNCTIONptr/fn-ptr funnel) and
   write-through there; load-side arena_chain_ok + swizzle then resolve it
   like every derived type. THEN the extern-global category (step 3). freeze+bind every `tests/*.mad` (scripted, capped, ONE
   run); classify remaining failures by state family; burn down by class, not
   per test.
4. **PHASE 4 PACK + DEFAULT-ON:** corpus freeze driver (stdlib closure under
   the pinned config), append to the binary, bind-by-default from
   `/proc/self/exe` when no explicit forest is given; `-dM` parity check;
   fulltest + torture + SMAUG soak green with it ON.
5. **LAZY DEFROST:** make restore demand-keyed (name/unit-lookup-driven
   materialization; decl index + id-addressed arena already support it).
6. **MEASURE + CLOSE:** `--show-stats` and SMAUG 51-TU wall before/after;
   record in the 06-22 plan; stamp it CLOSED; release per cadence.

## DISCIPLINE (the rules that made 2026-07-07 produce six fixes in one day)

- **Batch. Reducer-iterate. Full gates ONCE per batch** (bind gate + the two
  unit suites + one fulltest at the end). NEVER per-tiny-change retests.
- **Fix shape: move the missing state INTO the substrate** (arena records /
  intern pool / the existing template-record segments). NO new bespoke record
  family, NO new phase/slice vocabulary, NO parallel implementations.
- Loaded state must EQUAL parsed state — never re-parse / re-derive /
  hand-order output. When restore needs live registration side effects,
  RE-RUN the one live registration function over restored tokens (the
  register_skipped_class_template_function precedent), never a copy of it.
- Repo hygiene: never `git add -A` (`mir-debug-support.md` is not ours); no
  `&&` chains; `ulimit -t` + `timeout` on every run; ONE heavy job at a time;
  commit `-F` with the standard trailers; push every green batch.
- After any compaction: read THIS doc + the READ-FIRST banner in full before
  the first edit. If tempted to redesign, STOP — it was all in the plan.

## TRIPWIRES (each of these burned a day at least once)

- Diagnostics MISATTRIBUTE the file (consumer path + header line). Trust the
  line/col + `-v` context, not the filename.
- `strings` on a snapshot proves nothing (segments are compressed) — probe
  with the real reader (`tmp/tmpl_probe.cpp`, build line in the banner).
- Unit-test binaries go stale — relink before trusting, never mid-fulltest.
- `"\x01fn-template-placeholder"` parses as `\x01f` + "n-…" — match the
  existing literal byte-for-byte.
- The `.madh` token codec is shared by include-PCH and forest runs; the PCH
  replay's keyword re-promotion can MASK codec infidelity — test via forest.
