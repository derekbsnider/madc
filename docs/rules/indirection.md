# Indirection — the reasoning

The bare rule is `.claude/rules/indirection.md`. This file says why each owner
exists, what went wrong without it, and which families are still open.

## Why an index rule

Layered pointers, references and arrays keep failing the same way. The
language lets a `*` or `&` stack to any depth and combine with any postfix
form: `**c.pp()`, `***sp->p3`, `(char)**pp * 100`, `*it++`, `sizeof *m`. Every
arm that meets one of those shapes is tempted to read "the few shapes I have
seen" by hand. That read is a partial copy of the grammar, and a partial copy
is right until the first shape it did not list. The fix is never another arm.
It is the owner that already reads the whole production. This rule exists so
the owner is found before the copy is written.

The owner (2026-09-22) asked for it after the `**c.pp()` defect, which was
handed over as one parse error and measured as a family of six readers.

## The unary `*` family (consolidated 2026-09-22)

Six hand-rolled readers of a `*` operand, each wrong somewhere, all with gcc
and clang agreeing on the answer:

| reader | what it missed |
|---|---|
| two-star arm | any postfix operand: `**c.pp()`, `**cp->p2`, `**first(pp)`, `**arr2[0]`, `**::gpp`, `**t.name`, `**m`. It also dereferenced `**(q)++` ONCE (silent). |
| N-star arm | the postfix chain entirely: `***c.ppp()`, `***sp->p3` |
| single-star arm | its `parseExpression` fallback swallowed binary operators: `*&x + 1` read as `*(&x + 1)` (0, not 6); `*static_cast<int*>(vp) + 1` gave 9, not 6 (both silent) |
| cast operand's reader | `(int)*p++` dropped the `++`; `(char)**pp * 100` cast the product (-68, not 700), both silent; `(int)*it` refused |
| cast fallback | every unary-led cast operand was unbounded: `(char)-x * 100` gave -68, `(char)~x * 100` gave 88 (both silent) |
| sizeof, both forms | `sizeof **p` and `sizeof -x` refused; `sizeof(*m)` on `int[2][3]` measured the element (4, not 12) |

The most complete reader (the single-star arm) was itself wrong, so no existing
reader could be promoted to owner. The owner is the expression engine: it
already binds the operand of `-` `~` `!` and prefix `++` by precedence, and `*`
was the one unary operator it never read. gcc's `c_parser_unary_expression`
has the same shape: `CPP_MULT` calls `c_parser_cast_expression` for the
operand, then `build_indirect_ref`. madc now does the same.
`parseCastExpression` re-enters the engine with a bound: at depth 0, once the
operand is complete, it stops before any token that is not a postfix `->` `.`
`[` `(` `++` `--`. `build_indirection` types the result, composed from the arms
it replaced. Recursion handles `**x`, so there is no star count to get wrong.

Reducers: `tests/testderefoperandc`, `tests/testderefoperandcpp`,
`tests/testclasspostincprvalue` (the `*it++` receiver, a CIR half of the same
idiom). Gate: `scripts/check-one-deref-builder.sh`. It was red at 31 on the
pre-consolidation parser.

## Why the builder keeps three node kinds

`TokenDeref` and `TokenDerefStep` are the named-variable forms. Their CIR arms
carry variable-only lowering: lambda-capture dispatch and ctx bindings.
`TokenDerefExpr` is everything else. `build_indirection` picks among them. A
reference variable never takes the named form, because its value is the
referent and `deref_type_for_variable` would type `*rp` as the pointer.

## Why the type owners

- `as_pointer_dd()` forwards through a `DataDefCONST` wrapper.
  `dynamic_cast<DataDefPTR *>` does not, so it breaks on a const level (the
  header comment at `include/datadef.h` beside `as_pointer_dd`).
- `DataDef::is_void()`: a bare `rawtype() == dtVOID` is also true for `void*`
  and `void**`, because a `DataDefPTR` forwards its pointee's type. Sixteen
  sites asked by hand; nine were wrong.
- `getReferenceType` is the ONE reference collapse (the first-class
  references plan, `docs/plans/2026-06-17-first-class-references.md` Phase 4).
- madc stores a multi-dimensional array flattened: the scalar element is in
  `var.type` and the extents are in `var.dims`. Any "type of `*a`" or "type of
  `a[i]`" must go through `build_fixed_array_query_type`, or it answers the
  element where C answers the row.

## Symbol counting

Counting `(` `[` `{` `<` is solved by `DelimDepth`. Whether a `<` opens a
template-argument list is a name-lookup question ([temp.names]/3). Both are
`delimiter-tracking.md`'s. This rule only points there, because duplicating it
would be the disease it describes. Counting `*` in an expression is no longer
needed at all: the recursion replaced it.

## Open families (recorded 2026-09-22, each its own commit before the seam)

Each family is recorded as a `DupFamily` in `madc-knowledge`. "Verified" means
measured on the artifact; "candidate" means found by a read-only recon and not
yet measured.

- ~~`&` operand reader~~ — consolidated 2026-09-23: `parseAddressOfExpression`
  reads through `parseCastExpression` and builds with `build_address_of`,
  keeping only the compound-literal and unparenthesized qualified-id arms
  (`[expr.unary.op]/4` decides pointer-to-member on the spelling). Fixed:
  `&*p` / `&**pp` / `&*f(x)` / `&*it` / `&*this` refused; `&f` on a
  function-POINTER variable emitted `f` (silent); `(*twice)(3)` refused (the
  designator test was an oblique FPTR test); `(*t)(i)` through a
  pointer-to-function-pointer — first "Malformed expression", and after the
  `*` consolidation a SILENT `*t = i` — because the call arm knew the callee
  only as a TokenDerefExpr (now `TokenBase::is_indirection()`); `&*q++` and
  `(*sq++).m` (a stepped deref is an lvalue). Gated in
  `check-one-deref-builder.sh`.
