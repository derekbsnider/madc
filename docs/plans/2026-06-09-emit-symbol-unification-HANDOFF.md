# HANDOFF — emit_symbol unification + header-partition shim retirement (2026-06-09)

**READ THIS FIRST on resume / post-compaction. Assume you remember NOTHING.**
Run `bash scripts/resume.sh` (live git/build truth), then read this top to bottom.
This supersedes the older `docs/plans/2026-06-09-fstream-construction-HANDOFF.md` for
*current state/next-step* (that doc keeps the granular fstream/string history and the
turn-by-turn trail — still useful, but route from HERE).

---

## 0. ONE-PARAGRAPH STATE

Branch **`feature/cpp-detection-idiom-claude`**, HEAD **`0e6ba9a`**, working tree
**clean**. Earlier this session: (a) corrected the header-partition architecture I'd
misread (see §2), (b) landed a **data-driven embedded-header shim-bypass classifier**
(`65d2d67`), (c) landed **mangled-direct binding for named `std::` free-function
templates — `std::getline` works end-to-end** (`a1b4421`).

**STEPS 1 + 2 OF THE UNIFICATION ARE NOW DONE (the "resolver + gate FIRST" half):**
- **`80e75ea`** — Step 1a: merged `class_emit_name`+`nested_emit_name` → `local_emit_name`.
- **`d5d099e`** — Step 1b-i: added `CirBuilder::call_emit_symbol` (the ONE resolver,
  `emit_symbol ?: local_emit_name ?: var_emit_name`); routed `func_emit_name` (the
  half-baked path that ignored emit_symbol) through it.
- **`32df579`** — Step 1b-ii: routed EVERY call-symbol site (class_method_symbol,
  class_method_call, class_method_call_symbol, ctor_call_symbol, binary/unary operator,
  post-inc, value-of/fn-ptr-decay) through `call_emit_symbol`. `local_emit_name`'s value
  is now read in exactly ONE place.
- **`4517ab9`** — Step 2: `scripts/check-call-emit-symbol.sh` (wired into fulltest) FAILS
  on any raw `local_emit_name` value-read outside the resolver. Verified it catches an
  injected drift. THE anti-drift mechanism the user asked for.
All four commits behavior-preserving; gates UNCHANGED at each: fulltest **543/4/0/26**,
gcc.c-torture **1566/31/57/1**.

**STEP 3 STARTED — one wall cleared:**
- **`0e6ba9a`** — bind inline extern-template-class members external →
  **`cout << unsigned long` / `s.size()` now work** (member symbol `_ZNSolsEm`, g++ match;
  body no longer emitted). Zero regressions. See §4.1. This used the unified call_emit_symbol
  emit_symbol branch to route the call external.

**REMAINING Step 3 (the reds still need these — §4):** (a) `cout << std::string` — the free
`operator<<(ostream&, const string&)` must pass the class rhs as a const-REFERENCE (RK), not
a pointer (§4.2); (b) the free-`std::`-fn emit_symbol migration — place emit_symbol on free-fn
instantiations and RETIRE `try_std_free_function_call`/W2 call-site re-mangle + the `__ns_`
shim gate (§5.3 step 3 remainder); (c) then the per-red ingredients (ofstream good()/open(),
system(string), to_string, test rewrites to standard C++). The 4 reds
(testdefer/testfstream/testlargesizeofquery/testloop) are still red.

**The north star (user-confirmed, do not drift from it):** madc hand-rolls ONLY the
bucket-1/2 freestanding compiler headers; ALL glibc + ALL libstdc++ are consumed REAL
from the system. `std::` types/functions bind MANGLED-DIRECT to the real libstdc++
Itanium symbols via the ONE shared mangler — never via `__ns_` wrappers, never
hand-authored, never special-cased by name. See `[[project_header_partition_architecture]]`
and `[[project_cpp_mangled_direct]]`.

---

