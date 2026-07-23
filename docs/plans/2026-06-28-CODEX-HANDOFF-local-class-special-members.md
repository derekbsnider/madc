# CODEX HANDOFF — local-class WITH special members (the next two-tree lever)

**For:** Codex (GPT-5.5 xhigh). **From:** Claude, 2026-06-28.
**Branch:** `feature/front-end-performance-claude` · **HEAD:** `0b40eac8` · gates GREEN
(flag-off + flag-on 670/0/0/18; suite burndown **175 hit / 90 fallback = 66%**).
**Governing plan:** `2026-06-23-two-tree-cir-materialize-from-ast-PLAN.md` §0 RESUME
(the `✅✅ MATERIALIZE HALF LANDED` block — read it; this doc is the imperative execute-to-done).

---

## ⚡ MISSION (one sentence)
You just made a local **aggregate** class in a member-template body tsubst-**HIT** (`4d954145`,
`c15f2e15`). Now make a local class **with special members** (a user **dtor**, then a user **ctor**
+ a dtor whose body calls a member) materialize too — that is `basic_string::_M_construct`'s RAII
`struct _Guard` and `_Rb_tree::_M_emplace_hint_unique`'s `_Auto_node`, the bulk of the remaining 90
`template-id` fallbacks.

## THE REAL TARGET (libstdc++, grounded)
`/usr/include/c++/13/bits/basic_string.tcc:185` — the shape the suite needs:
```cpp
struct _Guard {
  explicit _Guard(basic_string* __s) : _M_guarded(__s) { }   // user ctor, init-list
  ~_Guard() { if (_M_guarded) _M_guarded->_M_dispose(); }     // user dtor, member-call BODY
  basic_string* _M_guarded;
};
```
`_Auto_node` (rb_tree) is the same KIND: a local guard with ctor + dtor.

## TESTS (staged gates-of-truth — isolated repros, NOT the libstdc++ monster)
**Slice 1 gate — `tmp/localclass_dtor_tsubst.mad`** (the green aggregate reducer + ONLY a user dtor;
differs from the working case by exactly the thing this slice must handle):
```cpp
struct Box {
  template<class U> U pick(U x) { struct Guard { U v; ~Guard() {} }; Guard g; g.v = x; return g.v; }
};
int main(){ Box b; int r=b.pick(77); printf("r=%d\n", r); return 0; }
```
- flag-off → `r=77` ✅ (re-parse, production).
- flag-on (`MADC_XTEST_DEP_PARSE=1 bin/madc --show-stats`) → currently **0 hit / 1 fallback**,
  `[why: tsubst: unsupported dependent local aggregate type marker]`.
- **Slice-1 DONE = flag-on `r=77` as a HIT** (`Box::pick<U>` gone from `[why:]`).

**Slice 2 gate — `tmp/localclass_ctor_tsubst.mad`** (ctor with init-list + method + dtor). ⚠️ **This
reducer currently prints `r=1` flag-OFF — a SEPARATE pre-existing PRODUCTION bug on the re-parse path
(local class with ctor-init-list + method inside a member-template body). FIX THAT FIRST or you have no
correct oracle to validate the tsubst HIT against.** It is NOT part of the tsubst materialize gap —
it reproduces with the env-gate OFF. See "OUT OF SCOPE / SEPARATE BUG" below.

The suite confirm set once both slices land: testset / testmap / testsubscript / testvector flag-on,
and the suite burndown.

---

## ✅ FOUNDATION ALREADY LANDED — extend it, do NOT rebuild
`4d954145` + `c15f2e15` built the aggregate materialize (verified, all gates green). The machinery,
by name (`cir_builder.cpp`):
- `tsubst_local_class_clone_supported(DataDefCLASS*)` **`:547`** — THE GATE. Returns `false` for any
  local class with `methods`/`ctors`/`staticconst`/`method_map`/`bases`/`base_class`/`has_user_ctor`/
  `has_user_dtor`/`has_vtable`/`has_vptr_slot`/`extern_ctor`/`extern_dtor`. **This `return false` is
  exactly why `_Guard`/`_Auto_node` fall back.** This slice widens it (carefully — see below).
