# Function Pointers

Store a function's address and call through it indirectly.

## Syntax

```c
void greet(string name) { cout << "Hi " << name << endl; }

int main()
{
	auto fn = greet;      // type inferred as function pointer
	fn("World");          // indirect call through the pointer
	return 0;
}
```

Output: `Hi World`

C-style declarator spellings also work (`int (*fp)(int);`, function-ptr
typedefs, struct members holding function pointers, the full C
declarator zoo — see `tests/testfnptrdecl.mad`).

## Type System

`DataDefFPTR` wraps a `FuncDef*` carrying the target function's
signature. It reports `is_function()=true` and `is_numeric()=true` —
the dual identity that distinguishes a *pointer to* a function (a
numeric value holding an address) from a function definition itself.

## How It Works

- **Parsing:** a bare function name in value position (assigned to
  `auto` or an FPTR variable) yields the function's address; an FPTR
  variable followed by `(` parses as an indirect call.
- **Lowering:** the CIR builder emits a C11 function-pointer call —
  c2mir/MIR own the calling convention. JIT-defined, native-emitted,
  and dlsym-resolved targets all work through the same shape.

## Key Design Decision

Bare function names WITHOUT `(` are only treated as addresses when the
destination is a function-pointer variable. Regular function names
(like `endl`) keep their normal behavior — stream manipulators are
unaffected.

## Files

- `include/datadef.h` — `DataDefFPTR`, `DataDefAUTO`
- `src/parser.cpp` — `auto` inference, FPTR path in expressions,
  declarator parsing
- `src/cir_builder.cpp` — indirect-call lowering, address emission
