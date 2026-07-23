# HANDOFF — template batch DONE, `<=>` through slice 2a′, next = slice 2b (hidden-friend operators)

**Date:** 2026-06-11 · **Supersedes** `docs/plans/2026-06-11-eval-leftovers-done-next-work-HANDOFF.md`
as the cold-restart contract (its §queue items 4/5 remain valid; everything else is here).

## STEP 0 — rehydrate

```bash
bash scripts/resume.sh
```
Then read this file fully. TRUST THE LIVE REPO over any memory line.

## 1. State snapshot (verified at write time)

- **Branch:** `feature/template-instantiation-claude` @ `0e09f06`, **pushed**, tree clean.
- **develop** @ `94643bc` — **LOCAL-ONLY. NEVER push develop without a `/release` version bump** (user rule).
- **fulltest:** **566 / 0 / 0 / 18**, exit 0, both check gates GREEN.
- **gcc.c-torture:** 1567/26/29/0/63 — failset **byte-identical** to `tmp/failset_lsq.txt` (55 lines).
- **SMAUG soak:** green (`cd /workspace/MadSMAUG/runtime/area; timeout 50 /workspace/madc/bin/madc --project /workspace/MadSMAUG/compile_commands.json -lcrypt 4000` → exit 124 + `Realms of Despair ready at`).
- Mirrors (claude_status.json / CHANGELOG / ROADMAP / docs/test-status.md / cpp-support.md P2.15 / KG / memory) synced through `0e09f06`.

## 2. What landed this session (newest first)

| Commit | What |
|---|---|
| `0e09f06` | status: queued review of upstream MIR PR #430 |
| `ab11b6b`/`fcceefb` | **`<=>` slice 2a′** — standalone `#include <compare>` works (g++ chain duplicated: `__cpp_concepts=202002L` @ c++20 floor; constraints CONSUMED never evaluated; requires-clause + trailing-requires + concept-definition consumption; `using NAME = type;` may shadow a registered type name) |
| `8baa2c1`/`c8fdb48` | **`<=>` slice 2a** — `<compare>` category types LIVE (test `testcompare_realhdr` -1 0 1 2 g++-verified). 5 general fixes: `__cplusplus` tracks `--std=` (was PINNED 201703L — every C++20 header region silently preprocessed away) + `__cpp_impl_three_way_comparison`; scope-relative/nested `resolve_namespaced_type_token`; QUALIFIED-tag scoped-enum pseudo-namespaces + `parsePostfixChain` walk; file-scope ctor-syntax decls record `top_decls` (out-of-class static member defs DROPPED their ctor args before); CLASS-typed static member exprs resolve to `Class__member` storage, not the silent-0 `TokenInt` fold |
| `69d1ffa`/`90e5f5c` | **`<=>` slice 1** — token gated at C++20 floor (c++17 lexes `<=` `>`; test `test3waygate`); CIR unhandled-binop default now LOUD error_node (was silent `code = N_ADD` — `a<=>b` compiled as `a+b` in madc mode since the token existed). USER RULING: faithful `<compare>` category types, NO pragmatic-int shape |
| `32cd31e` | cpp-support.md P2.15 created (`<=>` compliance gap + staged plan) |
| `065ac2c`/`e124de5` | **2d** — reference operands resolve as the referenced class: ONE `operand_object_class` helper per layer (Program parse-time ×5 surfaces, CirBuilder lowering ×5 surfaces); fixed `cout << const-string&-param` (bound bogus member streambuf* overload) AND `a+b`-on-reference-params (raw pointer arithmetic). Test `teststrrefparam_realhdr`. **TEMPLATE BATCH (queue item 2) COMPLETE** |
| `4bb8cc9`/`79f5e66` | **2c** — loud no-ctor-match: `class_ctor_call`/`_addr` no-match tails return error_node (class + arg types); no-user-ctor NULLs kept (member-ctor/trivial-copy fallbacks). NEW generic `.expect_err` runner fixture (compile-error tests: nonzero exit + stderr substrings; EXE pass skips). Test `testctornomatch` |
| `b5de674` | the ONE real drop 2c surfaced: `select_ctor_overload` unwraps vfREFERENCE args (`A local(p)` from `const A&` never matched the copy ctor; also dropped allocator<char> copy-construction in materialized operator+/__str_concat bodies). Test `testctorrefarg` |

