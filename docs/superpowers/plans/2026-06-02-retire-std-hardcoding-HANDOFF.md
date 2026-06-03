# RETIRE-STD-HARDCODING CAMPAIGN — FULL HANDOFF (rewritten 2026-06-03) — READ FIRST

> Self-contained resume doc for the "retire ALL std:: hardcoding" campaign. This is a
> SEPARATE track from the gcc-torture parity campaign (that lives on `develop`; see
> `2026-06-01-HANDOFF.md`). This campaign lives on its own feature branch and has NOT
> been merged to develop. Reading this doc + the spec + the memories below = full
> situational awareness; you should not need anything else to continue.

## ⏩ STEP 0 — ORIENT
```
bash scripts/resume.sh                              # live git/reflog/fork/build state
git -C /workspace/madc branch --show-current        # expect: feature/retire-std-hardcoding-claude
git -C /workspace/madc log --oneline develop..HEAD  # the campaign commits (live HEAD + count: resume.sh)
bash scripts/check-no-std-hardcoding.sh             # THE FINISH LINE — currently 468, target 0
```
Re-prove the keystone any time (fast, no full build needed if bin/madc is current):
```
bin/madc -v tmp/hooktest.mad   2>&1 | grep bind_std_libstdcpp   # inline std:: class → real symbols
bin/madc -v tmp/fstreamtest.mad 2>&1 | grep bind_std_libstdcpp  # #include <fstream> → real symbols
```

Authoritative spec: **`docs/superpowers/specs/2026-06-02-retire-std-hardcoding-design.md`** (read it).
Key memories: `project_string_as_class` (the Layer-1/2/3 truth), `feedback_correct_over_shortcuts`
(shortcuts categorically unacceptable), `project_cpp_mangled_direct`, MEMORY.md campaign line.

---

## 0. THE PRINCIPLE (never drift from this — the user enforced it HARD)

### 0.0 THE DEFINITIVE SPEC (user, 2026-06-03 — supersedes any softer wording below)
This is **architectural dependency removal, not a rename/refactor.** The goal is to make
**core madc independent of `std::string` as a required representation.** Any solution that
preserves `std::string` through an alias, typedef, wrapper, `std::basic_string<char>`,
implicit conversion, helper, adapter, template default, compatibility shim, or renamed
internal type is a **FAILED solution.** Hiding it from grep is not the goal.

The madc language/type system must NOT treat `std::string` as built-in, intrinsic, privileged,
or hardcoded. Any madc-level string-like type resolves through the **ordinary object/type
system, exactly like a user-defined class.** **These rules apply to ALL objects, not just
string** — any object can have operator overloads, copy-construction, etc.; the danger of
string special-casing is that it makes a feature look implemented when it is implemented ONLY
for string (PROVEN: `operator+` copy-init works for `std::string`, fails for a user class — see §6.0).

- **Allowed:** `std::string` in the COMPILER's own C++ (source text, diagnostics, file paths,
  maps, parser tokens, C++ codegen, temp buffers, host-language interop); and in tests for
  source snippets / expected diagnostics.
- **NOT allowed (core madc — lexer/parser/typechecker/codegen):** `BuiltinType::StdString` /
  `TypeKind::StdString`; hardcoded recognition of the madc type name `"std::string"`; special
  semantic rules that fire only because a type is std::string; parser/typechecker/codegen
  branches that privilege std::string as a madc language type; aliases/renamed constants that
  preserve it as a recognized intrinsic (this is what `is_std_string`/`is_string_class`/
  `DataDefSTRING` ARE — they must leave `src/`); fallback that maps unknown/unresolved
  string-like types to std::string.

**madc hardcodes ONLY the C/C++ primitive basis; every other type is COMPOSED from it.** The
language hardcodes a tiny set of fundamentals — `void`/`char`/`short`/`int`/`long`/`float`/
`double`/`bool` + the composition mechanisms (pointer/array/struct/union/enum/function) — and
**everything else — `std::string`, every stream, every container, every user type — is an
ordinary composed `DataDefCLASS`/`DataDefSTRUCT` built by parsing a declaration**, NOT a builtin.

**madc is a C++ front-end against real libstdc++, exactly like g++/clang++ + the linker.** A
symbol declared in a `#include` header *exists*; a use site is resolved at compile time to that
declaration, the symbol is **mangled from the declaration**, and the linker resolves it against
libstdc++. There is **no per-type code** — no "string handling," no "stream handling"; one
generic path: overload resolution → mangle the declaration → ABI from the declaration → emit call.

End state (all enforced by the finish-line gate):
- **No builtin DataDef instance / no `DataType` enum tag** for any non-primitive (`ddSTRING`,
  `DataDefSTRING`, `dt*STREAM`, `dd*STREAM`, `DataDef*STREAM` all GONE).
- **No hardcoded mangled `_Z…` literals** — `src/madc_mangle.cpp` is the single symbol source.
- **No wrappers/shims** (`string_*`, `streamout_*`, `streamin_*`, `*fstream_open/good/…`,
  `sstream_*`, `__std_*`).
- **No per-type lowering / registration callback** (`add_string_methods`, `add_fstream_methods`,
  the `SK_*`, `ostream_insert_symbol`, `STR_*` statics).
- **No type-identity PREDICATE in `src/`** — `is_std_string`, `is_std_string_ref`,
  `is_std_string_value`, `is_string_class` are renamed intrinsics; production code asks generic
  questions (`is_object()`, `class_needs_dtor()`, overload resolution) instead. (Permitted only
  under `tests/unit/`.) The gate counts these as of 2026-06-03.
