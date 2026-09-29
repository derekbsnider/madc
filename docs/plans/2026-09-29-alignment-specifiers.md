# Alignment specifiers and variable alignment (BUGS.md B62)

Owner ruling 2026-09-29: B62 is a planned change with its own session.

## Measured at 0f6fac60a

Reducers in `tmp/b62/` (c1.c, x1.cpp, t1.cpp, positions.sh). gcc = clang
and g++ = clang++ on every line.

- `_Alignas(N)`, `_Alignas(type)`, `alignas(...)`: dropped in every position,
  C and C++. The lexer maps both spellings to `__attribute__`, whose arm
  keeps a group only when it names a supported attribute.
- `__attribute__((aligned(N)))` on a VARIABLE: no effect in any position.
  `Variable` has no alignment, nothing emits one, `__alignof__(var)` answers
  the type's. A local `_Alignas(16)` object sits at 8 mod 16.
- A struct member's LEADING attribute (`AL int x;`) is read and discarded at
  the top of both member loops of `TokenSTRUCT::parse`; after the type
  (`int AL x;`) and after the declarator it works.
- Refused outright: `static AL char g;`, `const AL int g;` (file scope, block
  scope, members), `for (AL int i = 0; ...)`.
- Class templates and function-template locals: dropped like the rest.

## Semantics (C11 6.7.5, [dcl.align])

- `alignas(type-id)` is `alignas(alignof(type-id))`; `alignas(expr)` takes an
  integral constant expression. The type-id reading is chosen only when the
  type-id is the whole operand (`alignas(T::value)` is an expression), the
  rule `sizeof`/`alignof` already implement.
- `alignas(0)` has no effect. Several specifiers: the strictest wins; so for
  several `aligned(N)` (gcc). Neither lowers an alignment.
- An alignment in the decl-specifiers applies to every declarator of the
  declaration; one after a declarator applies to that declarator only.
- `alignas` is a keyword in C++11 and C23 (the keyword registry says so);
  `_Alignas` is accepted in every C mode, as gcc and clang do.

## Slices

1. **The reader.** The lexer stops erasing `_Alignas`/`alignas` (gated by the
   keyword registry). The attribute-specifier predicate answers for them,
   so every attribute site consumes them, and `consume_gnu_attributes` reads
   the operand when its caller asks for an alignment: a type-id through the
   same whole-operand test `sizeof` uses (factored out of
   `evaluate_type_query`, not copied), else a constant expression. A
   dependent operand (a template-parameter type, a value-dependent
   expression) contributes nothing, so template patterns keep today's
   behaviour until slice 4. Sites that pass no alignment skip the group.
2. **Members.** The leading attributes of a member line feed the line's
   alignment, like the ones after its type; `const AL int x;` is accepted.
   Struct layout then matches for every C member position.
3. **Variables.** `Variable` gets its declared alignment (0 = natural). The
   declaration's specifier positions (before and after storage class, cv,
   type) and each declarator's trailing attributes feed it. The CIR emits
   `_Alignas(N)` in the variable's specifiers (file scope, block scope,
   static, extern), and `__alignof__`/`_Alignof` of a variable answers
   max(type, declared). The refused positions are accepted.
4. **Dependent operands.** Class templates: check that an instantiation's
   body read evaluates the operand concretely. Function-template locals
   (libstdc++ `<atomic>`'s `alignas(_Tp) unsigned char __buf[sizeof(_Tp)]`):
   the pattern keeps the operand and emits `_Alignas(_Alignof(T))`, which
   `copy_cir_subtree`'s dependent `N_ALIGNOF` fold makes concrete; a
   value-dependent operand is measured in this slice before designing.

Out of scope, filed: C++ class bodies that need the class parser drop member
and aggregate attributes (B65); alignments above 16 (B66); a typedef's
alignment (B64). libstdc++'s aligned members sit in class-parser bodies, so
their layout changes with B65, not here.

## Status (2026-09-29)

- Slice 2 (members): a825cb684. Slice 3 (variables): 376f4cba1.
- Slice 1 (the reader) with slice 4's class-template part: a class template's
  head specifiers stay in its captured body and are read per instantiation;
  a pack in them expands (`alignas(alignof(T)...)`) through the body clone's
  expansion-unit lane, which now also takes a type-query operator unit.
- Open: slice 4's function-template locals (B62, parse-once member
  templates); found on the way and filed: B68 (dependent `alignof` in a
  parse-once body), B69 (a variadic class template-id in `sizeof`), B70,
  B71, B72, B73.

## Tests and gates

Each slice ships its reducer in `tests/` with the gcc and clang output
(`testalignas*`), Tier 1 JIT/EXE/OBJ (a variable's placement is ABI), Tier 2
per commit, and the batch checkpoint after slice 3 and after slice 4.

## Thread safety

Compile-time only: an alignment is a property of a declaration, carried on
parse-time objects owned by one `Program`. No runtime state is added.
