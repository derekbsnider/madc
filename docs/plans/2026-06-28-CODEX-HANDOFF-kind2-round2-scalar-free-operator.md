# CODEX HANDOFF — Kind 2 round 2: instantiate the SCALAR-returning free operator so `_M_realloc_insert` HITS

**For:** Codex (GPT-5.5 xhigh). **From:** Claude, 2026-06-28.
**Branch:** `feature/front-end-performance-claude` · **HEAD:** `c588b182` · gates GREEN (flag-on + flag-off 670/0/0/18; no-std-hardcoding GREEN).
**Strategic plan:** `docs/plans/2026-06-27-two-tree-end-state-and-reparse-deprecation.md` (parse-once, generic, g++ model — 4 KINDS, no per-shape/per-name catalog).
**Supersedes:** `docs/plans/2026-06-27-CODEX-HANDOFF-kind2-operator-template-reresolve.md` (round 1 — partly landed in `c588b182`; this narrows to the ONE remaining gap).

---

## ⚡ START HERE — read this whole file, then execute §TASK. The gap is pinned to one function. Do NOT re-derive; do NOT widen scope.

### THE MISSION (one sentence)
Make `std::vector::_M_realloc_insert` tsubst-**HIT** by giving `CirBuilder::std_free_operator_instantiation` a **third pass** that instantiates a **scalar-returning free operator on class operands** (`__gnu_cxx::operator-(const __normal_iterator&, const __normal_iterator&) -> difference_type`, and its comparison siblings `== < > <= >= !=`), so `class_operator_call` returns a real call instead of NULL — which makes the pattern-mode guard stop deferring.

### WHAT ROUND 1 (`c588b182`) ALREADY DID — build on it, do NOT redo
Codex round 1 landed: re-resolve copied dependent calls after substitution, materialize retained free-operator templates, requeue ODR-used tsubst bodies. Gates stayed green. It did **not** crack `_M_realloc_insert` — engagement is flat (testvector 11 hit / 4 fallback). The remaining wall is narrower and now exactly located (below). Do not re-touch the round-1 machinery; just add the missing pass.

---

## THE EXACT GAP (evidence — do NOT re-investigate)

`MADC_XTEST_DEP_PARSE=1 bin/madc --show-stats tests/testvector.mad` fallback profile:
```
11 hit / 4 fallback
  2x std::allocator_traits::destroy<_Up>        [why: tsubst: unresolved dependent member body]
  1x std::__cxx11::basic_string::_M_construct    [why: template-id '<' in body]
  1x std::vector::_M_realloc_insert<_Args...>    [why: tsubst: unresolved class operator in pattern]   <-- YOUR TARGET
```

**Why `_M_realloc_insert` falls back (CONFIRMED — do not re-litigate):**
- Its body uses `__gnu_cxx::operator-` on two `__normal_iterator` class operands (pointer-difference of iterators).
- `class_operator_call` (`cir_builder.cpp:8764`) → `try_free_operator_call` (`8470`) → `std_free_operator_instantiation` (`8085`) returns **NULL**, so `class_operator_call` returns NULL.
- Back in the binary-operator path (`~10784`), `class_operator_call` returned NULL, so the **pattern-mode guard at `cir_builder.cpp:10793`** sees class operands and emits `error_node("tsubst: unresolved class operator in pattern")` → the whole `_M_realloc_insert` body bails and falls back.

**Why `std_free_operator_instantiation` returns NULL (THE ROOT — this is the fix site):**
It has exactly **two passes**:
- **Pass 1** (`~8147`): `ret_is_ref` ONLY — the reference-returning stream shape (`operator<<` → `ostream&`). `operator-` returns a value, so `if (!ret_is_ref) continue;` skips every candidate.
- **Pass 2** (`~8198`): `!best && !member_callee && as_class_instance(rhs_dd)` — the by-value **class**-return shape (`string operator+(...)`). `operator-(iter,iter)` returns `difference_type` (a **scalar** ptrdiff_t), not a class, so Pass 2's class-return matching never binds.

So a free operator that **takes class operands but returns a scalar** matches neither pass → NULL. That family is iterator arithmetic/comparison: `operator-` (difference), and `== < > <= >= !=` (all return `bool`). **No name-keying** — the discriminator is "free operator overload, class operand(s), scalar return."

The MIR undefined-import dump (round-1 evidence) confirmed the missing symbol: `__ns___gnu_cxx_operator_mi` (Itanium `mi` = `operator-`).

---

