# CLAUDE.md — Mad-C (madc) Project Guide

## Project Summary

**madc** is "My jit-Assembled Dialect of C" — a C-like scripting language that JIT-compiles directly to x86-64 machine code using the asmjit library. Programs are compiled and executed in-process with no intermediate bytecode.

## Build System

```bash
make -C src          # build bin/madc
make -C src clean    # remove all object files
```

Source lives in `src/`, headers in `include/`, output in `bin/` and `obj/`.

## Key Architecture

| Component | File | Role |
|-----------|------|------|
| Lexer | `src/lexer.cpp` | Tokenizes `.mad` source files |
| Parser | `src/parser.cpp` | Builds AST from tokens |
| Compiler | `src/compiler.cpp` | Walks AST, emits x86 via asmjit |
| Typesafe | `src/typesafe.cpp` | Type-safe register helpers (Gp/Xmm moves, arithmetic) |
| Main headers | `include/madc.h`, `include/tokens.h`, `include/datadef.h`, `include/datatokens.h` | Core data structures |

Execution flow: `madc.cpp` → lexer → parser → compiler → JIT execute.

## asmjit Version Notes

The project uses the **manually installed** asmjit v1.14 at `/usr/local/` (NOT the apt-installed package at `/lib/x86_64-linux-gnu/`). The Makefile explicitly passes `-L/usr/local/lib -Wl,-rpath,/usr/local/lib` to ensure the right library is linked.

Headers are at `/usr/local/include/asmjit/`. Key API differences from older asmjit:
- `BaseReg::kTypeGp*` → `RegType::kGp*`
- `BaseReg::kGroupVec/kGroupGp` → `RegGroup::kVec/kGp`
- `ConstPool::kScopeLocal` → `ConstPoolScope::kLocal`
- `CallConv::kIdHost` → `CallConvId::kCDecl`
- `cc.call(target, sig)` → `cc.invoke(&node, target, sig)`
- `Imm::i64()` → `Imm::value()`
- `Operand::isEqual()` → `Operand::equals()`
- `FormatOptions::kFlag*` → `FormatFlags::k*`

## Running Tests

```bash
bin/madc tests/testint.mad
bin/madc tests/testmath.mad
bin/madc tests/testif.mad
```

Test programs use the `.mad` extension and start with `#!/../bin/madc`.

## Rules

See `.claude/rules/` for project-specific rules.
