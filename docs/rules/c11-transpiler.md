# C11 Transpiler — Reasoning

## Why strict C11

The pipeline is: madc source → parser → `cir_node` tree (MC11-IR) →
c2mir → MIR → machine code; `--emit=c11` renders the same tree as
portable C11 text. c2mir is a strict C11 compiler (14K lines) with a
short list of GNU extensions (statement expressions, labels-as-values —
see the rule file's verified list). Everything the builder produces
must be C11 that c2mir accepts without modification, and the emitted
text must also compile under any external C11 toolchain.

## c2mir rejection examples

Each hard limit was discovered empirically. c2mir's error messages:

- `_Complex`: `unknown type name '_Complex'` — c2mir has no complex
  number support. Workaround: struct-based lowering with helper
  functions for each operation.
- `typeof`: `unknown type name 'typeof'` — c2mir has no typeof.
  Workaround: resolve to the concrete type string during semantic
  analysis before emission.
- Statement expressions: `syntax error` on `({` — c2mir's parser
  does not recognize GNU statement expressions.
- `vector_size`: `unknown attribute 'vector_size'` — c2mir has no
  SIMD support at all.
- Inline asm: `syntax error` on `__asm__` — c2mir has no inline
  assembly support.
- Reserved C++ keywords: c2mir reserves `class`, `new`, `delete`,
  etc. even in C11 mode, because its lexer shares keyword tables
  with a partial C++ path. User identifiers that collide must be
  renamed via `safe_ident()`.

## Why each lowering pattern

### VLA → __builtin_alloca

c2mir rejects C99-style VLA declarations (`int arr[n]`) in some
contexts. `__builtin_alloca` is universally accepted and produces
the same stack allocation. Multi-dimensional VLAs use an
array-of-pointers approach with a single flat allocation.

### _Complex → struct

Since c2mir has no `_Complex` support, each complex type maps to a
struct with `re` and `im` fields. Helper functions are generated
per type (add, sub, mul, div, conj, neg, eq, ne, make, from_real,
real, imag). This matches how many C compilers lower `_Complex`
internally.

### std::string → placement-new wrappers

C11 has no `std::string`. The transpiler allocates stack space via
`__builtin_alloca(STDSTRING_SIZE)` and calls extern "C" wrappers
that perform placement-new construction, assignment, c_str access,
and destruction. The `STDSTRING_SIZE` macro is computed from
`sizeof(std::string)` at build time for ABI portability.

### Classes → struct + static functions

The Cfront approach: each class becomes an instance struct (data
members), a vtable struct (virtual method pointers), and static
functions for constructor, destructor, and methods. The first
parameter of each method is `ClassName_instance_t *this`. Virtual
dispatch goes through the vtable pointer (`__vptr`). Inheritance
copies base fields into the derived struct.

### Exceptions → setjmp/longjmp

C11 has no exceptions. The SJLJ approach uses `setjmp` at try
entry points and `longjmp` at throw sites. Exception type and
value are stored in thread-local context. Catch blocks dispatch
on the stored type (int=1, double=2, cstr=3). Destructors for
stack objects are injected before `longjmp` propagation.

## Why extern "C" everywhere

c2mir compiles C11. It resolves external symbols via `dlsym` at
MIR link time. Every C++ runtime function (string operations,
stream I/O, STL containers, namespace functions) must have an
`extern "C"` wrapper so its symbol name is unmangled and visible
to `dlsym(RTLD_DEFAULT, ...)`. Without the wrapper, the C++
mangled name is invisible to the C11 linker.

## String literals in nested initializers

c2mir has a bug where string literals inside nested (depth >= 1)
char array initializers are not handled correctly. The workaround
is to expand `"hello"` to `{'h','e','l','l','o','\0'}` when the
initializer nesting depth is >= 1. This produces identical code
but avoids the c2mir parser bug.

## Range designator expansion

GCC supports `[0 ... 3] = value` as a range designator in array
initializers. c2mir does not. The emitter expands these to
individual designators: `[0] = value, [1] = value, [2] = value,
[3] = value`.
