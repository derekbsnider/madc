# MADC REHYDRATION HANDOFF — 2026-06-08 (post-compaction, self-contained)

> **READ-FIRST, COLD-START DOCUMENT.** Assume you remember nothing. This file is
> written to be exhaustive on purpose — after reading it (plus the rules in
> `AGENTS.md`/`.claude/rules/`) you can resume the real-header / `<type_traits>`
> track without re-deriving anything. It supersedes the older entry point in
> `docs/plans/2026-06-07-realheader-track-HANDOFF.md` for *current* state (that doc
> remains valid for the per-fix history of the 2026-06-07 lexer/PP work and the
> struct/class unification directive in its §6).

---

## 0. THE 60-SECOND ORIENTATION

- **Repo:** `/workspace/madc`. **Branch:** `feature/realhdr-parse-gaps2-claude`
  (off `develop`). **HEAD:** `60c7e18`. **Working tree:** clean.
- **90 commits ahead of `develop`.** `develop` is **untouched** by all this work.
  **DO NOT push. DO NOT `/promote` develop→master.** (Parity gate — see §3.)
- **fulltest baseline:** `531 passed / 4 failed / 0 timed out / 26 skipped`.
  The **4 failures are PRE-EXISTING and unrelated** to anything in this arc:
  `testdefer`, `testfstream`, `testlargesizeofquery`, `testloop`. "Green" means
  *exactly those four and no others*.
- **gcc.c-torture baseline:** `1566 passed`, **88 hard fails** (31 compile + 57
  runtime) + 1 flaky timeout. This is the regression gate for any parser/cir change.
- **SMAUG soak baseline:** boots to `"Realms of Despair ready at address madc-dev
  on port <N>"` with **11 warnings** (the documented C89-looseness ones) and
  **0 errors**.
- **MIR fork pin (`MIR_COMMIT`):** `2ffebff`. (Fork at `/workspace/mir`, branch
  `develop`. NOT upstream MIR.)
- **What was just accomplished (this arc):** 5 gated feature commits advancing the
  C++ `<type_traits>` / template-instantiation frontier. **What's next:** finish the
  `std::is_*<T>::value` keystone (two precise gaps, §7.A) and/or extend the trait
  set + the header-partition plan (§7).

### Mission, in one paragraph
madc ("My Advanced Dialect of C") is a C/C++ dialect compiler. Pipeline:
`source → lexer → parser → cir_node tree (MC11-IR, == c2mir node_t) → c2mir → MIR
→ JIT` (also `--emit=c11` renders the same tree to portable C). The active track is
making madc **parse REAL system C++/libc headers** (libstdc++/glibc, unmodified) en
route to **C23/C++23 compliance** (the north star). The immediate objective on this
branch is the **string / iostream / sstream / fstream / `<type_traits>`** family.

---

## 1. READ-ORDER ON RESUME

1. **This file, top to bottom.** It has the full state, the session arc with code
   anchors, the methodology, and the precise next steps.
2. **`AGENTS.md`** (loaded as `CLAUDE.md` via `@AGENTS.md`) + the **Top-10 Rules**
   inside it, plus the rule files under `.claude/rules/` that touch your task
   (especially `gcc-methodology.md`, `clang-methodology.md`, `design-principles.md`,
   `pre-edit-checklist.md`, `no-parallel-implementations.md`, `testing-fulltest.md`).
3. **`docs/plans/2026-06-07-realheader-track-HANDOFF.md`** — the prior entry point.
   Its **COMPACTION ENTRY POINT** block (top) was refreshed to HEAD `a2d3f98`; its
   **§6** holds the struct/class unification directive (a separate, still-open
   workstream); its UPDATE blocks hold the per-fix history of the 2026-06-07
   lexer/preprocessor work (decltype-`<`, embedded-stub shadowing, recursive-macro
   loop, multi-line comment, empty-arg stringize, `--no-embedded-headers`, `-E`).
4. **`docs/plans/2026-06-07-template-id-disambiguation-research.md`** — has a
   **✅ RESOLVED** banner (partial spec landed) followed by the **⚠ CORRECTION**
   section: the full evidence-based diagnosis that the `iterator<>` blocker was
   *partial specialization*, not the originally-hypothesized template-id-vs-alias
   bug. Worth reading for *how* the misdiagnosis was caught (instrumentation).
5. **`madc-header-partition-handoff.md`** (repo root) — THE MASTER PLAN for the
   header-partition strategy (option B: madc owns only the freestanding headers +
   impersonates gcc; consumes real libstdc++/glibc unmodified). 6 steps + acceptance
   tests + non-goals.
6. **`docs/plans/2026-06-07-freestanding-vs-hosted-headers-strategy.md`** — the
   principle + madc's current-state mapping + the c2mir-borrow / `__madc__`
   refinements.
7. Background only if needed: `docs/plans/2026-06-07-parser-pp-architecture-research.md`
   (gcc/clang/c2mir front-end study), `docs/adr/0001-cir-c2mir-backend.md` (backend
   decision), `docs/plans/madc-vision-and-invariants.md` (I1–I8 invariants).
8. **Memory** (`/home/dev/.claude/projects/-workspace-madc/memory/`): start at
   `MEMORY.md`, then the most relevant: `project_template_instantiation`,
   `project_parser_pp_architecture`, `project_struct_is_class`,
   `project_string_as_class`, `project_cpp_mangled_direct`,
   `project_north_star_c23_cpp23`, `feedback_correct_over_shortcuts`,
   `feedback_two_canon_compilers`, `feedback_dont_ignore_warnings`,
   `feedback_emitc_gcc_parity_oracle`.

---

## 2. ENVIRONMENT, BUILD, TEST, BACKEND

