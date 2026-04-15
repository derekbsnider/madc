# Class Method Compilation Rules

## Name Mangling

- Method names must be mangled as `ClassName__methodName` to avoid collisions between classes
- Use `DataDefCLASS::method_map` (unmangled name -> Variable*) for method lookup at call sites
- `DataDefCLASS::findMethod()` checks `method_map` first, then falls back to the methods vector

## Hidden `__this` Parameter

- Methods receive a hidden `__this` parameter (void* as int64) as the first argument
- `__this` is injected at compile time in `TokenFunc::compile()` when `Method::owner_class` is non-null
- At the call site, `TokenCallMethod::compile()` sets `regdp.object` to the object's operand
- For stack-allocated objects (Mem operands), use LEA to get the address — do NOT use MOV (which loads the first 8 bytes of the struct)

## Member Access Inside Methods

- Unqualified member names inside method bodies resolve through `__this` via `TokenMember`
- Resolution happens in `parseExpression()`: if an identifier isn't found as a local variable, check `code->method->owner_class` for a matching member
- The member offset comes from `DataDefCLASS::m_offset()`
- `TokenMember::operand()` handles pointer-based access: `[gp_reg + offset]` for numeric members, LEA for string/object members

## Parse-Time Requirements

- Consume `(` before calling `parseFunction()` for class methods (parseDeclaration consumes it for regular functions, but `TokenCLASS::parse()` must do it explicitly)
- Parser param count check must subtract 1 for `owner_class` methods (hidden `__this` is in `func->parameters`)
