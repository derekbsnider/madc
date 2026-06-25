# Two-Tree Phase 3 — CONSUME the recipe (tsubst) — HANDOFF

**Date:** 2026-06-24 · **Branch:** `feature/front-end-performance-claude`
**Governing plan:** `2026-06-23-two-tree-cir-materialize-from-ast-PLAN.md` (read §11 + §0 RESUME).
**This doc:** the imperative execute-to-done for Phase 3 — the FIRST slice that delivers a
real win (a template method instantiated by copy+substitute instead of re-parse).

---

## 0. STATE (verify, do not trust blindly — run `scripts/resume.sh`)

✅ **KG SYNCED (2026-06-24).** FalkorDB was briefly unreachable mid-session; once back,
`madc-knowledge` was reconciled via `scripts/kg_query.sh`: Feature
`two_tree_tsubst_instantiation` (status in_progress) + Session
`session_2026_06_24_two_tree_phase4`, linked `LANDED` + `IMPLEMENTS_DECISION
→ cir_c2mir_backend_adr`. Captures: two-tree **Phase 3 COMPLETE**; **Phase 4 constructs
1–4 landed** (ptr/ref/const params `b5e9d86`; bare-T return `6117ccd`; pointer return
`c5adec2`; multiple type params + completeness guard `2204d19`); measured next targets
were pack 87 dominant > template-id 14, with direct type-arg binding called out as the
prerequisite cleanup. The local Codex update below completes direct type-arg binding; the
later local Codex updates also capture TYPE-pack metadata and implement direct
value/ref/expression/forwarding-call/constructor pack call-argument fan-out slices, then
admit the first covered system-header placement-new body, scalar `_Up` constructed-type
slice, simple class `_Up` placement-new slice, and direct `__destroy(T*)` helper slice.
The direct helper path emits a deferred Tree-1 marker for template-parameter pointees
and lowers the concrete class destructor/no-op after substitution; split system-header/user
parses now also reattach existing builtin/intrinsic `FuncDef`s to the active
`TokenProgram` so recipe capture can resolve compiler intrinsics. The remaining
design-level target is broader cir-node **pack expansion** (`tsubst_pack_expansion`
analogue) for real system-header forwarding/destructor patterns, class-valued constructor
argument packs that need real reference-forwarding/object-address lowering, broader system-header
nested/dependent calls, and template-id body surfaces.
The latest local slice also covers local non-pack nested namespace function-template calls
such as `sink(nn::ident(v))`, and the follow-up covers singleton by-value class-object
constructor packs in allocator-style placement-new bodies, then multi-element by-value
class-object constructor packs such as `PairBox(Item, Item)`, then value-returning
class-reference constructor packs such as `PairRef(const Item&, const Item&)`; all gated flag-off AND
flag-on 669/0/0/18; torture byte-identical by construction (env-gated); `test_cir` is
now 78/963/4.


DONE + gated + committed on this branch:
- **`b35cef2`** scoped template-param registry (`Program::template_param_scopes` +
  `TemplateParamScope` RAII + `intern_template_param` + `resolve_template_param`,
  consulted first in `resolve_current_class_type_alias`).
- **`2104299`** the PRODUCER + defer guard: `build_dependent_pattern(FuncDef*)`
  (parser.cpp) parses a member-fn-template body ONCE with param→`DataDefTemplateParam`
  placeholder and captures it as **`FuncDef::dependent_pattern`** (a TokenFunc parse-tree
  RECIPE), removing it from `pending_funcs`. `dependent_parse_in_progress` gates the
  instantiation entry points so nested calls don't eagerly instantiate.
- **`20bbf92`** widening step 1: the per-call dependence test
  (`call_involves_placeholder` / `datadef_involves_placeholder`, parser.cpp) replaced the
  global bail — the instantiation entry points now defer ONLY genuinely type-dependent
  calls (g++'s `any_type_dependent_arguments_p` analogue, pt.cc:30555).
- **✅ PHASE 3 FIRST SLICE DONE — the recipe is now CONSUMED (3 commits):**
  - **`e4dda75`** `tsubst_cir` core: `copy_cir_subtree` gains an optional
    `{placeholder→concrete}` map (default NULL = byte-identical); `subst_datadef` rewrites a
    `DataDefTemplateParam` directly or under ptr/ref/const layers via the canonical builders.
  - **`6c301f9`** `FuncDef::tsubst_source`: a concrete instance links back to its SOURCE
    member template (stamped in `instantiate_member_fn_template_for_call`).
  - **`2bf8696`** the SEAM (`func_def`, cir_builder.cpp): `tsubst_method_body` builds a
    covered method's BODY by `tsubst_cir` of the recipe (cir-built ONCE into a memoized
    Tree-1 pattern, `m_tsubst_body_patterns`) instead of `translate_block`. Binding recovered
    by aligning recipe placeholder params with the instance's concrete params. CONSERVATIVE
    capability gate: if the pattern build trips the `append_type_specs` guard (a body cast
    `(U)x` / local `U tmp;` lowers a placeholder TYPE → error node), memo NULL and FALL BACK
    to re-parse (not yet covered). Hybrid B: concrete signature/shell stays on the parse path;
    re-parse stays the fallback (PLAN §5).
  - **`2459a7e`** WIDENING — deferred type-spec MARKER (g++ TEMPLATE_TYPE_PARM-in-saved-tree):
    in pattern mode `append_type_specs` leaves a placeholder used as a TYPE (`(T)x`, `T tmp;`)
    as an N_IGNORE marker carrying the placeholder datadef (instead of erroring); tsubst
    EXPANDS it to the concrete specs via the SAME `append_type_specs` (byte-identical), or an
    error node → fall back. Unit-tested (expansion: test_cir 58/0).

