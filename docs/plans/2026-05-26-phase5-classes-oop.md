# Phase 5 Design: Classes & OOP

**Date:** 2026-05-26
**Scope:** Class mechanics in the transpiler — contextual identifiers, ctors/dtors, new/delete, inheritance, operator overloading, virtual dispatch
**Baseline:** 302/475 transpiler tests passing (63.6%)
**Target:** 10 additional tests (312/475)

## Problem

12 class/OOP transpiler tests fail. 2 are STL containers (deferred to
Phase 6). The remaining 10 cover core class mechanics that the emitter
doesn't handle yet.

## In-Scope Tests

| Test | Root Cause |
|------|-----------|
| testaddrclass | Gecko rejects `class` as identifier |
| testclassident | Gecko rejects `class` as identifier |
| testclassidentifier | Gecko rejects `class` as identifier |
| testenumclass | Gecko rejects `class` as identifier |
| testmapidentifier | Gecko rejects `map` as identifier |
| testnew | new/delete wrong type name + wrong indirection |
| testctorearly | Missing braces around dtor+return block |
| testinherit | Missing base cast + base dtor in chain |
| testoperover | Operator overloads silently dropped |
| testvirtual | No vtable field, call sites wrong |

## Design

### 1. Contextual identifiers (grammar fix)

Add grammar alternates so `class`, `map`, `set`, `vector`, `list` can
appear as plain identifiers in non-declaration contexts: struct members,
variable names, enum values, parameters. The legacy parser has
`is_contextual_identifier_token()` for this — the Gecko grammar needs
equivalent rules.

### 2. Constructor/destructor + new/delete

**Ctor/dtor naming:** Use Itanium mangled names from `madc_mangle.h`:
- `Foo::Foo(int)` → `_ZN3FooC1Ei`
- `Foo::~Foo()` → `_ZN3FooD1Ev`

**new expression:** `new Foo(w, h)` emits:
```c
struct Foo *p = (struct Foo *)malloc(sizeof(struct Foo));
_ZN3FooC1Eid(p, w, h);  // mangled ctor
```

**delete expression:** `delete p` emits:
```c
_ZN3FooD1Ev(p);  // mangled dtor
free(p);
```

**Dtor+return bracing:** When emitting destructor calls before a return
inside a conditional, always wrap in `{ dtor_call; return expr; }`.

### 3. Inheritance

**Struct layout:** Derived struct includes all base fields at the start
(already done by sema). This makes `(struct Base*)derived_ptr` valid
because the base subobject is at offset 0.

**Base method calls:** When calling a base-class method on a derived
pointer, emit `(struct Base*)` cast:
```c
_ZN6Animal8get_legsEv((struct Animal*)&d)
```

**Base destructor chain:** Derived destructor calls base destructor
after its own cleanup:
```c
void _ZN3DogD1Ev(struct Dog *this) {
    printf("Dog destructed\n");
    _ZN6AnimalD1Ev((struct Animal*)this);  // base dtor
}
```

**Multiple inheritance planning:** Primary base at offset 0 (Itanium ABI
layout). Secondary bases at computed offsets. Thunks adjust `this`
pointer for secondary base virtual calls. Not implemented in Phase 5
but the struct layout follows the convention so MI doesn't require
rearchitecting later.

### 4. Operator overloading

**Operator method emission:** Operator methods are emitted as regular
functions with Itanium-mangled operator names:
- `operator==` → `eq` → `_ZN7CountereqEi`
- `operator!=` → `ne` → `_ZN7CounterneEi`
- `operator<`  → `lt` → `_ZN7CounterltEi`
- `operator>`  → `gt` → `_ZN7CountergtEi`
- `operator+`  → `pl` → `_ZN7CounterplEi`
- `operator-`  → `mi` → `_ZN7CountermiEi`

**Call site rewriting:** When a binary operator's LHS is a class type
with that operator defined, emit the function call instead of the raw
operator:
```c
// c == 42  →  _ZN7CountereqEi(&c, 42)
```

This requires extending `madc_mangle.h` with `itanium_mangle_operator()`.

### 5. Virtual dispatch

**Vtable struct:** For each class with virtual methods, emit a vtable type:
```c
struct Foo_vtable {
    void (*method1)(struct Foo *);
    int (*method2)(struct Foo *, int);
};
```

**Vptr field:** Add `struct Foo_vtable *__vptr;` as the first field in
the class struct.

**Vtable instance:** Emit a static vtable for each concrete class:
```c
static struct Shape_vtable Circle_vtable_instance = {
    (void (*)(struct Shape *))_ZN6Circle4areaEv,
    // ...
};
```

**Ctor initialization:** Constructor sets `this->__vptr = &ClassName_vtable_instance;`.

**Virtual call sites:** `obj.method(args)` on a class with virtuals emits:
```c
obj.__vptr->method(&obj, args)
```

## Mangler extensions needed

Add to `madc_mangle.h`:
```cpp
// Mangle an operator: ("Counter", "==", {"int"}) → "_ZN7CountereqEi"
std::string itanium_mangle_operator(const std::string &class_name,
                                     const std::string &op,
                                     const std::vector<std::string> &param_types);
```

With operator name mapping:
```
==  → eq    !=  → ne    <   → lt    >   → gt
<=  → le    >=  → ge    +   → pl    -   → mi
*   → ml    /   → dv    %   → rm    =   → aS
+=  → pL    -=  → mI    <<  → ls    >>  → rs
[]  → ix    ()  → cl    new → nw    delete → dl
```

## Files modified

| File | Change |
|------|--------|
| `src/madc_grammar.cpp` | Contextual identifier alternates |
| `src/madc_tokenizer.cpp` | Token remapping for contextual idents |
| `src/madc_emit_c.cpp` | Ctor/dtor emission, new/delete, inheritance casts, operator dispatch, vtable, dtor bracing |
| `src/madc_mangle.cpp` | Add `itanium_mangle_operator()` |
| `include/madc_mangle.h` | Operator mangling API |
| `tests/unit/test_mangle.cpp` | Operator mangling tests |

### 6. Allocation mode via `prefer` keyword

`prefer` is an existing madc keyword (used for namespace preference
ordering: `prefer php, perl;`). Extend it to accept `calloc`:

```c
prefer calloc;   // zero-init allocations from this point forward
```

Default is `malloc()` for speed (priority #1). When `prefer calloc;`
is active, `new Foo(args)` emits `calloc(1, sizeof(struct Foo))`
instead of `malloc(sizeof(struct Foo))`. This ensures uninitialized
members are zero — useful for debugging and structs with optional fields.

The grammar already has `PREFER IDENT ';'` — `calloc` is just a new
recognized identifier in that production. The sema/emitter tracks it
as a boolean flag (`prefer_calloc`).

## Out of scope

- STL containers (map, set, vector, list) — Phase 6
- Multiple inheritance implementation — planned layout only
- Template class mangling — Phase 6+
- Exception handling in class contexts — Phase 6
