# Embedded Header Rules

## Adding a new embedded header

1. Create the header file in `include/madc/` (e.g., `stdlib.h`).
2. Use `#define` for constants — these are processed by the existing
   preprocessor.
3. Use `#load "libname.so" as ns;` if the header needs a shared library.
4. Functions are available via dlsym fallback — no explicit registration
   needed.
5. Run `make -C src` — `scripts/gen_embedded_headers.sh` regenerates
   automatically.

## Lazy registration (only when needed)

Use lazy registration only for:
- Global variables (cout, cin, cerr, stdin, stdout, stderr)
- Types and structs that need DataDef entries
- Functions that need specific signatures (not variadic)

Never register globals eagerly during tokenization — `tkProgram` does
not exist yet.

Procedure when a header needs lazy registration:
1. Add a boolean flag `_include_xxx` to `Program` in `include/madc.h`.
2. Initialize the flag to `false` in `_tokenizer_init()` (lexer.cpp).
3. Set the flag to `true` in the lexer's `#include` handler after the
   embedded header is tokenized.
4. In `_parser_init()` (parser.cpp), call `add_xxx()` if the flag is
   set.
5. `add_xxx()` populates `lazy_map` with entries — NOT direct
   registration.
6. Add resolution cases in `lazy_resolve()` and/or `lazy_resolve_type()`
   in parser.cpp.

## `lazy_map` entry format

```cpp
lazy_map["symbol_name"] = {LAZY_HEADER_ID, Program::lkVariable};
// lkVariable = globals (cout, cin)
// lkFunction = deferred function registration
// lkType     = typedefs (pid_t, size_t)
// lkStruct   = struct layouts (struct tm, struct stat)
```

## `#load` and `RTLD_GLOBAL`

- `#load` uses `RTLD_LAZY | RTLD_GLOBAL`.
- `RTLD_GLOBAL` makes loaded symbols visible via `dlsym(RTLD_DEFAULT)`,
  so functions can be called without a namespace prefix.

See `docs/rules/embedded-headers.md` for why the eager-registration
path was rejected and how dlsym fallback fits in.
