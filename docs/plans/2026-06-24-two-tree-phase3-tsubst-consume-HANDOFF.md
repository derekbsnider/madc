# Two-Tree Phase 3 — CONSUME the recipe (tsubst) — HANDOFF

**Date:** 2026-06-24 · **Branch:** `feature/front-end-performance-claude`
**Governing plan:** `2026-06-23-two-tree-cir-materialize-from-ast-PLAN.md` (read §11 + §0 RESUME).
**This doc:** the imperative execute-to-done for Phase 3 — the FIRST slice that delivers a
real win (a template method instantiated by copy+substitute instead of re-parse).

---

## 0. STATE (verify, do not trust blindly — run `scripts/resume.sh`)

➡️ **START HERE — next Codex session (2026-06-26). The direction CHANGED; this block
SUPERSEDES the pack-coverage NEXT-SLICE text below it.**

---
✅ **STEP 3.5 RESOLVED — `3d18eed7` "fix(cir): deref reference-param value reads in tsubst dependent-pattern body" (Claude 2026-06-26). Regression fixed ADDITIVELY; Codex's `bb44557c` gains fully preserved.**

The `bb44557c` flag-on regression (3 tests garbage) is FIXED. **Both gates now 670/0/0/18
(flag-off AND `MADC_XTEST_DEP_PARSE=1`), six canaries green, `test_cir` all pass, zero new
warnings — AND engagement stays 16 hit / 19 fallback (Codex's gains kept).** `testmemtmpl{refparam,fwdrefpack,typedefparam}` → `x=77` flag-on.

THE FIX (`src/cir_builder.cpp`, the reference value-read at the `// A numeric reference
parameter` comment, ~line 9957): while building a dependent-pattern body
(`m_tsubst_pattern_mode`), the body-bound `TokenVar` for a reference parameter has LOST the
`DataDefREF` wrapper (the dependent parse binds the body identifier to a non-reference
Variable, even though the recipe FuncDef still records the param type as a reference —
*verified: `recipe_fd` param is_ref=1 while the body var is_ref=0*). So the normal
reference-read deref (`is_reference() && type->is_pointer()` → `*name`) never fired, and the
body emitted bare `a` → instantiated body `*p = a` (pointer-into-int) instead of `*p = *a`. The
fix adds a SECOND condition right after the normal one: in pattern mode, if the body var is not
itself a reference but matches (by name) a by-reference PARAMETER of `m_cur_method`
(`pv->is_reference() && pv->type->is_pointer()`), deref it. This is the same param source of
truth `arg_spelling` uses in `member_template_method_call`. Address-of (`&a`) is unaffected (it
takes the separate `TokenAddrOf` path, not this value-read). NOTE the documented contract at
`tsubst_method_body` (~13103, "ordinary reference-parameter VALUE reads stay on the parsed-body
fallback") is now obsolete for non-dependent refs — they tsubst correctly; the comment should
be updated when that guard is next touched.

**NEXT (Codex): the -O2 measurement (now unblocked — 670 is restored).** Build
`make -j4 -C src CXXFLAGS="-std=c++11 -Wall -O2"`, then `MADC_XTEST_DEP_PARSE=1 bin/madc
--show-stats tests/testsubscript.mad`: confirm engaged=16 and read the instantiate time vs
flag-off. Per the standing perf finding tsubst may still be net-neutral at -O2 until the
header-forest lands (recipe-build overhead) — that's expected; the point of 3.5 was to make the
16 hits CORRECT so the lever is real. Then resume coverage of the next ranked fallback shapes.

---
📋 **HISTORICAL — `bb44557c` ROUND RESULT (the regression 3.5 fixed; kept for context).** DO step 3.5 BEFORE any new coverage. **(SUPERSEDED by ✅ above — 3.5 is done.)**

Codex landed the step-3 allocator-cluster work as `bb44557c` (extends the GENERIC tsubst
machinery — `resolve_copied_dependent_call`, `copy_cir_subtree`, `tsubst_method_body`,
`tsubst_call_can_rewrite_after_subst`, `pack_value_name_in_pattern`; +40 unit-test lines.
Hardcoding scan CLEAN — no callee-name `== "construct"` matching). Then it walled.

- ✅ **Coverage lever moved (bar half 1 MET):** flag-on `testsubscript` tsubst engagement
  **`6 hit / 29 fallback` → `16 hit / 19 fallback`** (more than doubled hits). The approach
  works — it really engages the allocator cluster.
- ❌ **But it REGRESSED 3 flag-on tests → gate FAILS (flag-on 667/3, not 670).** Root cause:
  the newly-engaged tsubst now reaches member-template **reference-parameter** cases that
  previously *fell back* to re-parse (correct). The tsubst'd body treats a **reference param
  as a raw integer** → garbage. Signature (all 3 want `x=77`):
  - `testmemtmplrefparam` — flag-off `x=77` ✅ / flag-on `x=1287181984` ❌ (warns at 14:17:
    `assigning pointer without cast to integer`)
  - `testmemtmplfwdrefpack` — flag-off `x=77` ✅ / flag-on `x=1278305456` ❌
  - `testmemtmpltypedefparam` — flag-off `x=77` ✅ / flag-on garbage ❌
- ✅ **Production UNAFFECTED:** flag-OFF (the default `bin/madc` path) is clean **670/0/0/18**.
  The regression is confined to the opt-in `MADC_XTEST_DEP_PARSE=1` path. `make -j4 -C src`
  builds clean; `test_cir` 87/1078/4.

**🔬 ROOT CAUSE — FULLY DIAGNOSED & VERIFIED (Claude 2026-06-26, via emit-c diff + instrumented
probes). This is a one-line bug; Codex implements the fix.**

The instantiated SIGNATURE is correct in both modes (`void C__set__mti(int *a, int *p)` — the
`int& a`→`int *a` lowering is fine). The bug is in the **body**, confirmed by `--emit=c11`:
- flag-OFF (re-parse, correct): body is `((*p) = (*a));` — reference param `a` is DEREFERENCED.
- flag-ON (tsubst hit, broken): body is `((*p) = a);` — `a` (the `int*` ref slot) is NOT
  dereferenced → assigns the pointer into `int *p`'s target → c2mir "pointer without cast to
  integer" + garbage.

WHY: the normal value-read of a reference variable dereferences at **`cir_builder.cpp:10007`**
(`if (tv->var.is_reference() && tv->var.type->is_pointer()) return N_DEREF(id(name))`). An
instrumented probe at that line shows, for the body read of `a`:
- flag-OFF: `is_ref=1 type_is_ptr=1 → DEREF` (re-parse binds `a` as a reference) → `*a` ✅
- flag-ON pattern build (`m_tsubst_pattern_mode=1`): `is_ref=0 type_is_ptr=0 → bare` → `a` ❌

So **the tsubst dependent pattern binds a NON-dependent reference parameter (`int& a`) as a
NON-reference variable** (`is_reference()==false`); the deref at :10007 never fires, the pattern
emits the bare pointer id, the copy preserves it. The pattern is built in
`build_dependent_pattern` (parser.cpp ~33712) via `parseFunction(..., dependent_parse_in_progress=true)`
— that dependent parse drops the reference-ness of a non-dependent ref param.

WHY only non-dependent refs (and why the 16 allocator hits are fine): the allocator
forwarding-ref args (`Args&&...`, and the ref-returning `std::forward`/`std::move` shapes) are
lowered by the SEPARATE pack / `ref_returning_call_type` / `ref_param_arg_addr` machinery that
already derefs explicitly. Only a PLAIN value-read of a non-dependent reference (`*p = a`) hits
the unhandled :10007 gap.

**STEP 3.5 (do FIRST, gated): make the tsubst body deref a reference-parameter read — restore
flag-on 670 while KEEPING engagement = 16.** Recommended fix layer (deepest, no special-case):
make `build_dependent_pattern`/`parseFunction`'s dependent parse declare a non-dependent
reference param so `Variable::is_reference()` is TRUE in the pattern — then the existing
`cir_builder.cpp:10007` deref handles it automatically and the copy preserves it. Alternative
(if the parser layer proves too entangled): in the tsubst copy path, when copying an `N_ID`
that names a reference parameter of the INSTANTIATED FuncDef, wrap it in `N_DEREF`. Prefer the
pattern-fidelity fix.

**⚠️ DO NOT "fix" by tightening `tsubst_body_has_unsupported_reference_param` to reject
non-dependent refs (the obvious-looking gate-around). VERIFIED 2026-06-26: removing bb44557c's
`&& tsubst_datadef_involves_template_param(...)` qualifier there fixes the 3 tests BUT drops
testsubscript engagement `16 → 6` — that guard loosening is load-bearing for the allocator
gains (their non-dependent refs DO lower correctly via the pack/ref machinery). The guard is
RIGHT to admit them; the gap is specifically the plain value-read deref above.**

**This round's MEASURED bar status:** engagement UP ✅ / correctness regressed ❌ / -O2 timing
NOT measured (skipped — disqualified at the correctness gate; measure only after 3.5 restores
670). So `bb44557c` is a correct-direction WIP, not a landable result. Tree is at `bb44557c`
(16 hits, gate red flag-on) — Claude restored it clean after the investigation; no WIP left.

---

**TRUSTABLE CHECKPOINT:** HEAD `915923f7` (steps 1–2 below LANDED: engagement counter +
ranked fallback profile; the step-3 coverage attempt was reverted this round). Tree clean.
Gate GREEN both halves at -O0 (re-verified by Claude 2026-06-26): `make -j4 -C src fulltest`
670/0/0/18 + drift gates; flag-on (`MADC_XTEST_DEP_PARSE=1`) 670/0/0/18; `test_cir` 86/1067/4.
Verify with `git log -1` + a smoke test, not a full rehydration.

**⚡ PERF REPRIORITIZATION (profiled 2026-06-26 — read before grinding more coverage):**
- **madc -O2 ≈ g++ PARITY.** `testsubscript` compiles in **0.800s at -O2** vs g++ **0.796s**
  (full `-O0 -c`) / 0.581s (`-fsyntax-only`). The scary "≈4× slower than gcc" was an
  artifact of profiling the **-O0 dev build** (3.1× slower than -O2, by design). Perf is NOT
  a crisis — madc is already at gcc parity on the optimized build.
- **tsubst currently gives ZERO benefit — slightly NEGATIVE.** flag-on vs flag-off at -O2:
  testsubscript 0.800→0.810, testset 0.507→0.519, testvector 0.540→0.558. It does not
  *engage* on real STL (a COVERAGE fact, `-O`-independent); the recipe-build overhead costs
  more than the (nonexistent) savings. **No re-parsing of headers happens** — madc reads the
  2.4MB closure 1.00× (include-once works). The dominant -O0 costs were header lex (48.5%) +
  interning *volume* + instantiation (~30%); at -O2 instantiate is only ~0.32s.
- **CONSEQUENCE:** tsubst only earns its keep once it ENGAGES (instantiation lever) and via
  the **header-forest** (its Tree-1 is the forest foundation; the forest skips the 48.5%
  header lex + ~2800 header-triggered instantiations — the way to go BELOW g++). Both are
  *future* wins; neither is a current speedup.

**🛠 BUILD CONVENTION (serial builds are the 10+ min cause):** ALWAYS build with
**`make -j4 -C src`** (parallel across 4 cores). ⚠️ **Do NOT set `CXX="ccache clang++"`** —
ccache is installed but wrapping `$CXX` corrupts `gen_sys_includes.sh`'s compiler probe →
`Failed to open include file: iostream` (verified 2026-06-26). Wiring ccache *gen-safely*
(compile-only, so `-O0↔-O2` flips become cache hits) is a worthwhile FOLLOW-UP but is not done
— `-j4` alone is the current safe win. **Measure perf ONLY at -O2**
(`make -j4 -C src CXXFLAGS="-std=c++11 -Wall -O2"`) via `--show-stats`; **NEVER treat -O0
timings as the perf baseline** (~3× inflated by design).

**NEXT ROUND — make tsubst EARN its keep (not catalog-padding). Each its own gated commit;
queue form, grind until budget-low or a wall:**
  1. ✅ **ENGAGEMENT COUNTER LANDED (Codex, 2026-06-26):** `--show-stats` now
     reports member-template body engagement as `tsubst bodies ..... H hit / F
     fallback`. A hit means CIR built the concrete body from retained Tree-1
     tsubst metadata; a fallback means the instantiated body had that metadata
     but still lowered the parsed concrete body. Current `testsubscript` smoke
     under `MADC_XTEST_DEP_PARSE=1` reports **6 hit / 29 fallback**, confirming
     the coverage gap is measurable. Proving test:
     `CIR: tsubst engagement counters split hits and fallbacks`. Gates:
     fulltest flag-off and flag-on 670/0/0/18; `test_cir` 86/1065/4; six
     flag-on canaries green.
  2. ✅ **FALLBACK PROFILE LANDED (Codex, 2026-06-26):** `--show-stats` now
     prints `tsubst fallback profile (ranked)` grouped by retained
     source-template shape, with a concrete emitted-symbol sample per row. Clean
     `-O2` profile for flag-on `testsubscript`: 6 hit / 29 fallback, total
     0.804 s, instantiate 0.327 s. Top ranked fallbacks:
     `std::allocator_traits::construct<_Up,_Args...>` (4),
     `std::allocator_traits::destroy<_Up>` (4),
     `std::__new_allocator::construct<_Up,_Args...>` (3), then
     `_Destroy_aux::__destroy`, `_Rb_tree` node/emplace helpers, and
     `std::vector::_M_realloc_insert` at 2 each. Proving test extends
     `CIR: tsubst engagement counters split hits and fallbacks`; gates fulltest
     flag-off and flag-on 670/0/0/18; `test_cir` 86/1067/4; six flag-on
     canaries green.
  3. 🟡 **WIP-LANDED (not gate-passing) as `bb44557c`.** Covered the allocator construct/destroy
     cluster (`allocator_traits::construct`/`destroy` + `__new_allocator::construct`); engagement
     rose **6→16 hits** on real `testsubscript`. BUT it over-reached into member-template
     reference-param cases and regressed 3 flag-on tests (see 🔴 ROUND RESULT block at top).
     **→ DO step 3.5 (ref-param deref fix) BEFORE any further coverage.**
  3.5. **← YOU ARE HERE. Fix ref-param deref in the tsubst'd body** so `bb44557c`'s 16 hits are
     all correct: restore flag-on 670/0/0/18 (the 3 `testmemtmpl{refparam,fwdrefpack,typedefparam}`
     tests back to `x=77`) while keeping engagement ≥16. Deepest-layer fix — dereference the
     referent on reference-param access; do NOT exclude ref-param cases from engagement. Details
     + signature in the 🔴 ROUND RESULT block at the top of §0.
  4. **SUCCESS = MEASURABLE at -O2:** tsubst-engaged count UP and `testsubscript` instantiate
     time DOWN (flag-on now *faster* than flag-off, not slower). That is the bar — "another
     covered construct" with no measured movement does NOT count.

**SETTLED — do not re-litigate:** the keystone `resolve_copied_dependent_call` is the
instantiation lever (reuse, don't reinvent); `std::forward`/`std::move` name-match is GONE —
no callee-name matching (Rule #7); the parser crash fix + `testdependentparseerror.expect_err`
stay; hybrid B stands; perf is judged at -O2 only.

**GATE (per slice):** `make -j4 -C src fulltest` 670/0/0/18 + flag-on
(`MADC_XTEST_DEP_PARSE=1`) + the six canaries green + a proving `test_cir`. Build hygiene:
never pipe `make` through `tail`/`head` (masks errors); confirm `bin/test_cir` relinked
(`strings | grep <probe>`).

**LATEST PERF-ROUND SLICE:** engagement and fallback-profile instrumentation are
landed. This is profiling, not a claimed speedup. Next commit should cover the
top real fallback shape(s), starting with the allocator `construct`/`destroy`
cluster, and use the counter to prove hits rise before judging -O2 time.

**JUST LANDED:** direct `std::move<Args>(args)...` forwarding-pack coverage, guarded direct
`_Destroy_aux`/member-template `__destroy` marker tsubst, Fix #1 pointer-parameter-pack
expansion, Fix #2 parser robustness, and the earlier first direct SYSTEM-HEADER
reference-forwarded placement-new pack body.
The `std::move` forwarding-pack proof is coverage-only: the existing copied dependent-call
path is structural, re-resolves the concrete template-id callee exactly like
`std::forward<Args>(args)...`, requires Tree-1 copies, and returns 34. Do not add
callee-name cases.
The `__destroy`-named member-template guard now admits Tree-1 body copying only when the
retained body itself contains a direct compiler-intrinsic `__destroy(T*)` marker. That marker
already substitutes the concrete pointee and lowers class pointees to the concrete destructor
and scalar/pointer pointees to a no-op. Iterator/object-address destructor forms still fall
back; do not broaden them until their marker/lowering shape is real. The proving test is
`CIR: tsubst lowers direct destroy-aux member body`; it runs under
`MADC_XTEST_DEP_PARSE=1`, requires Tree-1 copies, and returns 7.
Function-template deduction now recognizes direct pointer-qualified packs such as
`Args*... ps`, binds `Args` to each pointee type, and the substituted declaration keeps
the pointer suffix attached to every generated parameter (`T0* ps__0, T1* ps__1`). The
proving test is `CIR: tsubst fans out direct pointer-pack call arguments`; it runs under
`MADC_XTEST_DEP_PARSE=1`, requires Tree-1 copies, and returns 34. The original scratch
repro `tmp/destroy_pack_probe.mad` now exits 0 under the flag.
The old `is_system_header_path(pe->pattern->file) → unsupported_class_arg` guard now admits
one element only when its nested call reuses **`resolve_copied_dependent_call`** (`8ede28a5`)
and the resolved reference return is the same/derived class that the constructor reference
parameter expects. The test is `CIR: tsubst lowers system-header reference-forwarded
placement-new packs` and proves `cir_count_tree1_copies > 0` plus value `34`.
The follow-up widened that same guarded aperture to constructor conversions: when the copied
nested call returns a different class reference, the pack body may still tsubst if the target
reference class has a single-argument converting constructor; the emitter materializes the
converted target temp before the outer placement-new constructor. The test is `CIR: tsubst
lowers system-header converted reference-forwarded placement-new packs` and proves
`cir_count_tree1_copies > 0` plus value `135`.

**NEXT SLICE — widen remaining pack backlog (Codex, 2026-06-26).** The
destructor-pack slice was blocked on the `Args*... ps` parser crash diagnosed in the ROOT-CAUSE
block above; that blocker is now cleared and the first direct-marker `_Destroy_aux` member
body is landed. Ordered queue — each its own clean gated commit; keep going until budget-low or a
genuinely-hard wall, then record findings and stop:
  1. ✅ **FIX #2 (robustness, priority) LANDED (Codex, 2026-06-26):** `parseFunction`
     now exception-safely balances its temporary parameter compound scope, so the
     env-gated dependent parse error no longer leaves a stale `compounds` entry
     whose stack-local `Method` later SIGSEGVs in `active_cpp_lookup_namespace`.
     Regression: `tests/testdependentparseerror.mad` + `.expect_err`. Gates:
     fulltest flag-off and flag-on 670/0/0/18; `test_cir` 82/1014/4; six
     flag-on canaries green.
  2. ✅ **FIX #1 (feature) LANDED (Codex, 2026-06-26):** pointer-parameter-pack
     call expansion (`Args*... ps`) now works through direct pack deduction and
     token fan-out. Proving test: `CIR: tsubst fans out direct pointer-pack call
     arguments`; gates fulltest flag-off and flag-on 670/0/0/18; `test_cir`
     83/1026/4; six flag-on canaries green.
  3. ✅ **DIRECT DESTRUCTOR-MARKER SLICE LANDED (Codex, 2026-06-26):**
     `_Destroy_aux`-style member-template bodies named `__destroy` now use Tree-1 tsubst
     only when the retained body itself contains a direct `__destroy(T*)` marker. Proving
     test: `CIR: tsubst lowers direct destroy-aux member body`; gates fulltest flag-off and
     flag-on 670/0/0/18; `test_cir` 84/1043/4; six flag-on canaries green.
  4. ✅ **FORWARDING VARIANT COVERAGE LANDED (Codex, 2026-06-26):**
     direct `std::move<Args>(args)...` call-pack fan-out uses the same structural copied
     dependent-call path as `std::forward`, proving there is no callee-name special case.
     Proving test: `CIR: tsubst fans out move call-pack arguments`; gates fulltest flag-off
     and flag-on 670/0/0/18; `test_cir` 85/1055/4; six flag-on canaries green.
  5. **Resume the pack backlog NEXT:** more forwarding variants, broader destructor iterator
     forms only after their marker/lowering shape is real, then template-id body/return;
     object-address forwarding LAST. Keep the per-element resolver/return-class guard.
Do not attempt all shapes at once.

**RESOLVED BLOCKER (Codex, 2026-06-26):** the minimal `_Destroy_aux`-style probe needed a
pointer parameter pack (`Args*... ps`) so the existing `__destroy(T*)` Tree-1 marker still
sees a pointer-to-template-pointee. The parser now handles that spelling safely, and the
direct-marker `__destroy` member body is admitted without relaxing the broader guard. A value
pack of pointer arguments (`Args... ps`) still is not an equivalent proof because it loses the
pointer-to-template-pointee shape. Do **not** use this landed slice as permission to admit
iterator/object-address destructor bodies; those need their own real marker/lowering shape.

**↳ ROOT-CAUSE (Claude, gdb+valgrind, 2026-06-26).** `Args*... ps` is actually TWO bugs:
(1) **Parse-grammar gap** — the pointer-parameter-pack *call-site expansion* mis-parses
("Expecting identifier after type" at the call, e.g. `d.destroy_all(a, b)`). The bare
declaration parses fine (value- AND pointer-pack decls both exit 0); only the call expansion
breaks. (2) **Robustness bug (the actual crash)** — that parse error SIGSEGVs instead of
reporting cleanly. valgrind: invalid read on RECLAIMED STACK (1944 bytes below SP). gdb: the
fatal read is `compounds.top()->method->owner_class->canonical_cpp_spelling` in
`active_cpp_lookup_namespace` (parser.cpp:12547-48) during a LATER identifier resolution —
`compounds.top()` is a STALE member-template compound left on the scope stack by the
mis-parse's error recovery (we are parsing `main`, a free fn, which should bail at :12545 with
no `owner_class`). **FIX #2 (priority; contained; decoupled from the feature):** balance
`compounds` on the error-recovery path (RAII/save-restore) so a parse error NEVER leaves a
dangling scope entry — madc must not crash on a parse error. **FIX #1 (feature; the real
unblock):** support pointer-param-pack call expansion. Confirm the exact unbalanced push in a
`-g` build; repro = `tmp/destroy_pack_probe.mad` (scratch).

**⚠️ THE TRAP — read before coding:** a NAIVE relax of the :931 guard already regressed six
real-header canaries: `testcontainerdtor`, `testforeach2`, `testset`, `teststringref`,
`testsubscript`, `testsubscriptmember`. This needs a REAL expansion pass, NOT a guard relax.
Gate all six flag-on every iteration. Object-address broadening beyond the direct/converted
returned-class apertures remains the riskiest shape — leave wider variants for last.

**SETTLED — do not re-litigate:** the keystone IS the instantiation lever (reuse, don't
reinvent); the `std::forward`/`std::move` name-match is GONE — do NOT reintroduce callee-name
matching (Rule #7); Hybrid B stands (shell at parse, BODIES via tsubst); the system-header
bail stays for genuinely-unsupported shapes — this slice MOVES one class onto tsubst; commit
each slice clean + gated; keep mirrors (handoff/status/CHANGELOG/KG) in agreement.

**GATE (per slice):** `make -C src fulltest` 670/0/0/18 + the six canaries green under
`MADC_XTEST_DEP_PARSE=1` + a new `test_cir` case proving the covered pack goes
through tsubst (`cir_count_tree1_copies > 0` AND correct runtime value). Build hygiene: NEVER
pipe `make` through `tail`/`head` (masks errors, fakes exit 0); after editing
`cir_builder.cpp` run `make -C src ../bin/test_cir` and confirm the relink took
(`strings bin/test_cir | grep -c <probe>` — 0 means stale).

**NOT this session (Claude handles separately):** the flag-on-vs-flag-off perf MEASUREMENT —
we are flying blind on whether tsubst is actually faster; that is Claude's task, not a Codex
grind. The still-open surfaces after this slice: template-id body/return, broader dependent
calls, then the shell-copy follow-on for FULL re-parse deletion.

✅ **KG CURRENT (2026-06-26):** Feature `two_tree_tsubst_instantiation` `phase4_estimate_percent=74`,
`updated=2026-06-26`; session notes through the pointer-parameter-pack expansion and guarded
direct `_Destroy_aux`/member-template `__destroy` marker slice plus direct
`std::move<Args>(args)...` forwarding-pack coverage are present. The perf-round
engagement-counter slice adds `--show-stats` hit/fallback body counts and pins
the current `testsubscript` baseline at 6 hit / 29 fallback. The fallback
profile slice ranks the current top real patterns as allocator_traits
construct/destroy (4 each) and __new_allocator construct (3).
(Original sync note below.)

✅ **KG SYNCED (2026-06-24).** FalkorDB was briefly unreachable mid-session; once back,
`madc-knowledge` was reconciled via `scripts/kg_query.sh`: Feature
`two_tree_tsubst_instantiation` (status in_progress) + Session
`session_2026_06_24_two_tree_phase4`, linked `LANDED` + `IMPLEMENTS_DECISION
→ cir_c2mir_backend_adr`. Captures: two-tree **Phase 3 COMPLETE**; **Phase 4 constructs
1–4 landed** (ptr/ref/const params `b5e9d86`; bare-T return `6117ccd`; pointer return
`c5adec2`; multiple type params + completeness guard `2204d19`); measured next targets
were pack 87 dominant > template-id 14, with direct type-arg binding called out as the
prerequisite cleanup. The local Codex update below completes direct type-arg binding; the
later local Codex updates also capture TYPE-pack metadata and implement direct
value/ref/expression/forwarding-call/constructor pack call-argument fan-out slices, then
admit the first covered system-header placement-new body, scalar `_Up` constructed-type
slice, simple class `_Up` placement-new slice, and direct `__destroy(T*)` helper slice.
The direct helper path emits a deferred Tree-1 marker for template-parameter pointees
and lowers the concrete class destructor/no-op after substitution; split system-header/user
parses now also reattach existing builtin/intrinsic `FuncDef`s to the active
`TokenProgram` so recipe capture can resolve compiler intrinsics. The remaining
design-level target is broader cir-node **pack expansion** (`tsubst_pack_expansion`
analogue) for real system-header forwarding/destructor patterns, class-valued constructor
argument packs that need real reference-forwarding/object-address lowering, broader system-header
nested/dependent calls, and template-id body surfaces.
The latest local slice also covers local non-pack nested namespace function-template calls
such as `sink(nn::ident(v))`, and the follow-up covers singleton by-value class-object
constructor packs in allocator-style placement-new bodies, then multi-element by-value
class-object constructor packs such as `PairBox(Item, Item)`, then value-returning
class-reference constructor packs such as `PairRef(const Item&, const Item&)`, then local
reference-returning identity-forwarded class-reference packs where `Args&...` is passed
through `std::forward<Args>(args)...`, then simple system-header dependent-call
admission including reserved scalar/pointer helper names; all gated flag-off AND
flag-on 669/0/0/18; torture byte-identical by construction (env-gated);
`test_cir` is now 80/994/4. Rough Phase 4 implementation estimate is now 72% by
coverage weight, not session count.

✅ **2026-06-25 LOCAL CODEX UPDATE — SIMPLE SYSTEM-HEADER SCALAR DEPENDENT
CALLS + COPIED-CALL REACHABILITY.** `resolve_copied_dependent_call` now admits
the first non-pack system-header dependent-call shape instead of treating every
system-header call as a hard fallback: every substituted explicit template arg
and runtime arg must be a concrete non-class scalar/pointer shape, the substituted
return must be scalar/pointer/void, and the resolved/instantiated callee must have
a materializable body or real external symbol. The old implementation-reserved
`__*` name fence is gone for this simple scalar/pointer shape. Copied dependent
calls also insert the resolved callee into `referenced_funcs`, matching ordinary
call lowering so deferred lazy system-header bodies get emitted. A synthetic
system-header `nn::__ident<T>(T)` body now stays on the Tree-1 tsubst path and
runs through the split header/user parse path.

The guard remains deliberately conservative: real system-header forwarding,
destructor, reference/object-address, broader pack, and template-id body surfaces
still fall back until they get explicit lowering. Validation: focused flag-on
doctest green (1 test / 17 assertions); flag-on `bin/test_cir` green (80 tests /
994 assertions / 4 skipped); real-header canaries green under
`MADC_XTEST_DEP_PARSE=1` (`testcontainerdtor`, `testforeachref`, `testmadc_ns`,
`testsubscript`, `testvector`); `MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh`
green 669/0/0/18; `make -C src fulltest` green 669/0/0/18 with both check gates
green.

✅ **2026-06-25 LOCAL CODEX UPDATE — LOCAL REFERENCE-FORWARDED CLASS-REFERENCE
PLACEMENT-NEW PACKS.** The tsubst placement-new pack path now covers local retained
recipes where class objects are passed by reference through an identity
`std::forward<Args>(args)...` / `std::move(args)...` wrapper into class-reference
constructor parameters. During copied argument construction, the lowering peels that
identity forwarding call, copies the concrete pack operand, and address-takes the operand
per element instead of copying/addressing the forwarding call itself. This keeps the temp
scope inside the copied placement expression for value-returning class objects and uses
the original object address for reference-returning local forwarding.

The guard is deliberately narrow: if the pack-expansion pattern comes from a system
header, real reference-forwarding/object-address packs still fall back to the parsed-body
path. A broader attempt to admit system-header object-address forwarding caused real-header
canary output regressions (`testcontainerdtor`, `testforeach2`, `testset`,
`teststringref`, `testsubscript`, `testsubscriptmember`), so the safe next slice is a
separate real system-header forwarding/object-address expansion pass rather than relaxing
this local guard.

Validation for this local update:
- `g++ -S -fverbose-asm -O0` and `clang++ -S -fverbose-asm -O0` reference reducers checked.
- `make -C src` green.
- Focused flag-off/flag-on new test green: 1 test / 14 assertions.
- Placement-new subset flag-off/flag-on green: 8 tests / 110 assertions / 75 skipped.
- Pack subset flag-off/flag-on green: 12 tests / 154 assertions / 71 skipped.
- `bin/test_cir` flag-off/flag-on green: 79 tests / 977 assertions / 4 skipped.
- Real-header canaries green under `MADC_XTEST_DEP_PARSE=1`: `testcontainerdtor`,
  `testvector`, `teststringref`, `testforeachref`, `testvectorptr`.
- `make -C src fulltest` green: 669 passed / 0 failed / 0 timed out / 18 skipped.
- `MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh` green: 669/0/0/18.

✅ **2026-06-25 LOCAL CODEX UPDATE — NESTED FUNCTION-TEMPLATE INSTANTIATION KEYSTONE.**
The tsubst/copy path now instantiates missing local nested namespace function templates
instead of merely re-resolving already-existing overloads. Copied dependent `N_CALL` and
callee-id paths share `resolve_copied_dependent_call`: substituted explicit args and
argument types drive normal overload lookup first, then a concrete-typed synthetic call
instantiates the missing namespace function-template specialization on miss.

Reference-forwarding pack operands are rebuilt against the concrete callee. When the new
callee takes a reference, copied argument lowering rewrites `DEREF ID args` back to the
stored concrete pointer slot (`args`, `args__N`) before call emission; reference-returning
nested calls are deref-wrapped only when the source call was not already under a source
dereference. The previous local `std::forward`/`std::move` name peel is removed.

Validation for this local update:
- `make -C src ../bin/test_cir` green.
- `make -C src ../bin/madc` green.
- Flag-on focused `bin/test_cir -tc "*reference-forwarded*"` green: 79 test cases /
  977 assertions / 4 skipped.
- Flag-on direct canaries green: `testcontainerdtor`, `testforeachref`, `testmadc_ns`,
  `testsubscript`, `testsubscriptarrow`, `testvector`, `testvectorptr`,
  `testmemtmplpackexpand`, `testvariadicfn`.
- `MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh` green: 669 passed / 0 failed /
  0 timed out / 18 skipped.
- `make -C src fulltest` green: 669 passed / 0 failed / 0 timed out / 18 skipped;
  both check gates green.


DONE + gated + committed on this branch:
- **`b35cef2`** scoped template-param registry (`Program::template_param_scopes` +
  `TemplateParamScope` RAII + `intern_template_param` + `resolve_template_param`,
  consulted first in `resolve_current_class_type_alias`).
- **`2104299`** the PRODUCER + defer guard: `build_dependent_pattern(FuncDef*)`
  (parser.cpp) parses a member-fn-template body ONCE with param→`DataDefTemplateParam`
  placeholder and captures it as **`FuncDef::dependent_pattern`** (a TokenFunc parse-tree
  RECIPE), removing it from `pending_funcs`. `dependent_parse_in_progress` gates the
  instantiation entry points so nested calls don't eagerly instantiate.
- **`20bbf92`** widening step 1: the per-call dependence test
  (`call_involves_placeholder` / `datadef_involves_placeholder`, parser.cpp) replaced the
  global bail — the instantiation entry points now defer ONLY genuinely type-dependent
  calls (g++'s `any_type_dependent_arguments_p` analogue, pt.cc:30555).
- **✅ PHASE 3 FIRST SLICE DONE — the recipe is now CONSUMED (3 commits):**
  - **`e4dda75`** `tsubst_cir` core: `copy_cir_subtree` gains an optional
    `{placeholder→concrete}` map (default NULL = byte-identical); `subst_datadef` rewrites a
    `DataDefTemplateParam` directly or under ptr/ref/const layers via the canonical builders.
  - **`6c301f9`** `FuncDef::tsubst_source`: a concrete instance links back to its SOURCE
    member template (stamped in `instantiate_member_fn_template_for_call`).
  - **`2bf8696`** the SEAM (`func_def`, cir_builder.cpp): `tsubst_method_body` builds a
    covered method's BODY by `tsubst_cir` of the recipe (cir-built ONCE into a memoized
    Tree-1 pattern, `m_tsubst_body_patterns`) instead of `translate_block`. Binding recovered
    by aligning recipe placeholder params with the instance's concrete params. CONSERVATIVE
    capability gate: if the pattern build trips the `append_type_specs` guard (a body cast
    `(U)x` / local `U tmp;` lowers a placeholder TYPE → error node), memo NULL and FALL BACK
    to re-parse (not yet covered). Hybrid B: concrete signature/shell stays on the parse path;
    re-parse stays the fallback (PLAN §5).
  - **`2459a7e`** WIDENING — deferred type-spec MARKER (g++ TEMPLATE_TYPE_PARM-in-saved-tree):
    in pattern mode `append_type_specs` leaves a placeholder used as a TYPE (`(T)x`, `T tmp;`)
    as an N_IGNORE marker carrying the placeholder datadef (instead of erroring); tsubst
    EXPANDS it to the concrete specs via the SAME `append_type_specs` (byte-identical), or an
    error node → fall back. Unit-tested (expansion: test_cir 58/0).

Producer + consumption run ONLY behind env hook **`MADC_XTEST_DEP_PARSE`** (off by default →
production byte-identical). VALIDATED firing (not silent fallback): `Holder::set<int>` and
`testoutoflinemembertemplate::store<int>` build bodies by tsubst, BYTE-IDENTICAL emit to
re-parse. Gate every commit: build clean; fulltest flag-OFF AND flag-ON 669/0/0/18; --emit=c11
byte-identical; torture flag-off byte-identical by construction.

✅ **2026-06-24 LOCAL CODEX UPDATE — DEPENDENT-PARSE-of-`T`-as-a-TYPE gap RESOLVED
(uncommitted).** Root cause was C-style cast disambiguation in `Program::parseExpr_operatorArm`:
identifier cast heads only checked `datatype_map` / `struct_map`, so `(T)v` fell through as a
parenthesized expression and reported `T` as an undeclared identifier before the dependent
recipe could reach the type-spec marker. The parser now also consults
`resolve_current_class_type_alias`, which reaches the scoped `DataDefTemplateParam` registry,
for cast heads, cast-token consumption, and nested-cast lookahead. This records the placeholder
type without asking for concrete size/rawtype/conversion. `T tmp` locals were already accepted
through the declared-type path; regression coverage now pins both `member = (int)(T)v + 1;`
and `T tmp = v; member = (int)tmp + 1;` in dependent pattern parse.

Validation for the local update:
- `make -C src` green.
- `bin/test_cir` green: 59 test cases, 739 assertions, 4 skipped.
- `make -C src fulltest` green: 669 passed, 0 failed, 0 timed out, 18 skipped.
- `env MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh` green: 669 passed, 0 failed,
  0 timed out, 18 skipped.
- Focused reducers: `tmp/tsubst_marker.mad`, `tmp/tsubst_scalar.mad`,
  `tmp/tsubst_local.mad` clean.
- `--emit=c11` flag-off vs flag-on output byte-identical for marker/local reducers; no raw
  `T`, `tsubst:`, or `unbound template` markers in emitted C.
- GDB breakpoint confirmed `CirBuilder::tsubst_cir` is reached from
  `CirBuilder::tsubst_method_body` for the marker reducer, proving this is not silent fallback.

✅ **PHASE 4 — construct 1 DONE (ptr/ref/const params).** The binding RECOVERY now peels
`T*` / `T&` / `const T` param layers in lockstep with the concrete instance param, binding the
BARE placeholder (so `subst_datadef`'s existing peel rebuilds the derived type on body nodes).
New file-static `recover_param_binding` (cir_builder.cpp, beside `subst_datadef`) replaces the
scalar-only inline loop in `tsubst_method_body`; the old `is_template_param()` case is its
recursion base. A layer mismatch recovers nothing → safe re-parse fallback. VERIFIED: a
temporary `[TSUBST] … recovered N binding(s)` trace showed `T*`/`T&`/`const T&` go 0→1 binding
(fire tsubst, not silent fallback); trace removed. test_cir 60/60 (new derived-type case pins
`T*`→`int*`, `T&`→`int&`, Tree-1 immutability). Reducers `tmp/tsubst_{ptr,ref,constref}.mad`
run 2/4/6 and emit byte-identical flag-off vs flag-on. Torture byte-identical BY CONSTRUCTION
(all new code behind the `MADC_XTEST_DEP_PARSE` env gate — production never reaches it).

✅ **PHASE 4 — construct 2 DONE (bare-`T` dependent return type).** `tsubst_eligible`
(parser.cpp) now ACCEPTS a member template whose return type is the bare placeholder
(`T echo(T v)`). The return type lives in the concrete SHELL (resolved on the normal parse
path — hybrid B); the recipe parse passes the placeholder return type to `parseFunction`
(which already tolerates it — NO parser change needed), and tsubst substitutes the BODY via
the param binding. KEY TRAP avoided: `return_value_type().is_template_param()` is FALSE for a
bare-`T` return (it is a return TOKEN naming T, not a `DataDefTemplateParam`), and
byte-identical emit MASKS a silent re-parse fallback — so the gate discriminates the bare case
at the TOKEN level (return-token stream == the single identifier `T`) and firing was reproven
with the `[TSUBST]` trace, NOT byte-identical alone. A NON-bare dependent return (`T*`, `T&`,
`vector<T>`, `T::type`) stays REJECTED (token scan) → re-parse fallback. VERIFIED: trace shows
`echo`/`twice` fire (1 binding), `addr` (T* return) does NOT fire. test_cir 61/61 (new case:
`echo` eligible via funcdef_map key, `T*` `addr` rejected, exactly 1 pattern). Reducers
`tmp/tsubst_{retdep,retdep2,retptr}.mad` run 8/14/7 byte-identical. Torture byte-identical BY
CONSTRUCTION (`tsubst_eligible`'s only caller is `build_dependent_pattern`, gated behind
`MADC_XTEST_DEP_PARSE`).

✅ **PHASE 4 — construct 3 DONE (POINTER dependent return `T*`).** `tsubst_eligible` now
also accepts a return that is the bare param T wrapped in POINTER layers (`T`, `T*`, `T**`).
Detected on the return-token stream (recon via a temporary `[RETTOK]` dump: `id=60`=ident,
`id=13`=`tkStar`, `id=24`=`tkBand`; `const` is NOT in these tokens): exactly one identifier
(== T), every other token a `*` (`tkStar`). A `<` (template-id `vector<T>`), `::` (`T::type`),
or a second identifier stays REJECTED. VERIFIED (trace, not byte-identical alone): `T* passthru(T* p)`
fires (1 binding), runs 42, byte-identical; `vector<T>` return does NOT fire. test_cir 61/61
(updated case: `echo` bare-T + `addr` `T*` both eligible, `ref` `T&` not). Reducers
`tmp/tsubst_retptr2.mad` (42) + `tmp/tsubst_retptr.mad` (7) byte-identical. Torture byte-identical
BY CONSTRUCTION (gated). ⚠️ REFERENCE returns (`T&`/`const T&`) are deliberately EXCLUDED:
`tmp/tsubst_retref.mad` / `tsubst_retcref.mad` fail to compile **flag-off too** ("undeclared
identifier `Holder__passref`" + "lvalue required") — a SEPARATE pre-existing
ref-return-member-template instance-symbol bug, NOT tsubst. Revisit `T&` returns after that bug
is fixed (it's a call-site/instantiation-naming issue, a different track).

✅ **PHASE 4 — construct 4 DONE (MULTIPLE type params).** `tsubst_eligible` now accepts a
member template with ≥1 type param (was hard-capped at exactly 1). The `is_type`/`is_pack`
checks were generalized from index `[0]` to ALL indices (so a mixed `<class A, int N>` cannot
slip through), and the return / body `T`-scans now test a param-name SET (`pnames`) rather than
a single `T`. `recover_param_binding` already builds one binding per param.
VERIFIED (trace): `template<class A, class B> void set2(A a, B b)` fires with **2 bindings**
(runs 7, byte-identical); single-param `echo` still fires (1 binding). test_cir 62/62 (new case:
`set2` + `echo` both eligible, 2 patterns). Torture byte-identical BY CONSTRUCTION (gated).
🛠 **COMPLETENESS GUARD (the flag-on gate caught a real bug — do not remove):** multi-param
first crashed 10 flag-on tests (incl. `testvectorptr`). Cause: a real multi-param std method
where a type param appears only in the BODY (not as a parameter type) recovered a PARTIAL
binding (non-empty), so tsubst fired leaving UNSUBSTITUTED placeholders → wrong code no error
node caught. Fix (`tsubst_method_body`, deepest layer): require
`binding.size() >= source->template_param_names.size()` — every param recovered from the
signature, else fall back to re-parse. Single-param behavior unchanged (need=1 == old
binding.empty check).
⚠️ NON-TYPE params stay rejected (correct — they need value substitution, not built); they also
can't currently be exercised end-to-end because the explicit non-type member-template call
syntax (`f<int,5>(...)`) fails to PARSE (a separate pre-existing gap — see `tmp/tsubst_mixed.mad`).

✅ **2026-06-24 LOCAL CODEX UPDATE — DIRECT TYPE-ARG BINDING DONE (uncommitted).**
Concrete instantiated member-template `FuncDef`s now carry `tsubst_type_args`, a parser-owned
vector of resolved TYPE template arguments in `tsubst_source->template_param_names` order.
The vector is filled from `try_instantiate_namespace_fn_template` /
`instantiate_fn_template_binding` after explicit args, deduction, and defaults settle, then
stored beside `tsubst_source` in `instantiate_member_fn_template_for_call`. CIR
`tsubst_method_body` now builds `{DataDefTemplateParam* -> concrete DataDef*}` directly from
that vector via `Program::intern_template_param`, so `recover_param_binding` is retired.
This matches g++/clang's saved-tree-plus-args model and covers body-only type params that
cannot be recovered from a signature. New regression: `Holder::body_only<int>(7)` records
`int` in `tsubst_type_args` even though `T` appears only as `T tmp = (T)x` in the body.

The direct-binding slice exposed unsupported bodies that signature recovery had previously
kept on fallback. Two conservative guards restore correctness until widened deliberately:
reference-parameter bodies stay on re-parse fallback because value reads need the parsed
parameter variable metadata, and recipes containing dependent nested calls stay on fallback
because call re-resolution after substitution is not implemented yet. The flag-on gate caught
both classes (`testmemtmplrefparam` and the vector/subscript family); both gates are green now.
Validation: `bin/test_cir` 63 test cases / 766 assertions / 4 skipped; `make -C src fulltest`
669/0/0/18; `MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh` 669/0/0/18; `git diff --check`
clean. KG synced: Feature `two_tree_tsubst_instantiation` now records direct type-arg binding
as done locally and Session `session_2026_06_24_two_tree_direct_type_args_codex` is linked via
`LANDED`.

✅ **2026-06-24 LOCAL CODEX UPDATE — TYPE-PACK ARG METADATA CAPTURED (uncommitted).**
The parser instantiation path now also records concrete TYPE parameter-pack elements on the
instantiated `FuncDef` in `tsubst_type_arg_packs`, parallel to
`tsubst_source->template_param_names`: non-pack slots are empty; pack slots hold deduced
element `DataDef*`s in expansion order. `collect_ordered_type_arg_bindings` captures both
ordinary type args and pack slots from the same place the re-parse path already computes them
(`pack_param`/`pack_elems` and supported 0/1-element template-id packs). This does **not**
claim CIR fan-out is implemented yet; it removes the missing-data blocker so the future
`tsubst_pack_expansion` analogue has real arity and element types without inspecting
token-expanded reparse output. Unit coverage pins `template<class... Args> void pack_body(Args... args)`
called as `pack_body(1, 2)`: the concrete instance links to its source and records one pack
slot with two `int` elements. That slice validated at `bin/test_cir` 64 test cases / 778 assertions /
4 skipped; `make -C src fulltest` 669/0/0/18; `MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh`
669/0/0/18; focused env-gated variadic reducer clean; `git diff --check` clean. KG synced:
Feature `two_tree_tsubst_instantiation` now records type-pack metadata capture and Session
`session_2026_06_24_two_tree_direct_pack_args_codex` is linked via `LANDED`.

✅ **2026-06-24 LOCAL CODEX UPDATE — DIRECT VALUE-PACK FAN-OUT DONE (uncommitted).**
The tsubst path now handles the first actual CIR pack expansion shape: direct value-pack
call arguments such as `sink(args...)` in a member-template body with `Args... args`.
During dependent parse, `expr...` becomes `TokenPackExpansion`; CIR translation lowers the
inner expression once and tags the resulting `cir_node` with the source pack index and direct
value-pack name. During `copy_cir_subtree`, a marked child under an `N_LIST` fans out by the
instantiated `FuncDef::tsubst_type_arg_packs` slot, augments the active substitution map with
the concrete pack element type, and renames direct value-pack ids from `args` to `args__0`,
`args__1`, etc. Empty packs emit no list children. Complex pack patterns, forwarding forms,
ordinary reference-parameter bodies, dependent nested calls, and template-id body/return
surfaces still fall back.
Unit coverage pins `template<class... Args> int pack_call(Args... args) { return sink(args...); }`
called as `pack_call(3, 4)`: it fires tsubst instead of silent re-parse fallback and returns
34. Validation: `bin/test_cir` 65 test cases / 790 assertions / 4 skipped; `make -C src`
green; `make -C src fulltest` 669/0/0/18; `MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh`
669/0/0/18; `git diff --check` clean. KG synced: Feature
`two_tree_tsubst_instantiation` now records direct value-pack fan-out and Session
`session_2026_06_24_two_tree_direct_value_pack_fanout_codex` is linked via `LANDED`.

✅ **2026-06-24 LOCAL CODEX UPDATE — DIRECT REFERENCE-PACK FAN-OUT DONE
(uncommitted).** The direct pack fan-out path now also covers reference parameter packs such
as `template<class... Args> int pack_ref(Args&... args) { return sink(args...); }`. The missing
piece was not list expansion itself; `TokenPackExpansion` was looking only for a bare
`DataDefTemplateParam`, while `Args&... args` presents as `DataDefREF(DataDefTemplateParam)`.
CIR lowering now peels reference/const/pointer type layers to discover the underlying TYPE
pack marker. The re-parse fallback guard was narrowed too: ordinary reference-parameter bodies
still fall back, but reference params whose referent is a TYPE parameter pack are admitted, and
recipe `FuncDef`s are checked against source template metadata because the recipe shell does
not carry the pack metadata itself. Unit coverage pins `Args&... args` plus `sink(args...)`,
requires Tree-1 copies, and returns 34. Validation: `make -C src` green; `bin/test_cir` 66 test
cases / 802 assertions / 4 skipped; focused `MADC_XTEST_DEP_PARSE=1 bin/test_cir` green; related
canaries (`testmemtmplrefparam`, `testmemtmplfwdrefpack`, `testmemtmplpackexpand`,
`testvariadicfn`) green; `make -C src fulltest` 669/0/0/18; `MADC_XTEST_DEP_PARSE=1 bash
scripts/run_tests.sh` 669/0/0/18; `git diff --check` clean. KG synced: Feature
`two_tree_tsubst_instantiation` now records direct reference-pack fan-out and Session
`session_2026_06_24_two_tree_direct_reference_pack_fanout_codex` is linked via `LANDED`.

✅ **2026-06-24 LOCAL CODEX UPDATE — DIRECT EXPRESSION-PACK FAN-OUT DONE
(uncommitted).** The direct pack fan-out path now covers expression-pattern packs that do not
need call re-resolution, such as `sink((args + 1)...)`. The missing piece was metadata
discovery: the `TokenPackExpansion` root for `(args + 1)...` is the translated binary
expression, not the pack variable itself, so the earlier direct-id path could not find the pack
marker or value name. CIR lowering now recursively discovers the TYPE pack marker and direct
value-pack name inside the pattern tree, then reuses the existing `copy_cir_subtree` fan-out to
clone the expression once per concrete pack element and rename inner `args` leaves to
`args__N`. Unit coverage requires Tree-1 copies for `sink((args + 1)...)` and returns 45.
Validation: `make -C src` green; focused `bin/test_cir --test-case=*expression-pack*` green;
pack subset `bin/test_cir --test-case=*pack*` green; `bin/test_cir` and
`MADC_XTEST_DEP_PARSE=1 bin/test_cir` both 67 test cases / 814 assertions / 4 skipped; related
flag-on reducers/canaries (`tmp/tsubst_pack_expr.mad`, `tmp/tsubst_pack_ref.mad`,
`tests/testvariadicfn.mad`) green; `make -C src fulltest` 669/0/0/18 with both gates green;
`MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh` 669/0/0/18.

✅ **2026-06-24 LOCAL CODEX UPDATE — FIRST FORWARDING-CALL PACK FAN-OUT DONE
(uncommitted).** The pack fan-out path now covers the first dependent-call pattern:
`sink(std::forward<Args>(args)...)`. The parser now wraps function-call `expr...` forms as
`TokenPackExpansion` during dependent parse instead of returning before the generic
ellipsis wrapper. CIR copy still fans out one marked list child per concrete pack element,
but copied call-id leaves now re-resolve through the normal namespace overload machinery
using the concrete explicit template args and substituted parameter types, so placeholder
`std::forward<Args>` ids become the concrete specialization symbol for each cloned argument.
Pattern-build side effects in real libstdc++ constructor/destructor helpers were avoided by
keeping system-header template-id pack bodies on the parsed-body fallback until those broader
surfaces are explicitly covered. Unit coverage pins a local `std::forward<T>(T)` reducer,
requires Tree-1 copies for `sink(std::forward<Args>(args)...)`, and returns 34. Validation:
`make -C src` green; pack subset `bin/test_cir --test-case=*pack*` 5/60 green;
`bin/test_cir` and `MADC_XTEST_DEP_PARSE=1 bin/test_cir` both 68 test cases / 826 assertions
/ 4 skipped; previous flag-on failure canaries (`testcontainerdtor`, `testforeachref`,
`testvectorptr`) pass; `make -C src fulltest` 669/0/0/18 with both gates green;
`MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh` 669/0/0/18; `jq empty
claude_status.json` and `git diff --check` clean.

✅ **2026-06-24 LOCAL CODEX UPDATE — SYSTEM-HEADER SCALAR PLACEMENT-NEW PACK
SLICE DONE (uncommitted).** The system-header fallback guard now admits the
structurally covered placement-new constructor-pack body shape, rather than
rejecting every template-id pack body from a system-header path. In CIR pattern
mode, placement-new whose constructed type is still a template parameter (for
example allocator `_Up`) leaves a marker instead of lowering too early; during
`copy_cir_subtree`, tsubst substitutes that constructed type and lowers scalar
or pointer `_Up` placement construction through the existing placement-new path.
Pack expansion outside an `N_LIST` is accepted only for singleton packs, which
matches scalar constructor assignment; singleton value-pack copies keep the
original parameter name (`args`) instead of renaming to a non-existent
`args__0`. Class `_Up` placement construction with class-valued constructor
argument packs deliberately returns an error marker and falls back to re-parse
until object-argument pack lowering is covered.
Unit coverage pins both a simulated system-header placement-new pack body and
an allocator-style scalar `_Up` reducer returning 42.

Validation: `make -C src` green; `MADC_XTEST_DEP_PARSE=1 bin/madc --emit=c11
tmp/tsubst_pack_new_scalar.mad` emits `__ns_std_forward__o2(args)` (not
`args__0`); focused scalar/placement-new doctests green (3 tests / 40
assertions for placement-new subset); pack subsets green flag-off and flag-on
(8 tests / 98 assertions); canaries green under `MADC_XTEST_DEP_PARSE=1`
(`testcontainerdtor` exits 0 with known non-fatal diagnostics, plus
`testvectorptr`, `testplacementnew*`, `testvariadicfn`, `testforeachref`,
`testmemtmplpackexpand`); `bin/test_cir` and `MADC_XTEST_DEP_PARSE=1
bin/test_cir` both 72 test cases / 878 assertions / 4 skipped; `make -C src
fulltest` 669/0/0/18; `MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh`
669/0/0/18.

✅ **2026-06-25 LOCAL CODEX UPDATE — SIMPLE CLASS `_Up` PLACEMENT-NEW
TSUBST DONE.** The deferred constructed-type marker path now also admits class
`_Up` placement construction when the constructor pack elements are scalar/pointer-like.
After substituting `_Up`, `copy_cir_subtree` re-lowers the retained `TokenNEW`
through the existing class placement-new constructor path instead of forcing
fallback. A guard keeps class-valued constructor argument packs on the parsed-body
fallback; the first unrestricted attempt regressed real-header container canaries
through recursive object-argument lowering, so this slice deliberately covers the
simple scalar-argument class case only. Unit coverage pins a system-header-shaped
`new ((void*)p) Up(std::forward<Args>(args)...)` constructing `Box(int,int)` and
requires Tree-1 copies. Validation: `make -C src` green; focused class placement-new
doctest green (1 test / 14 assertions); `bin/test_cir` and
`MADC_XTEST_DEP_PARSE=1 bin/test_cir` both 73 test cases / 892 assertions / 4 skipped;
`make -C src fulltest` 669/0/0/18; `MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh`
669/0/0/18.

✅ **2026-06-25 LOCAL CODEX UPDATE — DIRECT `__destroy(T*)` HELPER TSUBST
DONE.** Direct compiler-intrinsic destructor helpers in retained template bodies now
have a Tree-1 path. In CIR pattern mode, `__destroy(p)` where `p` has a
template-parameter pointee type emits a deferred marker instead of inspecting the
placeholder pointee too early. During `copy_cir_subtree`, tsubst substitutes the
pointer element type, lowers class pointees to the concrete complete destructor call,
and lowers scalar/pointer pointees to a no-op expression. Broader system-header
destructor-pack/member-template surfaces such as `_Destroy_aux::__destroy` still stay
on the parsed-body fallback.

The slice also fixed a producer-layer multi-buffer lookup issue: `_parser_init`
re-runs builtin registration per parse, but `addFunction()` previously returned early
when `funcdef_map` already had a shared `FuncDef`, leaving later `TokenProgram`s
without a function variable for compiler intrinsics. It now reattaches the existing
`FuncDef` to the active `TokenProgram` when needed, so split system-header/user parses
can capture `__destroy(p)` during dependent recipe construction without creating a
parallel implementation.

Unit coverage pins a synthetic system-header `Cleaner::clean(T*) { __destroy(p); }`
body, verifies the source template has a dependent recipe, verifies the concrete
instance links to that source, requires Tree-1 copies, and runs a class destructor
side effect returning 7. Validation: `make -C src` green; focused direct-destroy
doctest green (1 test / 17 assertions); `bin/test_cir` and
`MADC_XTEST_DEP_PARSE=1 bin/test_cir` both 74 test cases / 909 assertions / 4 skipped;
`MADC_XTEST_DEP_PARSE=1 bin/madc tests/testcontainerdtor.mad` exits 0 with known
non-fatal diagnostics; `make -C src fulltest` 669/0/0/18;
`MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh` 669/0/0/18.

✅ **2026-06-25 LOCAL CODEX UPDATE — LOCAL NESTED NAMESPACE-CALL TSUBST
DONE.** Copied dependent-call re-resolution now covers the first non-pack nested
call shape in a retained member-template body: a local namespace function-template call
such as `sink(nn::ident(v))`, where `ident` depends on the member-template type parameter
but is not itself a pack expansion. The old pack-only call-id rewriter is now a
dependent-call rewriter: it substitutes explicit/argument DataDefs, then reuses the normal
`find_namespace_function_overload` path to choose the concrete callee symbol for the copied
Tree-2 node. The dependent-call eligibility scan admits only this re-resolvable namespace
call shape; member calls and unknown dependent expressions still fall back. Non-pack
system-header dependent calls deliberately mark the copied node unsupported so
`tsubst_method_body` falls back to the parsed body; pack-expanded system-header calls remain
covered by the existing placement-new path.

Unit coverage pins `template<class T> int nested(T v) { return sink(nn::ident(v)); }`,
requires Tree-1 copies, and returns 42. Validation: `make -C src` green; focused nested
doctest green (3 tests / 35 assertions); pack subset green flag-off and flag-on (8 tests /
98 assertions); `bin/test_cir` and `MADC_XTEST_DEP_PARSE=1 bin/test_cir` both 75 test cases /
921 assertions / 4 skipped; previously failing broad-guard canaries
(`testcontainerdtor`, `testvector`, `teststringref`, `testforeachref`) exit 0 under
`MADC_XTEST_DEP_PARSE=1`; `make -C src fulltest` 669/0/0/18; `MADC_XTEST_DEP_PARSE=1 bash
scripts/run_tests.sh` 669/0/0/18; `jq empty claude_status.json`; `git diff --check` clean.

✅ **2026-06-25 LOCAL CODEX UPDATE — SINGLETON CLASS-OBJECT PLACEMENT-NEW
PACK TSUBST DONE.** The deferred `_Up` placement-new marker now admits the first
class-valued constructor-pack case: a singleton class object that binds to a by-value
class constructor parameter, such as `Box(Item)` inside
`new ((void*)p) Up(std::forward<Args>(args)...)`. The class object pack expression stays
marked until copy-time substitution, so the copied Tree-2 body renames the pack parameter
and re-resolves the forwarded callee inside the instantiated function instead of
materializing a temporary during Tree-1 lowering. The guard remains deliberately narrow:
multi-element class-object packs and class-reference/object-address packs still mark the
copy unsupported and fall back to the parsed body.

Unit coverage pins a synthetic system-header `Maker::make<Up, Args...>` body with
`Box(Item)` construction, requires Tree-1 copies, and returns 42. Validation: `make -C src`
green; focused class-object doctest green (1 test / 14 assertions); placement-new subset
green flag-off and flag-on (5 tests / 68 assertions); pack subset green flag-off and flag-on
(9 tests / 112 assertions); `bin/test_cir` and `MADC_XTEST_DEP_PARSE=1 bin/test_cir` both
76 test cases / 935 assertions / 4 skipped; real-header canaries (`testcontainerdtor`,
`testvector`, `teststringref`, `testforeachref`, `testvectorptr`) exit 0 under
`MADC_XTEST_DEP_PARSE=1`; `make -C src fulltest` 669/0/0/18; `MADC_XTEST_DEP_PARSE=1 bash
scripts/run_tests.sh` 669/0/0/18.

✅ **2026-06-25 LOCAL CODEX UPDATE — MULTI-ELEMENT CLASS-OBJECT PLACEMENT-NEW
PACK TSUBST DONE.** The by-value class-object placement-new pack path now handles
multi-element packs by mapping each expanded element to the constructor parameter it will
occupy after fan-out. This covers allocator-style `new ((void*)p)
Up(std::forward<Args>(args)...)` where `Up` has a constructor like
`PairBox(Item, Item)`. The guard remains parameter-aware: any class object that would bind
to a reference/object-address parameter still marks the copy unsupported and falls back,
because that surface needs per-element `object_arg_addr` lowering rather than the existing
marked-expression fan-out.

Unit coverage pins a synthetic system-header `Maker::make<Up, Args...>` body with
`PairBox(Item, Item)` construction, requires Tree-1 copies, and returns 34. Validation:
`make -C src` green; focused multi-element doctest green (1 test / 14 assertions);
placement-new subset green flag-off and flag-on (6 tests / 82 assertions); pack subset
green flag-off and flag-on (10 tests / 126 assertions); `bin/test_cir` and
`MADC_XTEST_DEP_PARSE=1 bin/test_cir` both 77 test cases / 949 assertions / 4 skipped;
real-header canaries (`testcontainerdtor`, `testvector`, `teststringref`,
`testforeachref`, `testvectorptr`) exit 0 under `MADC_XTEST_DEP_PARSE=1`;
`make -C src fulltest` 669/0/0/18; `MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh`
669/0/0/18.

✅ **2026-06-25 LOCAL CODEX UPDATE — VALUE-RETURNING CLASS-REFERENCE
PLACEMENT-NEW PACK TSUBST DONE.** The deferred `_Up` placement-new path now admits the
first class-reference constructor-pack case: value-returning forwarded class objects that
bind to reference constructor parameters, such as `PairRef(const Item&, const Item&)`.
The copied Tree-2 lowering fans out the pack, copies each forwarded expression under its
concrete pack-element substitution, and keeps any required class-object temporary
declaration/assignment inside the placement-new statement expression. Real
reference-returning `std::forward` and broader object-address system-header packs still
fall back, after a canary caught that admitting those too early recurses through
placeholder-token object-address lowering.

Unit coverage pins a synthetic system-header `Maker::make<Up, Args...>` body with
`PairRef(const Item&, const Item&)` construction, requires Tree-1 copies, and returns 34.
GCC reference check: `g++ -S -fverbose-asm -O0 tmp/tsubst_ref_class_ctor_pack.cpp -o
tmp/tsubst_ref_class_ctor_pack.s` shows the canonical real-`std::forward` path passes
addresses of the function parameters to `PairRef`. Validation: `make -C src` green;
focused value-returning doctest green flag-off and flag-on (1 test / 14 assertions);
placement-new subset green flag-off and flag-on (7 tests / 96 assertions); pack subset
green flag-off and flag-on (11 tests / 140 assertions); `bin/test_cir` and
`MADC_XTEST_DEP_PARSE=1 bin/test_cir` both 78 test cases / 963 assertions / 4 skipped;
real-header canaries (`testcontainerdtor`, `testvector`, `teststringref`,
`testforeachref`, `testvectorptr`) exit 0 under `MADC_XTEST_DEP_PARSE=1`;
`make -C src fulltest` 669/0/0/18; `MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh`
669/0/0/18.

✅ **2026-06-24 LOCAL CODEX UPDATE — CONSTRUCTOR VALUE-PACK FAN-OUT DONE
(uncommitted).** Member-template constructor instantiation now participates in the same
two-tree metadata path as member functions: under `MADC_XTEST_DEP_PARSE` it builds the
dependent recipe, captures parser-settled TYPE args and TYPE-pack arg vectors from
`try_instantiate_namespace_fn_template`, and stamps the concrete constructor `FuncDef` with
`tsubst_source`, `tsubst_type_args`, and `tsubst_type_arg_packs`. CIR `tsubst_method_body`
can therefore copy a covered local constructor body from Tree-1 and fan out direct
value-pack arguments, e.g. `Holder(Args... args) { member = sink(args...); }`, instead of
only relying on the re-parsed constructor body. Real system-header constructor/destructor
pack surfaces with template-id bodies remain on the conservative fallback. Validation:
`make -C src` green; `MADC_XTEST_DEP_PARSE=1 bin/test_cir --test-case=*constructor*` green;
pack subset `bin/test_cir --test-case=*pack*` and flag-on pack subset both 6/72 green;
`bin/test_cir` and `MADC_XTEST_DEP_PARSE=1 bin/test_cir` both 69 test cases / 838 assertions
/ 4 skipped; related flag-on canaries (`testcontainerdtor`, `testforeachref`,
`testvectorptr`, `testvariadicfn`) exit 0; `make -C src fulltest` 669/0/0/18;
`MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh` 669/0/0/18; `jq empty
claude_status.json` and `git diff --check` clean.

📊 **DATA-GROUNDED NEXT TARGETS (measured 2026-06-24, not guessed).** A temporary
reason-tagged `tsubst_eligible` (env `MADC_XTEST_ELIG`, since reverted) was run flag-on over the
container/template corpus (testvector, testvectorptr, testmap, testset, teststdmapint,
testtemplatecontainer, testcontainerdtor, testoutoflinemembertemplate, testmadc_ns,
testnestedtemplatedtor). Rejection tally:
- **`pack` 87** — DOMINANT (~58%). Variadic templates (emplace, `make_*`, forwarding ctors).
- `OK` 47 — already eligible.
- `template-id-or-lt` 14 — a body/return `<` (a real `Foo<T>` template-id OR a `<` comparison;
  the scan can't tell them apart — conservatively rejects both).
- `nontype` / `dep-return` / `dep-name` — ~0 in this corpus (rare; don't prioritize).
⇒ The CHEAP relaxations / prerequisites are EXHAUSTED (params, ptr/ref returns, multi-param,
direct type-arg binding, direct type-pack metadata, direct value-pack fan-out, direct
reference-pack fan-out, direct expression-pattern pack fan-out, first forwarding-call pack
fan-out, covered local constructor value-pack fan-out, and covered system-header scalar
placement-new pack fan-out, direct destroy helper tsubst, and local nested namespace-call
tsubst — all landed locally). The
remaining next target is DESIGN-LEVEL (traced
2026-06-24, confirmed NOT a gate relaxation):

1. **PACKS (keep first — dominant).** The existing pack machinery (`pack_subst`,
   `first_template_pack_index`, the `pack_name...` token splice ~parser.cpp:2815) is ALL
   TOKEN-LEVEL — it serves the RE-PARSE path (splice N tokens, then parse). The tsubst path
   now has node fan-out slices for direct value/ref `args...`, direct expression patterns
   like `(args + 1)...`, the first local forwarding-call pattern
   (`std::forward<Args>(args)...` as a direct call argument), and a covered local
   constructor value-pack pattern, plus covered system-header scalar/pointer and
   simple class `_Up` placement-new construction, but broader cir-node **PACK
   EXPANSION** remains: real system-header forwarding/destructor pack patterns,
   real reference-forwarding/object-address constructor argument packs, broader
   system-header nested/dependent re-resolution, and the rest of g++'s
   `tsubst_pack_expansion` surface.
   Multi-commit.

Lower priority / blocked: template-id body/return (`vector<T>`, `Foo<T>` — 14; needs
dependent-template-id handling, AND disambiguating `<` template-id from `<` comparison);
reference dependent return (`T&` — blocked on the pre-existing ref-return instance bug);
non-type params (value substitution + blocked on the `f<int,5>` call-parse gap); `T::x`;
typeless placeholder + ADL; real `is_type_dependent`. The re-parse deletion (Phase 5 = g++
lex/parse/sema parity) waits for full coverage; Phase 6 (serialize Tree-1 / header-forest) waits
for Phase 5 (user directive 2026-06-24).

## 1. GOAL (one sentence)

At cir-build of a concrete instantiated member-template method, if a Tree-1 recipe exists
for it, build the method body by **`tsubst`** (copy the recipe's cir pattern + substitute
the placeholder with the concrete arg) and feed THAT to c2mir — instead of
`translate_block` over the re-parsed body. Capability-gated; re-parse fallback stays.

## 2. THE SEAM

`CirBuilder::func_def(TokenFunc *tf)` calls `node_t body = translate_block((TokenCpnd*)tf);`
at **`src/cir_builder.cpp:11458`** (verify the line — it drifts). That is where a concrete
method's body becomes cir. Phase 3 replaces that call, for covered methods, with the tsubst
result.

## 3. STEPS (each its own gated commit; gate = §5)

1. **cir-build the recipe → a Tree-1 cir pattern.** `translate_block` over the recipe's
   placeholder body produces cir nodes whose `datadef` is the `DataDefTemplateParam`
   placeholder. The Phase 1.5 `append_type_specs` guard (cir_builder.cpp) turns a
   placeholder reaching type-lowering into an error node — that is correct for a pattern
   that is NEVER compiled directly. So either (a) build the pattern in a mode where the
   guard is suppressed and the pattern is held un-lowered, or (b) fuse build+substitute so
   the placeholder is replaced BEFORE `append_type_specs` runs. **(b) is simpler and safer**
   — never hold a placeholder-bearing cir tree that could reach c2mir.
2. **`tsubst_cir(pattern, {placeholder → concrete})`.** Reuse **`copy_cir_subtree`**
   (cir_builder.cpp:379, Phase 1 — it already makes a private node_t base) and, in the
   copy, replace any `datadef` that `is_template_param()` with the concrete arg type.
   Handle DERIVED types: ptr-to-placeholder → `getPointerType(concrete)`,
   ref-to-placeholder → `getReferenceType(concrete)`, const → `getConstType`. The concrete
   arg comes from the instantiation's type-arg binding.
3. **Method↔pattern link.** A concrete instantiated method's `FuncDef` must find its
   recipe + the concrete type args. Record the SOURCE template identity on the instantiated
   `FuncDef` when it is produced (the member-fn-template instantiation path:
   `instantiate_member_fn_template_for_call` / `try_instantiate_namespace_fn_template`).
   The recipe lives on the SOURCE template's `FuncDef::dependent_pattern`; map
   instantiated → source by (owner, member display name) or a recorded back-pointer.
4. **Wire the seam (cir_builder.cpp:11458).** For a covered method (pattern exists +
   capability-eligible), use the tsubst result; else `translate_block` (fallback). Keep the
   re-parse path — its deletion trigger is full coverage (PLAN §5).
5. **Cache + ownership.** The Tree-1 cir pattern is built ONCE per template member and
   reused across instantiations — own it in the CirBuilder Tree-1 arena (immutable, never
   freed mid-compile). Key by the source FuncDef (PLAN §11.5b).

Start SCALAR-FIRST: the narrowest covered method (one type param, scalar body, e.g.
`void set(T v){ member = v; }` on a concrete class) so the §5 equivalence check is trivial
to satisfy and debug. Widen one construct per commit after.

## 4. THE CRUX RISK (read before coding)

This is the FIRST slice where **output actually changes** for covered methods. The
`--emit=c11` gate then means **tsubst output must be byte-identical to the re-parse
output** for those methods. Any divergence is a real bug — debug it against the re-parsed
body (they must lower to the same cir). De-risk by starting with the most trivial covered
method and diffing emitted C tsubst-vs-reparse on JUST that method before widening.

## 5. GATE (every commit — correctness only, never perf-gate)
- `make -C src` clean, no new warnings.
- `make -C src fulltest` → 669/0/0/18 (flag-off, production).
- **flag-ON** `MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh` → 669/0/0/18 (isolation +,
  once consumption is wired, correctness of covered methods).
- gcc.c-torture failset byte-identical to the 51-name baseline (0 timeouts).
- `--emit=c11` byte-identical vs the prior commit on representative TUs — for a covered
  method this is the tsubst==reparse equivalence proof.

## 6. SETTLED — do not re-litigate
- Hybrid B (concrete shell at parse, tsubst the BODIES at cir-build). NOT full cir-level
  deferral (A), NOT parse-layer copy (C). (PLAN §11.3.)
- Deferral primitive = the PER-CALL dependence test (`call_involves_placeholder`), g++'s
  per-node model — NOT a global "defer everything in a template" flag.
  `dependent_parse_in_progress` is now only the "am I in a dependent parse" gate. Before
  widening eligibility to bodies that MIX dependent + non-dependent template-id calls,
  finish widening per §11.5c (typeless placeholder + ADL bit + real `is_type_dependent`).
- The env-gated flag-on byte-identical harness is THE validator. Keep it until the re-parse
  path is deleted (PLAN §5 trigger).

## 7. WIDENING BACKLOG (after scalar consumption works) — PLAN §11.5c
1. typeless placeholder for a deferred call (madc `unknown_type_node` analogue) + ADL bit.
2. real `is_type_dependent(expr)` predicate from operand types — **DONE 2026-06-25
   (`62409d08`)**; at tsubst re-run the NORMAL call-resolution entry on concrete args
   (reuse, don't fork) — **§8 local instantiation-on-miss is now landed; remaining work is
   retiring the system-header bail and catalog entries deliberately.**
3. then dependent member access (`T::x`), template-ids (`Foo<T>`), packs — one per commit,
   each relaxing a `tsubst_eligible` constraint with the gate green.

## 8. COMPLETED KEYSTONE NOTES — nested fn-template INSTANTIATION in the copy path (2026-06-25)

This was the capability that retired the local `std::forward`/`std::move` name-match:
the tsubst/copy path can now RESOLVE existing overloads and INSTANTIATE a nested
fn-template on demand for local copied dependent calls. The system-header dependent-call
bail (`cir_builder.cpp:653`) and the `tsubst_eligible` catalog entries that mirror it
remain until the real system-header forwarding/destructor/object-address cases are
widened under the same validation. This is the g++ `tsubst → finish_call_expr` model
(it instantiates, not just resolves; pt.cc:22158, `finish_call_expr` semantics.cc:3315).

### 8.1 SETTLED — do not re-litigate
- Step A (generic `is_type_dependent`, the `type_dependent_expression_p`/pt.cc:30357
  analogue) is DONE + committed (`62409d08`). KEEP IT; `call_involves_placeholder` wraps it.
- The old name-match (`identity_forwarding_operand`) was a Rule-#7 violation and is now
  removed. Keep the structural path (`ref_returning_call_type` + `void_addr_of`, the
  `object_arg_addr` cir_builder.cpp:1997 shape); do NOT restore callee-name peeling as a
  "fix."
- The system-header bail (`cir_builder.cpp:653-656`) is LOAD-BEARING BY DESIGN — added in
  `e93b31a1` alongside the re-resolution to EXCLUDE system-header dependent calls that need
  wider validation. Do NOT remove it merely because local nested instantiation landed.
- Nested instantiation WAS required; there was NO shallow slice that avoided it (verified
  2026-06-25 from the bail's provenance — the catalog mirrors this exact gap).

### 8.2 HISTORICAL WIP patch
`tmp/stepC-nested-instantiation-wip.patch` (against `62409d08`) already: (a) removes the
name-match (helper + fwd decl + the `explicit_arg_node` peel → structural
`copy_expr_under(arg)` + `(!ref_returning && nonaddressable ? temp_addr_from_value :
void_addr_of)`); (b) in `rewrite_copied_dependent_call_id` (cir_builder.cpp:659), on a
`find_namespace_function_overload` miss, synthesizes `TokenCallFunc synth(tcf->var)`, sets
`synth.explicit_template_args = explicit_args` (substituted), calls
`m_prog->instantiate_namespace_fn_template_for_call(&synth)`, then re-resolves. It also
carries `[FWD2]` fprintf probes — STRIP THEM. Apply with `git apply`, or re-derive from here.

### 8.3 THE BUG IN THE WIP + THE FIX (root cause, pinned)
The WIP reaches the miss with CORRECT inputs (probe: `ns=std name=forward explicit=[Item]
at=[Item]`) but instantiation DECLINES — `winner=nil` after the call — because the synth
borrows the PATTERN's parameters, which are still PLACEHOLDER-typed (`Args`), so
`try_instantiate_namespace_fn_template`'s deduction is confused.
**FIX:** give the synth CONCRETE-typed parameters matching the substituted arg types `at`
(already computed at cir_builder.cpp:639-650). Build parameter tokens whose `datadef()`
returns `at[i]` — investigate the lightest token type for this (a `TokenVar` over a
throwaway `Variable` of that type, or another token that overrides `datadef()`; see the
`datadef()` overrides in `include/madc.h` ~539-635). Then `instantiate_namespace_fn_
template_for_call(&synth)` registers `forward<Item>` and the re-resolve succeeds.

### 8.4 VERIFY (parser-internals; SLOW recompiles — batch probes)
- Probe `try_instantiate_namespace_fn_template` (parser.cpp:31386) to confirm WHY it
  declines with placeholder params and that concrete params fix it.
- Confirm the 2nd `find_namespace_function_overload(at, &explicit_args)` MATCHES
  `forward<Item>` (arg `Item` vs param `Item&`; the matcher handles value→ref — the
  re-parse path resolves it, so this should too).
- The instantiated body IS compiled by `translate_module`'s `pending_funcs` drain
  (cir_builder.cpp:13947) — confirmed, no extra wiring needed.

### 8.5 GATE (per slice; never perf-gate)
- POC test `CIR: tsubst lowers reference-forwarded class-reference placement-new packs`
  (test_cir.cpp:1640) must pass VIA tsubst: `cir_count_tree1_copies(tree) > 0` AND `got==34`.
- fulltest flag-off AND flag-on (`MADC_XTEST_DEP_PARSE=1`) 669/0/0/18; `test_cir` green.
- 2026-06-25 local update satisfied this gate, stripped probes, and deleted
  `identity_forwarding_operand` for good.

### 8.6 THEN (follow-on slices, each its own gated commit)
Once instantiation works: retire the system-header bail (653-656) and the matching
`tsubst_eligible` catalog entries (parser.cpp:33386) ONE per commit — each previously-
excluded construct now re-resolves+instantiates; `-Wunused-function` confirms dead catalog.

### 8.7 BUILD/PROBE GOTCHAS (each cost real cycles 2026-06-25 — heed them)
- NEVER pipe `make` through `tail`/`head`: it MASKS compile errors AND makes a background
  task report exit 0 (the pipe's code, not make's). Verify:
  `make -C src ../bin/test_cir 2>&1 | grep -iE 'error:|Error [0-9]'` (empty = clean).
- STALE TEST BINARIES: `make -C src` rebuilds `bin/madc` but NOT `bin/test_cir`. Use
  `make -C src ../bin/test_cir`, then VERIFY the rebuild took: `strings bin/test_cir |
  grep -c <probe-string>` (0 ⇒ stale, the `.o` didn't recompile — chase why).
- `node_t` vs `cir_node`: a copied `value` is `node_t` (struct node) → use `value->code`;
  `error_msg` is a `cir_node` EXTENSION → `CIR_NODE(value)->error_msg`.
- Best "why did it fall back" probe: `cir_report_errors(stderr, result)` at
  cir_builder.cpp:12455 — prints every error node with message + source location.
- `DBG()` is thread-dead in cir_builder worker threads — use raw `fprintf(stderr,...)`.
- Run ONE unit test: `LD_LIBRARY_PATH="lib:/usr/local/lib:$LD_LIBRARY_PATH" bin/test_cir
  -tc="<exact test-case name>"`.
