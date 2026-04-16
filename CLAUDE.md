# CLAUDE.md — Mad-C (madc) Project Guide

## Project Summary

**madc** is "My jit-Assembled Dialect of C" — a C-like scripting language that JIT-compiles directly to x86-64 machine code using the asmjit library. Programs are compiled and executed in-process with no intermediate bytecode.

The "Mad" in Mad-C: mix functions from multiple programming languages (PHP, Perl, Python, Ruby, JavaScript) in a single program via namespaces.

## Shell Command Rules

**Never chain commands with `&&` or use shell variable substitution.** Each Bash tool call must be a single, simple command. This prevents repeated permission prompts.

## Build System

```bash
make -C src          # build bin/madc
make -C src clean    # remove all object files
make -C src test     # build and run unit tests
make -C src fulltest # build + unit tests + all integration tests
```

Source lives in `src/`, headers in `include/`, output in `bin/` and `obj/`.

## Key Architecture

| Component | File | Role |
|-----------|------|------|
| Lexer | `src/lexer.cpp` | Tokenizes `.mad` source; handles `#include`, `#load` |
| Parser | `src/parser.cpp` | Builds AST; struct/class/namespace resolution |
| Compiler | `src/compiler.cpp` | Walks AST, emits x86 via asmjit; stream I/O, dlcall |
| Typesafe | `src/typesafe.cpp` | Type-safe register helpers (Gp/Xmm moves, arithmetic) |
| php:: | `src/ns_php.cpp` | 36 PHP-style string + array functions |
| perl:: | `src/ns_perl.cpp` | 21 Perl-style functions (chop, grep, glob, split) |
| python:: | `src/ns_python.cpp` | 16 Python-style functions (title, center, zfill, format) |
| ruby:: | `src/ns_ruby.cpp` | 12 Ruby-style functions (squeeze, tr, chars, rotate) |
| js:: | `src/ns_js.cpp` | 6 JS-style functions (base64, URL encoding, JSON) |
| STL | `src/ns_stl.cpp` | STL container helpers: vector<T>, map<K,V>, set<T>, list<T> |
| Headers | `include/madc.h`, `include/tokens.h`, `include/datadef.h`, `include/datatokens.h` | Core data structures |

Execution flow: `madc.cpp` -> lexer -> parser -> compiler -> JIT execute.

## asmjit Version Notes

The project uses the **manually installed** asmjit v1.14 at `/usr/local/` (NOT the apt-installed package at `/lib/x86_64-linux-gnu/`). The Makefile explicitly passes `-L/usr/local/lib -Wl,-rpath,/usr/local/lib` to ensure the right library is linked.

Headers are at `/usr/local/include/asmjit/`. Key API differences from older asmjit:
- `BaseReg::kTypeGp*` -> `RegType::kGp*`
- `BaseReg::kGroupVec/kGroupGp` -> `RegGroup::kVec/kGp`
- `ConstPool::kScopeLocal` -> `ConstPoolScope::kLocal`
- `CallConv::kIdHost` -> `CallConvId::kCDecl`
- `cc.call(target, sig)` -> `cc.invoke(&node, target, sig)`
- `Imm::i64()` -> `Imm::value()`
- `Operand::isEqual()` -> `Operand::equals()`
- `FormatOptions::kFlag*` -> `FormatFlags::k*`

## Running Tests

```bash
bin/madc tests/testint.mad        # run a single test
make -C src test                  # run unit tests
```

54 integration tests in `tests/*.mad`, 25 unit tests in `tests/unit/`. All must pass before merging. Skip `include_helper.mad` when running batch tests (it's included by `testinclude.mad`, not standalone). `testcin.mad` requires stdin input.

## Key Design Notes

- **Stream methods** use type-specific wrappers (`ifstream_good`, `ofstream_good`) because `std::ios` is a virtual base class — casting `void*` to `ios*` gives the wrong pointer offset.
- **String parameters** are pass-by-reference. `voperand()` creates a bare Gp register for `vfPARAM` non-numeric vars; `cleanup()` skips their destruction.
- **`dtSTRING -> dtCHARptr` coercion** happens automatically in `TokenCallFunc::compile()` via `string_cstr()` when a function expects `const char*`.
- **MadValue/MadArray** — tagged union + container for PHP-style mixed-type arrays. Used internally by php:: array functions.
- **dlopen functions** use variadic calling: 0 declared params, actual args passed based on compile-time types. String args auto-coerce to `const char*`.
- **Class methods** receive a hidden `__this` pointer as their first parameter. `this.member` compiles as an offset from that pointer.
- **Multi-return functions** use a hidden `__retbuf` parameter — a pointer to caller-allocated stack memory where return values are written.
- **Ternary operator** uses stack-slot merge: both branches of `cond ? a : b` write to the same stack location, avoiding the need for phi nodes.

## Rules

See `.claude/rules/` for project-specific rules.
