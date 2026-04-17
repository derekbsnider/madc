# Struct Compiler Rules

Rules for compiling struct variables and member access.

## Member address computation

- Use `addOffset(delta)` to add a member offset to a struct's Mem
  operand — NEVER `setOffset(delta)`. `setOffset` replaces the base
  stack displacement; `addOffset` preserves it.
- Call `setSize(var.type->size)` on the member Mem before using it.

## TokenMember::operand return shape

- Numeric member (`is_numeric()` true): return the `x86::Mem` operand
  directly — callers can read/write the memory location.
- Non-numeric member (string, object): LEA the member address into a
  fresh Gp register and return that Gp. Callers (e.g. `string_assign`)
  want a pointer to the member's location, not the value.

## String member lifecycle

- String members inside a struct are NOT auto-initialized by stack
  allocation. The memory is uninitialized bytes.
- In `TokenCpnd::voperand()` btStruct case, for every string member:
  LEA the member's address and call `string_construct(addr)` to
  placement-new a `std::string` there.
- In `TokenCpnd::cleanup()` default case, for every string member:
  call `string_destruct(addr)` to invoke `~std::string()`.
- Skipping construction leaks through to `string_assign` / destructor
  running `free` on garbage — double-free crash at runtime.

## Reading a numeric member in a read context

- `TokenMember::compile()` on the read path (`regdp.first == nullptr`)
  must LOAD the Mem operand into a fresh Gp before setting
  `regdp.first`. The BSL (`<<`) operator's right-hand side expects a
  Gp, not a Mem.

See `docs/rules/struct-compiler.md` for code samples and the
rationale behind each rule.
