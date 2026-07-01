# tsubst 2B completion — HANDOFF for fresh context (2026-07-01)

> ## ✅ 2B COMPLETE (2026-07-01, same day) — READ THIS FIRST
>
> The classifier landed together with the REAL root-cause fix. **§2's "ctor-context
> receiver/arg swap" theory below is WRONG** — kept only as the historical trail.
> The actual mechanism, proven by an abort-trap backtrace + probes:
>
> 1. Once the classifier makes the basic_string iterator-range ctor tsubst-eligible,
>    its Tree-1 **pattern parse** (`…__o9____pat47`) re-parses the ctor body, which
>    (via the inline `_M_construct` chain) parses the local `struct _Guard` at
>    FUNCTION-LOCAL scope while `Program::instantiating_canonical_spelling` still
>    holds the enclosing instantiation's spelling.
> 2. `TokenCLASS::parse` (and the `TokenSTRUCT` mirror) stamped that spelling onto
>    the new class unconditionally → the pattern-local `_Guard` got
>    `canonical_cpp_spelling = "std::__cxx11::basic_string<char,…>"`.
> 3. `resolve_arg_spelling_datadef`'s canonical-spelling struct_map scan then
>    resolved the basic_string template-id to `_Guard` (key `_Guard` sorts before
>    `basic_string…`), so pair's piecewise-ctor `_Args1` pack deduced to
>    `const _Guard*`, `std::forward<const _Guard&>` was instantiated (`__o5`), and
>    map's `first(std::forward<_Args1>(get<0>(…)))` member-init lowered as
>    `basic_string(const _Guard*)` — the §2 error. Not a receiver swap at all: the
>    ARGUMENT's type was corrupted at parse-time deduction.
>
> **The fix (deepest layer, `Program::instantiating_spelling_applies_here()`):** a
> class/struct DEFINED at function-local (block) scope is a LOCAL class
> ([class.local]) and never takes the instantiating template-id as its canonical
> spelling — gate the stamp on `compounds.empty()` at both TokenCLASS/TokenSTRUCT
> sites. The local class falls to the namespace fallback (`std::__cxx11::_Guard`),
> which cannot collide in the template-id canonical scan.
>
> **Results:** classifier + fix, all gates green — fulltest 674/0/0/16 exit 0;
> flag-on gate PROGRESS on ALL SIX container tests (testset 7/3, testmap 8/3,
> testvector 15/0, testsubscript 29/6, testcontainerdtor 27/6, testmadc_ns 27/6;
> baseline bumped); suite burndown **176/90 (66%) → 237/29 (89%)**, the
> `template-id '<' in body` class 70 → 9. The residual flag-on warnings
> ("incompatible argument type for pointer type parameter", basic_string.h:87 /
> reducer :31) are the PRE-EXISTING concrete-Guard-ctor `this`-capture mistyping
> (§B), a separate track. Remaining top `[why:]` classes: template-id-in-body 9,
> non-type template param 6, >1 pack param 6, `_Auto_node`/`_Rb_tree` no-ctor-match 5.

**Objective:** complete step-2 **Piece 2B** — land the comparison-`<` `tsubst_eligible`
classifier so the burndown improves, WITHOUT breaking the map/container tests.

This handoff supersedes the diagnostic sprawl in
`2026-07-01-tsubst-step2-local-class-HANDOFF.md` (Piece-2B section) — read this first;
that doc is the blow-by-blow trail. **2A is landed and green; this is only about 2B.**

---

## 0. Current state (all committed; tree GREEN)
- `develop` HEAD around `ec38ccbf`; `src/` byte-identical to **2A commit `b1e0b218`**.
- **2A is DONE** (`b1e0b218`): local-class ctor/dtor call-rebind. `testlocalclassraii.mad`
  0/1→1/0 flag-on; all gates green. This handoff does NOT touch 2A — it's the foundation.
- The **2B classifier is CORRECT and saved** at `tmp/2b-comparison-lt-gate-BACKUP.patch`
  (a `src/parser.cpp` `tsubst_eligible` change). It is NOT committed because applying it
  breaks 4 tests via the bug below.
