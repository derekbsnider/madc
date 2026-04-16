# C Preprocessor Directives

madc implements a subset of the C preprocessor at the lexer level.

## `#define` / `#undef`

```c
#define MAX 100
#define PI 3.14159
#define MSG "hello"

cout << MAX << endl;    // 100
cout << PI << endl;     // 3.14159

#undef MAX
// MAX is no longer defined
```

Substitution works by pushing the define value back into the source stream for
re-tokenization. This means defines can expand to any valid token sequence, not
just simple values.

## Conditional Compilation

```c
#ifdef FEATURE_X
    // compiled only if FEATURE_X is defined
#endif

#ifndef FEATURE_Y
    // compiled only if FEATURE_Y is NOT defined
#endif

#if defined(FEATURE_X)
    // same as #ifdef
#endif

#if !defined(FEATURE_X)
    // same as #ifndef
#endif

#if 1
    // always compiled
#endif

#if 0
    // never compiled
#endif
```

### `#elif` / `#else`

```c
#ifdef LINUX
    // Linux-specific code
#elif defined(MACOS)
    // macOS-specific code
#else
    // fallback code
#endif
```

Nested conditionals are fully supported.

## `#pragma pack`

Controls struct field alignment:

```c
#pragma pack(push, 1)
struct packed_header {
    char magic;
    int32_t size;
};  // 5 bytes, no padding
#pragma pack(pop)

struct normal {
    char magic;
    int32_t size;
};  // 8 bytes, with 3 bytes padding after magic
```

## `#include`

See [embedded-headers.md](embedded-headers.md).

## `#load`

```c
#load "libfoo.so" as foo;
// foo::function_name() now available via dlsym
```

Loads a shared library via `dlopen` with `RTLD_LAZY | RTLD_GLOBAL`. The `RTLD_GLOBAL`
flag makes all symbols globally visible, so after loading you can also call functions
without the namespace prefix via the dlsym fallback.