### 2.1 Build
```bash
make -C src                 # build bin/madc + lib/libmadc.{a,so}; regenerates
                            #   embedded headers + predefined macros at PARSE time
make -C src clean           # remove objects
make -C src test            # doctest unit tests only
make -C src fulltest        # unit + ALL integration tests (the gate)
```
Build needs `clang++`/`g++` with C++11 and the **madc MIR fork** at `/workspace/mir`
(branch `develop`, pinned `MIR_COMMIT=2ffebff`). The fork carries native `_Complex`,
`__attribute__((cleanup))`, ≤16-byte SIMD, the scope-depth decl-layout fix, and the
ABI fixes the CIR backend depends on. **Not** upstream MIR.

If your change is core-parser/compiler only and doesn't touch `madcdat`/storage,
`./configure --enable-madcdat=no` shrinks the rebuild; re-enable before final
validation if you touched shared/storage surfaces.

### 2.2 Useful madc flags (your daily tools)
- `--std=c++17` (or `c++11`) — dialect. The **test runner uses the default
  (STD_MADC) mode** for `.mad` files with no `.flags` fixture; the C++ template /
  trait tests in this arc all pass in default mode (verified), so no `.flags`
  needed.
- `--emit=c11` — render the cir_node tree as C ("what c2mir sees"). The fidelity
  oracle: a gcc-compiled `--emit=c11` of a program must match the g++-compiled
  original C++ (`feedback_emitc_gcc_parity_oracle`).
- `-E` — **preprocess-only** dump (expand `#include`/`#define`/macros, print the
  token stream, stop). THE bisection instrument (see §6.1).
- `--no-embedded-headers` — diagnostic: make madc fall through to the REAL system
  headers instead of its `include/madc/` stubs (reuses the existing
  `registration_policy`/`is_embedded_header_allowed()` gate).
- `--dump-source` — preprocessed token stream. `--dump-cir` — dump the tree.
- `-v` / `--verbose` — sets `madc_verbose`; enables all `DBG(...)` output. NOTE the
  DBG stream split: some DBG goes to `cout`, some to `cerr`. When instrumenting,
  print to `std::cerr` and redirect `--emit=c11`'s stdout to `/dev/null` so the
  emitted C doesn't drown your trace (see §6.2).

### 2.3 The validation gate trio (run for EVERY parser/cir change)
1. **fulltest** — must stay `531/4/0/26` (exactly those 4 reds).
   ```bash
   make -C src fulltest 2>&1 | grep -E "passed,|FAIL:" | tail -6
   ```
2. **gcc.c-torture failset** — must stay `1566 passed / 88 hard fails`. Run in
   background (~15–20 min on this QNAP):
   ```bash
   python3 scripts/run_gcc_testsuite.py --root gcc_testsuite --madc bin/madc
   # tail line: "1566 passed, 31 compile-failed, 57 runtime-failed, 1 timed out, 30 skipped"
   ```
   For a rigorous regression check, diff the *failset* vs the parent build (stash,
   rebuild parent, run, restore, rebuild, run, `comm -13`). For C++-only changes
   that are provably inert for C (e.g. everything in this arc — the new code only
   fires on C++ template/trait constructs), the count-match + known-suspects check
   is sufficient; note the reasoning in the commit.
3. **SMAUG C89 soak** — must boot clean. Use a RANDOM free port (this box is shared
   by several agents; "Address already in use" is a port collision, NOT a madc bug):
   ```bash
   cd /workspace/MadSMAUG
   P=$((6000 + RANDOM % 800))
   MADC=/workspace/madc/bin/madc MADC_CPU_LIMIT=0 MADC_MEM_LIMIT=0 \
     timeout 600 ./MadSMAUG.sh $P > /tmp/smaug.log 2>&1 &
   # wait for "ready at address" / "error:" with an until-loop; expect:
   #   "Realms of Despair ready at address madc-dev on port <P>." + 11 warnings + 0 errors
   ```
   The 11 warnings are genuine C89 looseness (9 `grub.c` `int*`-vs-`bool*` arg
   mismatches + 2 `magic.c` int/pointer comparisons) that `gcc -Wall` also flags —
   madc is correctly aligned with gcc, NOT over-strict. **Do NOT silence them.**

### 2.4 Background-task & shell gotchas (learned the hard way)
- A **bare foreground `sleep`** is blocked by the harness. To wait on a condition
  use a Bash **until-loop** (`until <check>; do sleep 10; done`) or `run_in_background`.
- `grep -c` / `grep -cE` **buffers** — it emits only the final count at EOF, so a
  piped SMAUG boot shows nothing until it finishes. Pipe to a log file and `tail`/
  `grep` the file instead when you want to observe progress.
- Don't queue two full SMAUG boots in one command (wasteful — each is a ~158k-LOC
  parse). One boot, observed via a log, is enough.
- Per AGENTS.md: **single commands, no `&&` chains** (avoids permission-prompt spam).

### 2.5 Backend (settled — do not re-litigate, ADR 0001)
`madc parser → cir_node (MC11-IR == c2mir node_t) → c2mir → MIR → JIT` is the SOLE
backend (asmjit removed). Direct-MIR is a scalpel for runtime internals only.
`--emit=c11` is a first-class third output. New language features get a **C11
lowering** (Tier 1) by default; raise c2mir (Tier 2) or MIR (Tier 3) only for true
primitives with no faithful C11 form. See `.claude/rules/lowering-vs-raising.md`.

---

## 3. BRANCH / PROMOTION / PIN DISCIPLINE

- Feature branches off `develop`; agent-owned WIP suffixed `-claude`. This branch is
  `feature/realhdr-parse-gaps2-claude`, **local only, NOT pushed**.
