# HANDOFF — `<=>` track COMPLETE; next = MIR PR #430 review / `===` / eval package C / promote backlog

**Date:** 2026-06-11 (evening) · **Supersedes**
`docs/plans/2026-06-11-spaceship-token-lowering-HANDOFF.md` as the cold-restart
contract (everything still-relevant from it is folded in here).

## STEP 0 — rehydrate

```bash
bash scripts/resume.sh
```
Then read this file fully. TRUST THE LIVE REPO over any memory line.

## 1. State snapshot (verified at write time)

- **Branch:** `feature/template-instantiation-claude` @ `8c510df`, **pushed**, tree clean.
- **develop** @ `94643bc` — **LOCAL-ONLY. NEVER push develop without a `/release` version bump** (user rule 2026-06-11).
- **fulltest:** **572 / 0 / 0 / 18**, exit 0, both check gates GREEN.
- **gcc.c-torture:** 1567/26/29/0/63 — failset **byte-identical** to `tmp/failset_lsq.txt` (55 lines), verified per feature commit (5 full runs this session, all identical).
- **SMAUG soak:** green (`cd /workspace/MadSMAUG/runtime/area; timeout 50 /workspace/madc/bin/madc --project /workspace/MadSMAUG/compile_commands.json -lcrypt 4000` → exit 124 + `Realms of Despair ready at`).
- **MIR fork:** `/workspace/mir` develop @ `2ffebff` == `MIR_COMMIT` (untouched this session).
- Mirrors (claude_status.json / CHANGELOG / ROADMAP / docs/test-status.md / cpp-support.md P2.15 / KG `Feature{spaceship_operator_cpp20}`,`Feature{free_operator_dispatch}` / memory `project_spaceship_operator.md`) synced through `8c510df`.

## 2. What landed this session — the COMPLETE `<=>` compliance track (P2.15)

All feature commits gated individually (fulltest + full torture failset-diff + SMAUG soak), g++-verified tests per slice. Newest first:

