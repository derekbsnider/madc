# Plan — Lazy member-function-body instantiation (conform to [temp.inst])

**Branch:** `feature/cpp-detection-idiom-claude` (WIP, off green tip `feature/header-partition-claude`).
**Date:** 2026-06-09. **Tier:** 1 (pure madc front-end sema; c2mir never sees source).
**Rule anchors:** #1 g++ is canon · #2 deepest layer · #4/#7 reuse existing, data-driven ·
#5 layer boundaries · no-parallel-implementations.

## 1. Problem (probe-confirmed, not theorized)

`std::string line; line.size();` throws `Unknown namespace 'pointer'` at
`basic_string.h:3486`. Root cause, established by probing (NOT the prior handoff's
"instantiate pointer_traits<char*>" theory, which 4 probes falsified):

- madc **eagerly parses every member-function BODY** when a class template is
  instantiated. `_M_local_data()`'s body does `std::pointer_traits<pointer>::pointer_to(...)`;
  during that eager body-parse `pointer` (a member typedef) isn't found → the throw.
- `size()` **never calls** `_M_local_data()`. Per **[temp.inst]**, implicitly
  instantiating a class instantiates member *declarations*; member-function
  *definitions* instantiate only on **odr-use**. **g++ does not instantiate
  `_M_local_data()`'s body here.** madc's eager body parse is a *conformance* bug.
- Proof the general mechanism is fine: `tmp/ptr_repro.mad`
  (`std::Tmpl<member-typedef>::staticmethod()` in an instantiated body) compiles
  in madc. The wall is *only* that we parse a body we must not parse yet.

The 119,740-line `-v` trace for that 2-line program is the eager-parse cost made visible.

## 2. Architecture facts (from the two Explore maps)

- **Eager site:** `parser.cpp:18213` — `parse_deferred_function_bodies(deferred_method_bodies)`
  at the tail of `TokenCLASS::parse`. Instantiation re-enters `TokenCLASS::parse`
  (re-injected tokens, `parser.cpp:2759-2760`), so every instantiation flushes every body.
- **Per-method lazy-parse primitive (reuse):** `DeferredFunctionBody{var, method,
  body_tokens, file, line, column}` (`madc.h:1242`), filled by
  `enqueue_deferred_function_body` (`parser.cpp:16761`), drained by
  `parse_deferred_function_body` / `..._bodies` (`parser.cpp:16776/16845`) which
  re-push tokens, `parseCompound`, build a `TokenFunc`, push to `pending_funcs`.
- **odr-use reachability engine (reuse):** `cir_builder.cpp:8789-8835`. `roots`
  (user code, incl. `main`) lowered unconditionally; `lib_funcs` (system-header
  bodies) lowered **only if their symbol ∈ `referenced_funcs`**, iterated to a
  fixpoint. `referenced_funcs` is populated by: method calls
  (`class_method_call`/`emit_symbol_method_call` → `referenced_funcs.insert(sym)`),
  ctor use (`class_ctor_call:4192`), dtor use (`3738/7007`), vtable slots
  (`class_vtable_def:2746`). The comment at 8798-8806 frames it as g++'s ODR model.
- **External instantiations need no body:** libstdc++ template instantiations get
  ctor/dtor bound to real `C1`/`D1` (`emit_symbol`, commit 6b5d4ea/38d9152), and
  whole-vtable skip when `is_externally_defined()` (`cir_builder.cpp:2610`). So for
  `std::string`, construction/destruction are external — the *only* bodies we ever
  need are inline non-virtual methods actually called (e.g. `size()`).
- **Emit symbol = `var.name`** (mangled `ClassName__method`); a body is emitted iff
  a `TokenFunc` with that name is in `pending_funcs`. This is the join key between
  the deferred-body map and `referenced_funcs`.
- **Type/layout stays eager:** member typedefs (`type_aliases`) and data-member
  layout (`compute_layout`) run *before* 18213 — deferring 18213 leaves them intact.

## 3. Design — extend the existing engine from lazy-LOWER to lazy-PARSE

**One sentence:** for *system-header template instantiations only*, don't parse
member-function bodies at 18213; keep them keyed by emit symbol; the existing
cir_builder reachability fixpoint parses a deferred body the moment its symbol is
odr-used (`referenced_funcs`), then lowers it — feeding the fixpoint transitively.

### 3a. Defer (parser side)
- New persistent map on `Program`:
  `std::map<std::string, DeferredFunctionBody> deferred_lazy_bodies;` (key = `var->name`).