## 1. HOW TO REHYDRATE
1. `bash scripts/resume.sh` — branch/HEAD/build/runaway-process truth.
2. Read this doc fully, then `[[project_header_partition_architecture]]`,
   `[[project_cpp_mangled_direct]]`, `[[feedback_gated_debug_not_churn]]`,
   `[[feedback_verify_over_stale_handoff]]`.
3. `git log --oneline 110e026..HEAD` for this branch's commits;
   `git show a1b4421` for the getline mechanism; `65d2d67` for the classifier.
4. Cap EVERY run: `( ulimit -t 120; timeout 180 <cmd> )`, ONE heavy job at a time.
5. NAS mtime trap: `touch src/<f>.cpp` before `make`; verify the binary rebuilt
   (`stat -c %Y bin/madc` before/after) — mtime drift silently skips rebuilds.

---

## 2. ARCHITECTURE I MISREAD TWICE (do not repeat)

`madc-header-partition-handoff.md` is THE model. madc PROVIDES only bucket-1/2
(freestanding compiler headers: stddef/stdarg/limits/float/intrinsics + `#include_next`
shims). ALL glibc + ALL libstdc++ (`<string>`,`<iostream>`,`<fstream>`,…) are bucket-3,
consumed REAL/unmodified. **Do NOT hand-author embedded libstdc++ (the prior
"finish embedded <fstream> inc-5/inc-6" plan was the vendoring ANTI-GOAL — retracted).**
The hand-rolled SYSTEM-header shims in `include/madc/` (iostream/fstream/sstream/string/
vector/map/set/algorithm + glibc twins stdio.h/string.h/…) must be RETIRED, replaced by
consuming the real headers. `--no-embedded-headers` = bypass shims, parse real headers.

**Reality check (measured):** a full flip to real headers regresses 543→346 (the embedded
shims are load-bearing for ~197 tests). BUT most of those regressions collapse to a FEW
root-cause clusters (e.g. the `_GLIBCXX_BEGIN_NAMESPACE_VERSION` "failures" were an
ARTIFACT of running real libstdc++ in STD_MADC mode WITHOUT `--std=c++17` — STD_MADC
deliberately omits `__cplusplus`, lexer.cpp:1481). So retirement is INCREMENTAL, gated on
fixing real-header consumption per cluster. Chosen path: get the 4 reds green via real
headers in `--std=c++17 --no-embedded-headers` mode (the proven testcout_realhdr path),
fix the consumption bugs, defer physical shim deletion + the STD_MADC `__cplusplus` flip.

---

## 3. WHAT LANDED THIS SESSION (committed)

- **`65d2d67` — data-driven embedded-header partition classifier.** New policy
  `RegistrationPolicy.bypass_system_library_headers`; `--no-embedded-headers` now bypasses
  ONLY system-library shims (real glibc/libstdc++ twins) and KEEPS madc-own (`ns_*`/
  `__madc__`, no real twin) + freestanding (resolve in the compiler-owned dir) embedded.
  Classifier `Program::embedded_header_is_system_library_shim` (parser.cpp) +
  `madc_compiler_owned_include_dir` (gen_sys_includes.sh). Fixes the old all-or-nothing
  that wrongly dropped ns_php. Gate: fulltest 543/4 (default mode untouched).
- **`a1b4421` — named `std::` free-function templates bind MANGLED-DIRECT (std::getline).**
  See §6 for the full mechanism. VERIFIED: `std::getline(inf,line)` emits the EXACT g++
  symbol and reads lines end-to-end. Gates: fulltest 543/4, torture 1566/31/57/1.
- Doc syncs (`b63d54b`): corrected stale "std::string still crashes" claims (Codex's
  c932003+c9fd222 made string construction/mutation/`s[i]`/`a+b`/`.size()` all work).

---

## 4. THE 4 RED TESTS — what each still needs

All 3 stream tests should move to `--std=c++17 --no-embedded-headers` (real headers) + be
rewritten to standard C++ where they use removed dialect forms.

- **testloop**: ofstream/ifstream + `getline` (DONE) + `inf.good()` + `cout<<` + `system(string)`.
- **testfstream**: above + non-standard `to_string(s,42)` (2-arg), `strlen(string)`,
  `stoi` — rewrite to `std::to_string(42)`, `s.c_str()`, etc. (standard C++).
