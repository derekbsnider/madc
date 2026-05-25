# Future Considerations: Memory Safety, C++23, and Language Evolution

Added 2026-05-24. Cross-references all existing plans to ensure we're not
building in architectural dead-ends.

## 1. Rust-Like Memory Safety

madc's current memory model is C-like: raw pointers, manual management,
no bounds checking. As C++ and Rust both move toward memory safety, madc
should prepare for optional safety features without sacrificing performance.

### What to prepare for now

**In the class model (cpp-support.md Phase 1):**
- Constructors/destructors enable RAII — this is the foundation for
  automatic resource cleanup. Get this right first.
- `new`/`delete` with constructor/destructor calls creates the ownership
  pattern that smart pointers build on.

**In the type system (code-cleanup.md):**
- When cleaning up the DataDef hierarchy, ensure there's room for
  ownership annotations: `T*` (raw), `owned<T>` (unique), `shared<T>` (refcounted).
- The `vfADDRTAKEN` flag already tracks "address escaped" — this is the
  seed of borrow analysis. Don't remove it during cleanup.

**In the compiler (typed-register-ir.md):**
- The IR's `Addr` shape (pointer-to-value in a Gp) is where borrow
  tracking would attach. Each `Addr` could carry lifetime/ownership
  metadata once the IR is in place.

### What NOT to do now
- Don't implement a borrow checker. That's a multi-month project.
- Don't add Rust syntax (`&mut`, lifetimes). Keep C/C++ surface syntax.
- Don't change pointer semantics. Safety should be opt-in via annotations
  or a compiler flag, not breaking changes.

### Possible future: `--fsanitize=address` equivalent
- madc could add optional runtime bounds checking on array/pointer access
- Controlled by a flag: `madc --check-bounds script.mad`
- The alloca bump pool already has an overflow check — this pattern
  extends to array subscript bounds

## 2. C++23/26 Features Worth Considering

### Already aligned with madc's direction

| Feature | C++ Version | madc Status | Notes |
|---------|-------------|-------------|-------|
| `auto` type deduction | C++11 | Partial (fn ptrs only) | Extend in cpp-support.md Phase 4c |
| Lambdas | C++11 | Working | Already good |
| Range-based for | C++11 | Working | Already good |
| `enum class` | C++11 | Planned | cpp-support.md Phase 4b |
| `constexpr` | C++11-23 | Partial (const fold) | Current constant folding is sufficient for now |
| Structured bindings | C++17 | Not started | `auto [a, b] = func();` — madc already has multi-return (`a, b := func()`) |
| `std::optional` | C++17 | Not started | Could map to nullable pointers |
| Modules | C++20 | Planned Phase 3 | precompiled-headers.md |
| Concepts | C++20 | Not started | Depends on general templates |
| `std::expected` | C++23 | Not started | Error handling without exceptions |
| `std::print` | C++23 | Not started | madc already has printf |
| `static operator()` | C++23 | Not started | Useful for functors |
| Deducing `this` | C++23 | Not started | Would simplify method dispatch |

### Features madc should adopt the SPIRIT of, not the syntax

**`std::expected<T, E>` (C++23):** Instead of exceptions (heavy SJLJ
overhead), madc could support Go-style error returns: `result, err :=
func()`. Multi-return already works — just needs convention + helper type.

**Pattern matching (C++26 proposal):** madc already has `rust::match` —
this is ahead of C++ here. Ensure the implementation is solid.

**Contracts (C++26 proposal):** `[[pre: x > 0]]` / `[[post: result >= 0]]`.
These are compile-time-checkable assertions. madc's `assert.h` support
covers runtime checks; contracts would be compile-time.

## 3. Impact on Existing Plans

### revival-plan.md
- **Status is stale:** Says "Phase 3 Complete" but we're well past that
  (452 tests, 97.9% GCC parity, libmadc API, PCH infrastructure)
- **Should be updated or archived.** The plan served its purpose as the
  original roadmap. Current state is tracked in TODO.md and the KG.

### typed-register-ir.md
- **Test count outdated:** References "170 tests" — now 452
- **Scope validated:** The code cleanup analysis confirms the IR plan's
  premise: 90 safemov calls and 66 emit_ir_value calls spread across
  compiler.cpp prove the coercion logic is scattered
- **Sequence clarified:** Do code cleanup Phase A (dispatch table) before
  IR Stage 3 (calls) — the cleanup makes IR port cleaner

### libmadc-phase4.md
- **New dependencies:** zlib (-lz) for PCH, needs pkg-config update
- **New capability:** `--emit-pch` could become API: `program.emit_pch()`
- **Alloca config:** The 64KB bump pool should be configurable via
  `compile_options::alloca_pool_size`
- **Thread safety:** asmjit's rsp-relative model means one Program per
  thread is safe. Document this constraint.

### perry-rust-integration.md
- **Still correct:** C++ first, C shim second, Rust third. No changes.
- **Memory safety angle:** If madc adds optional ownership annotations
  (future), the Rust wrapper could map them to Rust ownership semantics
  naturally — `owned<T>` → `Box<T>`, `shared<T>` → `Arc<T>`.

### precompiled-headers.md
- **Blocker identified:** Parser can't handle full system header
  declarations. The code cleanup (parser simplification) directly
  unblocks this.
- **Module path confirmed:** Phase 3 (.madm) aligns with C++20 modules
  direction and the `.cppm`/`.ixx`/`.mxx` file extensions.

### cpp-support.md
- **Sequencing correct:** C foundation → cleanup → C++ depth
- **Memory safety prep:** Phase 1 (constructors/destructors) is the
  RAII foundation that memory safety builds on. Get it right.
- **Exception alternative:** Consider Go-style error returns via
  multi-return as a lighter alternative to SJLJ exceptions.

## 4. Recommended Plan Updates

| Plan | Action |
|------|--------|
| revival-plan.md | Archive or update status to "Phase 3.5+ Complete" |
| typed-register-ir.md | Update test count (170→452), add cleanup prerequisite note |
| libmadc-phase4.md | Add zlib dep, PCH API, alloca config sections |
| code-cleanup.md | Add note: "prerequisite for IR Stage 3 and PCH completion" |
| cpp-support.md | Add memory safety prep notes to Phase 1 |
| cross-cutting-insights.md | Already current |
| precompiled-headers.md | Already current |
| perry-rust-integration.md | No changes needed |

## 5. The Big Picture Sequence

```
                    DONE                    NEXT                 FUTURE
                    ────                    ────                 ──────
C Foundation ──────►  ✓ 97.9% GCC parity
                      ✓ 452 tests
                      ✓ SMAUG port

Code Cleanup ─────────────────────────► Phase A (dispatch table)
                                        Phase B (parser simplify)

Typed IR ──────────────────────────────────────► Stage 0-1 (scaffold)
                                                 Stage 2-3 (ops, calls)

C++ Depth ─────────────────────────────────────► Phase 1 (ctors, ops)
                                                  Phase 2 (inheritance)

PCH/Modules ──► Phase 1 partial ───────────────► Complete Phase 1
                                                  Phase 2 (AST serial)
                                                  Phase 3 (.madm)

Memory Safety ─────────────────────────────────────────────► Optional
                                                              annotations

libmadc ──────► API exists ────────────────────► C shim
                                                  Rust/Node.js bindings
```

Each row depends on the rows above it being solid. The foundation is
proven. Cleanup unblocks everything else.