- Suite-wide burndown baseline (2A): **176 hit / 90 fallback = 66%**; the `template-id '<'
  in body` reason is **70 of the 90** fallbacks (the lever 2B targets).

## 1. What the 2B classifier does (and why it's correct)
`tsubst_eligible` (parser.cpp) rejects a member-template body if it contains ANY `tkLT`.
But most such `<` are the **less-than operator** (`while (__len < __capacity)` in
`_M_construct`), not template-ids. The classifier admits a `tkLT` iff it is provably safe:
- `template_id_suffix_end(d, i) == i` → NO balanced close → a comparison/shift operator; OR
- `tsubst_lt_opens_concrete_cast(...)` → a `static_cast<size_type>`-style concrete cast
  (prev token is a cast keyword, span has no nested `<` and no template-param name); OR
- the pre-existing covered pack-expansion carve-out.
It NEVER admits a balanced/dependent template-id (that was the WIDE-gate break via
`__and_ : conditional<...>::type`). This design is confirmed correct.

## 2. THE BUG (definitive root cause — pinned to the exact mechanism)
Applying the classifier makes the **basic_string range ctor**
(`basic_string(_InputIterator,_InputIterator,const _Alloc&)`, basic_string.h:760; its body
calls `_M_construct`) become a **tsubst HIT**. That hit then hard-fails to compile the map:

```
cir error: no matching constructor for call to
  'basic_string_char_std__char_traits_char__std__allocator_char_(const _Guard*)'
  @/usr/include/c++/13/bits/stl_map.h:102
```

Chain, proven by probes (see §4):
1. The range ctor's Tree-1 pattern CONTAINS a `_Guard` local-var declaration (an
   `N_SPEC_DECL` whose type is the pattern `_Guard`) — i.e. it contains `_M_construct`'s
   `_Guard __guard(this)` construction (inlined / present in its pattern).
2. **2A's own code** (`tsubst_method_body`, cir_builder.cpp ~14727) walks the pattern,
   finds pattern local class `_Guard`, looks up the concrete `struct_map["basic_string…_Guard"]`
   (matches because the range ctor's owner is basic_string, same as `_M_construct`'s), and
   adds `binding[pattern_Guard] = concrete_Guard`.
3. During the range ctor's `tsubst_cir` copy, `subst_datadef` maps `_Guard → concrete_Guard`
   (verified: probe `[sd] subst _Guard -> basic_string…_Guard`).
4. The `_Guard __guard(this)` CONSTRUCTION mis-lowers **because the enclosing method is a
   CONSTRUCTOR**: the ctor's own object-under-construction is a `this` (basic_string), and
   the local `_Guard __guard(this)` construction collides with it — the emitted call has
   receiver/arg SWAPPED: `basic_string(const _Guard*)` instead of `_Guard(&__guard, this)`.
   In a METHOD (`_M_construct`, the reducer's `build`) there is no such collision — those
   work (reducer is 1/0, runs correct).

**One-line root cause:** 2A's local-class remap fires for a CONSTRUCTOR body that contains a
`_Guard`-style construction, and ctor-context local-class construction lowering swaps the
receiver/arg, emitting `EnclosingClass(const _Guard*)`.

## 3. What is RULED OUT (do NOT re-investigate these — all probed CLEAN)
- **Parse-time deduction is NOT the leak.** All clean for `_Guard`:
  - recorded pack args (`concrete_type_arg_packs`) at both `instantiate_member_fn_template_for_call`
    (parser.cpp ~34200) and `instantiate_member_ctor_template_for_construction` (~34376);
  - the pair-construction key (ctor_args in `…_ctor_template_for_construction`);
  - the deduction binding site (`try_instantiate_namespace_fn_template`, parser.cpp ~31874);
  - the scalar `concrete_type_args` at both paths.
  The `_Guard` enters **CIR-side**, via 2A's `binding` → `subst_datadef`, NOT parse-time.
- **Range-ctor "cascade" theory is WRONG** (an earlier pass claimed the range ctor became a
  hit only because `_M_construct` became emittable). `_M_construct` FALLS BACK in map (it is
  NOT a hit). The range ctor is the hit and the trigger.
- **Restricting the collector to `N_SPEC_DECL` nodes does NOT help** — the range ctor's
  pattern genuinely contains a `_Guard` `N_SPEC_DECL`, so it's still collected. (This change
  is otherwise a reasonable narrowing but insufficient alone.)
