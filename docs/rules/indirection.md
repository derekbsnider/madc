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
