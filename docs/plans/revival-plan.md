# Mad-C Revival Plan

**Date:** 2026-04-14  
**Status:** Phase 3 Complete — Phase 4 Planned

## Context

madc was last touched ~7 years ago. The asmjit v1.14 migration has been completed and the binary builds and runs. Phase 1 is complete: 24/24 tests pass, doctest unit tests pass. This plan covers the full path to a robust, extensible, "gone mad" C-style JIT language.

---

## Phase 1 — Foundation: Correctness & Developer Experience ✓ COMPLETE

*Goal: Make madc reliable and usable as a scripting tool. Unblocks everything else.*

### 1.1 Verbose flag (`-v / --verbose`)

**Problem:** 524 `DBG()` callsites always emit traces, polluting stdout alongside program output.

**Files:** `src/madc.cpp`, `include/madc.h`, `src/lexer.cpp`, `src/parser.cpp`, `src/compiler.cpp`, `src/typesafe.cpp`

**Approach:**
- Add a global `bool madc_verbose = false;` in `include/madc.h`
- Parse `-v` / `--verbose` in `madc.cpp` before running
- Change all `#define DBG(x) x` → `#define DBG(x) if(madc_verbose) x`
- `madc.cpp` already has `#define DBG(x)` (disabled) — update to match

**Docs to create:** `.claude/rules/debug.md`, `docs/usage.md`

---

### 1.2 Register-only execution & memory writeback

**Context:** madc's speed comes significantly from operating predominantly in virtual registers. asmjit's compiler handles register allocation and only spills to the stack when it must. This is intentional and should be preserved.

**The bug:** For types that *must* live at a specific memory address (structs, C++ objects like stringstream, heap-allocated strings), register modifications are not being stored back to that address. The register has the right value; the memory does not. This causes struct writes and stringstream state to be silently lost.

**The solution — two explicit modes:**

1. **`register` variables** (new keyword): Live entirely in virtual registers. Never written to memory. Maximum speed. Lifetime limited to their scope. Use for hot numeric locals. This is madc's current de-facto behavior — we make it explicit.
   ```c
   register int x = 123;   // stays in a Gp register, never hits memory
   register double d = 1.5; // stays in an Xmm register
   ```

2. **Normal variables**: For scalars, use asmjit's stack slots (fast, but memory-backed). For complex types (struct, class, string, stream), always memory-backed with explicit stores after modification.

**Files:** `src/compiler.cpp` (`TokenAssign::compile`, `TokenCpnd::cleanup`, `Variable::modified`), `include/madc.h` (`Variable`, `vfREGISTER` flag), `src/parser.cpp` (`register` keyword in parseDeclaration), `include/tokens.h`

**Audit approach:**
1. Add `vfREGISTER` to `varflag_t` — variables with this flag never get store instructions
2. Audit `TokenCpnd::cleanup()` — emit stores for `vfMODIFIED` variables that are NOT `vfREGISTER`
3. Audit `TokenAssign::compile()` — after writing to a Gp/Xmm register for a memory-backed variable, emit explicit `mov [mem], reg`
4. For struct members: `TokenMember` stores must go to `base_ptr + member_offset`

**Performance note:** `register` variables in hot loops (like the `test2.mad` 100M iteration loop) should remain as fast as they are now.

---

### 1.3 Char literal parsing

**Problem:** Lexer creates `TokenChar` (type 10) correctly, but parser has no handler for it in `parseExpression()`.

**Files:** `src/parser.cpp` (add `case ttChar:` in `parseExpression`), `include/tokens.h` (TokenChar already exists)

**Approach:**
- In `parseExpression()`, handle `ttChar` the same way `ttInt` is handled — push a `TokenInt` with the char's integer value (or a dedicated TokenChar handler)
- `putchar('h')` should then work — it's just an integer argument

---

### 1.4 Error reporting cleanup

**Problem:** `showerror()` exists and works, but parse errors are silently swallowed by `catch(std::exception &e)` in `parser.cpp:1790`. The user sees nothing useful.

**Files:** `src/parser.cpp` (catch block), `src/lexer.cpp` (throwbuf/throwstream), `src/madc.cpp` (top-level catch)

**Approach:**
- The `throwstream` mechanism (lexer.cpp:575-598) already formats nice errors with filename:line:col and caret — but the catch in parser.cpp swallows `std::exception` silently
- Add a top-level `try/catch` in `madc.cpp` that prints the exception message
- Make the parser catch block re-throw after marking parse as failed, or use a dedicated `ParseError` exception class that carries the already-formatted message
- Result: user sees `tests/test.mad:28:15: error: unexpected token type 10 value 104 char h` cleanly