- **testdefer**: the `defer` madc feature + ofstream.
- **testlargesizeofquery**: SEPARATE track — uint32→64 array-dim truncation
  (`short buf[(1<<62)-256]`); needs widening member_dims/arr_dims/N_ARR; niche/risky/deferred.

**Shared blockers (real-header `cout<<` operand binding) — both block testloop/testfstream;
`cout` ITSELF works (testcout `cout<<const char*<<int<<endl` is green):**

1. **`cout << unsigned long` / `long long` / `s.size()` — ✅ FIXED (`0e6ba9a`).** Was: c2mir
   error at `<ostream>:470` "lvalue required as unary & operand" because `operator<<(int)` is
   DECLARED-ONLY (bound external `_ZNSolsEi`, worked) but `operator<<(unsigned long)` is INLINE
   (`<ostream>:172 {return _M_insert(__n);}`) → madc emitted the body, forwarding into the
   non-exported member template `_M_insert<T>`. FIX (landed): the post-parse CIR bind pass
   (cir_builder ~9460, runs once `is_extern_template_instantiated` is known — unlike parse
   time, so this canNOT go in `bind_declared_cpp_symbol`) now binds non-template, non-already-
   bound methods/operators of EXPLICITLY-INSTANTIATED classes (cls spelling has '<') external,
   mirroring its existing ctor/dtor binding. `cout<<(unsigned long)` → `_ZNSolsEm` (g++ match),
   body not emitted. VERIFIED vs g++: `cout<<5UL`→5, `cout<<"len="<<s.size()`→len=11.
2. **`cout << std::string` — ✅ FIXED (`bace903`).** Root cause was one layer deeper than
   the "pointer vs reference" framing: the W2 candidate filter required param[1] to EXACTLY
   match the rhs, so the free `operator<<(basic_ostream<_C,_T>&, const basic_string<_C,_T,_A>&)`
   (template-dependent param; `_Alloc` deducible only from the rhs) was never selected and
   madc fell back to the wrong MEMBER overload (`streambuf*`). FIX (landed): shared
   `deduce_param_against_class` (extracted from the getline path — one implementation)
   lets param[1] deduce against the rhs CLASS (reference param + reference return only);
   deduced rhs passes BY ADDRESS; `requalify_head` now PRESERVES leading cv so the symbol
   mangles **RK** not R. Emits g++'s exact symbol; chained `cout<<a<<" "<<b<<" "<<b.size()<<endl`
   works. Side discovery (verified PRE-EXISTING at clean HEAD via stash/rebuild):
   **`std::string a + b` SIGSEGVs in real-header mode** (garbage temporary, crash in
   free/printf) — the "a+b works (c9fd222)" claim is stale for this mode; separate track.

3. **Unqualified `getline(inf,line)`** (tests use `using namespace std`) resolves to the
   GLOBAL POSIX `getline(char**,size_t*,FILE*)` (3-param, from real `<cstdio>`) → "expected
   3 got 2". Needs an overload set across `::getline` + (via using-directive) `std::getline`,
   picking by arg types. Sidesteppable by qualifying `std::getline` in the rewritten tests;
   the unqualified lookup is its own gap.

---

## 5. THE NEXT WORK — emit_symbol unification (Pattern A) + drift-prevention gate

**User decision (via AskUserQuestion): unify ALL call-symbol derivation onto
`FuncDef::emit_symbol` (Pattern A everywhere). Exec order: resolver + gate FIRST, then
migrate. Two emit fields, not three.**

### 5.1 The drift (audited this session)
- ONE shared mangler exists: `itanium_mangle_*` (madc_mangle.cpp). No competing mangler. ✓
- **Pattern A (proper, "place on the node"):** class members get the mangled symbol placed
  on `FuncDef::emit_symbol` at parse via `bind_declared_cpp_symbol` (parser.cpp:17339, sets
  emit_symbol via itanium_mangle_ctor/dtor/operator/member_sub at 17363-17372). The call
  reads emit_symbol (cir_builder:817, 3222, class_method_symbol@823, class_method_call_symbol@3430).
  **Wired for CLASS MEMBERS ONLY.**
