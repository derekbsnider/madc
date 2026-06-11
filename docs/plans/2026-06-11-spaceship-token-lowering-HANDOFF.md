# HANDOFF — `<=>` slice 2b DONE (hidden-friend bodies), next = the `<=>` token lowering

**Date:** 2026-06-11 · **Supersedes** `docs/plans/2026-06-11-spaceship-slice2b-HANDOFF.md`
as the cold-restart contract (its §3c/3d queue items remain valid; everything else is here).

> **UPDATE (same day, @ `7a56d72`): §3a is DONE** — the `<=>` token works
> (builtin byte-select into a category temp via
> `CirBuilder::three_way_builtin_lowering` + parse-time
> `Program::comparison_category_class` typing + `"<=>"` in both operator
> symbol maps; test `testspaceship_realhdr`, 8 g++-verified shapes;
> fulltest 570/0/0/18, torture failset identical, SMAUG green). The next
> work item is **§3b (rewritten candidates + `= default` comparisons)**;
> details in cpp-support.md P2.15. Precedence corner noted there: `<=>`
> sits at the relational tier, unparenthesized `a < b <=> c` groups left.

## STEP 0 — rehydrate

```bash
bash scripts/resume.sh
```
Then read this file fully. TRUST THE LIVE REPO over any memory line.

## 1. State snapshot (verified at write time)

- **Branch:** `feature/template-instantiation-claude` @ `5f63a20` + mirror-sync commit, **pushed**, tree clean.
- **develop** @ `94643bc` — **LOCAL-ONLY. NEVER push develop without a `/release` version bump** (user rule).
- **fulltest:** **569 / 0 / 0 / 18**, exit 0, both check gates GREEN.
- **gcc.c-torture:** 1567/26/29/0/63 — failset **byte-identical** to `tmp/failset_lsq.txt` (55 lines), verified per ingredient commit.
- **SMAUG soak:** green (`cd /workspace/MadSMAUG/runtime/area; timeout 50 /workspace/madc/bin/madc --project /workspace/MadSMAUG/compile_commands.json -lcrypt 4000` → exit 124 + `Realms of Despair ready at`).
- Mirrors (claude_status.json / CHANGELOG / ROADMAP / docs/test-status.md / cpp-support.md P2.15 / KG / memory) synced.

## 2. What landed this session — `<=>` slice 2b (three commits)

| Commit | What |
|---|---|
| `5c8bb41` | **Free-operator dispatch**: parsed non-member operator functions (user global/namespace free ops, hoisted friends, prior instantiations) rank via shared `score_arg_to_param` over the union of `"::"+opname`-suffixed `namespace_fn_overload_sets` (`Program::find_free_operator_function`, fed from `lower_free_operator_to_call`) and lower to an ordinary `TokenCallFunc`. Global `operatorX` definitions register per-overload sets under `"::operatorX"`. Comparisons `== != < > <= >=` joined `object_operator_symbol` (string ==/!= now compile via body instantiation as a side effect). Ranking extracted into ONE `rank_fn_overload_candidates`. Test `testfreeop`. |
| `477d1a0` | **Literal 0 = null-pointer constant** ([conv.ptr]): `score_arg_to_param` gains `arg_is_zero_literal` (rank 3 — below pointer args 4/5, above udc 2), propagated through the converting-ctor recursion. Threaded from token-sighted ranking layers. Test: `a == 0` selects `operator==(A, const int*)` over `operator==(A,A)`-via-ctor. |
| `5f63a20` | **Hidden-friend operator bodies**: friend operator DEFINITIONS hoist to namespace scope after the class completes (`parse_hoisted_friend_operator` — token re-injection + scope isolation mirroring `instantiate_fn_template_binding`; `skipped_friend_operator_definition` filters bodyless / `= default` / `= delete`). Friend FUNCTIONS modeled: `DataDefCLASS::friend_function_names` (name-based grant); `Program::pending_function_display_name` stamps the FuncDef display name BEFORE the body parses so the grant matches while the hoisted body reads `__v._M_value`. `select_ctor_overload` passes `is_zero_integer_literal` (now shared in `tokens.h`) so the `0` materializes the `__cmp_cat::__unspec` arg through its `__unspec(__unspec*)` ctor. Tests `testhiddenfriend`, `testcompareops_realhdr` (7 ordering shapes incl. reversed `0 > r`, partial unordered — g++-verified). |

**Result:** `bin/madc --std=c++20 --no-embedded-headers tmp/sw_cls.mad` prints `less`.

## 3. NEXT (in priority order)

### 3a. The `<=>` TOKEN lowering itself (P2.15 item 1)

