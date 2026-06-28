# CODEX HANDOFF — local-class materialize half (the dominant two-tree lever)

**For:** Codex (GPT-5.5 xhigh). **From:** Claude, 2026-06-28.
**Branch:** `feature/front-end-performance-claude` · **HEAD:** `2537177d` · gates GREEN (flag-off + flag-on 670/0/0/18; burndown 172/93 = 64%).
**Governing plan:** `2026-06-23-two-tree-cir-materialize-from-ast-PLAN.md` §0 RESUME (read the `🎯 2026-06-28` local-class block — it has the full map; this doc is the imperative execute-to-done).

---

## ⚡ MISSION (one sentence)
Make a member-template body that DEFINES a LOCAL CLASS tsubst-**HIT** by capturing the local class generic in Tree-1 (DONE) and **materializing a concrete per-instantiation local class in Tree-2** — the dominant remaining burndown lever (≈61 `template-id` fallbacks are mostly `basic_string::_M_construct`'s `_Guard`; `_Rb_tree::_M_emplace_hint_unique`'s `_Auto_node` is the same KIND).

## TEST (your gate-of-truth — a 12-line isolated repro, NOT the libstdc++ monster)
`tmp/localclass_tsubst.mad`:
```cpp
struct Box {
    template<class U> U pick(U x) { struct Guard { U v; }; Guard g; g.v = x; return g.v; }
};
int main(){ Box b; int r=b.pick(77); printf("r=%d\n", r); return 0; }
```
- flag-off (`bin/madc`) → `r=77` ✅ (re-parse, production).
- flag-on (`MADC_XTEST_DEP_PARSE=1 bin/madc --show-stats`) → currently `r=77` but **0 hit / 1 fallback**.
- **DONE = flag-on `r=77` as a HIT** (`tsubst bodies ..... 1 hit / 0 fallback`, `Box::pick<U>` gone from `[why:]`). Then the real bodies (`_M_construct`, `_M_emplace_hint_unique`) move fallback→hit and the suite burndown drops.

---

## ✅ FOUNDATION ALREADY LANDED — build on it, do NOT redo
- `20b41381` **Tree-1 capture:** a generic dependent local class is no longer emitted globally.
  Predicate `struct_has_dependent_member(DataDefSTRUCT*)` (`cir_builder.cpp`, near line ~470) skips it
  at the 3 struct-emission sites (incl. the real one found by gdb: `translate_module` Pass-0
  `struct_def` at `cir_builder.cpp:~15249`). Without this, the placeholder member hit
  "unsubstituted template parameter 'U' reached type lowering". KEEP it.
- The two trees EXIST: Tree-1 = the pattern cir (memoized `m_tsubst_body_patterns`, built in pattern
  mode where placeholder types become deferred markers, `append_type_specs:~2197`); Tree-2 = the
  per-instantiation copy+substitute (`copy_cir_subtree` + the `binding` map in `tsubst_method_body`).

## THE TASK — (A) and (B) are ATOMIC (A alone REGRESSES); land them in ONE gated commit.

**(A) Admit the local-class body past the dependent-call scan.**
`tsubst_pattern_has_dependent_call` (`cir_builder.cpp:13546`) currently DEFERS the body: `g.v` is a
`TokenMember` which DERIVES FROM `TokenCallFunc`, so the scan's
`tsubst_datadef_involves_template_param(tc->datadef()) → return true` fires on its placeholder (`U`)
result type. A dependent member-**FIELD** access (not a method call) must NOT bail — tsubst
substitutes it. Distinguish a field-access `TokenMember` from a real dependent call here and let it
through. (Be surgical — this scan is load-bearing for genuinely-unsupported dependent calls.)

**(B) Materialize the concrete local class in Tree-2.**
When tsubst copies the pattern for a concrete instantiation, the local class definition node and every
reference to it must retarget to a per-instantiation CONCRETE clone:
1. Clone the generic local `DataDefSTRUCT`/`DataDefCLASS`; `subst_datadef` (`cir_builder.cpp:425`,
   extend if needed) each member type through the `binding` map (U→concrete); unique name (e.g.
   `Guard__<instkey>`); `addMember` + `finalize()` to recompute layout (`include/datadef.h:485,635`).