- **madc contains NO reference to a real std:: type** — no `#include <string>`, no
  `sizeof(std::string)`. Layout is DERIVED from the parsed header; correctness is cross-checked
  **in a DOCTEST only** (the test `#include`s the real headers; madc does not).
- **The ONLY hardcoded std:: data is the auto-include symbol→header trigger map** (config table).

---

## 1. THE CONTRACT + THE GATE (done is machine-defined, never claimed)

`scripts/check-no-std-hardcoding.sh` greps `src/` + `include/` for every builtin DataDef / wrapper
/ tag / hardcoded literal / **tombstone comment** that names dead machinery ("gone without a trace
— we have git for a reason"). It is **wired into `make -C src fulltest`**, so the suite is RED
until the count is 0. The only permitted homes for std:: symbol knowledge are the mangler and the
auto-include map.

- **"DONE" = `check-no-std-hardcoding.sh` GREEN _and_ `make -C src fulltest` passes _and_ SMAUG
  soaks clean.** NEVER "it behaviorally works."
- **The count must only DROP, never rise** (even a WIP comment naming dead machinery trips it —
  that already happened once at 06adc04→reverted; reword generic, the specifics live in git).
- **LOOPHOLE CLOSED 2026-06-03 (`9db1eb0`): the gate previously counted only the builtin
  TAGS/wrappers, so the same dependence survived under ALTERNATIVE names (`is_std_string`,
  `is_string_class`, the `string_*` / `STR_*` families). The gate now counts those too
  (tests/unit/ exempt). The honest count JUMPED 468 → ~775 — that is not a regression, it is the
  truth the loophole hid. The 468 was a false floor; ~775 is the real worklist size.**
- **New baseline: ~775** (was a false 468). It only DROPS as the string/stream machinery is
  actually removed and each `is_std_string` site is generalized to `is_object()` /
  `class_needs_dtor()` / overload resolution. No faked intermediate drops.

WHY this is the only durable anti-drift design: any type-specific code OR type-tag can rot. The
gate makes "done" unfakeable; a future session cannot declare victory while the rug exists.

---

## 2. THE THREE LAYERS (why this "kept sneaking back" for a week)

Sessions kept declaring "done" at Layers 1–2 while Layer 3 — the *builtins* — was NEVER removed
(git-verified: `DataDefSTRING`/`add_string_methods` have no removal commit; ancient, on master too).
- **Layer 1 — the `dtSTRING`/`dtSTRINGref` TAGS + `tkSTRING`/`tkVECTOR`/`ns_stl.cpp` → RETIRED**
  (P2.14 on develop; grep 0). std::string recognized by class identity. REAL, done.
- **Layer 2 — hardcoded `_ZSt…` literals → SWEPT** (this campaign: mangler completed + the
  cir_builder sweep, `grep '"_Z' src/cir_builder.cpp` empty). REAL, done.
- **Layer 3 — builtin DataDef + callback registration + `sizeof(std::…)` + wrappers → THE
  REMAINING WORK** (this campaign). "std::string is a real class" meant Layers 1–2 (dispatch /
  identity), NOT header-defined. The memory `project_string_as_class` was corrected to say so.

---

## 3. LIVE STATE (verify with STEP 0)

- Branch **`feature/retire-std-hardcoding-claude`**, PUSHED (== origin), tracked tree clean. HEAD
  SHA and the commit-count are VOLATILE — every handoff commit bumps them, so they are NOT pinned
  here; read them live from `resume.sh` / STEP 0 and never trust a SHA written in this prose. Stable
  facts: develop = origin/develop = merge-base = **`110e026`**, UNTOUCHED → zero drift to develop
  (git-confirmed); MIR fork pin **`8864a73`** unchanged (this campaign needs NO fork work — it's
  pure madc front-end).
- **Finish-line gate: 468** (target 0; held flat all of 2026-06-03 — the string work ADDS the
  header-class mechanism, the count only drops at the migration's deletion step).
- **THIS BRANCH NOW CONTAINS MERGED FEATURE WORK** (all ff-merged in during 2026-06-03, which is
  why integration is 475 not the old 457): the full **Multiple/Virtual Inheritance** feature
  (S1–S5, see `project_multiple_inheritance`), **Virtual Destructors** (see
  `project_cpp_parser_correctness` + `2026-06-03-virtual-destructors-*`), and **C++ parser-correctness
  A/B/C** (const members/methods, &reference). These are proper-C++ features the campaign builds on;
  they are NOT std:: hardcoding.
- **`make -C src` builds clean, 0 warnings.** Integration `bash scripts/run_tests.sh` = **475
  passed**, 6 known feature-gap fails (testcin, testdefer, testforeach2, testfstream,
  testlargesizeofquery, testloop) + flaky `testfortypedcomma` (flips fail↔timeout — IGNORE).
  Baseline failset captured in `tmp/baseline_failset.txt` (regenerate after a clean build).
  `teststruct2` PASSES.
- **MIGRATION WORKLIST (read for the full string roadmap):**
  `docs/superpowers/plans/2026-06-03-string-migration-worklist.md` — the builtin std::string
  machinery inventoried into 8 role-clusters (A–H), 231 `ddSTRING` refs reconciled, a
  dependency-ordered migration sequence, and the precise mechanism-blocks. The detailed
  STRING-FIRST PROGRESS + NEXT are in §6 below (authoritative over this §3 commit trail).
- **`tmp/test_mangle` 44 cases / 143 assertions green** (every generated symbol vs `c++filt`).
- **SMAUG boots to ready** through the latest HEAD (pure C — never touches the hook; the param-
  spelling capture runs for its functions but only fills a vector, no behavior change).
- develop's mirrors (`claude_status.json`, `2026-06-01-HANDOFF.md`) reflect the PARITY track and
  remain correct for it — **do NOT edit them for this campaign until it merges to develop.**

### Commit trail (develop..HEAD, oldest→newest)
```
spec (d1e6ace) → mangler completeness W1: So/Si/Sd/Ss complete-spec (d03e38d), non-member std
  template ops + $Tn (71a2f48), std-vars + fn-ptr types (d9ad564) → THE SWEEP delete every _ZSt
  literal from cir_builder (5ec3072) → stream mangler doctests (9582beb) → THE GATE (529eea7) +
  wired into fulltest (1ead143) → delete ns_stl tombstones 473→469 (6f850ac) → INC 1
  FuncDef::declaration_only (9ad1c35) → INC 2 canonical_cpp_spelling facility (6740f67) → INC 3
  the keystone hook (8776a5e) → G2 array members in classes (ed97b24) → G1 namespaced template-arg
  (c45a822) → INC 4 STEP 1 include/madc/fstream (06adc04) → gate-fix reword comment (c773ac5)
  [+ docs(handoff) commits interleaved]
```

---

## 4. HOW THE KEYSTONE WORKS (the heart — the mechanism that lets the builtins be DELETED)

The generic path: **a bodyless method/ctor/dtor/operator of a `std::` class auto-binds
`FuncDef::emit_symbol` to its real libstdc++ Itanium symbol, generated by the mangler from the
parsed declaration (zero literals).** `emit_symbol` is already consumed by
`CirBuilder::class_method_call` → `emit_symbol_method_call` (src/cir_builder.cpp:2911/2977): madc
emits no body, the linker resolves the symbol against libstdc++. Pieces (all in src/parser.cpp
unless noted):

1. **`FuncDef::declaration_only`** (include/madc.h) — set true in `parseFunction`'s two bodyless
   return branches (search `declaration_only = true`, ~14597/14645). Marks "prototype, no body".
2. **`FuncDef::is_const_method`** (include/madc.h) — for a trailing-`const` method (`good() const`
   → Itanium `K`). Field exists, ctor-initialized false; the PARSER CAPTURE is not wired yet
   (see inc 5 — needed for good()/eof()).
3. **`canonical_cpp_spelling`** (the no-literals foundation):
   - `DataDef::canonical_cpp_spelling` (include/datadef.h) — a type's full Itanium-canonical
     spelling, e.g. `std::basic_ofstream<char,std::char_traits<char>>`. Empty = use `name`.
   - `TemplateDef::defining_namespace` (include/madc.h) — captured = `current_namespace` (e.g.
     "std") at TokenTEMPLATE::parse (~13684).
   - `instantiate_template_use` (~1541) builds `"<ns>::<tname><arg-spellings…>"` from each arg's
     OWN `canonical_cpp_spelling` (args are instantiated first, so they carry theirs — RECURSIVE:
     `char_traits<char>` → "std::char_traits<char>" → nested into the parent) and stashes it in
     `Program::instantiating_canonical_spelling` around the class re-parse (~1658).
   - `TokenCLASS::parse` copies the stash onto the new `DataDefCLASS` right after `new
     DataDefCLASS(...)` (search `instantiating_canonical_spelling`).
4. **`FuncDef::param_cpp_spellings`** (include/madc.h) — each param's canonical C++ spelling
   CAPTURED AT PARSE TIME in `parseFunction`'s param loop (leading-`const` + base spelling + `*`
   depth + `&`), index-aligned with `parameters` (hidden `__this`/`__retbuf`/`__va_args` = ""). WHY
   parse-time: madc drops top-level pointee-const on pointer params, so a DataDef-derived spelling
   would mangle `const char*` as `Pc` not `PKc` → wrong symbol. Reading the source tokens keeps it
   exact.
5. **`bind_std_libstdcpp_symbol(pgm, ddc, mvar, kind, mname, is_operator)`** (static helper just
   above `TokenCLASS::parse`, ~11386) — gated on `pgm.current_namespace=="std"` &&
   `fd->declaration_only` && `!ddc->canonical_cpp_spelling.empty()`. Collects param spellings (sans
   `__this`) and calls `itanium_mangle_{member,ctor,dtor,operator}_sub(...)`, sets `fd->emit_symbol`.
   Wired into the **dtor**, **ctor**, and **method** blocks of `TokenCLASS::parse` (search
   `bind_std_libstdcpp_symbol(`).
6. **G1** (c45a822, `resolve_declared_type_token` ~1705): parse a namespaced template-id as a
   template ARGUMENT — when an identifier is followed by `:: Name <` and `Name` is in
   `template_map`, strip the ns qualifier and instantiate by bare name. (Templates live in
   `template_map` by bare name; `namespace_datatype_map` holds only concrete types.) Needed for
   nested args like `basic_ofstream<char, std::char_traits<char>>`.
7. **G2** (ed97b24, `TokenCLASS::parse` data-member branch ~11843): parse fixed-size array members
   (`long _buf[64]`), ported from `TokenSTRUCT::parse`; no-op for scalar members.

PROVEN (tmp/hooktest.mad inline + tmp/fstreamtest.mad via `#include <fstream>`): a bodyless
`std::basic_ofstream<char,std::char_traits<char>>` binds ctor/dtor/close/is_open to the REAL
symbols `_ZNSt14basic_ofstreamIcSt11char_traitsIcEE{C1Ev,D1Ev,5closeEv,7is_openEv}`, byte-matching
the independent mangler output — zero literals.

---

## 5. WHAT'S DONE (each gated: build clean + 457 integration + SMAUG boots)

- **W1 — mangler complete** (`madc_mangle.cpp`, doctests `tests/unit/test_mangle.cpp` 44/143):
  So/Si/Sd/Ss complete-spec abbreviations, non-member std template ops (`operator<<`/`>>`/`getline`
  /`endl`) + `$Tn` params, std-vars (`_ZSt4cout`), function-pointer types (endl manipulator).
- **THE SWEEP** (5ec3072): every `_ZSt` literal removed from `cir_builder.cpp` (grep clean).
- **THE GATE** (529eea7 + 1ead143): committed, wired into `make fulltest`.
- **Tombstones** (6f850ac): `ns_stl` dead-name comments deleted; 473→469.
- **Inc 1** (9ad1c35) declaration_only · **Inc 2** (6740f67) canonical_cpp_spelling facility ·
  **Inc 3** (8776a5e) the hook — proven.
- **G2** (ed97b24) array members in classes · **G1** (c45a822) namespaced template-arg.
- **Inc 4 step 1** (06adc04): `include/madc/fstream` — `basic_ofstream`/`basic_ifstream` templates
  (+ `char_traits`) with bodyless ctor/dtor/close/is_open; binds via `#include <fstream>`. It is a
  NEW embedded header that COEXISTS with the builtins (builtins own the `ofstream`/`ifstream`
  typedefs; header owns the `basic_*` templates — no overlap). NOT yet wired to replace builtins.

---

## 6. WHAT'S NEXT (the substantial remaining work — count drops at inc 6)

### 6.0-DONE (2026-06-03 later) — GENERIC OPERATOR COPY-INIT now works for ALL classes
The generic machinery described in 6.0 below is now BUILT and verified (commits `1aeaa38`
operator typing + temp materialization + ref-param scoring; `88f69cd` implicit copy ctor).
`V c = a + b` for a user class works WITH a copy ctor (`tmp/userplus3.mad` → 7) and WITHOUT one
(`tmp/userplus.mad` → 7, trivial implicit copy); `teststringplus` stays green THROUGH the shared
generic typing + ctor selection (no string branch in the selector). Integration 475, SMAUG boots.
Pieces landed: (1) `TokenOperator::resolved_type` + `Program::resolve_object_operator_type` +
`DataDefCLASS::binary_operator_return_type` (operator expr typed by the operator's return type,
authoritative over the pointer/arith heuristics); (2) generic `select_ctor_overload` via
`score_arg_to_param` re-enabled; (3) `score_arg_to_param` ref-param peeling (`const T&` stored as
`T*` now binds a `T` object); (4) `class_operator_call` materializes a by-value object result into
a cleanup temp (trivial native-return / non-trivial `__retbuf`); (5) implicit copy ctor (trivial
struct copy) in `class_ctor_call`. **Still generic-incomplete:** implicit copy ctor for NON-trivial
classes (object members → member-wise copy-construct; they usually declare a copy ctor); unifying
string's operator CODEGEN (it still uses the `string_concat` emit_symbol branch — typing/selection
are already unified). **NEXT for the campaign:** migrate string onto this generic path and DELETE
the string special-cases — only now does the gate count drop.

