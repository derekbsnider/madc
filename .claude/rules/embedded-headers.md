# Embedded Header Rules

## Adding a new embedded header

1. Create the header file in `include/madc/` (e.g., `stdlib.h`).
2. Use `#define` for constants — these are processed by the existing
   preprocessor.
3. If the header's functions live in a shared library, add a module row
   in `src/madc_modules.cpp` (name, interface header, per-OS image) so
   scripts `import <module>;` — a header never spells a library file name.
4. Functions are available via dlsym fallback — no explicit registration
   needed.
5. Run `make -C src` — `scripts/gen_embedded_headers.sh` regenerates
   automatically.

## Declare real return types — never rely on the fallback for signed int

- The dlsym/implicit fallback gives an undeclared function a generic 64-bit
  (`long`) return. For a function that returns a **signed `int`** whose result
  is used in a comparison, that is a correctness bug: libc returns `int` in
  `eax`, so a negative value read as 64-bit `long` becomes a huge positive.
- Declare the real return type in the embedded header. In particular the
  comparison family MUST be `int`: `strcmp`, `strncmp`, `strcasecmp`,
  `strncasecmp`, `memcmp` (this is what broke SMAUG's `bsearch_skill_exact` →
  `skill_lookup` → combat). Audit other signed-`int` libc fns (`atoi`,
  `memcmp`-likes, etc.) the same way.
- `char *` / pointer-returning fns must also be declared (already are) so
  ternaries and assignments don't mismatch `char*` vs `long`.

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