- `clone_local_aggregate_members(prog, src, dst, subst)` **`:561`** — clones member fields via
  `subst_datadef` (U→concrete), copies layout/align/bitfield/default-init metadata, `finalize()`.
- `materialize_local_aggregate_datadef(prog, sdd, subst)` **`:645`** — the entry: memoize-by-key
  (`tsubst_local_aggregate_key`), gate via `tsubst_local_class_clone_supported`, clone, register in
  `struct_map`/`datatype_map`, store in `prog->tsubst_local_aggregate_map` (`include/madc.h:1536`).
- `subst_datadef` **`:685`** — calls `materialize_local_aggregate_datadef` when a struct member
  `struct_has_dependent_member` (`:715-720`).
- `tsubst_method_body(tf, fd, &reason)` **`:14104`** (called `:14564`) — the per-instantiation
  Tree-1→Tree-2 body copy+substitute. Synth-dtor completeness: `synth_dtor_syms.insert(...)` `:14307`,
  `emittable(s)` `:14328`.
- Late-struct emission: `late_struct_anchor` **`:15671`**, splice via `emit_class_struct_with_deps` /
  `emit_struct_with_deps` at **`:16412-16433`** — the hook that emits a struct materialized during body
  translation (Pass-0 already ran). Aggregate clones already ride this; the special-member clone must
  too, PLUS emit its ctor/dtor functions.

## THE TASK — widen from aggregate to special-member local class
**(1) Widen the gate `tsubst_local_class_clone_supported` (`:547`) to ADMIT a user dtor (slice 1),
then a user ctor (slice 2).** Do it by SHAPE, not by relaxing everything: admit a local class whose
ctors/dtor/methods are themselves tsubst-materializable (their bodies use `U` only in ways the body
tsubst already handles). Keep rejecting vtable/vptr/bases/extern — those are out of scope. The gate
must stay conservative: anything you can't materialize → `false` → clean re-parse fallback.

**(2) Clone + materialize the special members in the concrete clone** (extend
`clone_local_aggregate_members` / `materialize_local_aggregate_datadef`):
- For the **dtor**: the concrete `Guard__tsubst_<key>` needs a concrete dtor. An EMPTY user dtor can
  reuse the `64fc880d` synth-dtor completeness path (`class_gets_synth_dtor` `:6907`) IF you mark the
  clone so it's treated as synth — OR materialize the user dtor body via `tsubst_method_body` (the
  general answer the real `_Guard` needs: its dtor body calls `_M_dispose()`). Prefer the general
  path; the empty-dtor reducer is just the first checkpoint.
- For the **ctor** (slice 2): clone each ctor, tsubst its body (init-list → member assignments) via
  the same body machinery, register/emit the concrete ctor symbol, and make the body's `Guard g(x);`
  construction resolve to the concrete ctor.
- Register the materialized special-member functions so they EMIT (mirror how `tsubst_method_body`
  requeues ODR-used bodies; the dtor/ctor of a body-time clone must land in the module, not dangle).

**(3) Retarget references in the instantiated body** (already works for fields; extend to calls):
`Guard g;`/`Guard g(x);` decl type → concrete clone; `g.v` → concrete member; the implicit dtor call
at scope exit and the ctor call → the concrete clone's dtor/ctor symbols (not the generic placeholder
ones). The synth-dtor completeness (`64fc880d`) already recognizes a synth dtor symbol — make the
materialized dtor symbol satisfy `emittable()` the same way.

**(4) Admit the body past the dependent-call scan if needed.** `tsubst_pattern_has_dependent_call`
(`:13901`) already lets the aggregate body through; a dtor/ctor body that calls a member (`g.get()`,
`_M_dispose()`) may re-trip it — apply the same field-vs-call discipline `c15f2e15` used. Be surgical;
this scan is load-bearing for genuinely-unsupported dependent calls.

