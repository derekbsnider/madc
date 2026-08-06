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

## `#load`

```text
#load "libfoo.so" as foo;
// foo::function_name() now available via dlsym
```

Loads a shared library via `dlopen` with `RTLD_LAZY | RTLD_GLOBAL`. The
`RTLD_GLOBAL` flag makes all symbols globally visible, so loaded
functions are also callable without the namespace prefix through the
dlsym fallback. `--no-auto-load` disables `#load` processing (link
explicitly with `-l` instead).