---

### 1.5 doctest unit test framework

**Problem:** No automated unit tests. Hard to verify correctness of individual compiler components, register operations, or type system logic.

**Approach:**
- Add `doctest.h` (single-header, zero dependencies) to `include/`
- Create `tests/unit/` directory for C++ unit tests
- Create `tests/unit/test_typesafe.cpp`, `tests/unit/test_datadef.cpp`, `tests/unit/test_lexer.cpp`
- Add `make test` target to `src/Makefile` that builds and runs unit tests
- Unit tests cover: register type conversions, safemov variants, DataType arithmetic, lexer token output for key inputs

**Docs to create:** `docs/testing.md`

---

## Phase 2 — Core Language Features ✓ COMPLETE

*Goal: Full struct/class support, formalized C++ integration, namespace resolution.*

### 2.1 User-defined `struct`

**Problem:** Only one hardcoded struct (`ddTESTSTRUCT`) exists. Users can't define their own.

**Files:** `src/parser.cpp` (parseDeclaration, struct keyword), `src/compiler.cpp`, `include/datadef.h` (DataDefSTRUCT), `include/madc.h`

**Approach:**
- In `parseDeclaration()`, handle `struct Name { ... }` syntax:
  - Parse field list, build a `DataDefSTRUCT` dynamically
  - Register the new struct type in `Program`'s type registry
  - `struct Foo f;` declares a variable of that type, allocating `sizeof(Foo)` bytes
- Fix member access writeback (depends on 1.2) for assignment to struct fields
- Struct layout: sequential field offsets, no padding for now (keep it simple)

**Tests:** `tests/teststruct.mad` should pass after this + 1.2

---

### 2.2 `class` definitions

**Problem:** Not implemented. The type system has `BaseType::btClass` but no parser/compiler support.

**Files:** `src/parser.cpp`, `src/compiler.cpp`, `include/madc.h` (Method, DataClass)

**Approach:**
- `class Foo { int x; void bar() { ... } }` syntax
- Methods stored as `Method` objects on `DataClass`
- Method calls: `foo.bar()` — compile as regular function call with implicit `this` pointer (first arg)
- Start with data members only (no vtable/virtual), then add methods
- Constructor/destructor: explicit `Foo()` / `~Foo()` syntax, JIT-compiled
- Existing `DataClass` and `Method` classes in `madc.h` give the scaffolding

---

### 2.3 Namespace resolution (`::`)

**Problem:** `::` is already lexed as `tkNS` but the parser ignores it.

**Files:** `src/parser.cpp` (parseExpression, parseCallFunc), `include/madc.h` (namespace registry)

**Approach:**
- In `parseExpression`, when identifier is followed by `tkNS`, enter namespace resolution:
  - Build qualified name: `ns + "::" + member`
  - Look up in namespace registry (new `std::map<string, NamespaceHandler*>` on Program)
- Each namespace has a handler that resolves `ns::name` to a function/variable/constant
- Built-in namespaces registered at startup: `std::`, `ios::`, `php::`, etc.
- Unknown namespace → dlopen fallback (see Phase 3)

---

### 2.4 `std::` namespace (formalize C++ stdlib integration)

**Approach:**
- `std::cout`, `std::cerr` → map to existing `cout`/`cerr` variables
- `std::string` → map to existing `dtSTRING`
- `std::endl` → map to existing `endl` function
- `std::vector<T>` → new type `dtVECTOR` with JIT calls to `std::vector<void*>` methods
- `std::map<K,V>` → new type `dtMAP`
- Functions: `std::to_string(x)`, `std::stoi(s)`, `std::stod(s)` → JIT calls to stdlib

**New file:** `src/ns_std.cpp` — namespace handler for `std::`

---

### 2.5 `#include` support and `using` statement

**Approach:**
- `#include "foo.mad"` — lexer detects `#include`, opens and tokenizes included file inline
- `#include <foo>` — looks in a search path (configurable, default `/usr/share/madc/include/`)
- `using std::cout;` — imports a name from a namespace into the current scope (no prefix needed)
- `using namespace std;` — imports all names from a namespace

**Files:** `src/lexer.cpp` (handle `#include`), `src/parser.cpp` (handle `using`)

---

## Phase 3 — Multi-Language Namespaces & Dynamic Loading ✓ COMPLETE

*Goal: The "Mad" in Mad-C — mix functions from multiple language traditions.*

### 3.1 `php::` namespace

