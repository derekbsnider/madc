# tsubst burndown step-2 — local-class-in-tsubst (`_M_construct`'s `_Guard`)

## ★ 2A LANDED (2026-07-01) — call-rebind implemented; reducer is a HIT

The BREAKTHROUGH design (rebind, not new machinery) is implemented and verified.
`tests/testlocalclassraii.mad` (disarmed reducer) is now **1 hit / 0 fallback**
flag-on and prints `42` (== gcc). Flag-on gate GREEN, container tests unchanged,
baseline bumped (`testlocalclassraii 0/1 -> 1/0`).

**What landed (all in `src/cir_builder.cpp` + one member in `cir_builder.h`):**
1. `collect_cir_node_datadefs()` — walks a cir subtree collecting every
   `node->datadef` (discovers the Tree-1 pattern's local classes; a local var's
   type appears on its decl node).
2. In `tsubst_method_body`, before `tsubst_cir(pattern, binding)`: with the
   concrete owner `m_cur_method->owner_class` (probed = `Box_int32_t`), look up
   each pattern local class `pc` (enclosing==NULL) as
   `struct_map[owner->name + "__" + pc->name]` (the g++/line-22675 naming the
   PARSER already built during member-template instantiation). For each match:
   `binding[pc] = concrete` (remaps the local-var TYPE, so the type-driven
   scope-exit dtor injection targets the concrete dtor) AND populate
   `m_tsubst_local_method_remap` (pattern method emit symbol -> concrete emit
   symbol, matched by `method_map` key: ctor `Guard`->`Guard`, dtor
   `~Guard`->`~Guard`). Saved/restored around the copy.
3. In `copy_cir_subtree`, a new N_CALL branch (keyed purely by callee symbol via
   `m_tsubst_local_method_remap`) retargets a raw-copied pattern ctor/dtor call
   to the concrete symbol. This is why the `TokenDecl`-origin ctor call (which
   the existing TokenCallFunc rebind block cannot see) gets fixed.

**Probed ground truth (the facts the design rests on):** the Guard ctor call's
origin is a **`TokenDecl`** (local var `g(this)`), copied RAW -> pattern symbol
`main____pat44__49__Guard__Guard__50` (emittable=0) tripped the completeness
check. The concrete `Box_int32_t__Guard` (+ ctor `Box_int32_t__build__mti__…__Guard__Guard__52`
dtor `…___dtor__53`, both declonly=0/emittable) **already exists in struct_map**,
built at PARSE time by member-template instantiation — independent of the CIR
re-parse fallback, so rebinding is viable when tsubst becomes a hit. Only ONE
un-emittable callee (the ctor); the dtor is injected later, type-driven (hence
the `binding[pc]=concrete` type remap).

**⚠ PRE-EXISTING local-class-dtor bug discovered (SEPARATE track, NOT 2A):**
tsubst flag-on is now **byte-identical to flag-off re-parse on all three
variants** (parse-once law satisfied):
| variant | gcc | flag-OFF | flag-ON |
|---|---|---|---|
| disarmed (reducer) | 42 | 42 | 42 (hit) |
| dtor-runs (`tmp/guard_dtor_runs.mad`) | 0 | SIGSEGV | SIGSEGV |
| throw-unwind (`tmp/guard_throw_unwind.mad`) | caught7/0 | caught7/42 | caught7/42 |
The dtor-runs crash + throw-unwind wrongness happen EQUALLY on the re-parse path
(flag-off never enters the 2A code — `tsubst_method_body` returns at the
`getenv` gate), so 2A introduces **zero regression**; it faithfully mirrors the
fallback. The root cause is BELOW tsubst: the concrete Guard ctor mis-captures
`this` (warning `incompatible argument type for pointer type parameter` at
`g(this)`: concrete ctor param `Box*` vs `Box_int32_t*` receiver) so the stored
`b` is bad and `b->reset()` in the dtor faults. This is a **parser-level local-
class instantiation typing bug**, orthogonal to the burndown. The handoff's
"throw-path runs correctly" bar cannot be met by a tsubst change alone — it needs
this separate fix. Reducers kept in `tmp/`; the committed test stays disarmed.

