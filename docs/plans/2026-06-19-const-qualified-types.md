# Const-qualified type identity — campaign plan (2026-06-19)

> **DECISION (SETTLED, user-chosen, no-shims).** madc will gain a first-class
> const-qualified type identity (`DataDefCONST`), mirroring `DataDefREF`. This was
> chosen over the narrow "preserve the const spelling through deduction" hack
> because that hack is a shim (it papers over the missing type identity at one call
> site and would have to be repeated everywhere const matters). Do NOT implement the
> spelling hack. See docs/plans/2026-06-19-map-instantiation-strategy.md (sub-wall B)
> for why this is needed: `map<int,int>`'s pair piecewise ctor `__o7` has param
> `tuple<int>` but the call passes `tuple<const int&>`, so `select_ctor_overload`
> rejects it. Root cause: madc has no const-qualified type, so `const int` ≡ `int`.

## Why structural (the precedent)

`DataDefREF` (datadef.h:958) is the exact model: a thin qualified-type that IS-A its
lowering (DataDefPTR), keeps the same name/size/DataType, but answers
`is_reference()` true so consumers recover the canonical `T&`. Reference identity is
"in the type, no parallel flags" — the first-class-references campaign
([[project_retire_embedded_shims]]) retired three side-channels into the one
`DataDefREF`. Const is the same shape: a qualifier that must live IN the type, not
as a parse-time-only flag (`ParsedParamSig::is_const`, madc.h:1028, is the only
place const exists today, and it's dropped after parse).

## The model: `DataDefCONST`

```cpp
// const-qualified T. IS-A its base's lowering (const has NO runtime/ABI effect —
// same size, same DataType, same codegen), but is_const() is true so type identity
// and spelling carry `const`. base_type is the unqualified T.
class DataDefCONST : public DataDef {
public:
    DataDef *base_type;
    DataDefCONST(DataDef &base)
        : DataDef("const " + base.name, base.size, base.type()), base_type(&base) {}
    virtual bool is_const() const { return true; }
    // Transparent forwarding so every site that does NOT test is_const() treats it
    // exactly like base (the DataDefREF discipline):
    virtual bool is_pointer()   const { return base_type->is_pointer(); }
    virtual bool is_reference() const { return base_type->is_reference(); }
    virtual bool is_numeric()   const { return base_type->is_numeric(); }
    virtual bool is_integer()   const { return base_type->is_integer(); }
    // ... forward the rest of the predicate surface base relies on.
};
```
`DataDef::is_const()` defaults false (new virtual, datadef.h ~214 next to
`is_reference()`). `Program::getConstType(DataDef*)` with a `const_type_cache`
(mirror `ref_type_cache`, madc.h:1617). `const const T` folds to `const T`
(idempotent); `getConstType` of an already-const returns it unchanged.

Ordering with REF/PTR: `const T&` = reference-to-(const T) = `DataDefREF(getConstType(T))`;
`const T*` = pointer-to-(const T) = `DataDefPTR(getConstType(T))`; `T* const` =
const-(pointer-to-T) = `getConstType(DataDefPTR(T))`. Keep `getConstType` applying to
the *pointee/referent* the way C++ means it — the parser decides which by where the
`const` sits relative to `*`/`&`.

## Phases (each ends GREEN: `make -C src fulltest` == 656/6, then commit)

- **Phase 1 — FOUNDATION (additive, no behavior change). ✅ DONE @ d100143**
  (fulltest 656/6, pushed on wip/tuple-instantiation-claude). Added `DataDefCONST`
  (datadef.h), `DataDef::is_const()` virtual (default false), `Program::getConstType`
  + `const_type_cache` (parser.cpp). Nothing constructs a `DataDefCONST` yet →
  identical behavior. DO NOT redo this phase. Resume at Phase 2.

- **Phase 2 — PRODUCE at the const-on-a-type sites.** Where the parser consumes a
  leading/trailing `const` on a TYPE (the 56 `tkCONST` sites in parser.cpp — audit
  them; many are on declarations/methods, NOT type identity), wrap the resulting
  DataDef in `getConstType`. Do this INCREMENTALLY and gated: start with the
  template-argument / function-parameter type path (the map-relevant one), behind
  `#ifdef FEATURE_CONST_TYPES`, so the blast radius is controlled. Expect churn:
  every `const int` becoming a distinct type can shift overload resolution, name
  mangling, and `==`-on-DataDef comparisons. Fix each regression at its root (a
  consumer that should be const-transparent but tests identity).

  **Producer #2 LANDED 2026-08-29 (s145, C mode, UNGATED)**: the C
  declaration grammar mints pointee-const — `consume_declarator_stars`
  wraps a pending low-level const via `getConstType` before each
  `getPointerType` derivation (`const char *` ≠ `char *`; c-testsuite
  00219, gate `testconstpointee`). Gated to `is_c_mode()`: C has no
  overloads/mangling/templates, so the blast radius is the C lanes
  (c-testsuite 220/220 green). Came with two transparency pieces the C++
  flip will reuse: `DataDefCONST` forwards ALL `as_*_dd()` accessors
  (predicates already forwarded — inconsistent views SIGSEGV'd member
  access), and the two member-access chokepoints (arrow pointee
  extraction, dot struct_type) peel `unqualified()`. The C++ producer
  remains Phases 3/4. Write-rejection through a const pointee is still
  a P4 residue (madc accepts stores gcc rejects — diagnostics gap, not
  a wrong-value gap).

- **Phase 3 — THREAD through deduction + instantiation naming.**
  `resolve_arg_spelling_datadef` (parser.cpp:15041) must RE-APPLY const (today it
  peels cv at 15067-77 and drops it) → return `getConstType(base)`. Tid-pack element
  resolution (parser.cpp:30594) then yields `const int&` and `tuple<_Args1...>`
  instantiates+names with const, matching the call arg's `tuple_const_int32_t_`.

- **Phase 4 — CONST-TRANSPARENCY sweep.** Mirror the DataDefREF discipline: audit
  every `dynamic_cast<DataDefX*>`, `->type()==`, and name-equality on DataDef to make
  sure a `DataDefCONST` is unwrapped (via a `strip_const()`/`base_type` helper) where
  const is irrelevant (codegen, size, most arithmetic), and respected where it
  matters (overload ranking, `const` method/param matching, top-level-const
  discard per [over. load]/[dcl.fct]). C++ rule: top-level const on a by-value
  PARAMETER is ignored for signatures — handle that so `void f(const int)` and
  `void f(int)` stay the same overload.

- **Phase 5 — RETIRE `ParsedParamSig::is_const`** where it now duplicates the type
  (the no-parallel-flags endgame), keeping only genuinely parse-local uses.

## Risk + discipline

- Const is pervasive; Phase 2 WILL surface regressions. Each one is a consumer that
  conflated `const T` with `T` — fix at that consumer (make it const-transparent or
  const-aware), never by suppressing the `DataDefCONST`. That is the no-shims rule.
- Top-level-const param discard ([over.load]/p3.1) and `const`-pointee vs
  `const`-pointer are the two correctness traps; get them right in Phase 4.
- Gate Phase 2+ work behind `#ifdef FEATURE_CONST_TYPES` so a half-done phase never
  ships a regression; remove the guard when the phase is green ([[feedback_finish_plans_fully]]).

## Why this finally lands map (the payoff)

With const in the type: `forward_as_tuple(const int&)` → `tuple<const int&>` and the
piecewise ctor's deduced `tuple<_Args1...>` → `tuple<const int&>` produce the SAME
type/name → `select_ctor_overload` selects `__o7` → (with sub-wall A, the out-of-line
body attach) map's `m[1]=2` construction binds. Const-qualified types also unblock
the broader C++ correctness surface (const-correct overloads, `const_iterator`,
const member matching) far beyond map.