Producer + consumption run ONLY behind env hook **`MADC_XTEST_DEP_PARSE`** (off by default →
production byte-identical). VALIDATED firing (not silent fallback): `Holder::set<int>` and
`testoutoflinemembertemplate::store<int>` build bodies by tsubst, BYTE-IDENTICAL emit to
re-parse. Gate every commit: build clean; fulltest flag-OFF AND flag-ON 669/0/0/18; --emit=c11
byte-identical; torture flag-off byte-identical by construction.

✅ **2026-06-24 LOCAL CODEX UPDATE — DEPENDENT-PARSE-of-`T`-as-a-TYPE gap RESOLVED
(uncommitted).** Root cause was C-style cast disambiguation in `Program::parseExpr_operatorArm`:
identifier cast heads only checked `datatype_map` / `struct_map`, so `(T)v` fell through as a
parenthesized expression and reported `T` as an undeclared identifier before the dependent
recipe could reach the type-spec marker. The parser now also consults
`resolve_current_class_type_alias`, which reaches the scoped `DataDefTemplateParam` registry,
for cast heads, cast-token consumption, and nested-cast lookahead. This records the placeholder
type without asking for concrete size/rawtype/conversion. `T tmp` locals were already accepted
through the declared-type path; regression coverage now pins both `member = (int)(T)v + 1;`
and `T tmp = v; member = (int)tmp + 1;` in dependent pattern parse.

Validation for the local update:
- `make -C src` green.
- `bin/test_cir` green: 59 test cases, 739 assertions, 4 skipped.
- `make -C src fulltest` green: 669 passed, 0 failed, 0 timed out, 18 skipped.
- `env MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh` green: 669 passed, 0 failed,
  0 timed out, 18 skipped.
- Focused reducers: `tmp/tsubst_marker.mad`, `tmp/tsubst_scalar.mad`,
  `tmp/tsubst_local.mad` clean.
- `--emit=c11` flag-off vs flag-on output byte-identical for marker/local reducers; no raw
  `T`, `tsubst:`, or `unbound template` markers in emitted C.
- GDB breakpoint confirmed `CirBuilder::tsubst_cir` is reached from
  `CirBuilder::tsubst_method_body` for the marker reducer, proving this is not silent fallback.

