# REHYDRATION HANDOFF — Parser restoration (DONE) + real-header track (in progress)

Date: 2026-06-07. Branch **`feature/realhdr-parse-gaps2-claude`** (off `develop`,
**local only, NOT pushed**). HEAD **`c2126dc`**. Working tree **clean**.
fulltest baseline: **522 passed / 4 failed / 0 timed out / 26 skipped**.

> This document supersedes `docs/plans/2026-06-07-parser-restoration-HANDOFF.md`
> for *current* state. That older doc describes the early restoration sub-thread
> (the `DelimDepth`/angle-scanner work, steps 1–6); this doc covers the COMPLETED
> restoration (steps 7–25) and the real-header track that followed. Read this one
> first; the memory file `project_parser_restoration.md` is the one-screen index.

This is self-contained. After reading it you can continue without re-deriving
anything. Verify live state with §1 commands before editing.

---

## 0. TL;DR — what to do on resume

Two big things happened this session, both on this branch:

1. **The parser restoration is COMPLETE.** `parseExpression` went 3,577 → ~172
   lines (original shunting-yard shape), and free `static f(Program &pgm, …)`
   functions went **138 → 2** (the 2 left are intentional function-pointer
   callbacks). Both of the user's headline goals are met. Nothing left to do
   here except optional polish (§2.6).

2. **The real-header track resumed** (madc parsing real system C++/libc headers).
   The restored parser made the *parser-only* probe column nearly all-green. I
   then cleared the entire `<wchar.h>`/`sys/cdefs.h` **preprocessor** wall and
   fixed a CIR **emitter** bug, landing 3 header-track commits. `<iosfwd>` is now
   fully green. The string/streams family advanced to ONE remaining blocker.

**UPDATE 2026-06-07 (later same day): bug B is FIXED.** The `template_map`
namespace-scoping fix landed (`5ea75a2`) + an amalgamation follow-up (`4f4fbd2`).
`char_traits` now parses; the 8 string/streams headers advanced past it. See
the **§5 "RESOLVED"** banner and the updated §3.3 table / §13 for the new
frontier. The text below §5 is the original (pre-fix) plan, kept for the
mechanism record. **New immediate-next: the `'lconv' is not a declaration in
'::'` blocker** (string + all 5 streams headers, §6/§13).

**The immediate-next task** (the live thread when this handoff was written): fix
the **last char_traits blocker**, which is fully root-caused to a 3-line repro —
**`template_map` is keyed by bare name, not namespace-scoped**, so a same-named
class template in a second namespace overwrites the first and qualified lookup
of the shadowed one fails. Full mechanism + fix plan in **§5**. It's invasive
(template registration/lookup core) so it needs careful, test-gated work.

Each step: small, **`make -C src fulltest` after every change** (must stay
522/4/0/26 — see §1 for the 4 expected reds), gcc/clang-canon methodology
(reduce first, fix deepest layer).

---

## 1. Environment, build, test, baseline

- Repo: `/workspace/madc`. Branch `feature/realhdr-parse-gaps2-claude` (off
  `develop`). HEAD `c2126dc`. **Do not push; do not promote to master** (parity
  gate — see `.claude/rules/branching.md`). `develop` untouched this session.
- Backend: `madc parser → cir_node (MC11-IR) → c2mir → MIR → JIT`. The MIR fork
  is at `/workspace/mir` (branch `develop`, pinned by `MIR_COMMIT`).
- Build/test:
  ```bash
  make -C src               # build bin/madc + lib
  make -C src fulltest      # unit (doctest) + all integration tests
  ```
- **Baseline 522/4/0/26.** The 4 reds are PRE-EXISTING and unrelated to this
  work: `testdefer`, `testfstream`, `testlargesizeofquery`, `testloop`. (The
  count was 521 before this session; +1 is the new `testarraytypedefstruct`.)
  A change is "green" iff it keeps exactly those 4 reds and no others.
- `parser.cpp` is **24,311 lines**. `--dump-source` dumps madc's preprocessed
  token stream (useful for preprocessor bugs); `--emit=c11` renders the
  cir_node tree as C (the "what c2mir sees" view); `--dump-cir` dumps the tree.
- Scratch files go in `tmp/` (gitignored). Many reducers from this session are
  there (§7) but `tmp/` is not committed — recreate from §7 if gone.

---

## 2. The parser restoration (COMPLETE — context, not a TODO)

### 2.1 Why (the user's mandate)
The user said madc's parser had been degraded by Claude+Codex from its original
**method-based, state-machine** design into a sprawl of free functions that
thread a `Program &pgm` parameter everywhere, plus a 3,577-line `parseExpression`
"god function". Original `a22343e`: 1,401 lines, **0** free `f(Program&,…)`
functions, everything a `Program::` method, `parseExpression` a ~175-line
shunting-yard with local `stack<TokenBase*> exStack/opStack`. The user's metric
(stated explicitly mid-session): **NOT line count** — it's (a) the count of
external non-method `f(Program&,…)` functions (drive to 0 by making them
`Program` methods that use class fields internally), and (b) breaking up
`parseExpression`.

