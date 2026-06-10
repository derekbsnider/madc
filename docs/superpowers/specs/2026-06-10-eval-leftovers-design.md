# Eval leftovers on CIR — design

2026-06-10. Approved by the user in-session. Successor to
`docs/plans/2026-06-10-libmadc-eval-on-cir-plan.md` (increments 1 and 3
landed there; this covers everything that remains).

## Scope and order

Three packages, in this order. **AOT save/load is explicitly excluded**
(long-defer, per user — it is the separate AOT track, not eval).

1. **B — expression-DSL string compare** (`testmadcevalexprctx`): small,
   self-contained.
2. **A — scope capture at the user call site** (`testmadcevalscope` + the
   scope-access unit category): the design-sensitive piece.
3. **C — increment-2 unit categories** (the remaining non-AOT
   `doctest::skip()` cases in `tests/unit/test_libmadc_program.cpp`):
   large but divisible by category.

## Why these broke: asmjit era vs CIR era (context)

- **asmjit**: the engine registered the bare public names (`eval_int`, …),
  so the parser saw the eval callee at the *user's* call site and appended
  the scope-capture context there (`runtime_eval_scope_target` +
  `parseCallFunc`, parser.cpp:9783-9812). `addLiteral` produced `ddSTRING`
  (a real `std::string` constant), so the expression DSL's
  `user.name == "echo"` compared string values by inheritance from the
  old string special-case.
- **CIR**: `madc::eval_*` are ordinary namespace wrappers in `<ns_madc>`
  (the php::explode pattern — NO engine registration, by user direction).
  The wrapper body is the only place the old hook could fire, and it can
  only see the wrapper's own locals — wrappers structurally cannot
  capture caller scope. Literals are now `const char*` (g++ canon), so
  `char* == char*` is a pointer compare. Host callbacks and globals are
  now MIR-link-time concerns instead of raw pointers.

## Settled semantics (user decisions)

- **String-value comparison exists ONLY in the expression DSL**
  (`eval_expression*`). Full eval (`eval_unit`/`eval_int("return …;")`)
  compiles real madc source and keeps real-language semantics:
  `char* == char*` is a pointer compare, exactly as gcc/clang would
  compile it. Verified before deciding: no test in the repo expects
  char*-literal value compare in the real language (`teststringeq.mad`
  compares `std::string` objects through the class operator — unaffected;
  the only value-compare expectation is `testmadcevalexprctx`, i.e. the
  DSL).
  - Considered and deferred: re-allowing string-valued literals in the
    real language under `--std=madc` / a pragma. No test or use case
    wants it; if ever wanted, the `LanguageStd` enum is the gate.
- **All six relational operators** get DSL string-value semantics
  (`strcmp` covers them uniformly; matches the old `dtSTRING` behavior).
- **Scope capture is a read-only snapshot.** Locals are copied by value
  into the hidden context at the call site; the eval'd child cannot write
  back to caller locals. No `capture=true`-style parameter: write-back is
  moot for the DSL (its policy rejects assignments), and per-call control
  already exists structurally — plain form = implicit capture (iff the
  engine policy allows), `_ctx` form = explicit context only, engine
  policy = host-level kill switch (expression and full-eval gates remain
  independently controllable). A future write-back need would be an
  explicit new API, not a default-on flag.

## Package B — DSL string compare

**Where:** `internal_program_runtime_eval_expression` (madc_program.cpp),
a rewrite pass after `validate_expression_ast` succeeds and before
`build_expression_function`. Only this pipeline; full eval and the real
language never see it.

**What:** walk the parsed expression; every comparison node (`==`, `!=`,
`<`, `<=`, `>`, `>=`) whose operands are BOTH string-typed (`dtCHARptr` —
what context strings and string literals materialize as, via
`make_expression_context_literal` → `addLiteral`) is replaced with
`strcmp(a, b) OP 0`. Running after policy validation means the
machinery-emitted `strcmp` call needs no policy exception, and user code
still cannot call functions unless the policy allows.