- **Do NOT promote develop→master** until `develop` reaches feature parity with
  `master`. `master` still carries the removed asmjit backend at full C89 parity;
  `develop` (CIR backend) is climbing back. SMAUG running is a *milestone*, not
  parity. The parity gate is CIR integration coverage ≥ master's
  (`project_cir_parity_campaign`, `docs/parity/`, `docs/adr/0001`).
- `/release` cuts a versioned release ON `develop` for major milestones only — not
  individual bug fixes (`feedback_release_cadence`).
- **Pin discipline:** if madc starts depending on new MIR fork commits, merge them
  to the fork's `develop`, push, and bump `MIR_COMMIT` in the SAME madc commit.
  This arc added NO new MIR dependency (all front-end work) — pin unchanged.
- **Never `git checkout` over uncommitted work** (`feedback_never_lose_code`). Commit
  early; use `#ifdef` guards or `git stash`. (Lost an uncommitted `ctype.h` edit this
  way in a prior session.)

---

## 4. THE SESSION ARC — 5 FEATURES, IN DETAIL

All on `feature/realhdr-parse-gaps2-claude`. Every commit was validated with the full
gate trio (fulltest + torture + SMAUG) before the next began. Commit order:

```
9c6c6c2 feat(template): partial specialization (template<class T> struct X<pattern>)
7930a81 feat(headers): define __madc__ identity macros (header-partition Step 0)
45db326 feat(traits): type-trait builtins __is_class/_union/_enum/_base_of/_same
0950f1e fix(class): capture integral static-const member values
a2d3f98 feat(traits): fold trait builtins in CONSTANT-expression context too
```
(plus `11814f4`, `7da9e1f`, `5042355`, `e2867b7`, `60c7e18` = docs.)

### 4.1 `9c6c6c2` — Template partial specialization

**The presenting symptom.** `<string_view>` derailed instantiating
`basic_string_view<char>` with `parser.cpp:...: error: Expecting a type argument to
iterator<>`. The prior session's handoff "verified" the cause as a
template-id-vs-type-alias disambiguation bug at `parser.cpp:2935-2942` (a member alias
`iterator` being hijacked by the namespace template `std::iterator`).