### 6.0-MIGRATION SCOPING (2026-06-03, precise — verified by reading the code)
Root insight: **the string class DOES report `is_object()` true** (it is a `DDClass` →
`basetype()==btClass`; datadef.h ~189). So the `is_std_string` checks in `src/` are NOT "is this an
object" — they gate **string-SPECIFIC wrappers** (`string_obj_arg`/`string_construct`/`string_cstr`/
`string_concat`/`string_equals`) and a **return-representation contract**. THAT is why they cannot be
blindly flipped to `is_object()`: the generic object path and the string path disagree on how an
object rvalue is represented.

The specific blocker for operator-codegen unification (`class_operator_call`):
- **string operators take the `emit_symbol` branch** (cir_builder.cpp ~4284): comparison →
  `int string_XX(void* a, void* b)`; `+` → `string_concat(void* out, void* a, void* b)` (out-slot);
  `=`/`+=` → `string_XX(void* this, rhs)`. They return a **`void*` ADDRESS**.
- **user operators take the generic branch** (~4372) which (with Part B) returns an **OBJECT LVALUE**
  (`id(objtmp)`), not a void*.
- **Consumers of a string operator+ result assume the void* contract**: `string_obj_arg` (line ~972,
  `(void*)translate_expr(arg)` — NO `&`) and `translate_stream_chain` (~1761, cout<<). Routing string
  operator+ through the generic branch (object-lvalue return) WITHOUT updating these consumers
  produces `(void*)<struct value>` = garbage (this is exactly the `teststringplus` line-2 regression
  seen when the generic ctor selector was enabled before operator typing existed).