### 2.2 Goal 1 result — free functions → Program methods: 138 → 2
Every `static f(Program &pgm, …)` free function was re-homed onto `Program` as a
method (so it reaches `tokens`, `compounds`, `class_scope_stack`, etc. as
members instead of threading `pgm`). The **only 2 remaining** are
`register_std_namespace_spec` / `register_madc_namespace_spec` (parser.cpp ~5874)
— they MUST stay free because they're registered as **function-pointer
callbacks** via `namespace_registry.add_namespace("std", register_std_namespace_spec)`.
So Goal 1 is effectively 100%.

Families re-homed, in order (commit → step): constant-expression evaluator
(37eed02, s7), array-dimension classifier `bracket_dim_*` (e618106, s8), K&R
old-style params (4916e9d, s13), namespace-resolution helpers (f20c63e, s14),
static_assert trio (6407708, s15), type-query cluster (6a4b037, s16),
cast-parsing family (cac9492, s17), template-machinery leaves (9485bf5, s18),
template-machinery core 16 fns (c0a85bc, s19), resolve_*_type_token quartet
(63c2132, s20), type-name resolution helpers (ce6da8e, s21), class-body/
deferred-body cluster (d0f94f4, s22), GNU/C23 attribute consumers (b8fdeb8, s23),
param-signature/qualified-declarator cluster (e42fad6, s24), all remaining
singletons (e3c078c, s25). Two parser-local types were lifted to `madc.h` so the
new method signatures resolve: `CppSymKind` enum and `ParsedParamSig` struct
(both just before `class Program`).

### 2.3 Goal 2 result — parseExpression decomposed: 3,577 → ~172 lines
`Program::parseExpression` is now at parser.cpp **12924** and ends at **13095**
(~172 lines): locals → the shunting-yard `while (!done && tb)` loop with the
`switch(tb->type())` dispatch → `opStack` drain → `return exStack.top()`. The
four substantial arms became named `Program` methods:
- `parseExpr_dataTypeArm` (parser.cpp **9443**) — ttDataType (type name in expr).
- `parseExpr_symbolArm` (**9577**) — ttSymbol (`;` terminator, `,` comma operator).
- `parseExpr_identifierArm` (**9622**) — ttIdentifier, the ~1,433-line giant.
- `parseExpr_operatorArm` (**11068**) — ttMultiOp/ttOperator, the ~1,836-line giant.
(`ttKeyword` falls through into the `ttIdentifier` dispatch.)

### 2.4 The ExprStep protocol (how the arms were extracted) — SET IN STONE
Defined in `include/madc.h` line **1547**:
```cpp
enum class ExprStep { Break, Continue, Redo, Done, Return };
```
Each extracted arm returns one of these, mapping 1:1 to its original inline
control flow:
- `Break`   = fall to the per-token epilogue (peek/advance next token).
- `Continue`= re-enter the loop; the arm already advanced `tb` (was `continue;`).
- `Redo`    = re-dispatch the (rewritten) `tb` without advancing (was
  `goto redo_expression_token;`).
- `Done`    = terminate the expression (was `done = true;`).
- `Return`  = early-return a specific `TokenBase*` from parseExpression,
  bypassing the opStack drain (operator arm only; carried via an out-param
  `TokenBase *&result`).
