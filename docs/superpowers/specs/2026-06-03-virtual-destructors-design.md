# Virtual Destructors — Design Spec

**Date:** 2026-06-03
**Track:** C++ parser-correctness, Stage C (the design-first gap #1 from
`docs/superpowers/plans/2026-06-03-cpp-parser-correctness.md`).
**Branch:** `feature/cpp-virtual-dtors-claude`, cut from `feature/retire-std-hardcoding-claude`
HEAD (`0c11549`, which carries MI S1–S5 + parser-correctness Stages A/B).
**KG:** Feature `Virtual Destructors`; deferred items tracked as Gaps
`pure_virtual_and_abstract_classes` (MEDIUM) and `explicit_pseudo_destructor_call` (LOW),
both `HAS_GAP` from the feature.

## Goal

`virtual ~X()` makes a class polymorphic. `delete base_ptr` dispatches to the
most-derived destructor through the vtable. `typeid` / `dynamic_cast` work on a
destructor-only polymorphic base. The emitted vtable and behavior match
`g++ -std=c++17` byte-for-byte.

## The gap (root cause)

`class W { virtual ~W(); };` does not set `has_vtable`. `is_virtual` is parsed at
`src/parser.cpp:11993` but the **destructor branch (`12004`) ignores it**; only the
virtual-*method* path (`12218`) registers a slot and sets `has_vtable`. Consequences:
a destructor-only base is non-polymorphic, so `typeid`/`dynamic_cast`/virtual
dispatch on it take wrong paths and `_ZTI` is referenced but never emitted; and
`delete base_ptr` always calls the **static** type's destructor
(`src/cir_builder.cpp:5055`), never the derived one.

## Ground truth (g++ vtable, pinned 2026-06-03)

For `struct A { virtual void f(); virtual ~A(); virtual void g(); };`,
`g++ -fdump-lang-class` emits:

```
Vtable for A  (_ZTV1A): 6 entries
  0   offset_to_top = 0
  8   & _ZTI1A
  16  A::f          slot 0
  24  A::~A  (D1)   slot 1   complete-object destructor   _ZN1AD1Ev
  32  A::~A  (D0)   slot 2   deleting destructor          _ZN1AD0Ev
  40  A::g          slot 3
```

Two facts this design depends on:

1. The destructor occupies **two consecutive vtable slots** — D1 (complete-object)
   then D0 (deleting) — at the destructor's **declaration-order position** among the
   virtual functions (NOT forced to the front).
2. Both entries demangle to `A::~A`; they differ only by ABI variant (D1 vs D0).

## Decision: faithful D1/D0 two-slot layout

Chosen over a simplified single-slot scheme. Rationale: it matches g++
byte-for-byte and stays interop-correct if a madc user class ever derives a
libstdc++ polymorphic type (e.g. `class E : public std::exception`), whose vtable
expects the deleting destructor at the D0 slot. This is the same faithfulness
rationale that made the S5 RTTI prologue the real enabler. The cost is one
synthesized D0 wrapper per polymorphic class and a changed `delete` lowering.

## madc's existing destructor model (reused, not rebuilt)

- `Cls___dtor` — base destructor: runs member + non-virtual-base destructors.
- `Cls___dtor_complete` (`class_complete_dtor_symbol`, `cir_builder.cpp:3600`) —
  complete-object destructor: base dtor + virtual-base dtors, run once. This IS the
  Itanium D1.
- `synth_complete_dtor_def` (`cir_builder.cpp:3645`) emits `Cls___dtor_complete`.
- new/delete use `calloc`/`free`; so "operator delete" for a user class is `free`.
- Stack objects, by-value members, and explicit scope-exit destruction already call
  the **non-virtual** complete destructor of the statically-known type. That is
  correct (static type == dynamic type there) and is UNCHANGED by this work.

## Components

### 1. Parser — register the destructor's two vtable slots

`src/parser.cpp`, destructor branch (~12004). When `is_virtual` (or any base
destructor is virtual — inherited virtuality), register **two** consecutive slots at
the destructor's declaration position, mirroring the virtual-method path (~12218):

- Slot names are **class-name-independent fixed markers**: `"~"` (D1) and
  `"~$deleting"` (D0). They must NOT embed the class name, because base and derived
  share the destructor's vtable position — a derived class inherits the base's
  destructor slots (see below) and an override must reuse the SAME slot, not add a new
  one. Neither marker can collide with a real method name (a source identifier is
  never just `~` and never contains `$`).
- Push both markers into `vtable_slots` **only if not already present** (so an
  inherited destructor slot is reused, not duplicated), set `has_vtable = true`, and
  record a `has_virtual_dtor` determination on the class.
- Inherited virtuality: a base's two destructor slots are already copied into the
  derived `vtable_slots` by the base-merge loop at `11952`. A derived class with its
  own (implicit or explicit) destructor finds the `"~"`/`"~$deleting"` markers already
  present and simply re-resolves them to its own D1/D0 symbols (see §2) — that is the
  override. A class is treated as having a virtual destructor if it declares
  `virtual ~X()` OR any base has a virtual destructor.

This also makes `vtable_slots` non-empty for a destructor-only class, so the vtable
emits (§2) and `_ZTI` becomes reachable — fixing the original typeid/dynamic_cast
symptom.

### 2. Vtable emission — emit D1/D0 symbols for the destructor slots

`src/cir_builder.cpp`, `class_vtable_def` slot loop (~2944). The slot value currently
comes from `findMethod(slot_name)`. Add, BEFORE the `findMethod` lookup: when the
slot name is the D1 marker (`"~"`), emit `class_complete_dtor_symbol(cdd)`; when it is
the D0 marker (`"~$deleting"`), emit the new deleting-destructor symbol
`cdd->name + "___dtor_deleting"` (§3). Both use the **current** `cdd` (the class whose
vtable is being emitted), NOT the marker's text — so a derived class's table resolves
the inherited markers to the DERIVED D1/D0 symbols, which is exactly the virtual
override.