**NEXT after 2A commit:** (1) Piece 2B — the narrow `<`-gate admission is what
still blocks the REAL `_M_construct` (its `static_cast<size_type>` trips
`tsubst_eligible`), so container tests are UNCHANGED by 2A (expected). (2) The
pre-existing local-class-dtor `this`-capture bug as its own gcc-parity track.

---

**Status:** scoped + RED test landed; core materialization NOT yet implemented.
**Goal:** convert `basic_string::_M_construct<_InIterator>` (and the reduced
`Box::build<U>`) from a re-parse FALLBACK to a parse-once HIT. This is the
single highest-frequency masked fallback (every string-using test) and the
biggest real burndown lever — see
`docs/plans/2026-07-01-templateid-gate-insight-HANDOFF.md` §5.2 and
`docs/plans/2026-07-01-tsubst-kind3-wip-verification-FINDINGS.md`.

## RED test (landed)
`tests/testlocalclassraii.mad` — a `<`-free member template with a local RAII
class (`Guard`: user ctor + non-empty dtor + a pointer member). Isolates the
local-class wall WITHOUT the `<`-gate entanglement (its body has no template-id,
so it passes `tsubst_eligible`). Today:
- flag-off: PASS (prints 42).
- `MADC_XTEST_DEP_PARSE=1 bin/madc --show-stats`: **0 hit / 1 fallback**,
  `[why: tsubst body calls un-emittable symbol]` (the un-materialized
  `Guard::Guard` / `~Guard`). Runs correctly flag-on only because it falls back
  to re-parse.
**DONE for step-2 = `1 hit / 0 fallback` AND prints 42 flag-on**, plus the
`_M_construct` fallback gone from testset/testmap/testsubscript's `[why:]`.

## The two independent capabilities `_M_construct` needs

### Piece 2A — local-class ctor + non-empty dtor materialization (THE core)
`subst_datadef` (cir_builder.cpp:774) materializes a dependent local struct via
`materialize_local_aggregate_datadef` (704) → `clone_local_aggregate_members`
(620), gated by `tsubst_local_class_clone_supported` (602). That gate REJECTS any
local class with a user ctor (`cdd->has_user_ctor`, line 612), non-empty methods,
or a non-empty dtor (only `tsubst_local_class_has_only_empty_dtor`, 589, is
allowed). `_Guard` has BOTH a user ctor and a non-empty RAII dtor → rejected →
`subst_datadef` returns the un-substituted dependent `Guard` → the body's
`Guard g(this)` ctor call + scope-exit `~Guard()` reference un-emitted symbols →
completeness check bails.

**What must be built:** extend the clone to also clone the ctor + dtor (and any
methods), and INSTANTIATE their bodies with the same `subst` map (T→concrete),
registering the instantiated FuncDefs so `emit_class_struct_with_deps`
(cir_builder.cpp:5987 — local classes already route through the class/ctor/vtable
emit path) emits them. Key facts:
- The clone already substitutes MEMBER types (`clone_local_aggregate_members`);
  extend it to clone `src->ctors` / `src->methods` / the dtor and their bodies.
- **No re-parse shortcut.** flag-off "works" only because it RE-PARSES `build`'s
  body (the path the campaign deletes). tsubst must materialize `Guard` from the
  Tree-1 pattern on the parse-once spine — that is the whole point (parse-once.md).
- The ctor/dtor bodies are NOT templates; when `Box<int>::build<int>` instantiates,
  T is fixed, so `Guard`'s `Box*` → `Box<int>*` and `b->reset()` → `Box<int>::reset`.
  Reuse the body-instantiation the main tsubst body already uses (the copy +
  `subst_datadef` walk), applied to the ctor/dtor TokenFuncs.
- **Scope-exit dtor injection** for the materialized local class must fire on the
  tsubst'd body's normal + early-exit paths (the existing dtor-injection machinery).

### g++ recon (2026-07-01) — the authoritative model + the madc gap it exposed
**g++ model (`gcc/cp/pt.cc`, the `TAG_DEFN` case ~line 20196, in `tsubst_stmt`):**
a local class defined inside a template body is NOT an independent template — it is
*"instantiated along with its containing function."* g++ does exactly:
1. `tsubst(TREE_TYPE(t), args, …)` — instantiate the local class TYPE with the
   enclosing args (substituted member types);