2. Register it and EMIT it via the **late-struct splice** — `late_struct_anchor` at
   `cir_builder.cpp:~15309` is the EXISTING hook for structs instantiated during body translation
   (Pass-0 already ran, so a body-time clone splices in after the anchor). Find how late structs are
   inserted there and reuse it.
3. Retarget the body's references: the `Guard g;` decl's type, and the `g.v` member access's owning
   class, must point at the concrete clone (so `g.v` resolves to the concrete `int v` member and the
   default-ctor/dtor symbols are the concrete ones — the synth-dtor completeness from `64fc880d`
   already recognizes a synth dtor symbol).
4. Memoize per `(generic-local-class, binding)` so multiple uses/instantiations don't duplicate.

g++ model: a local class in a template is dependent — instantiated WITH the enclosing template
(`tsubst` of the class + its members on substituted args).

## GATE (every commit — correctness, never perf-gate)
1. `make -j4 -C src` clean, no new warnings.
2. flag-off `make -C src fulltest` 670/0/0/18 + drift gates green (`scripts/check-no-std-hardcoding.sh`
   GREEN). Production is byte-identical by the `MADC_XTEST_DEP_PARSE` env-gate.
3. flag-on `MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh` 670/0/0/18.
4. `tmp/localclass_tsubst.mad` flag-on = HIT (`r=77`, `Box::pick<U>` gone from `[why:]`).
5. `scripts/tsubst_burndown.sh` — suite FALLBACK DROPS (or flat), NEVER rises (the law:
   `.claude/rules/parse-once.md`). RE-RUN IT — per-test peeks lie (a Kind landed green per-test but
   regressed suite-wide earlier this week; the burndown caught it).
6. gcc.c-torture byte-identical (flag-on-only change → by construction; spot-check).
7. No callee/template/class-NAME hardcoding (Rule #7) — key on shape (dependent local class), never on
   `"_Guard"`/`"_Auto_node"`/`"_M_construct"`.

## TOOLS
- **gdb abort-backtrace** (how I found the real emission path): drop a `getenv("X")`-guarded
  `abort()` at a suspected site, `gdb -batch` with `set environment`, `run`, `bt`. Reliable when a
  line breakpoint won't bind on a multi-line statement.
- `scripts/tsubst_burndown.sh` — suite-wide HIT/FALLBACK + ranked `[why:]` worklist.
- The minimal reducer isolates the mechanism from the libstdc++ noise — iterate against it, then
  confirm on testmap/testset/testsubscript/testvector.

## SETTLED — DO NOT RE-LITIGATE
- The two trees EXIST; this is the materialize half, not a rebuild. Hybrid B stands.
- DO NOT loosen `tsubst_eligible` to "admit concrete-only `<`" — proven a no-op + breakage this week
  (the gate is a proxy; the capability above is the real work). Stash `off-plan eligibility-gate
  detour` if you want to see why.
- KEEP the `20b41381` foundation, the `64fc880d` synth-dtor completeness, the mature
  `resolve_copied_dependent_call` generic re-resolve (Kind 1/2/3) — build on them.
- The env-gate keeps flag-off byte-identical — every change is production-safe by construction.

## FILE:LINE MAP (HEAD 2537177d — verify, they drift)
- scan to relax (A): `tsubst_pattern_has_dependent_call` `cir_builder.cpp:13546` (the
  `tsubst_datadef_involves_template_param(tc->datadef())` check inside the TokenCallFunc branch).
- subst hook (B1): `subst_datadef` `cir_builder.cpp:425`.
- emission hook (B2): `late_struct_anchor` `cir_builder.cpp:~15309`; struct emitter `struct_def`
  `cir_builder.cpp:4739`; the foundation skip predicate `struct_has_dependent_member` `~470`.
- pattern build / Tree-2 subst seam: `tsubst_method_body` `cir_builder.cpp:~13712` (pattern build at
  ~13800, binding at ~13822).
- layout: `DataDefSTRUCT::addMember`/`finalize` `include/datadef.h:485,635`.

## HANDBACK
When the reducer HITS (or you wall): one paragraph — reducer result, real-body engagement
before/after (burndown), gate results; if walled, the gdb backtrace + which step (A scan / B clone /
B emit / B retarget) declined. Claude verifies and sets up the next KIND.