## YOUR TOOLS (all landed — use them)
- **MIR undefined-import dump** — `cir_dump_undefined_imports` (`madc_cir.cpp:103`, auto-fires on any link failure): names every missing symbol UNTRUNCATED. Your primary instrument after each build.
- **The two existing passes** in `std_free_operator_instantiation` (`8085`) are your TEMPLATE — Pass 2 (by-value class return, `~8198`) is the closest shape. Copy its structure: deduce the overload against operand spellings, build `targs`, mangle via `itanium_mangle_std_free_template`, create the FuncDef inst. The DIFFERENCE: the return is a scalar `difference_type`/`bool`, not a class — set the FuncDef return datadef to the scalar, and `best_retc = NULL` / `best_ret_ref = false`.
- **`class_operator_external_call`** (called at `8547`) is the emit path — once you return a non-NULL `inst`, `try_free_operator_call` already routes through it. Verify it tolerates a scalar return (it emits an N_CALL whose value is the scalar; no DEREF, no retbuf).
- **Round-1 ODR-use recording + completeness check** (`bail_restore` `~13680`, `cir_collect_call_callees` `~15966`): once the operator resolves, the completeness check stops bailing `_M_realloc_insert` and it becomes a HIT. You do not need to touch this.

---

## THE TASK — one pass, gated commit

**Slice — add Pass 3 to `std_free_operator_instantiation` (`cir_builder.cpp:8085`): scalar-returning free operator on class operands.**
1. After Pass 2, add a pass that matches a `free_operator_overloads` candidate where: `ov.opname == mname`, both params are class/by-ref-class spellings that deduce against the operand classes, and the **return is a scalar** (not `&`, not a class — `difference_type`, `ptrdiff_t`, `bool`, `long`, etc.).
2. Deduce the template args from BOTH operands (the iterator template params), mangle the symbol, create the inst FuncDef with the **scalar return datadef**.
3. **Emit a real body** if the symbol is a template (libstdc++ does NOT export instantiated template operators — a `declaration_only` extern would re-create the undefined-import poison). Instantiate the body the same way round-1 "materialize retained free-operator templates" does for the value-return operators; reuse that path — do NOT add a second materialization path (Rule #7 / no-parallel-impl).
4. The guard at `10793` needs **no edit** — once `class_operator_call` returns the real call, the guard's `if (ov) return ov;` at `10784` fires and the deferral is never reached.

**DONE for this slice:** `tests/testvector.mad` `_M_realloc_insert` is a HIT (drops out of the `[why:]` fallback profile), engagement UP (vector ≥ 12 hit, ideally 13+), all 4 container tests exit 0. The `allocator_traits::destroy` and `basic_string::_M_construct` fallbacks are OUT OF SCOPE for this slice (separate KINDs — leave them).

---

## GATE (every commit — correctness only, NEVER perf-gate)
1. `make -j4 -C src` clean, no new warnings.
2. flag-off `make -C src fulltest` → **670/0/0/18** + drift gates green (flag-off is byte-identical by the `getenv` gate — production unchanged).
3. flag-on `MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh` → **670/0/0/18**.
4. `bash scripts/check-no-std-hardcoding.sh` → GREEN (no per-type/per-callee/per-operator-NAME hardcoding — the pass keys on shape: class operands + scalar return, never on `"operator-"` or `"__gnu_cxx"` literals).
5. `--show-stats` engagement UP on testvector, never down; `_M_realloc_insert` gone from `[why:]`.
6. gcc.c-torture byte-identical (flag-on-only change → by construction; spot-check).

---

## SETTLED — DO NOT RE-LITIGATE
- **Parse-once, generic (g++) is the direction.** Add the pass by KIND (class-operand scalar-return free operator), never by callee/operator name. A fix that greps for `operator-` or `__gnu_cxx` is WRONG (Rule #7).
- The MIR dump, construction guard, completeness check, ODR-use recording, pattern-mode operator guard, and round-1 re-resolve machinery are CORRECT and committed — build on them, do not remove or fork them.
- The env-gate (`MADC_XTEST_DEP_PARSE`) keeps flag-off byte-identical — every change is production-safe by construction.
- Scope is ONE pass for ONE KIND. Do NOT also chase `allocator_traits::destroy` (dependent member body — different KIND) or `basic_string::_M_construct` (template-id-in-body — different KIND) in this slice. One gated commit, one KIND.

## FILE:LINE MAP (HEAD c588b182 — verify, they drift)
- **FIX SITE:** `std_free_operator_instantiation` `cir_builder.cpp:8085`; Pass 1 `~8147`, Pass 2 `~8198`. Add Pass 3 after Pass 2.
- emit path: `class_operator_external_call` called at `cir_builder.cpp:8547` (inside `try_free_operator_call` `8470`).
- dispatch: `class_operator_call` `cir_builder.cpp:8764`; binary-op path + the guard that must stop firing `~10784`/`10793`.
- completeness / ODR-use (no edit needed): `bail_restore` `~13680`, `cir_collect_call_callees` `~15966`.
- MIR dump: `cir_dump_undefined_imports` `madc_cir.cpp:103`.

## HANDBACK
When `_M_realloc_insert` hits (or you wall): one paragraph — engagement before/after (testvector hit/fallback), gate results, and if walled, the dump output + which deduction/mangle step declined. Claude verifies and sets up the next KIND (`allocator_traits::destroy`).