So unifying is a COORDINATED multi-consumer change, do it per-operator with full verification:
1. Make the generic branch use `emit_symbol` as the call symbol when set (so a routed string op uses
   `string_concat`/`string_equals`).
2. Decide ONE object-rvalue representation. Recommended: object LVALUE (the generic Part-B form);
   then update `string_obj_arg` (take `&` of the operator-result lvalue) and `translate_stream_chain`
   to address-of. Comparison operators are the SAFE first target (scalar `int` result — NO
   materialization/representation ripple; only the `!=`-negation needs carrying over).
3. Remove the corresponding block from the `emit_symbol` operator branch; verify `teststringplus` +
   full suite + SMAUG after EACH operator family; revert on any regression.
4. Then the bigger count items: replace the `is_std_string`→`string_*`-wrapper sites with generic
   object handling (`object_var_addr`, `class_member_construct/destruct`, generic conversion), delete
   `DataDefSTRING`/`ddSTRING`/`dt*` tags + `add_string_methods`, and the gate finally drops.

STATE 2026-06-03 turn end: generic foundation DONE+verified+pushed (HEAD `2afdbd4`); migration NOT
started (it is the coordinated ripple above — do not rush it; a regression to working string
functionality is the failure mode). `tmp/userops.mad`/`userplus*.mad` are the user-class reducers;
`tests/testuserops.mad` is the committed guard.