- **Guarding on `!fd->method_display_name.empty()` is a TRAP** — empty display name is common
  (not ctor-specific); it disabled the 2A remap almost everywhere and broke the reducer
  (0/1) and all container hits. Do NOT detect "constructor" that way. Use a STRUCTURAL check
  (is the placeholder Variable in `owner->ctors`?).
- **`MADC_CMP_EXCLUDE=basic_string` bisect**: suppressing comparison admission for
  basic_string bodies makes testmap compile clean; every other file still errors → the
  culprit is exactly the ONE basic_string range-ctor comparison-hit.
- **No gate-only "green + win" exists**: fencing basic_string (or local-class-defining
  bodies, or ctors) yields burndown 176/90 = baseline (a NO-OP classifier). The range ctor
  is the ONLY body the classifier moves suite-wide, and it's the one that breaks. A real
  burndown win REQUIRES fixing the construction lowering (Fix A below), not fencing.

## 4. Reproduction & diagnostics
```
git apply tmp/2b-comparison-lt-gate-BACKUP.patch      # apply the classifier
make -C src
MADC_XTEST_DEP_PARSE=1 bin/madc tests/testmap.mad     # -> the cir error above
bash scripts/tsubst_flagon_gate.sh                     # testset/testvector PROGRESS, map/etc RUN FAILED
```
Probes used (re-add as needed; all env-gated, inert without the env):
- `subst_datadef` (cir_builder.cpp ~770): print when `it->second` name contains `_Guard`
  → shows `_Guard -> basic_string…_Guard`.
- 2A remap loop (cir_builder.cpp ~14737, right after `binding[pd] = cc;`): print
  `m_cur_method->owner_class` + `fd->method_display_name` + `pc->name` → shows the
  **basic_string `<ctor>`** is the remapper.
- Hit list: after the completeness loop in `tsubst_method_body` (~14840), print owner +
  `fd->method_display_name` + `recipe->file:line`. Confirms `_M_construct` is NOT a hit; the
  basic_string range ctor (recipe basic_string.h) IS.
- Reducers in `tmp/`: `guard_private.mad` (local `_Guard` calling a PRIVATE enclosing method
  — falls back cleanly, proves private access alone is safe), `guard_dtor_runs.mad`,
  `guard_throw_unwind.mad`.

## 5. Fix directions
**Fix A (the real fix — enables a burndown win): correct ctor-context local-class
construction lowering.** Make `tsubst_cir`/`copy_cir_subtree` emit `_Guard __guard(this)`
as `_Guard::_Guard(&__guard, this)` even when the enclosing method is a CONSTRUCTOR — i.e.
do NOT let the constructor's implicit object-under-construction be taken as the receiver of
the local-class construction. The bug produces `EnclosingClass(const _Guard*)` (swapped).
Start at the construction node in the range ctor's tsubst'd body: find where the local
`_Guard` object construction is lowered and why, inside a ctor, the enclosing `this` becomes
the receiver. g++ model (pt.cc `TAG_DEFN` line 20196): local classes are instantiated with
`current_class_ptr/ref` saved/restored — the local class's construction is independent of
the enclosing ctor's object. Match that: the `_Guard` construction's receiver is `&__guard`,
period. Once fixed, the range ctor is a safe hit → set/vector win + map compiles; verify the
full gate set.