**Approach:**
- Implement as C++ wrapper functions that mirror PHP semantics:
  - `php::explode(delim, str)` → split string → `dtSTRING[]`
  - `php::implode(glue, arr)` → join array → `dtSTRING`  
  - `php::trim(str)` → strip whitespace
  - `php::strlen(str)` → string length
  - `php::str_replace(search, replace, subject)`
  - `php::array_push(arr, val)`, `php::array_pop(arr)`
  - `php::count(arr)` → array size
  - `php::printf(fmt, ...)`, `php::sprintf(fmt, ...)`
- Wrapper functions compiled into madc binary, exposed to JIT via function pointer table
- Return types map to existing madc types

**New file:** `src/ns_php.cpp`

---

### 3.2 dlopen-based namespace fallback

**Problem:** User wants `mylib::someFunc()` to auto-load `libmylib.so`.

**Approach:**
- When namespace `foo` is unknown, attempt `dlopen("libfoo.so", RTLD_LAZY)`
- Parse function signature from call site (argument types known from JIT context)
- Use `dlsym(handle, "foo_funcname")` or `dlsym(handle, "funcname")` (try both)
- Cache the handle — subsequent `foo::` calls reuse the open handle
- If dlopen fails, compile error with clear message
- Explicit `#load "libfoo.so" as foo;` syntax also supported for non-standard library names

**Files:** `src/parser.cpp`, new `src/ns_dlopen.cpp`

---

### 3.3 `dlopen()` / `dlsym()` as first-class language functions

**Approach:**
- `void* handle = dlopen("libfoo.so");`
- `void* fn = dlsym(handle, "myfunc");`
- `fn(arg1, arg2);` — call through function pointer with known signature
- Add `dtFUNCPTR` data type, `dtHANDLE` for dlopen handles
- JIT generates: `call [reg]` for function pointer calls

---

## Phase 4 — `libmadc.so`

*Goal: Embed madc in other programs.*

### 4.1 Decouple static globals

**Problem:** `DataDef*` singletons (`ddINT`, `ddSTRING`, etc.) are file-scope statics in `parser.cpp`. Multiple `Program` instances would share them unsafely.

**Approach:**
- Move all `DataDef*` singletons into `Program` as member variables
- Add factory methods: `Program::getIntDef()`, `Program::getStringDef()`, etc.
- Update all call sites (parser.cpp, compiler.cpp, datadef.h) that reference the globals
- Static construction of `version`, `cout`, `cerr` variables moves into `Program::init()`

**Files:** `include/madc.h`, `src/parser.cpp` (lines 35-57 and all references)

---

### 4.2 Public C++ API

The C++ API is the primary embedding surface. madc is implemented in
C++ already — exposing a stable C++ class is a small refactor of the
existing `Program` class plus some hardening, not a from-scratch
design.  Modern C++ hosts (game engines, CMS plugins, native dev tools)
get the natural API; the C shim in §4.3 is a thin wrapper for legacy
hosts and FFI bindings (Python ctypes, Rust, Node-API).

**New headers:** `include/libmadc/*.h` — public C++ surface, namespaced
under `madc::` and grouped under `libmadc/` to mirror the
`libmadc.so` artifact name. Distinct from internal headers
(`madc.h`, `tokens.h`, `datadef.h`) which stay private, and from
`include/madc/` which already holds embedded scripting headers
(`stdio.h`, `iostream`, etc. baked into the binary). First
deliverable shipped: `include/libmadc/value.h`.

```cpp
namespace madc {

class value;          // tagged union: int64 / double / string / pointer
class error;          // structured: code, message, source location
class program;        // one madc execution context — replaces today's Program

class program {
public:
    program();
    ~program();

    // Loading
    void exec_file(const std::string &path);
    void exec_string(const std::string &source);
    void compile_file(const std::string &path);   // load + JIT, don't run

    // Calling into a script
    value call(const std::string &fn_name, std::initializer_list<value> args);

    // Calling out of a script — register a C++ callable under a name
    // the script can invoke.
    using callback = std::function<value(std::vector<value> &)>;
    void register_function(const std::string &name, callback fn,
                           const std::string &signature);

    // Globals
    value get_global(const std::string &name);
    void  set_global(const std::string &name, const value &v);

    // Sandbox / resource limits
    void set_cpu_limit(unsigned seconds);
    void set_mem_limit_mb(unsigned mb);
    void set_dlsym_allowlist(const std::vector<std::string> &names);
    void disable_dlsym();

    // Diagnostics
    void set_verbose(bool v);
    const std::vector<error> &errors() const;

private:
    struct impl;             // pimpl — internal Program lives here
    std::unique_ptr<impl> _;
};

} // namespace madc
```