## 3. NEXT (in priority order)

### 3a. `<=>` slice 2b — hidden-friend operator bodies (`r < 0`)

The ONE remaining wall before the token lowering. Reducer (works through parse; fails at CIR):
```bash
bin/madc --std=c++20 --no-embedded-headers tmp/sw_cls.mad   # r < 0 → "invalid types of comparison operands"
```
(`tmp/sw_cls.mad` = `#include <compare>` first, `strong_ordering r = strong_ordering::less; if (r < 0) ...`. tmp/ is gitignored — reconstruct from this line if gone.)

**g++ -O0 canon** (`tmp/spaceship.{cpp,s}`): `a <=> b` = INLINE byte-select into `r._M_value`
(-1/0/1, NO call); `r < 0` = **CALL the TU-local weak friend**
`_ZStltSt15strong_orderingNSt9__cmp_cat8__unspecE` (strong_ordering by value in dil).
NOT exported from libstdc++.so → mangled-direct W2 CANNOT link it → the **body must compile**.

Three ingredients (all analyzed, none started):
1. **Hoist friend DEFINITIONS to namespace scope.** Friend decls inside a class body are
   skipped wholesale (`parser.cpp` ~19146 → `skip_template_nonclass_declaration` +
   `register_skipped_friend_type`). When the skipped friend is an OPERATOR **with a body**,
   re-queue its tokens for parsing at the ENCLOSING NAMESPACE scope after the class
   completes (the class is registered by then, so `partial_ordering __v` params resolve).
   Precedent for token re-injection: template instantiation's deque-front injection.
2. **Literal `0` scores as a null-pointer constant.** `operator<(strong_ordering, __cmp_cat::__unspec)`
   consumes the `0` via `__unspec`'s `consteval __unspec(__unspec*)` ctor — i.e. the
   C++ null-pointer-constant conversion (integer literal 0 → pointer). `score_arg_to_param`
   (cir_builder ~4328) currently returns -1 for int-vs-pointer. Add the GENERAL C++ rule:
   an integral CONSTANT EXPRESSION of value 0 binds a pointer param (then the
   ctor-conversion path in `pdc->is_object()`/allow_udc covers `__unspec`).
3. **Dispatch to parsed free-operator BODIES.** `class_operator_call` (cir_builder ~6105)
   consults member operators + `try_free_operator_call` (W2 spelling set, mangled-direct)
   — a PARSED namespace-scope `operator<` function (from #1) must also be found. Check how
   user-code free operators bind today (the parser registers `operatorX` namespace
   functions — `lower_free_operator_to_call` / `binop_overload_symbol` path); the hoisted
   friends should ride that, NOT a new mechanism.

Also queued from slice 1: the member-operator `findMethod` FALLBACK in
`select_operator_overload` (cir_builder ~4953) returns the first by-name member when
nothing scores — the same silent-wrong-selection smell 2c killed; consider a loud error
once 2b's free-op dispatch exists (it's what masked `cout << s` pre-2d).

### 3b. `<=>` token lowering (after 2b)

Builtin scalars: parse `a <=> b` (Token3Way, precedence 6, already a TokenOperator);
CIR-lower per canon to a category-typed temp: declare `strong_ordering __t` (integral/
pointer operands) / `partial_ordering` (floating), assign `_M_value` from the byte-select
`(a<b ? -1 : a>b ? 1 : 0)` (floats add the unordered arm `: a==b ? 0 : -127`... NOTE
libstdc++ unordered = **2** per `_Ncmp::_Unordered`, verify in the header). The class
types come from the parsed `<compare>` (require the include in c++20 mode; auto-include
`<compare>` in STD_MADC per the auto-include table). Then class `operator<=>` overloads
ride the (now reference-aware) operator machinery. Test plan: extend `testcompare_realhdr`
or new `testspaceship_realhdr` with `(a<=>b) < 0` shapes vs g++ output.

### 3c. Upstream MIR PR #430 review (user-queued 2026-06-11)

