# C11 Transpiler Rules

The transpiler emits strict C11 that c2mir can consume. Every C++
feature must be lowered to a C11 equivalent before emission.

## c2mir limits — verify against `mir/c2mir/README.md` + the fork before lowering

Genuinely unsupported by c2mir (lower or avoid):
- No SIMD / `__attribute__((vector_size))` — a *floor* (MIR) gap; scalarize for
  the JIT, emit-C → gcc/clang for real SIMD. Roadmapped to be added (Track 1.6,
  raise MIR). See `.claude/rules/lowering-vs-raising.md`.
- No inline assembly
- No `typeof` (C11 has none — resolve to the concrete type at sema time)

**Already supported — do NOT lower these (the old list was stale):**
- `wchar_t` and wide string literals — verified 2026-07-27: `wchar_t c;`,
  `wcslen(L"ab")` and `#include <wchar.h>` all compile and run correctly
  against the g++ oracle. The old "no wchar_t" entry sent a libc++
  investigation down the wrong path before it was checked.
- `_Complex` — native in the **madc MIR fork** (no `struct __madc_cX` lowering;
  the `_Complex` entry under "Lowering patterns" below is superseded).
- `_Generic` — c2mir handles it natively (`N_GENERIC`).
- Statement expressions `({ ... })` — c2mir supports them (GNU extension).
- Also available as c2mir extensions: labels-as-values (computed goto), range
  cases, `__builtin_jcall`/`jret`, overflow builtins.

Reserved words (not a c2mir limit — a C++/C identifier-collision concern):
- No C++ keywords as identifiers — use `safe_ident()` to prefix
  reserved words (`class`, `new`, `delete`, `this`, `try`, `catch`,
  `throw`, `template`, `namespace`, `using`, `operator`, `virtual`,
  `private`, `public`, `protected`)

## Lowering patterns

- **VLA:** `int arr[n]` → `int *arr = (int *)__builtin_alloca(n * sizeof(int));`
- **_Complex:** `double _Complex` → `struct __madc_cdouble { double re, im; };`
  with generated helper functions for arithmetic, comparison, conjugation.
- **Strings:** `std::string` → `void *s = __builtin_alloca(STDSTRING_SIZE);`
  with placement-new wrappers (`string_construct`, `string_assign_cstr`,
  `string_cstr`, `string_destruct`).
- **Classes:** → struct instance + vtable struct + static functions.
  Constructor: `ClassName__ClassName(inst *, args)`.
  Methods: `ClassName__methodName(inst *, args)`.
  `new`/`delete` → `calloc` + ctor / dtor + `free`.
- **Exceptions:** `try`/`catch`/`throw` → `setjmp`/`longjmp` with
  `__madc_try_ctx_t` and type-dispatched catch.
- **References:** → pointer semantics.
- **Lambdas:** → hoisted free functions + function pointer.
- **Templates:** → on-demand concrete instantiation per type used.

## Extern "C" boundary

- Every C++ function callable from transpiled C11 code needs an
  `extern "C"` wrapper in the runtime.
- Runtime wrappers live in `madc_mir_backend.cpp` and are resolved
  via `dlsym(RTLD_DEFAULT, ...)` at MIR link time.
- Embedded headers in `include/madc/` are pure C declarations — no
  `extern "C"` guards, no C++ types.

## Emission hygiene

- Always use `safe_ident()` when emitting user identifiers.
- Struct sizes use computed `sizeof()` macros (`STDSTRING_SIZE`),
  never hard-coded byte counts.
- String literals in nested (depth >= 1) char array initializers
  must be expanded to explicit byte lists (c2mir workaround).
- Range designators (`[0 ... 3]`) must be expanded to individual
  designators.
- Destructor calls must be injected at every scope exit, return,
  and exception unwind path.

See `docs/rules/c11-transpiler.md` for the reasoning, worked
examples, and c2mir rejection messages for each constraint.