**Implementation:** the existing `Program` becomes `madc::program::impl`.
Public methods forward through.  `exec_file` etc. throw `madc::compile_error`
or `madc::runtime_error` (subclasses of `std::exception`) on failure — the
existing `throwit` / `Throw` machinery already produces source-tagged errors,
just needs a public exception type.

**Files:**
- `include/libmadc/embed.h` (new, public)
- `include/libmadc/value.h` (new, public — shipped)
- `include/libmadc/error.h` (new, public)
- `src/embed.cpp` (new) — pimpl forwarding
- existing internals stay where they are; the `madc::` namespace is purely
  the public façade.

### 4.3 C shim (`madc_api.h`) + libmadc.so build

Thin `extern "C"` wrappers over §4.2's C++ API for hosts that can't link
C++ directly (legacy C codebases, FFI bindings).  Hand-written, ~200 LOC,
each function is roughly:

```c
extern "C" int madc_exec_file(madc_program *p, const char *path) {
    try { reinterpret_cast<madc::program *>(p)->exec_file(path); return 0; }
    catch (const madc::compile_error &e) { /* stash */ return -1; }
    catch (...) { return -2; }
}
```

**New header:** `include/madc_api.h`

```c
typedef struct madc_program_opaque madc_program;

madc_program *madc_create(void);
void          madc_destroy(madc_program *);
int           madc_exec_file   (madc_program *, const char *path);
int           madc_exec_string (madc_program *, const char *src);
int           madc_call        (madc_program *, const char *fn,
                                const madc_value *args, int nargs,
                                madc_value *result);
int           madc_register    (madc_program *, const char *name,
                                madc_func_t fn, const char *signature);
const char   *madc_last_error  (madc_program *);
/* … resource-limit / sandbox setters mirror the C++ class … */
```

**`src/Makefile` additions:**
- `lib/libmadc.so` target — shared library from all `.o` files except
  `madc.o` (the CLI driver).  Includes `embed.o` (C++ API) and
  `madc_api.o` (C shim).
- `bin/madc` links against `lib/libmadc.so` as a thin wrapper.
- `make install` copies headers (`include/madc/`, `include/madc_api.h`)
  and library to `/usr/local/`.

---

### 4.4 JIT code cache / AOT compilation

**Problem:** Every `madc some.mad` boot pays full lex + parse + asmjit compile,
even if the source hasn't changed. On a real-world program this dominates
startup — for SMAUG 1.8 (~158k LOC across 49 .c files) it's ~43s of pure
compile vs ~0.2s for the actual `boot_db()` runtime data load. Native
gcc-compiled SMAUG starts in ~1s because the compile happened once long
ago.

**Two layered approaches, in increasing complexity:**

#### 4.4.a JIT code cache — drop-in for existing CLI

Cache the asmjit-emitted machine code alongside its relocation metadata,
keyed on a hash of all input sources. On cache hit, skip lex/parse/compile
entirely, mmap the cached `.text` blob, replay relocations against the
current process's symbol layout, and jump.

**Cache layout (one file per top-level source):**

```
.madc-cache/<hash>.code     # raw CodeBuffer bytes (mmap'd RX after patching)
.madc-cache/<hash>.relocs   # [(offset, symbol_name, kind), …] patch table
.madc-cache/<hash>.meta     # input source hashes, madc version, asmjit version
```

**Cache-key hash:** SHA-256 over (madc binary's own version string +
asmjit version + every input file's contents — main + every transitive
`#include`). Any input change invalidates.

**Relocation kinds to handle:**
- Absolute call to a libc function (`printf`, `recv`, `stat` …) — resolve
  via current process's `dlsym(RTLD_DEFAULT, name)`
- Absolute call to a madc runtime helper (`string_cstr`, `madc_dlopen`,
  `__madc_sscanf`, `__madc_vsprintf`) — resolve via a built-in name table
- Variable-data pointer (`Variable::data` for globals, including string
  literals) — re-allocate the Variable in the same order on load and
  patch each reference to its new `data` address
- Function-entry label addresses — re-bind by name during cache load
- Embedded immediate string-literal pointers — same as variable-data,
  string literals are stored as global `__literal__N` Variables

**asmjit hooks needed:**
- `CodeHolder::sectionById(0)` gives the .text `Section` whose
  `_buffer` is the raw bytes
- Walk `CodeHolder::relocEntries()` for every reloc site — each entry has
  source offset, target kind, and target value/symbol
- For named externals, resolve via `LabelEntry::name()` (functions get
  named via `cc.funcEntry()` etc.)

