# Code Style Rules

- C++11 standard (`-std=c++11`)
- Tabs for indentation (existing code uses tabs)
- Header guards use `#ifndef __FILENAME_H` / `#define __FILENAME_H 1` pattern
- All asmjit-dependent code uses `using namespace asmjit;` — no need to qualify `asmjit::x86::` unless in headers
- `DBG(x)` macro wraps debug-only statements — currently defined as `#define DBG(x) x` (debug always on); future cleanup should make this conditional
- `regdefp_t` is a pair of `(Operand*, DataDef*)` — first is the return register, second is the data type
- `TokenBase` subclasses implement `compile()` and `operand()` — compile emits JIT code, operand returns the register holding the value
