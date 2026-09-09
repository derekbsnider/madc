# C Preprocessor Directives

madc implements the C preprocessor at the lexer level.

## `#define` / `#undef`

```c
#define MAX 100
#define PI 3.14159
#define MSG "hello"

cout << MAX << endl;
cout << MSG << endl;
```

Output: `100` then `hello`.

Substitution pushes the define's value back into the source stream for
re-tokenization, so defines can expand to any valid token sequence —
including function-like macros with arguments, `#`/`##` operators, and
variadic `__VA_ARGS__` forms found in real system headers.

## Conditional Compilation

`#ifdef` / `#ifndef` / `#if` / `#elif` / `#else` / `#endif`, with full
`defined(...)` and constant-expression evaluation (the same evaluator
that serves real system headers, including `?:` and arithmetic). Nested
conditionals are fully supported.

```c
#define FEATURE_X 1

#ifdef FEATURE_X
cout << "X on" << endl;
#else
cout << "X off" << endl;
#endif

#if !defined(FEATURE_Y)
cout << "no Y" << endl;
#endif
```

Output: `X on` then `no Y`.

## `#pragma pack`

Controls struct field alignment:

```c
#pragma pack(push, 1)
struct packed_header {
	char magic;
	int32_t size;
};
#pragma pack(pop)

struct normal {
	char magic;
	int32_t size;
};

cout << sizeof(struct packed_header) << " " << sizeof(struct normal) << endl;
```

Output: `5 8` (packed: no padding; natural: 3 pad bytes after `magic`).

See [struct-alignment.md](struct-alignment.md) for the full layout rules.

## `#include`

`#include <...>` reaches the real installed system headers (and madc's
embedded headers where those exist); `#include "..."` includes project
files, any extension. In the default madc dialect, common standard
identifiers auto-include their headers — see
[the language overview](overview.md). CLI: `-I<dir>` adds search
directories, `-D<name>[=v]` defines macros, `-E` preprocesses only, and
`-dM` prints the effective macro table.

## Library binding — `import`, and the low-level `#load`

Programs bind a library with `import name [as ns];` — a module's interface
and library, spelled for the target by the module map, so no `.so` /
`.dylib` / `.dll` appears in the source — see [import.md](import.md).

`#load` is the low-level directive underneath it, kept for tooling and
fixtures the way `#pragma` is: you spell the exact file, you own the
platform.

```text
#load "/tmp/build/libfoo.so" as foo;
// foo::function_name() now resolves by name at first call
```

It binds the named file verbatim through the same machinery `import` uses
(opened `RTLD_GLOBAL` on the JIT, so unprefixed calls resolve through the
dlsym fallback too; namespace members lower to the same runtime-resolved
call in every lane). `--no-auto-load` applies to both directives.