**Mechanism:** the child calls libc `strcmp` directly (registered
internally, resolved through the existing dlsym import path) — `strcmp`
is natively extern-C, so no madc-side export is needed and no C++ type
information is at stake. No `__madc_expr_strcmp_runtime` shim.

**New error:** a mixed compare (`user.name == 5`) becomes a loud DSL
rejection ("cannot compare string and non-string values") via the
existing `fail_program_runtime` plumbing with line/column, instead of
silent pointer/int comparison.

## Package A — scope capture at the user call site

**Trigger:** at namespace-call binding (generalize the two existing
`auto_scope_context` trigger sites, parser.cpp:10538 and 13434, to the
qualified-name path): callee is namespace `madc`, display name in
`is_runtime_eval_scope_public_name` (parser.cpp:372), the relevant engine
scope-access gate is on, and the call is not already a `_ctx` variant.
Note: that list says `eval` but the `<ns_madc>` public is spelled
`eval_unit` — the list gains `eval_unit` (keeping `eval` is harmless; no
such public exists in the namespace).

**Transform** (the proven asmjit-era mechanism, re-keyed from bare names
to `madc::`-qualified public names): rebind the callee to the sibling
`_ctx` wrapper through the existing namespace-overload machinery
(`namespace_fn_overload_sets` / `find_namespace_function_overload`), then
let the existing `parseCallFunc` tail declare the hidden
`__madc_eval_scope_ctx_N` array local in the CALLER's scope and append
the `TokenScopeContext` (CIR lowering already landed in `8d73306`).

**Data flow:** caller locals → `collect_runtime_eval_scope_variables`
snapshot (by value) → CIR lowers `TokenScopeContext` to
`__madc_scope_set_*` calls populating the array → `madc::*_ctx`
(mangled-direct host C++ function) → `build_runtime_expression_context` →
`madc::value` object → child program (expression path: context-root
identifier resolution; full-eval path: typed child globals via
`install_runtime_eval_scope_globals`).

**Additions:**
- `<ns_madc>`: the five full-eval `_ctx` publics (`eval_unit_ctx`,
  `eval_bool_ctx`, `eval_int_ctx`, `eval_double_ctx`, `eval_string_ctx`
  — names mirror their non-ctx publics; overload shapes too, e.g.
  `eval_int_ctx(const char*, array&)`) as **declaration-only C++
  namespace functions resolved mangled-direct** (user direction,
  2026-06-10): no extern-C runtime declarations, no wrapper bodies. The
  implementations are real `namespace madc { … }` C++ functions in the
  host binary (parser.cpp, next to the `madc_runtime_eval_*` internals
  they call), exported via -rdynamic and resolved by
  `cir_import_resolver` through their Itanium symbols — the proven
  php:: declaration-only pattern. This keeps C++ type information
  (`array&`, overloads) instead of flattening through a C ABI.
- **String-class predicate** (new named helper, per
  `.claude/rules/helper-methods.md`): "is this DataDef the std::string
  class". Shared machinery — the collector uses it now; increment-2
  string call marshalling (`native_type_from_datadef`) uses it next.
  This is a flagged marshalling-boundary exception to the
  no-std-hardcoding rule, in the same sanctioned category as the mangler
  and the auto-include trigger map: the libmadc boundary marshals to
  `madc::value` kinds, and `kind::string` ↔ `std::string` is that
  boundary's job.
- **Migrate the existing `<ns_madc>` surface to the same shape** (user
  direction, 2026-06-10): the extern-C `__madc_*_runtime` exports are
  EXCLUSIVELY the C-linkage API for C programmers consuming
  libmadc.a/.so — never the script-side resolution path. The script
  header's extern-C declarations and `madc::` wrapper bodies are
  deleted; every `madc::` public (eval, expression, ctx, and
  context_set_* families) becomes declaration-only, resolved
  mangled-direct to real `namespace madc { … }` C++ implementations in
  the host. Per `.claude/rules/cpp-first-api.md`: the C++ layer is the
  one real implementation; the extern-C exports remain host-side as
  thin shims over it for C hosts (declared in the C API surface, not in
  the script header). Gates: the three green `testmadceval*` tests pin
  behavior through the migration.
