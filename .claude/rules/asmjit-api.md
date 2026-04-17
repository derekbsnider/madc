# asmjit API Rules

The codebase targets asmjit v1.14 installed at `/usr/local/`. The old
API is broken in places and must not be used.

- Never use the old enum prefixes (`BaseReg::kTypeGp*`, `BaseReg::kGroup*`,
  `ConstPool::kScope*`, `CallConv::kId*`, `FormatOptions::kFlag*`).
  Use the v1.14 equivalents.
- Never use `cc.call(target, funcsig)` for compiler-level calls.
  Use `cc.invoke(&node, target, funcsig)`.
- Never use `FuncSignatureT<...>(CallConvId::kCDecl)` or `FuncSignatureBuilder`.
  Use `FuncSignature::build<...>()` and `FuncSignature` directly.
- Never use `imm.i64()` or `op.isEqual(other)`. Use `imm.value()` and
  `op.equals(other)` (or `op == other`).
- Never use `Operand::size()`. Use `x86RmSize()` (on asmjit operands only,
  NOT on C++ containers).
- Never use `cc.setArg(idx, reg)`. Use `funcnode->setArg(idx, reg)`
  where `funcnode` is the pointer from `cc.newFunc()`.
- Never use `cc.movsd(reg, (uintptr_t)ptr)`. Use
  `cc.movq(reg, asmjit::x86::qword_ptr((uintptr_t)ptr))`.
- When adjusting a stack `Mem` operand for a struct member, always use
  `addOffset(delta)`, NEVER `setOffset(delta)` — the latter destroys the
  base stack displacement.
- Multi-statement `DBG(...)` blocks that share a local variable must be
  combined into a single DBG call (each DBG is a `do { } while(0)`
  scope).
- Two code paths must NOT write to the same virtual register on
  divergent branches. Use a stack-slot merge pattern (see
  `docs/rules/ternary.md`).

See `docs/rules/asmjit-api.md` for the full old-to-new API mapping
table, deprecation history, and worked examples.