Builtin scalars: `a <=> b` (Token3Way, already lexes + parse-rejects below c++20; the
CIR unhandled-binop default is a loud error today). Lower per g++ -O0 canon
(`tmp/spaceship.{cpp,s}`): declare a category-typed temp (`strong_ordering` for
integral/pointer operands, `partial_ordering` for floating), assign `_M_value` from the
inline byte-select `(a<b ? -1 : a>b ? 1 : 0)`; floats add the unordered arm — NOTE
libstdc++ unordered = **2** (`_Ncmp::_Unordered`), verify in the header. Require
`#include <compare>` in c++20 mode (g++ does); auto-include `<compare>` in STD_MADC per
the auto-include table. Class `operator<=>` overloads should then ride the
(reference-aware + free-operator) machinery — the hoisted friend `operator<=>` bodies
ALREADY parse (they're in the overload sets). Test plan: `testspaceship_realhdr` with
`(a<=>b) < 0` shapes vs g++ output.

### 3b. Rewritten candidates ([over.match.oper]) + defaulted comparisons (P2.15 item 2)

- `r != 0` rewrites to `!(r == 0)` — `<compare>` defines NO `operator!=`; today it
  errors loudly (correct but incomplete). Same machinery later serves `a < b` →
  `(a <=> b) < 0` for class types and reversed `==`.
- `friend constexpr bool operator==(strong_ordering, strong_ordering) = default;` is
  SKIPPED by the hoist filter — ordering-vs-ordering `==` needs defaulted-comparison
  generation.

### 3c. Upstream MIR PR #430 review (user-queued 2026-06-11, unchanged)

https://github.com/vnmakarov/mir/pull/430 — computed-goto RA fixes (`MIR_LADDR` dest
missing OUT_FLAG; jmpi edge-split clobber). Fork runs MIR_gen + labels-as-values →
real exposure. If adopted: merge to fork develop + bump `MIR_COMMIT` in the SAME madc
commit (pin discipline).

### 3d. Queue items 4 / 5 + promote backlog (unchanged)

4. `===` STD_MADC-gated strict-equality over `madc::value` (does not exist yet).
5. eval package C (38 `test_libmadc_program` skips) — `docs/plans/2026-06-10-libmadc-eval-on-cir-plan.md`.
Plus the 41 class-(a) promote-gate backlog (`docs/parity/failset-classification.md`;
K&R ×23 = biggest lever; gate ≥1608 of 1652).

## 4. Session gotchas (new — older ones in memory / the superseded handoff)

- **`<string>` under `--std=c++20` fails**: `Unknown namespace 'ranges'` (~virtual line
  616) — a NEW C++20 header region (`__cplusplus` now correctly 202002L exposes it).
  Untriaged; c++17 unaffected. Will block c++20 programs that include `<string>`.
- The friend-hoist swallows parse failures (catch + DBG, mirroring template
  instantiation) — a friend body that fails to parse silently leaves no overload;
  dispatch then errors at the use site. Use `-v` and grep `parse_hoisted_friend_operator`
  to see which friend failed and why.
- `function_display_name` is stamped pre-body via `Program::pending_function_display_name`
  (set in parseDeclaration around the parseFunction call). Anything else that needs
  the SOURCE name mid-body can read the FuncDef's display name now.
- The namespace path sanitizes operator symbols in parse ids
  (`operator==` → `__ns_std_operator_as_as`); the overload-set KEY keeps the real
  spelling (`std::operator==`). Match by display name, never by parse id.

## 5. Gates per change (unchanged discipline)

Capped runs `( ulimit -t 120; timeout 180 … )`, ONE heavy job at a time:
1. `make -C src fulltest` → 569/0/0/18-or-better, exit 0, both check gates GREEN.
2. Parser/CIR changes: full `scripts/run_gcc_testsuite.py`, diff failset vs
   `tmp/failset_lsq.txt` — byte-identical or explained.
3. SMAUG soak (cmd in §1) — exit 124 + ready line.
4. Commit per logical change; push the feature branch; sync mirrors + KG + memory.
5. Reducers MUST run with their original flags (`--std=c++20 --no-embedded-headers`).

## 6. Key artifacts

- `docs/plans/cpp-support.md` **P2.15** — the `<=>` plan of record (slices 1/2a/2a′/2b logged).
- KG: `Feature{spaceship_operator_cpp20}` (updated), `Feature{free_operator_dispatch}` (new).
- Memory: `project_spaceship_operator.md` (current through 2b).
- Reducers (tmp/, gitignored): `sw_cls.mad` (NOW GREEN — prints `less`),
  `spaceship.{cpp,s}` (g++ canon), `freeop_probe.mad` / `hiddenfriend_probe.mad`
  (promoted to tests), `strcmp_probe.mad` (string < still red — separate arg-emission wall).