- ~~array operand element~~ — consolidated 2026-09-23: four sites re-derived
  what an array operand denotes from its flattened `datadef()` (the scalar):
  `array_decay_pointer`, `effective_pointer_type_for_member_access`, the CIR's
  `ctor_arg_datadef`, and `build_indirection`'s own arms — and none knew a
  `TokenSubscriptExpr` chain's depth. `Program::array_operand_element_type` is
  the owner (the extents live in `dims`, `m_dims`, `extra_indices` and the
  chain depth). Fixed: on an array OF function pointers the designator
  identity fired (`(*table)(5)` emitted `table(5)`; `sizeof(*table)` 16, gcc 8,
  silent; `(*grid[1])(5)` refused — the last two regressions of the `*`
  consolidation); rows of pointers typed as one pointer (`sizeof(*pa[1])` 4,
  gcc 8); a row decayed to the scalar (`sizeof(*(m + 1))` 4, gcc 12; `**(m +
  1)`, `*s.g[1]` refused); `*arr` on a class array dispatched `operator*`;
  `q.in->x` and `ps[1]->x` refused. Reducers `testfptrarrayderef`,
  `testarrayrowderef`, `testarrayrowderefcpp`. Still open: `one.x` on an
  array is accepted as `one[0].x` (gcc rejects); `type_query_chain_datadef`
  counts one subscript level for a `TokenSubscriptExpr` (sizeof family).
- ~~expression end~~ — consolidated 2026-09-23 (not indirection, but found
  here): the conditional-end short-circuit of `parseExpression` was a second,
  incomplete copy of the end of an expression, so a pending `=` bound two
  juxtaposed operands (`int r = (x)(4)` built `int r = x = 4`, exit 0) —
  UPDATE 90's open question. `Program::finish_expression` is the one end.
- ~~`call_arm_callee_kinds`~~ — consolidated 2026-09-23: the call through an
  expression admitted a callee only as a ternary / deref / cast / member and
  only with no binary operator pending. `(x, f)(x)`, `(f = twice)(x)` and
  `g(3) + (*tab)(3)` were refused. A `(` directly after a closed operand of
  function-pointer type is a call (adjacency, as every call arm asks it;
  the type through `as_fptr_dd`). Reducer `tests/testcallthroughexpr`.
- ~~`function_vs_function_pointer_predicate`~~ — consolidated 2026-09-23. A
  `DataDefCONST` forwards `is_function()` and `is_numeric()`, so
  `is_function() && is_numeric()` is true for a const function pointer, and
  the parser site that followed it with `static_cast<DataDefFPTR *>` misread
  the wrapper; `dynamic_cast<DataDefFPTR *>` is NULL for one (the call arms
  refused `(*pc)(3)` through a `const fn_t *` — fixed with the call arm,
  `faecc887e`). The ten semantic sites ask `as_fptr_dd()` / `as_funcdef_dd()`;
  the thirteen that render or walk the FPTR node itself say so with
  `// allowed-exception:`. Gate: `check-one-fptr-predicate.sh`.
- ~~`deref_kind_enumeration`~~ — measured 2026-09-23: the subscript-base arm
  named TokenDeref and TokenDerefExpr but not TokenDerefStep (ttBase), so
  `(*rp++)[1]` on a pointer to a row fell to the lambda introducer; it asks
  `is_indirection()` now (`tests/testderefstepsubscript`). Every other kind
  test was measured correct: the CIR pack/tsubst walkers descend only into
  child expressions (a named deref has none — `take3(*p...)`, `*p++...` and
  `*(p + 0)...` all match g++), `va_arg`, `validate_expression_ast` and the
  graph walker name every kind they mean, and each lvalue use of a step
  (`*p++ = v`, `(*p++) += 2`, `++*p++`, `(*q++)->x`) matches gcc.
- `reference_bind_address_expr` builds address nodes for reference binding
  (seven sites) beside `build_address_of` — candidate: share the plain-lvalue
  case.
- `cast_operand_shape_arms`: the cast arm's identifier-chain, call, literal and
  paren readers. Each is correct by shape; they duplicate `parseCastExpression`.
- `sizeof_operand_measurement`: `resolve_type_query_datadef`'s identifier fast
  paths measure with bare `chain->datadef()` (candidate).
- `declarator_star_suffix_outside_parse_declarator`: seven `tkMul` loops that
  bypass `consume_declarator_stars` (range-for verified; six candidates).
- `single_level_pointee_accessor`: `dynamic_cast<DataDefPTR *>` 106 times vs
  `as_pointer_dd()` 19 times (candidate).
- `all_levels_pointer_peel_gate_blind_spot`: two `&&`-clause peel loops the
  pointer-peel gate's regex misses (verified present; correct today).
- `star_count_over_spelling_string`: the mangler's `parse_type` vs
  `resolve_canonical_type_spelling` (candidate).
- `overload_sig_pointer_depth_probe`: `ParsedParamSig.pointer_depth` plus
  `unwrap_pointer_depth` (candidate).
- Doc drift: `parse_member_fnptr_declarator` and `parse_fnptr_member_tail` are
  named as owners in five comments and gate headers, but neither exists
  (verified).