- Collector: capture string-class locals/params/globals as
  `kind::string` (it captures bool/int/real/array today; string support
  left when `dtSTRING` retired).
- `__madc_scope_set_string_runtime(void *ctx, const char *key, void *str)`
  extern-C export (parser.cpp, next to the int/real/array setters at
  ~992-1008) taking `std::string*` — the host copies the value out, so
  the CIR lowering stays a plain pointer pass (no `c_str()` emission).
  Plus the matching string branch in the cir_builder TokenScopeContext
  lowering (cir_builder.cpp:7787-7806). These setters stay extern-C:
  they are compiler-machinery symbols emitted by the CIR builder (the
  `__madc_vla_free` category), not user-resolved namespace functions —
  the mangled-direct direction applies to the user-facing `madc::`
  surface.
- `char*` locals: NOT captured in this pass (no test needs them);
  trivially added later with a cstr setter if wanted.

**Error handling:** no compound scope → existing `Throw`
(parser.cpp:9803). Policy off → no transform; the call behaves exactly
as today. A failed `_ctx` overload rebind is a loud parse error, never a
silent fallback to the non-ctx call.

## Package C — increment-2 categories, dependency order

1. **`register_function` / engine callbacks** (~8 cases): the missing
   piece is host-pointer visibility at MIR link time —
   `MIR_load_external` on the `CirJitSession` context; the parser-side
   registration machinery survives from the asmjit era.
2. **`get/set_global`** (~3): `CirJitSession` resolves linked-module
   data-item addresses; reads/writes go through them. String globals
   ride the string-class predicate. Master's behavior is the spec.
3. **String call marshalling** (~5): extend `native_type_from_datadef` +
   `perform_call` with the string class, including sret/__retbuf shapes
   for string returns. Master's perform_call paths are the spec.
4. **fork-per-invocation + invoke_limits** (~8): fork/rlimit plumbing
   exists; the session must be built pre-fork; mostly
   unskip-verify-fix.
5. **Expression-policy extras + error-surface tail** (~4): host string
   bindings, math.h header groups, exec_file-missing-main message shape,
   child-policy restriction case.

## Testing and gates

- B: remove `tests/testmadcevalexprctx.mir_skip`; extend the `.mad` +
  `.expect` with an ordering compare (e.g. `user.name < "zzz"`); add a
  unit case for the mixed-compare rejection (host-error path).
- A: remove `tests/testmadcevalscope.mir_skip`; un-skip the scope-access
  unit cases (test_libmadc_program.cpp ~1400-1525).
- C: each category lands with its unit-category un-skip as the gate.
- Every landing: `make -C src fulltest` green + exit 0 (both check
  gates), FULL torture failset-diff vs `tmp/failset_lsq.txt` (zero
  regressions — package A touches parser call-binding, so this is
  load-bearing), SMAUG soak after parser-touching changes, every run
  capped (`( ulimit -t 120; timeout 180 … )`).
- Unit-suite debugging: the engine captures `std::cout` — use
  `bin/test_libmadc_program --out=FILE`; `make -C src` does not relink
  `bin/test_*` (only `make -C src test` does).

## Rejected alternatives

- **A2 — parser intrinsics** lowering `madc::eval_*` straight to the
  `_ctx_runtime` helpers: duplicates the wrappers' marshalling in the
  parser and breaks the one-pattern-per-namespace architecture.
- **A3 — wrapper-side capture**: structurally impossible; a wrapper body
  cannot see caller scope.
- **B in both pipelines** (value compare in full eval too): would fork
  eval'd source semantics away from gcc/clang canon.
- **`capture=` boolean parameter**: redundant with the `_ctx` variants +
  engine policy; classic bool-trap.