### 6.0 THE REAL BLOCKER (2026-06-03) — generic object machinery is INCOMPLETE; string special-casing hid it (SUPERSEDED by 6.0-DONE above; kept for the diagnosis)
**Removing the `is_std_string` sites is NOT a find-and-replace to `is_object()` — the generic
object path has HOLES that string's special-casing was papering over.** Proven this session
with minimal reducers (kept in `tmp/`: `overload_sel.mad`, `userplus*.mad`):

- `c = a + b` (assignment) on a user class V with `V operator+(V&)` **WORKS**.
- `V c = a + b` (copy-init) **FAILS** (`incompatible argument type for arithmetic type parameter`),
  **even with an explicit copy ctor.** The identical construct works for `std::string` — ONLY
  because string has `is_string_operator_plus` + `string_temp_decl` special cases.

Root causes (both generic, both must be fixed for ALL classes):
1. **Operator expressions on objects are not TYPED.** `TokenAdd::datadef()` (include/tokens.h:204)
   returns the pointer/arithmetic type — for two objects it falls to the default `int`, and for
   `obj + "lit"` it returns the RHS `const char*`. Operator-overload resolution only runs at
   CODEGEN (`class_operator_call`), never at the type level. So copy-init ctor selection sees the
   wrong arg type. **Groundwork landed (`9db1eb0`), NOT wired:**
   `DataDefCLASS::binary_operator_return_type()` + `Program::resolve_object_operator_type()`. To
   wire it, the arithmetic operators' `datadef()` must prefer the object-operator result type OVER
   the pointer-arithmetic short-circuit (add a `resolved_type` member to `TokenOperator`, returned
   first by `TokenAdd`/`Sub`/`Mul`/`Div`/`Mod::datadef()`), then call
   `resolve_object_operator_type` in `popOperator` (parser.cpp ~6790). A first attempt that only
   set `_datatype` regressed `teststringplus` because `TokenAdd::datadef()` ignores `_datatype`
   when an operand is a pointer — hence the `resolved_type`-first rework.
2. **No generic object-rvalue temp materialization for user operators.** `class_operator_call`'s
   user-class branch (cir_builder.cpp ~4372-4401) returns the raw call; a by-value object result is
   not materialized into an addressable cleanup temp, so it can't be copy-constructed from / have
   members called. The generic materializer already exists (`object_call_temp_addr`, ~1102) — wire
   the operator path through it (handle both the trivial native-struct return and the non-trivial
   `__retbuf` return ABIs).
3. **Implicit copy constructor** for classes with no user-declared one (trivial bit-copy /
   member-wise) so `T c = <T rvalue>` works without an explicit copy ctor.

**SEQUENCING CONSEQUENCE:** the generic `select_ctor_overload` (generic `score_arg_to_param`
ranking) CANNOT be deployed until (1) lands — without correct operand typing it mis-selects the
ctor for `T c = a + b` (verified: regressed `teststringplus`). So `select_ctor_overload` is
currently REVERTED to its string-aware form (cir_builder.cpp), with the limitation documented
inline. Order: **(1) operator typing → re-enable generic `select_ctor_overload` → (2) temp
materialization → (3) implicit copy ctor → then convert string's operators to ride the SAME
generic path and DELETE `is_string_operator_plus`/`string_concat`/etc.** Build the machinery for
ALL classes FIRST; string is then just a consumer with no privileged branch. DONE this session
(generic, regression-free, `9db1eb0`): generic `score_arg_to_param`, generic METHOD overload
resolution (`findMethodOverload`/`reselect_method_overload`/`method_display_name`),
`ctor_call_symbol`.

### STRING-FIRST REORDER (2026-06-03) — superseded by 6.0; the string surface below still applies once 6.0's machinery exists
The MI/virtual-base + virtual-destructor features are now DONE on develop-track branches and
merged into this campaign branch (HEAD has full MI S1-S5 + virtual dtors + parser-correctness
A/B/C). std::string is NOT a virtual-inheritance type, so it does NOT need the inc-5 vbase ABI —
it is the cleaner next target, and doing it first de-entangles the stream tests from string.
**So the order is now: std::string FIRST, then streams (inc 5/6), then cin/sstream/conversions.**