- At `parser.cpp:18213`, gate: if **(this class is a template instantiation) AND
  (`ddc->from_system_header`)** → move each `DeferredFunctionBody` into
  `deferred_lazy_bodies[body.var->name]` instead of parsing it. Else → parse now
  (unchanged eager path). Gate predicate is data-driven (`from_system_header` +
  the existing instantiation-context signal `instantiating_canonical_spelling` /
  `parsing_template_instantiated_member_body()`), never a name test.
- New parser method `bool Program::parse_deferred_lazy_body(const std::string &sym)`:
  if `sym ∈ deferred_lazy_bodies` and not already materialized, call the existing
  `parse_deferred_function_body` on it (→ `TokenFunc` in `pending_funcs`), erase
  from the map, return true. Idempotent.

### 3b. Demand (cir_builder side, inside the existing fixpoint)
- In `translate_module`'s reachability fixpoint (`cir_builder.cpp:8822-8835`): when a
  symbol `s ∈ referenced_funcs` has no lowered/known `TokenFunc` but
  `prog->has_deferred_lazy_body(s)` → `prog->parse_deferred_lazy_body(s)`, take the
  resulting `TokenFunc`, `func_def` it, set `grew=true`. Its calls insert into
  `referenced_funcs` → next iteration materializes those. Pure extension of the loop.