**Fix B (narrow, green, but likely no net win): scope 2A's remap to the DEFINING method.**
In `tsubst_method_body`'s 2A block (cir_builder.cpp ~14727), skip the local-class remap when
`m_cur_method` is a CONSTRUCTOR — detected STRUCTURALLY (scan `own->ctors` for a Variable
whose `type == <the concrete method FuncDef>`), NOT via `method_display_name.empty()`. This
makes the range ctor fall back (safe, map compiles) while methods (`_M_construct`, reducer)
still remap. WARNING: this alone gives burndown 176/90 (no net win) because the range ctor is
the only mover — commit it only together with Fix A, or as an explicit "green, no-op-for-now"
scope boundary if Fix A is deferred. Do NOT commit a broken tree; do NOT commit a pure no-op
and call 2B done.

## 6. Gates (must all pass before committing 2B)
- Build 0-warn; `make -C src fulltest` exit 0 (includes the tsubst flag-on gate).
- `scripts/tsubst_flagon_gate.sh` GREEN — RUN IT WITHOUT A PIPE (`bash …; echo $?`); a
  `| tail` makes `$?` read tail's exit, not the gate's (this fooled an earlier pass).
- gcc.c-torture c17 byte-identical to the 51-name baseline
  (`docs/parity/torture-failset-current.txt`) — run CLEAN (no concurrent load; memcpy-a* are
  load-induced timeouts).
- Burndown (`scripts/tsubst_burndown.sh`) should IMPROVE past 176/90 on a real Fix-A landing
  (bump `docs/parity/tsubst-flagon-baseline.txt` for any test whose hit count rises).

## 7. Discipline (session-earned; from the resume file + AGENTS.md)
- Commit via `git commit -F -` heredoc; stage files EXPLICITLY; never `git add -A`; never
  stage `mir-debug-support.md`. `develop` is held for `/release` — do not push raw.
- NEVER roll back the emitted program on a fallback (dropped-nested-hit SIGSEGV — the
  reverted Codex Kind-3 WIP). NEVER mask a bad hit with a bail/guard. Verify by RUNNING
  flag-on, not just compiling.

---
---

# APPENDIX — FULL HISTORIC KNOWLEDGE (the whole 2A→2B arc)

## A. Campaign context — the tsubst / parse-once model
- **The law** (`.claude/rules/parse-once.md`): new C++ template support resolves on the
  **parse-once generic spine** (the g++ tsubst model — parse the pattern ONCE, instantiate by
  re-running the generic resolver over the saved tree with substituted args). NEVER re-lex /
  re-parse to instantiate. Re-parse is a TRANSITIONAL fallback, env-gated by
  `MADC_XTEST_DEP_PARSE`, slated for deletion at suite-wide burndown = 0.
- **Two trees**: Tree-1 = the dependent PATTERN (a `TokenFunc`/cir built once with template
  params bound to `DataDefTemplateParam` placeholders, via `build_dependent_pattern`, stored
  on `FuncDef::dependent_pattern`). Tree-2 = the per-instantiation copy produced by
  `tsubst_cir(pattern, binding)` == `copy_cir_subtree(src, &subst)`.
- **The seam** (`CirBuilder::tsubst_method_body`, cir_builder.cpp ~14532): for a concrete
  instantiated member-template method, build its body by tsubst of the Tree-1 recipe instead
  of lowering the re-parsed body. Returns NULL (→ caller falls back to `translate_block`,
  the re-parse) unless capability + `MADC_XTEST_DEP_PARSE` gate is satisfied. On a hit it runs
  a COMPLETENESS CHECK: `cir_collect_call_callees(result)` → every callee must be `emittable()`
  (in pending_funcs with a real body / deferred-lazy / external / synth-dtor); a non-emittable
  callee → `bail_restore("tsubst body calls un-emittable symbol")` → fallback. `bail_restore`
  ONLY restores the `referenced_funcs` set (safe) — it must NEVER roll back the emitted program
  (`m_prog->ast`/`m_output_externs`/`deferred_lazy_bodies`); doing so drops legitimate NESTED
  hits and causes a JIT setjmp SIGSEGV (this sank Codex's reverted Kind-3 WIP,
  `tmp/codex-tsubst-kind3-wip-BACKUP.patch`; findings in
  `2026-07-01-tsubst-kind3-wip-verification-FINDINGS.md`).
