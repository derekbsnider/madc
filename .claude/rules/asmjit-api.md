# asmjit API Migration Notes (v1.14)

The codebase was originally written against an older asmjit API. These are the
mappings applied during the April 2026 revival to make it build against v1.14.

## Register Type Constants

Old: `BaseReg::kTypeGp8Lo`, `kTypeGp8Hi`, `kTypeGp16`, `kTypeGp32`, `kTypeGp64`  
New: `RegType::kGp8Lo`, `RegType::kGp8Hi`, `RegType::kGp16`, `RegType::kGp32`, `RegType::kGp64`

Switch on `reg.type()` which returns `RegType` enum.

## Register Group Constants

Old: `BaseReg::kGroupVec`, `BaseReg::kGroupGp`  
New: `RegGroup::kVec`, `RegGroup::kGp`

`isGroup()` method still available on `BaseReg`.

## ConstPool Scope

Old: `ConstPool::kScopeLocal`  
New: `ConstPoolScope::kLocal`

## Calling Convention

Old: `CallConv::kIdHost`  
New: `CallConvId::kCDecl`

## Function Invocation

Old: `InvokeNode* call = cc.call(target, funcsig);`  
New: `InvokeNode* call; cc.invoke(&call, target, funcsig);`

The `cc.call(Operand)` overload still exists for the raw x86 CALL instruction.
For compiler-level function calls with signatures, use `cc.invoke()`.

## Immediate Value Access

Old: `imm.i64()`  
New: `imm.value()`

## Operand Equality

Old: `op.isEqual(other)`  
New: `op.equals(other)` (or `op == other`)

## Format Flags

Old: `FormatOptions::kFlagMachineCode`, `kFlagDebugRA`, `kFlagDebugPasses`  
New: `FormatFlags::kMachineCode` (kFlagDebugRA and kFlagDebugPasses removed)

## movsd for non-double types (datadef.h)

The `movsd` instruction only accepts `(Xmm, Xmm)` or `(Xmm, Mem)` operands.
The old default case `cc.movsd(reg, (uintptr_t)ptr)` is invalid.
Fixed to: `cc.movq(reg, asmjit::x86::qword_ptr((uintptr_t)ptr))`
