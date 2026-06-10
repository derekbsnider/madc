# Canon research — how g++/clang model alias-spelled reference returns

Task 1 of `docs/plans/2026-06-10-testfstream-alias-reference-plan.md`.
Recorded 2026-06-10 against libstdc++ 13, clang++ 14-era AST dump, g++ 13.
Reducers: `tmp/canon1.cpp` (AST), `tmp/canon2.cpp` (codegen), `tmp/canon3.cpp`
(full testfstream chain).

## 1. clang AST — the alias is sugar; the reference lives in the canonical type

`basic_string<char>::operator[]` declaration (`bits/basic_string.h:1254`),
as instantiated:

```
CXXMethodDecl ... operator[] 'reference (size_type)' implicit_instantiation
```

The *spelling* of the return type stays `reference` (a class-scope alias
chaining `__gnu_cxx::__alloc_traits<allocator<char>>::reference` →
`value_type&` → `char&`). The call sites in `main` show where the truth
lives:

```
char c = s[1]:
  ImplicitCastExpr 'value_type':'char' <LValueToRValue>
    CXXOperatorCallExpr 'value_type':'char' lvalue '[]'
      DeclRefExpr 'reference (size_type)' lvalue CXXMethod 'operator[]'

char *p = &s[1]:
  UnaryOperator 'value_type *' prefix '&'
    CXXOperatorCallExpr 'value_type':'char' lvalue '[]'
```

Reading:

- In clang, a typedef/alias is a **sugar layer over a canonical type**
  (`TypedefType` → desugars to `char &`). The reference qualifier is part of
  the canonical type — it is **not** a property of the declarator spelling.
- A call to a function whose canonical return type is `T&` produces an
  expression of type `T` with value category **lvalue** (C++ expression
  types are never references; the reference-ness becomes lvalue-ness).
- Use-as-value inserts `LValueToRValue` — the deref-at-use.
- `&<lvalue>` applies directly, yielding `T*` — no extra machinery.

## 2. g++ codegen — T& = address in %rax + deref-at-use

`tmp/canon2.cpp`, `g++ -std=c++17 -O0 -S -fverbose-asm`:

```
char get(std::string &s) { return s[1]; }
        call    _ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEixEm@PLT
        movzbl  (%rax), %eax          # deref the returned address

char *addr(std::string &s) { return &s[1]; }
        call    _ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEixEm@PLT
        # %rax used directly — no deref
```

This is exactly madc's existing `returns_ref` lowering (a `T&` return
travels as `T*`; consumers deref at use, address-of consumes the pointer
as-is). The Itanium symbol does not encode the return type (`ixEm` carries
only params), which is why the alias-dropped `&` is silent until the value
is used.

## 3. testfstream chain in canon

`tmp/canon3.cpp` (the rewritten test body) compiles and prints
`hi / 42 / 12345 / 2` identically under **both** `g++ -std=c++17` and
`clang++ -std=c++17`.

Symbol classification against `libstdc++.so.6` (`nm -D`):

| symbol | exported? | consequence for madc |
|---|---|---|
| `…9to_stringB5cxx11…` / `…4stoi…` | **0** hits | inline — madc must compile the header bodies (this is why `&__str[__neg]` inside `to_string`'s own body at `basic_string.h:4171` hits the alias-ref wall) |
| `…7getline…` | 12 hits | external — binds mangled-direct (already works) |
| `basic_string::operator[]` (`…ixEm`) | 9 hits | external — binds mangled-direct; **only the return-type reference-ness is madc's job** |

`stoi`'s body dependency: it calls
`__gnu_cxx::__stoa<long, int>(&std::strtol, "stoi", …)` — a function
template in `ext/string_conversions.h` whose body madc must instantiate
(Task 3 verifies at runtime).

## 4. The model madc must mirror

> **An alias is a type, not a spelling.** Resolution of any type spelling —
> including class-scope aliases and template-dependent alias chains — must
> produce (DataDef base, pointer depth, IS-REFERENCE) as one unit. The
> reference qualifier is part of the resolved type; a declarator-level `&`
> token is only ONE way a type acquires it.