2. `complete_type(tmp)` — complete its layout;
3. **eagerly instantiate every member** — `for (fld : TYPE_FIELDS(tmp))
   if (FUNCTION_DECL && !DECL_ARTIFICIAL && DECL_TEMPLATE_INSTANTIATION)
   instantiate_decl(fld, /*defer_ok=*/false)` (and `maybe_instantiate_nsdmi_init`
   for FIELD_DECLs), with `current_class_ptr/ref` saved/restored.
So the ctor/dtor/methods are instantiated EAGERLY at the point the local class is
materialized — not lazily on ODR-use. This is precisely what madc must mirror.

**madc gap the recon exposed:** madc has NO analogue of `instantiate_decl(fld)` for
an *ordinary* (non-template) local-class method. `instantiate_member_fn_template_for_call`
(parser.cpp:33965) only drives member *templates* for a *call* (requires
`fd->is_member_template`). `_Guard`'s ctor/dtor have no template params of their own
(they are dependent only via the enclosing `T`). So 2A is **NEW machinery**, not a
reuse — this is the material finding.

**Concrete implementation shape (grounded):** the body-instantiation primitive is
`tsubst_cir(pattern, binding)` (cir_builder.cpp:14673) — it copy-substitutes a
pattern cir with a binding; `materialize_local_aggregate_datadef` (704) is where the
local class type is materialized. Following the g++ eager model, extend materialize
(or its caller) to, per ctor/dtor/method of `src_class`:
1. clone the method's `FuncDef` onto the clone class with `subst_datadef`'d signature
   + a concrete emit symbol;
2. `tsubst_cir` the method's pattern body with the SAME `binding` → concrete body cir;
3. register the concrete `FuncDef`/body as a pending def so `emit_class_struct_with_deps`
   (5987) emits it;
4. **rebind the enclosing body's ctor/dtor CALLS** — the copied build body (from
   `tsubst_cir`) binds `Guard g(this)` / scope-exit `~Guard()` to the PATTERN's Guard
   methods (the failing symbol probed = `…__pat44__…Guard__Guard…`, the pattern ctor);
   they must resolve to the CLONE's methods so the emitted symbols match. THIS
   rebinding is the subtle sub-problem — verify the copied calls' target FuncDef is
   the clone's, not the pattern's.
Sub-problems to get right, each a potential silent miscompile: (a) call rebinding
[above]; (b) concrete emit-symbol matching between the clone's methods and the body's
calls; (c) scope-exit dtor injection for the materialized class on all exits incl.
the throw path (the `_Guard` dtor IS the throw-path code — a happy-path-only test
will NOT catch a broken unwind; RUN with an exercised catch path or inspect the
emitted SJLJ). Estimated a multi-step build, not a single edit.