| Commit | What |
|---|---|
| `01528ed` | **`= default` comparison synthesis** ([class.compare.default]): defaulted ==/<=> synthesize from the ORDERED member list at class COMPLETION (`synthesize_defaulted_comparison` → `parse_hoisted_friend_operator`; == = memberwise &&-chain, <=> = lexicographic early-return chain; category = partial_ordering if any floating member else strong_ordering, from the parsed `<compare>`; scalar/pointer members only — bases/class-typed members bail loud). TWO trigger sites: defaulted FRIEND (`skipped_friend_defaulted_comparison` at the hoist filter — `<compare>`'s `operator==(strong_ordering,strong_ordering)=default` → ordering-vs-ordering ==/!= work) and defaulted MEMBER (class-body method path @ the `defaulted_or_deleted` continue; **defaulted <=> queues the implicit defaulted == too, p4** — `auto operator<=>(const V&) const = default;` alone yields all six comparisons). Test `testdefaultedcmp_realhdr`. |
| `aff26fa` | **Rewritten candidates** ([over.match.oper]): `Program::rewritten_operator_candidate` at `lower_free_operator_to_call`'s tail (fires ONLY when every direct set missed; recursion bounded by the new `no_rewrite` param — longest chain `!=`→`==`→reversed `==`). `x != y` → `!(x == y)` (a member == serves via raw token + TokenLnot wrap); `x == y` → reversed `y == x`; relationals → `(x <=> y) @ 0` when a member/free operator<=> covers the pair. Only-`<=>`/`==` class idiom = all six comparisons. Test `testrewritten_realhdr`. |
| `7a56d72` | **The `<=>` token lowering** ([expr.spaceship]): builtin scalars lower CIR-side (`CirBuilder::three_way_builtin_lowering`) to a category temp + inline byte-select into `_M_value` (g++ -O0 canon, NO call; operands materialized into typed temps = evaluated once; integral→strong, floating→partial with unordered=**2**). Parse-time `Program::comparison_category_class` types Token3Way as the category CLASS ((a<=>b)<0 dispatches, `auto r = a<=>b` copy-inits; loud include-hint error without `<compare>`). `"<=>"` joined `object_operator_symbol` + `binop_overload_symbol` → class operator<=> rides the machinery (hoisted friends bind `r<=>0` AND reversed `0<=>r`). Test `testspaceship_realhdr`. |
| `5f63a20` | **Hidden-friend operator bodies hoist** ([class.friend]): friend operator DEFINITIONS re-parse at namespace scope after the class completes (`parse_hoisted_friend_operator` — token re-injection + scope isolation mirroring `instantiate_fn_template_binding`). Friend FUNCTIONS modeled: `DataDefCLASS::friend_function_names` (name-based grant); `Program::pending_function_display_name` stamps the FuncDef display name BEFORE the body parses (the grant must match while the hoisted body reads `__v._M_value`). `select_ctor_overload` passes `is_zero_integer_literal` (shared `tokens.h` helper) → the `0` materializes `__cmp_cat::__unspec` through its `__unspec(__unspec*)` ctor. Tests `testhiddenfriend`, `testcompareops_realhdr`. |
| `477d1a0` | **Literal 0 = null-pointer constant** ([conv.ptr]): `score_arg_to_param` gains `arg_is_zero_literal` (rank 3 — below pointer args 4/5, above udc 2), propagated through the converting-ctor recursion; threaded from token-sighted ranking layers. |
| `5c8bb41` | **Free-operator dispatch**: parsed non-member operator functions (user free ops, hoisted friends, prior instantiations) rank via the ONE shared scorer over the union of `"::"+opname`-suffixed `namespace_fn_overload_sets` (`Program::find_free_operator_function`, fed from `lower_free_operator_to_call`) and lower to an ordinary `TokenCallFunc`. Global `operatorX` definitions register per-overload sets under `"::operatorX"`. Comparisons `== != < > <= >=` joined `object_operator_symbol` (string ==/!= now compile via body instantiation as a side effect). Ranking extracted into `rank_fn_overload_candidates` (one impl, also used by `find_namespace_function_overload`). Test `testfreeop`. |

Earlier same-day (previous sessions, already in the superseded handoffs): `<=>` slices 1/2a/2a′ and the template-instantiation batch.

## 3. NEXT (priority order — user-set queue)

### 3a. Upstream MIR PR #430 review (USER-QUEUED 2026-06-11)

https://github.com/vnmakarov/mir/pull/430 — "Fix two RA bugs breaking computed goto
(laddr/jmpi) under the generator" (OPEN, cyanogilvie): (1) `MIR_LADDR` dest missing
`OUT_FLAG` in insn_descs; (2) jmpi edge-splitting clobbers a reg operand with a label
(fix = simplified RA for `MIR_JMPI` fns). Crashes at -O2/-O3 under MIR_gen. madc runs
MIR_gen + the fork carries labels-as-values → torture/SMAUG computed-goto exposure.
Evaluate: does the fork (`/workspace/mir`, derekbsnider/mir `develop`, pin `MIR_COMMIT`)
already cover it? Try the PR's reducer. If adopted: merge to fork develop, push, and
bump `MIR_COMMIT` in the SAME madc commit (pin discipline, `.claude/rules/build.md`).

### 3b. `===` madc-dialect operator (queue item 4)

STD_MADC-gated strict-equality over `madc::value` (does not exist yet: lexer token +
DSL semantics + decide the real-language surface). See `[[libmadc-eval-track]]`.

### 3c. eval package C (queue item 5)

The 38 `test_libmadc_program` skips (AOT save/load, fork-child execution, host
callbacks/register_function, string call marshalling, get/set_global, limit-rejection
shapes) — plan `docs/plans/2026-06-10-libmadc-eval-on-cir-plan.md`.

### 3d. The 41 class-(a) promote-gate backlog

`docs/parity/failset-classification.md` — K&R old-style-definition parsing ×23 is the
biggest lever; promote gate = ≥1608 of 1652 in-scope (`.claude/rules/branching.md`).

### 3e. `<=>` polish (non-blocking, queued in P2.15)

- Precedence corner: `<=>` sits at the relational tier (C++ ranks it between shifts
  and relationals) — unparenthesized `a < b <=> c` groups left; canonical shapes fine.
- Synthesis breadth: defaulted comparisons with BASES or class-typed members
  (recursive member comparison) bail to the loud error today.

## 4. Gotchas (new this session — older ones in memory)

- **`Q a(1), b(2);` HANGS the parser** — multi-declarator with ctor-argument
  initializers loops in parseExprStmt at the comma after the first declarator.
  **Pre-existing** (verified at clean HEAD before this session's changes; sibling of
  the known testfortypedcomma for-init gap). Recorded in claude_status
  known_pre_existing_gaps. Workaround: split declarations. A test that uses the
  shape will hang the suite (runner caps kill it, but it reads as a timeout).
- **`<string>` under `--std=c++20` fails**: `Unknown namespace 'ranges'` (~virtual
  line 616) — a C++20 header region the now-correct `__cplusplus` exposes. Untriaged;
  c++17 unaffected. Will block c++20 TUs that include `<string>`.
- The friend-hoist and defaulted-synthesis parses swallow failures (catch + DBG,
  mirroring template instantiation) — a failed body leaves no overload and the USE
  site errors. Debug with `-v` and grep `parse_hoisted_friend_operator`.
- `function_display_name` is stamped pre-body via `Program::pending_function_display_name`
  (set around parseDeclaration's parseFunction call). Anything needing the SOURCE
  name mid-body reads the FuncDef display name.
- The namespace path sanitizes operator symbols in parse ids (`operator==` →
  `__ns_std_operator_as_as`); the overload-set KEY keeps the real spelling
  (`std::operator==`). Match by display name, never by parse id.
- Synthesized defaulted comparisons take operands BY VALUE (T, T) — matches
  `<compare>`'s friends; dispatch ranks by-value and by-const-ref identically here.

## 5. Gates per change (unchanged discipline)

Capped runs `( ulimit -t 120; timeout 180 … )`, ONE heavy job at a time:
1. `make -C src fulltest` → 572/0/0/18-or-better, exit 0, both check gates GREEN.
2. Parser/CIR changes: full `scripts/run_gcc_testsuite.py`, diff failset vs
   `tmp/failset_lsq.txt` — byte-identical or explained.
3. SMAUG soak (cmd in §1) — exit 124 + ready line.
4. Commit per logical change; push the feature branch; sync mirrors + KG + memory.
5. Reducers MUST run with their original flags (`--std=c++20 --no-embedded-headers`)
   — flagless = embedded path = masks real-header bugs.

## 6. Key artifacts

- `docs/plans/cpp-support.md` **P2.15** — the `<=>` record (all slices logged; track complete).
- Tests (all g++-verified): `testspaceship_realhdr`, `testrewritten_realhdr`,
  `testdefaultedcmp_realhdr`, `testcompareops_realhdr`, `testfreeop`,
  `testhiddenfriend`, `testcompare_realhdr`, `test3waygate`.
- KG: `Feature{spaceship_operator_cpp20}` (complete), `Feature{free_operator_dispatch}`.
- Memory: `project_spaceship_operator.md` (current through completion).
- Reducers (tmp/, gitignored — reconstruct from the tests if gone): `sw_cls.mad`,
  `sw_tok.mad`, `sw_clsop.mad`, `sw_rw.mad`, `sw_def.mad`, `sw_mdef.mad` (all GREEN);
  `spaceship.{cpp,s}` (g++ canon); `strcmp_probe.mad` (string `<` still red — separate
  pre-existing arg-emission wall; ==/!= now compile); `rw7.mad` (the multi-declarator
  hang reducer).

## 7. Mechanism map (where the new machinery lives)

| Mechanism | Anchor |
|---|---|
| Free-operator dispatch | `Program::find_free_operator_function` + `rank_fn_overload_candidates` (parser.cpp, near `find_namespace_function_overload`); fed from `lower_free_operator_to_call` |
| Rewritten candidates | `Program::rewritten_operator_candidate` (parser.cpp, right after `lower_free_operator_to_call`) |
| Friend hoisting | `parse_hoisted_friend_operator` + `skipped_friend_operator_definition` + `skipped_friend_defaulted_comparison` + `synthesize_defaulted_comparison` (parser.cpp ~18250-18600); queue + execution in `TokenCLASS::parse` (friend-skip site + class-completion loop) |
| Friend-function access grant | `DataDefCLASS::friend_function_names` (datadef.h); `function_is_friend_of` / `current_function_friend_name` / `access_flag_violation(cur_function)` (parser.cpp ~11100) |
| Literal-0 NPC scoring | `score_arg_to_param(arg_is_zero_literal)` (cir_builder.cpp ~4273); `is_zero_integer_literal` (tokens.h) |
| `<=>` builtin lowering | `CirBuilder::three_way_builtin_lowering` (cir_builder.cpp, after `class_operator_call`); hook in translate_expr's binary path before the N_* switch |
| `<=>` parse-time typing | `Program::comparison_category_class` + the tk3Way branch atop `resolve_object_operator_type` (parser.cpp) |
