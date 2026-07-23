# CODEX HANDOFF — Kind 2: instantiate operator/templates so the container cluster tsubst-HITS

**For:** Codex (GPT-5.5 xhigh). **From:** Claude, 2026-06-27 night.
**Branch:** `feature/front-end-performance-claude` · **HEAD:** `3659857a` · gates GREEN (flag-on + flag-off 670/0/0/18).
**Strategic plan:** `docs/plans/2026-06-27-two-tree-end-state-and-reparse-deprecation.md` (parse-once, generic, g++ model).

---

## ⚡ START HERE — read this whole file, then execute §TASK. Do NOT re-derive; the root cause is pinned and the dead-ends are listed.

### THE MISSION (one sentence)
Make `std::vector::_M_realloc_insert` (and the rest of the container node-construction cluster) tsubst into a real HIT by **instantiating** the operator/templates they call (`__gnu_cxx::operator-`, `std::move`, `__uninitialized_move_if_noexcept`, `allocator_traits::destroy`) on the concrete operand/arg types — Kind 2 of the parse-once spine — instead of the bodies falling back / poisoning.

### WHY THIS IS THE TASK (proven this session)
- The FIRST container body already tsubst-HITS: `_Rb_tree::_M_create_node` (testmap **5/6 → 6/5**, exit 0). The machinery works.
- It worked because its only un-recorded callee was a **deferred-lazy member** (`_M_get_node`) — fixed by ODR-use recording (`3659857a`).
- `vector::_M_realloc_insert` does NOT hit: its callees are **template instantiations** the tsubst path references but never instantiates → MIR "undefined import". They need **Kind 2 (instantiate them)**, exactly like `1d69ee40` did for system-header free *calls*.

---

## THE EXACT PROBLEM (evidence — do not re-investigate)

Re-apply the eligibility ext (§ELIGIBILITY below), build, run `MADC_XTEST_DEP_PARSE=1 bin/madc tests/testvector.mad`. The MIR undefined-import **dump** (I built it — `madc_cir.cpp:103 cir_dump_undefined_imports`, fires on any link failure) prints exactly:
```
undefined MIR import: __ns___gnu_cxx_operator_mi          (+ __o2)
undefined MIR import: __ns_std___uninitialized_move_if_noexcept_a__o2 (+ __o3)
undefined MIR import: __ns_std_move__o5
undefined MIR import: allocator_traits_std__allocator_int32_t___destroy__mti  (+ string variant)
[7 undefined import(s) total]
```
**Root cause (CONFIRMED, do not re-litigate):** `_M_realloc_insert` tsubst's and references these symbols, but the tsubst path does NOT instantiate their bodies. Worse, the speculative pattern-build creates **declaration-only FuncDef side-effects** (e.g. `std_free_operator_instantiation` makes a decl-only operator FuncDef) that **persist and poison the parsed-body fallback** (the re-parse finds the signature, never emits the body). So neither tsubst nor the fallback defines them → undefined import. **The fix is to INSTANTIATE them (give them bodies), not to fall back.**

Minimal repro: `tmp/vec_recurse.mad` (`vector<string>; push_back(...)`). `tmp/rbtree_recurse.mad` (map). `tmp/veci_recurse.mad` (vector<int>, no-crash control).

---

## YOUR TOOLS (all landed this session — use them)
- **MIR undefined-import dump** (`madc_cir.cpp:103`, auto-fires on link failure): names every missing symbol UNTRUNCATED. This is your primary instrument — it converts "guess and rebuild" into "the dump names the exact symbol → instantiate it." MIR truncates its own error; the dump does not.
- **The validated foundation** (`3659857a`): ODR-use recording (`cir_collect_call_callees` `cir_builder.cpp:15745`), the completeness check + `bail_restore` (`cir_builder.cpp:13551`), the pattern-mode operator guard (`cir_builder.cpp:10620`) and construction guard (`bb0aa065`). These make a body that CAN'T yet re-resolve fall back cleanly. As you make a symbol instantiable, the completeness check stops bailing that body and it becomes a HIT.
- **The Kind-1 model** (`1d69ee40`): `resolve_copied_dependent_call` (`cir_builder.cpp:819`) re-resolves a dependent *call* on substituted arg types + instantiate-on-miss (`instantiate_namespace_fn_template_for_call`, `parser.cpp:33351`) + post-resolve body-availability check. Kind 2 is the same shape for operators/templates.

---

## THE TASK — instantiate, slice by slice (each its own gated commit)

**Slice 2a — free operator `__gnu_cxx::operator-` (THE first, clears the most).**
The operator goes through `class_operator_call` (`cir_builder.cpp:8619`) → `try_free_operator_call` (`8325`) → `std_free_operator_instantiation` (`7940`). That machinery already deduces the overload + mangles + creates a FuncDef — **but currently declaration-only** (no body emitted), which is the poison.
- Make the operator instantiation **emit a real body** for the concrete operand types (instantiate the template body, like `instantiate_namespace_fn_template_for_call` does for free fns), so `func_emit_name(operator)` has a definition in `pending_funcs` with `!declaration_only`.
- Then the completeness check (`13551`) sees it as emittable → `_M_realloc_insert` stops bailing on the operator.
- The pattern-mode operator guard (`10620`) currently DEFERS the operator (emits an error → fallback). Once instantiation works, RELAX that guard for the now-instantiable operator (or route it through the copy-path re-resolve like Kind 1).
- g++ model: `tsubst_expr` CALL_EXPR → `perform_koenig_lookup`/`build_new_op` → `build_over_call` instantiates. (Recon in `2026-06-27-tsubst-construction-deferral-PLAN.md` §4.)