- **Pattern B (call-site re-mangle):** free `std::` operators (W2 `try_free_operator_call`,
  cir_builder:5047) and the new getline path (`try_std_free_function_call`) re-derive the
  symbol at the call site via `itanium_mangle_std_free_template`. NOT placed on emit_symbol.
- **THE HALF-BAKED PATH (user's exact concern):** `func_emit_name` (cir_builder:160) — the
  chooser for EVERY free/namespace call — does NOT read `emit_symbol`; it only checks
  `nested_emit_name`, then falls to `var_emit_name` (= `var.name` = `__ns_std_getline`). So
  even if emit_symbol is placed on a free fn, this path ignores it.

### 5.2 Two fields, not three (design decision)
The three `fd->*emit*` strings collapse to TWO concepts:
- `emit_symbol` = bind to an EXTERNAL ABI symbol, **madc emits NO body** (lots of code keys
  on `!emit_symbol.empty()` to skip body emission). KEEP as-is — distinct semantics.
- `class_emit_name` (arity-overload-disambiguated madc-emitted method symbol) and
  `nested_emit_name` (hoisted nested-fn/lambda symbol) are **the SAME concept** — "the
  symbol a madc-EMITTED function's body is defined-as and called-as" — never both set.
  **MERGE into one `local_emit_name`.**
(Do NOT also merge emit_symbol into one `emit_name`+is_external flag — its "no body"
semantics are load-bearing in too many places; two is the correct stopping point.)

### 5.3 Execution plan (order = A: resolver+gate first, then migrate)
**Step 1 — consolidate (behavior-preserving). ✅ DONE (`80e75ea`, `d5d099e`, `32df579`).**
- ✅ Merged `class_emit_name` + `nested_emit_name` → `local_emit_name` (FuncDef).
- ✅ Added the ONE canonical resolver `CirBuilder::call_emit_symbol`. NOTE: shipped as TWO
  overloads — a `static call_emit_symbol(FuncDef*, default_sym)` core (no instance state, so
  the static helpers `class_method_call_symbol`/`ctor_call_symbol`/`class_method_symbol`
  delegate to it) + a `(const Variable&, FuncDef*) const` form supplying `var_emit_name(v)`.
- ✅ Routed EVERY call-symbol site through it (func_emit_name, class_method_symbol@843,
  class_method_call@3231, class_method_call_symbol@3450, ctor_call_symbol@4181,
  binary op@5489, unary op@5612, post-inc@6926, value-of/fn-ptr-decay@6516). Behavior-
  preservation verified by the parser invariant `var.name==local_emit_name when set` (for
  class methods; nested fns keep source-name alias but func_emit_name reads local_emit_name
  first) + emit_symbol⊥local_emit_name. Gates UNCHANGED.

**Step 2 — drift-prevention gate. ✅ DONE (`4517ab9`).** `scripts/check-call-emit-symbol.sh`
(wired into fulltest). Enforces the achievable invariant: `local_emit_name`'s VALUE is read
ONLY inside `call_emit_symbol`; elsewhere only `.empty()` predicates / assignment LHS /
comments. (The originally-imagined "every N_CALL symbol must be call_emit_symbol-derived"
gate is impractical — legit raw-symbol N_CALLs exist: runtime helpers, intrinsics, the
emit_symbol-direct external branches. The local_emit_name single-reader rule is the tight,
low-false-positive proxy that actually catches hand-rolled symbol derivation.) Verified it
fails on an injected `return f->local_emit_name;` and is green on the unified tree.

**Step 3 — migrate free `std::` fns onto Pattern A: ⬅ IN PROGRESS (design verified
against the code 2026-06-09 late; the payoff that moves reds).**
Place `emit_symbol` on the instantiated free-fn FuncDef so the call reads emit_symbol via
`call_emit_symbol`; `try_std_free_function_call`'s bespoke N_CALL emission and the `__ns_`
prefix gate (cir_builder ~7357) are RETIRED. **Verified facts the design rests on:**
- Non-template namespace fns (php::) already ride the generic path: parser puts the
  Itanium symbol on `Variable::storage_alias_name` (parser.cpp ~27243 →
  `namespace_cpp_function_symbol`), consumed via `var_emit_name` (precedence 3).
  Template instantiations get `emit_symbol` (precedence 1) — per-call symbol.
- The generic arg loop (cir_builder ~993) ALREADY passes class ref-params by address
  (`param_object_class` → `object_arg_addr(arg, pc)`) and numeric refs via N_ADDR —
  IF the callee FuncDef carries correct `parameters` + `ref_params`.
- `call_target_funcdef` (static, cir_builder:565) is THE single resolver every consumer
  uses (arg emission @989, retbuf classification @587, callee naming @7352, fn-ptr decay,
  inline classifier) — the instantiation hook belongs THERE (memoized), so every consumer
  sees the instantiated FuncDef consistently.
- **GAP found: `object_arg_addr(arg, target)` (cir_builder:897) has NO derived→base
  adjustment** — when the arg's class != target it falls into the CONVERTING-CTOR temp
  path, which would construct a Base temp instead of upcasting for `Base&` params
  (wrong for getline(ifstream→istream&) and for USER classes too). Must add: walk the
  arg class's bases for `target`; if found at offset N, emit (void*)((char*)&arg + N).
  Generic C++ correctness fix at the deepest layer.

**Execution steps (gate each: build + getline/cout/string canaries + fulltest + torture):**
- **A (mechanical):** `call_target_funcdef` static → CirBuilder method wrapping the static
  core (callers are all CirBuilder methods). Behavior-preserving.
- **B (generic fix):** derived→base ref binding in `object_arg_addr` (before the
  converting-temp fallback).
- **C (the migration):** extract the overload-selection/deduction half of
  `try_std_free_function_call` into a shared `deduce_free_function_overload`; new
  `std_free_function_instantiation(tcf, cdf)` = deduce → mangle →
  build+memoize FuncDef {emit_symbol=sym, parameters=matched (base) class DataDefs,
  ref_params, returns/returns_ref (resolve return spelling against matched param
  classes, else builtin, else bail→NULL=fall through), declaration_only} +
  `need_output_extern(sym, native_func_shape(inst))` (mirrors class-member binds).
  Hook in `call_target_funcdef`: a namespace-fn FuncDef with captured
  `free_function_overloads` for (ns, display_name) → return the instantiation
  (memoized by TokenCallFunc*). Discovery = pure signature/data lookup — the `__ns_`
  PREFIX gate dies. Then DELETE try_std_free_function_call's emission + the gate.
  Check the generic call-result path derefs `returns_ref` externals (class-method
  sites do at 3240/3366; if the plain-call path lacks it, add it there).
- **D (later, separate):** the W2 OPERATOR path (try_free_operator_call) re-mangle —
  operators don't flow through TokenCallFunc/the generic call path; migrating them
  needs an operator-call analog and is NOT required to retire the named-fn machinery.
  Keep its deducer shared (deduce_param_against_class already is, since `bace903`).

**Gate each step: fulltest (known reds only) + gcc.c-torture ALONE + cout/getline/ofstream
canaries. HIGH blast radius (every call-symbol site) — torture every iteration.**

---

## 6. THE getline MECHANISM (DONE, `a1b4421`) — reference for the migration

Named `std::` free-function template → real Itanium symbol, no shims, generalizes the W2
operator path to named functions:
- **Parse capture** (`capture_free_function_overload`, parser.cpp; called in
  `register_skipped_namespace_template_function` BEFORE the single-Variable bail, so EVERY
  overload is captured; system-header-only via `is_system_header_path`): records each
  overload's return + param spellings + template params into `Program::free_function_overloads`
  (reuses `extract_free_signature`).
- **Call site** (`CirBuilder::try_std_free_function_call`, cir_builder.cpp; helpers
  `substitute_tparams`, `requalify_head`): select overload by arity → deduce template args
  by matching each template-id param against the call arg's class self/bases
  (`collect_self_and_base_spellings`) → **requalify each head with the matched class's
  FULLY-QUALIFIED spelling** (the captured spelling is unqualified since it lives inside
  `namespace std`; without this you get a bare `13basic_istream`/`12basic_string` that never
  links — must become `St13basic_istream`/`NSt7__cxx1112basic_string`) → `$Tn`-substitute
  template params (the mangler maps `$Tn`→`T_`/`T0_`/…) → `itanium_mangle_std_free_template`
  → emit N_CALL passing reference params BY ADDRESS, N_DEREF the result if the return is a ref.
- **Currently gated on `__ns_` callee prefix** (cir_builder ~7244) — that's a SHIM (user
  flagged it); it goes away in §5.3 step 3 when discovery becomes pure signature-match +
  emit_symbol. (I reverted an uncommitted attempt to just drop the gate — do it properly.)

