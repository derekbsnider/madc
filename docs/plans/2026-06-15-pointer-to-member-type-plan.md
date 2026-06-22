# Plan — Pointer-to-member type (`int C::*`), staged

**Status:** design (not yet started). **Branch:** `feature/retire-embedded-shims-claude`
(local-only). **Driver:** the shared wall now blocking 5 of the 12 retire-embedded-shims
failures — set/map/subscript/containerdtor/madc_ns stall at `bits/stl_pair.h:188` on a
pointer-to-data-member parameter type. **Approach:** implement a real pointer-to-member
type at the declarator/type layer (deepest fix). There is no skip/defer alternative on the
table — a parse-skip heuristic is a shim and is not considered.

---

## 1. Recon (done 2026-06-15; gcc 13 + clang both canon)

The exact construct (`bits/stl_pair.h:613`, a PRIVATE nested helper of `std::pair`):

```cpp
struct __zero_as_null_pointer_constant {
    __zero_as_null_pointer_constant(int __zero_as_null_pointer_constant::*) { }   // <-- ptr-to-DATA-member param
    template<typename _Tp, typename = __enable_if_t<is_null_pointer<_Tp>::value>>
    __zero_as_null_pointer_constant(_Tp) = delete;
};
```

Three reducers, **g++ and clang accept all three identically** (`tmp/ptm{1,2,3}.cpp`):

| Reducer | Shape | madc today |
|---|---|---|
| ptm1 | `struct Z { Z(int Z::*) {} };` — the type in a param | FAIL `Failed to find type when parsing function parameters` (parser.cpp:30169) |
| ptm2 | `int C::*p = &C::y; c.*p; pc->*p;` — data-member-ptr USE | FAIL `Expecting name in qualified declarator` (parser.cpp:28453) |
| ptm3 | `int (C::*pf)(int) = &C::f; (c.*pf)(41);` — member-FUNCTION-ptr | FAIL `Expecting ')' after parenthesized declarator` (parser.cpp:31873) |

madc has **zero** pointer-to-member support: no `.*`/`->*` tokens, no member-pointer
DataDef category (`BaseType` = `btSimple|btStruct|btFunct|btClass`, datadef.h:37), no
`&C::m` constant.

**Two facts that shape the scope:**

1. **None of the 5 blocked tests USE pointer-to-member** (`grep -c '::\*|\.\*|->\*'` = 0 in
   all 5). They only need `std::pair` to *parse*. So the wall needs **Stage 1 only** (the
   data-member-pointer *type* must parse + have a representation) — not the operator surface
   and not member-function-pointers.
2. **A coupled second wall likely sits right behind it.** The deprecated DR-811 ctors
   immediately after are `pair(_U1&&, __zero_as_null_pointer_constant, ...)` — a trailing C
   ellipsis in a C++ ctor. madc mishandles even `struct V { V(int a, ...) {} }; V v(7);`
   (`no matching constructor for V(int)` — confirmed, tmp/varargctor). Whether it bites
   depends on parse-vs-instantiate of the un-called template ctor; Stage 1 must verify and,
   if needed, fold in faithful trailing-`...` handling.

## 2. Lowering model (Tier-1 — madc owns it; c2mir/MIR have no pointer-to-member)

Per `.claude/rules/lowering-vs-raising.md`, a pointer-to-member has a faithful C11 form, so
it lowers in madc (no fork change), keeping `--emit=c11` portable. Match the **Itanium C++
ABI** so emitted C and any cross-TU use agree:

- **Pointer-to-DATA-member** = a `ptrdiff_t` byte offset into the object; the **null** value
  is `-1` (Itanium), not `0`. Lowerings:
  - `&C::m`      → `(ptrdiff_t)offsetof(C, m)`
  - `obj.*pm`    → `*(T*)((char*)&obj + pm)`
  - `p->*pm`     → `*(T*)((char*)p   + pm)`
  - `pm == nullptr` → `pm == -1`
- **Pointer-to-MEMBER-FUNCTION** = Itanium 16-byte struct `{ fnptr-or-(vtable_byte_offset+1);
  ptrdiff_t this_adjustment }` (low bit of field 1 = "virtual"); calling resolves virtual
  vs non-virtual + applies `this` adjustment. This is the hard part — **Stage 3, deferred**.

## 3. Type representation

Add a dedicated `DataDefMemberPtr` (a `DataDef`) carrying:
- `owner_class` (the `C` in `T C::*`),
- `pointee` (the member type `T`, or a function type for member-fn-ptrs),
- `is_function_member` flag.

