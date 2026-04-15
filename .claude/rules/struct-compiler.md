# Struct Compiler Rules

Rules for compiling struct variables and member access. These patterns are easy to get wrong.

## Stack Slot Displacement: addOffset, not setOffset

When computing a struct member's address, always use `addOffset()` to add the member offset to the existing stack displacement:

```cpp
// CORRECT — adds to [rbp - slot_size], giving [rbp - slot_size + member_offset]
x86::Mem member_mem = _obj.as<x86::Mem>();
member_mem.setSize(var.type->size);
member_mem.addOffset((int64_t)offset);

// WRONG — replaces the full displacement, giving [rbp + member_offset]
// (loses the stack base address entirely)
member_mem.setOffset((int64_t)offset);
```

The `operand_map` entry for a struct variable points to its top-of-stack Mem operand. The member offset is relative to that base, not to `[rbp]`.

## Numeric vs Non-Numeric Members

`TokenMember::operand()` returns different things depending on whether the member type is numeric:

- **Numeric members** (`is_numeric()` returns true): return the `x86::Mem` operand directly for writes; load into a fresh `Gp` register for reads.
- **String/object members**: LEA the member address into a `Gp` register. The Gp holds a pointer to the member's location in the struct's stack memory. This pointer is what gets passed to `string_assign`, `string_copy`, etc.

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

## String Member Lifecycle

String members inside a struct must be explicitly constructed and destructed around the struct's JIT scope. They are NOT automatically initialized by asmjit stack allocation — the memory is uninitialized bytes, not a valid `std::string`.

**Construction** — in `TokenCpnd::voperand()`, btStruct case, after allocating the stack slot:

```cpp
// For each string member in the struct:
x86::Mem str_mem = struct_mem;
str_mem.addOffset((int64_t)member.offset);
x86::Gp addr = pgm.cc.newIntPtr("str_addr");
pgm.cc.lea(addr, str_mem);
// call string_construct(address) to placement-new a std::string there
```

**Destruction** — in `TokenCpnd::cleanup()`, default (non-register) case, for structs with string members:

```cpp
// For each string member in the struct:
// call string_destruct(address) to explicitly invoke ~std::string()
```

Failure to construct results in `string_assign` or the destructor calling free on garbage pointers — a double-free crash at runtime.

## Reading a Numeric Member via BSL / compile()

When a struct member is used in a read context (e.g. `cout << obj.field`), `TokenMember::compile()` must load the Mem operand into a Gp register before setting `regdp.first`. The BSL (`<<`) operator's right-hand side expects a Gp, not a Mem:

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
