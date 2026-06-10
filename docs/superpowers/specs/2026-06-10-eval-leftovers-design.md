# Eval leftovers on CIR — design

2026-06-10. Approved by the user in-session. Successor to
`docs/plans/2026-06-10-libmadc-eval-on-cir-plan.md` (increments 1 and 3
landed there; this covers everything that remains).

## Scope and order

Four packages, in this order. **AOT save/load is explicitly excluded**
(long-defer, per user — it is the separate AOT track, not eval).

1. **B — expression-DSL string compare** (`testmadcevalexprctx`): small,
   self-contained.
2. **A0 — value unification** (user decision 2026-06-10): `MadValue` and
   `MadArray` die; the one host type is the public `madc::value`;
   script-side `madc::array` becomes an alias of `madc::value`. This is
   the prerequisite that unblocks full mangled-direct resolution for the
   array-taking `madc::` publics.
3. **A — scope capture at the user call site** (`testmadcevalscope` + the
   scope-access unit category) + the full ns_madc mangled-direct
   migration: the design-sensitive piece.
4. **C — increment-2 unit categories** (the remaining non-AOT
   `doctest::skip()` cases in `tests/unit/test_libmadc_program.cpp`):
   large but divisible by category. Gets its own plan once B/A0/A land.

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

## Package A0 — value unification (`MadValue`/`MadArray` → `madc::value`)

**User decision (2026-06-10):** the internal `MadValue`/`MadArray` pair
(datadef.h, asmjit-era php:: machinery) and the public `madc::value`
(include/libmadc/value.h, phase 4.2 of the embedding API) are parallel
implementations of the same concept — the value.h header's own
"deliberately separate, do not mix" comment marks the debt. They unify
onto the public type. `ddVALUE` does not exist; `MadValueKind` dies with
`MadValue`.

**Decisive inventory (verified 2026-06-10):**
- madcdat's ~284 `madc::value` references are all object-kind rows; it
  touches `kind::array` exactly once → array-representation freedom.
- No ns_* helper ever READS `MadArray.assoc` (only `.clear()` calls);
  every array helper operates on the indexed `data` vector. The sole
  real assoc consumer is `build_runtime_expression_context`
  (parser.cpp:206-214) — the MadArray→value conversion layer itself.
- Therefore: PHP-hybrid arrays are unexercised; the keyed/indexed split
  maps exactly onto `kind::object`/`kind::array`; and the
  insertion-ordering concern has zero real consumers (`as_object()`
  stays `std::map` — no public API change).

**The shape:**
- `madc::value` is THE type. Its 8 kinds are unchanged (madcdat's
  object-kind rows are load-bearing — merging kinds was considered and
  rejected). One semantic addition: `object()` / `array()` on a
  `kind::null` value VIVIFY it (null → empty object / empty array)
  instead of throwing, so a default-constructed script `madc::array`
  works with both keyed and indexed helpers.
- Script-side `madc::array` = an ALIAS of `madc::value` (one
  implementation, both names). The mangler resolves the alias to the
  class identity (g++ canon — typedefs are transparent in mangling), so
  script calls to `madc::eval_expression_ctx(..., array&)` produce the
  host `madc::value&` symbols. `ddARRAY` is retargeted: it describes
  `sizeof(madc::value)` with qualified class identity `madc::value`;
  the `array` / `madc::array` name mappings (parser.cpp:1738,
  `madc_ns["array"]`, pch.cpp:286) keep resolving to it. The `dtARRAY`
  DataType tag and the builtin registration survive until A0.2.
- Keyed context arrays (`context_set_*`, scope capture) become
  `kind::object` entries; indexed php::/perl::/… arrays become
  `kind::array` vectors. The ns_*.cpp helper signatures change
  `MadArray*` → `madc::value*` and bodies use `value::array()` /
  `value::object()`; behavior is pinned by the existing testphp /
  testperl / testlang / testrust / testmadceval* expectations.
- **`build_runtime_expression_context` is DELETED** — the script ctx
  already IS a `madc::value` object; `set_expression_context_root`
  consumes it directly. `validate_expression_context_paths` and
  `make_expression_context_literal` already speak `madc::value`.
- The `__madc_scope_set_*` setters cast to `madc::value*` and write
  object fields.
- Layering note: datadef.h (core) gains `#include "libmadc/value.h"` —
  the value type is genuinely shared between the compiler runtime and
  the embedding API; the C++ layer is the one real implementation
  (cpp-first-api). No cycle: value.h depends only on the standard
  library.
- TRACE REQUIRED before edit (pre-edit checklist): how builtin ddARRAY
  objects construct/destruct on CIR today (`madc::array ctx;` works in
  testmadcevalexprctx — find that lowering). Zero-initialized
  `std::vector`s function as empty in practice, but `madc::value` holds
  a `std::string` member, which zero-init does NOT make valid — the
  unified type needs real ctor/dtor calls (mangled-direct to the host
  `madc::value` symbols madc itself exports, the std::string model).

**A0.2 (queued, NOT this track):** retire the builtin entirely — a true
header-defined `madc::value`/`madc::array` class parsed from an embedded
header, deleting `parser.cpp:1738` / `pch.cpp:286` / `TokenARRAY` /
`madc_ns["array"]` and the `dtARRAY` tag. Gate: container-template
instantiation (faithful layout: the class holds `std::vector` /
`std::map` / `unique_ptr` members) or a pimpl refactor of `madc::value`.

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
  the host (new `src/ns_madc.cpp`, the ns_php.cpp layout). Per
  `.claude/rules/cpp-first-api.md`: the C++ layer is the one real
  implementation; the extern-C exports remain host-side as thin shims
  over it for C hosts (declared in the C API surface, not in the script
  header). The array-taking publics resolve mangled-direct because
  package A0 made script `madc::array` alias the host `madc::value`.
  Gates: the three green `testmadceval*` tests pin behavior through the
  migration.
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

- **A0 — interim rename only** (`MadArray` → `madc::array` as a distinct
  class, `MadValue` untouched or nested): churns the same six ns_*.cpp
  files twice once the unification lands; rejected in favor of unifying
  now, while the public API has ~one consumer.
- **A0 — merging value's `array`/`object` kinds** (one PHP-style hybrid
  container): madcdat's rows-as-objects make `kind::object` load-bearing
  across the storage backends; rejected.
- **A0 — pimpl `madc::array` now** to retire the builtin completely in
  this track: honest but adds compiler-path risk across six namespace
  surfaces on top of the eval work; queued as the A0.2 gate option
  instead.

- **A2 — parser intrinsics** lowering `madc::eval_*` straight to the
  `_ctx_runtime` helpers: duplicates the wrappers' marshalling in the
  parser and breaks the one-pattern-per-namespace architecture.
- **A3 — wrapper-side capture**: structurally impossible; a wrapper body
  cannot see caller scope.
- **B in both pipelines** (value compare in full eval too): would fork
  eval'd source semantics away from gcc/clang canon.
- **`capture=` boolean parameter**: redundant with the `_ctx` variants +
  engine policy; classic bool-trap.
