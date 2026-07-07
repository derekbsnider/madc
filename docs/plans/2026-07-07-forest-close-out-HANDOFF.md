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
- **`<map>` — 4 of 5 gaps FIXED (wip @b2903b72, bind gate 14/14 green):** the
  flush re-runs member-template registration INSIDE the owner's class scope
  (class_scope_stack — `pair<iterator,bool>` return resolution); restored
  disambiguated overloads reproduce live's local_emit_name invariant (`_un` /
  `__oN` — a bound `++it` had emitted an undefined canonical symbol); an
  incomplete EMPTY aggregate (tuple's `_Tuple_impl_1` recursion tail) records
  and restores verbatim (payload-less shape only; is_complete verbatim);
  funcdef_files classifies a bodied def by its ORIGIN token's file, not its
  unit. **THE ONE REMAINING map GAP (measured):** 3 undefined MIR imports —
  pair/tuple member-template CTOR instantiations + `_Auto_node` dtor — bodies
  that materialize in a compile phase the freeze's translate never runs, so
  they have NO func-def in the partition. Run
  `bin/madc -v --freeze=... tmp/s4_map_prod.cpp 2>&1 | grep 'arena_complete 1b'`
  — the permanent diagnostic lists all 154 such symbols. FIX = the banner's
  invariant-4 item: FORCE-MATERIALIZE deferred bodies at freeze (the freeze's
  translate must run the same late instantiation passes a compile runs) so the
  partition is complete. This also serves the whole-corpus freeze: a save must
  not depend on which bodies the producer's own main happened to ODR-use.
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
3. **REAL-TEST SOAK:** freeze+bind every `tests/*.mad` (scripted, capped, ONE
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