The loop's dispatch does: `if (step==Redo) goto redo_expression_token; if
(step==Continue) continue; if (step==Return) return arm_result; if (step==Done)
done = true;` then `break`.

**THE KEY EXTRACTION TECHNIQUE (compiler-as-oracle):** after moving an arm body
verbatim into a method, the C++ compiler flags every ARM-LEVEL `break;`/`continue;`
as "break/continue statement not within loop or switch" (illegal at method
scope) — while INNER loop/switch breaks compile fine. So you convert *exactly*
the flagged ones to `ExprStep` returns and leave the rest. That's why the
behavior is **byte-identical**, not merely test-green. The operator arm also had
3 direct `return <TokenBase*>` statements (early exits) → these needed the new
`ExprStep::Return` + `result` out-param.

### 2.5 The coordinated-batch conversion recipe (free fn → method) — REUSE THIS
For the family conversions I used a Python transform. The hardened recipe (after
two pitfalls, both fixed — see §8):
1. **Strip forward declarations FIRST** (so `find()` matches the definition, not
   a fwd decl with an identical first line). Regex:
   `static[^;{}\n]*\bNAME\b\s*\([^{};]*\)\s*;\s*\n`.
2. For each def (process **bottom-to-top** so line indices stay valid):
   - Find the def line (`^static\b.*\bNAME\s*\(\s*Program\s*&\s*pgm`).
   - Find the matching close brace with a **string/char/comment-aware brace
     matcher** (NOT naive `{`/`}` per-line counting — that runs away on braces
     inside string/char literals or comments; this bit me on step 25, §8).
   - Replace the signature: `\Astatic\s+(?P<ret>.*?)\bNAME\s*\(\s*Program\s*&\s*pgm\s*(?:,\s*)?`
     → `<ret>Program::NAME(`.
   - Xform the FULL body (not just the sig line — that was the step-19 pitfall):
     `pgm.`→`` ; cluster-internal calls `C(\s*pgm\s*,\s*`→`C(` and `C(\s*pgm\s*\)`→`C()`
     for every C in the batch (multiline-tolerant!); then bare `\bpgm\b`→`*this`
     (free-helper args). Assert no stray `pgm` remains.
3. **Strip def-side default args** (move to the in-class declaration; C++ forbids
   defaults in both). Re-add defaults that lived in a removed fwd decl to the
   new in-class decl.
4. External call sites: `NAME(\s*\*this\s*,\s*`→`NAME(` and `NAME(\s*\*this\s*\)`
   →`NAME()` (method callers); `NAME(\s*pgm\s*,\s*`→`pgm.NAME(` and `NAME(\s*pgm\s*\)`
   →`pgm.NAME()` (still-free callers).
5. Add the method decls to `madc.h` (extract authoritative sigs from the
   transformed defs). Assert NO `Program &*this` anywhere (corruption guard).
6. `make -C src` (compiler catches stray `pgm`, arity/default mismatches), then
   `make -C src fulltest`.

### 2.6 Optional restoration polish left (LOW priority, not blocking anything)
- Dedup `TokenSTRUCT::parse` vs `TokenCLASS::parse` (~2,300 near-identical lines).
- The `operator<` resolution end-to-end (the method-only gate ~parser.cpp 11465
  in the OLD handoff — verify current line). Free/template `operator<` scans but
  doesn't fully resolve.
- Further decompose other large methods if desired.

---

## 3. The real-header track (the active work)

### 3.1 Goal
North star: madc parses **real** system C++/libc headers (retiring curated
stubs), reaching C23/C++23 compliance, all through the one `cir_node`/MC11-IR →
c2mir → MIR backend. The concrete instrument is `scripts/probe_real_headers.sh`.

### 3.2 The probe harness — two columns, what they mean
`bash scripts/probe_real_headers.sh` parses a default set of std:: headers TWO
ways and prints the first error of each:
- **`madc` column** = madc does its OWN preprocessing + parse of the real header.
- **`pp` column** = feed gcc-PREPROCESSED source (macros expanded, `# line`
  markers stripped) so madc only PARSES.
**Interpretation:** if `pp` passes but `madc` fails → the gap is in madc's
**PREPROCESSOR** (macro/directive handling), NOT the parser. If BOTH fail at the
same construct → it's the **PARSER**. The script's own header comment documents
this and the known culprits.

### 3.3 Current probe results (after the bug-B fix, 2026-06-07)
```
HEADER         madc (own preprocess)                                pp (parser-only)
type_traits    OK                                                   OK
utility        OK                                                   OK
char_traits    OK   <-- bug B fixed                                 OK
iosfwd         OK                                                   OK
memory         690:6 error: Too many parameters                     OK
vector         690:6 error: Too many parameters                     OK
string         53:15 error: 'lconv' is not a declaration in '::'    OK   <-- past char_traits
string_view    768:45 error: Expecting a type argument to iterator<>  OK <-- past char_traits
ostream        53:15 error: 'lconv' is not a declaration in '::'    OK
istream        53:15 error: 'lconv' …                               OK
iostream       53:15 …                                              OK
sstream        53:15 …                                              OK
fstream        53:15 …                                              OK
map            36:9 error: Expecting type in class definition       OK
set            36:9 error: Expecting type in class definition       OK
algorithm      52:13 error: 'abs' is not a declaration in '::'      OK
```
The char_traits fix advanced all 8 string/streams headers. The string/streams
family now shares ONE new blocker: `'lconv' is not a declaration in '::'` —
almost certainly the same shape as `algorithm`'s `'abs'` (a global-namespace
`using ::name;` of a libc symbol, here `lconv` from `<clocale>`), so fixing
that family likely clears string + all 5 streams + algorithm together.
`string_view` has its own next step (`iterator<>` template-arg resolution).
**The restoration's big win:** the `pp` (parser-only) column is now essentially
all-green — including `map`/`set`/`vector`/`string`/streams. The OLD headline
blocker for `<map>`/`<set>` (the `__ptr_cmp`/`operator<` `<…>` over-consumption)
is GONE — the unified angle-scanning machinery from the restoration fixed it.
So the remaining `madc`-column failures are now a small, sorted set (§5, §6).

---

## 4. Fixes landed this session (header track)

### 4.1 `0665b86` — array-typedef emitter bug + `__gnuc_va_list`
Two coupled fixes that cleared the FIRST `<wchar.h>` blocker.
- **Emitter bug (deepest layer):** In `src/cir_builder.cpp`, the top-level
  declaration emit driver uses a lambda `struct_behind(DataDef*)` to identify the
  struct behind a typedef (to mark its tag in an `emitted_structs` set so a
  second typedef of the same tag emits only `struct Tag`, not the full body).
  `struct_behind` peeled one POINTER level but **not `DataDefCArray`** (array)
  layers, so for an ARRAY typedef (`typedef struct Tag {..} NAME[N];`) it
  returned NULL → the tag was never marked emitted → a SECOND array typedef of
  the same tag re-emitted the full body → **c2mir rejected the duplicate ("tag X
  redeclaration")**. Fix: peel `DataDefCArray::element_type` first in
  `struct_behind`. Find it: `grep -n "auto struct_behind = " src/parser.cpp` —
  WAIT, it's in `cir_builder.cpp` (the lambda inside the emit driver). Reduced to
  a 2-line, va_list-free repro; test `tests/testarraytypedefstruct.mad`.
- **`__gnuc_va_list`:** real `<wchar.h>` line 43 does `#define __need___va_list;
  #include <stdarg.h>; typedef __gnuc_va_list va_list;`. Embedded `stdarg.h`
  didn't define `__gnuc_va_list`. Added (include/madc/stdarg.h line 29):
  `typedef struct __madc_va_list_tag __gnuc_va_list[1];` — a DIRECT typedef of
  the SAME tagged struct, with `va_list` left byte-identical (so the `va_start`
  intrinsic is unaffected). The prior attempt (per the older handoff's gotcha)
  added this and regressed 8 va_args tests because the emitter bug above made the
  duplicate-tag emission fail; fixing the emitter is what let this land. Without
  the emitter fix, `typedef __gnuc_va_list va_list;` (va_list = struct[1] and
  __gnuc_va_list = struct[1]) emits the struct body twice → "tag redeclaration".

### 4.2 `68dee85` — embedded `sys/cdefs.h` glibc macros
madc reads its OWN embedded `include/madc/sys/cdefs.h` (a stub that hardcodes the
OUTCOMES of glibc's attribute conditionals — e.g. `#define __THROW` empty — rather
than replicating `__GNUC_PREREQ`/`__has_attribute`). It was missing the
`__nonnull` family and inline-family macros, so real prototypes like
`wchar_t *wcscpy(...) __THROW __nonnull((1,2));` left a dangling macro after the
param list → madc read it as a K&R definition ("Expecting brace after function
declaration"); and optimized inline blocks tripped on undeclared `__extern_inline`.
Added (matching glibc's no-attribute fallback shapes): `__nonnull`,
`__attribute_nonnull__`, `__attr_access[_none]`, `__attr_dealloc[_free]`, `__wur`,
the `__attribute_*__` family, `__glibc_macro_warning`, `__attribute_artificial__`,
and the inline family `__always_inline`/`__extern_inline`/`__extern_always_inline`/
`__fortify_function`. **Deliberately NOT** adding feature-selection macros
(`__GNUC_PREREQ`/`__glibc_has_*`) — the stub avoids those by design (hardcode
outcomes). **Result: real `<wchar.h>` parses end-to-end; `<iosfwd>` fully green;**
the string/streams family advanced past the entire wchar/cdefs wall.

### 4.3 `c2126dc` — namespace-qualified types as struct members (bug A of the bisection)
The struct-member type parser (`src/parser.cpp`, in `TokenSTRUCT::parse`'s member
loop, around the throw at line **14036** "Expecting type in struct definition,
got '<name>'") only consulted the unqualified `datatype_map` for an identifier
member type. So a namespace-/class-qualified member type (`a::T`, `ns::Tmpl<...>`,
`Outer::Inner`) was rejected — at global scope and inside a namespace alike. Fix:
when the bare lookup misses AND a `::` follows, consume the identifier and fall
through to the shared `pgm.resolve_declared_type_token(tn, true, true)` (which
handles `::`-qualified and template-id types via `resolve_namespaced_type_token`
+ `instantiate_template_use`); only taken when `::` follows so genuine non-types
still error. Test `tests/testqualifiedmembertype.mad`. (This is ONE of the two
bugs behind the char_traits failure; see §5.)

---

## 5. ~~THE OPEN BLOCKER~~ — `__gnu_cxx::char_traits` (bug B) — **RESOLVED 2026-06-07**

> **RESOLVED in `5ea75a2`** (+ amalgamation `4f4fbd2`). `template_map` is now
> `map<string, vector<TemplateDef>>` (per-namespace variants keyed by bare name),
> all selection funnels through a new `find_template(name, ns_hint)` helper,
> `register_template()` owns insert/merge, and `instantiate_template_use()` takes
> an `ns_hint` (and COPIES the selected `TemplateDef` to dodge a vector-realloc
> dangling-reference hazard). The concrete-instantiation mangled key folds in the
> namespace ONLY when the bare name actually has >1 variant, so every
> single-namespace template's internal tag is byte-identical (zero churn for
> std::). The expression-context qualified call passes the resolved namespace as
> `ns_hint`; the type path was left on bare-name selection because its only
> collision (`std::char_traits : public __gnu_cxx::char_traits`) resolves before
> `std::char_traits` registers. Regression test `tests/testtemplatenamespacescope`.
> Follow-up `4f4fbd2` collapsed 10 hand-rolled `alias_use`→`use` probe sites onto
> one `instantiate_template_id(name, tb, ns_hint)` seam so the hint flows
> uniformly. fulltest 524/4/0/26, no-std-hardcoding gate green.
>
> **Note (qualified template-id as a DECLARATION type):** `ns::Tmpl<int> v;` as a
> variable declaration still fails ("Expecting ';' after struct member") — but it
> fails with a SINGLE namespace too, so it is a SEPARATE pre-existing gap (the
> declaration-type parser doesn't resolve qualified template-ids), NOT part of
> bug B. Worth a future thread; the expression path (`ns::Tmpl<int>::f()`) works.
>
> The original (pre-fix) plan follows for the mechanism record.

### (historical) THE OPEN BLOCKER — `__gnu_cxx::char_traits` (bug B)

This blocks 8 headers: string, string_view, char_traits, ostream, istream,
iostream, sstream, fstream (all `473:4 'char_traits' is not a member of namespace
'__gnu_cxx'`). It is a GENUINE parser/name-resolution bug (same error in BOTH
probe columns).

### 5.1 The bisection chain (how I got from the header to the root cause)
- `char_traits.h:473` = the `};` closing `std::char_traits<char>` (an explicit
  specialization, lines ~337–473). Its method bodies call
  `__gnu_cxx::char_traits<char_type>::length(__s)` etc. (lines 397/409/421/…).
- Local copy for editing: `cp /usr/include/c++/13/bits/char_traits.h tmp/ct_bisect.h`,
  truncate + close namespaces to bisect. Found: the `std::char_traits` PRIMARY
  (line 331, `: public __gnu_cxx::char_traits<_CharT>`) resolves `__gnu_cxx::char_traits`
  fine; it only fails later in the explicit specialization.
- Reductions (in `tmp/`, recreate from §7):
  - `_ct3` — `std::char_traits` FORWARD-DECLARED + method ref to
    `__gnu_cxx::char_traits<char_type>::length` → **OK**.
  - `_ct5` — `std::char_traits` DEFINED & deriving + same ref → **FAILS** (15:6).
  - `_ct13` — `std::char_traits` DEFINED, NOT deriving + ref → **FAILS** (so
    derivation is NOT required; a DEFINED primary is).
  - `_ct14` — DEFINED `std::char_traits` + ref from a plain function in `std`
    (no explicit spec) → **FAILS** (4:41). Minimal expression form.
  - `_tmpl` — char_traits-FREE: `namespace a { template<class C> struct W { static
    int f(){return 1;} }; } namespace b { template<class C> struct W {…2…}; } int
    main(){ return a::W<int>::f(); }` → **FAILS** "'W' is not a member of namespace
    'a'". THE CLEAN ROOT-CAUSE REPRO.

### 5.2 Root cause
`template_map` (declared **include/madc.h:1079**:
`std::map<std::string, TemplateDef> template_map;  // name -> definition`) is
keyed by **BARE NAME**, not namespace-scoped. So when `std::char_traits` (a
defined template) is registered, it OVERWRITES `__gnu_cxx::char_traits` in
`template_map` (both key "char_traits"). Subsequent qualified lookup of
`__gnu_cxx::char_traits` can't find the shadowed `__gnu_cxx` version → "not a
member of namespace '__gnu_cxx'". A FORWARD-DECLARED `std::char_traits` (`_ct3`)
doesn't register/overwrite, so it worked.

`TemplateDef` ALREADY carries a `defining_namespace` field (used by
`template_declared_in_namespace`) — so the disambiguating data exists; the map
keying and the qualified lookup just don't use it.

### 5.3 The two qualified-resolution PATHS (both need the namespace fix)
The qualified name `__gnu_cxx::char_traits<...>` is resolved by different code in
different syntactic contexts:
- **Struct-member type** (`a::T *p;`) — `TokenSTRUCT::parse` member loop, ~line
  14036. **FIXED in `c2126dc`** (falls through to `resolve_declared_type_token`).
- **Expression** (`__gnu_cxx::char_traits<char>::length(0)`) — in
  `parseExpr_identifierArm`/the namespace-member resolver around parser.cpp
  **10703–10709** (`vmi = nsi->second.find(member_name); if (== end) Throw "is
  not a member of namespace"`). This path looks up the member in
  `namespace_map[ns]` / `namespace_datatype_map[ns]` — and for a TEMPLATE that
  lives in `template_map` keyed by bare name, the shadowing bites here.
- Other throw sites with the same string: parser.cpp 9157, 12836 (expr `::`
  forms), 13469 (the `using`-declaration handler — NOT the char_traits path).

### 5.4 Fix plan (INVASIVE — template registration/lookup core; regression risk)
The right fix: make template storage/lookup namespace-aware.
- **Storage:** key `template_map` by QUALIFIED name (e.g. `"__gnu_cxx::char_traits"`)
  — OR keep bare-name key but make it `map<string, vector<TemplateDef>>` and
  disambiguate by `defining_namespace`. Qualified key is simpler to reason about.
- **Registration sites** (audit ALL): parser.cpp ~**19733/19734** (guarded insert
  — first wins) and ~**19858/19875** (`template_map[class_name] = td;` —
  unconditional overwrite). Register under the qualified name using
  `current_namespace` + `class_name`.
- **Lookup sites** (audit ALL): parser.cpp ~**2273** (`template_map.find(tname)`),
  ~**2726**, ~**9430**, plus the `template_declared_in_namespace` method (already
  namespace-aware via `defining_namespace`). For a QUALIFIED use `ns::Tmpl`, look
  up `ns::Tmpl` (or filter by `defining_namespace==ns`). For an UNQUALIFIED use,
  search the current namespace scope chain → global (mirror
  `resolve_namespace_name_in_scope`, now a `Program` method).
- The namespace-qualified TYPE resolver `resolve_namespaced_type_token`
  (parser.cpp ~1745, a `Program` method) and `resolve_declared_type_token`
  (~2918; delegates to the former at ~3079) must consult the namespace-scoped
  template registry for `ns::Tmpl<args>`. Instantiation goes through
  `instantiate_template_use` / `instantiate_template_alias_use` (now `Program`
  methods, restoration step 19).
- **Validate** against `tests/testtemplate*`, vector/map/set tests, AND the
  `_tmpl` / `_ct14` reducers AND the real `bits/char_traits.h`, then full probe +
  `make -C src fulltest`. Risk surface = ALL template usage; go incrementally,
  fulltest after each sub-step. Add a regression test (a `_tmpl`-style two-
  namespace same-named-template + qualified call).
- **Caution:** `std` types interplay with the embedded headers and the
  retire-std-hardcoding work — confirm you don't shadow/break `std::` template
  resolution (vector/map/set). Run the `scripts/check-no-std-hardcoding.sh` gate
  if you touch std registration (it's wired into fulltest).

---

## 6. Other remaining header blockers (after char_traits) — lower priority

All `madc`-column only (parser-only `pp` column is OK), i.e. likely PREPROCESSOR
or near-parser:
- **`memory`, `vector` — `690:6 Too many parameters`.** Same logical line (690) →
  one root cause hit by both. Likely a macro mis-expansion producing an
  over-long parameter list, OR a real parser limit on parameter count. Reduce by
  finding what's at the relevant header line under madc's `--dump-source`.
- **`map`, `set` — `36:9 Expecting type in class definition`.** Note: the
  parser-only `pp` column is OK for map/set, so this is the madc PREPROCESSOR (a
  `_GLIBCXX_*`/macro gap), not the parser. Use the §3.2 two-column method:
  `g++ -E` the failing inner header, diff madc `--dump-source`, find the
  unexpanded/mis-expanded macro.
- **`algorithm` — `52:13 'abs' is not a declaration in '::'`.** A `using ::abs;`
  or similar global-namespace using-declaration of a libc function; likely
  `abs` isn't declared at global scope in madc's view at that point.

Methodology for each: reduce to the smallest failing `#include` (probe inner
sub-headers individually, like §4.2 did for wchar), then `--dump-source` vs
`g++ -E` to classify preprocessor-vs-parser, then minimal reducer, then fix the
deepest layer.

---

## 7. Reducer inventory (tmp/, gitignored — recreate as needed)

| file | what it tests | result |
|---|---|---|
| `tmp/_w.mad` | `#include <wchar.h>` | now OK (was the 43:22 / 100:22 / 342:15 chain) |
| `tmp/_cd.mad` | `#include <sys/cdefs.h>` + `__THROW __nonnull` | now OK |
| `tmp/_va.mad` | `#define __need___va_list; #include <stdarg.h>; typedef __gnuc_va_list va_list;` | now OK |
| `tmp/_dup2.mad` | `typedef struct S{int x;} A[1]; typedef struct S B[1];` (array re-emit) | now OK (1 body) |
| `tmp/_ct.mad` | `#include bits/char_traits.h` | FAILS 473:4 (bug B) |
| `tmp/_ct3.mad` | fwd-decl std::char_traits + method ref | OK |
| `tmp/_ct5.mad` | DEFINED+deriving std::char_traits + method ref | FAILS (bug B) |
| `tmp/_ct13.mad` | DEFINED not-deriving + ref | FAILS (bug B) |
| `tmp/_ct14.mad` | DEFINED std::char_traits + ref from fn in std (no spec) | FAILS (bug B, minimal) |
| `tmp/_tmpl.mad` | **char_traits-free** two-namespace same-named template + `a::W<int>::f()` | FAILS — THE root-cause repro |
| `tmp/_min.mad` | `namespace b{ struct S{ a::T*p; }; }` (struct-member qualified type) | now OK (bug A fixed) |
| `tmp/_glob.mad` | same at global scope | now OK |
| `tmp/ct_bisect.h` | editable copy of real bits/char_traits.h | for bisection |

Committed regression tests added this session: `tests/testarraytypedefstruct.mad`
(array typedef of tagged struct, `7 11`) and `tests/testqualifiedmembertype.mad`
(qualified member type, `7 7`).

---

## 8. Gotchas / hard-won learnings (do not relearn)

- **Naive `{`/`}` brace counting RUNS AWAY** on braces inside string/char literals
  or comments. The step-25 batch corrupted other functions' signatures this way
  (it ran one function's span to a wrong end and `pgm`→`*this`'d a swath,
  producing `Program &*this`). FIX = a string/char/comment-aware brace matcher
  (in the step-25 script). Always assert `"Program &*this" not in text` after a
  scripted transform, and `git checkout` to revert if it trips (the tree was
  always committed before each scripted batch — do that).
- **`find()` matching a forward decl** (identical first line to the def): remove
  fwd decls FIRST, then find the def.
- **Sig-replace must come BEFORE body xform** (else `pgm`→`*this` mangles the
  `Program &pgm` in the signature so the sig regex won't match). The step-19/20
  scripts got this order right; an early step-19 attempt got it wrong and only
  transformed the sig line (not the body) → leftover `pgm` → compile errors. Use
  the compiler as the oracle.
- **Default args:** C++ forbids them in both decl and def. When you turn a free
  fn into a method, move the default to the in-class decl and STRIP it from the
  out-of-class definition. If a default lived in a removed fwd decl, RE-ADD it to
  the new decl (else callers relying on it break with arity errors).
- **Parser-local types in method signatures:** if a converted method's signature
  uses a `parser.cpp`-local type (`CppSymKind`, `ParsedParamSig`), LIFT that type
  to `madc.h` (before `class Program`) so the in-class decl compiles.
- **The `__gnuc_va_list` / `va_start` coupling:** `va_list` must remain THE direct
  `struct __madc_va_list_tag[1]` typedef the intrinsic recognizes — add
  `__gnuc_va_list` as a SEPARATE direct typedef of the same struct; don't make
  `va_list` an alias-of-an-alias. And the array-typedef emitter bug (§4.1) must
  be fixed or the duplicate-tag emission breaks it.
- **The embedded-cdefs philosophy:** hardcode glibc's conditional OUTCOMES (empty
  attribute macros, etc.); do NOT replicate `__GNUC_PREREQ`/`__has_attribute`
  (those drive broad feature selection — adding them risks disabling needed
  types). The stub at `include/madc/sys/cdefs.h` follows this.
- **Line attribution across `#include`s is unreliable** — the parser reports the
  top file with an inner line number (e.g. the char_traits `473:4` was attributed
  oddly). Bisect/trace rather than trusting `file:line` across includes. But
  WITHIN a single file the per-token line is reliable.
- **`make` regenerates `embedded_headers.cpp`** from `include/madc/*` via
  `scripts/gen_embedded_headers.sh` — so editing `include/madc/sys/cdefs.h` or
  `include/madc/stdarg.h` then `make -C src` picks it up; the generated
  `src/embedded_headers.cpp` shows in `git status` and must be committed too.

---

## 9. Diagnostic tools & exact commands

```bash
# Build + full regression (MUST stay 522/4/0/26):
make -C src
make -C src fulltest

# Real-header burn-down (two-column parser-vs-preprocessor):
bash scripts/probe_real_headers.sh
bash scripts/probe_real_headers.sh wchar.h   # single (if it accepts args)

# Probe ONE header directly (madc own-preprocess):
printf '#include "/usr/include/c++/13/bits/char_traits.h"\nint main(){return 0;}\n' > tmp/_x.mad
bin/madc --std=c++17 --emit=c11 tmp/_x.mad 2>&1 >/dev/null | sed 's/\x1b\[[0-9;]*m//g' | grep -m1 'error:'

# Preprocessor-vs-parser classify (gcc preps, madc only parses):
g++ -std=c++17 -E HEADER 2>/dev/null | grep -v '^#' > tmp/_pp.cpp
bin/madc --std=c++17 --emit=c11 tmp/_pp.cpp 2>&1 >/dev/null | grep -m1 'error:'

# See madc's PREPROCESSED token stream (find unexpanded macros):
bin/madc --std=c++17 --dump-source tmp/_x.mad 2>/dev/null | sed -n 'START,ENDp'

# See what c2mir gets (the cir_node tree rendered as C):
bin/madc --std=c++17 --emit=c11 tmp/_x.mad 2>/dev/null | sed -n 'START,ENDp'

# Compare a macro's madc expansion vs gcc:
printf '__INT64_TYPE__\n' | g++ -std=c++17 -E -x c++ - 2>/dev/null | tail -1
```

---

## 10. Commit list this session (newest first, branch feature/realhdr-parse-gaps2-claude)

```
4f4fbd2 refactor(parser): one instantiate_template_id seam for the alias|class template-id probe
5ea75a2 fix(parser): namespace-scope template_map so same-named templates don't collide  [bug B]
e91858f docs(plan): comprehensive rehydration handoff (this doc)
c2126dc fix(parser): resolve namespace-qualified types as struct members          [bug A]
68dee85 feat(headers): flesh out embedded sys/cdefs.h with glibc attribute + inline macros
0665b86 fix(cir+headers): array typedef of a tagged struct must not re-emit the body; add __gnuc_va_list
e3c078c refactor(parser): re-home remaining free helpers as Program methods (restoration step 25)
e42fad6 …param-signature/qualified-declarator cluster (step 24)
b8fdeb8 …GNU/C23 attribute consumers (step 23)
d0f94f4 …class-body/deferred-body cluster (step 22)
ce6da8e …type-name resolution helpers (step 21)
63c2132 …type-token resolution quartet (step 20)
c0a85bc …template-machinery core 16 fns (step 19)
9485bf5 …template-machinery leaf consumers (step 18)
cac9492 …cast-parsing family (step 17)
6a4b037 …type-query cluster (step 16)
6407708 …static_assert trio (step 15)
f20c63e …namespace-resolution helpers (step 14)
4916e9d …old-style (K&R) parameter family (step 13)
b57af67 …extract the giant ttOperator arm from parseExpression (step 12)
86ee60b …extract the giant ttIdentifier arm (step 11)
78596fb …extract ttSymbol arm (step 10)
f43daa9 …extract ttDataType arm + ExprStep protocol (step 9)
e618106 …array-dimension classifier (step 8)
37eed02 …constant-expression evaluator (step 7)
```
(Earlier restoration steps 1–6 — the `DelimDepth`/angle-scanner work — and the
`#include_next` + earlier real-header commits are below these on the branch.)

---

## 11. Methodology / rules to honor (from AGENTS.md)

- **GCC and clang are BOTH canon.** Reduce a failing case to a minimal repro and
  compare madc to `gcc -S -fverbose-asm -O0` / `clang -S -O0` (or `g++ -E` for
  preprocessor) BEFORE forming a hypothesis. **Fix at the deepest layer**, no
  shims. **Think twice, code once.** **Understand what exists before assuming it
  doesn't** (search the 24k-line parser first).
- **`make -C src fulltest` after every change** — never leave the tree red beyond
  the 4 known reds. Commit before any scripted bulk transform so revert is clean.
- **No hard-coding specifics into general machinery**; enums/predicates over
  string compares. (The char_traits fix must be GENERAL namespace-scoping, not a
  `char_traits`/`std`/`__gnu_cxx` special-case.)
- **No special-casing specific C++ classes / no hardcoding what headers provide**
  — madc must map `#include` details to libc/libstdc++ like clang. (User's
  explicit constraint.)
- Commit early; feature branch off `develop`; `-claude`-owned; **do not push, do
  not promote to master.**
- Be honest about metrics and mistakes (e.g. the brace-matcher run-away was
  caught and reverted, not papered over).

---

## 12. Broader context / north star

This branch sits in the long-running **retire-std-hardcoding** + **real-header
parsing** arc toward C23/C++23 compliance, all through the one
`cir_node`/MC11-IR → c2mir → MIR backend. The parser restoration directly served
this: a clean, reusable, method-based parser is the foundation, and it already
paid off (the `<map>`/`<set>` parser blocker vanished). See `AGENTS.md`,
`docs/plans/madc-vision-and-invariants.md`, `docs/adr/0001-cir-c2mir-backend.md`,
and the memory index at
`/home/dev/.claude/projects/-workspace-madc/memory/MEMORY.md` (esp.
`project_parser_restoration`, `project_north_star_c23_cpp23`,
`feedback_correct_over_shortcuts`, `feedback_emitc_gcc_parity_oracle`).

`develop` is untouched by this session. The retire-std-hardcoding gate is
`scripts/check-no-std-hardcoding.sh` (wired into fulltest) — keep it green if you
touch std registration.

---

## 13. Next threads, in priority order

0. ~~`template_map` namespace-scoping (bug B)~~ — **DONE** `5ea75a2`/`4f4fbd2`.
1. **`'lconv' / 'abs' is not a declaration in '::'`** — the string + all 5
   streams headers now share the `'lconv'` blocker, and `algorithm` has the
   twin `'abs'`. Both are a global-namespace `using ::name;` of a libc symbol
   (lconv ← `<clocale>`, abs ← `<cstdlib>`) that madc doesn't have declared at
   `::` at that point. Likely ONE fix clears 7 headers. THE immediate-next task.
   Method: reduce `using ::lconv;` / `using ::abs;` to a minimal repro; check
   whether the libc symbol/type is declared globally in madc's view (probe the
   relevant embedded header / `<clocale>`,`<cstdlib>`), fix the deepest layer.
2. **`memory`/`vector` `690:6 Too many parameters`** — §6. One root cause, 2
   headers (a real parameter-count limit or a macro mis-expansion).
3. **`map`/`set` `36:9 Expecting type in class definition`** — §6 (preprocessor).
4. **`string_view` `768:45 iterator<>` template-arg resolution** — its own next
   step after the `lconv` family.
5. (Optional) restoration polish — §2.6 (dedup STRUCT/CLASS parse, operator<).
   Also: qualified template-id as a *declaration* type (`ns::Tmpl<int> v;`) —
   a pre-existing gap surfaced while testing bug B (see §5 note).

After the std:: header family is green under madc's own preprocessing, the next
arc is wiring real headers into the build (retire curated stubs) per
`docs/plans/` real-header-PCH notes.

---

END OF HANDOFF. On resume: read this, run §1 + §3 verify commands, then start
§13 thread #1 (the `template_map` namespace-scoping fix) using the §5 plan.