**std::string footprint (measured 2026-06-03):** 225 `ddSTRING` refs, 100 `string_*` wrappers, 53
`STR_*` statics, 11 `DataDefSTRING`, 8 `sizeof(std::string)`/`string_obj_words`. Multi-session;
migrate test-by-test keeping the suite green; gate drops only when the builtins are deleted.

**Canon (verified 2026-06-03):** `std::string` = `std::__cxx11::basic_string<char,
std::char_traits<char>, std::allocator<char>>`, sizeof 32 / align 8; methods are EXPORTED by
libstdc++ (weak/vague-linkage, e.g. `length()/c_str()/size()/append()/substr()` — `nm -DC`
confirmed), so string follows the mangled-direct keystone (unlike vector/map/set). Canon length
symbol: `_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE6lengthEv`. Stage A2 (trailing-const
method, merged) already wires `is_const_method` → the `_ZNK` prefix works now.

**PREREQUISITE discovered (the FIRST string sub-task):** a header-defined `std::string` must be
declared as `std::__cxx11::basic_string<...>` because the real symbol REQUIRES the `__cxx11`
component (`St7__cxx1112basic_string`) — it cannot be shortcut. But madc today:
- does NOT parse `inline namespace` (`grep -c "inline namespace" = 0`), and
- FAILS to resolve a doubly-qualified use site `std::__cxx11::basic_string<...>` →
  "Unknown namespace '__cxx11'" (parser.cpp ~10494/10528).
The mangler ALREADY has `__cxx11`/`basic_string` handling (9 refs in madc_mangle.cpp), so the gap
is PARSER nested/inline-namespace resolution, not the mangler. So sub-task 1 of string-first =
parse `inline namespace __cxx11` (members visible as `std::Name` AND mangled with the `__cxx11`
component) + resolve `std::__cxx11::Name<...>` at a use site. THEN author `include/madc/string`
(basic_string<char> binding mangled-direct, coexisting with the builtin like the fstream header),
prove the keystone symbol == canon, THEN migrate the 225 sites + delete the builtins.

**PROGRESS 2026-06-03 (string-first, all gated + pushed):**
- `467cd13` — three general parser prerequisites: (1) multi-level qualified-template
  resolution (`std::__cxx11::Name<...>` — skip the ident:: qualifier chain to the
  bare-name template); (2) typedef of a qualified/templated base type
  (`typedef __cxx11::basic_string<...> string;` resolves via the unified resolver);
  (3) `consume_template_close` — match `<`/`>` as a balanced delimiter pair so the
  lexer's single `>>` (TokenBSR) splits into this level's `>` + a pushed-back `>` for
  the enclosing one. Isolated to template-arg parsing; expression `>>` unaffected.
