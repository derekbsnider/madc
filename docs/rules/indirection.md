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

- `as_pointer_dd()` forwards through a `DataDefQUAL` wrapper.
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
  `DataDefQUAL` forwards `is_function()` and `is_numeric()`, so
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
- ~~`cast_operand_shape_arms`~~ — consolidated 2026-09-23: the cast arm read
  its operand through nine shape arms (`__real__`, `sizeof`, identifier+call,
  postfix chain, `*`, `&`, paren / chained cast, literal, the rest) and a
  postfix-step hook, each re-deriving part of the engine — the call helper
  was the engine's call arm minus explicit template arguments and the static
  member reselect, the literal materializer re-did the lexer's adjacent-string
  concatenation, and it read only the literal: `(long)"abc"[1]` sent the `[`
  to the lambda introducer. The operand is `parseCastExpression`'s (gcc's
  `c_parser_cast_expression` recurses into itself); the two helpers are
  deleted. Reducer `tests/testcastoperand`; gated (rule 6).
- ~~`sizeof_operand_measurement`~~ — consolidated 2026-09-23: three measurers
  of `sizeof expr` disagreed. The parenthesized form's two identifier fast
  paths (a contextual-identifier copy and a plain one) read the chain with
  `parsePostfixChain`, measured its flattened `datadef()` and patched a
  fixed-array member by its element count; the unparenthesized form measured
  the node's scalar (`sizeof s.a` 4, gcc 12; `sizeof s.n` 1, gcc 10; `sizeof
  s.n[1]` 1, gcc 5 — a regression of the `*` consolidation); neither counted a
  member-row subscript. `Program::array_operand_type` (the array with its
  extents; `array_operand_element_type` is its element) is the owner, a
  variable operand goes to the one expression measure, and the fast paths are
  deleted. Reducer `tests/testsizeofoperand`.
- ~~`arithmetic_operand_value_and_promotion`~~ — consolidated 2026-09-23 (the
  handoff's Gap `unary_operator_integer_promotion`, measured at 3x its recon).
  Every operator's parse-side type read its children as `left->datadef()` and
  re-derived the promotion by hand, so the defects spread across the operator
  set rather than one arm. Unary `~`/`-` kept the operand's type: `f(~uc)`
  picked `f(unsigned char)`, and `sizeof(~ch)` was 1. Unary `+` was dropped
  outright, so `(+k).v` never ran a class's `operator+()` (1, g++ 101),
  `sizeof(+a)` measured the array (12, g++ 8), and `+[](int){...}` did not
  compile. A bit-field narrower than int kept its declared `unsigned` type,
  and `%` had no type at all (`l % 3` was 4 bytes). The worst case was a
  reference operand, which typed the operator as the reference's lowered
  POINTER: `auto a = rl + 2` bound a pointer to 42 and crashed. `auto` itself
  deduced through a second copy of the operator typing (`deduce_expr_type`'s
  `+ - * /` arm), which read a reference leaf the same way (`auto c = rl`
  crashed) and answered double for any real operand (`auto x = f * 2` on a
  float was 8 bytes). The owners are
  `operand_value_type` ([expr]/5) and `promoted_operand_type` ([conv.prom],
  bit-field and enum aware) over `integer_promoted_type`, which cir's
  `enum_promotes_to` now composes from. Unary `+` is `TokenUnaryPlus` (c2mir's
  one-operand `N_ADD`), and `deduce_expr_type` asks `operand_value_datadef`.
  Reducers `tests/testunarypromotion`, `tests/testunarypromotionc`; gate
  `check-one-operand-promotion.sh` (24 direct child reads and the shifts'
  hand-rolled floor at the parent commit). `+f`'s decay type is
  `getPointerType(f)`: the one `T*` mint already folded a function TYPE to its
  fn-pointer, and now folds a FuncDef too ([conv.func]), so the three sites that
  each minted a FuncDef's pointer by hand (the call through an expression,
  deduction's decay, `auto fp = f`) share it — the declarator-reader gate's
  `new DataDefFPTR(` baseline fell from 5 to 3.
  Found beside it and fixed next, each its own commit: C++ `==`/`<`/`&&`/`!`
  are typed int, not bool; `b ? uc : ss` skips the usual arithmetic
  conversions; compound assignments (`+=` ... `^=`) have no type (int); a
  function-pointer-typed argument never ranks against a function-pointer
  parameter (`f(pg)` and `f(+g)` pick `f(long)`, silent); a free unary
  `operator-`/`operator+` is refused; aggregate-initializing a reference
  member (`RM rm{lv}`) stores the value as the pointer; `long long` is
  `long`'s identity on Linux (`f(long)` + `f(long long)` collide); C
  `signed char` selects `char` in `_Generic`; C's GNU `__auto_type` is
  unsupported; a C unfixed enum promotes to int where gcc/clang pick
  `unsigned int` (madc and c2mir agree on int, so a backend decision).
- ~~`cxx_comparison_result_not_bool`~~ — fixed 2026-09-23, the first found
  beside the family above. Every built-in comparison and logical operator
  (`==` `!=` `<` `>` `<=` `>=` `&&` `||` `!`) was typed int in C++ as well as C,
  silently: `f(i == j)` picked `f(int)` over `f(bool)`, `sizeof(i == j)` was 4,
  `auto a = (i == j)` deduced int, a template's `f(a < b)` did the same, and a
  madc `println("{}", i < j)` printed 1 where the carrier's own comparison
  printed true. The result is the language's, so it cannot live in the token
  class alone (tokens carry no mode): each operator class answers
  `yields_truth_value()`, and `resolve_object_operator_type` — the reduce-time
  owner that already types built-ins — records bool as the token's OWN type
  under `presents_as_cpp()`. Not `resolved_type`: that field also means "an
  overload or a decay chose this" (`noexcept_eval_expr` reads it so), and a
  class operand's overload still sets it and wins (`K::operator<` returning
  int stays int). Reducers `tests/testcomparebool` (C++), `testcompareboolc`
  (C stays int), `testcompareboolmadc` (the dialect formats `true`).
- ~~`conditional_usual_arithmetic_conversion`~~ — fixed 2026-09-23, the
  second found beside the operand-promotion family, and worse than recorded:
  wrong VALUES, not only types. The engine's ternary arm typed a conditional as
  its TRUE arm (the false arm only when the true one was int), so `b ? uc :
  ss` was unsigned char where both languages say int, `b ? i : l` int (long),
  `b ? fl : d` float and `b ? i : d` int (double), sizeof(b ? uc : ss) 1, and
  `auto a1 = nb ? uc : ss` declared an unsigned char that stored -5 as 251.
  c2mir computed the arms' conversion correctly all along; only the parse-side
  type every consumer reads was wrong. `Program::conditional_arithmetic_type`
  is [expr.cond]/7 for two arithmetic arms: C++ keeps a type both arms share
  after lvalue-to-rvalue (`b ? uc : uc2` unsigned char, an enum pair the enum,
  a bool pair bool), otherwise — and always in C (6.5.15p5) — the usual
  arithmetic conversions over the promoted arms. It composes
  `operand_value_datadef`, `promoted_operand_type`, `usual_arithmetic_result`
  and `proven_scalar_identity`; the pointer, array, carrier and class rules
  of the arm stay as they were. Reducers `tests/testconditionaltype`,
  `testconditionaltypec`. Found beside it: C `_Generic` selects `default` for
  an enum operand where gcc/clang select its compatible type (joins
  `c_unfixed_enum_promotion`).
- ~~`compound_assignment_untyped`~~ — fixed 2026-09-23, the third found beside
  the operand-promotion family. The ten compound-assignment classes (`+=` ...
  `^=`) declared no type and answered the operator default, int, in both
  languages: `_Generic(d *= 2)` selected int and `sizeof(uc += 1)` was 4
  (silent, in C), `f(p += 1)` bound `f(int)` (a c2mir check error) and
  `*(p += 2)` was refused as a dereference of a non-pointer. An assignment
  expression has its left operand's type ([expr.ass]/1, C11 6.5.16p3) —
  TokenAssign's own `operand_value_type(left)` rule — so the ten now derive
  one `TokenCompoundAssign` carrying it (and their shared precedence and
  associativity). The operand gate's rule 3 refuses a bare
  `TokenMultiOp("@=")` (ten at the parent). Reducers
  `tests/testcompoundassigntype`, `testcompoundassigntypec`.
- ~~`overload_rank_fptr_argument`~~ — fixed 2026-09-23, the fourth found beside
  the operand-promotion family. `score_arg_to_param` answered every
  fn-pointer PARAMETER (by signature) but not a function ARGUMENT against an
  arithmetic parameter. `DataDefFPTR::is_numeric()` is true, so a
  fn-pointer argument took the numeric lane and scored an exact 5 against
  `long` by storage (rawtype 64 == 64), tied the fn-pointer overload and,
  declared first, won — `f(pg)`, a typedef'd, a const and a reference
  fn-pointer all picked `f(long)`, silently. A designator scored the neutral
  0 against `long` and tied `k(bool)`'s boolean conversion, so `k(g)` was
  refused as ambiguous. [conv.bool] is a function argument's only
  arithmetic conversion, and the scorer now ranks it as one (3), beside the
  parameter lane. Any other arithmetic parameter ranks NEUTRAL (0), as a
  designator always did, rather than being refused. Refusing it was the first
  cut, and Tier 2 caught it where the Tier 1 run had not: a
  member-template or varargs placeholder's marker parameter is that same
  64-bit storage, and a lambda (a function in madc) must still reach its
  instantiate-and-reselect arm (g++.dg lambda-conv10, lambda-mangle2,
  lambda-variadic8). Reducer `tests/testoverloadfnptrarg`. Found
  beside it: a call THROUGH a reference to a fn-pointer (`int (*&r)(int) =
  pg; r(4)`) is refused as juxtaposed operands (`call_through_fnptr_reference`,
  next).
- ~~`call_through_fnptr_reference`~~ — fixed 2026-09-23, four layers deep. (1)
  Parse: the identifier arm asked `var->type->is_function()`, false for a
  reference variable (its type is the lowered pointer), so `r(4)` pushed `r`
  as a value and refused the `(`. A reference to a function or fn-pointer now
  calls through its referent's value, the way `(expr)(args)` does: the
  engine's two call-through arms share `function_value_pointer_type` and
  `build_call_through_value`. (2) Declaration: only `param_decl` rendered a
  pointer to a fn-pointer (`int (**fp)(int)`); `var_decl` peeled to the
  DataDefFPTR and printed its 64-bit storage, `long long *pp`, for a local,
  static or file-scope pointer OR reference to one, in C as well as C++ —
  latent since `(*pp)(4)` used to be dropped whole (the pre-session binary
  printed the argument). The arm is now `pointer_to_fnptr_pieces`, shared.
  (3) A reference to a FUNCTION type lowered one level too deep (`int
  (**fn)(int)`): a pointer to a function type IS the fn pointer
  (`getPointerType`'s fold), so that level is the fn pointer's own `*`. (4)
  Binding it, `&g` spelled the source name; `&f` of a function is its
  designator's symbol (`function_value_symbol`, shared with the value arm).
  Reducers `tests/testcallfnptrref` (C++), `tests/testfnptrptrc` (C). Found
  beside it: a reference over a function or fn-pointer mangles as a pointer
  (`PPFiiE` where g++ has `RPFiiE` / `RFiiE`; `reference_to_function_mangling`,
  next).
- ~~`reference_member_aggregate_init`~~ — fixed 2026-09-23. A reference MEMBER
  of an aggregate binds its initializer as a reference parameter binds its
  argument. CIR's brace-list builder (`aggregate_init_list`) handed every
  slot to `init_value`, which translated the initializer's VALUE into the
  reference's pointer slot: `struct RM { long &r; }; RM rm{lv};` stored 40
  and the first read crashed. The class-aggregate lane applied a bare `&`,
  which no prvalue has (`RC rt{5, 1}` for `const int &c`). Both now bind
  through `ref_param_arg_addr`, the one reference-bind owner; the list
  builder's four member arms share `init_slot_value`. Reducer
  `tests/testrefmemberaggr`. (The parser-side reference-bind builder,
  `reference_bind_address_expr`, is the separate candidate below.)
- ~~`reference_to_function_mangling`~~ — fixed 2026-09-23. The structural
  spelling of a function (pointer) through its layers,
  `fptr_structural_spelling`, counted every peeled layer as a `*`, the
  reference's included: `int (*&)(int)` minted `_Z…PPFiiE` (g++ `RPFiiE`) and
  `int (&)(int)` `PPFiiE` (g++ `RFiiE`), so a madc definition and a g++
  caller never linked. It now records each layer (`*` or `&`), folds a
  function TYPE's innermost layer into its own declarator core (`(*)` or
  `(&)`, `structural_spelling_core`), and the mangler reads the `(&)(` form
  as `RF…E`. The namespace-function reference path spells through the same
  owner. Gates: two oracle-corpus rows (`f_fpref`, `f_fnref`, g++ == clang++)
  replayed by `test_mangle`, and the interop link lane — a madc definer and
  a g++ user, and the reverse — now carries both shapes.
- ~~`c_signed_char_generic_identity`~~ — fixed 2026-09-23, silent. madc's
  `DataType` enum folds `char` and `signed char` into one value (`dtCHAR =
  dtINT8`), so every rawtype switch reads them as one type, and two of them
  spoke for both. (1) CIR's `append_type_specs` emitted `signed char` as plain
  `char`: on an unsigned-char target (aarch64-linux, under qemu) `signed char
  sc = -3` printed 253 and `(int)(signed char)200` printed 200 where gcc
  prints -3 and -56. (2) The signature renderer shared by `_Generic` and
  `__builtin_types_compatible_p` (`canonical_builtin_simple_type_name`)
  rendered both as `char`: `_Generic(sc, char: …, signed char: …)` chose
  `char`, `signed char *` chose `char *`, and
  `__builtin_types_compatible_p(char, signed char)` was 1. Both arms now ask
  the one scalar-identity owner, `Program::proven_scalar_identity`, which
  already told `ddINT8` from `ddCHAR`. C++ overloading was already right: it
  ranks by that identity. Reducers `tests/testsignedcharc`, the emitted-C
  spelling `tests/testsignedcharemit`, and the C++ twin `tests/testsignedchar`.
- ~~`c_const_pointee_typedef_cast`~~ — fixed 2026-09-23, silent, found beside
  the signed-char family. A declaration hands its leading `const` to
  `parse_declarator` (`leading_const`), and `consume_declarator_stars` wraps
  the pointee (`const char *p` is not `char *`). The typedef reader and the C
  cast arm consumed their leading `const` and passed nothing, so `typedef
  const char *ccp`, `typedef const int CI; CI *`, `typedef const struct T
  *P`, `(const char *)p` and `(ccp)p` all named `char *` / `int *` / `struct
  T *`: `_Generic` chose the unqualified association, and
  `__builtin_types_compatible_p(ccp, const char *)` was 0. The typedef's
  prefix `const` now wraps its base (`getConstType`, the qualified type IS
  the alias; `typedef int const CI` through the reader's `base_const`), the
  struct/union tag forms receive it as `typedef_prefix_const` (the
  `typedef_prefix_align` one-producer model), and the cast passes it as
  `leading_const`, which binds only a pointee (a cast's result is
  unqualified, C11 6.5.4). C mode only: C++ const identity is the
  const-qualified-types campaign (`cxx_const_overload_identity` measured it
  there, silent too). Reducer `tests/testconsttypedefcastc`. Residues: `typedef
  const enum`, a trailing `int const` shared by a declarator list's tail.
- ~~`template_arg_address_of_function`~~ — fixed 2026-09-23, a regression the
  const family's wider Tier 1 found (`tests/testtplparamdeclarator`, outside
  nb6). A non-type template argument substituted into a cloned body is ONE
  operand ([temp.param]/8), but three clones spliced its raw tokens: the
  class-template body clone, the out-of-line member clone and
  `clone_template_tokens_with_type_subst`. `P()` over `&g` read `&(g())`, which
  the `&` owner (450242bfe) rightly refuses ("expecting addressable
  expression"); the old hand reader read `&g` and then called it. `O->m` over
  `&obj` read `&(obj->m)`, refused before this arc as well. One owner now
  splices, `splice_nontype_template_arg`: it groups a multi-token argument
  exactly when the next body token is a postfix step (`(` `[` `.` `->` `++`
  `--`, the only operators that bind tighter than the argument's own). A
  template-argument position (`Other<P>`) is never followed by one, so the
  spelling that keys the instantiation is unchanged. The alias-template
  `tok_subst` carries TYPE arguments as tokens and stays out (`T(int)` must
  never group). Gate: `check-one-nontype-splice.sh`, two-sided. Reducer
  `tests/testtplnontypegroup`. Found beside it, each its own gap: a function
  template's pointer non-type argument is never instantiated
  (`fcall<&h>(4)`); `s.*&S::m` (the `.*` operand is not the cast-expression
  owner's); `FnPtr<(&g)>` keys a second specialization.
- ~~`volatile_dropped_in_emitted_c`~~ — fixed 2026-09-23 in two steps,
  silent. A `volatile` local changed between `setjmp` and `longjmp` keeps its
  last stored value (C11 7.13.2.1p3) only because every access to a volatile
  object is a real load or store (5.1.2.3p6). madc lost that twice over. (1)
  The declaration reader consumed `volatile` and nothing carried it, so neither
  the IR nor the `--emit=c11` output had it. (2) c2mir records
  `type_qual.volatile_p` but never read it: `process_func_decls_for_allocation`
  put every scalar local in a register, and `longjmp` restored the register's
  `setjmp`-time contents. The reducer printed `jmp: -834290028` / `nested: 8`
  where gcc and clang print 4511 and 47 at -O0 and -O2; c2m compiling the C
  directly was wrong the same way. Step 1 (0378fc87d, the carrier): the object
  carries `vfVOLATILE`, set from the reader's `base_volatile` /
  `volatile_after_star`; cir appends `N_VOLATILE` to the spec list, or for a
  pointer object to its own `N_POINTER` — the FIRST in c2m's suffix order,
  which binds innermost first; `emit_declarator` renders pointer qualifiers;
  a declarator list's tail re-pushes the qualifier through one owner,
  `push_declarator_list_tail` (two copies merged). Step 2: c2mir's allocation
  keeps a volatile scalar in memory. MIR-gen needed nothing (measured -O0
  through -O3). Reducers `tests/testvolatileemit`, `tests/testvolatilesetjmpc`,
  `tests/testvolatilesetjmpo2c`; MIR corpus `new/volatile-setjmp.c`. The
  type-level half (`volatile int *q`, a volatile member, `typedef volatile`)
  is `pointee_volatile`, below.
- ~~`mir_volatile_memory_access`~~ — fixed 2026-09-23, one layer below the
  step above (a Tier 3 raise, on the owner's go). MIR had no volatile concept:
  at `-O2` GVN forwarded the load before a loop into the loop, so a spin on a
  `volatile sig_atomic_t` flag set by a signal handler (C11 5.1.2.3p5) never
  ended, in c2m and madc alike. Counted at run time, the pre-change c2m -O2
  performed 3 of the 9 volatile accesses gcc -O2 performs in the gate's first
  case and 2 of 3 in its member case (stores and reads removed or merged); at
  -O0 it still missed 3, because c2mir never generated the read of a
  discarded `*vp;`, `(void) *vp` or `(*vp, 0)` at all. `MIR_mem_t.volatile_p` now
  marks an access through a volatile lvalue (`volatile:` in text MIR, a
  `TAG_VOLATILE` prefix byte in binary MIR, `*(volatile T *)` in mir2c).
  c2mir sets it at the five lvalue arms from the C type, and its one
  sub-object builder, `mem_part_op` (six copies merged: a member, an
  `__int128` half, a complex part, a block chunk, `__real__`/`__imag__`, a
  V128 result view), carries an object's bit to its parts. Every MIR-gen pass
  asks one predicate, `volatile_mem_insn_p`: address transformation keeps the
  variable in memory, GVN gives the access no value number and never uses it
  as a source (a store still kills what it may alias), DSE and both dead-code
  eliminations keep it, and the combiner never moves it or folds it into a
  read-modify-write insn. Gate `check-volatile-accesses.sh` counts the
  accesses at run time (fault + single-step on a `PROT_NONE` page) against gcc
  -O2 at c2m -O0..-O3, with a `-Dvolatile=` control, and holds the two
  one-owner rules. Reducers `tests/testvolatilesignalc`, MIR corpus
  `new/volatile-access.c`, cross fixture `tests/cross/aarch64_volatile.c`.
  Not fixed here, each its own gap: c2mir's member access does not merge the
  object's qualifiers into the member's TYPE (`c2mir_member_access_qualifiers`;
  the operand carries the bit, the C type does not).
- ~~`pointee_volatile`~~ — fixed 2026-09-23 (C mode), silent. The step above
  made MIR honour a volatile access; madc's front end still handed c2mir only a
  volatile OBJECT. Every other spelling was dropped in the reader: the pointee
  of `volatile int *q`, a struct member, `typedef volatile int vint`, a cast
  `(volatile int *)p`, a parameter, a function's return, a level inside a
  chain (`int *volatile *pp`). A spin through `volatile sig_atomic_t *` hung at
  -O2 (the load hoisted), a `vint` local came back from `longjmp` as garbage,
  and `_Generic` picked `int *` for all of them. The model is gcc's
  `build_qualified_type`: `DataDefCONST` became ONE qualified variant,
  `DataDefQUAL`, carrying a cv MASK, minted only by `getQualifiedType` (one
  variant per (unqualified base, mask), `const(volatile T)` merged, never a
  wrapper over a wrapper); `is_const()` / `is_volatile()` read the mask and
  `unqualified()` peels all of it. A second class per qualifier would have
  doubled every peel the const campaign had already made transparent. Every
  site that asked "is this the const wrapper" was classified: a peel stays a
  peel, a const TEST reads `is_const()`, a renderer spells the mask. The
  producers are the pointee-const model's: `consume_declarator_stars` takes a
  `leading_cv` mask (a `volatile` in its run qualifies the level like a
  `const`), the declaration, both parameter readers and the cast arm pass their
  leading run, the typedef reader its prefix mask (`typedef_prefix_cv` for the
  tag forms) and its own top-level cv (a typedef has no flag), and
  `member_declarator` the line's cv, a member's top-level volatile going into
  its type. The CIR peel `dd_peel_pointers` records each level's cv
  (`level_cv`), so var, member, typedef, parameter, return, typed-extern, cast,
  fn-pointer and `va_arg` declarators spell `N_VOLATILE` where the type has it;
  const is not spelled yet (c2mir would start enforcing it on madc's own
  lowering; `cir_pointee_const_dropped`). The forest record carries the mask
  in `flags` (0 = const, the records written before it). One consumer broke
  on a qualified aggregate and was already broken for const: the brace-init
  reader tested `dynamic_cast<DataDefSTRUCT *>` on the qualified type, so
  `typedef const struct P CP; CP cp = { 3, 4 };` was refused — it reads the
  unqualified type now. And the bit-field layout judged an `int` field's signedness
  from its type's NAME, so `volatile signed f : 25` read back zero-extended
  (the c2mir-tests lane's `new/bf1.c`); it reads the unqualified name. C mode
  only at first (`--std=c*`); madc and C++ modes are `cxx_pointee_volatile`,
  below. Gate `check-volatile-accesses.sh` gained a madc leg (-O0..-O3,
  29 accesses, a cast and a typedef case). Reducers
  `tests/testvolatilepointeec`, `tests/testvolatilepointeeo2c`,
  `tests/testqualifiedaggregateinitc`. Residues: a top-level const member stays
  unmodeled; `_Generic`'s association reader cannot read `volatile int (*)[4]`
  (a hand-rolled type-name reader, refuses loudly).
- ~~`cxx_pointee_volatile`~~ — fixed 2026-09-23 (the producers), silent. The
  step above gated every producer on `is_c_mode()`, so in madc mode — which is
  also what a `.c` file with no `--std` compiles in — and in C++ modes a
  volatile pointee, member or typedef still never reached the IR: the same
  `-O2` spin hung, the same `vint` local came back from `longjmp` as garbage,
  and `_Generic` picked `int *`. The scope is one statement now,
  `Program::modeled_cv()` — volatile in every mode (an access is performed as
  written, [intro.execution]/7), const in C only (the C++ const identity stays
  the const campaign's) — and every producer masks with it:
  `consume_declarator_stars`, the nested and parameter-array arms of
  `parse_declarator`, `member_declarator`, the typedef reader's prefix and
  top-level cv. Reducers `tests/testvolatilepointeeo2cxx` (`--std=c++17 -O2`)
  and `tests/testvolatilepointeeo2` (madc mode, `-O2`). The volatile TYPE in
  C++ then needs its identity — mangling, overload ranking, deduction,
  volatile member functions — each its own step.
- ~~`cxx_volatile_mangling`~~ — fixed 2026-09-23, loud at link. With the
  type distinct, its symbol was still g++'s for the unqualified type: every
  parameter spelling the mangler reads was assembled by hand from tokens —
  "leading const + base + stars" — in both parameter readers, so no volatile
  (nor any cv below the first star) ever reached it; `f(int*)` and
  `f(volatile int*)` both minted `_Z1fPi` and the second became `f__o2`, which
  nothing links. `param_declarator_spelling` now spells a parameter from the
  reader's token structure with EVERY level's cv read from the declarator's
  type (the outermost level is the parameter object, dropped by [dcl.fct]/5),
  and `ItaniumMangler::parse_type` reads the result: trailing decorations first
  (a leading cv binds to the core — reading it first merged `volatile int*
  volatile*`'s base qualifier into the outer level, PVPi for g++'s PVPVi), one
  level's cv words as ONE `V`/`K` set and one substitution candidate
  (`_Z1nPVKcS0_`), a leading cv set dropped for a parameter. The rule "a
  qualified pointer spells its cv after the `*`" is `cv_qualified_spelling`,
  shared by the `DataDefQUAL` name, `basic_class_datadef_spelling` and the
  parameter spelling — the name spelled `int *volatile` as `volatile int*`,
  the pointer TO volatile's name. A reference's leading cv qualified nothing:
  `volatile int &r` was `int &` (the reader applies a pending cv only at a
  `*`), so it now qualifies the referent at the `&`. Reducer
  `tests/testvolatilemanglecxx` (calls through the g++ symbol names, declared
  `extern "C"`), unit cases in `test_mangle.cpp`.
- ~~`volatile_lvalue_type`~~ — fixed 2026-09-23, silent (V2 of volatile
  across the board). A variable's own top-level volatile rode a flag,
  `vfVOLATILE`, that the CIR spelled but no TYPE carried, while a member's
  and a typedef's volatile were already their types' — one fact in two
  representations. So `&vx` was `int *` (overload `f(volatile int*)` lost to
  `f(int*)`, `T*` deduced `T = int`, `__builtin_types_compatible_p(__typeof__
  (&vx), volatile int *)` was 0), a volatile lvalue bound `int &`, and `T &`
  deduced `int` — a template body then accessed the object as non-volatile.
  A bridge that re-merged the flag into a type wherever the language needs
  the glvalue's type was drafted and dropped for the root: parseDeclaration
  now qualifies the variable's TYPE (a fixed array's element, madc storing
  the element in `var->type`; a reference's referent is already qualified at
  the `&`), before the file-scope snapshot the global is emitted from; the CIR
  spells it from the type (`peel_pointer_declarator`'s level cv, the alias's
  added cv) and the two flag paths in `var_decl` are gone; the bit is
  retired. Then member access merges the OBJECT's cv into the member's type
  ([expr.ref]/4, C11 6.5.2.3p3) at the three `.`/`->` arms
  (`member_access_type` over `glvalue_cv` — the glvalue's type cv, a
  reference's referent); the six call-argument vectors (three spelled raw
  `datadef()`, two `operand_value_datadef`, one with array decay) are one
  owner, `call_argument_type`; a by-value parameter drops the argument's
  top-level cv in `score_arg_to_param`, a by-value `T` in deduction, and a
  parameter spelled `volatile T *` / `volatile T &` removes its own cv from
  `T`. c2mir had the same hole on its own: its member access copied the
  member's declared type (`qualify_member_type` now merges the object's
  const/volatile, onto an array member's elements), and its `&` of a member
  rebuilt the pointee from the declaration again (it now keeps the
  expression's qualifiers) — `_Generic (&vs.m, volatile int *: 2)` picked
  `int *` in plain C through c2m (MIR corpus `new/volatile-member-type.c`).
  Two C-style casts read a now-qualified type as its class (the postfix
  engine's method receiver, the ctor-declaration arm) — `as_class_dd()`
  forwards. The model is the modeled bits', not volatile's alone: in C a
  variable's top-level const is its type's too (`&cs.m` of a `const struct S
  cs` is `const int *` — the new MIR corpus case failed through madc on
  exactly that), C++ const staying the const campaign's (the vfCONSTANT
  read-only marking is unchanged). That exposed the parse-time value slot:
  `Variable::set` / `get` / `inc` / `dec` / `cmp` / `slot_size` dispatched on
  `type == &ddINT`, so a qualified int stored nothing and `int arr[N]` with
  `const int N = 4` folded to `int arr[0]` (sizeof refused) — they read
  `slot_type()`, the unqualified type, now; so do libmadc's host value
  marshallers. Reducers `tests/testvolatileobjecttypec`,
  `tests/testvolatilelvaluecxx`, `tests/testconstobjecttypec`. Residues: a
  function-pointer OBJECT's own volatile (`int (*volatile fp)(int)`) is not
  modeled (it was spelled nowhere either); a volatile PARAMETER object
  (`void f(volatile int n)`) is still non-volatile inside the body.
- ~~`cxx_volatile_template_args`~~ — fixed 2026-09-23, silent (V1d + V1f). A
  template type argument's cv rode a spelling string beside a type that
  lacked it (`consume_template_type_arg_qualifiers`), so `__is_same` and
  partial-specialization binding — which read the DataDef — saw `volatile
  int *` as `int *`: `same<volatile int *, int *>` was 1,
  `is_volatile<remove_pointer<volatile int *>::type>` 0. The type-id's cv now
  goes INTO the type through one owner, `parse_type_id` (the abstract
  declarator with the caller's leading cv — the pointee's at the first `*`,
  else the type-id's own together with an east `T volatile`; a cv after the
  last `*` the pointer's), reached through `fold_template_arg_declarator`
  (the modeled words leave the spelling, so nothing is spelled twice). Four
  consumers had leaned on the qualifier being absent: the partial-spec
  matcher bound `_Tp` of `remove_volatile<_Tp volatile>` to the QUALIFIED
  type (the pattern's qualifier is matched, not deduced — [temp.deduct.type]
  /8 — it now sheds it); `unwrap_baked_trait_arg` peeled any qualifier off a
  baked trait argument, so libstdc++'s `is_same<volatile int, int>` was true
  (a baked const still rides `referent_const`, every other bit stays); the
  using-alias reader skipped its target's leading cv (`using VI = volatile
  int;`); and the alias-template BODY readers could not resolve a leading cv
  at all — an alias template whose target begins with `const` or `volatile`
  (`template <class T> using c_t = const T *;`) was refused as an undeclared
  identifier at every use. The class-pattern normalizer (V1f) refused any
  qualified level whose mask was not const-only, so a class template's
  `typedef T volatile type;` fell back and lost the qualifier; its ConstType
  node carries the mask in `flags` now (0 = const, the older records). The
  gxx-c++11 lane then named one more consumer: g++.dg alias-decl-57's
  `tuple_size<volatile tuple<>>` had matched only because the argument LOST
  its volatile (straight to the `tuple<T...>` spec); kept, it must match
  `tuple_size<volatile __has_tuple_size<T>>`, and the flat matcher gave up at
  an alias-template-id in a pattern. An alias template whose target is one
  of its own parameters is transparent there now ([temp.alias]/2).
  Reducers `tests/testvolatiletemplateargcxx`,
  `tests/testvolatiletypetraitscxx` (real `<type_traits>`). Residue: the
  spelled trait-argument reader (`__is_same(volatile int, int)` written
  directly) still hand-rolls its stars and drops volatile — with the
  `_Generic` and `typeof` readers, V5.
- ~~`cxx_volatile_methods`~~ — fixed 2026-09-23 (V1e), silent. The method
  qualifier reader dropped `volatile`, so `int get()` and `int get()
  volatile` were one signature (the second renamed `C__get__o2`), every call
  took the first, and inside the volatile member `this` was a plain `C *` —
  its member reads were not volatile. `FuncDef::is_volatile_method` beside
  `is_const_method`, and `method_cv()` the mask every identity reader takes:
  the mangler's member prefix spells `V` before `K` (`_ZNV1C3getEv`,
  `_ZNVK1C4bothEv`; its `bool const_method` parameters are an unsigned mask,
  a bool still meaning const), the out-of-line definition matcher
  (`function_declarator_member_cv`), the class-pattern record (the const
  word carries the mask), the forest record (`DF_IS_VOLATILE_METHOD`). The
  implicit object's cv is one owner, `implicit_object_cv` (was
  `implicit_object_constness`, 1/0): a mask, the receiver's type cv, an
  arrow receiver's POINTEE (`pc->get()` through a `volatile C *` read the
  pointer variable's own cv — and a `C *const p` was taken for a const
  object), a member function's own for `this`; `findMethodOverload` rejects
  a member lacking one of the object's bits and prefers the closest sibling,
  as the CIR's conversion-function selection does. In a volatile member
  function `this` is `volatile C *` ([class.this]) and a member read through
  the implicit `this` merges its cv. Reducer `tests/testvolatilemethodcxx`
  (the g++ symbols called through their names). Residue: a pointer to a
  volatile member function (`DataDefMemberFnPtr` carries const alone).
- ~~`reference_to_pointer_subscript`~~ — fixed 2026-09-23, silent, older than
  the volatile work (the HEAD baseline returned garbage, exit 0). A
  subscript through a REFERENCE to a pointer (`int *&rp; rp[1]`) indexed the
  reference's own cell: a reference is stored as a pointer to its referent,
  the TokenVar read dereferences it (`x` -> `(*x)`), but the named-variable
  subscript arm built the bare stored pointer as its base, so `rp[1]` read
  the NEXT `int *` and returned it as an int. The arm now dereferences a
  reference whose referent is a pointer — the read's own deref; a carrier
  `value &` (its stored pointer IS the carrier) and a class receiver
  (`operator[]`, above it) have no pointer referent. Found beside it: a
  reference parameter's first level is its REFERENT, never a top-level cv,
  so `int *volatile &` mangles `RVPi` (`param_declarator_spelling` spelled
  it `RPi`). Reducer `tests/testrefptrsubscript`.
- ~~`cxx_volatile_overload_rank`~~ — fixed 2026-09-23, silent. The ranker
  peeled the pointee's qualifier before comparing, so `int*` and `volatile
  int*` scored an identical exact match against both `f(int*)` and
  `f(volatile int*)` and declaration order decided (a volatile argument could
  select the overload that drops the qualifier). `score_arg_to_param` now
  applies [conv.qual]: the argument's first-level pointee cv must be a subset
  of the parameter's (else not viable), an added qualifier ranks one below
  the identity ([over.ics.rank]/3.2.5), deeper levels must match exactly (an
  added qualifier there needs const at each level between, which C++ mode
  does not model yet); and a reference binds a referent of its own cv or a
  less qualified lvalue — a non-const lvalue reference never a more qualified
  one, a more qualified referent ranking one below ([over.ics.rank]/3.2.6).
  Residue: a named volatile variable's lvalue still reports its unqualified
  type (the object flag), so `f(&vx)` / `k(vx)` rank as `int` — the lvalue-cv
  step, V2.
- `declarator_star_suffix_outside_parse_declarator`: seven `tkMul` loops that
  bypass `consume_declarator_stars` (range-for verified; six candidates).
- ~~`single_level_pointee_accessor`~~ — consolidated 2026-09-23, a live
  regression by then. Once a member's or a typedef's top-level cv became its
  TYPE (`struct N *volatile next`, `typedef int *volatile vip` — QUAL over
  PTR, in C since `pointee_volatile` and in every mode since
  `cxx_pointee_volatile`), the 119 "is it a pointer, what does it point at"
  sites that asked `dynamic_cast<DataDefPTR *>` — NULL for a qualified
  pointer — all mis-answered: `n1.next->v` was refused ("expression before
  '->' is not a typed pointer") in every mode, `n1.p + 2` stepped by the
  pointer's own size (the element-size owner fell to `dd->size`), a
  subscript typed the element `int64`, a `T *` deduction from a qualified
  pointer argument failed, and a bit-field reached through a pointer to a
  volatile struct lost its layout (`owner_struct_type` cast the qualified
  pointee). 103 sites ask `as_pointer_dd()` now (`pointer_dd_of` for a
  possibly-NULL type); the 16 that dispatch on the node's EXACT class keep
  the cast, marked — `subst_datadef` and `tsubst_datadef_key` test PTR before
  QUAL, so the forwarding accessor there would rebuild `QUAL(PTR(T))` as
  `PTR(T')` and drop the qualifier on substitution; the forest records, the
  class-pattern normalizer, and the structural spellings likewise. Gate
  `check-one-pointee-accessor.sh` (fulltest; two-sided). Reducers
  `tests/testqualifiedpointermember` (C) and
  `tests/testqualifiedpointermembercxx`.
- `all_levels_pointer_peel_gate_blind_spot`: two `&&`-clause peel loops the
  pointer-peel gate's regex misses (verified present; correct today).
- `star_count_over_spelling_string`: the mangler's `parse_type` vs
  `resolve_canonical_type_spelling` (candidate).
- `overload_sig_pointer_depth_probe`: `ParsedParamSig.pointer_depth` plus
  `unwrap_pointer_depth` (candidate).
- Doc drift: `parse_member_fnptr_declarator` and `parse_fnptr_member_tail` are
  named as owners in five comments and gate headers, but neither exists
  (verified).
