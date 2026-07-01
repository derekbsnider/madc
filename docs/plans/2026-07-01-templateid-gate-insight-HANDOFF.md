# tsubst `template-id '<' in body` — INSIGHT + retargeting (2026-07-01)

**For:** whoever grinds the tsubst burndown next (likely Codex).
**Branch:** develop (tsubst work is env-gated `MADC_XTEST_DEP_PARSE`, production byte-identical).
**Supersedes** the optimistic decomposition in `2026-06-27-two-tree-end-state-and-reparse-deprecation.md`
lines 123-129 (the "concrete class-param id / static_cast are safe, big win" claim). Read this first.

## 1. Fresh burndown (HEAD `1193b7c3`, 2026-07-01)
```
674 tests exercise tsubst · 175 HIT / 90 FALLBACK · 66% hit
 70 [why: template-id '<' in body]   <- 78% of all fallbacks
  6 non-type template param · 6 >1 pack param
  5 no matching constructor ... _Rb_tree ...   (Kind 4 construction)
  1 reference-param value-read · 1 recipe parse failed
```
Progress since 06-28 baseline (104→90 fallback, 60→66%). `template-id '<' in body` is STILL 70.

## 2. THE INSIGHT — the 70 is a *first-blocker* tally, not an *only-blocker* tally
`tsubst_eligible` (`parser.cpp:33785`) rejects on the FIRST `<` (tkLT) it sees in the body
(`return no("template-id '<' in body")`). `[why:]` records that first rejecting clause. So the 70
counts bodies whose FIRST blocker is a `<` — it says NOTHING about whether a `<`-admit would make
them HITS. Measured the actual bodies behind the 70 (`--show-stats sample=`, string/map/set tests):

| body | recurs in | real blocker BEHIND the `<` |
|------|-----------|------------------------------|
| `basic_string::_M_construct<_InIterator>` | EVERY string-using test (dominant) | **local class** `_Guard` RAII in the tsubst'd body (plan §7 note) — NOT a concrete template-id |
| `_Rb_tree::_M_insert_<_Arg,_NodeGen>` / `_M_insert_unique<_Arg>` / `operator()<_Arg>` | every map/set test | dependent template-ids → **rebind / dependent member-type** (Kind 3, Slice D) + construction (Kind 4) |

The plan ASSUMED the 70 were dominated by concrete-arg template-ids (`Foo<int>`) and `static_cast<>`
that a gate refinement would flip to hits. **The data refutes that.** The 70 are concentrated in a
handful of DISTINCT hard bodies (`_M_construct`, `_Rb_tree::_M_insert_*`), each recurring across
hundreds of tests. Their `<` masks a DEEPER capability gap (local-class, rebind, construction).

## 3. Consequence for slice ordering
Refining the `<`-gate to admit concrete/cast template-ids will NOT drop 90→~20. It will shift those
bodies' `[why:]` from "template-id in body" to the deeper reason (un-emittable symbol / construction
/ local-class) and they FALL BACK anyway (cleanly — Slice-A completeness check `3659857a` + the
construction guard have landed, so no SIGSEGV). Net fallback ~unchanged. **Gate refinement alone is
a near-zero win.** The real levers are the capabilities BEHIND the `<`:
1. **local-class-in-tsubst** (`_Guard`) → unlocks `basic_string::_M_construct` = the single
   highest-frequency body (every string test). Likely the biggest real drop. START HERE.
2. **rebind / dependent member-type** (Kind 3 / Slice D, `subst_datadef` at `cir_builder.cpp:425`) +
   **construction deferral** (Kind 4 / Slice B) → unlocks the `_Rb_tree` family (map/set).