---

## 7. PROCESS / GOTCHAS (cost me time this session — heed them)
- **`madc_verbose` is `thread_local`** → the repo's `DBG(x)` macro is SILENT on the
  parser/cir_builder worker threads. Don't trust empty DBG output there. Use the gated
  `FFDBG` macro (cir_builder.cpp, `#ifdef MADC_DBG_FREEFN`): build `make -C src
  CXXFLAGS=-DMADC_DBG_FREEFN`, run, read stderr; normal build = off. Gate diagnostics with
  `#if`, never add/remove churn (`[[feedback_gated_debug_not_churn]]`).
- **`--dump-cir` writes to STDERR** (not stdout) — capture with `2>&1`, not `2>/dev/null`.
  `--emit=c11` is a SEPARATE renderer (cir_emit_c.cpp), NOT the JIT/c2mir feed — for the JIT
  path use `--dump-cir`. (I chased a `--emit=c11` `__ns_std_getline` that wasn't the JIT symbol.)
- **NAS mtime drift** — `touch src/<f>.cpp` before `make`; verify `stat -c %Y bin/madc`
  before/after actually changed.
- **Tests run STD_MADC by default** (no `__cplusplus`); real libstdc++ needs `--std=c++17`.
  Forcing `--no-embedded-headers` WITHOUT `--std=c++17` produces artifact failures.
- **Gates:** fulltest 543/4/0/26 (reds: testdefer/testfstream/testlargesizeofquery/testloop);
  gcc.c-torture 1566/31/57/1 (run ALONE: `python3 scripts/run_gcc_testsuite.py --root
  gcc_testsuite --madc bin/madc`). Canaries: `tmp/gl_printf.mad` (getline reads lines),
  testcout_realhdr ("This is a test, x = -1"), test_extern_polymorphic.
- **Don't push remote without asking.** Commit early on this feature branch.

## 8. REDUCERS (tmp/, gitignored)
`tmp/gl_printf.mad` (getline works end-to-end), `tmp/gl_qual.mad`/`tmp/gl_only.mad`
(getline + cout walls), `tmp/cul.mad` (cout<<unsigned long), `tmp/cstr.mad` (cout<<string),
`tmp/loop_real.mad` (standard-C++ testloop shape). g++ oracle: `g++ -std=c++17 -c x.cpp; nm
x.o | grep U` for the real symbols.

## 9. OPEN ITEMS
- The `__ns_` shim gate (cir_builder ~7244) still in committed code — removed by §5.3 step 3.
- W2 + getline call-site re-mangle → retire into emit_symbol (§5.3 step 3).
- testlargesizeofquery: separate uint32→64 array-dim track.
- Physical shim deletion + STD_MADC `__cplusplus` flip: deferred (big, needs the cluster
  fixes first).

See `[[project_header_partition_architecture]]`, `[[project_cpp_mangled_direct]]`,
`[[project_template_instantiation]]`, `[[feedback_gated_debug_not_churn]]`,
`[[feedback_verify_over_stale_handoff]]`, `[[feedback_correct_over_shortcuts]]`.
