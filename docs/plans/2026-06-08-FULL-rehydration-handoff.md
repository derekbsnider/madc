# MADC — FULL REHYDRATION HANDOFF (2026-06-08, consolidated, self-contained)

> **READ-FIRST COLD-START DOCUMENT.** Assume you remember nothing. This single
> file is written to be exhaustive on purpose: after reading it plus the Top-10
> Rules in `AGENTS.md`, you can resume the **real-header / `<type_traits>` /
> template-instantiation** track with zero re-derivation — every command, every
> code anchor, every diagnosis, every baseline, and the precise next step are
> here. It **consolidates and supersedes** the layered
> `docs/plans/2026-06-08-rehydration-handoff.md` (§0–§13) and the older
> `docs/plans/2026-06-07-realheader-track-HANDOFF.md` for *current state* (those
> remain valid for the per-commit lexer/PP micro-history and the struct/class
> §6 directive).

---

## TABLE OF CONTENTS
- §0  60-second orientation
- §1  Read-order on resume
- §2  Environment, build, test, backend
- §3  Branch / promotion / MIR-pin discipline
- §4  The complete feature arc on this branch (every commit, with diagnosis + anchors)
- §5  Confirmed capabilities (exact observables)
- §6  Real-header parse landscape (the worklist)
- §7  THE NEXT BLOCKER — namespace-qualified template-id in constant-capture (root-cause map + plan)
- §8  Other open workstreams
- §9  Methodology that works (and why)
- §10 The constant-evaluation architecture (mental model — read before touching it)
- §11 Key files & anchors table
- §12 DataDef / type-model facts
- §13 Process lessons (mtime staleness, JIT exit-code artifact, …)
- §14 Verify-on-resume block
- §15 One-paragraph resume script

---

## §0  THE 60-SECOND ORIENTATION

- **Repo:** `/workspace/madc`. **Branch:** `feature/realhdr-parse-gaps2-claude`
  (cut off `develop`). **HEAD:** `6344e7d`. **Working tree:** clean.
- **96 commits ahead of `develop`.** `develop` is **UNTOUCHED** by all this work.
  **DO NOT push. DO NOT `/promote` develop→master.** (Parity gate — see §3.)
- **fulltest baseline:** `534 passed / 4 failed / 0 timed out / 26 skipped`.
  The **4 failures are PRE-EXISTING and unrelated** to this arc: `testdefer`,
  `testfstream`, `testlargesizeofquery`, `testloop`. "Green" = exactly those four.
  (`tests/` holds 565 `.mad` files; the rest are skipped helpers or pass.)
- **gcc.c-torture baseline:** `1566 passed`, `31 compile-failed`, `57 runtime-failed`,
  `1 timed out`, `30 skipped`. This is the regression gate for any parser/cir change.
  (88 hard fails = 31+57; the lone timeout is the known-flaky `pr60003.c`.)
- **SMAUG C89 soak baseline:** boots to `"Realms of Despair ready at address
  madc-dev on port <N>"` with **0 errors** (and the documented C89-looseness
  warnings — do NOT silence them).
- **MIR fork pin (`MIR_COMMIT`):** `2ffebff`. Fork at `/workspace/mir`, branch
  `develop`. **NOT** upstream MIR. This arc added NO new MIR dependency — pin unchanged.
- **What this arc just accomplished:** advanced the C++ `<type_traits>` /
  template-instantiation / class-member-constant frontier with a chain of gated
  commits. `<type_traits>` now PARSES; 11/19 surveyed headers parse.
- **The single most valuable next task (§7):** fix the namespace-qualified
  template-id in a static-const member captured during template instantiation
  (`std::__are_same<float,float>::__value`), which blocks `<string_view>` and
  `<memory>`. Everything is reduced and root-caused; an instrumentation plan is in §7.

### Mission, in one paragraph
**madc** ("My Advanced Dialect of C") is a C/C++ dialect compiler. Pipeline:
`source → lexer → parser → cir_node tree (MC11-IR, == c2mir node_t) → c2mir → MIR
→ JIT`. `--emit=c11` renders the same tree to portable C (a first-class third
output). The active track is making madc **parse REAL system C++/libc headers**
(libstdc++/glibc, *unmodified*) en route to **C23/C++23 compliance** (the north
star — `project_north_star_c23_cpp23`). The immediate objective on this branch is
the **string / iostream / sstream / fstream / `<type_traits>`** family. The
backend is settled (ADR 0001): one IR, c2mir/MIR is the sole backend, new features
get a C11 lowering by default. **No hardcoding of standards/targets; everything
gated through the `--std=`/`LanguageStd` enum + the keyword/feature registry; no
special-casing individual consumers** (the I1–I8 invariants in
`docs/plans/madc-vision-and-invariants.md`).

---

## §1  READ-ORDER ON RESUME

1. **This file, top to bottom.** It is self-contained.
2. **`AGENTS.md`** (loaded as `CLAUDE.md` via `@AGENTS.md`) — the **Top-10 Rules**,
   especially #2 (fix at the deepest layer), #3 (think twice, code once — no
   speculative micro-fixes), #4 (understand what exists before assuming it
   doesn't). Plus the rule files that touch this task:
   `gcc-methodology.md`, `clang-methodology.md`, `design-principles.md`,
   `pre-edit-checklist.md`, `no-parallel-implementations.md`, `testing-fulltest.md`,
   `lowering-vs-raising.md`, `mc11-ir.md`, `backend-strategy.md`.
3. **Memory** (`/home/dev/.claude/projects/-workspace-madc/memory/`): start at
   `MEMORY.md` (the RESTART line routes here), then the most relevant:
   `project_template_instantiation`, `project_struct_is_class`,
   `project_string_as_class`, `project_cpp_mangled_direct`,
   `project_north_star_c23_cpp23`, `project_parser_pp_architecture`,
   `feedback_correct_over_shortcuts`, `feedback_two_canon_compilers`,
   `feedback_dont_ignore_warnings`, `feedback_emitc_gcc_parity_oracle`.
4. Background only as needed: `docs/plans/2026-06-08-rehydration-handoff.md`
   (the layered predecessor — same facts, more granular per-commit notes);
   `madc-header-partition-handoff.md` (the header-partition master plan, repo root);
   `docs/plans/2026-06-07-freestanding-vs-hosted-headers-strategy.md`;
   `docs/plans/2026-06-07-parser-pp-architecture-research.md`;
   `docs/adr/0001-cir-c2mir-backend.md`; `docs/plans/madc-vision-and-invariants.md`.

---

## §2  ENVIRONMENT, BUILD, TEST, BACKEND