g++ model: a local class in a template is dependent — instantiated WITH the enclosing template,
INCLUDING its member functions (`tsubst` of the class + each of its methods on the substituted args).

## GATE (every commit — correctness, never perf-gate)
1. `make -j4 -C src` clean, no new warnings.
2. flag-off `make -C src fulltest` **670/0/0/18** + drift gates (`scripts/check-no-std-hardcoding.sh`
   GREEN). Production byte-identical by the `MADC_XTEST_DEP_PARSE` env-gate.
3. flag-on `MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh` **670/0/0/18**.
4. `tmp/localclass_dtor_tsubst.mad` flag-on = HIT (slice 1); `tmp/localclass_ctor_tsubst.mad` flag-on
   = HIT **and** flag-off = `r=77` (slice 2, after the separate-bug fix).
5. `scripts/tsubst_burndown.sh` — suite FALLBACK **DROPS** (from 90), NEVER rises (the law,
   `.claude/rules/parse-once.md`). **RE-RUN IT — per-test peeks lie** (a Kind landed green per-test
   but regressed suite-wide earlier this week; the burndown caught it).
6. gcc.c-torture byte-identical (flag-on-only change → by construction; spot-check the failset diff).
7. No callee/template/class-NAME hardcoding (Rule #7) — key on shape (local class with
   materializable special members), never on `_Guard`/`_Auto_node`/`_M_construct`/`_M_dispose`.

## OUT OF SCOPE / SEPARATE BUG (do not conflate — but slice 2 needs it fixed first)
`tmp/localclass_ctor_tsubst.mad` prints **`r=1` with the env-gate OFF** (pure production / re-parse
path). A local class with a ctor init-list + a method, instantiated inside a member-template body, is
mis-compiled on the existing re-parse path — independent of all tsubst work. This must be root-caused
and fixed on its own (3-oracle: gcc/clang vs madc; fix at the deepest layer, never shim) BEFORE slice 2
can be validated, because the tsubst HIT for that shape has no correct oracle until flag-off is `r=77`.
Slice 1 (dtor-only) is unaffected — its flag-off is already correct.

## TOOLS
- **gdb abort-backtrace** (how the real emission path was found for the foundation): drop a
  `getenv("X")`-guarded `abort()` at a suspected site, `gdb -batch` with `set environment`, `run`,
  `bt`. Reliable when a line breakpoint won't bind on a multi-line statement.
- `scripts/tsubst_burndown.sh` — suite-wide HIT/FALLBACK + ranked `[why:]` worklist.
- `--show-stats` `[why: ...]` per fallback names the rejecting reason + sample symbol.
- Iterate against the minimal reducers, then confirm on testset/testmap/testsubscript/testvector.

## SETTLED — DO NOT RE-LITIGATE
- The two trees EXIST; this is widening the materialize, not a rebuild. Hybrid B stands.
- KEEP the foundation: `20b41381` Tree-1 capture, `64fc880d` synth-dtor completeness, the aggregate
  materialize (`4d954145`/`c15f2e15`), the mature `resolve_copied_dependent_call` generic re-resolve.
- DO NOT loosen `tsubst_eligible` to "admit concrete-only `<`" — proven a no-op + breakage this week
  (the gate is a proxy; the capability above is the real work).
- The env-gate keeps flag-off byte-identical — every change is production-safe by construction
  (EXCEPT the separate `r=1` bug, which is already flag-off-broken and must be fixed in the open).

## HANDBACK
When slice 1 HITS (or you wall): one paragraph — reducer result, real-body engagement before/after
(burndown), gate results; if walled, the gdb backtrace + which step (gate-widen / clone special member
/ emit / retarget) declined. Note separately whether you touched the `r=1` production bug. Claude
verifies and sets up the next KIND.