## 4. Pending experiment (confirms §2/§3 — CHECK RESULT before acting)
Env-gated bypass added at `parser.cpp:33785` (`&& !getenv("MADC_TRY_WIDE_ELIG")`, marked
`// EXPERIMENT: measure the ceiling`). Building. To read the ceiling:
`MADC_XTEST_DEP_PARSE=1 MADC_TRY_WIDE_ELIG=1 bin/madc --show-stats tests/testset.mad`
(and testmap/testsubscript/testvector). **If** the `[why:]` shifts to deeper reasons / fallback
stays ~flat / any test SIGSEGVs → §2 confirmed, gate-refinement is not the lever → **REVERT the
experiment line**, retarget to §3.1 (local-class). **If** fallback drops sharply → the plan was
right after all; keep it and do the structural gate refinement (classify each `<`: admit when the
`<`..`>` span holds no param name, or it's a cast; reject dependent-arg template-ids — Rule #7: no
callee-name keying). Result recorded below when the build completes:

> **RESULT (CONFIRMED 2026-07-01, build `beb4evykz` exit 0).** Bypassing the `<`-reject did NOT
> convert fallbacks to hits — it **BREAKS THE COMPILE**. `MADC_TRY_WIDE_ELIG=1` on testset /
> testmap / testsubscript all exit **1** with `error: '_M_dispose' is a private member of
> basic_string...` (the half-tsubst'd string body). The `[stats]` totals collapse (testsubscript
> 28h/7f → 5h/2f, testset 6h/4f → 1h/1f) because those are PARTIAL stats printed before the abort.
> §2/§3 CONFIRMED: the 70 are masked deeper blockers; gate refinement is a near-zero win AND unsafe.
> Experiment line REVERTED (tree clean); the built binary still carries the inert env-gated line —
> rebuild when starting real work (env-gated ⇒ normal invocations are baseline-identical).
>
> **BONUS FINDING — completeness-check gap:** Slice-A (`3659857a`) catches un-emittable *symbols*
> at the MIR layer but NOT *access-control* cir errors (`_M_dispose is private`) from a half-tsubst'd
> body — so an over-admitted body ESCAPES to break the compile (exit 1) instead of falling back.
> Before ANY further eligibility widening, the completeness check must also treat a cir access/error
> in the tsubst'd body as "reject → fallback" (it already threads `cir_first_error_msg`; extend it to
> gate admission, not just report `[why:]`).

