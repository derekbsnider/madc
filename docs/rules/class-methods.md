# Class Method Compilation — Design Rationale

## Why mangle method names as ClassName__methodName?

Without mangling, two classes that each define a method called `toString` would collide at the asmjit label level. The `ClassName__methodName` convention creates unique identifiers per class while remaining human-readable in debug output and error messages.

## Why a hidden __this parameter as void*?

All member offsets are computed at parse time from `DataDefCLASS`. The method body only needs a base pointer to the object's memory — it adds known offsets to access each member. Passing `void*` as `int64` avoids any C++ type dependency in the JIT-generated code; the compiler already knows the layout, so type information is unnecessary at runtime.

## Why LEA for stack-based objects?

Stack-allocated objects are represented as `x86::Mem` operands, which encode a displacement relative to `rbp`. A called function cannot use the caller's `rbp`-relative addressing. LEA resolves the `Mem` operand to an absolute pointer that the callee can use directly. Without LEA, the callee would receive a meaningless displacement value instead of a valid address.

## Why method_map with unmangled names?

The `methods` vector in `DataDefCLASS` stores entries with mangled names (`ClassName__methodName`) because that is what the compiler needs for label resolution. However, user code references methods by their unmangled name (`obj.methodName()`). The `method_map` provides O(1) lookup from the unmangled name the parser sees to the `Variable*` the compiler needs, avoiding a linear scan with string manipulation on every method call.