- **Secondary MI groups** (`this_offset != 0`): both destructor slots get the
  existing this-adjusting `make_thunk` treatment, exactly like an overridden method
  reached through a secondary-base vptr. Pin the thunked layout against
  `g++ -fdump-lang-class` for a class with a secondary polymorphic base that has a
  virtual destructor.
- The `class_vtable_def` early-return `if (... vtable_slots.empty()) return NULL;`
  (line 2870) now naturally allows a destructor-only class through (its slots are the
  two destructor entries), emitting the prologue + D1/D0.

### 3. Deleting-destructor synthesis (D0)

New emitter beside `synth_complete_dtor_def` (`cir_builder.cpp:3645`), deduped like
the existing complete-dtor emission (the `emitted_complete_dtors` set at ~8050):

```
void Cls___dtor_deleting(struct Cls *p) { Cls___dtor_complete(p); free(p); }
```

Emit it for every polymorphic class with a virtual destructor (i.e. whose vtable
carries a D0 slot). `free` is madc's operator-delete for user classes.

### 4. `delete` — virtual dispatch through the D0 slot

`src/cir_builder.cpp`, `TokenDELETE` lowering (~5055). Branch on whether the static
type `cdd` has a **virtual** destructor:

- **Virtual destructor:** lower to a virtual call through the D0 slot and **drop the
  separate `free`** (D0 frees):
  `(*(void(**)(void*)) ((void**)((char*)p)[0])[<D0 addr_point + slot index>])(p)`
  — read the vptr at offset 0, index to the D0 slot, call with `p`. The D0 addr point
  / slot index comes from `vtable_group_slot` (the same lookup virtual method calls
  use). Evaluate `p` once (statement-expression temporary) since it is used only in
  the dispatch.
- **Non-virtual destructor (today's case):** unchanged — `Cls___dtor_complete(p); free(p)`.
- **No class / no user destructor:** unchanged — `free(p)` only.

Blast radius is minimal: only `delete ptr` where `ptr`'s static type has a virtual
destructor changes.

## Data flow

`virtual ~X()` → parser registers D1/D0 slots + `has_vtable` → `compute_layout` /
`build_vtable_groups` lay out the two slots (S5 prologue already handled) →
`class_vtable_def` emits D1 = `___dtor_complete`, D0 = `___dtor_deleting` (thunked in
secondary groups) → `synth` emits the D0 wrapper → `delete base_ptr` reads the vptr,
indexes the D0 slot, calls it (which frees). RTTI (`_ZTI`/typeid/dynamic_cast) works
because the table now exists for any class with a virtual destructor.

## Error handling / edges

- **Pure-virtual destructor** `virtual ~X() = 0;` — OUT OF SCOPE. madc does not parse
  pure-virtual `= 0` at all (verified 2026-06-03: "Expecting brace after function
  declaration" at the `= 0`). That is a separate "abstract classes" feature
  (KG Gap `pure_virtual_and_abstract_classes`); pure-virtual destructors depend on it.
- **Explicit pseudo-destructor call** `p->~X()` — DEFERRED (rare; placement-new /
  allocator code). KG Gap `explicit_pseudo_destructor_call`. When implemented it must
  dispatch virtually to the D1 (complete-object) slot.
- **Implicit (compiler-generated) virtual destructor:** if a class declares no
  destructor but inherits a virtual one, it still needs D1/D0 slots resolving to its
  own implicit complete destructor. Covered by the inherited-virtuality path (§1) +
  the existing implicit-complete-dtor machinery.

## Testing

Every behavior is matched to `g++ -std=c++17` output (g++ is canon):

1. **Virtual delete dispatch** (`tests/test_vdtor_delete.mad`): base pointer to a
   derived object; `delete bp` must run the derived destructor then the base
   destructor (print-order proof) — compare to g++.
2. **Destructor-only polymorphic base** (`tests/test_vdtor_rtti.mad`): a class whose
   only virtual is `~Base()`; `typeid(*bp).name()` and a `dynamic_cast` downcast must
   work (this is the literal gap #1 unblock). Compare to g++.
3. **MI secondary-base virtual destructor** (`tests/test_vdtor_mi.mad`): `delete`
   through a pointer to a secondary polymorphic base; correct destructor runs with
   correct `this`-adjustment. Compare to g++.
4. **Vtable layout doctest** (`tests/unit/test_class_layout.cpp` addition): assert the
   D1/D0 slot pair sits at the destructor's declaration-order position and that
   `has_vtable` is set for a destructor-only class — mirroring the g++ dump above.
5. **No-leak / no-double-free**: the virtual `delete` path frees exactly once (D0
   frees; the separate `free` is dropped). A run under the existing test harness
   plus an explicit alloc/free-count check.

**Gate per task:** build 0-warn → `bash scripts/run_tests.sh` (baseline 471 pass; the
7 documented fails incl. flaky `testfortypedcomma`; must not grow) → no-std
finish-line 468 (unchanged) → SMAUG soak (virtual destructors do not appear in C89
SMAUG, so risk is low, but soak anyway since vtable/delete codegen changed) → commit.
Coordinator independently re-verifies and owns the SMAUG check.

## Out of scope (tracked)

- Pure-virtual destructors / abstract classes — KG `pure_virtual_and_abstract_classes`.
- Explicit pseudo-destructor calls `p->~X()` — KG `explicit_pseudo_destructor_call`.
- Custom per-class `operator delete` (sized/placement) — user classes use `free`.