**Slice 2b — `std::move`, `__uninitialized_move_if_noexcept`, `allocator_traits::destroy`.** Same pattern: these are free/static template calls in the body. They likely already route through `resolve_copied_dependent_call` (the Kind-1 path) but produce decl-only FuncDefs. Ensure instantiate-on-miss emits a real body (not just a signature). Use the dump after 2a to see which remain.

**DONE for the cluster:** `tests/testvector.mad` and `tests/testsubscript.mad` exit 0 with engagement UP (vector ≥ 13 hit, the `_M_realloc_insert`/`__uninit_copy` shapes become hits), `[why:]` for those shapes empty.

---

## GATE (every commit — correctness only, never perf-gate)
1. `make -j4 -C src` clean, no new warnings.
2. flag-off `make -C src fulltest` → 670/0/0/18 + drift gates green (production unchanged — flag-off is byte-identical by the `getenv` gate).
3. flag-on `MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh` → 670/0/0/18 (runtime-correct — the suite RUNS the JIT'd code).
4. The 4 container tests exit 0; engagement (`--show-stats`) UP, never down; `[why:]` shrinks.
5. gcc.c-torture byte-identical (flag-on-only change → by construction; spot-check).

---

## SETTLED — DO NOT RE-LITIGATE
- **Parse-once, generic (g++) is the direction.** No per-shape catalog: a fix keyed on a template/callee NAME is wrong (Rule #7) — that's what exploded the old Phase 4. Instantiate by KIND.
- The MIR dump, the construction guard, the completeness check + ODR-use recording, the operator/construction pattern-mode guards are CORRECT and committed — build on them, don't remove them.
- The env-gate (`MADC_XTEST_DEP_PARSE`) keeps flag-off byte-identical — every change is production-safe by construction.
- The eligibility ext (§ELIGIBILITY) is the gate that admits these bodies. It REGRESSES vector/subscript UNTIL Slice 2 lands (they emit undefined imports). Land 2a/2b, THEN commit the eligibility ext in the same series so the gate stays green. Do NOT commit the eligibility ext alone.

## WHAT I ALREADY TRIED TONIGHT — DO NOT REPEAT (these do NOT yield hits; they only make bodies FALL BACK, which is not the goal)
1. ODR-use recording (committed — KEEP, it's why testmap hits). 
2. Completeness oracle excluding declaration-only pending FuncDefs (committed — KEEP).
3. `referenced_funcs` snapshot/restore on bail (committed — KEEP).
4. Pattern-mode operator guard (committed — KEEP, but RELAX once instantiation works).
None of (1)-(4) instantiate the operator/templates — they make un-instantiable bodies fall back cleanly. **Your job is the missing piece: actually instantiate them.** Do not spend cycles trying to make vector "fall back better" — make it HIT.

---

## FILE:LINE MAP (current, HEAD 3659857a — verify, they drift)
- operator instantiation: `std_free_operator_instantiation` `cir_builder.cpp:7940`; `try_free_operator_call` `8325`; `class_operator_call` `8619`; dispatch in `translate_expr` ~`10608`.
- Kind-1 call re-resolve (the model): `resolve_copied_dependent_call` `cir_builder.cpp:819`; instantiate-on-miss `instantiate_namespace_fn_template_for_call` `parser.cpp:33351`.
- tsubst seam: `tsubst_method_body` `cir_builder.cpp:13361`; completeness check + `bail_restore` `~13551`; pattern-mode operator guard `~10620`.
- MIR dump: `cir_dump_undefined_imports` `madc_cir.cpp:103` (called `madc_cir.cpp:397`).
- ODR-use walk: `cir_collect_call_callees` `cir_builder.cpp:15745`.

## ELIGIBILITY EXT — re-apply this exact helper (reverted from parser.cpp; it admits the cluster)
Add before `tsubst_has_member_call_pack_expansion` (~`parser.cpp:33468`) and OR it into `covered_system_header_pack_template_id_body` (~`parser.cpp:33635`):
```cpp
static bool tsubst_has_unqualified_call_pack_expansion(FuncDef *fd)
{
    if ( !fd ) return false;
    const std::vector<TokenBase *> &d = fd->member_template_decl;
    for ( size_t i = 0; i + 1 < d.size(); ++i ) {
        TokenBase *t = d[i];
        if ( !t || t->type() != TokenType::ttIdentifier ) continue;
        if ( !d[i + 1] || d[i + 1]->id() != TokenID::tkOpBrk ) continue;
        if ( i > 0 && d[i - 1] && (d[i-1]->id() == TokenID::tkDot
             || d[i-1]->id() == TokenID::tkDeRef || d[i-1]->id() == TokenID::tkNS) ) continue;
        size_t open = i + 1, close = tsubst_matching_close(d, open, TokenID::tkOpBrk, TokenID::tkClBrk);
        if ( close >= d.size() ) continue;
        if ( tsubst_range_has_pack_expansion(d, open + 1, close) ) return true;
        i = close;
    }
    return false;
}
```

## HANDBACK
When the cluster hits (or you wall), leave a one-paragraph note: which slices landed, engagement numbers, gate results, and — if walled — the dump output + which instantiation path declined. Claude verifies and sets up the next round.
