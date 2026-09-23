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
- Unary `&`: its operand too is `parseCastExpression`'s, built by
  `Program::build_address_of` (the reader keeps only the compound literal and
  the unparenthesized qualified-id, [expr.unary.op]/4). Result type:
  `addressof_result_type`. "Is this `*x`": `TokenBase::is_indirection()`.
- Function vs function pointer: `as_funcdef_dd()` / `as_fptr_dd()` (const-safe) —
  never `is_function() && is_numeric()` or an unmarked `dynamic_cast<DataDefFPTR *>`
  (gated: `check-one-fptr-predicate.sh`); bare `is_function()` means "either". Its
  overload rank is `score_arg_to_param`'s: a fn-pointer parameter by signature;
  against arithmetic, bool is its one conversion, the rest rank neutral. A call
  through a callable VALUE: `build_call_through_value`; a pointer/reference TO a
  fn-pointer declares through cir's `pointer_to_fnptr_pieces`.
- `*var`: `deref_type_for_variable`. The ARRAY an operand denotes (extents):
  `Program::array_operand_type`; its element (the ROW): `array_operand_element_type`
  — never the operand's `datadef()` (flattened). sizeof of an expression
  (`type_query_expression_value`), decay (`array_decay_pointer`), `*a`, `a->m` ask them.
- An operand's VALUE (a reference is its referent) and its integer promotions:
  `operand_value_type` / `promoted_operand_type` (tokens.h) — every operator's type
  reads its children through them; `=`/`@=` are the left operand's (`TokenAssign`,
  `TokenCompoundAssign`) (gated: `check-one-operand-promotion.sh`); `auto`
  and a lambda's return deduce through `Program::operand_value_datadef`.
- A comparison or logical operator's result (bool in C++, int in C): the token's
  OWN type, recorded from `yields_truth_value()` by `resolve_object_operator_type`
  — never `resolved_type`.
- `c ? a : b` over two arithmetic arms ([expr.cond]/7, C11 6.5.15p5):
  `Program::conditional_arithmetic_type`, composed from the operand owners above.
- End of an expression (bind pending operators, refuse juxtaposed operands):
  `Program::finish_expression` — every exit of `parseExpression`.
- Class prvalue receiver/argument: `class_operator_value_result` → `object_arg_addr`;
  a postfix step's overload: `class_postfix_step_operator` (cir).

## Types
- Mint `T*` / `T&` / `const T`: `getPointerType` (a function type or a FuncDef
  folds to its fn-pointer — [conv.func]) / `getReferenceType` (the one collapse)
  / `getConstType`.
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
