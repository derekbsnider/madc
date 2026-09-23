# Indirection — layered pointers, references, arrays, and their symbols

- Every concern below has ONE owner. Reuse it; a missing case goes INTO the
  owner — never a re-derivation in an arm.
- Before handling any `*` `&` `&&` `[ ]` `::*` `<`, find its owner here. Not
  listed: search the CONCEPT, state the result (Rule #4), then add it here.

## Expressions
- Operand of unary `*`, of a cast, of a bare `sizeof`: `Program::parseCastExpression`
  (the engine, bounded). Deref node: `Program::build_indirection`, its only
  builder — `**x` is `*(*x)` by recursion, never a star count. Gate:
  `check-one-deref-builder.sh`.
- Unary `&`: `parseAddressOfExpression` + `addressof_result_type`.
- `*var`: `deref_type_for_variable`. Decay in a value context:
  `Program::array_decay_pointer`. sizeof of an expression: `type_query_expression_value`.
- Class prvalue receiver/argument: `class_operator_value_result` → `object_arg_addr`;
  a postfix step's overload: `class_postfix_step_operator` (cir).

## Types
- Mint `T*` / `T&` / `const T`: `getPointerType` / `getReferenceType` (the one
  collapse) / `getConstType`.
- One pointee level: `as_pointer_dd()`, never `dynamic_cast<DataDefPTR *>`.
  All levels: `dd_peel_pointers` (gated). void: `DataDef::is_void()` (gated).
- Referent: `TokenSubscript::referent_type`; for member access
  `effective_pointer_type_for_member_access`. A multi-dim row:
  `build_fixed_array_query_type` — madc stores arrays flattened.

## Declarators and symbols
- Declarators: `parse_declarator` / `member_declarator`; a `*`+cv run:
  `consume_declarator_stars`; `[dims]`: `parse_array_dimensions` +
  `nest_carray_dims` (gated).
- `(` `[` `{` `<` counting, `>>` splitting, whether a `<` opens: `delimiter-tracking.md`.
- A spelling's decorations: `ItaniumMangler::parse_type`; P/R/O/K: `encode_type`.

See `docs/rules/indirection.md` for the reasoning and the open families.