### ★ BREAKTHROUGH (2026-07-01 probes) — 2A is CALL REBINDING, not new machinery
Empirical probing of the reducer (`MADC_XTEST_DEP_PARSE=1 --show-stats tmp/guard_reducer.mad`,
env-gated `fprintf` probes since reverted) overturned the "build new method-instantiation"
assumption:
1. `struct_has_dependent_member(Guard)` returns **false** — `Guard`'s member is `Box*`
   (pointer to the enclosing dependent class), and `template_param_under_type_layers`
   only detects a member that is *directly* a template param, not `Box<T>*`. So the
   `materialize_local_aggregate_datadef` path is **never even reached** for `_Guard`-shaped
   locals. (Same for `_M_construct`'s `_Guard`: member `basic_string* _M_guarded`.)
2. **The concrete Guard ctor/dtor ALREADY EXIST and are emittable.** The normal member-
   template instantiation of `build<int>` (the `__mti__` path) creates real-bodied
   (`declaration_only=0`) pending defs:
   `Box_int32_t__build__mti__Box_int32_t__Guard__Guard` (ctor) and
   `Box_int32_t__build__mti__Box_int32_t__Guard___dtor` (dtor).
3. **But the tsubst-copied body CALLS the PATTERN symbol** `main____pat44__49__Guard__Guard__50`
   (emittable=0) — `tsubst_cir` copies the pattern's ctor/dtor call verbatim and never
   retargets it to the concrete Guard the mti already built. That single un-emittable
   callee is what trips the completeness check → fallback.

**So the fix is REBINDING** the copied local-class ctor/dtor calls (and the local var's type)
from the pattern Guard to the concrete `..._mti__..._Guard` the enclosing instantiation
already produced — NOT cloning/instantiating methods (that machinery already runs). This is
a localized `tsubst_cir` change, dramatically smaller than the original 2A estimate.

**Precise next step (small):** in `tsubst_cir`, when copying a call whose target is a
method of a PATTERN local class, remap the target to the concrete instantiation's local
class method. Open questions to resolve first (one more probe): (a) where the concrete
`..._mti__..._Guard` datadef/methods are registered (struct_map? keyed how?), and (b) the
mapping from the pattern Guard (`__pat44__Guard`) to it given the active instantiation
prefix (`Box_int32_t__build__mti`). Likely hook: make `subst_datadef` map the pattern local
class → the concrete one (currently returns it unchanged because shdm=false), so the copied
calls resolve through the concrete class. THEN verify: reducer `1 hit / 0 fallback` + prints
42, AND a THROW-path variant (guard NOT disarmed → dtor disposes) runs correctly flag-on.

### Piece 2B — narrow `<`-gate admission — ATTEMPTED 2026-07-01, classifier CORRECT, REVERTED (exposed a separate map-path bug)

**★ KEY RECON FINDING (overturns the original 2B framing):** the dominant
`[why: template-id '<' in body]` fallback is USUALLY NOT a template-id at all — it
is the **less-than OPERATOR**. The instantiated `_M_construct` is the INPUT-iterator
overload (basic_string.tcc:171), whose only `<` is `while (... && __len < __capacity)`
— a comparison. (The forward overload's only `<` is `static_cast<size_type>`, a
concrete cast.) `tsubst_eligible` rejected EVERY `tkLT` blindly.

**The classifier (proven correct, saved: `tmp/2b-comparison-lt-gate-BACKUP.patch`):**
admit a `tkLT` iff it does NOT open a balanced template-id — reuse
`template_id_suffix_end(d, i)`: it returns the close index for a real `<...>`, or `i`
unchanged when there is no balanced close (a comparison/shift operator). Plus a
`tsubst_lt_opens_concrete_cast` helper (prev token is `static_cast`/`const_cast`/
`reinterpret_cast`/`dynamic_cast`, and the `<...>` span has no nested `<` and no pname
= concrete cast type). This errs SAFELY: a comparison mis-read as a template-id only
stays on the fallback; a **dependent template-id is NEVER admitted** (that was the WIDE
failure). This is the correct answer to the "narrow classifier" the original plan asked
for, and it is NOT the `pnames`-only check that was rightly warned against.

**Result: correct AND partially landed-worthy, but REVERTED because it exposed a
SEPARATE deeper bug.** With it: testset (`set<string>`) 6/4→7/3 and testvector 14/1→15/0
became HITS and RAN CLEAN (rc=0) — the classifier works. BUT testmap / testsubscript /
testcontainerdtor / testmadc_ns gained a hit that **hard-fails to compile flag-on**:
`cir error: no matching constructor for call to 'basic_string_...(const _Guard*)'`.
Probed (`MADC_HIT_PROBE`): the newly-admitted map-only hits are the `pair<const string,int>`
CONSTRUCTION bodies — `_Rb_tree::_Auto_node::_Auto_node`, `__new_allocator<pair<...>>`,
`allocator_traits<...>` — ALL with `remap=0` (so it is **NOT** the 2A local-class code).
One of them tsubst-mis-resolves the const-pair element construction and emits a
`basic_string(_Guard*)` call. This is a **Kind-4 construction bug** (const-qualified
`pair` node construction under tsubst), the same family as the 2A pre-existing
local-class-dtor `this`-capture finding — surfaced only once the body becomes a hit.
The flag-on gate correctly REDs on it (it RUNS the tests).

**★ DEFINITIVE ROOT CAUSE (2026-07-01, traced to the exact call) — the blocker is the
FORWARDING-PACK CONSTRUCTION WALL, not a gate issue:**
- Suite-wide burndown baseline (2A): **176 hit / 90 fallback = 66%**; `template-id '<' in
  body` is **70 of 90** (78%) — 2B's target.
- The ONLY comparison-`<` body 2B admits in these tests is **`_M_construct` itself**
  (`__len < __capacity`, a METHOD not a ctor) — exactly the desired win; it RUNS CLEAN in
  set/vector. There is **NO gate-level dodge**: admitting `_M_construct` makes it emittable,
  which cascades — the `basic_string` range ctor (already eligible, only gated by
  `_M_construct`'s emittability) also becomes a hit, feeding the map's node construction.
- The fatal call, traced via `call_target_funcdef`: the arg to `basic_string(const _Guard*)`
  is **`std::forward`** (`fdn='forward' ns='std'`, callvar `__ns_std_forward__o4`). So the
  pair ctor's `first(std::forward<_U1>(x))` gets **`_U1 = _Guard`**. The `_Args` pack
  forwarded through `_Auto_node(_Args&&...)` → `allocator_traits::construct(...,
  forward<_Args>(args)...)` → `pair(_U1&&,_U2&&)` is NOT substituted (the `_Auto_node(...,
  _Args*)` NO-MATCH — present under 2A too, but there NON-FATAL/discarded). With `_Args`
  unsubstituted, `_U1` falls back to the most-recently-registered type = `_Guard` (created
  when `_M_construct` became a hit). Under 2A `_M_construct` re-parses, timing differs, and
  it resolves correctly.
- CONCLUSION: this is the **forwarding-pack-in-static-call KIND** (named in
  `project_reparse_deprecation`). It must be fixed BEFORE 2B can land. Fixing it = proper
  `_Args` pack substitution through the `allocator_traits::construct`/`_Auto_node`/pair
  chain so `std::forward<_Args>` expands to the concrete arg types (not a "current type"
  fallback). A distinct major KIND — likely a hard-wall grind (see
  `reference_agent_tooling_division`). g++ semantics: each `forward<_Ui>(arg_i)` binds
  `_Ui` from the corresponding deduced pack element; madc must carry the deduced pack
  element types down the construction chain.

**NEXT for 2B:** fix the forwarding-pack construction wall FIRST (above), THEN re-apply the
saved classifier patch (`tmp/2b-comparison-lt-gate-BACKUP.patch`) — it is correct and gives
the set/vector wins immediately, and the map wins once the pack wall is fixed. Do NOT
re-apply the classifier before the pack-wall fix (it hard-breaks testmap/testsubscript/
testcontainerdtor/testmadc_ns). **CAUTION (still valid):** never
widen to admit a balanced/dependent template-id — the WIDE admit-all broke the compile via
`__and_ : conditional<...>::type` (templateid-gate-insight §4b); the classifier's
"reject any balanced `<...>` except a concrete cast" rule is what keeps that safe.

## Guardrails (learned this session — non-negotiable)
- **NEVER harden the fallback net by rolling back the emitted program**
  (`m_prog->ast` / `m_output_externs` / `deferred_lazy_bodies`) — a fallback of an
  OUTER body drops legitimate NESTED hits → runtime setjmp SIGSEGV. HEAD rolls back
  only the ODR-use `referenced_funcs` set (safe). This is what sank the reverted
  Kind-3 WIP.
- **Never mask a bad hit with a bail/guard.** If a materialized local class
  miscompiles, fix the materialization; do not add a screen that demotes it (that
  regresses the burndown AND can hide a crash — see the FINDINGS doc).
- **Verify by RUNNING flag-on, not just compiling.** The `_Guard`/`_M_realloc`
  crash class is happy-path-clean; run the tests (the new `scripts/tsubst_flagon_gate.sh`
  does this). Every increment: build 0-warn, `make fulltest` (incl. the tsubst
  flag-on gate), gcc-torture c17 byte-identical to the 51-name baseline, and the
  reducer HITS + prints 42 flag-on. Bump `docs/parity/tsubst-flagon-baseline.txt`
  on the resulting hit gains.

## First concrete sub-slice
Make `tests/testlocalclassraii.mad` HIT: in `tsubst_local_class_clone_supported`
stop rejecting user-ctor/non-empty-dtor when the ctor/dtor bodies are
instantiable; teach `clone_local_aggregate_members` to clone `ctors`/dtor and
instantiate their bodies with `subst`; confirm the emit path (5987) picks them up.
Gate green + reducer HIT before touching `_M_construct` (2B).