**Granularity options:**
1. **Whole-program cache (simplest):** one cache entry per top-level
   `.mad`. Any change rebuilds everything. Probably fine for SMAUG —
   you re-JIT once after a source edit, then boot in <1s for the rest
   of the dev cycle.
2. **Per-function cache:** finer grain, partial rebuilds. Significantly
   more bookkeeping (function-level dependency graph, symbol table
   stitching across cached + freshly-compiled functions). Defer.

**Estimated effort for 4.4.a:** ~1 day, ~300 LOC. Touches:
- `src/cache.cpp` (new) — serialize, deserialize, relocation patcher
- `src/madc.cpp` — check cache before falling through to full compile
- `src/parser.cpp` — emit a deterministic order of Variable allocation
  so cache loads can recreate the same data-pointer layout

**Disable knobs:** `MADC_NO_CACHE=1` env var, `--no-cache` flag.

#### 4.4.b True AOT — compile to a real shared object

asmjit can target ELF object output instead of in-memory JIT. The flow:

```
madc --aot myapp.mad -o libmyapp.so
gcc -o myapp myapp_launcher.c -L. -lmyapp -lmadc
./myapp                            # boots in milliseconds
```

The launcher is a ~10-line C program that calls `madc_run_aot()` from
`libmadc.so`, which dlopen's the AOT'd object and jumps to its `main`.

This is what makes the embedding story complete — a CMS or game engine
ships madc-compiled scripts as ordinary `.so` files, no JIT pause at
load time. It also dovetails with the safe-mode / sandbox direction
(reference: `feedback project_madc_as_embedded_script.md`) since AOT
modules can be signed and audited offline.

**Effort for 4.4.b:** ~2-3 days. Most of the work is teaching the
compiler to emit position-independent code with explicit symbol
references rather than baked-in absolute addresses, plus a real
relocation table per the System V ELF spec. asmjit has primitives;
the work is wiring them up and writing a small Object/ELF writer.

#### Recommended sequence

1. Land 4.1 + 4.2 (libmadc.so + API) first — that's what makes 4.4.b
   possible at all.
2. Then 4.4.a (JIT cache) for the immediate dev-cycle and SMAUG-boot
   wins.
3. Then 4.4.b (AOT) for the embedding/sandbox story.

---

## Implementation Order Summary

| # | Item | Depends On | Priority |
|---|------|-----------|----------|
| 1.1 | `-v` verbose flag | — | Critical |
| 1.2 | Register writeback bug | — | Critical |
| 1.3 | Char literals | — | High |
| 1.4 | Error reporting | — | High |
| 1.5 | doctest framework | — | High |
| 2.1 | User-defined struct | 1.2 | High |
| 2.2 | Class definitions | 1.2, 2.1 | Medium |
| 2.3 | Namespace resolution (`::`) | — | Medium |
| 2.4 | `std::` namespace | 2.3 | Medium |
| 2.5 | `#include` + `using` | 2.3 | Medium |
| 3.1 | `php::` namespace | 2.3 | Medium |
| 3.2 | dlopen namespace fallback | 2.3 | Medium |
| 3.3 | First-class dlopen/dlsym | 3.2 | Low |
| 4.1   | Decouple static globals | All Phase 2 | Low |
| 4.2   | C++ embedding API (`madc::program`) | 4.1 | Low |
| 4.3   | C shim (`madc_api.h`) + libmadc.so build | 4.2 | Low |
| 4.4.a | JIT code cache (skips lex/parse/compile on cache hit) | 4.3 | Medium |
| 4.4.b | True AOT to ELF .so | 4.3, 4.4.a | Low |

---

## Docs & Rules Created Per Phase

Each phase creates corresponding documentation:

- **Phase 1:** `docs/usage.md`, `docs/testing.md`, `.claude/rules/debug.md`, `.claude/rules/testing.md`
- **Phase 2:** `docs/language/structs.md`, `docs/language/classes.md`, `docs/language/namespaces.md`, `docs/language/include.md`
- **Phase 3:** `docs/language/php-namespace.md`, `docs/language/dlopen.md`
- **Phase 4:** `docs/embedding.md`, `docs/api.md`

---

## Verification

After each phase, run:
```bash
make -C src clean && make -C src   # must build cleanly
bin/madc tests/testint.mad         # core test, no verbose output
bin/madc -v tests/testint.mad      # verbose output visible
make -C src test                   # doctest suite passes
```

Phase 1 complete when all 24 `.mad` tests pass (or have a documented, understood reason for failure).