### 2.1 Build
```bash
make -C src                 # build bin/madc + lib/libmadc.{a,so}; regenerates
                            #   embedded headers + predefined macros at build time
make -C src clean           # remove all objects
make -C src test            # doctest unit tests only
make -C src fulltest        # unit + ALL integration tests (THE gate)
```
Build needs `clang++`/`g++` (C++11) and the **madc MIR fork** at `/workspace/mir`
(branch `develop`, pinned `MIR_COMMIT=2ffebff`). The fork carries native
`_Complex`, `__attribute__((cleanup))`, ≤16-byte SIMD, the scope-depth decl-layout
fix, and the SysV-varargs/`_Complex`/`_Alignas` ABI fixes the CIR backend depends
on. **Not** upstream MIR.

If your change is core-parser/compiler-only and doesn't touch `madcdat`/storage,
`./configure --enable-madcdat=no` shrinks the rebuild; re-enable before final
validation if you touched shared/storage surfaces.

### 2.2 The madc flags you will actually use
- `--std=c++17` (or `c++11`) — dialect. The test runner uses **default (STD_MADC)
  mode** for `.mad` files with no `.flags` fixture; the C++ template/trait/enum
  tests in this arc all pass in default mode (verified), so no `.flags` needed.
- `--emit=c11` — render the cir_node tree as C ("what c2mir sees"). **The fidelity
  oracle:** a gcc-compiled `--emit=c11` of a program must match the g++-compiled
  original C++ (`feedback_emitc_gcc_parity_oracle`). Divergence = a madc bug.
- `-E` — **preprocess-only** dump (expand `#include`/`#define`/macros, print the
  token stream, stop). THE bisection instrument for real-header derails (§9.1).
- `--no-embedded-headers` — diagnostic: fall through to the REAL system headers
  instead of the `include/madc/` stubs (reuses the existing `registration_policy` /
  `is_embedded_header_allowed()` gate).
- `--dump-source` (preprocessed token stream) / `--dump-cir` (the tree).
- `-v` / `--verbose` — sets `madc_verbose`, enabling `DBG(...)` output. NOTE the
  DBG stream split (some to `cout`, some to `cerr`). When instrumenting, print to
  `std::cerr` and redirect `--emit=c11`'s stdout to `/dev/null` so emitted C
  doesn't drown the trace.

### 2.3 The validation gate trio (run for EVERY parser/cir change)
1. **fulltest** — must stay `534/4/0/26` (exactly those 4 reds):
   ```bash
   make -C src fulltest 2>&1 | grep -E "passed,|FAIL: tests" | tail -8
   ```
2. **gcc.c-torture failset** — must stay `1566 / 31 / 57 / 1`. ~15–20 min on this
   QNAP. **Run it ALONE (no concurrent SMAUG/fulltest)** — the per-test cap is **5s**
   (`run_gcc_testsuite.py --timeout` default 5.0), so heavy tests (memcpy-a1/a2/a4,
   ~3.8s clean) FLAKE to "timed out" under load. The compile/runtime FAIL counts are
   the real signal; a timeout delta under load is not a regression (confirm with a
   clean no-load re-run).
   ```bash
   python3 scripts/run_gcc_testsuite.py --root gcc_testsuite --madc bin/madc
   # tail: "1566 passed, 31 compile-failed, 57 runtime-failed, 1 timed out, 30 skipped"
   ```
3. **SMAUG C89 soak** — must boot clean, on a RANDOM free port (shared box;
   "Address already in use" is a port collision, NOT a madc bug):
   ```bash
   cd /workspace/MadSMAUG
   P=$((6000 + RANDOM % 800))
   MADC=/workspace/madc/bin/madc MADC_CPU_LIMIT=0 MADC_MEM_LIMIT=0 \
     timeout 600 ./MadSMAUG.sh $P > /tmp/smaug.log 2>&1 &
   # poll with an until-loop for "ready at address" / "error:"; then pkill -f "port $P"
   ```

### 2.4 Background-task & shell gotchas (learned the hard way)
- A **bare foreground `sleep`** is blocked by the harness. Wait on a condition with
  an `until <check>; do sleep 10; done` loop, or `run_in_background`.
- `grep -c` **buffers** — emits only the final count at EOF; pipe to a log + `tail`
  when you want live progress.
- One SMAUG boot at a time (each is a ~158k-LOC parse). Don't queue two.
- Per AGENTS.md: **single commands, no `&&` chains** (avoids permission-prompt spam).
- **NAS mtime staleness — see §13.1. This cost real time. `touch src/<file>.cpp`
  before `make -C src` when iterating on one file; clean-rebuild when results look
  impossible.**

### 2.5 Backend (settled — do not re-litigate, ADR 0001)
`madc parser → cir_node (MC11-IR == c2mir node_t) → c2mir → MIR → JIT` is the SOLE
backend (asmjit removed). Direct-MIR is a scalpel for runtime internals only.
`--emit=c11` is a first-class third output. New language features get a **C11
lowering (Tier 1)** by default; raise c2mir (Tier 2) or MIR (Tier 3) only for true
primitives with no faithful C11 form (`.claude/rules/lowering-vs-raising.md`).

---

## §3  BRANCH / PROMOTION / MIR-PIN DISCIPLINE

- Feature branches off `develop`; agent-owned WIP suffixed `-claude`. This branch
  is `feature/realhdr-parse-gaps2-claude`, **local only, NOT pushed**.
- **Do NOT promote develop→master** until `develop` reaches feature parity with
  `master`. `master` still carries the removed asmjit backend at full C89 parity;
  `develop` (CIR backend) is climbing back. SMAUG running is a *milestone*, not
  parity. Gate = CIR integration coverage ≥ master's (`project_cir_parity_campaign`,
  `docs/parity/`, `docs/adr/0001`).
- `/release` cuts a versioned release ON `develop` for major milestones only — not
  individual bug fixes (`feedback_release_cadence`).
- **MIR pin discipline:** if madc starts depending on new fork commits, merge them
  to the fork's `develop`, push, and bump `MIR_COMMIT` in the SAME madc commit.
  This arc added NO MIR dependency.
- **Never `git checkout` over uncommitted work** (`feedback_never_lose_code`).
  Commit early; use `#ifdef` guards or `git stash`.

---

## §4  THE COMPLETE FEATURE ARC ON THIS BRANCH

96 commits ahead of develop. Two phases: (4.1) the 2026-06-07 lexer/PP real-header
groundwork, then (4.2–4.8) the 2026-06-08 template/trait/enum/member-constant work.
Every feature commit was validated with the full gate trio. Newest-first commit log
is reproduced in §4.0; the substantive features are detailed in §4.1–§4.8.