**That diagnosis was WRONG — caught by instrumentation, not assumption (Rule #4).**
I added temporary `DBG(std::cerr ...)` at the two throw sites (`parser.cpp:2341` and
`:2644`) plus `resolve_typename_type_token` and saw: the live throw is at **2341**
(never reaches 2935-2942); the failing template arg token is literally **`typename`**.
The real chain:
```
using string_view = basic_string_view<char>
  → instantiate basic_string_view<char>
  → using const_reverse_iterator = std::reverse_iterator<const_iterator>
  → instantiate reverse_iterator<const char*>
  → its base clause: public iterator<typename iterator_traits<_Iterator>::iterator_category, …>
  → resolve arg `typename iterator_traits<const char*>::iterator_category`
  → iterator_traits<const char*> resolves to the EMPTY PRIMARY template
    (instrumented: owner=iterator_traits_char_  member=iterator_category  alias=NULL
     incomplete=0  depsurface=0)
  → no iterator_category member → resolve_typename_type_token returns NULL
  → the arg to iterator<> is NULL → throw at 2341
```
Root cause confirmed with minimal reducers + g++ oracle:
`template<class T> struct tr{...}; template<class T> struct tr<T*>{...};` then
`tr<char*>` → **g++ selects the `<T*>` partial spec, madc used the PRIMARY for the
pointer too** (a silent correctness bug). madc handled *explicit* specialization
(`template<> struct box<int>`) but **silently discarded PARTIAL specs** and always
instantiated the primary.

**Where it was dropped.** `TokenTEMPLATE::parse` (parser.cpp). A partial spec has
`specialized_template_id == true` AND `typeparams` **non-empty**. The explicit-spec
branch fired only for `specialized_template_id && typeparams.empty()`; the primary
branch only for `!specialized_template_id`. A partial spec matched NEITHER → its
captured body returned without registering anything.

**The crucial simplification.** madc **monomorphizes** templates (token-substitute +
re-parse on use, Borland-style — see `project_template_instantiation`). So by the time
a partial spec is matched at instantiation, the concrete args are already concrete
DataDefs — **no dependent-placeholder machinery needed**.

**The fix (deepest layer, no per-type hardcoding):**
- `TemplateDef` (include/madc.h:1082) gained `bool is_partial_specialization` and
  `std::vector<std::vector<TokenBase *>> spec_pattern` (the pattern token sequence per
  arg slot, e.g. `["T","*"]` for `X<T*>`); ctor at madc.h:1096 inits the bool.
- New member `std::map<std::string, std::vector<TemplateDef>> partial_spec_map;`
  (madc.h:1119) — **kept OUT of `template_map`** because `register_template` merges
  same-namespace variants and would clobber the primary.
- Registration: `TokenTEMPLATE::parse` now captures the spec pattern tokens during
  spec-arg collection (parser.cpp:**20442** `spec_pattern_tokens`, populated at
  **20466**) and, for the partial-spec case, sets the fields + pushes into
  `partial_spec_map` (parser.cpp:**20542-20544**), before the existing explicit-spec
  branch.
- Matching: `instantiate_template_use` calls **`match_partial_specialization`**
  (parser.cpp:**2478**, declared madc.h:**1124**, defined **9799**) right after the
  concrete args are resolved and `registered_mangled` is computed but before the cache
  check. On a match it does `td = *spec; subst = spec_subst; token_subst.clear();` so
  the spec's body + deduced params drive the rest; identity (mangled/canon) stays keyed
  on the concrete args, so caching is unchanged and the path is **inert when no partial
  specs are registered**.
- Unification: file-static **`unify_spec_pattern_arg`** (parser.cpp:**9741**) handles
  the shapes real headers use: `[cv] PARAM [*]*` (DEDUCIBLE — peel the pattern's
  pointer levels off the concrete type via `DataDefPTR::base_type`, bind PARAM to the
  remainder) and a fully-concrete pattern (must spelling-equal the concrete arg).
  Specificity score = pointer/cv structure peeled (`__is_base_of`-style: more
  structure ⇒ more specialized); `match_partial_specialization` picks the max-score
  match and requires every own typeparam to be deduced. v1 bails (returns NULL → falls
  back to primary) when any slot is a non-type param (`type_args.size() !=
  arg_spellings.size()`), so no wrong match on `template<int N>`-style specs.

**Validated:** fulltest 528→529/4 (+`testpartialspec`); torture 1566/88 identical
(inert for C); SMAUG ready/0-err; `<string_view>` advanced PAST the `iterator<>`
blocker (`iterator_traits<char*>` now matches its `_Tp*` spec — specs=2). New test
`tests/testpartialspec.mad` (direct pointer-spec selection + the dependent
`it_traits<T*>` path) matches g++ (`0 1 42`).

**GOTCHA discovered & documented:** madc's **JIT does not propagate a `return
method()` value through `main`'s exit code** — even for a NON-template class. So
`int main(){ return obj.v(); }` gives exit 0 regardless of what `v()` returns. My
early reducers used exit codes and gave false "JIT=0 vs emit=1" divergences that were
purely a test artifact. **Verify method results via OUTPUT (printf) or emit+gcc, never
exit code.** (`--emit=c11 | gcc | run` was correct throughout.)

**SEPARATE pre-existing bug noted (NOT touched):** *explicit* specialization use-site
mangling is wrong — `tr<int>` mangles to `tr_int32_t` (canonical name) but the explicit
spec registered as `tr_int` (arg spelling), so the explicit spec body is not selected
at the use site. Orthogonal to partial spec; lives in the explicit-spec path
(parser.cpp ~20546+). Left for a future fix.

### 4.2 `7930a81` — `__madc__` identity macros (header-partition Step 0)

**Why.** Per the strategy, madc must be a PEER like Clang — it impersonates the host
gcc (it seeds the toolchain's WHOLE predefined-macro set, incl. `__GNUC__`, so
unmodified libstdc++/glibc parse) but had **no identity of its own**. Clang defines
`__clang__` AND `__GNUC__`; madc had only the gcc mirror.

**The fix.** In `lexer.cpp` (the hand-set-builtins block, right after the existing
`MADC_VERSION` define ~line 1068), seed:
```cpp
define_map["__madc__"]         = "1";
define_map["__MADC__"]         = "1";
define_map["__MADC_VERSION__"] = "\"" MADC_VERSION_STR "\"";
```
Placed with the hand-set builtins (seeded BEFORE the generated gcc-mirror at
`lexer.cpp` ~1476, whose loop must not clobber them) and defined in **EVERY mode**
(like `__clang__`, unlike `__cplusplus`/`__GNUG__` which are C++-only). Verified:
`__madc__`=1 + `__MADC_VERSION__`="0.25.0" visible in both C++ and default modes; gcc
does NOT define `__madc__` (peer check). fulltest 529/4, zero regression.

**Mechanism context (important for Step 4 of the header plan).**
`src/predefined_macros.cpp` is **AUTO-GENERATED** by
`scripts/gen_predefined_macros.sh` from `gcc -dM -E -std=c++17 -x c++ /dev/null` — it
is a literal mirror of the host gcc's predefined set, regenerated at build time
(gitignored, `DO-NOT-EDIT`). So madc's `#if __GNUC__ >=` / `__SIZEOF_*__` / `__*_TYPE__`
/ feature-test environment IS the host gcc's for c++17. That's why header-partition
**Step 4 (close `madc -dM` vs `gcc -dM` gaps) is effectively satisfied by
construction.** (Caveat: madc has no `madc -dM` dump flag of its own; adding one would
make acceptance-test #3 directly runnable. C-mode / other-std parity is unverified.)

### 4.3 `45db326` — Type-trait builtins (header-partition Step 5, v1)

**Why.** libstdc++ `<type_traits>`/`<tuple>` implement `std::is_*` via compiler trait
intrinsics (`__is_class(T)` etc.). madc implemented **NONE** (verified — all MISS, zero
source handling).

**Design — table-driven evaluator in the expression arm, mirroring `sizeof`.**
- File-static `type_trait_arity` (parser.cpp:**4062**) + `is_type_trait_builtin`
  (**4071**) recognize the supported names.
- `Program::evaluate_type_trait` (parser.cpp:**4112**, declared madc.h:**1687**)
  parses `( type-list )` reusing the template-arg machinery
  (`consume_template_type_arg_qualifiers` + `resolve_declared_type_token` + `*`
  folding via `getPointerType`), evaluates, and returns a `TokenInt(0/1)` typed
  `ddBOOL`.
- Wired into `parseExpr_identifierArm` right next to the `sizeof`/`alignof` fold
  (search "type-trait builtins" near the sizeof fold).
- Faithful predicates: `trait_is_class` (parser.cpp:**4083** —
  `dynamic_cast<DataDefSTRUCT*> && !union_layout`; covers class+struct, excludes
  union/enum because `DataDefCLASS : DataDefSTRUCT` and `DataDefENUM` is neither),
  `trait_is_union` (`DataDefSTRUCT::union_layout`), `trait_is_enum`
  (`dynamic_cast<DataDefENUM*>`), `trait_is_base_of` (parser.cpp:**4096** — walks
  `base_class` + the MI `bases` vector; true for self and indirect bases),
  `__is_same` (resolved-DataDef name equality).

**CORRECTNESS-FIRST (the load-bearing design rule here).** A WRONG trait bool silently
corrupts SFINAE → wrong overloads → subtle bugs. So madc implements **only the gcc-13
builtins it can answer EXACTLY** from its DataDef model — no more. Supported set:
`__is_same`, `__is_class`, `__is_union`, `__is_enum`, `__is_base_of`. Everything else
stays **unrecognized → clear error, never a wrong answer**. Two deliberate exclusions:
- `__is_pointer` / `__is_void` — gcc 13 does NOT provide these as builtins (libstdc++
  implements them in-library); a madc builtin could SHADOW a library identifier. Also,
  madc's `DataDefPTR::is_integer()` returns **true** (codegen-tuned, not
  trait-faithful), so reusing scalar predicates for `__is_integral` etc. would give
  wrong answers — another reason to defer them.
- References are modeled as pointers in madc (`project_template_instantiation`: "a
  reference is a strict pointer"), so `__is_reference`/`__is_lvalue_reference` can't be
  distinguished — deferred.

**Validated vs g++ -std=c++17:** identical (`11000 10 10 1101 10`), incl. indirect
base, self, non-base. fulltest 529→530/4 (+`testtypetraits`); torture 1566/88 identical
(only 5 reserved `__is_*` names recognized in C++ expr context — inert for C); SMAUG
ready/0-err.

### 4.4 `0950f1e` — Capture integral static-const member values

**The bug.** A static data member's in-class initializer
(`static const int value = 5;`) was parsed then **DISCARDED** — only the type was
stored in `DataDefCLASS::static_member_types` — so `X::value` read a **0 placeholder**
(g++ gives 5). This is the foundation of the `std::integral_constant` /
`std::is_*::value` shape. Confirmed at the parse site (a skip-loop consumes the
initializer tokens to `;` and throws them away) and three read sites that all emitted
`TokenInt(0)`.

**The fix.**
- `DataDefCLASS` (datadef.h:**668**) gained
  `std::map<std::string, int64_t> static_member_const_values;`.
- `Program::capture_constant_initializer_value` (parser.cpp:**5175**, declared
  madc.h:**1563**) — a **speculative** fold (assumes `=` consumed): save tokens /
  diagnostics / last_error, try `parse_constant_integer_expression()` expecting a
  terminating `;`; on success the initializer is consumed (stream left at `;`) and the
  value returned; on ANY failure it restores and returns false. Same save/try/restore
  idiom as `bracket_dim_constant_expression_parses`.
- At the member parse, when the member type is integral and not a pointer AND the
  capture succeeds, store the value and continue; otherwise the existing structural
  skip-loop runs unchanged → **zero behavior change for all non-integral / non-constant
  initializers**.
- `resolve_class_static_member_const_value` (parser.cpp:**1680**) walks the base chain
  like the type resolver; the three read sites (parser.cpp **8836**, **9606**, and the
  one inside `parseExpr_identifierArm`) now return `TokenInt(captured_value)` instead of
  `TokenInt(0)`.

**Validated vs g++:** `b::value`=5, `ic::value`=42, inherited `derived::maxv`=127,
negative `minv`=-128. fulltest 530→531/4 (+`teststaticconstmember`); torture 1566/88
identical (inert for C — C structs have no static members); SMAUG ready/0-err.

### 4.5 `a2d3f98` — Fold trait builtins in CONSTANT-expression context

**The bug.** The constant-expression mini-parser (`parse_constant_primary` →
`parse_constant_ternary`, a recursive descent **separate** from `parseExpression`)
didn't know the trait builtins. So `static const bool value = __is_class(T);` (the
integral_constant initializer — evaluated via the §4.4 capture path, which goes through
`parse_constant_integer_expression`) and `int a[__is_class(S)?4:1]` (array dim) failed
with "Expecting integer constant expression".

**The fix.** In `parse_constant_primary` (parser.cpp:**4908**), right after the
`sizeof`/`alignof` fold, add the `is_type_trait_builtin` branch: call
`evaluate_type_trait(tb, name)` and return `static_cast<TokenInt*>(r)->ival()`. Args are
concrete (monomorphized), so it folds to 0/1. Combined with §4.4, a trait-initialized
static member now captures the correct value.

**Validated vs g++:** `int a[__is_class(S)?4:1]` → size 4, `int b[__is_enum(E)?7:1]` →
size 7. fulltest 531/4 (`testtypetraits` extended to cover constant context); torture
1566/88 identical (only the 5 reserved names fold — inert for C); SMAUG ready/0-err.

---

## 5. CONFIRMED CAPABILITIES NOW (what works, with the exact observable)

Run any of these to confirm live state (default mode unless noted). All match g++.
```bash
# partial specialization (direct + dependent path)
bin/madc tests/testpartialspec.mad        # -> 0 1 42

# trait builtins (expr + constant context)
bin/madc tests/testtypetraits.mad         # -> "11000 10 10 1101 10" then "4 7"

# static-const member values (expr context)
bin/madc tests/teststaticconstmember.mad  # -> 127 -128 255 127
```

---

## 6. METHODOLOGY THAT WORKED (reuse it; it's why this arc landed cleanly)

### 6.1 `-E` bisection for real-header derails
The live-include path reports **bogus `.mad` coordinates** for errors inside included
headers. To get the REAL line:
```bash
printf '#include <string_view>\nint main(){return 0;}\n' > tmp/_x.mad
bin/madc --std=c++17 -E tmp/_x.mad > tmp/_x_pp.cpp          # madc-preprocess
bin/madc --std=c++17 --emit=c11 tmp/_x_pp.cpp 2>&1 >/dev/null | grep -m1 error:
#   -> reports the real line in the preprocessed file; read context around it.
```
Classify: `g++ -E` the header, feed the gcc-preprocessed file to `bin/madc --emit=c11`
— if THAT parses but madc's own-preprocess fails, it's a madc **preprocessor** gap;
if both fail, a **parser** gap. **Bisect by `#include`, not by `file:line`** (cross-
include attribution is unreliable).

### 6.2 Instrument, don't assume (Rule #4 in action)
The partial-spec diagnosis was caught ONLY because I added temporary
`DBG(std::cerr << "[TAG] ..." )` at the throw sites and READ the actual failing token,
instead of trusting the prior handoff's static hypothesis. Pattern:
- Print to `std::cerr` (not cout); run `bin/madc -v ... --emit=c11 file >/dev/null
  2>trace.log`; `grep "[TAG]" trace.log`.
- Remove all instrumentation before committing (it was throwaway scaffolding — these
  were MY temporary tags, distinct from the codebase's permanent `DBG()` diagnostics,
  which must never be removed).

### 6.3 gcc AND clang are BOTH canon (`feedback_two_canon_compilers`)
Reduce every failing case to a minimal repro; compare against `gcc -S -fverbose-asm
-O0` and `clang -S -O0` (or `g++ -E` for preprocessor) BEFORE forming a hypothesis.
For semantics, compile the reducer with g++ and compare OUTPUT. gcc's `-dM`/
`-print-file-name=include` are the impersonation oracles. Don't dedupe the gcc/clang
rule pair — gcc has `-fverbose-asm` source annotations, clang does not; keep both.

### 6.4 Correctness-first; shortcuts categorically unacceptable (`feedback_correct_over_shortcuts`)
RED-FLAG tells = about to hardcode a literal / add a wrapper-shim / special-case a
specific class higher up / think "good enough for now" → STOP, fix the deepest layer.
The trait work embodies this: implement only what madc can answer EXACTLY; never emit a
plausible-but-wrong trait bool. (Hardcoded stream `_ZSt` literals once caused DAYS of
drift — don't repeat it.)

### 6.5 Reuse existing mechanisms FIRST (`design-principles`, user-reinforced)
- The trait evaluator reuses the `sizeof` fold site + the template-arg type parser.
- The static-const capture reuses the `bracket_dim_constant_expression_parses`
  save/try/restore idiom.
- The (reverted) A1 attempt reused the `ExprStep::Redo` re-dispatch idiom.
Don't default to new mechanisms; search the 24k-line parser first.

### 6.6 The monomorphization insight
madc instantiates templates by token-substitution + re-parse on use. Consequence:
inside an instantiated template body, type params are ALREADY concrete by the time the
parser/evaluator sees them. So features like partial-spec matching and trait evaluation
**never need dependent-placeholder handling** — they always operate on concrete
DataDefs. This is why both landed without a two-phase-lookup engine.

### 6.7 Don't ignore warnings (`feedback_dont_ignore_warnings`)
Analyze every madc build warning AND every g++ warning on `--emit=c11` output. An
ignored g++ warning once hid a real `length()`-returns-void bug.

---

## 7. REMAINING WORK — PRECISE NEXT STEPS

### 7.A The `std::is_*<T>::value` keystone — TWO gaps left
The value is now CAPTURED correctly (§4.4 + §4.5: `static const bool value =
__is_class(T)` folds to the right 0/1 at instantiation). What's missing is READING it
through the two real-header access forms:

- **A1 — template-id scope access `X<T>::value` in expression context.** Currently
  `b<int>::value` errors "use of undeclared identifier 'b'": the identifier arm rejects
  a template NAME (`b`) before instantiating it. I prototyped a fix (in
  `parseExpr_identifierArm`, at the `if (!var)` block: if the name is in `template_map`
  /`template_alias_map` and the next token is `<`, call `resolve_declared_type_token(tb,
  true, true, /*consume_class_member_chain=*/false)` to instantiate, then `tb = inst;
  return ExprStep::Redo;` so the ttDataType arm's existing `::` handling at parser.cpp
  ~9934 resolves `::value`). **It compiled and got further** (reached `::value`) but
  surfaced a SECOND gap: the freshly-instantiated class's static-member lookup didn't
  find `value` ("undeclared identifier 'value'"), and `a.value` instance-access of a
  static member is also unsupported ("Unidentified member"). I **reverted the A1 hunk**
  to keep the committed state clean (Rule #3 — don't ship half-wired). Next: re-apply
  the Redo hook, then debug why `resolve_class_qualified_expression` (parser.cpp ~9495,
  reached from the ttDataType arm ~9934) doesn't find the static member on the
  instantiated `b_int` — likely the instantiation produced an incomplete placeholder, or
  the static-member maps aren't carried/visible. Also wire instance-access of statics
  (`a.value`) if libstdc++ needs it (it mostly uses `::value`).

- **A2-rest — static-member CONSTANTS in constant-expression context.** Traits now fold
  in `parse_constant_primary` (§4.5), but a static-const member used in a constant
  context (`int a[L::m]`) still fails "Expecting integer constant expression" — the
  constant-expr parser doesn't resolve `Scope::member` to its captured constant. Add a
  branch in `parse_constant_primary` (parser.cpp:4908): when the atom is a class/type
  (ttDataType or a type-resolvable identifier, possibly a template-id) followed by `::`,
  resolve the scope and call `resolve_class_static_member_const_value`. CAUTION: this
  parser is widely used (array dims, enum values, case labels) — high blast radius;
  gate hard and keep the branch narrow (only fire when `::` follows a resolved type).

Together A1 + A2-rest make `std::integral_constant`-derived `std::is_*<T>::value`
readable end-to-end. Minimal end-to-end reducer to drive toward (g++ gives `1 0`):
```cpp
#include <stdio.h>
struct S {};
template<class T> struct is_cls { static const bool value = __is_class(T); };
int main(){ printf("%d %d\n", is_cls<S>::value, is_cls<int>::value); return 0; }
```

### 7.B More trait predicates (header-partition Step 5, continued)
Each remaining gcc-13 trait is ONE table entry in `type_trait_arity` + one branch in
`evaluate_type_trait` + a FAITHFUL predicate. Implement only when madc can model it
exactly (correctness-first). Candidates and what they need:
- `__has_virtual_destructor` — madc tracks virtual dtors (the MI/virtual-dtor work,
  `project_multiple_inheritance` / `project_cpp_parser_correctness`); check the vtable
  /dtor flags on DataDefCLASS.
- `__is_polymorphic` — has any virtual function / vptr (`has_vptr_slot`/`has_vtable`).
- `__is_abstract` — has a pure-virtual (note: pure-virtual `=0` was DEFERRED earlier;
  verify support first).
- `__is_final` — needs `final` tracking (madc may not record it — check).
- `__is_empty` — class with no non-static data members and no vptr.
- `__underlying_type(E)` / `__type_pack_element<N, Ts...>` — TYPE-producing (return a
  type token, used in type context, not bool) — a different shape; wire via
  `resolve_declared_type_token`'s type path, not the expression fold.
- HARDER, need member/ctor analysis (defer): `__is_constructible`, `__is_assignable`,
  `__is_convertible`, `__is_trivially_*`, `__is_standard_layout`, `__is_aggregate`,
  `__is_pod`, `__is_trivial`, `__has_unique_object_representations`.
Enumerate what the installed libstdc++ actually calls:
```bash
g++ -E -x c++ - <<'EOF' 2>/dev/null | grep -ohE '__(is|has|underlying|type_pack)_[a-z_]+' | sort -u
#include <type_traits>
#include <tuple>
#include <memory>
EOF
```

### 7.C The `<string_view>` frontier (after A1)
With partial spec landed, `<string_view>` now derails LATER: instantiating
`basic_string_view<char>` hits **"Expecting type in class definition"** inside the body
(a member declaration the class-body parser rejects — bisect via -E §6.1). This is the
next real-header blocker for the string/streams family once `::value`-style access is
unblocked.

### 7.D Header-partition plan — remaining steps (`madc-header-partition-handoff.md`)
- **Step 0 (`__madc__`) — ✅ DONE** (§4.2).
- **Step 4 (macro gaps) — effectively satisfied** by `predefined_macros.cpp`
  generation (§4.2). Optional: add a `madc -dM` dump flag for acceptance-test #3.
- **Step 5 (trait builtins) — v1 LANDED** (§4.3/§4.5); continue per §7.B.
- **Step 1 (madc-owned freestanding dir)** — stand up a freestanding header dir from
  c2mir's `mirc_*.h` / `x86_64/mirc_x86_64_*.h` (NOT gcc's `$OWN` dir — non-goal #2),
  wire it search-first with correct `#include_next` chaining, then **retire the library
  stubs** in `include/madc/` (string/vector/map/set/algorithm/streams + glibc dups)
  that currently SHADOW the real headers (the documented embedded-stub-shadowing
  blocker). Larger; do after Step 5 lands enough for `<type_traits>` to parse.

### 7.E Separate open workstreams (not this track, but live)
- **struct≡class unification** — `docs/plans/2026-06-07-realheader-track-HANDOFF.md`
  §6 + `project_struct_is_class`. The user directive: struct and class differ only by
  default access; a struct body should parse member functions/ctors/dtors/operators
  like a class body. Struct member methods currently throw "Expecting ';' after struct
  member". HIGH blast radius (blanket struct=class caused 65–120 torture regressions
  thrice) — must keep trivial structs on the native path (re-gate object-specific
  handling on `is_nontrivial_class`, not `is_object()`).
- **explicit-spec use-site mangling** bug (§4.1, `tr<int>`→`tr_int32_t` mismatch).
- **retire-std-hardcoding campaign** — `project_string_as_class`,
  `project_cpp_mangled_direct`, `scripts/check-no-std-hardcoding.sh` (wired into
  fulltest). Converges with header-partition Step 1.

---

## 8. KEY FILES & ANCHORS (current line numbers @ HEAD 60c7e18)

| Concern | File:line | Symbol |
|---|---|---|
| Partial-spec fields | include/madc.h:1091,1094,1096 | `is_partial_specialization`, `spec_pattern` |
| partial_spec_map | include/madc.h:1119 | `partial_spec_map` |
| Match declaration | include/madc.h:1124 | `match_partial_specialization` |
| Match call site | src/parser.cpp:2478 | in `instantiate_template_use` |
| Unify helper | src/parser.cpp:9741 | `unify_spec_pattern_arg` (file-static) |
| Match definition | src/parser.cpp:9799 | `match_partial_specialization` |
| Spec registration | src/parser.cpp:20442,20466,20542 | `TokenTEMPLATE::parse` |
| Trait recognizer | src/parser.cpp:4062,4071 | `type_trait_arity`, `is_type_trait_builtin` |
| Trait predicates | src/parser.cpp:4083,4096 | `trait_is_class`, `trait_is_base_of` |
| Trait evaluator | src/parser.cpp:4112 (decl madc.h:1687) | `evaluate_type_trait` |
| Trait fold (expr arm) | near the `sizeof` fold in `parseExpr_identifierArm` | "type-trait builtins" |
| Trait fold (const) | src/parser.cpp:4908 (`parse_constant_primary`) | `is_type_trait_builtin` branch |
| Static-const map | include/datadef.h:668 | `static_member_const_values` |
| Capture method | src/parser.cpp:5175 (decl madc.h:1563) | `capture_constant_initializer_value` |
| Const-value resolver | src/parser.cpp:1680 | `resolve_class_static_member_const_value` |
| Static read sites | src/parser.cpp:8836, 9606, (+ identifier arm) | return `TokenInt(cv)` |
| Two throw sites (diag) | src/parser.cpp:2341, 2644 | "Expecting a type argument to … <>" |
| Qualified-class expr | src/parser.cpp ~9495 (`resolve_class_qualified_expression`) | A1 target |
| ttDataType arm `::` | src/parser.cpp ~9934 | A1 re-dispatch lands here |
| Redo handling | src/parser.cpp ~13386 (`goto redo_expression_token`) | A1 idiom |

Regression tests added this arc: `tests/testpartialspec.{mad,expect}`,
`tests/testtypetraits.{mad,expect}`, `tests/teststaticconstmember.{mad,expect}`.

---

## 9. RELEVANT DATADEF / TYPE-MODEL FACTS (so you don't re-derive them)

- `enum class BaseType { btSimple, btStruct, btFunct, btClass }` — **no btEnum,
  btUnion**. (datadef.h:24)
- `DataDefCLASS : public DataDefSTRUCT` (datadef.h:656) — a class IS-A struct;
  `basetype()` returns btClass for class, btStruct for struct.
- Union = `DataDefSTRUCT` with `union_layout == true` (datadef.h ~275). Class/struct =
  `union_layout == false`.
- Enum = `DataDefENUM` (datadef.h:838), `DataType::dtINT`, NOT a DataDefSTRUCT.
- Pointer = `DataDefPTR` (datadef.h:807) with `base_type` = pointee, `is_pointer()`
  true. **`DataDefPTR::is_integer()` returns true** (codegen convenience — do NOT use
  scalar predicates for trait faithfulness).
- Bases: `DataDefCLASS::base_class` (single inheritance) + `std::vector<BaseSpec>
  bases` (MI; `BaseSpec{ DataDefCLASS *base; size_t offset; bool is_virtual; uint32_t
  access; bool is_primary; }`, datadef.h:648).
- `void` = `ddVOID` / `DataType::dtVOID`.
- `ddBOOL`, `ddINT`, `ddUINT64`, etc. are global DataDef instances in parser.cpp ~5150.
- `TemplateDef` (madc.h:1082): `typeparams`, `typeparam_defaults`, `typeparam_is_type`,
  `has_non_type_params`, `class_name`, `body` (cloned tokens), `defining_namespace`,
  `owner_class`, + (new) `is_partial_specialization`, `spec_pattern`.
- `template_map` = name → vector of per-namespace primary `TemplateDef`s
  (`find_template(name, ns_hint)` selects). `partial_spec_map` = name → vector of
  partial-spec `TemplateDef`s (separate, by design). `template_alias_map` = alias
  templates.

---

## 10. VERIFY-ON-RESUME BLOCK (run first)

```bash
cd /workspace/madc
git rev-parse --short HEAD            # expect 60c7e18 (or later if work continued)
git branch --show-current             # feature/realhdr-parse-gaps2-claude
git status --short                    # expect clean
cat MIR_COMMIT                        # 2ffebff
make -C src 2>&1 | grep -iE ': error|: warning' | head   # clean build
make -C src fulltest 2>&1 | grep -E "passed,|FAIL:" | tail -6   # 531 passed / 4 failed
# the 3 capabilities landed this arc (all match g++):
bin/madc tests/testpartialspec.mad        # 0 1 42
bin/madc tests/testtypetraits.mad         # 11000 10 10 1101 10 / 4 7
bin/madc tests/teststaticconstmember.mad  # 127 -128 255 127
# real-header frontier:
printf '#include <string_view>\nint main(){return 0;}\n' > tmp/_sv.mad
bin/madc --std=c++17 --emit=c11 tmp/_sv.mad 2>&1 >/dev/null | grep -m1 error:
#   -> "Expecting type in class definition" (the post-partial-spec frontier, §7.C)
# torture (background, ~20 min) + SMAUG soak per §2.3 before declaring any change done.
```

---

## 11. THE ONE-PARAGRAPH RESUME SCRIPT

On resume: read this file + `AGENTS.md` Top-10 + the memory files in §1.8. Run §10 to
confirm HEAD `60c7e18`, clean tree, fulltest 531/4, and the three landed capabilities.
The branch advanced the C++ `<type_traits>`/template frontier with 5 gated commits
(partial specialization, `__madc__` identity, trait builtins in expr + constant
context, static-const member value capture). The immediate high-leverage target is the
`std::is_*<T>::value` keystone — two precise gaps remain (§7.A): A1 = template-id
`X<T>::value` scope access (re-apply the `ExprStep::Redo` hook in
`parseExpr_identifierArm`, then debug the instantiated class's static-member lookup +
`a.value` instance-static access), and A2-rest = static-member constants in the
constant-expression parser (`parse_constant_primary`). Alternatively extend the trait
set (§7.B) or push the header-partition plan's Step 1 (§7.D). Use the `-E` bisection
+ g++ oracle + instrument-don't-assume methodology (§6); be correctness-first (never a
wrong trait bool); gate every change with fulltest + torture failset + SMAUG soak; do
NOT push; do NOT promote to master.

END OF HANDOFF.
