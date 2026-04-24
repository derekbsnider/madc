# Typed-Register IR Rules

- Values that cross a token boundary carry an `IRValue` — never a
  bare `asmjit::Operand`. The `(type, shape)` pair is required
  context; callers that drop it are one step from an operand-shape
  bug.
- `IRShape` is `Reg`, `Mem`, `Imm`, or `Addr`. Do not invent new
  shapes without updating `docs/rules/typed-register-ir.md` first.
- Once a token is ported to the IR, it MUST NOT call `cc.mov(...)`,
  `cc.movsxd(...)`, `cc.cvtss2sd(...)`, `pgm.safemov(...)`, etc.
  directly. Every move, extend, or conversion goes through
  `IRBuilder::load` / `store` / `coerce`.
- Tokens not yet ported keep their existing asmjit code path. Use
  `builder.cc()` to cross the boundary only when strictly
  necessary, and delete the escape hatch when the token is ported.
- Adding a new coercion (e.g. `dtSTRING → const char *`) goes in
  `IRBuilder::coerce` — never inline it in a token's `compile()`.
  One place per coercion. This is the rule that makes the IR
  worth building.
- Unit-test every new IR operation in `tests/unit/test_ir.cpp`
  with a doctest case that checks the emitted asmjit instruction
  via `StringLogger`. Don't wait for an integration test to catch
  a shape bug in `IRBuilder` itself.
- Emit-as-you-build is the contract: any `IRBuilder` call that
  returns a Reg has already emitted the instruction that fills it.
  No deferred emission, no reorder-later escape hatch.
- Token ports land one at a time with `make -C src fulltest`
  green after each. A port that regresses an integration test
  gets reverted, not worked-around.

See `docs/rules/typed-register-ir.md` for the design rationale,
migration staging, and worked examples.