### §4.0 Commit log (develop..HEAD, newest first — abridged to the substantive ones)
```
6344e7d docs(handoff): §13 — anon enums + qualified const-fold; ns-const-capture map; mtime lesson
f6ef0a8 feat(class/enum): class/struct-scoped anonymous enums + qualified constants in constant context
a99c1e5 feat(class): resolve unqualified sibling static-const members in constant context
fea8963 feat(template): template-id X<T>::value in expression context (A1 keystone)
a2d3f98 feat(traits): fold trait builtins in CONSTANT-expression context too
0950f1e fix(class): capture integral static-const member values (X::value reads real value, not 0)
45db326 feat(traits): __is_class/__is_union/__is_enum/__is_base_of/__is_same (header-partition Step 5)
7930a81 feat(headers): define __madc__ identity macros (header-partition Step 0)
9c6c6c2 feat(template): partial specialization (template<class T> struct X<pattern>)
de24e23 fix(lexer): # (stringize) of an empty macro argument yields "" not literal #
be5a08f fix(lexer): multi-line /* */ comment on a directive line lexed as code
a2f387c feat(lexer): depth-guard backstop for runaway macro/preprocessor expansion
1cc70f0 fix(lexer): object-macro re-expansion no longer infinite-loops (#define A A)
f71a472 feat(cli): --no-embedded-headers — use real system headers (diagnostic)
2e853de fix(parser): '<' in trailing-return decltype no longer consumes to EOF
97eeeeb/d1f28f4 fix(parser): nested method-bearing struct + trailing declarator (class/struct body)
fcca0a3 fix(-E): reconstruct char literals in token_spelling
1f8e3fd feat(cli): add -E (preprocess-only) + parser/PP architecture research
f52c3ad fix(cir): multi-star casts must emit every pointer level
735c719 fix(headers,cir): type stdin/stdout/stderr as FILE* + declare inet_ntoa/inet_ntop
```
(Interleaved `docs(...)` commits are handoff/research updates.)

### §4.1 The 2026-06-07 lexer/PP real-header groundwork
Before templates could be exercised against real headers, the lexer/preprocessor
needed to survive libstdc++/glibc. Landed fixes (see the older handoff for blow-by-
blow): `-E` preprocess-only mode + char-literal reconstruction in token spelling;
`--no-embedded-headers`; object-macro self-reference no longer infinite-loops
(`#define A A`); a depth-guard backstop for runaway expansion; multi-line `/* */`
comments on directive lines; `#`-stringize of an empty arg → `""`; `'<'` in a
trailing-return `decltype(...)` no longer consuming to EOF; nested method-bearing
struct + trailing declarator (both struct and class bodies). Also general C lowering
fixes that fell out of the SMAUG C89 audit: multi-star casts emit every pointer
level; `stdin/stdout/stderr` typed `FILE*`; libc ptr-returning fns declared.

