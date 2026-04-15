# Changelog

## [Unreleased] — Phase 1 (2026-04-14)

### Added

- **`-v` / `--verbose` flag** — `DBG()` output is now gated behind `madc_verbose`; running without the flag produces clean program output only. Flag parsed in `madc.cpp`; `bool madc_verbose` declared in `datadef.h` (so it's visible to all headers before `madc.h`) and defined in `madc.cpp`.

- **`register` keyword** — Declares a variable as register-only, preventing any memory writeback. Sets `vfREGISTER` flag on the variable. Implemented as `TokenREGISTER` (tokens.h), parsed in `parser.cpp`, keyword registered in `lexer.cpp`. Makes madc's existing register-first execution model explicit and opt-in.

- **doctest unit test framework** — `include/doctest.h` (single-header, v2.4.11), `tests/unit/` directory, `make -C src test` target. First test file `tests/unit/test_datadef.cpp` (25 tests covering DataType enum, varflag_t bitmask, DataDef type queries, DataDefSTRUCT layout/offsets, Variable set/get/cmp/inc/dec/modified).

- **Documentation:**
  - `docs/usage.md` — language reference: data types, register keyword, built-in functions, control flow, operators
  - `docs/testing.md` — integration test runner, doctest setup, how to write new unit tests
  - `docs/test-status.md` — per-test results table with expected output and known issues
  - `docs/plans/revival-plan.md` — full Phase 1–4 roadmap
  - `.claude/rules/debug.md` — DBG macro rules, do-while idiom, multi-statement blocks
  - `.claude/rules/testing.md` — test rules, doctest gotchas, PR requirements

- **`.gitignore`** — covers compiled test binaries (`tests/unit/test_*`), editor backup files (`*.cpp~`, `*.h~`, `Makefile~`, `*.sh~`)

### Fixed

- **Char literal compilation** — `putchar('h')` and char expressions were silently skipped (no `case ttChar:` in `TokenBase::compile()`). Added `TokenChar::compile()` and `TokenChar::operand()` methods; added `case TokenType::ttChar:` dispatch in `compiler.cpp`. Fixes `test4.mad`.

- **Struct member access** — Two bugs:
  1. `TokenMember::operand()` called `setOffset(member_offset)` which *replaced* the entire stack displacement, giving `[rbp + offset]` instead of `[rbp - slot_size + offset]`. Fixed with `addOffset(member_offset)`.
  2. Numeric members were returned as Mem operands when used in read contexts (e.g. `cout <<`); the BSL compiler expected a Gp register. Fixed by loading numeric members into a new Gp register in `TokenMember::compile()` on the read path.

- **Struct string member lifecycle** — String members inside structs were not initialized via placement-new (`string_construct`). When `string_assign` was called, the destructor freed garbage, causing double-free crashes. Fixed by adding a construction loop in `TokenCpnd::voperand()` (btStruct case) and a matching destruction loop in `TokenCpnd::cleanup()`.

- **`DBG()` dangling-else** — `#define DBG(x) if(madc_verbose){x;}` captured the `else` branch in `if(cond) DBG(stmt); else ...` patterns in the parser. Fixed with `do { if(madc_verbose){x;} } while(0)` idiom across all 5 source files.

- **`madc_verbose` visibility** — `extern bool madc_verbose` was declared in `madc.h` but `datatokens.h` uses `DBG()` and is included before `madc.h`. Fixed by moving the `extern` declaration to `datadef.h` (the first header included everywhere).

- **Multi-statement DBG blocks in `_compiler_init()`** — Three separate `DBG()` calls shared a `static FileLogger logger` variable, but each `do-while(0)` block had its own scope. Merged into a single `DBG(...)` call.

- **`dynamic_cast` vs `static_cast` for virtual base** — `TokenREGISTER::parse()` used `static_cast<TokenDecl*>` on a value whose base class is virtual. Fixed with `dynamic_cast<TokenDecl*>`.

- **asmjit v1.14 migration** (prior session, captured here for completeness):
  - `BaseReg::kTypeGp*` → `RegType::kGp*`
  - `BaseReg::kGroupVec/kGroupGp` → `RegGroup::kVec/kGp`
  - `ConstPool::kScopeLocal` → `ConstPoolScope::kLocal`
  - `CallConv::kIdHost` → `CallConvId::kCDecl`
  - `cc.call(target, sig)` → `cc.invoke(&node, target, sig)`
  - `Imm::i64()` → `Imm::value()`
  - `Operand::isEqual()` → `Operand::equals()`
  - `FormatOptions::kFlag*` → `FormatFlags::k*`
  - `movsd(reg, ptr)` → `movq(reg, qword_ptr(ptr))` for non-Mem sources

### Changed

- **`#define DBG(x)`** changed from `#define DBG(x) x` (always-on) to `#define DBG(x) do { if(madc_verbose){x;} } while(0)` in all 5 source files (`madc.cpp`, `lexer.cpp`, `parser.cpp`, `compiler.cpp`, `typesafe.cpp`).

- **`varflag_t`** — added `vfREGISTER = 16` between `vfPARAM = 8` and `vfREGSET = 64`.

- **`Makefile`** — added `test` target: builds all `tests/unit/*.cpp` against `TESTOBJ` (all `.o` except `madc.o`) and runs them.

### Known Issues

- `test5.mad` and `test4.mad` user-defined `print(string s)` function outputs garbled strings — string pass-by-value is not yet supported (strings cannot be safely copied in the current type system).
- `testsstream.mad` runs without errors but produces no visible output — stringstream operations work, but the test doesn't extract from the stream to display results.

---

## Prior to Changelog

- Project originally written ~2019.
- Dormant for ~7 years due to asmjit API changes breaking the build.
- April 2026: asmjit v1.14 migration completed; binary builds and runs.