- **Metric**: `scripts/tsubst_burndown.sh` (suite-wide HIT/FALLBACK + ranked `[why:]` classes).
  `scripts/tsubst_flagon_gate.sh` (+ `docs/parity/tsubst-flagon-baseline.txt`) is a fulltest
  ratchet that COMPILES AND RUNS the container tests flag-on (RED on crash, hit-drop, or
  fallback-rise). This gate is what catches a bad hit that a flag-OFF fulltest misses.

## B. STEP-2 = local-class-in-tsubst. 2A is DONE; here is its full story.
**Problem:** `basic_string::_M_construct`'s `_Guard` (a local RAII class: `basic_string*
_M_guarded` member, ctor arms, dtor `~_Guard(){ if(_M_guarded) _M_guarded->_M_dispose(); }`
disposes unless disarmed) is the single highest-frequency masked fallback. The reduced shape
is `Box<T>::build<U>` with a local `Guard { Box* b; Guard(Box*):b; ~Guard(){if(b)b->reset();} }`.

**RED reducer (committed):** `tests/testlocalclassraii.mad` (+ `.expect` = `42`) — a `<`-free
member template `Box<T>::build<U>` with a local RAII `Guard` that is DISARMED (`g.b=0`) so its
dtor is a happy-path no-op. Baseline: flag-off PASS (42); flag-on 0 hit/1 fallback (RED target).

**★ THE 2A BREAKTHROUGH (this is the key insight): it was a CALL-REBIND, NOT new machinery.**
Probing (`MADC_LC_PROBE`, since reverted) established:
- `struct_has_dependent_member(Guard)` = FALSE — Guard's member is `Box*` (pointer to the
  enclosing dependent class), NOT directly a template param, so `template_param_under_type_layers`
  misses it and the `materialize_local_aggregate_datadef` clone path is NEVER reached for it.
- The concrete `Box_int32_t__Guard` + its ctor/dtor **already exist** as emittable real-bodied
  pending defs (`Box_int32_t__build__mti__Box_int32_t__Guard__Guard`, `…___dtor`), built at
  PARSE time by member-template instantiation — independent of the CIR re-parse fallback. So
  rebinding to them is viable when tsubst becomes a hit.
- The ctor call's origin is a **`TokenDecl`** (the local var `g(this)`), so it's raw-copied by
  `copy_cir_subtree` and KEEPS the PATTERN ctor symbol (`main____pat44__…_Guard__Guard`,
  emittable=0) — that single un-emittable callee is the whole fallback. The existing N_CALL
  rebind block only handles `TokenCallFunc` origins, so it never saw the ctor.

**2A implementation (LANDED @ `b1e0b218`, all in cir_builder.cpp + 1 member in cir_builder.h):**
1. `collect_local_decl_datadefs(pattern)` — walk the pattern collecting local-class datadefs
   (NOTE: at `b1e0b218` this was `collect_cir_node_datadefs`, collecting from ALL nodes — that
   over-reach is exactly the 2B bug in §2; the fix candidates in §5 tighten it).
2. In `tsubst_method_body` (before the copy): with the concrete owner
   `own = m_cur_method->owner_class`, for each pattern local class `pc` (enclosing==NULL) look
   up `struct_map[own->name + "__" + pc->name]` → concrete `cc`; add `binding[pc] = cc` (remaps
   the local-var TYPE so the type-driven scope-exit dtor injection targets the concrete dtor)
   and populate `m_tsubst_local_method_remap` (pattern method emit symbol → concrete emit symbol,
   matched by `method_map` key: ctor→ctor, `~`dtor→`~`dtor). Saved/restored around the copy.
3. In `copy_cir_subtree`: a symbol-keyed N_CALL branch (before the TokenCallFunc rebind block)
   that retargets a raw-copied pattern ctor/dtor call whose callee is in
   `m_tsubst_local_method_remap` to the concrete symbol — this catches the `TokenDecl`-origin
   ctor call. Result: `testlocalclassraii` 0/1→1/0, prints 42.

**⚠ PRE-EXISTING local-class-dtor bug found while verifying 2A (SEPARATE track, NOT a
regression):** flag-on tsubst is BYTE-IDENTICAL to flag-off re-parse on all three variants —
| variant | gcc | flag-OFF | flag-ON |
|---|---|---|---|
| disarmed (reducer) | 42 | 42 | 42 (hit) |
| dtor-runs (`tmp/guard_dtor_runs.mad`) | 0 | SIGSEGV | SIGSEGV |
| throw-unwind (`tmp/guard_throw_unwind.mad`) | caught7/0 | caught7/42 | caught7/42 |
So 2A introduced ZERO regression. The dtor-runs crash + throw-unwind wrongness are a
PRE-EXISTING bug BELOW tsubst (present equally on re-parse): the concrete Guard ctor mis-types
its `this` capture (`Box*` param vs `Box_int32_t*` receiver → warning "incompatible argument
type for pointer type parameter"), so the stored `b` is bad and `b->reset()`/`_M_dispose()`
faults. The committed reducer stays DISARMED (the common, correct case). This is likely the
SAME family as the 2B ctor-context bug in §2.

## C. g++ recon (authoritative model — /workspace/gcc/gcc/cp/pt.cc)
- **`TAG_DEFN` case, line ~20196** (in `tsubst_stmt`): a local class in a template body is NOT
  an independent template — it is *"instantiated along with its containing function."* g++ does:
  `tsubst(TREE_TYPE(t), args)` → `complete_type(tmp)` → for each member fn (`!DECL_ARTIFICIAL`
  && `DECL_TEMPLATE_INSTANTIATION`) `instantiate_decl(fld, /*defer_ok=*/false)` — EAGERLY, with
  `current_class_ptr`/`current_class_ref` SAVED and RESTORED around the loop.
- **Implications for madc:** (1) local-class members (ctor/dtor) are instantiated eagerly with
  their containing function, in the enclosing scope's ACCESS context (so `_Guard::~_Guard` can
  call the enclosing `basic_string::_M_dispose`, which is PRIVATE); (2) g++ has NO "hit vs
  fallback" distinction — instantiation is uniform + order-stable; (3) the local class's
  construction is INDEPENDENT of the enclosing ctor's object-under-construction
  (`current_class_ptr` is saved/restored) — this is exactly what madc's ctor-context lowering
  violates (§2 Fix A). g++ recon confirms the target semantics; the madc bug is that its tsubst
  HIT diverges from its re-parse FALLBACK in when/how these instantiate.

## D. 2B attempts & failures (chronological — do NOT repeat)
1. **WIDE admit-all `<`** (remove the reject entirely): breaks the COMPILE via
   `__and_ : conditional<...>::type` dependent-base → "auto" base. The `pnames`-only span check
   is insufficient (`conditional<_B1,…>`'s params aren't in the current body's pnames yet are
   dependent). → the classifier must reject balanced/dependent template-ids; only admit
   comparison operators + concrete casts. (This shaped the CORRECT classifier in §1.)
2. **The correct classifier** (comparison + concrete-cast): testset 6/4→7/3, testvector
   14/1→15/0 (WIN, run clean) but testmap/testsubscript/testcontainerdtor/testmadc_ns hard-fail
   compile (`basic_string(const _Guard*)`). → led to the §2 root-cause hunt.
3. **Pack-restriction** (comparison admission only for `pack_count==0`): map still 8/3 broken —
   `_M_construct` is not a pack body; the trigger isn't packs. Disproved the pack framing.
4. **ctor-template exclusion via `owner->ctors`** in `tsubst_eligible`: removed ONE hit
   (8→7) but map STILL broke 7/4 — so the range ctor alone isn't the whole story at the gate
   level, AND this exclusion missed the actual mover. (Structural ctor detection is still the
   right tool for §5 Fix B, just applied at the wrong layer here.)
5. **cast-only** (drop the comparison rule): map GREEN but zero win (testset back to 6/4). The
   comparison rule is where both the win and the break come from, inseparably.
6. **local-class-defining-body fence** (reject comparison admission if body has
   `struct`/`class`/`union`): flag-on gate GREEN, but burndown 176/90 = BASELINE (no-op).
7. **`MADC_CMP_EXCLUDE=<file>` bisect**: `basic_string` → map compiles; all others still error.
   Pinned the culprit to the basic_string range-ctor comparison-hit.
8. **Deduction probes** (`MADC_DEDUCE_PROBE`) at all parse-time binding sites: ALL clean for
   `_Guard` → the leak is CIR-side, not parse-time.
9. **`subst_datadef` probe** (`MADC_SD_PROBE`): `[sd] subst _Guard -> basic_string…_Guard`
   FIRES → the leak is 2A's `binding[_Guard]=concrete`. The `[2Abind]` probe then showed the
   **basic_string `<ctor>`** is the remapper (recipe basic_string.h) — NOT `_M_construct`.
10. **`collect_local_decl_datadefs` (N_SPEC_DECL-only) narrowing**: range ctor STILL remaps
    `_Guard` (its pattern genuinely has a `_Guard` N_SPEC_DECL) → insufficient alone.
11. **`!fd->method_display_name.empty()` ctor guard**: CATASTROPHIC — empty display name is
    common, not ctor-specific; disabled the 2A remap almost everywhere, broke the reducer (0/1)
    and all container hits. Ctor detection MUST be structural (`owner->ctors`), never by name.

## E. Env-var / probe reference (all inert without the env; re-add as needed)
- `MADC_XTEST_DEP_PARSE=1` — enable the tsubst-hit path (production default OFF).
- `MADC_LC_PROBE` — (historic 2A) local-class probes in copy_cir_subtree/subst_datadef.
- `MADC_SD_PROBE` — subst_datadef `_Guard`-substitution + the 2A-bind method attribution.
- `MADC_DEDUCE_PROBE` — parse-time deduction/type-arg `_Guard` probes (all came up clean).
- `MADC_HIT_PROBE` — per-hit owner/method/recipe dump in tsubst_method_body.
- `MADC_CMP_EXCLUDE=<substr>` — bisect: suppress comparison admission by decl-token file.
- `MADC_NO_2A_REMAP` — disable 2A's binding/remap population (swaps the failure mode:
  ON→`basic_string(_Guard*)`, OFF→`'_M_dispose' is a private member`).

## F. Commit trail (develop)
- `b7360e62` step-2 RED reducer `testlocalclassraii.mad`.
- `4d27e5c2` tsubst flag-on gate + baseline (catches crash/hit-drop).
- `dd1c40ad` g++ recon (TAG_DEFN).
- `d84a7d1e` 2A BREAKTHROUGH doc (call-rebind, not machinery).
- **`b1e0b218` 2A LANDED** (the actual code; `src/` = this state is green).
- `6e4ce944`, `e1a91dfb`, `a27d6f78`, `da997865`, `ec38ccbf` — 2B recon docs (classifier
  correct, blocker root-caused; classifier saved `tmp/2b-comparison-lt-gate-BACKUP.patch`).
- Baseline: `docs/parity/tsubst-flagon-baseline.txt` (testlocalclassraii bumped to 1/0).

## G. Related knowledge files
- `2026-07-01-tsubst-step2-local-class-HANDOFF.md` — the blow-by-blow trail (5 passes).
- `2026-07-01-tsubst-kind3-wip-verification-FINDINGS.md` — the reverted Codex Kind-3 WIP.
- `2026-07-01-templateid-gate-insight-HANDOFF.md` — the WIDE-gate break analysis (§4b).
- `tmp/2b-comparison-lt-gate-BACKUP.patch` — the correct classifier, ready to apply.
- `tmp/guard_private.mad`, `tmp/guard_dtor_runs.mad`, `tmp/guard_throw_unwind.mad` — reducers
  (+ `.cpp` gcc oracles).
