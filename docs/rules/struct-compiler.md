# Struct Compiler — Reasoning

See `.claude/rules/struct-compiler.md` for the rules themselves.

## Why `addOffset`, not `setOffset`

The `operand_map` entry for a struct variable points to its top-of-stack
Mem operand — e.g. `[rbp - 48]`. The member offset is relative to that
base, not to `[rbp]`.

```cpp
// CORRECT — adds to [rbp - slot_size], giving [rbp - slot_size + member_offset]
x86::Mem member_mem = _obj.as<x86::Mem>();
member_mem.setSize(var.type->size);
member_mem.addOffset((int64_t)offset);

// WRONG — replaces the full displacement, giving [rbp + member_offset]
// (loses the stack base address entirely)
member_mem.setOffset((int64_t)offset);
```

## Why numeric vs non-numeric members return different shapes

Numeric members have a size the CPU can read/write in one instruction.
Returning the Mem lets the caller `mov` directly — no extra Gp
round-trip.

Non-numeric members (strings, nested structs, objects) don't have a
single-instruction load/store. The caller instead needs a pointer to
the member so it can pass it to helpers like `string_assign`,
`string_destruct`, or `string_cstr`.

```cpp
if ( var.type->is_numeric() )
    _operand = member_mem;      // Mem — caller can mov/store directly
else
{
    x86::Gp addr_reg = pgm.cc.newIntPtr(var.name.c_str());
    pgm.cc.lea(addr_reg, member_mem);
    _operand = addr_reg;        // Gp — pointer to member in stack memory
}
```

## Why string members need explicit construct / destruct

asmjit `newStack(size, align)` just reserves stack bytes — it does not
call constructors. `std::string` has a non-trivial constructor (SSO
buffer setup, null pointer for the heap-buffer slot, etc.). Reading
uninitialised bytes as a `std::string` means the object's internal
pointers are garbage. The first call to `string_assign` or the
destructor then does `free(garbage)` — SIGSEGV or double-free.

Construction:

```cpp
// For each string member in the struct, in TokenCpnd::voperand btStruct case:
x86::Mem str_mem = struct_mem;
str_mem.addOffset((int64_t)member.offset);
x86::Gp addr = pgm.cc.newIntPtr("str_addr");
pgm.cc.lea(addr, str_mem);
// call string_construct(address) to placement-new a std::string there
```

Destruction:

```cpp
// For each string member, in TokenCpnd::cleanup() default case:
// call string_destruct(address) to explicitly invoke ~std::string()
```

## Why the read-path must load Mem into a Gp

The `<<` operator (BSL / stream-output) expects its right-hand operand
as a value — specifically a Gp register. It doesn't know how to
consume a Mem directly. Without the load, `cout << obj.field` would
fail at the BSL compile step.

```cpp
// In TokenMember::compile(), when regdp.first == nullptr (read path):
if ( var.type->is_numeric() )
{
    x86::Gp gp = pgm.cc.newGpReg(appropriate_type, var.name.c_str());
    pgm.cc.mov(gp, member_mem);
    _operand = gp;
    regdp.first = &_operand;
}
```