### §4.2 `9c6c6c2` — Template partial specialization
**Symptom:** `<string_view>` derailed instantiating `basic_string_view<char>` →
`error: Expecting a type argument to iterator<>`. **Misdiagnosis caught by
instrumentation (Rule #4):** the prior handoff blamed a template-id-vs-type-alias
bug at parser.cpp:2935-2942; temporary `DBG(std::cerr…)` at the throw sites showed
the live throw was elsewhere and the failing arg token was literally `typename` —
the real cause was that madc **silently discarded PARTIAL specializations** and
always instantiated the primary (so `iterator_traits<_Tp*>` used the empty primary →
no `iterator_category`). **Fix:** `TemplateDef` gained `is_partial_specialization` +
`spec_pattern`; partial specs register into a new `partial_spec_map` (kept OUT of
`template_map`, whose same-namespace merge would clobber the primary);
`instantiate_template_use` calls `match_partial_specialization`
(parser.cpp:**9939**, call site **2499**) which unifies each spec's pattern against
the concrete args (`unify_spec_pattern_arg`: `[cv] PARAM [*]*` pointer-peel deduction
+ concrete-literal spelling match), picks max specificity, and drives the matched
body; identity stays keyed on the concrete args (inert when no partial specs are
registered). **Crucial simplification:** madc **monomorphizes** (token-substitute +
re-parse on use), so by the time a partial spec is matched the args are already
concrete DataDefs — **no dependent-placeholder machinery needed.** v1 bails on
non-type params. Test `tests/testpartialspec.mad` → `0 1 42` (matches g++).
**Separate pre-existing bug (untouched):** *explicit*-spec use-site mangling is
wrong (`tr<int>`→`tr_int32_t` canonical, but the spec registered as `tr_int`).

### §4.3 `7930a81` — `__madc__` identity macros (header-partition Step 0)
madc impersonates the host gcc (it seeds the WHOLE `gcc -dM` predefined set incl.
`__GNUC__`, so unmodified libstdc++/glibc parse) but had no identity of its own. Like
Clang defines `__clang__` AND `__GNUC__`, lexer.cpp now seeds (with the hand-set
builtins, BEFORE the generated gcc-mirror so the mirror loop can't clobber them):
`define_map["__madc__"]="1"; ["__MADC__"]="1"; ["__MADC_VERSION__"]="\"…\""` — in
EVERY mode. **Mechanism note for header-partition Step 4:** `src/predefined_macros.cpp`
is AUTO-GENERATED by `scripts/gen_predefined_macros.sh` from
`gcc -dM -E -std=c++17 -x c++ /dev/null`, so madc's feature-test/`__SIZEOF_*__`
environment IS the host gcc's for c++17 — Step 4 (close `madc -dM` vs `gcc -dM` gaps)
is satisfied by construction (madc has no `madc -dM` flag yet; adding one would make
acceptance-test #3 runnable).

### §4.4 `45db326` — Type-trait builtins (header-partition Step 5, v1)
libstdc++ `<type_traits>` implements `std::is_*` via compiler intrinsics
(`__is_class(T)` etc.). madc implemented NONE. Added a table-driven evaluator
mirroring `sizeof`: `is_type_trait_builtin` (parser.cpp:**4092**) +
`evaluate_type_trait` (**4133**, decl in madc.h) parse `( type-list )` reusing the
template-arg machinery and return a `TokenInt(0/1)` typed `ddBOOL`, wired into both
`parseExpr_identifierArm` (next to the sizeof fold) AND `parse_constant_primary`
(§4.7, the constant-context fold at **5059**). **CORRECTNESS-FIRST (load-bearing
rule):** a WRONG trait bool silently corrupts SFINAE → implement only the gcc-13
builtins madc can answer EXACTLY (`__is_same`, `__is_class`, `__is_union`,
`__is_enum`, `__is_base_of`); everything else stays unrecognized → clear error,
never a wrong answer. Deliberate exclusions: `__is_pointer`/`__is_void` (gcc 13 does
NOT provide these as builtins — libstdc++ implements them in-library — and madc's
`DataDefPTR::is_integer()` returns true, codegen-tuned not trait-faithful);
references are modeled as pointers so `__is_reference` can't be distinguished. Test
`tests/testtypetraits.mad` → `11000 10 10 1101 10` / `4 7` (matches g++).

### §4.5 `0950f1e` — Capture integral static-const member values
A static data member's in-class initializer (`static const int value = 5;`) was
parsed then DISCARDED — only the type was stored — so `X::value` read a 0
placeholder (the `std::integral_constant`/`is_*::value` foundation). Added
`DataDefCLASS::static_member_const_values` (datadef.h:**668**) +
`capture_constant_initializer_value` (parser.cpp:**5315**) — a **speculative** fold
(save tokens/diagnostics/last_error, try `parse_constant_integer_expression`
expecting `;`, restore on any failure; same idiom as
`bracket_dim_constant_expression_parses`). The member parse stores the value when
the type is integral and not a pointer and the capture succeeds; otherwise the
existing structural skip-loop runs unchanged. `resolve_class_static_member_const_value`
(parser.cpp:**1680**) walks the base chain; three read sites now return
`TokenInt(captured)` instead of `TokenInt(0)`. Test `tests/teststaticconstmember.mad`
→ `127 -128 255 127` (matches g++).

### §4.6 `fea8963` — A1 keystone: template-id `X<T>::value` in EXPRESSION context
**Symptom:** `is_cls<S>::value` → "use of undeclared identifier 'is_cls'". The
identifier arm rejected a bare template NAME before instantiating it, so the
`std::is_*<T>::value` shape (the spine of `<type_traits>`) was unreachable.
**Diagnosis (Rule #4):** the prior handoff feared the *instantiated class's*
static-lookup was broken; I disproved that — `typedef is_cls<S> A; A::value` already
worked (`1 0`), localizing it to expression **dispatch**, not instantiation. Then
instrumented and found TWO things: (a) `parseExpr_identifierArm` (parser.cpp:**10179**)
didn't instantiate a template-id; (b) the ttIdentifier call site in `parseExpression`
(~**13600**) silently DROPPED `ExprStep::Redo` while the ttDataType arm
(~**13551**) honored it — so even after a hook set `tb`, the loop overwrote it with
`nextToken()`. **Fix:** at the top of the final `if (!var)` block in the identifier
arm (parser.cpp:**11498**), when the unresolved name is in
`template_map`/`template_alias_map` and `<` follows, call
`instantiate_template_id(name, ident_tb)` → `tb = inst; return ExprStep::Redo`; AND
add `if (step == ExprStep::Redo) goto redo_expression_token;` at the ttIdentifier
call site. The ttDataType arm's existing `::member` handling
(`resolve_class_qualified_expression`, parser.cpp:**9643**) then resolves `::value`
against the instantiated class (which already carries the captured static-const value
from §4.5, and class-scoped enum values from §4.8). **Provably inert for C**
(template maps empty for C TUs). **Effect: real `<type_traits>` now PARSES.** Test
`tests/testtemplateidvalue.mad` → `1 1 0` / `1 0` / `3` (matches g++).

### §4.7 `a99c1e5` — A2-rest: unqualified sibling static-const in CONSTANT context
The real `std::ios_base` shape: `static const category all = (ctype | numeric |
collate | time | monetary | messages);` inside the class body references earlier
sibling static-const members by unqualified name → "Expecting integer constant
expression". New helper `resolve_current_class_static_member_const_value`
(parser.cpp:**1722**, decl madc.h) mirrors `resolve_current_class_type_alias`: walks
`class_scope_stack` (innermost-first) + the active method's owner class, looking up
the name in each class's `static_member_const_values`. `class_scope_stack` already
holds the class being parsed for the whole body, so earlier siblings are visible.
Called from `parse_constant_primary` (parser.cpp:**5064**, right after the
trait-builtin fold); returns the captured value on a hit, else falls through to the
existing throw (NARROW — non-members and C are unaffected). Advanced `<ostream>`/
`<istream>` past their first const-expr blocker. Test
`tests/teststaticconstsibling.mad` → `0 1 8 15` (matches g++).

### §4.8 `f6ef0a8` — class/struct anonymous enums + qualified constants in CONSTANT context
libstdc++ exposes trait results pervasively as `enum { __value = N };` inside
class/struct bodies and reads them as class-scoped constants in constant context.
madc (a) couldn't PARSE an anonymous enum in a **struct** body
(`TokenSTRUCT::parse`'s member loop treats `enum` only as a member *type*); (b)
scoped plain-enum enumerators GLOBALLY (so `Class::e` didn't resolve and bare names
leaked); (c) couldn't fold a qualified `Class::member`/template-id `Tmpl<T>::member`
in constant context. **Three additive parts (each gated, clean-build verified):**
1. **`cpp_struct_body_needs_class_parser`** (parser.cpp:**14164**; enum-detection at
   **14300**) now detects an **enum DEFINITION member** (`enum [class][tag] { … };`,
   distinguished from a bare `enum T m;` by a `{` before the member `;`) and routes
   the struct to the **class** body parser via the existing struct→class delegation
   (the class parser already parses enum members). C++-mode only; the high-blast-
   radius struct member loop is NOT modified. (NOTE: a `struct` with class-like
   members is already delegated to `TokenCLASS::parse`; this just adds enum-defs to
   the trigger set. `struct S { static const int x=5; }; S::x` already worked → the
   struct static-member store path exists; struct promotion is at parser.cpp
   ~15246-15258.)
2. **`TokenENUM::parse`** (parser.cpp:**18327**; new branch at **18467**): a plain
   (non-scoped) enum defined inside a class/struct in C++ mode registers its
   enumerators into the enclosing class's `static_member_types` (→ `&ddINT`) +
   `static_member_const_values` (reusing the §4.5 store) via
   `class_scope_stack.back()`, instead of as global constants. So `Class::e`
   resolves (via `resolve_class_qualified_expression` 9603) and the bare name doesn't
   leak. C / scoped-enum / global enums unchanged.
3. **`fold_constant_qualified_member`** (parser.cpp:**4938**, decl madc.h:**1768**) +
   a hook in `parse_constant_primary`: folds a qualified class-scoped integral
   constant in CONSTANT context — `Class::m`, `Outer::Inner::m`, `ns::Class::m`, and
   the leading/segment template-id forms `Tmpl<T>::m` / `ns::Tmpl<…>::m`. It resolves
   the leading token to a scope (ttDataType definition; or a bare identifier that
   names a class type via `datatype_map`, a template via `instantiate_template_id`
   when `<` follows, or a namespace → `pending_ns`), then walks the `::`-chain
   (instantiating template-id segments, descending nested classes) and resolves the
   final member via `resolve_class_static_member_const_value`. Gated on the leading
   token beginning such a chain, so plain atoms fall through unchanged.

**Effect:** struct AND class anonymous enums parse + resolve (expression + constant
context); `int a[Class::flag]` and `int a[W<int>::v]` fold; `<ostream>`/`<istream>`
advanced past the `std::less` blocker (to a new `_S_categories_size` gap). Test
`tests/testanonenum.mad` → `1 2 3 5 6` / `3 4` (matches g++). **KNOWN GAP carried
forward:** the namespace-qualified template-id in a static-const captured during
instantiation (`std::__are_same<float,float>::__value`) still mis-folds — see §7.

---

## §5  CONFIRMED CAPABILITIES (exact observables — all match g++ -std=c++17)
Run any to confirm live state (default mode unless noted):
```bash
bin/madc tests/testpartialspec.mad         # 0 1 42        (partial specialization)
bin/madc tests/testtypetraits.mad          # 11000 10 10 1101 10 / 4 7   (trait builtins, expr+const)
bin/madc tests/teststaticconstmember.mad   # 127 -128 255 127            (static-const member values)
bin/madc tests/testtemplateidvalue.mad     # 1 1 0 / 1 0 / 3             (A1: X<T>::value in expr)
bin/madc tests/teststaticconstsibling.mad  # 0 1 8 15                    (sibling static-const in const)
bin/madc tests/testanonenum.mad            # 1 2 3 5 6 / 3 4             (anon enums + qualified const-fold)
```
Also working (verified, no committed test — keep as reducers if useful):
- `nn::are_same<int,int>::value` in **expression** context → `1 0`.
- `template<typename V> struct nt { int f(){ return nn::are_same<V,float>::value; } };`
  `nt<float>().f()` → `1` (namespaced template-id in an instantiated METHOD body).
- `template<typename T> struct W{enum{v=5};}; template<typename U> struct X{static const int m=W<U>::v+1;};`
  `X<int>::m` → `6` (PLAIN — non-namespaced — template-id enum in a static-const capture).
- Full numeric_traits shape WITHOUT the namespace prefix (`are_same<V,float>::value`
  in a static-const ternary) → `26 55`.

---

## §6  REAL-HEADER PARSE LANDSCAPE (the worklist)
`#include <H>\nint main(){return 0;}` through `madc --std=c++17 --emit=c11` (error =
parse blocker). **11/19 surveyed parse clean:**
```
OK   <type_traits> <utility> <string> <sstream> <iostream> <fstream> <iosfwd>
OK   <vector> <map> <set> <initializer_list>
FAIL <string_view> :: Expecting integer constant expression   (numeric_traits ns case, §7)
FAIL <memory>      :: Expecting integer constant expression   (numeric_traits ns case, §7)
FAIL <ostream>     :: use of undeclared identifier '_S_categories_size'
FAIL <istream>     :: use of undeclared identifier '_S_categories_size'
FAIL <tuple>       :: Expecting template class name           (fwd-decl class template w/ non-type param)
FAIL <functional>  :: Expecting template class name
FAIL <algorithm>   :: Unknown type 'string' in function pointer typedef
FAIL <array>       :: Expecting integer constant expression
FAIL <bitset>      :: Expecting integer constant expression
```
Note: a header that "parses" (no error) is necessary but not sufficient — it may
still mis-RUN if a constant silently degrades (correctness-first applies). The §7
namespace case is exactly such a silent-degrade hazard.

---

## §7  THE NEXT BLOCKER (highest leverage) — namespace-qualified template-id in a static-const captured during instantiation

**The construct (real, from libstdc++ `<bits/numeric_traits.h>`, pulled by
`<string_view>` and `<memory>`):**
```cpp
template<typename _Value> struct __numeric_traits_floating {
  static const int __max_digits10 =
    (2 + (std::__are_same<_Value, float>::__value  ? 24
        : std::__are_same<_Value, double>::__value ? 53 : 64) * 643 / 2136);
  static const int __digits10      = (std::__are_same<_Value,float>::__value ? 6  : …);
  static const int __max_exponent10= (std::__are_same<_Value,float>::__value ? 38 : …);
};
// __are_same is:  template<typename,typename> struct __are_same { enum { __value=0 }; };
//                 template<typename _Tp>      struct __are_same<_Tp,_Tp>{ enum { __value=1 }; };
```
When `__numeric_traits_floating<float>` instantiates, the static-const initializer
contains `std::__are_same<float,float>::__value` (a namespace-qualified template-id
whose `::__value` is a class-scoped **enum** constant). madc mis-evaluates it.

**Clean-build root-cause map (reducers were `tmp/_q*.mad`; recreate from §5/§7):**
| reducer | construct | context | result | g++ |
|---|---|---|---|---|
| expr | `nn::are_same<int,int>::value` | expression | **works** `1 0` | `1 0` |
| `_q4` | `nn::are_same<V,float>::value` | instantiated **method** body | **works** `1` | `1` |
| `_x`  | `W<U>::v` (no ns) | static-const **capture** | **works** `6` | `6` |
| `_q2` | `are_same<V,float>::value` (no ns) | static-const capture (ternary) | **works** `26 55` | `26 55` |
| `_q3` | `nn::are_same<int,float>::value` (fixed args) | static-const capture | **THROWS** "Expecting int const expr" | `55` |
| `_q`  | `nn::are_same<V,float>::value` | static-const capture (V subst) | **silently `0 0`** (capture not called) | `26 55` |

**Conclusions (reasoned, not assumed):**
- The namespace + template-id + `::value` **resolver is fine** (works in expression
  context, including in an instantiated method body with substitution).
- Plain (non-namespaced) template-id enum in a static-const capture **is fine**
  (`_x`, `_q2`).
- The failure is **specific to the constant-capture path during instantiation** and
  **only with a namespace prefix.** In that path the namespaced id is seen as the
  namespace name **directly followed by `<`** — the `::Name` segment is DROPPED
  (e.g. `nn` rawstr literally "nn", next token `<`). The same tokens reach the
  expression parser intact. So the corruption is upstream of
  `parse_constant_primary`/`fold_constant_qualified_member`, in how the static-const
  initializer tokens are handled during instantiation — NOT in the fold (proven
  correct for `ns::Tmpl<…>::m` once it receives intact tokens), NOT in substitution,
  NOT in the resolver.
- **Layered behavior to chase:** V-substituted (`_q`) and fixed-args (`_q3`) take
  DIFFERENT capture routes — `_q` skips the capture entirely (→ silent 0), `_q3`
  attempts it and throws. Both must be fixed.

**NEXT-SESSION PLAN (do this, in order):**
1. Recreate the reducers (`_q`, `_q2`, `_q3`, `_q4`, expr) from the table above.
2. Instrument the **token stream at the START of `capture_constant_initializer_value`**
   (parser.cpp:5315) — dump `tokens[0..N]` until `;` — for `_q3` (fixed args, capture
   IS called) and compare against the method-body path. Find exactly where `::Name`
   is dropped *before* the const parser runs. **Print to `std::cerr`; gate on
   `getenv("FOLDTRACE")`; ALWAYS `touch src/parser.cpp` before `make` (mtime, §13.1).**
3. The drop almost certainly happens during the instantiation re-parse's handling of
   the member line or an early namespace-qualified-name resolution that consumes
   `:: Name` of a `ns::Tmpl` before the initializer is captured. Fix at that layer
   so the const-capture receives the same intact `ns :: Name < … > :: member` tokens
   the expression parser sees. The fold (§4.8 part 3) already handles them.
4. Separately, find why `_q` (V-substituted) **skips** the capture entirely (→ 0):
   trace the `is_static_member` + `=`-detection gate at parser.cpp ~17114 during
   `nt<float>` instantiation; the V-substituted namespaced initializer may break the
   member-line parse before the capture gate.
5. Validate: `_q`/`_q2`/`_q3` all → g++ values; then re-survey `<string_view>` and
   `<memory>` (they should advance past "Expecting integer constant expression").
   Full gate trio. Add a regression test (numeric_traits-shaped, namespaced).

---

## §8  OTHER OPEN WORKSTREAMS (pick by leverage)

- **`<ostream>`/`<istream>`: `_S_categories_size`.** After §4.8, these advanced from
  the `std::less` blocker to "use of undeclared identifier '_S_categories_size'" —
  almost certainly a class-scoped enum constant (`enum { _S_categories_size = … }`)
  referenced where it isn't yet visible (likely an array dim or a sibling). Bisect
  via `-E` (§9.1); may be unblocked by the §7 work or a near-cousin of it.
- **`<tuple>`/`<functional>`: "Expecting template class name".** Forward-declared
  class template with a non-type param: `template<typename _Tp, size_t _Nm> struct
  array;`. Template forward-declaration parsing gap. Likely a contained
  `TokenTEMPLATE::parse` fix (allow a bodyless `struct Name;` declaration with mixed
  type+non-type params).
- **`<algorithm>`: "Unknown type 'string' in function pointer typedef".** A
  function-pointer typedef whose signature mentions a type madc hasn't resolved in
  that scope. Bisect via `-E`.
- **`<array>`/`<bitset>`: "Expecting integer constant expression".** Likely the same
  family as §7 or a non-type template param used in a constant context; bisect.
- **struct≡class unification** — `project_struct_is_class` +
  `docs/plans/2026-06-07-realheader-track-HANDOFF.md` §6. A struct should parse
  member functions/ctors/dtors/operators like a class body (default access aside).
  HIGH blast radius (blanket struct=class → 65–120 torture regressions historically;
  promotion-on-object-member is the safe pattern — see parser.cpp ~15246-15258).
  §4.8 part 1 took the additive route (route enum-def structs to the class parser)
  rather than touching the struct member loop — keep that discipline.
- **explicit-spec use-site mangling** (§4.2: `tr<int>`→`tr_int32_t` vs registered
  `tr_int`). Orthogonal to partial spec.
- **header-partition plan** (`madc-header-partition-handoff.md`): Step 0 (`__madc__`)
  DONE; Step 4 satisfied-by-generation; Step 5 (trait builtins) v1 landed — continue
  the trait set (`__has_virtual_destructor`, `__is_polymorphic`, `__is_abstract`,
  `__is_final`, `__is_empty`, `__underlying_type`, …; implement only what madc can
  answer EXACTLY). Step 1 (stand up a madc-owned freestanding header dir from
  c2mir's `mirc_*`, retire the `include/madc/` library stubs that shadow real
  headers) is the larger remaining step.
- **retire-std-hardcoding campaign** — `project_string_as_class`,
  `project_cpp_mangled_direct`, `scripts/check-no-std-hardcoding.sh` (wired into
  fulltest). Converges with header-partition Step 1.
- **RUNTIME (not parsing) gap:** mangled-direct std fns (e.g. `std::terminate` →
  `_ZNSt9terminateEv`) are emitted as wrapper calls the MIR loader can't resolve, so
  even a bare `#include <type_traits>; return 0;` fails to RUN (`import of undefined
  item _ZNSt9terminateEv`). That blocks RUNNING real-header programs, distinct from
  parsing — retire-std-hardcoding / mangled-direct territory.

---

## §9  METHODOLOGY THAT WORKS (reuse it — it's why this arc landed cleanly)

### §9.1 `-E` bisection for real-header derails
The live-include path reports **bogus `.mad` coordinates** for errors inside
included headers. To get the REAL line:
```bash
printf '#include <string_view>\nint main(){return 0;}\n' > tmp/_x.mad
bin/madc --std=c++17 -E tmp/_x.mad > tmp/_x_pp.cpp                 # madc-preprocess
bin/madc --std=c++17 --emit=c11 tmp/_x_pp.cpp 2>&1 >/dev/null | grep -m1 error:
#   -> real line in the preprocessed file; read context around it.
```
Classify: `g++ -E` the header, feed gcc's preprocessed file to `bin/madc --emit=c11`
— if THAT parses but madc's own-preprocess fails, it's a madc **preprocessor** gap;
if both fail, a **parser** gap. **Bisect by `#include`, not by `file:line`.**

### §9.2 Instrument, don't assume (Rule #4 in action)
Both the partial-spec misdiagnosis (§4.2) and the A1 dispatch bug (§4.6) were caught
ONLY by adding temporary `std::cerr` tags and READING actual tokens/peeks — not by
trusting a static hypothesis. Pattern: print to `std::cerr`, gate on
`getenv("FOLDTRACE")` (so you don't have to recompile to toggle), run with
`--emit=c11 file >/dev/null 2>trace.log`, grep the tag. **Remove all such
scaffolding before committing** (distinct from the codebase's permanent `DBG()`,
which must never be removed). And **A/B reduce** to localize: `typedef X<S> A;
A::value` working vs `X<S>::value` failing pinned the A1 bug to dispatch, not
instantiation; the §7 table pins the namespace bug to const-capture, not the resolver.

### §9.3 gcc AND clang are BOTH canon (`feedback_two_canon_compilers`)
Reduce every failing case to a minimal repro; compare against `gcc -S -fverbose-asm
-O0` and `clang -S -O0` (or `g++ -E` for preprocessor) BEFORE forming a hypothesis;
for semantics, compile the reducer with g++ and compare OUTPUT. Keep both rules — gcc
has `-fverbose-asm` source annotations, clang doesn't.

### §9.4 Correctness-first; shortcuts categorically unacceptable (`feedback_correct_over_shortcuts`)
RED-FLAG tells = about to hardcode a literal / add a wrapper-shim / special-case a
class higher up / think "good enough for now" → STOP, fix the deepest layer. The
trait work embodies this (implement only what's answerable EXACTLY); the §7 silent-0
degrade is exactly the kind of "parses but wrong" outcome to NOT ship.

### §9.5 Reuse existing mechanisms FIRST (`design-principles`)
The trait evaluator reused the `sizeof` fold site + template-arg parser; the
static-const capture reused the `bracket_dim_…` save/try/restore idiom; A1 reused the
`ExprStep::Redo` re-dispatch; the const-fold reused `instantiate_template_id` +
`resolve_class_static_member_const_value`; §4.8 reused the existing struct→class
delegation rather than editing the struct member loop. Search the 24k-line parser
before adding machinery.

### §9.6 The monomorphization insight
madc instantiates templates by token-substitution + re-parse on use, so inside an
instantiated body the type params are ALREADY concrete by the time the
parser/evaluator sees them. Partial-spec matching and trait evaluation therefore need
NO dependent-placeholder handling — they always operate on concrete DataDefs.

### §9.7 Don't ignore warnings (`feedback_dont_ignore_warnings`)
Analyze every madc build warning AND every g++ warning on `--emit=c11` output. An
ignored g++ warning once hid a real `length()`-returns-void bug.

---

## §10  THE CONSTANT-EVALUATION ARCHITECTURE (mental model — read before touching it)

There are **two** constant evaluators, and confusing them wasted time this session:
1. **`parse_constant_primary` / `parse_constant_integer_expression`** — a
   recursive-descent mini-parser (parser.cpp ~5036), SEPARATE from `parseExpression`.
   Used for: array dimensions (`int a[…]`), enum values, case labels, non-type
   template args, and **static-const member initializer capture** via
   `capture_constant_initializer_value` (speculative save/restore). This is where the
   trait fold (§4.4), the sibling static-const resolver (§4.7), and
   `fold_constant_qualified_member` (§4.8) live.
2. **`parseExpression`** (the full expression parser, ~13519, with the `redo_
   expression_token` loop) — used for runtime expressions AND, lazily, when a
   class-scoped constant is accessed via `Class::member` in expression context. This
   is where the A1 hook (§4.6) and `resolve_class_qualified_expression` live.

A class-scoped integral constant must therefore be resolvable from BOTH:
- expression context → `resolve_class_qualified_expression` (9643) reads
  `static_member_types` (1662) + `static_member_const_values` (1680).
- constant context → `fold_constant_qualified_member` (4938) reads the same maps; and
  unqualified sibling names → `resolve_current_class_static_member_const_value` (1722).

The §7 bug lives in evaluator (1)'s **capture during instantiation** for a
namespace-qualified template-id: the tokens arrive corrupted (`::Name` dropped)
before the fold runs. Evaluator (2) gets them intact. **Do not "fix" it inside the
fold** — the fold is correct; fix the token handling upstream.

---

## §11  KEY FILES & ANCHORS (current line numbers @ HEAD 6344e7d)

| Concern | File:line | Symbol |
|---|---|---|
| Static-member type resolver | src/parser.cpp:1662 | `resolve_class_static_member_type` |
| Static-member const-value resolver | src/parser.cpp:1680 | `resolve_class_static_member_const_value` |
| Current-class const-value resolver (A2) | src/parser.cpp:1722 | `resolve_current_class_static_member_const_value` |
| Template-id instantiation entry | src/parser.cpp:2315 | `instantiate_template_id` |
| Partial-spec match call site | src/parser.cpp:2499 | in `instantiate_template_use` |
| Trait recognizer / evaluator | src/parser.cpp:4092 / 4133 | `is_type_trait_builtin` / `evaluate_type_trait` |
| **Qualified const-fold (§4.8.3)** | src/parser.cpp:4938 | `fold_constant_qualified_member` |
| Constant-expr primary | src/parser.cpp:5036 | `parse_constant_primary` |
| ↳ trait fold hook | src/parser.cpp:5059 | `is_type_trait_builtin` branch |
| ↳ sibling static-const hook (A2) | src/parser.cpp:5064 | comment "Unqualified sibling…" |
| **Static-const initializer capture** | src/parser.cpp:5315 | `capture_constant_initializer_value` |
| Partial-spec matcher | src/parser.cpp:9939 | `match_partial_specialization` |
| Qualified-class expr resolver | src/parser.cpp:9643 | `resolve_class_qualified_expression` |
| ttDataType expr arm | src/parser.cpp:10000 | `parseExpr_dataTypeArm` |
| ttIdentifier expr arm | src/parser.cpp:10179 | `parseExpr_identifierArm` |
| ↳ A1 template-id hook | src/parser.cpp:11498 | comment "Template-id in expression context" |
| ttDataType call site (honors Redo) | src/parser.cpp:13551 | `goto redo_expression_token` |
| ttIdentifier call site (A1 Redo) | src/parser.cpp:13600 | `goto redo_expression_token` |
| Redo loop label | src/parser.cpp:13519 | `redo_expression_token:` |
| struct→class delegation predicate | src/parser.cpp:14164 | `cpp_struct_body_needs_class_parser` |
| ↳ enum-def detection (§4.8.1) | src/parser.cpp:14300 | comment "An enum DEFINITION member" |
| Enum parser | src/parser.cpp:18327 | `TokenENUM::parse` |
| ↳ class-scoped enum registration (§4.8.2) | src/parser.cpp:18467 | comment "Enumerator of an enum DEFINED inside" |
| Static-const map | include/datadef.h:668 | `static_member_const_values` |
| Static-member-type map | include/datadef.h:663 | `static_member_types` |
| class scope stack | include/madc.h:1149 | `class_scope_stack` |
| fold decl | include/madc.h:1768 | `fold_constant_qualified_member` |

Tests added this arc: `tests/{testpartialspec,testtypetraits,teststaticconstmember,
testtemplateidvalue,teststaticconstsibling,testanonenum}.{mad,expect}`.

---

## §12  DataDef / TYPE-MODEL FACTS (so you don't re-derive them)
- `enum class BaseType { btSimple, btStruct, btFunct, btClass }` — **no btEnum,
  btUnion** (datadef.h:24).
- `DataDefCLASS : public DataDefSTRUCT` (datadef.h:656) — a class IS-A struct;
  `basetype()` → btClass for class, btStruct for struct.
- Union = `DataDefSTRUCT` with `union_layout == true`. Class/struct = false.
- Enum = `DataDefENUM` (datadef.h:838), `DataType::dtINT`, NOT a DataDefSTRUCT.
- Pointer = `DataDefPTR` (datadef.h:807) with `base_type` = pointee, `is_pointer()`
  true. **`DataDefPTR::is_integer()` returns true** (codegen convenience — do NOT use
  scalar predicates for trait faithfulness).
- Bases: `DataDefCLASS::base_class` (single) + `std::vector<BaseSpec> bases` (MI).
- `static_member_types` (name→DataDef*) and `static_member_const_values`
  (name→int64_t) live on **DataDefCLASS** — so a plain `DataDefSTRUCT` must be
  promoted to DataDefCLASS (or routed to the class parser, §4.8.1) to hold them.
  `struct S { static const int x=5; }` already works, so the promotion/routing path
  exists.
- `ddBOOL/ddINT/ddINT32/ddINT64/ddUINT64/ddVOID/…` are global DataDef instances in
  parser.cpp (~5150 region). `TokenType::ttDataType == 14`, `ttIdentifier == 8`
  (tokens.h:18-28). `TokenID::tkNS` = `::`, `tkLT` = `<`.
- `TemplateDef` (madc.h): `typeparams`, `typeparam_defaults`, `typeparam_is_type`,
  `has_non_type_params`, `class_name`, `body`, `defining_namespace`, `owner_class`,
  + `is_partial_specialization`, `spec_pattern`. `template_map` = name → per-namespace
  primaries; `partial_spec_map` = name → partial specs (separate, by design);
  `template_alias_map` = alias templates.

---

## §13  PROCESS LESSONS

### §13.1 NAS mtime staleness (cost real time this session)
On this QNAP filesystem, `make -C src` sometimes reports "up to date" and SKIPS
recompiling an edited `src/parser.cpp` (mtime granularity / clock skew), so the
running `bin/madc` reflects STALE code. Symptom: traces that "should" fire don't, or
behavior flip-flops between rebuilds. **Always `touch src/<file>.cpp` before
`make -C src`** when iterating on one file; when results look impossible do
`make -C src clean && make -C src` to be certain. Several mid-session observations
this session were stale-build artifacts; only clean-build results were trusted for
the commits.

### §13.2 JIT exit-code artifact
madc's JIT does NOT propagate a `return method()` value through `main`'s exit code,
even for a NON-template class — `int main(){ return obj.v(); }` gives exit 0
regardless. **Verify method/const results via OUTPUT (printf) or `--emit=c11 | gcc |
run`, never the process exit code.**

### §13.3 Torture timeouts are flaky under load
`run_gcc_testsuite.py` caps each test at **5s**; heavy tests (memcpy-a1/a2/a4, ~3.8s
clean) flake to "timed out" when SMAUG/fulltest run concurrently. Run torture alone;
treat only compile/runtime FAIL-count changes as regressions.

---

## §14  VERIFY-ON-RESUME BLOCK (run first)
```bash
cd /workspace/madc
git rev-parse --short HEAD            # expect 6344e7d (or later)
git branch --show-current             # feature/realhdr-parse-gaps2-claude
git status --short                    # expect clean
cat MIR_COMMIT                        # 2ffebff
make -C src 2>&1 | grep -iE ': error|: warning' | head    # clean build
make -C src fulltest 2>&1 | grep -E "passed,|FAIL: tests" | tail -8   # 534 / 4
# the six landed capabilities (all match g++):
bin/madc tests/testpartialspec.mad         # 0 1 42
bin/madc tests/testtypetraits.mad          # 11000 10 10 1101 10 / 4 7
bin/madc tests/teststaticconstmember.mad   # 127 -128 255 127
bin/madc tests/testtemplateidvalue.mad     # 1 1 0 / 1 0 / 3
bin/madc tests/teststaticconstsibling.mad  # 0 1 8 15
bin/madc tests/testanonenum.mad            # 1 2 3 5 6 / 3 4
# the next frontier (should print the §7 blocker):
printf '#include <string_view>\nint main(){return 0;}\n' > tmp/_sv.mad
bin/madc --std=c++17 --emit=c11 tmp/_sv.mad 2>&1 >/dev/null | grep -m1 error:
#   -> "Expecting integer constant expression"  (numeric_traits ns case, §7)
# torture (ALONE, ~20 min) + SMAUG soak per §2.3 before declaring any change done.
```

---

## §15  THE ONE-PARAGRAPH RESUME SCRIPT
On resume: read this file + `AGENTS.md` Top-10 + the memory files in §1.3. Run §14 to
confirm HEAD `6344e7d`, clean tree, fulltest 534/4, and the six landed capabilities.
This branch advanced the C++ `<type_traits>`/template/enum/member-constant frontier:
partial specialization, `__madc__` identity, trait builtins (expr + constant), static-
const member value capture, the **A1 keystone** (template-id `X<T>::value` in
expression context — fix was expression DISPATCH, not instantiation), the **A2-rest**
sibling static-const fold, and **class/struct anonymous enums + qualified/template-id
constant folding**. Real `<type_traits>` now parses; 11/19 family headers parse. The
single highest-leverage next task is **§7**: the namespace-qualified template-id in a
static-const member captured during instantiation
(`std::__are_same<float,float>::__value`, blocking `<string_view>`/`<memory>`) — fully
reduced and root-caused (the const-capture path receives the namespaced id with its
`::Name` segment dropped, while the expression parser handles it intact; the fix is
upstream of the proven-correct `fold_constant_qualified_member`, with an
instrumentation plan in §7). Use the `-E` bisection + instrument-don't-assume + g++
oracle methodology (§9); be correctness-first (never ship a silently-wrong constant);
`touch` before `make` (§13.1); gate every change with fulltest + torture (alone) +
SMAUG; do NOT push; do NOT promote to master.

END OF FULL HANDOFF.