`sizeof` = 8 for data-member-ptr, 16 for member-function-ptr. It is its own category — the
`_type` +10000/+20000 pointer/reference offset scheme (datadef.h:267-276) models ref/pointer
LEVELS and is orthogonal; do not overload it. Member pointers are C++-only → gate behind the
`--std=` C++ floor (the keyword/feature registry), per invariant I-no-hardcoded-standards.

## 4. Stages

> **STAGE 1 LANDED** (commit on `feature/retire-embedded-shims-claude`). The
> param/ctor-SIGNATURE context is done (`DataDefMemberPtr` + a syntactic `<name>::*`
> hook at `parseFunction`'s `grabnt`, recognized WITHOUT resolving the owner — that is
> what made the template-nested stl_pair helper work). +tests/testptmtype, 3-oracled.
> fulltest 602→603, zero regressions. **Result:** all 5 ptm-blocked tests advance past
> stl_pair.h:188 — testset now parses the whole `<set>` header and reaches its OWN C++20
> `set::contains` use (testset.mad:19); map/subscript/containerdtor/madc_ns hit a NEW
> deeper header wall at `:102 "Expecting member name in class definition"`; the bare
> `set<int>` reducer hits `:64 "Missing operand"`. **No flip yet** (deeper, unrelated
> walls — the DR-811 vararg ctor did NOT turn out to be the next wall). The typedef /
> variable-declarator / member contexts still raise a clean parse error ("Expecting
> identifier after '::'" / "Expecting name in qualified declarator") and move to Stage 2
> (a pointer-to-member VALUE is only useful with `&C::m`/`.*`). NEXT walls (`:102`,
> `set::contains`) are NEW features, not pointer-to-member — re-triage before continuing.

### Stage 1 — data-member-pointer TYPE (unblocks the 5 tests)
- **Parse** `T C::*` as a pointer-to-member declarator. Hook points:
  - `parse_qualified_declarator_part` (parser.cpp:28438): after a nested-name-specifier `C::`,
    a `*` (optionally `* const`/`* volatile`) is a pointer-to-member declarator, not a name.
  - function-param declarator (parser.cpp ~30140-30170) and parenthesized declarator
    (parser.cpp:31873) reach the same recognition.
- **Represent** via `DataDefMemberPtr` (data case, 8-byte offset); `sizeof` correct; lowers to
  `ptrdiff_t` in emitted C11.
- **Acceptance:** ptm1 parses; the 5 container tests advance. If the DR-811 trailing-`...` ctor
  is the next wall, fold in faithful C++ trailing-ellipsis handling (real gap, tmp/varargctor)
  — coupled sub-item, fixed at the deepest layer, not skipped.
- **Gate:** 3-oracle ptm1 (g++/clang/madc); `make -C src fulltest` zero-regression; the 5
  tests are the signal. +`tests/testptmtype` (a data-member-ptr type in a param/typedef that
  parses and the program runs).

### Stage 2 — data-member-pointer USE
- New operator tokens `tkDotStar` (`.*`) and `tkArrowStar` (`->*`), gated by `--std=` C++ floor.
- `&C::m` address-of-data-member constant → `offsetof`-style lowering.
- `.*` / `->*` lowering (§2); null member-ptr (`-1`) + equality comparisons.
- **Gate:** 3-oracle ptm2; +`tests/testptmdata` (run, values via cout); full fulltest.

### Stage 3 — member-FUNCTION-pointers (DEFERRED; LARGE)
- `R (C::*)(args)` type, `&C::f`, `.*`/`->*` *call*, the Itanium 16-byte representation +
  virtual dispatch (vtable-offset encoding + `this`-adjustment). Recon of the exact ABI layout
  is TODO at the time this stage starts.
- **Not needed by any of the current 12** — do it when a real consumer appears. 3-oracle ptm3.

## 5. Risks / notes
- **DR-811 trailing-`...` ctor** — the most likely Stage-1 follow-on wall; budget for it.
- **Itanium null = -1** for data-member-ptr (a 0 offset is a valid first member) — get this
  right or `== nullptr` breaks.
- **Member-fn-ptr ABI** is the genuinely large piece — explicitly deferred to Stage 3.
- One IR, one emitter: all lowering flows through the cir_node → c2mir path so `--emit=c11`
  stays portable; no special-casing, no hardcoded specifics (invariants I6/I-std).

## 6. Sequencing
Stage 1 (+ the coupled vararg fix if it surfaces) is the count-dropper — it should flip
set/map/subscript/containerdtor/madc_ns (≈5 of 12). Stages 2–3 are correctness completeness
with no current test consumer; land Stage 1 under the full gate first, then decide whether
Stage 2 is worth doing immediately or after the other clusters (`__is_assignable` arity-2,
the vector member-ref pair, for_each MIR-link).