- `9eda32c` — **`include/madc/string` authored**: `std::__cxx11::basic_string<char,...>`
  with the real layout (char* + size_type + 16-byte SSO buffer = 32) + a core bodyless
  surface (ctor/dtor, length/size/c_str/empty const, clear, at, append). All NINE bind
  via the keystone to the EXACT libstdc++ symbols (verified vs `nm -D`: const→`_ZNK`,
  size_type param→`m`, `const char*`→`PKc`). NO `string` typedef → coexists with the
  builtin. **Bind gate GENERALIZED**: now keyed on the instantiation
  `canonical_cpp_spelling` starting with `std::` (the template's defining namespace),
  NOT the instantiation-SITE namespace — so a std type binds wherever instantiated;
  bodyless-only (declaration_only), so vector/map/set bodies untouched.
- **Found + tracked (KG `shift_operator_associativity`, deferred):** `1<<4>>1` yields 4
  not 8 — the shunting-yard forces left-assoc same-precedence pop only for prec 3/4,
  not 5 (shifts). Adding 5 fixes it BUT breaks `cout<<` chains (stream lowering depends
  on the right-leaning shape) — verified. MUST be fixed together with the stream-chain
  generalization (inc 5/6), not alone. Pre-existing (confirmed on clean HEAD).
- **Known limitation (not yet needed):** a fully-qualified STATEMENT declaration
  `std::__cxx11::basic_string<...> s;` still uses single-level resolution at
  `parseStatement` ~17570 (only the typedef + type-resolver paths got multi-level). Real
  usage goes through the `string` typedef, so this is unblocking only if a use site
  writes the fully-qualified template directly.

- `98daf8b` — **string member surface extended** (subagent, coordinator-verified + pushed):
  +13 member methods, all binding to canon (data, capacity, find(char*), rfind, compare,
  substr, resize(ulong), reserve, push_back, pop_back, operator[], operator=(char*),
  operator+=(char*)). 22 bodyless methods now bind. Gate 468, integration 475, SMAUG clean.
- **Found + tracked (KG `overload_by_param_type`, the NEXT inline step):** member/ctor
  overloads that share name+arity but differ by param TYPE collide on the mangled key
  (`Class__find`, `Class__Class`) — the 2nd overload re-binds the 1st's params → wrong
  symbol. Blocked 6 string overloads (find(char), resize(ulong,char), the char*/{ulong,char}
  ctors, operator=(char), operator+=(char)). Needed broadly for the migration. FIX = encode
  the param-type signature into the mangled key; INLINE parser work (not delegatable).

- `0314c3d` — **`overload_by_param_type` REGISTRATION fixed** (KG gap done): `unique_overload_symbol`
  gives a same-name/same-arity/type-differing member or ctor overload a fresh `Class__name__oN`
  internal key (recorded in `class_emit_name`); first/single overloads unchanged. The 6 deferred
  string overloads re-added and all bind to exact canon (find char/char\*, two ctors, operator=
  char/char\*, operator+= char/char\*, resize overloads). 22→28 string methods bind. Gate 468,
  integration 475 zero-regr, SMAUG clean. **Revealed a companion gap (KG `overload_selection_by_arg_type`,
  open):** call sites still select overloads by ARITY only, so a same-arity type-differing call
  picks the wrong one (`Box b("hi")` ran the int ctor vs g++). Registration now produces the
  distinct FuncDefs; argument-type-based call-site selection is the missing piece.

**NEXT (string-first continues, in order):**
1. **Call-site overload SELECTION by arg type** (KG `overload_selection_by_arg_type`) — rank
   same-arity candidates in `select_ctor_overload` + method dispatch by param-type match. Needed
   for real string calls in the migration + correct user-class overloading. INLINE.
2. **Non-member operators** `operator+` / `==` / `<<` (free functions, `_ZSt…`) — need W2
   (non-member operator overload resolution) + a non-member bind path. INLINE. (1 and 2 are both
   call-site overload-resolution work and can go together.)
3. **MIGRATION** — switch the ~225 ddSTRING sites to the header type and DELETE the
   builtins/wrappers/statics (gate count finally DROPS here). Test-by-test, INLINE.
4. Streams (inc 5/6, which also lands the shift-assoc fix), then cin/sstream/conversions → gate 0.

### (original ordering, retained for reference)

### Inc 5 — the virtual-base ABI (the one genuinely-new class-model feature; highest risk)
The header needs the real hierarchy so `<<` (basic_ostream) and `good()`/`eof()` (basic_ios)
resolve; `basic_ios` is a VIRTUAL base whose subobject is NOT at offset 0:
- **Author the bases** in `include/madc/fstream` (or a `bits/` header it includes): `ios_base`,
  `basic_ios<CharT,Traits> : ios_base`, `basic_ostream<…> : virtual basic_ios`, `basic_istream<…>
  : virtual basic_ios`, then `basic_ofstream : basic_ostream` (+ filebuf member), `basic_ifstream :
  basic_istream`. Layout must make madc compute the real sizes (ofstream 512 = ostream 272 +
  filebuf 240; see §7).
- **Add a per-class base-subobject byte offset to `DataDefCLASS`** (datadef.h:699/717 — today single
  base assumed @ offset 0; the base-member copy is `TokenCLASS::parse` ~11540-11571). DERIVE the
  offset from the header-declared layout (NEVER hardcode 248/256; if not cleanly derivable, STOP +
  escalate). Cross-check the computed offset in a DOCTEST that `#include`s the real `<fstream>`.
- **Offset-aware `this`-adjust** in `class_method_call`: when the resolved method belongs to a base
  whose subobject offset is non-zero, emit `sym((char*)&obj + offset, …)`. Offset 0 → byte-identical
  to today (`<<`/open/close/is_open unaffected).
- **Wire `is_const_method` capture** in parseFunction/TokenCLASS::parse (consume a trailing `const`
  after the param `)` — today it would error; see §4 item 2) so `good()/eof()` mangle with `K`
  (`_ZNKSt9basic_iosIcSt11char_traitsIcEE{4goodEv,3eofEv}`).
- **Add `open(const char*, openmode)`**: declare the `_Ios_Openmode` enum in the header so the
  param spelling is `std::_Ios_Openmode` → `…4openEPKcSt13_Ios_Openmode` (the param-spelling path
  already produces this; verified target in `tests/unit/test_mangle.cpp`).

### Inc 6 — switch registration + DELETE the builtins (THE GATE COUNT DROPS HERE)
- Add the `ofstream`/`ifstream`/`fstream` typedefs in the header (now they can replace the builtins).
- DELETE: `DataDefIFSTREAM/OFSTREAM/FSTREAM/ISTREAM/OSTREAM/SSTREAM` + the `dt*STREAM`/`dd*STREAM`
  enum tags + their parser/cir_builder branches; `add_fstream_methods` (parser.cpp ~5124) + its call
  (~5613); the `std_types["ofstream"]=make_namespace_type_token(...,ddOFSTREAM)` etc. (~5407-5408);
  the `ifstream_*`/`ofstream_*`/`fstream_*` externs + wrappers (madc_mir_backend.cpp + delete
  `madc_stream_runtime.cpp`, drop it from src/Makefile); `sizeof(std::ofstream)` etc. in datadef.h.
- Generalize `translate_stream_chain`/`stream_ident_kind` (cir_builder.cpp ~1609/1702) from
  "named cout/cin/cerr/clog" to "any object whose type is (derived from) ostream/istream", so
  `outf << x` routes through the mangled operators. `getline(inf,line)` → the mangled `std::getline`
  (already in the W1 mangler). Build with `-Wall`; `-Wunused-function` confirms the cut is complete.
- `testfstream`/`testloop` must PASS via the header path. Gate count drops by the stream block.

### Inc 7 — the rest, to gate=0
- cin `>>` (`testcin`). Then **std::string** (the big one — `ddSTRING` is woven through ~239 sites;
  migrate test-by-test keeping 457 green; delete `add_string_methods` + `ddSTRING` + the `STR_*`
  statics + `string_*` wrappers). Then `stringstream` + conversions (`to_string`/`stoi`, the
  `__std_*` wrappers). Final grep-gate → 0. Codify a `.claude/rules/` rule: std:: symbols are
  mangler-generated, never hardcoded; only the auto-include map is hardcoded std:: data.

---

## 7. STREAM LAYOUT FACTS (g++ probe `tmp/streamprobe.cpp`, this libstdc++ — ground truth)
```
sizeof: ofstream 512, ifstream 520, fstream 528, ostream 272, istream 280,
        basic_ios<char> 264, ios_base 216, filebuf 240
base-subobject offsets WITHIN the derived object:
  ofstream→ostream = 0 ; ofstream→basic_ios = ofstream→ios_base = 248
  ifstream→istream = 0 ; ifstream→basic_ios = 256
```
`basic_ios` is a VIRTUAL base of basic_ostream/basic_istream → its subobject sits at the END
(248/256), NOT offset 0, and DIFFERS per stream class. `<<`/`>>`/open/close/is_open use the
most-derived / ostream@0 `this`; only good()/eof() (basic_ios) need the +offset. Symbols exported
by libstdc++ (confirmed `nm -DC`): open/close/is_open are real exports; good/eof are weak
vague-linkage exports — so the +offset calls WILL link.

---

## 8. RULES OF ENGAGEMENT / METHODOLOGY (the user enforces these)
- **SHORTCUTS ARE CATEGORICALLY UNACCEPTABLE.** RED-FLAG TELLS = about to hardcode a literal / add
  a wrapper-shim / special-case higher up / think "good enough for now" → STOP, fix the deepest
  layer. The hardcoded `_ZSt` literals were exactly this and cost DAYS. ([[feedback_correct_over_shortcuts]])
- **gcc/g++/clang IS canon** — verify every symbol/layout vs `c++filt` / `nm -D libstdc++` / a g++
  probe BEFORE asserting.
- **"WAIT" MEANS PAUSE AND TALK — never revert / `git checkout` over uncommitted work.**
- **The USER prefers doing the delicate keystone work inline (not delegated).** A subagent was used
  once for inc-3 groundwork and the user pulled it back ("I thought you were doing it"); the agent's
  param-capture was reviewed/kept, the rest done by hand. Default: drive it yourself; verify
  objectively (the gate + doctest symbols + SMAUG make verification mechanical).
- **GATE EVERY CHANGE:** build clean (0 warnings) → `bash scripts/run_tests.sh` stays 457 (diff the
  FAIL list; only the known 6 + flaky) → COORDINATOR re-runs the SMAUG soak himself for any
  parser/codegen change → commit → push → keep the count monotonically ↓. NEVER relay a claimed
  result; verify it.
- **Commit messages: avoid embedded `"..."` quotes** (one commit silently failed from that —
  use plain prose). Keep them factual + the verification evidence.

---

## 9. VERIFICATION COMMANDS (cap heavy runs; ONE heavy job at a time)
```
( ulimit -t 400; timeout 500 make -C src 2>&1 | grep -icE "warning:|error:" )   # 0 = clean
bash scripts/check-no-std-hardcoding.sh                                          # the finish line
( ulimit -t 600; timeout 700 bash scripts/run_tests.sh > tmp/gate.log 2>&1 ); grep "passed" tmp/gate.log
# mangler doctests:
( ulimit -t 60; g++ -std=c++11 -Iinclude tests/unit/test_mangle.cpp obj/madc_mangle.o -o tmp/test_mangle ) && ./tmp/test_mangle
# keystone proof:
bin/madc -v tmp/hooktest.mad 2>&1 | grep bind_std_libstdcpp
bin/madc -v tmp/fstreamtest.mad 2>&1 | grep bind_std_libstdcpp
# SMAUG soak (parser/codegen changes) — exit 124 = survived = good, grep the literal ready line:
cd /workspace/MadSMAUG/runtime/area; timeout 50 /workspace/madc/bin/madc /workspace/MadSMAUG/src/SMAUG.mad 40NN > /workspace/madc/tmp/smaug.log 2>&1; echo $?
grep -c "Realms of Despair ready at" /workspace/madc/tmp/smaug.log ; pkill -9 -f 'bin/madc'
```
(`sleep` is blocked in the harness; `pkill` returns 1 when nothing matches — harmless. `setrlimit
RLIMIT_CPU: Operation not permitted` from a madc child under `ulimit -t` is harmless.)

Scratch lives in `tmp/` (gitignored): `hooktest.mad` (inline keystone proof), `fstreamtest.mad`
(header path proof), `streamprobe.cpp` (layout probe), `test_mangle`/`expect` (mangler oracle).

---

## 10. NO-DRIFT CHECKLIST (state was left consistent)
- All work committed + pushed on the feature branch; tracked tree clean (live HEAD via `resume.sh`).
- develop UNTOUCHED at `110e026` (git-confirmed `HEAD..develop` = 0); develop mirrors are correct
  for the parity track — do NOT edit them for this campaign until it merges.
- Memory corrected: `project_string_as_class` (Layer 1+2 done, Layer 3 NOT) + MEMORY.md line +
  `project_cpp_mangled_direct`. `feedback_correct_over_shortcuts` strengthened.
- The gate is committed + wired into fulltest → "done" is unfakeable + the count can't silently rise.
- No half-done code: every commit builds + passes the gate + SMAUG; inc 4 step 1's header coexists
  with the builtins (no conflict); inc 5 (the vbase) is PLANNED but NOT started.