✅ **PHASE 4 — construct 1 DONE (ptr/ref/const params).** The binding RECOVERY now peels
`T*` / `T&` / `const T` param layers in lockstep with the concrete instance param, binding the
BARE placeholder (so `subst_datadef`'s existing peel rebuilds the derived type on body nodes).
New file-static `recover_param_binding` (cir_builder.cpp, beside `subst_datadef`) replaces the
scalar-only inline loop in `tsubst_method_body`; the old `is_template_param()` case is its
recursion base. A layer mismatch recovers nothing → safe re-parse fallback. VERIFIED: a
temporary `[TSUBST] … recovered N binding(s)` trace showed `T*`/`T&`/`const T&` go 0→1 binding
(fire tsubst, not silent fallback); trace removed. test_cir 60/60 (new derived-type case pins
`T*`→`int*`, `T&`→`int&`, Tree-1 immutability). Reducers `tmp/tsubst_{ptr,ref,constref}.mad`
run 2/4/6 and emit byte-identical flag-off vs flag-on. Torture byte-identical BY CONSTRUCTION
(all new code behind the `MADC_XTEST_DEP_PARSE` env gate — production never reaches it).

✅ **PHASE 4 — construct 2 DONE (bare-`T` dependent return type).** `tsubst_eligible`
(parser.cpp) now ACCEPTS a member template whose return type is the bare placeholder
(`T echo(T v)`). The return type lives in the concrete SHELL (resolved on the normal parse
path — hybrid B); the recipe parse passes the placeholder return type to `parseFunction`
(which already tolerates it — NO parser change needed), and tsubst substitutes the BODY via
the param binding. KEY TRAP avoided: `return_value_type().is_template_param()` is FALSE for a
bare-`T` return (it is a return TOKEN naming T, not a `DataDefTemplateParam`), and
byte-identical emit MASKS a silent re-parse fallback — so the gate discriminates the bare case
at the TOKEN level (return-token stream == the single identifier `T`) and firing was reproven
with the `[TSUBST]` trace, NOT byte-identical alone. A NON-bare dependent return (`T*`, `T&`,
`vector<T>`, `T::type`) stays REJECTED (token scan) → re-parse fallback. VERIFIED: trace shows
`echo`/`twice` fire (1 binding), `addr` (T* return) does NOT fire. test_cir 61/61 (new case:
`echo` eligible via funcdef_map key, `T*` `addr` rejected, exactly 1 pattern). Reducers
`tmp/tsubst_{retdep,retdep2,retptr}.mad` run 8/14/7 byte-identical. Torture byte-identical BY
CONSTRUCTION (`tsubst_eligible`'s only caller is `build_dependent_pattern`, gated behind
`MADC_XTEST_DEP_PARSE`).

✅ **PHASE 4 — construct 3 DONE (POINTER dependent return `T*`).** `tsubst_eligible` now
also accepts a return that is the bare param T wrapped in POINTER layers (`T`, `T*`, `T**`).
Detected on the return-token stream (recon via a temporary `[RETTOK]` dump: `id=60`=ident,
`id=13`=`tkStar`, `id=24`=`tkBand`; `const` is NOT in these tokens): exactly one identifier
(== T), every other token a `*` (`tkStar`). A `<` (template-id `vector<T>`), `::` (`T::type`),
or a second identifier stays REJECTED. VERIFIED (trace, not byte-identical alone): `T* passthru(T* p)`
fires (1 binding), runs 42, byte-identical; `vector<T>` return does NOT fire. test_cir 61/61
(updated case: `echo` bare-T + `addr` `T*` both eligible, `ref` `T&` not). Reducers
`tmp/tsubst_retptr2.mad` (42) + `tmp/tsubst_retptr.mad` (7) byte-identical. Torture byte-identical
BY CONSTRUCTION (gated). ⚠️ REFERENCE returns (`T&`/`const T&`) are deliberately EXCLUDED:
`tmp/tsubst_retref.mad` / `tsubst_retcref.mad` fail to compile **flag-off too** ("undeclared
identifier `Holder__passref`" + "lvalue required") — a SEPARATE pre-existing
ref-return-member-template instance-symbol bug, NOT tsubst. Revisit `T&` returns after that bug
is fixed (it's a call-site/instantiation-naming issue, a different track).

✅ **PHASE 4 — construct 4 DONE (MULTIPLE type params).** `tsubst_eligible` now accepts a
member template with ≥1 type param (was hard-capped at exactly 1). The `is_type`/`is_pack`
checks were generalized from index `[0]` to ALL indices (so a mixed `<class A, int N>` cannot
slip through), and the return / body `T`-scans now test a param-name SET (`pnames`) rather than
a single `T`. `recover_param_binding` already builds one binding per param.
VERIFIED (trace): `template<class A, class B> void set2(A a, B b)` fires with **2 bindings**
(runs 7, byte-identical); single-param `echo` still fires (1 binding). test_cir 62/62 (new case:
`set2` + `echo` both eligible, 2 patterns). Torture byte-identical BY CONSTRUCTION (gated).
🛠 **COMPLETENESS GUARD (the flag-on gate caught a real bug — do not remove):** multi-param
first crashed 10 flag-on tests (incl. `testvectorptr`). Cause: a real multi-param std method
where a type param appears only in the BODY (not as a parameter type) recovered a PARTIAL
binding (non-empty), so tsubst fired leaving UNSUBSTITUTED placeholders → wrong code no error
node caught. Fix (`tsubst_method_body`, deepest layer): require
`binding.size() >= source->template_param_names.size()` — every param recovered from the
signature, else fall back to re-parse. Single-param behavior unchanged (need=1 == old
binding.empty check).
⚠️ NON-TYPE params stay rejected (correct — they need value substitution, not built); they also
can't currently be exercised end-to-end because the explicit non-type member-template call
syntax (`f<int,5>(...)`) fails to PARSE (a separate pre-existing gap — see `tmp/tsubst_mixed.mad`).

✅ **2026-06-24 LOCAL CODEX UPDATE — DIRECT TYPE-ARG BINDING DONE (uncommitted).**
Concrete instantiated member-template `FuncDef`s now carry `tsubst_type_args`, a parser-owned
vector of resolved TYPE template arguments in `tsubst_source->template_param_names` order.
The vector is filled from `try_instantiate_namespace_fn_template` /
`instantiate_fn_template_binding` after explicit args, deduction, and defaults settle, then
stored beside `tsubst_source` in `instantiate_member_fn_template_for_call`. CIR
`tsubst_method_body` now builds `{DataDefTemplateParam* -> concrete DataDef*}` directly from
that vector via `Program::intern_template_param`, so `recover_param_binding` is retired.
This matches g++/clang's saved-tree-plus-args model and covers body-only type params that
cannot be recovered from a signature. New regression: `Holder::body_only<int>(7)` records
`int` in `tsubst_type_args` even though `T` appears only as `T tmp = (T)x` in the body.

The direct-binding slice exposed unsupported bodies that signature recovery had previously
kept on fallback. Two conservative guards restore correctness until widened deliberately:
reference-parameter bodies stay on re-parse fallback because value reads need the parsed
parameter variable metadata, and recipes containing dependent nested calls stay on fallback
because call re-resolution after substitution is not implemented yet. The flag-on gate caught
both classes (`testmemtmplrefparam` and the vector/subscript family); both gates are green now.
Validation: `bin/test_cir` 63 test cases / 766 assertions / 4 skipped; `make -C src fulltest`
669/0/0/18; `MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh` 669/0/0/18; `git diff --check`
clean. KG synced: Feature `two_tree_tsubst_instantiation` now records direct type-arg binding
as done locally and Session `session_2026_06_24_two_tree_direct_type_args_codex` is linked via
`LANDED`.

✅ **2026-06-24 LOCAL CODEX UPDATE — TYPE-PACK ARG METADATA CAPTURED (uncommitted).**
The parser instantiation path now also records concrete TYPE parameter-pack elements on the
instantiated `FuncDef` in `tsubst_type_arg_packs`, parallel to
`tsubst_source->template_param_names`: non-pack slots are empty; pack slots hold deduced
element `DataDef*`s in expansion order. `collect_ordered_type_arg_bindings` captures both
ordinary type args and pack slots from the same place the re-parse path already computes them
(`pack_param`/`pack_elems` and supported 0/1-element template-id packs). This does **not**
claim CIR fan-out is implemented yet; it removes the missing-data blocker so the future
`tsubst_pack_expansion` analogue has real arity and element types without inspecting
token-expanded reparse output. Unit coverage pins `template<class... Args> void pack_body(Args... args)`
called as `pack_body(1, 2)`: the concrete instance links to its source and records one pack
slot with two `int` elements. That slice validated at `bin/test_cir` 64 test cases / 778 assertions /
4 skipped; `make -C src fulltest` 669/0/0/18; `MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh`
669/0/0/18; focused env-gated variadic reducer clean; `git diff --check` clean. KG synced:
Feature `two_tree_tsubst_instantiation` now records type-pack metadata capture and Session
`session_2026_06_24_two_tree_direct_pack_args_codex` is linked via `LANDED`.

✅ **2026-06-24 LOCAL CODEX UPDATE — DIRECT VALUE-PACK FAN-OUT DONE (uncommitted).**
The tsubst path now handles the first actual CIR pack expansion shape: direct value-pack
call arguments such as `sink(args...)` in a member-template body with `Args... args`.
During dependent parse, `expr...` becomes `TokenPackExpansion`; CIR translation lowers the
inner expression once and tags the resulting `cir_node` with the source pack index and direct
value-pack name. During `copy_cir_subtree`, a marked child under an `N_LIST` fans out by the
instantiated `FuncDef::tsubst_type_arg_packs` slot, augments the active substitution map with
the concrete pack element type, and renames direct value-pack ids from `args` to `args__0`,
`args__1`, etc. Empty packs emit no list children. Complex pack patterns, forwarding forms,
ordinary reference-parameter bodies, dependent nested calls, and template-id body/return
surfaces still fall back.
Unit coverage pins `template<class... Args> int pack_call(Args... args) { return sink(args...); }`
called as `pack_call(3, 4)`: it fires tsubst instead of silent re-parse fallback and returns
34. Validation: `bin/test_cir` 65 test cases / 790 assertions / 4 skipped; `make -C src`
green; `make -C src fulltest` 669/0/0/18; `MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh`
669/0/0/18; `git diff --check` clean. KG synced: Feature
`two_tree_tsubst_instantiation` now records direct value-pack fan-out and Session
`session_2026_06_24_two_tree_direct_value_pack_fanout_codex` is linked via `LANDED`.

✅ **2026-06-24 LOCAL CODEX UPDATE — DIRECT REFERENCE-PACK FAN-OUT DONE
(uncommitted).** The direct pack fan-out path now also covers reference parameter packs such
as `template<class... Args> int pack_ref(Args&... args) { return sink(args...); }`. The missing
piece was not list expansion itself; `TokenPackExpansion` was looking only for a bare
`DataDefTemplateParam`, while `Args&... args` presents as `DataDefREF(DataDefTemplateParam)`.
CIR lowering now peels reference/const/pointer type layers to discover the underlying TYPE
pack marker. The re-parse fallback guard was narrowed too: ordinary reference-parameter bodies
still fall back, but reference params whose referent is a TYPE parameter pack are admitted, and
recipe `FuncDef`s are checked against source template metadata because the recipe shell does
not carry the pack metadata itself. Unit coverage pins `Args&... args` plus `sink(args...)`,
requires Tree-1 copies, and returns 34. Validation: `make -C src` green; `bin/test_cir` 66 test
cases / 802 assertions / 4 skipped; focused `MADC_XTEST_DEP_PARSE=1 bin/test_cir` green; related
canaries (`testmemtmplrefparam`, `testmemtmplfwdrefpack`, `testmemtmplpackexpand`,
`testvariadicfn`) green; `make -C src fulltest` 669/0/0/18; `MADC_XTEST_DEP_PARSE=1 bash
scripts/run_tests.sh` 669/0/0/18; `git diff --check` clean. KG synced: Feature
`two_tree_tsubst_instantiation` now records direct reference-pack fan-out and Session
`session_2026_06_24_two_tree_direct_reference_pack_fanout_codex` is linked via `LANDED`.

✅ **2026-06-24 LOCAL CODEX UPDATE — DIRECT EXPRESSION-PACK FAN-OUT DONE
(uncommitted).** The direct pack fan-out path now covers expression-pattern packs that do not
need call re-resolution, such as `sink((args + 1)...)`. The missing piece was metadata
discovery: the `TokenPackExpansion` root for `(args + 1)...` is the translated binary
expression, not the pack variable itself, so the earlier direct-id path could not find the pack
marker or value name. CIR lowering now recursively discovers the TYPE pack marker and direct
value-pack name inside the pattern tree, then reuses the existing `copy_cir_subtree` fan-out to
clone the expression once per concrete pack element and rename inner `args` leaves to
`args__N`. Unit coverage requires Tree-1 copies for `sink((args + 1)...)` and returns 45.
Validation: `make -C src` green; focused `bin/test_cir --test-case=*expression-pack*` green;
pack subset `bin/test_cir --test-case=*pack*` green; `bin/test_cir` and
`MADC_XTEST_DEP_PARSE=1 bin/test_cir` both 67 test cases / 814 assertions / 4 skipped; related
flag-on reducers/canaries (`tmp/tsubst_pack_expr.mad`, `tmp/tsubst_pack_ref.mad`,
`tests/testvariadicfn.mad`) green; `make -C src fulltest` 669/0/0/18 with both gates green;
`MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh` 669/0/0/18.

✅ **2026-06-24 LOCAL CODEX UPDATE — FIRST FORWARDING-CALL PACK FAN-OUT DONE
(uncommitted).** The pack fan-out path now covers the first dependent-call pattern:
`sink(std::forward<Args>(args)...)`. The parser now wraps function-call `expr...` forms as
`TokenPackExpansion` during dependent parse instead of returning before the generic
ellipsis wrapper. CIR copy still fans out one marked list child per concrete pack element,
but copied call-id leaves now re-resolve through the normal namespace overload machinery
using the concrete explicit template args and substituted parameter types, so placeholder
`std::forward<Args>` ids become the concrete specialization symbol for each cloned argument.
Pattern-build side effects in real libstdc++ constructor/destructor helpers were avoided by
keeping system-header template-id pack bodies on the parsed-body fallback until those broader
surfaces are explicitly covered. Unit coverage pins a local `std::forward<T>(T)` reducer,
requires Tree-1 copies for `sink(std::forward<Args>(args)...)`, and returns 34. Validation:
`make -C src` green; pack subset `bin/test_cir --test-case=*pack*` 5/60 green;
`bin/test_cir` and `MADC_XTEST_DEP_PARSE=1 bin/test_cir` both 68 test cases / 826 assertions
/ 4 skipped; previous flag-on failure canaries (`testcontainerdtor`, `testforeachref`,
`testvectorptr`) pass; `make -C src fulltest` 669/0/0/18 with both gates green;
`MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh` 669/0/0/18; `jq empty
claude_status.json` and `git diff --check` clean.

✅ **2026-06-24 LOCAL CODEX UPDATE — SYSTEM-HEADER SCALAR PLACEMENT-NEW PACK
SLICE DONE (uncommitted).** The system-header fallback guard now admits the
structurally covered placement-new constructor-pack body shape, rather than
rejecting every template-id pack body from a system-header path. In CIR pattern
mode, placement-new whose constructed type is still a template parameter (for
example allocator `_Up`) leaves a marker instead of lowering too early; during
`copy_cir_subtree`, tsubst substitutes that constructed type and lowers scalar
or pointer `_Up` placement construction through the existing placement-new path.
Pack expansion outside an `N_LIST` is accepted only for singleton packs, which
matches scalar constructor assignment; singleton value-pack copies keep the
original parameter name (`args`) instead of renaming to a non-existent
`args__0`. Class `_Up` placement construction with class-valued constructor
argument packs deliberately returns an error marker and falls back to re-parse
until object-argument pack lowering is covered.
Unit coverage pins both a simulated system-header placement-new pack body and
an allocator-style scalar `_Up` reducer returning 42.

Validation: `make -C src` green; `MADC_XTEST_DEP_PARSE=1 bin/madc --emit=c11
tmp/tsubst_pack_new_scalar.mad` emits `__ns_std_forward__o2(args)` (not
`args__0`); focused scalar/placement-new doctests green (3 tests / 40
assertions for placement-new subset); pack subsets green flag-off and flag-on
(8 tests / 98 assertions); canaries green under `MADC_XTEST_DEP_PARSE=1`
(`testcontainerdtor` exits 0 with known non-fatal diagnostics, plus
`testvectorptr`, `testplacementnew*`, `testvariadicfn`, `testforeachref`,
`testmemtmplpackexpand`); `bin/test_cir` and `MADC_XTEST_DEP_PARSE=1
bin/test_cir` both 72 test cases / 878 assertions / 4 skipped; `make -C src
fulltest` 669/0/0/18; `MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh`
669/0/0/18.

✅ **2026-06-25 LOCAL CODEX UPDATE — SIMPLE CLASS `_Up` PLACEMENT-NEW
TSUBST DONE.** The deferred constructed-type marker path now also admits class
`_Up` placement construction when the constructor pack elements are scalar/pointer-like.
After substituting `_Up`, `copy_cir_subtree` re-lowers the retained `TokenNEW`
through the existing class placement-new constructor path instead of forcing
fallback. A guard keeps class-valued constructor argument packs on the parsed-body
fallback; the first unrestricted attempt regressed real-header container canaries
through recursive object-argument lowering, so this slice deliberately covers the
simple scalar-argument class case only. Unit coverage pins a system-header-shaped
`new ((void*)p) Up(std::forward<Args>(args)...)` constructing `Box(int,int)` and
requires Tree-1 copies. Validation: `make -C src` green; focused class placement-new
doctest green (1 test / 14 assertions); `bin/test_cir` and
`MADC_XTEST_DEP_PARSE=1 bin/test_cir` both 73 test cases / 892 assertions / 4 skipped;
`make -C src fulltest` 669/0/0/18; `MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh`
669/0/0/18.

✅ **2026-06-25 LOCAL CODEX UPDATE — DIRECT `__destroy(T*)` HELPER TSUBST
DONE.** Direct compiler-intrinsic destructor helpers in retained template bodies now
have a Tree-1 path. In CIR pattern mode, `__destroy(p)` where `p` has a
template-parameter pointee type emits a deferred marker instead of inspecting the
placeholder pointee too early. During `copy_cir_subtree`, tsubst substitutes the
pointer element type, lowers class pointees to the concrete complete destructor call,
and lowers scalar/pointer pointees to a no-op expression. Broader system-header
destructor-pack/member-template surfaces such as `_Destroy_aux::__destroy` still stay
on the parsed-body fallback.

The slice also fixed a producer-layer multi-buffer lookup issue: `_parser_init`
re-runs builtin registration per parse, but `addFunction()` previously returned early
when `funcdef_map` already had a shared `FuncDef`, leaving later `TokenProgram`s
without a function variable for compiler intrinsics. It now reattaches the existing
`FuncDef` to the active `TokenProgram` when needed, so split system-header/user parses
can capture `__destroy(p)` during dependent recipe construction without creating a
parallel implementation.

Unit coverage pins a synthetic system-header `Cleaner::clean(T*) { __destroy(p); }`
body, verifies the source template has a dependent recipe, verifies the concrete
instance links to that source, requires Tree-1 copies, and runs a class destructor
side effect returning 7. Validation: `make -C src` green; focused direct-destroy
doctest green (1 test / 17 assertions); `bin/test_cir` and
`MADC_XTEST_DEP_PARSE=1 bin/test_cir` both 74 test cases / 909 assertions / 4 skipped;
`MADC_XTEST_DEP_PARSE=1 bin/madc tests/testcontainerdtor.mad` exits 0 with known
non-fatal diagnostics; `make -C src fulltest` 669/0/0/18;
`MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh` 669/0/0/18.

✅ **2026-06-25 LOCAL CODEX UPDATE — LOCAL NESTED NAMESPACE-CALL TSUBST
DONE.** Copied dependent-call re-resolution now covers the first non-pack nested
call shape in a retained member-template body: a local namespace function-template call
such as `sink(nn::ident(v))`, where `ident` depends on the member-template type parameter
but is not itself a pack expansion. The old pack-only call-id rewriter is now a
dependent-call rewriter: it substitutes explicit/argument DataDefs, then reuses the normal
`find_namespace_function_overload` path to choose the concrete callee symbol for the copied
Tree-2 node. The dependent-call eligibility scan admits only this re-resolvable namespace
call shape; member calls and unknown dependent expressions still fall back. Non-pack
system-header dependent calls deliberately mark the copied node unsupported so
`tsubst_method_body` falls back to the parsed body; pack-expanded system-header calls remain
covered by the existing placement-new path.

Unit coverage pins `template<class T> int nested(T v) { return sink(nn::ident(v)); }`,
requires Tree-1 copies, and returns 42. Validation: `make -C src` green; focused nested
doctest green (3 tests / 35 assertions); pack subset green flag-off and flag-on (8 tests /
98 assertions); `bin/test_cir` and `MADC_XTEST_DEP_PARSE=1 bin/test_cir` both 75 test cases /
921 assertions / 4 skipped; previously failing broad-guard canaries
(`testcontainerdtor`, `testvector`, `teststringref`, `testforeachref`) exit 0 under
`MADC_XTEST_DEP_PARSE=1`; `make -C src fulltest` 669/0/0/18; `MADC_XTEST_DEP_PARSE=1 bash
scripts/run_tests.sh` 669/0/0/18; `jq empty claude_status.json`; `git diff --check` clean.

✅ **2026-06-25 LOCAL CODEX UPDATE — SINGLETON CLASS-OBJECT PLACEMENT-NEW
PACK TSUBST DONE.** The deferred `_Up` placement-new marker now admits the first
class-valued constructor-pack case: a singleton class object that binds to a by-value
class constructor parameter, such as `Box(Item)` inside
`new ((void*)p) Up(std::forward<Args>(args)...)`. The class object pack expression stays
marked until copy-time substitution, so the copied Tree-2 body renames the pack parameter
and re-resolves the forwarded callee inside the instantiated function instead of
materializing a temporary during Tree-1 lowering. The guard remains deliberately narrow:
multi-element class-object packs and class-reference/object-address packs still mark the
copy unsupported and fall back to the parsed body.

Unit coverage pins a synthetic system-header `Maker::make<Up, Args...>` body with
`Box(Item)` construction, requires Tree-1 copies, and returns 42. Validation: `make -C src`
green; focused class-object doctest green (1 test / 14 assertions); placement-new subset
green flag-off and flag-on (5 tests / 68 assertions); pack subset green flag-off and flag-on
(9 tests / 112 assertions); `bin/test_cir` and `MADC_XTEST_DEP_PARSE=1 bin/test_cir` both
76 test cases / 935 assertions / 4 skipped; real-header canaries (`testcontainerdtor`,
`testvector`, `teststringref`, `testforeachref`, `testvectorptr`) exit 0 under
`MADC_XTEST_DEP_PARSE=1`; `make -C src fulltest` 669/0/0/18; `MADC_XTEST_DEP_PARSE=1 bash
scripts/run_tests.sh` 669/0/0/18.

✅ **2026-06-25 LOCAL CODEX UPDATE — MULTI-ELEMENT CLASS-OBJECT PLACEMENT-NEW
PACK TSUBST DONE.** The by-value class-object placement-new pack path now handles
multi-element packs by mapping each expanded element to the constructor parameter it will
occupy after fan-out. This covers allocator-style `new ((void*)p)
Up(std::forward<Args>(args)...)` where `Up` has a constructor like
`PairBox(Item, Item)`. The guard remains parameter-aware: any class object that would bind
to a reference/object-address parameter still marks the copy unsupported and falls back,
because that surface needs per-element `object_arg_addr` lowering rather than the existing
marked-expression fan-out.

Unit coverage pins a synthetic system-header `Maker::make<Up, Args...>` body with
`PairBox(Item, Item)` construction, requires Tree-1 copies, and returns 34. Validation:
`make -C src` green; focused multi-element doctest green (1 test / 14 assertions);
placement-new subset green flag-off and flag-on (6 tests / 82 assertions); pack subset
green flag-off and flag-on (10 tests / 126 assertions); `bin/test_cir` and
`MADC_XTEST_DEP_PARSE=1 bin/test_cir` both 77 test cases / 949 assertions / 4 skipped;
real-header canaries (`testcontainerdtor`, `testvector`, `teststringref`,
`testforeachref`, `testvectorptr`) exit 0 under `MADC_XTEST_DEP_PARSE=1`;
`make -C src fulltest` 669/0/0/18; `MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh`
669/0/0/18.

✅ **2026-06-25 LOCAL CODEX UPDATE — VALUE-RETURNING CLASS-REFERENCE
PLACEMENT-NEW PACK TSUBST DONE.** The deferred `_Up` placement-new path now admits the
first class-reference constructor-pack case: value-returning forwarded class objects that
bind to reference constructor parameters, such as `PairRef(const Item&, const Item&)`.
The copied Tree-2 lowering fans out the pack, copies each forwarded expression under its
concrete pack-element substitution, and keeps any required class-object temporary
declaration/assignment inside the placement-new statement expression. Real
reference-returning `std::forward` and broader object-address system-header packs still
fall back, after a canary caught that admitting those too early recurses through
placeholder-token object-address lowering.

Unit coverage pins a synthetic system-header `Maker::make<Up, Args...>` body with
`PairRef(const Item&, const Item&)` construction, requires Tree-1 copies, and returns 34.
GCC reference check: `g++ -S -fverbose-asm -O0 tmp/tsubst_ref_class_ctor_pack.cpp -o
tmp/tsubst_ref_class_ctor_pack.s` shows the canonical real-`std::forward` path passes
addresses of the function parameters to `PairRef`. Validation: `make -C src` green;
focused value-returning doctest green flag-off and flag-on (1 test / 14 assertions);
placement-new subset green flag-off and flag-on (7 tests / 96 assertions); pack subset
green flag-off and flag-on (11 tests / 140 assertions); `bin/test_cir` and
`MADC_XTEST_DEP_PARSE=1 bin/test_cir` both 78 test cases / 963 assertions / 4 skipped;
real-header canaries (`testcontainerdtor`, `testvector`, `teststringref`,
`testforeachref`, `testvectorptr`) exit 0 under `MADC_XTEST_DEP_PARSE=1`;
`make -C src fulltest` 669/0/0/18; `MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh`
669/0/0/18.

✅ **2026-06-24 LOCAL CODEX UPDATE — CONSTRUCTOR VALUE-PACK FAN-OUT DONE
(uncommitted).** Member-template constructor instantiation now participates in the same
two-tree metadata path as member functions: under `MADC_XTEST_DEP_PARSE` it builds the
dependent recipe, captures parser-settled TYPE args and TYPE-pack arg vectors from
`try_instantiate_namespace_fn_template`, and stamps the concrete constructor `FuncDef` with
`tsubst_source`, `tsubst_type_args`, and `tsubst_type_arg_packs`. CIR `tsubst_method_body`
can therefore copy a covered local constructor body from Tree-1 and fan out direct
value-pack arguments, e.g. `Holder(Args... args) { member = sink(args...); }`, instead of
only relying on the re-parsed constructor body. Real system-header constructor/destructor
pack surfaces with template-id bodies remain on the conservative fallback. Validation:
`make -C src` green; `MADC_XTEST_DEP_PARSE=1 bin/test_cir --test-case=*constructor*` green;
pack subset `bin/test_cir --test-case=*pack*` and flag-on pack subset both 6/72 green;
`bin/test_cir` and `MADC_XTEST_DEP_PARSE=1 bin/test_cir` both 69 test cases / 838 assertions
/ 4 skipped; related flag-on canaries (`testcontainerdtor`, `testforeachref`,
`testvectorptr`, `testvariadicfn`) exit 0; `make -C src fulltest` 669/0/0/18;
`MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh` 669/0/0/18; `jq empty
claude_status.json` and `git diff --check` clean.

📊 **DATA-GROUNDED NEXT TARGETS (measured 2026-06-24, not guessed).** A temporary
reason-tagged `tsubst_eligible` (env `MADC_XTEST_ELIG`, since reverted) was run flag-on over the
container/template corpus (testvector, testvectorptr, testmap, testset, teststdmapint,
testtemplatecontainer, testcontainerdtor, testoutoflinemembertemplate, testmadc_ns,
testnestedtemplatedtor). Rejection tally:
- **`pack` 87** — DOMINANT (~58%). Variadic templates (emplace, `make_*`, forwarding ctors).
- `OK` 47 — already eligible.
- `template-id-or-lt` 14 — a body/return `<` (a real `Foo<T>` template-id OR a `<` comparison;
  the scan can't tell them apart — conservatively rejects both).
- `nontype` / `dep-return` / `dep-name` — ~0 in this corpus (rare; don't prioritize).
⇒ The CHEAP relaxations / prerequisites are EXHAUSTED (params, ptr/ref returns, multi-param,
direct type-arg binding, direct type-pack metadata, direct value-pack fan-out, direct
reference-pack fan-out, direct expression-pattern pack fan-out, first forwarding-call pack
fan-out, covered local constructor value-pack fan-out, and covered system-header scalar
placement-new pack fan-out, direct destroy helper tsubst, and local nested namespace-call
tsubst — all landed locally). The
remaining next target is DESIGN-LEVEL (traced
2026-06-24, confirmed NOT a gate relaxation):

1. **PACKS (keep first — dominant).** The existing pack machinery (`pack_subst`,
   `first_template_pack_index`, the `pack_name...` token splice ~parser.cpp:2815) is ALL
   TOKEN-LEVEL — it serves the RE-PARSE path (splice N tokens, then parse). The tsubst path
   now has node fan-out slices for direct value/ref `args...`, direct expression patterns
   like `(args + 1)...`, the first local forwarding-call pattern
   (`std::forward<Args>(args)...` as a direct call argument), and a covered local
   constructor value-pack pattern, plus covered system-header scalar/pointer and
   simple class `_Up` placement-new construction, but broader cir-node **PACK
   EXPANSION** remains: real system-header forwarding/destructor pack patterns,
   real reference-forwarding/object-address constructor argument packs, broader
   system-header nested/dependent re-resolution, and the rest of g++'s
   `tsubst_pack_expansion` surface.
   Multi-commit.

Lower priority / blocked: template-id body/return (`vector<T>`, `Foo<T>` — 14; needs
dependent-template-id handling, AND disambiguating `<` template-id from `<` comparison);
reference dependent return (`T&` — blocked on the pre-existing ref-return instance bug);
non-type params (value substitution + blocked on the `f<int,5>` call-parse gap); `T::x`;
typeless placeholder + ADL; real `is_type_dependent`. The re-parse deletion (Phase 5 = g++
lex/parse/sema parity) waits for full coverage; Phase 6 (serialize Tree-1 / header-forest) waits
for Phase 5 (user directive 2026-06-24).

## 1. GOAL (one sentence)

At cir-build of a concrete instantiated member-template method, if a Tree-1 recipe exists
for it, build the method body by **`tsubst`** (copy the recipe's cir pattern + substitute
the placeholder with the concrete arg) and feed THAT to c2mir — instead of
`translate_block` over the re-parsed body. Capability-gated; re-parse fallback stays.

## 2. THE SEAM

`CirBuilder::func_def(TokenFunc *tf)` calls `node_t body = translate_block((TokenCpnd*)tf);`
at **`src/cir_builder.cpp:11458`** (verify the line — it drifts). That is where a concrete
method's body becomes cir. Phase 3 replaces that call, for covered methods, with the tsubst
result.

## 3. STEPS (each its own gated commit; gate = §5)

1. **cir-build the recipe → a Tree-1 cir pattern.** `translate_block` over the recipe's
   placeholder body produces cir nodes whose `datadef` is the `DataDefTemplateParam`
   placeholder. The Phase 1.5 `append_type_specs` guard (cir_builder.cpp) turns a
   placeholder reaching type-lowering into an error node — that is correct for a pattern
   that is NEVER compiled directly. So either (a) build the pattern in a mode where the
   guard is suppressed and the pattern is held un-lowered, or (b) fuse build+substitute so
   the placeholder is replaced BEFORE `append_type_specs` runs. **(b) is simpler and safer**
   — never hold a placeholder-bearing cir tree that could reach c2mir.
2. **`tsubst_cir(pattern, {placeholder → concrete})`.** Reuse **`copy_cir_subtree`**
   (cir_builder.cpp:379, Phase 1 — it already makes a private node_t base) and, in the
   copy, replace any `datadef` that `is_template_param()` with the concrete arg type.
   Handle DERIVED types: ptr-to-placeholder → `getPointerType(concrete)`,
   ref-to-placeholder → `getReferenceType(concrete)`, const → `getConstType`. The concrete
   arg comes from the instantiation's type-arg binding.
3. **Method↔pattern link.** A concrete instantiated method's `FuncDef` must find its
   recipe + the concrete type args. Record the SOURCE template identity on the instantiated
   `FuncDef` when it is produced (the member-fn-template instantiation path:
   `instantiate_member_fn_template_for_call` / `try_instantiate_namespace_fn_template`).
   The recipe lives on the SOURCE template's `FuncDef::dependent_pattern`; map
   instantiated → source by (owner, member display name) or a recorded back-pointer.
4. **Wire the seam (cir_builder.cpp:11458).** For a covered method (pattern exists +
   capability-eligible), use the tsubst result; else `translate_block` (fallback). Keep the
   re-parse path — its deletion trigger is full coverage (PLAN §5).
5. **Cache + ownership.** The Tree-1 cir pattern is built ONCE per template member and
   reused across instantiations — own it in the CirBuilder Tree-1 arena (immutable, never
   freed mid-compile). Key by the source FuncDef (PLAN §11.5b).

Start SCALAR-FIRST: the narrowest covered method (one type param, scalar body, e.g.
`void set(T v){ member = v; }` on a concrete class) so the §5 equivalence check is trivial
to satisfy and debug. Widen one construct per commit after.

## 4. THE CRUX RISK (read before coding)

This is the FIRST slice where **output actually changes** for covered methods. The
`--emit=c11` gate then means **tsubst output must be byte-identical to the re-parse
output** for those methods. Any divergence is a real bug — debug it against the re-parsed
body (they must lower to the same cir). De-risk by starting with the most trivial covered
method and diffing emitted C tsubst-vs-reparse on JUST that method before widening.

## 5. GATE (every commit — correctness only, never perf-gate)
- `make -C src` clean, no new warnings.
- `make -C src fulltest` → 669/0/0/18 (flag-off, production).
- **flag-ON** `MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh` → 669/0/0/18 (isolation +,
  once consumption is wired, correctness of covered methods).
- gcc.c-torture failset byte-identical to the 51-name baseline (0 timeouts).
- `--emit=c11` byte-identical vs the prior commit on representative TUs — for a covered
  method this is the tsubst==reparse equivalence proof.

## 6. SETTLED — do not re-litigate
- Hybrid B (concrete shell at parse, tsubst the BODIES at cir-build). NOT full cir-level
  deferral (A), NOT parse-layer copy (C). (PLAN §11.3.)
- Deferral primitive = the PER-CALL dependence test (`call_involves_placeholder`), g++'s
  per-node model — NOT a global "defer everything in a template" flag.
  `dependent_parse_in_progress` is now only the "am I in a dependent parse" gate. Before
  widening eligibility to bodies that MIX dependent + non-dependent template-id calls,
  finish widening per §11.5c (typeless placeholder + ADL bit + real `is_type_dependent`).
- The env-gated flag-on byte-identical harness is THE validator. Keep it until the re-parse
  path is deleted (PLAN §5 trigger).

## 7. WIDENING BACKLOG (after scalar consumption works) — PLAN §11.5c
1. typeless placeholder for a deferred call (madc `unknown_type_node` analogue) + ADL bit.
2. real `is_type_dependent(expr)` predicate from operand types; at tsubst re-run the NORMAL
   call-resolution entry on concrete args (reuse, don't fork) — retires the flag.
3. then dependent member access (`T::x`), template-ids (`Foo<T>`), packs — one per commit,
   each relaxing a `tsubst_eligible` constraint with the gate green.
