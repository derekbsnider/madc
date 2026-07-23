# Cfront Template Instantiation Research

## Key Finding: Monomorphize, Don't Repository

Cfront 3.0 (1991) used a "repository model" — try to link, parse linker
errors for undefined template symbols, generate the missing instantiations,
retry. This was universally regarded as slow and fragile. GCC docs call it
"vastly increased complexity." It was abandoned.

Modern compilers (GCC, Clang) use the **Borland model**: each translation
unit independently emits all template instantiations it uses. Duplicates
are collapsed by the linker via COMDAT sections / weak symbols.

**For madc's transpiler:** Use the Borland model — monomorphize per
translation unit during transpilation. Mark generated functions as
`static` (internal linkage) to avoid duplicate symbol conflicts.

## Template Instantiation Mechanism

When the transpiler sees `vector<int> v;`:

1. Record `vector<int>` as a needed instantiation in the sema
2. At emit time, generate a complete C struct + function set:

```c
struct __vector_int {
    void *__data;    // internal storage
    int64_t __size;
    int64_t __capacity;
};

static void *__vector_int_construct(void *p)
    { return new(p) std::vector<int64_t>; }
static void __vector_int_destruct(void *p)
    { ((std::vector<int64_t> *)p)->~vector(); }
static void __vector_int_push_back(void *p, int64_t v)
    { ((std::vector<int64_t> *)p)->push_back(v); }
// etc.
```

3. Each generated function is `static` — no cross-TU visibility needed
   for JIT execution (single TU). For AOT, use weak linkage.

## How Cfront Lowered Classes

```
class Foo {          struct Foo {
    int x;               int x;
    void bar(int);   };
};                   void Foo_bar(struct Foo *__this, int arg);
```

- Data members → struct fields (same order)
- Methods → free functions with `__this` as first param
- Constructors → `__ct` functions, called explicitly
- Destructors → `__dt` functions, called at scope exit / delete
- Operator overloads → mangled free functions
- Virtual → vtable struct + `__vptr` as first field
- Derived → base struct as first member (offset 0 compatibility)
- `new Foo()` → `malloc(sizeof) + ctor call`
- `delete p` → `dtor call + free`

## Why Cfront Failed (and What We Avoid)

Cfront 4.0 was abandoned in 1993 because **exception handling** couldn't
be expressed in portable C (stack unwinding). We already solved this with
SJLJ (setjmp/longjmp) + cleanup stack — the same approach SJLJ compilers
use.

## Sources

- GCC Template Instantiation docs (Cfront vs Borland model)
- "Inside the C++ Object Model" by Stanley Lippman (Cfront implementor)
- farisawan-2000/cfront-3 on GitHub (Cfront 3.0.3 source)
- Stroustrup: "A History of C++ 1979-1991"
