# madc Status Report

**Date:** 2026-04-15
**Branch:** develop
**Build:** Clean (no errors, no warnings)
**Tests:** 54/54 integration (testcin needs stdin), 25/25 unit — all passing

---

## What Works

| Feature | Status |
|---------|--------|
| Integer/float/double arithmetic | Solid |
| String operations | Solid |
| User-defined functions (int/string params) | Solid |
| Structs (user-defined + hardcoded) | Solid |
| Classes (data members) | Solid |
| cout/cerr stream output (`<<`) | Solid |
| stringstream | Solid |
| ifstream/ofstream (open/close/good/eof/getline) | Solid |
| while/for/do-while/if-else | Solid |
| Namespace resolution (`::`) | Solid |
| php/perl/python/ruby/js namespaces (94 functions) | Solid |
| MadArray (php::explode/implode/sort etc) | Solid |
| `#include` (relative + absolute paths) | Solid |
| `using namespace` / `using ns::member` | Solid |
| `#load` + dlopen namespace fallback | Solid |
| dlopen/dlsym/dlcall | Solid |
| File I/O (read/write/loop with getline) | Solid |
| Type conversions (to_string/stoi/stod/strlen) | Solid |
| C library globals (system/getenv/setenv) | Solid |
| switch/case/default | Solid |
| cin/>> input | Solid |
| Class methods with `this` pointer | Solid |
| Ternary operator (`?:`) | Solid |
| Multiple return values (Go-style) | Solid |
| Regex (madc::regex_match/search/replace) | Solid |
| madc:: namespace | Solid |
| std:: container scoping (vector, map, set, list) | Solid |

## Namespace Inventory

| Namespace | Functions | Highlights |
|-----------|-----------|------------|
| `std::` | 5 | cin, cout, cerr, endl, for_each |
| `madc::` | 4 | array, regex_match, regex_search, regex_replace |
| `php::` | 36 | explode, implode, trim, sort, str_replace, array ops |
| `perl::` | 21 | chop, chomp, grep, glob, split, join |
| `python::` | 16 | title, swapcase, center, ljust, rjust, zfill, format |
| `ruby::` | 12 | squeeze, tr, chars, rotate, compact, gsub |
| `js::` | 6 | btoa/atob, URL encode/decode, parseInt, JSON stringify |
| `#load` | any | dlopen shared libraries as namespaces with lazy dlsym |

**Total: ~100 namespace functions across 8 namespaces.**

## Known Limitations

| Limitation | Notes |
|------------|-------|
| String params are pass-by-reference | Mutation inside function affects caller. True copy semantics not implemented. |
| dlcall returns int64 only | No float/double return via dlcall. |
| Multi-return in brace-less if doesn't parse | `if (cond) return a, b;` requires braces: `if (cond) { return a, b; }` |
| String multi-return not yet implemented | Multiple return values work for numeric types only. |

## Bugs Found and Fixed (2026-04-14 — 2026-04-15)

| Bug | Root Cause | Fix |
|-----|-----------|-----|
| String pass-by-value in user-defined functions | `voperand()` constructed an empty string for params, losing the caller's pointer via LEA overwrite | Parameter variables get a bare Gp register; `cleanup()` skips param destruction |
| `dtSTRING -> dtCHARptr` coercion missing | Passing a string to `puts()` or C functions didn't convert | Added `string_cstr()` helper; auto-coerce in `TokenCallFunc::compile()` |
| Stream `good()`/`eof()` returning wrong values | `ifstream` inherits `ios` via virtual inheritance; casting `void*` to `ios*` skipped 256-byte pointer offset | Type-specific wrapper functions (`ifstream_good`, `ofstream_good`, etc.) |
| `#include` with absolute paths | Code always prepended current file's directory, mangling `/tmp/foo.mad` to `tests//tmp/foo.mad` | Only prepend base dir for relative paths (not starting with `/`) |
| Garbled error messages | `throw (string + string).c_str()` threw pointer to destroyed temporary | Use `throwstream` which owns the message lifetime |

## Development Roadmap

| Phase | Goal | Status |
|-------|------|--------|
| Phase 1 | Foundation: verbose, char literals, struct fix, register, doctest | **Complete** |
| Phase 2 | Structs, classes, namespaces, std::, #include, using | **Complete** |
| Phase 3 | php/perl/python/ruby/js namespaces, dlopen, MadArray | **Complete** |
| Phase 3.5 | switch, cin, class methods, ternary, multi-return, regex, STL containers, escape sequences, subscript, := | **Complete** |
| Phase 4 | `libmadc.so` embedding API | Planned |

## Commits This Session

| Commit | Description |
|--------|-------------|
| `191cef0` | Fix string params, add dtSTRING→dtCHARptr coercion |
| `10a7dae` | Phase 2.1: user-defined struct definitions |
| `2e7c691` | Phase 2.3+2.4: namespace resolution and std:: namespace |
| `f4dae14` | Phase 2.5: #include directive and using statement |
| `7905812` | Phase 2.2: class definitions with data members |
| `13ce945` | Phase 3.1: php:: namespace with string functions |
| `e3d880e` | Rework php:: namespace: remove C/C++ duplicates, add PHP-unique functions |
| `08e0b26` | Add MadValue/MadArray types, full php:: array function suite |
| `21c4301` | Phase 3.2: #load directive and dlopen namespace fallback |
| `2ca6833` | Phase 3.3: first-class dlopen/dlsym/dlclose/dlcall functions |
| `4141c1a` | Mark Phase 3 complete in revival plan |
| `da54767` | Add perl:: namespace and php::chop |
| `685461c` | Add python::, ruby::, js:: namespaces and C library globals |
| `b4ee6de` | Add ifstream/ofstream/fstream file I/O and type conversion functions |
| `f01523f` | Fix stream good()/eof() — virtual inheritance pointer offset |
| `47f7ccb` | Docs: namespace references, README rewrite, CHANGELOG update |
| `655a261` | Fix #include with absolute paths and dangling pointer in error messages |