- cir_builder *requests* parsing via a `prog->` method (parser owns parsing) — no
  layer inversion (cir_builder already depends on `prog`). At `translate_module`
  time the token stream is quiescent, so re-pushing body tokens + `parseCompound`
  is safe; verify empirically (probe #1 below).

### 3c. Scope = zero blast radius outside the gate
Non-system-header, non-template, and user-template bodies parse eagerly **exactly as
today** → existing C++ tests, gcc.c-torture, and SMAUG are structurally untouched
(C never sets `from_system_header` template instantiations with `<`-spellings).

## 4. Why not alternatives (rejected)
- *Fix `allocator_traits<>::pointer → char*` so the unused body parses* — fixes a
  symptom of a body that must not be instantiated (anti-#2). (Still needed later for
  LAYOUT of member types, but that is independent and not this wall.)
- *Parse-time-only demand via `reselect_method_overload`* — misses emit-time odr-uses
  (ctor/dtor/virtual/free-operator), risking the "missed completion-force" miscompile
  the handoff feared. The emit-time `referenced_funcs` set is authoritative/complete.
- *A new parse-time reachability walk* — duplicates the reachability engine
  (violates no-parallel-implementations).

## 5. Increments + gates (each gated before commit)
Standing gates: build clean (no new warnings); `fulltest` (known reds
testdefer/testfstream/testlargesizeofquery/testloop only); **gcc.c-torture run ALONE**
(`1566/31/57/1`); canaries `testcout` real-header + `test_extern_polymorphic` +
`tmp/fs_out.mad` (ofstream writes hello42 + clean exit). g++ is the oracle for values.

- **I0 (probe).** Confirm `parse_deferred_function_body` is safe to call at
  `translate_module` time (re-entrancy). Spike: defer one known-unused body, force a
  call to another, verify materialization. Decide 3b feasibility before coding it.
- **I1.** Add `deferred_lazy_bodies` + `parse_deferred_lazy_body` +
  `has_deferred_lazy_body` (parser). No behavior change yet (map stays empty unless
  the 18213 gate routes to it). Build + fulltest.
- **I2.** Wire the 18213 gate (defer for system-header template instantiations).
  Expect: `std::string line; line.size()` no longer parses `_M_local_data()`.
  Probe str1; then the cir_builder side will still need I3 to lower `size()`.
- **I3.** Wire the cir_builder fixpoint demand (3b). str1 should now compile + run
  (`return (int)line.size()` == 0 for an empty string). Gate hard (canaries +
  torture-alone — partial-spec/instantiation-touching ⇒ torture mandatory).
- **I4.** Re-run the real-header `<string>`/`getline` path → testfstream/testloop.
  Then land the chain (this + the detection idiom) onto the green tip once
  `testcout_realhdr` is green again. Then `<sstream>` (#23).

## 6. Risks
- **Re-entrant parse at emit time** (I0 gates this). If unsafe, fall back to a
  dedicated post-parse phase driven by a parse-time odr-use set (record symbols in
  `reselect_method_overload` + static-call sites) — but verify it covers ctor/dtor/
  virtual for any *non-external* deferred class (external ones need no body).
- **Missed completion-force** → incomplete-type/undefined-symbol at MIR-link, which
  fails LOUDLY (never silent). The `<`-spelling + `from_system_header` gate keeps the
  surface small and the failure mode loud.
- **A deferred body that IS needed but never referenced** ⇒ link error; caught by the
  canaries (ofstream exercises the real write path) + torture/SMAUG.

## 7. Anchors (verify with grep — lines drift)
- `parser.cpp`: eager flush 18213 · `enqueue_deferred_function_body` 16761 ·
  `parse_deferred_function_body` 16776 · `parse_deferred_function_bodies` 16845 ·
  `instantiating_canonical_spelling`/`parsing_template_instantiated_member_body` ~4921 ·
  `reselect_method_overload` 6054 · `request_template_instantiation_completion` 3000.
- `cir_builder.cpp`: reachability fixpoint 8789-8835 · `referenced_funcs` inserts at
  call/ctor/dtor/vtable (3262/4192/3738/7007/2746) · `func_def` 8101 · `translate_module` 8524.
- `madc.h`: `DeferredFunctionBody` 1242 · `pending_funcs` 1241 · `FuncDef` flags ~120-171.
- Reducers (tmp/): `str1.mad` (minimal trigger), `ptr_repro.mad` (general-mechanism OK),
  `at1.mad` (alloc-traits pointer — layout follow-up, NOT this wall). g++ oracle for values.

## 8. RESULTS — landed 2026-06-09 (commit `2173ae0`, WIP branch)

**DONE + committed.** I1+I2+I3 implemented; build clean; parse-at-emit-time
materialization verified sound (no re-entrancy issue). The `instantiating_canonical_spelling`
restore in `parse_deferred_function_body` was needed (without it a materialized body's
nested template-ids fall back to defaults).

**EFFECT (probed, not theorized):**
- **testcout (real `<iostream>`):** clean WIP (no lazy) fails at the SAME
  `_M_local_data` wall (`testcout.mad:3486:6 Unknown namespace 'pointer'`); WITH lazy
  it **advances past** that wall — testcout never odr-uses `_M_local_data`. New frontier
  below.
- **str1 (`std::string line; line.size()`):** still the `pointer` wall, **correctly** —
  `std::string`'s dtor genuinely odr-uses `_M_local_data` (dtor → `_M_dispose` → … →
  `_M_local_data`), so lazy defers it but it materializes on use. Needs frontier #2.

So lazy instantiation dissolves walls in *unused* inline bodies (real, conformant), but
not in *used* ones — those need the body to actually compile or to be externally bound.

### Frontier #1 — testcout: `import of undefined item allocator_int32_t___dtor`
The materialized `basic_ostream<char>`/`basic_istream<char>` dtors reference
`allocator<int>::~allocator()` (`int` is real here: `char_traits<char>::int_type == int`).
That symbol is **referenced but never defined** in the lazy path (it WAS defined on the
green tip via eager instantiation + the synth-dtor pass). Not in `deferred_lazy_bodies`
(probe-confirmed) → either (a) a late-instantiated `allocator<int>` whose trivial dtor the
synth-dtor pass (`cir_builder.cpp:9078`, gated `class_needs_dtor`) doesn't emit, while the
materialized ostream-dtor body still emits the *call* → call-vs-definition mismatch; or
(b) `allocator<int>::~allocator` is `declaration_only`. NEXT: instrument who references it
+ whether `allocator<int>` is in `struct_map`/`class_needs_dtor` at synth-dtor time; fix
the mismatch at the deepest layer (likely: don't emit a call to a trivial/absent dtor, OR
ensure the def is produced for a late instantiation).

### Frontier #2 — str1 (the headline): `extern template` external-binding
`basic_string<char>` is **non-polymorphic**, so `is_externally_defined()` returns false at
the `!has_vtable` gate (`parser.cpp:6316`) → madc emits its ctor/dtor → they reach
`_M_local_data` → wall. g++ uses libstdc++.so for `basic_string<char>` because `<string>`
declares **`extern template class basic_string<char>;`**. madc has **no `extern template`
handling** (grep confirms). THE FIX (data-driven, no name test): parse
`extern template class X<...>;`, mark that instantiation as externally-provided, and bind
its members (ctor→`C1`, dtor→`D1`, methods→mangled) to libstdc++ symbols (generalize the
6b5d4ea/38d9152 binding to non-polymorphic extern-template instantiations; the
`has_vtable` requirement in `is_externally_defined` is really "can we name its vtable" —
for a non-poly class there's no vtable to worry about, only ctor/dtor/method symbols).
CAUTION: do NOT blanket-bind all `from_system_header` non-poly instantiations — only
`extern template`-declared ones; libstdc++ does not export inline-only instantiations like
`vector<int>` (R2 note) → those must be madc-instantiated. With both lazy (done) +
extern-binding, `_M_local_data`'s deferred body is never materialized → str1 links against
libstdc++.so. This is the next major piece (its own focused session).

## 9. MILESTONE — landed 2026-06-09 (commits `2173ae0`, `11ac1bc`, `aef0366`)

**Real libstdc++ `<string>` AND `<iostream>` COMPILE + RUN end-to-end** (the str1 +
testcout cases), ALL GATES GREEN. Three landed pieces:
1. `2173ae0` — lazy member-function-body instantiation (frontiers #1/#2 were diagnosed here).
2. `11ac1bc` — extern-template external-binding (frontier #2 fixed): `extern template
   class basic_string<char>;` is captured + flagged `is_extern_template_instantiated`;
   the binding pass binds its ctor→C1/dtor→D1 even though it's non-polymorphic.
3. `aef0366` — Pass 1.9 re-runs the reachability fixpoint AFTER the synth-dtor passes
   (frontier #1 fixed): a deferred library dtor (allocator<int>::~allocator) reached only
   through a synthesized aggregate dtor is now materialized + DEFINED, not just referenced.

Verified: str1 `exit 0`; testcout `"This is a test, x = -1"` (= g++); fulltest **543/4**
(known reds only, testcout_realhdr GREEN — the 542/5 detection-idiom regression healed);
gcc.c-torture **1566/31/57/1** (UNCHANGED, C path untouched); canaries (ofstream hello42,
test_extern_polymorphic) correct.

### REMAINING (task #25) — richer string usage / getline
`tmp/str_use.mad` (`std::string s="hello"; s+=" world"; cout<<s<<s.size()`) still fails at
`basic_string.h:3486` — but now as **c2mir CHECK errors** (lvalue-required-as-&, incompatible
return-expr), NOT a parse error. `operator+=` is madc-emitted (only ctor/dtor bind external),
its body reaches `_M_local_data`, which returns `pointer` — and `basic_string::pointer` (via
`allocator_traits<allocator<char>>::pointer`) still resolves to the opaque `__detected_or_t`
STRUCT, not `char*` (§12.1). FIX = make that alloc-traits `pointer == char*` by selecting the
`allocator_traits<allocator<_Tp>>` partial spec (pointer=`_Tp*`). This is the member-TYPE
resolution (layout) that lazy-instantiation deliberately did NOT address.

**REFINED 2026-06-09 (probed `match_partial_specialization` for `allocator_traits`):** the
`allocator_traits<allocator<_Tp>>` partial spec **IS correctly selected** —
`best=1, _Tp=char, score=51`; `c80577c`'s `unify_nested_spec_pattern_arg` WORKS. So this is
NOT a partial-spec-matching bug. The struct-pointer is a DOWNSTREAM bug — most likely a
CACHE/ORDER issue: `instantiate_template_use` runs `match_partial_specialization` (~2653)
then a cache check (~2662) that returns a cached instance; if `allocator_traits<allocator<char>>`
was first instantiated via the PRIMARY (`__detected_or_t`) and cached before the partial spec
won, later refs (at1, `_M_local_data`) get the cached primary. NEXT PROBE: instantiation/cache
order for `allocator_traits<allocator<char>>` (registered_mangled key + is_complete), and dump
the selected partial-spec body's `pointer` member. (Or (b): the partial spec's `using
pointer = _Tp*` alias doesn't resolve to `char*` after subst — less likely.)

**CORRECTED 2026-06-09 (two theories REFUTED by probing — the real layer is narrower):**
- REFUTED #1 (alloc-traits pointer): `allocator_traits<allocator<char>>` selects its partial
  spec correctly (`best=1, _Tp=char`); every `allocator_traits` instantiation has a CONCRETE
  arg (probed all of them — no unreduced `_Char_alloc_type` rebind). So `pointer` is not a
  struct via that path.
- REFUTED #2 (`reinterpret_cast target is not a type`): that is a SEPARATE bare-`<memory>`
  closure parse issue (e.g. `align.h` `reinterpret_cast<uintptr_t>`); `<string>` does NOT hit
  it. Don't conflate.
- **ACTUAL remaining layer = `pointer_traits<char*>::pointer_to` not resolving.** str_use
  fails with c2mir CHECK errors at `_M_local_data` (`incompatible return-expr type in function
  returning a pointer` + `lvalue required as & operand`), NOT a parse error. `_M_local_data()`
  is `return std::pointer_traits<pointer>::pointer_to(*_M_local_buf);` (pointer=char*); the
  `pointer_traits<char*>` `_Tp*` partial spec / its `pointer_to(element_type&){return
  addressof(__r);}` isn't resolving → `pointer_to` yields a default int → "integer for pointer
  result", and the inner `&` hits a non-lvalue. **This is exactly the prior handoff's §12.8.**
  NEXT: probe `instantiate_template_id`/`match_partial_specialization` for `pointer_traits<char*>`
  (the FLAT `_Tp*` unifier should pick it) + whether it's instantiated in the `_M_local_data`
  body context; then `addressof`. g++ oracle: returns `char*`. Reducer `tmp/str_use.mad`.

  **CONFIRMED (PTPROBE in match_partial_specialization, run on str_use): `pointer_traits`
  NEVER reaches `match_partial_specialization` → `pointer_traits<char*>` is NEVER INSTANTIATED.**
  So the gap is upstream of partial-spec matching.
  **REFUTED (EAPROBE in the parseExpr ns-resolution arm ~11984): that arm is NOT the path that
  parses `std::pointer_traits<pointer>::pointer_to` in the failing body** (member_name never
  "pointer_traits" there). So the earlier "fix at ~11984" guess was WRONG. STILL OPEN (next
  session, verify by probe — do NOT assume):
    (a) WHICH method body produces the str_use c2mir errors? The `basic_string.h:3486` stamp is
        the class-close fallback for ALL synthesized/instantiated bodies, so it is NOT
        necessarily `_M_local_data` — instrument `parse_deferred_lazy_body` / func_def to print
        the symbol whose body fails to lower.
    (b) WHAT parse path handles `std::pointer_traits<pointer>::pointer_to(...)` in that body
        (it's not the 11984 ns-arm — maybe a type-context resolver / `resolve_typename_type_token`
        / a qualified-id path / a different parseExpr arm). Instrument to find where the
        template-id should be instantiated and isn't.
  Only after (a)+(b) form the fix. The body is reached via lazy materialization at emit time
  (`parse_deferred_function_body`, `instantiating_canonical_spelling` restored) — the re-parse
  context may itself be why the usual path isn't taken. HIGH blast radius if the fix lands in
  parseExpr/type resolution → gate torture-ALONE.

  ### BISECTION (2026-06-09, no rebuild — ran reduced variants on the milestone binary)
  `std::string` is really THREE distinct sub-issues; str1's success is the narrow path only:
  - **WORKS:** `std::string s; s.size()` (default ctor [external C1] + trivial size) — str1.
  - **FAIL (c2mir @3486):** mutators `s += "x"` / `s = "hi"` — madc-EMITTED body (operator+=/
    operator=/_M_replace/_M_construct) whose lowering produces "fn returning pointer returns
    integer" + non-lvalue `&` (the pointer_traits/_M_local_data family, body resolution).
  - **FAIL (RUNTIME crash, NOT compile):** `std::string s = "hello"` → COMPILES+runs, then
    `SIGSEGV in __libc_free` at `main [JIT]` freeing `0xff…f9` (wild ptr). This is **layout
    fidelity**: ctor-from-`const char*` is external C1; the dtor (external D1) frees because
    the SSO check (`_M_p == &_M_local_buf[0]`) fails — madc's `basic_string<char>` byte-layout/
    sizeof must match libstdc++ exactly for external C1/D1 to agree. (Default ctor works because
    empty SSO is robust; "hello" still fits SSO so the crash is a layout/offset mismatch, NOT a
    heap path.) DISTINCT from the @3486 body issue — own sub-task (cf. the ofstream vbase-layout
    fidelity concern). Probe: compare madc `sizeof(std::string)` + member offsets vs g++ (g++
    `__cxx11::basic_string<char>` = 32 bytes: _M_p@0, _M_string_length@8, union{_M_local_buf[16]
    / _M_allocated_capacity}@16).
  getline needs BOTH #2 (mutators) and #3 (layout) — so both gate testfstream/testloop.
</content>
</invoke>