https://github.com/vnmakarov/mir/pull/430 — "Fix two RA bugs breaking computed goto
(laddr/jmpi) under the generator" (OPEN, cyanogilvie): (1) `MIR_LADDR` dest missing
`OUT_FLAG` in insn_descs; (2) jmpi edge-splitting clobbers a reg operand with a label
(fix = simplified RA for `MIR_JMPI` fns). Crashes at -O2/-O3 under MIR_gen. madc runs
MIR_gen + the fork carries labels-as-values → torture/SMAUG computed-goto exposure.
Evaluate: does the fork (`/workspace/mir`, derekbsnider/mir `develop`, pin `MIR_COMMIT`)
already cover it? Try the PR's reducer. If adopted: merge to fork develop + bump
`MIR_COMMIT` in the SAME madc commit (pin discipline, `.claude/rules/build.md`).

### 3d. Queue items 4 / 5 (unchanged from the superseded handoff)

4. `===` STD_MADC-gated strict-equality over `madc::value` (does not exist yet; lexer
   token + DSL semantics + decide real-language surface).
5. eval package C (the 38 `test_libmadc_program` skips) — plan
   `docs/plans/2026-06-10-libmadc-eval-on-cir-plan.md`.
Plus the 41 class-(a) promote-gate backlog (`docs/parity/failset-classification.md`;
K&R parsing ×23 = biggest lever; gate = ≥1608 of 1652).

## 4. Session gotchas (new this session — older ones in memory)

- **Deep-header parse errors mis-attribute position** to the TOP .mad file with a
  VIRTUAL line number. That number ≈ the `-E` stream line (`bin/madc -E` same flags) —
  and often ≈ the REAL header's line (compare:736 `using int64_t` reported as "736").
  Use `-v` (DBG trail) + `-E` + the real header to locate constructs. Worth a proper fix.
- **Concept/requires consumption is TOLERANT, not semantic** — madc defines
  `__cpp_concepts` but treats every constraint as satisfied. If overload selection in
  headers ever DEPENDS on a failing constraint, this lies. Recorded in cpp-support P2.15.
- **`DelimDepth` angle tracking desyncs on comparison `<`** inside braces (compound
  requirements). The concept consumer uses ()/[]/{}-only counting; reuse that pattern.
- **predefined_macros are captured at ONE std** (`scripts/gen_predefined_macros.sh`).
  `__cplusplus` is now derived from `--std=` (`Program::cplusplus_value_for_std`); other
  std-dependent captured macros may have the same staleness class — audit when one bites.
- **`git add -p` is interactive** — stage partial hunks via `git apply --cached` with a
  hand-built patch file (worked cleanly for the b5de674/79f5e66 split).
- Stale `obj/parser.o` carried `-DMADC_DBG_QCALL` from a prior session — `rm obj/parser.o`
  + rebuild if `[FREEOP]` stderr noise appears ([[feedback_stale_test_binaries]] class).

## 5. Gates per change (unchanged discipline)

Capped runs `( ulimit -t 120; timeout 180 … )`, ONE heavy job at a time:
1. `make -C src fulltest` → must be 566/0/0/18-or-better, exit 0, both check gates GREEN.
2. Parser/CIR changes: full `scripts/run_gcc_testsuite.py`, diff failset vs
   `tmp/failset_lsq.txt` — byte-identical or explained.
3. SMAUG soak (cmd in §1) — exit 124 + ready line.
4. Commit per logical change; push the feature branch; sync mirrors + KG + memory.
5. Reducers MUST run with their original flags (`--std=c++17`/`c++20`
   `--no-embedded-headers`) — flagless = embedded path = masks real-header bugs.

## 6. Key artifacts

- `docs/plans/cpp-support.md` **P2.15** — the `<=>` plan of record (slices 1/2a/2a′ logged).
- KG: `Feature{spaceship_operator_cpp20}`, `Feature{loud_no_ctor_match_error}`,
  `Feature{reference_operand_operator_resolution}` (`scripts/kg_query.sh`).
- Memory: `project_spaceship_operator.md`, `project_template_batch_2b.md` (current).
- Reducers (tmp/, gitignored): `sw_cls.mad` (compare-first), `spaceship.{cpp,s,mad}`
  (g++ canon), `rK.mad`/`refplus.mad` (2d, now green), `cmp/p*.{h,mad}` (slice-2a bisection).