## 4b. ROOT CAUSE of the WIDE break (gdb `catch throw`, 2026-07-01) — NOT what §4 assumed
The `_M_dispose is private` throw does **not** come from a tsubst'd body accessing a private member.
gdb backtrace (env-gated experiment binary, `MADC_TRY_WIDE_ELIG=1 testset.mad`):
```
throwbuf::sync (the Throw)  <-  TokenCLASS::parse   <- TokenSTRUCT::parse <- parseKeyword
  <- instantiate_template_use <- instantiate_template_id <- fold_constant_qualified_member
  <- parse_constant_* (primary..lor..ternary) <- fold_nontype_arg_constant
  <- instantiate_template_use <- instantiate_template_id
```
Mechanism: admitting `_M_construct` shifts the instantiation set so a **NON-TYPE template-arg
constant fold** (`fold_nontype_arg_constant` → `fold_constant_qualified_member` →
`instantiate_template_id`) drives a **nested template instantiation** that re-parses a class body
(`TokenCLASS::parse`). During that nested re-parse the **access-control context (`cur_class`,
i.e. `code->method->owner_class`) is not established**, so accessing basic_string's OWN private
`_M_dispose` from within its own method *spuriously* trips `access_flag_violation`
(`parser.cpp:14966`), raised as a hard `Throw(tb)` at `parser.cpp:17772` — an instantiation path
with **no try/catch** (unlike `build_dependent_pattern`'s recipe parse, which DOES catch at ~33923).
This uncatchable throw is almost certainly why Codex's two widening attempts died.

**SMOKING GUN (gdb `-g`, break at the throwing line).** The FATAL chain's *first* throw is NOT the
access violation — it is **`"Unknown base class 'auto'"` at `parser.cpp:25205`**, thrown while
`instantiate_template_use(tname="__and_")` (`parser.cpp:4287`) re-parses **`std::__and_`** (libstdc++'s
variadic conjunction trait) via `TokenCLASS::parse`. `__and_`'s base is `conditional<...>::type` — a
**dependent member-type**. In the nested constant-fold instantiation context madc cannot resolve
`conditional<...>::type`, so the dependent base degrades to the placeholder name **`"auto"`** →
"Unknown base class 'auto'". (The `_M_dispose is private` message seen without gdb is a LATER/secondary
throw; the `__and_`/`auto` base-resolution failure is the primary one that catch-throw stops on first.)

**What this proves:** admitting the masked `<`-in-body bodies pulls in libstdc++ **trait templates
whose bases are dependent `::type` expressions** (`__and_ : conditional<...>::type`, and the whole
`__and_/__or_/__not_/enable_if/conditional` SFINAE family). Resolving those needs the
**dependent-member-type / `typename` KIND (Kind 3)** — which `subst_datadef` (`cir_builder.cpp:425`)
explicitly LACKS ("NO dependent member-type resolution ... the rebind gap"). So the real capability
behind the `template-id '<' in body` wall is **Kind 3 (dependent `::type` / rebind resolution)**,
sharper than "local-class first." The degraded `"auto"` base name is the tell that a dependent base
type reached class-parse unresolved.

**Deepest-layer fix (Rule #2):** implement Kind-3 dependent-member-type resolution so a dependent
base like `conditional<_B1::value, _B2, _B1>::type` resolves once its scope is concrete (g++
`make_typename_type`→`lookup_member`, plan §4/§6.3), instead of degrading to `"auto"`. Secondary:
the nested `instantiate_template_use`-via-`fold_nontype_arg_constant` path must not throw uncatchably
on an unresolved base — degrade to a clean fallback (the fallback-net hardening) so widening is
safe to iterate.

## 5. CONFIRMED retargeting for Codex (do in THIS order)
0. **Kind 3 — dependent member-type / `::type` / rebind resolution is THE unlock (§4b smoking gun).**
   Admitting the `<`-in-body bodies pulls in libstdc++ trait templates with dependent bases
   (`__and_ : conditional<...>::type`, the `__and_/__or_/__not_/conditional/enable_if` family); madc
   degrades the unresolved dependent base to `"auto"` → "Unknown base class 'auto'". Implement the
   `make_typename_type`→`lookup_member` analogue in `subst_datadef` (`cir_builder.cpp:425`) so a
   dependent `::type` base resolves once its scope is concrete. Reduce with
   `MADC_TRY_WIDE_ELIG=1 bin/madc tests/testset.mad` (re-add the env-gated line first — it was reverted).
   Also harden the `fold_nontype_arg_constant → instantiate_template_use → TokenCLASS::parse` path so
   an unresolved base degrades to a CLEAN FALLBACK, never an uncatchable `Throw` (this is the exact
   uncatchable-throw class that killed the prior widening attempts).
1. **Harden the fallback net FIRST** (the bonus finding): a cir error in the tsubst'd body ⇒ clean
   fallback, never a broken compile. This de-risks every subsequent widening (Codex's prior two
   reverts were exactly this class — widen → break → revert; with a real net, widening is safe to
   iterate). Small, high-leverage, no new capability.
2. **local-class-in-tsubst** (`_Guard` RAII) ⇒ unlocks `basic_string::_M_construct` = the
   single highest-frequency masked body (every string test). Biggest real fallback drop. This is
   the actual "template-id in body" lever, mislabeled by the first-blocker tally.
3. **rebind / dependent member-type (Kind 3) + construction deferral (Kind 4)** ⇒ the `_Rb_tree`
   family (map/set). Machinery map: `2026-06-27-tsubst-construction-deferral-PLAN.md` §3.5/§6.3.
4. Re-run `scripts/tsubst_burndown.sh` after each; the 70 should now drop as bodies become HITS
   (not merely shift `[why:]`). Do NOT key any fix on a callee/template name (Rule #7).
