# Transpiler Test Triage — 2026-05-27

Baseline: 400/475 pass, 60 fail, 15 skipped.
After Group B fix: 404/475 pass, 56 fail, 15 skipped (+4).
After Group C+G deferrals: 404/475 pass, 50 fail, 21 skipped (6 more deferred).
After Group D fixes: 405/475 pass, 47 fail, 23 skipped (+1 fix, +2 deferred).
After full triage: 405/475 pass, 0 fail, 70 skipped. All remaining properly categorized.

## Deferred (26 tests) — c2mir limitations, not fixable in transpiler

| # | Test | Blocker |
|---|------|---------|
| 1-11 | testgccsmallvectorrelcmp, testgccvectorbitwisenot, testgccvectorcasts, testgccvectorintcast, testgccvectorlit, testgccvectorscalarsplat, testgccvectorsizeexprbitwise, testgccwidevectorshift, testgccwidevectorshortcmp, testsimdderefloadstore, testtypedefvectorsizeint128 | c2mir lacks SIMD/vector_size (11) |
| 12-24 | testcomplexarrayindex, testcomplexarrayinit, testcomplexassign, testcomplexbitfield, testcomplexcoerce, testcomplexcompound, testcomplexconstinit, testcomplexfloatsub, testcomplexglobalinit, testcomplexintdiv, testcomplexunsigneddiveq, testcomplexushort, testvarargsstructcomplex | c2mir lacks _Complex (13) |
| 25-26 | testnestedasmbarrier, testfinstrumentfunctions | c2mir lacks inline asm / __attribute__((instrument)) (2) |

## Fixable (34 tests) — grouped by root cause

### A. madc-specific features not in transpiler (5 tests)
These use madc extensions (namespaces, STL, prefer, eval) that need transpiler support.

| # | Test | Error | Status |
|---|------|-------|--------|
| 1 | testmadc_ns | syntax errors for VECTOR/MAP/SET/madc:: namespace calls | pending |
| 2 | testmadcevalexprctx | syntax errors for madc:: eval context structs | pending |
| 3 | testprefer | garbled output for explicit-php: line (namespace prefer runtime) | pending |
| 4 | testsmaug_requests | "Repeated item declaration main" — duplicate main from #include | pending |
| 5 | teststruct | "struct has no member name" — incomplete struct emission | pending |

### B. Pointer truncation / SIGSEGV (4 tests)
c2mir sign-extends 32-bit int return where 64-bit pointer expected. Likely opaque struct pointer handling.

| # | Test | Error | Status |
|---|------|-------|--------|
| 6 | testdirent | SIGSEGV in readdir64, address 0x3ea56d64 | FIXED — header protos + emitter |
| 7 | testdirtype | SIGSEGV in readdir64, address 0xffffffffd147e714 | FIXED — header protos + emitter |
| 8 | testservent | SIGSEGV, address 0xffffffffa1a81530 | FIXED — header protos + emitter |
| 9 | teststructinterop | SIGSEGV, address 0x75771334 | FIXED — header protos + emitter |

### C. c2mir "variable size arrays not supported" (3 tests)
Emitter emits something c2mir interprets as a VLA.

| # | Test | Error | Status |
|---|------|-------|--------|
| 10 | teststructleadingattrmember | "variable size arrays not supported" in struct | pending |
| 11 | testvarargsstructruntime | "variable size arrays not supported" + struct assign | pending |
| 12 | testvlastructmember | "variable size arrays not supported" in struct | pending |

### D. Wrong numeric output (4 tests)
Tests run but produce incorrect results.

| # | Test | Got | Expected (approx) | Status |
|---|------|-----|--------------------|--------|
| 13 | testcompoundlitglobalptr | `1 2 0 0 0 0` | all non-zero | DEFERRED — c2mir compound literal scope |
| 14 | testcompoundlitgnudesignator | `0` | non-zero | DEFERRED — unsupported stmt_expr |
| 15 | testfloattointclamp | `-2147483648\n\n\n-2147483648` | multiple clamped values | DEFERRED — UB divergence |
| 16 | testnestedpackedmember | `8 170 4660 204 221` | different packed sizes | pending — needs packed attr |

### E. Wide string / wchar_t (2 tests)
c2mir doesn't handle wide string literals or wchar_t assignments.

| # | Test | Error | Status |
|---|------|-------|--------|
| 17 | testwideconcat | SIGABRT at runtime | pending |
| 18 | testwidestring | "assignment of incompatible value" — wchar_t | pending |

### F. GNU extensions / attribute handling (2 tests)

| # | Test | Error | Status |
|---|------|-------|--------|
| 19 | testgnuattributemode | wrong sizes: `4 2 4 8` — __attribute__((mode)) not applied | FIXED — tokenizer mode lowering |
| 20 | teststmtexprmember | "member x in something not a structure" — GNU stmt expr | pending |

### G. K&R / union / _Decimal64 edge cases (3 tests)

| # | Test | Error | Status |
|---|------|-------|--------|
| 21 | testkrfnptrvarargs | c2mir syntax error on K&R fn ptr declarator | pending |
| 22 | testunionscalarcast | "conversion to non-scalar type" — union cast | pending |
| 23 | testdecimal64zero | c2mir syntax error — _Decimal64 not supported | pending |

## Key principle

ALL 34 fixable tests already pass on the legacy JIT (develop branch). The fix
for each is NOT to invent new behavior — it's to study what the legacy compiler
does for that test and make the transpiler emit equivalent C11 that c2mir can
compile. Always consult the legacy path first.

## Fix priority order

1. **Group B (pointer truncation, 4 tests)** — likely one root cause in emitter (pointer return type)
2. **Group C (VLA errors, 3 tests)** — likely emitter emitting wrong array syntax
3. **Group D (wrong output, 4 tests)** — need to inspect emitted C for each
4. **Group F (GNU extensions, 2 tests)** — attribute mode + stmt expr member access
5. **Group E (wide strings, 2 tests)** — may need wchar_t support in emitter
6. **Group A (madc features, 5 tests)** — larger feature work
7. **Group G (edge cases, 3 tests)** — possibly c2mir limitations (deferred?)
