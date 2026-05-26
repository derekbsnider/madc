# Phase 4 Design: Itanium ABI Mangler + Strings & Streams

**Date:** 2026-05-26
**Scope:** Build Itanium ABI name mangler, then fix string/stream transpiler failures
**Baseline:** 295/475 transpiler tests passing (62.1%)

## Problem

The transpiler uses `ClassName__methodName` for C++ method names in emitted
C11. This prevents:
1. Producing `.o` files that g++ can link against
2. Resolving libc++ symbols via dlsym (they use Itanium mangled names)
3. Consistent naming between user classes and stdlib classes

Additionally, 17 string/stream tests fail due to emitter gaps (string
assignment, fstream wrappers, va_list, global init).

## Design

### Phase 4A: Itanium ABI Name Mangler

**New files:**
- `src/madc_mangle.cpp` — mangling implementation
- `include/madc_mangle.h` — public API
- `tests/unit/test_mangle.cpp` — unit tests verified against c++filt

**API:**

```cpp
// Mangle a free function: foo(int, double) → _Z3fooid
std::string itanium_mangle(const std::string &func_name,
                            const std::vector<std::string> &param_types);

// Mangle a method: Foo::bar(int) → _ZN3Foo3barEi
std::string itanium_mangle_method(const std::string &class_name,
                                   const std::string &method_name,
                                   const std::vector<std::string> &param_types);

// Mangle constructor: Foo::Foo(int) → _ZN3FooC1Ei
std::string itanium_mangle_ctor(const std::string &class_name,
                                 const std::vector<std::string> &param_types);

// Mangle destructor: Foo::~Foo() → _ZN3FooD1Ev
std::string itanium_mangle_dtor(const std::string &class_name);

// Mangle with namespace: ns::Foo::bar(int) → _ZN2ns3Foo3barEi
std::string itanium_mangle_nested(const std::vector<std::string> &qualifiers,
                                   const std::string &name,
                                   const std::vector<std::string> &param_types);
```

**Itanium ABI grammar subset (implemented):**

```
mangled-name := _Z <encoding>
encoding     := <name> <bare-function-type>
              | <special-name>
name         := <nested-name>
              | <unscoped-name>
nested-name  := N <qualifier>+ <unqualified-name> E
unscoped-name := <unqualified-name>
unqualified-name := <source-name>
                  | <ctor-dtor-name>
source-name  := <length> <identifier>
ctor-dtor-name := C1 | C2 | C3    (complete, base, allocating ctor)
               | D0 | D1 | D2    (deleting, complete, base dtor)
bare-function-type := <type>+     (parameter types, no return type)
                    | v           (void = no params)
```

**Builtin type codes (from Itanium ABI / verified via typeid):**

```
v = void        b = bool         c = char
s = short       i = int          l = long
x = long long   f = float        d = double
j = unsigned int    m = unsigned long
y = unsigned long long
a = signed char     h = unsigned char
w = wchar_t         e = long double
```

**Type modifiers:**

```
P = pointer to       (Pc = char*, Pi = int*)
R = reference to      (Ri = int&)
K = const             (PKc = const char*)
```

**Class/struct types:**

```
<length><name>        (3Foo = Foo, 7MyClass = MyClass)
```

**Substitutions (Phase 4A scope — basic only):**

```
Ss = std::string (std::basic_string<char, ...>)
So = std::ostream
Si = std::istream
St = std::  (namespace prefix)
```

The full substitution compression (S_, S0_, S1_) is deferred — it's an
optimization for long template names, not needed for correctness.

**What's deferred:**
- Template mangling (`I...E` syntax) — Phase 5+ when we handle templates
- Operator overload names (`plEii` for `operator+(int,int)`) — Phase 5
- Full substitution compression — when needed for template-heavy code
- Return type in template function mangling

### Phase 4B: String & Stream Fixes

**Modify:** `src/madc_emit_c.cpp`, `src/madc_mir_backend.cpp`

Using the existing legacy patterns (not reinventing):

1. **String assignment:** When sema says a variable is TC_CLASS with class
   "string", emit `string_assign_cstr(var, "value")` instead of bare `=`.
   Pattern from: `compiler.cpp` string handling.

2. **fstream wrappers:** Add extern C wrappers for ifstream/ofstream/fstream
   to `madc_mir_backend.cpp`. These must be typed wrappers (not generic
   casts) because `std::ios` is a virtual base class — casting `void*` to
   `ios*` gives the wrong pointer offset. Pattern from: `compiler.cpp:2577`.
   - `ofstream_construct/destruct/open/close/good/is_open`
   - `ifstream_construct/destruct/open/close/good/eof/is_open`
   - `fstream_construct/destruct/open/close`
   Emitter recognizes fstream types and emits wrapper calls instead of
   `.open()`/`.close()` method syntax.

3. **STRING_T mapping:** Add "STRING_T" and "std::string" as typedef
   aliases for `char[MADC_STRING_SIZE]` in the sema type resolver.

4. **Global string init:** Emit `__madc_init_globals()` function with
   `string_construct()` calls for each global string. Called at top of
   `main()`. Matching `__madc_cleanup_globals()` at end.

5. **va_list fix:** Emit `va_list args; va_start(args, last_param);` ...
   `va_end(args);` instead of `__va_args`.

### Phase 4C: dlsym Resolution with Mangled Names

**Modify:** `madc_mir_backend.cpp` (import_resolver)

Add a fallback path in `madc_import_resolver()`:
1. If the symbol name starts with `_Z`, try `dlsym(RTLD_DEFAULT, name)`
2. This resolves mangled names against already-loaded libstdc++/libc++
3. No new dependencies — just dlsym on what's already in the process

This unlocks calling any C++ stdlib method without explicit wrappers,
as long as the emitter produces the correct mangled name.

## What stays as extern C (our API):

- String runtime: `string_construct`, `string_destruct`, `string_assign`,
  `string_cstr`, `string_length`, `string_append` — these are madc's
  runtime API, used by all pipelines, exported via libmadc
- Stream I/O: `streamout_*`, `streamin_*` — our typed dispatch wrappers
- fstream wrappers — needed due to virtual base class offset issue
- Namespace functions: `__php_strlen`, `__perl_chop`, etc. — our API
- These are category 1 (our code, our API, hand-wired)

## What uses mangled names (libc++ access):

- Any stdlib method not explicitly wrapped
- User-defined class methods, ctors, dtors
- Future: stdlib containers, algorithms, etc.
- These are category 2 (their code, generic resolution)

## Success Criteria

1. `test_mangle` unit tests pass, verified against c++filt
2. Emitter uses mangled names for user class methods
3. Import resolver resolves mangled names via dlsym
4. 12+ of the 17 string/stream tests fixed
5. `make -C src fulltest` still green (legacy unaffected)
6. `bash scripts/run_tests.sh --backend=mir` shows improved count

## Files

| File | Change |
|------|--------|
| `src/madc_mangle.cpp` | New — Itanium ABI mangler |
| `include/madc_mangle.h` | New — mangler public API |
| `tests/unit/test_mangle.cpp` | New — unit tests |
| `src/madc_emit_c.cpp` | Use mangled names, fix string/stream emission |
| `src/madc_mir_backend.cpp` | Add fstream wrappers, dlsym fallback |
| `src/Makefile` | Add new source files |
