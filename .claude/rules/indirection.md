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
- The address of a const object whose reads fold to their value (`&cx`, any
  reference binding of `cx`): cir's `folded_read_object`, from `node1`'s N_ADDR —
  never a guard on the fold at one site.
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
  A named OBJECT's or MEMBER's array type: `object_array_type` / `member_array_type`;
  the extents under all of them: `Variable::array_dims` / `DataDefSTRUCT::m_array_dims`.
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
- Binding a reference (an argument, an aggregate's reference MEMBER slot): cir's
  `ref_param_arg_addr` (a const referent materializes a prvalue).
- A reference-returning call's result (`*call`, every call arm and the host-call shim):
  cir's `reference_call_result` (gated: `check-one-reference-call-result.sh`).
- A class-pointer conversion (implicit via cir's `upcast_class_ptr`; a static or C-style
  cast): `base_subobject_ptr` up / `derived_object_ptr` down (null stays null).
- Class prvalue receiver/argument: `class_operator_value_result` → `object_arg_addr`;
  a postfix step's overload: `class_postfix_step_operator` (cir).

## Types
- Mint `T*` / `T&` / cv `T`: `getPointerType` (a function type or a FuncDef
  folds to its fn-pointer — [conv.func]) / `getReferenceType` (the one collapse)
  / `getQualifiedType` (ONE `DataDefQUAL` per (base, cv mask); `getConstType` is
  const on it). "Is it const/volatile": `is_const()` / `is_volatile()`, never the
  wrapper's class; `unqualified()` peels the whole mask.
- One pointee level: `as_pointer_dd()` (`pointer_dd_of` when the type may be NULL) — a
  qualified pointer IS a pointer; never `dynamic_cast<DataDefPTR *>` outside exact-class
  dispatch (a rebuild, a key, a record — marked; gated: `check-one-pointee-accessor.sh`).
  All levels: `dd_peel_pointers` (gated). void: `DataDef::is_void()` (gated).
- A scalar's identity (`char` and `signed char` share one rawtype):
  `Program::proven_scalar_identity` — never `rawtype()`.
- Referent: `TokenSubscript::referent_type`; for member access
  `effective_pointer_type_for_member_access`. A multi-dim row:
  `build_fixed_array_query_type` — madc stores arrays flattened.

## Declarators and symbols
- Declarators: `parse_declarator` / `member_declarator`; a `*`+cv run:
  `consume_declarator_stars` (a caller's consumed leading cv goes in as the
  `leading_cv` mask, never dropped; the bits `modeled_cv()` names qualify each
  pointee — volatile in every mode, const in C only); `[dims]`:
  `parse_array_dimensions` + `nest_carray_dims` (gated).
- A `C::*` chain ahead (its head a class name, a class or dependent type token):
  `member_pointer_declarator_ahead`; its owner: `parse_member_pointer_owner`.
- An object's top-level cv (`modeled_cv()`'s bits) is its declared TYPE's — a variable's
  (parseDeclaration), a member's (incl. a class body's), a typedef's, a parameter
  OBJECT's (the definition's, never the function type's), a reference's referent (the
  `&`); never a flag — all through `declarator_object_cv`. A qualified array: its
  ELEMENTS (`qualify_array_elements`). A value slot dispatches on `Variable::slot_kind()` (gated: `check-one-slot-dispatch.sh`). A declarator list's tail re-pushes it:
  `push_declarator_list_tail`. Member access merges the object's cv: `member_access_type`
  over `glvalue_cv`; a call argument's type: `call_argument_type`.
- A TYPE-ID (a template argument, a using-alias or alias-template target, a `_Generic` /
  `__builtin_types_compatible_p` type name, a spelled trait argument, a trailing return
  type — `adopt_trailing_return_type`): `parse_type_id` — its leading/east/after-star cv
  is the TYPE's; never a hand-rolled `*`/`[]` loop (gated: `check-one-ptr-operator-fold.sh`).
- A type's cv levels in the emitted tree: `dd_peel_pointers(dd, &level_cv)` then
  `pointer(cv)` per level and `append_cv_specs` for the base (cir).
- A function's RETURN type in an emitted declarator (a definition, a prototype, an
  extern, a function pointer's target, a return temp): cir's `append_return_declarator`
  (gated: `check-one-return-declarator.sh`).
- `(` `[` `{` `<` counting, `>>` splitting, whether a `<` opens: `delimiter-tracking.md`.
- A non-type template argument spliced into a cloned body (one operand):
  `splice_nontype_template_arg` (gated: `check-one-nontype-splice.sh`).
- A spelling's decorations: `ItaniumMangler::parse_type` (trailing first; one level's
  cv words are ONE V/K set); P/R/O/K/V: `encode_type`. A function (pointer) through its
  layers: `fptr_structural_spelling` (a reference is R). A qualified type's spelling:
  `cv_qualified_spelling`; a parameter's: `param_declarator_spelling` (cv from the type).
- A pointer's or referent's qualification conversion ([conv.qual], [over.ics.rank]/3.2.5-6):
  `score_arg_to_param` — never a peel of the pointee's cv before comparing; a pointer
  pointee must be IDENTICAL (its exact rank one level down), never a `rawtype()` test.
- A partial specialization's pattern against a type: `unify_spec_pattern_arg` — each `*`
  level's cv exactly (`level_cv`), the core's modeled cv on the TYPE, not its spelling.
- A member function's cv-qualifier-seq: `FuncDef::method_cv()` (mangling, out-of-line
  matching, the record); the implicit object's cv: `Program::implicit_object_cv`.

See `docs/rules/indirection.md` for the reasoning and the open families.
