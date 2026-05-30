> ARCHIVED: describes the removed asmjit JIT backend.

# asmjit API Migration Reference (v1.14)

See `.claude/rules/asmjit-api.md` for the rules.

The codebase was originally written against an older asmjit API. These
mappings were applied during the April 2026 revival to make it build
against v1.14.

## Register Type Constants

| Old | New |
|-----|-----|
| `BaseReg::kTypeGp8Lo` | `RegType::kGp8Lo` |
| `BaseReg::kTypeGp8Hi` | `RegType::kGp8Hi` |
| `BaseReg::kTypeGp16`  | `RegType::kGp16`  |
| `BaseReg::kTypeGp32`  | `RegType::kGp32`  |
| `BaseReg::kTypeGp64`  | `RegType::kGp64`  |

Switch on `reg.type()` which returns `RegType` enum.

## Register Group Constants

| Old | New |
|-----|-----|
| `BaseReg::kGroupVec` | `RegGroup::kVec` |
| `BaseReg::kGroupGp`  | `RegGroup::kGp`  |

`isGroup()` method still available on `BaseReg`.

## Other Constants

| Old | New |
|-----|-----|
| `ConstPool::kScopeLocal` | `ConstPoolScope::kLocal` |
| `CallConv::kIdHost` | `CallConvId::kCDecl` |
| `FormatOptions::kFlagMachineCode` | `FormatFlags::kMachineCode` |
| `FormatOptions::kFlagDebugRA` | *(removed)* |
| `FormatOptions::kFlagDebugPasses` | *(removed)* |

## Function Invocation

Old: `InvokeNode* call = cc.call(target, funcsig);`
New: `InvokeNode* call; cc.invoke(&call, target, funcsig);`

`cc.call(Operand)` still exists for the raw x86 CALL instruction. For
compiler-level calls with signatures, use `cc.invoke()`.

## Immediate Value Access

| Old | New |
|-----|-----|
| `imm.i64()` | `imm.value()` |

## Operand Equality

| Old | New |
|-----|-----|
| `op.isEqual(other)` | `op.equals(other)` or `op == other` |

## Signature Building

| Old | New |
|-----|-----|
| `FuncSignatureT<R, A, B>(CallConvId::kCDecl)` | `FuncSignature::build<R, A, B>()` |
| `FuncSignatureBuilder` | `FuncSignature` |

## Other migration points

| Old | New |
|-----|-----|
| `cc.setArg(idx, reg)` | `funcnode->setArg(idx, reg)` (use the FuncNode pointer from `cc.newFunc()`) |
| `Operand::size()` | `x86RmSize()` (only on asmjit Operands) |
| `cc.movsd(reg, (uintptr_t)ptr)` | `cc.movq(reg, asmjit::x86::qword_ptr((uintptr_t)ptr))` |

## Why `addOffset`, not `setOffset`

Stack `Mem` operands from asmjit already embed a negative displacement
from `rbp`. `setOffset(n)` replaces the entire displacement, so
computing a struct member's address that way gives `[rbp + member_offset]`
— pointing into the wrong address entirely. `addOffset(delta)` adds to
the existing displacement, giving the correct `[rbp - slot + offset]`.

## Why multi-statement DBG blocks need combining

`DBG(...)` is `do { … } while(0)`, so each invocation is its own scope.
A `static FileLogger logger;` declared in one DBG call is not visible
in the next:

```cpp
// CORRECT — single DBG block, logger visible throughout
DBG(
    static FileLogger logger(stdout);
    logger.setFlags(FormatFlags::kMachineCode);
    code.setLogger(&logger);
);

// WRONG — logger declared in first block, cannot be used in second
DBG(static FileLogger logger(stdout));
DBG(logger.setFlags(FormatFlags::kMachineCode));  // compile error
```

## Why the register-convergence limitation exists

asmjit's register allocator cannot handle the same virtual register
being written on two divergent code paths (e.g., both branches of an
if/else writing to the same Gp). The liveness analysis assumes a
virtual register has exactly one defining point. Divergent writes
confuse the allocator and produce wrong code or bail out.

Workaround: write each branch into its own fresh Gp, then merge via
a stack slot:

```cpp
x86::Mem slot = cc.newStack(8, 8);
// true branch: mov(slot, true_val)
// false branch: mov(slot, false_val)
// after merge: mov(result_reg, slot)
```

This pattern is used by the ternary operator implementation.
