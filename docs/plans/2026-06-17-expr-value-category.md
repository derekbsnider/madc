# Plan — Expression value-category (lvalue / xvalue / prvalue) for correct forwarding-ref deduction

**Status:** active, scheduled LAST (extracted from
`docs/plans/2026-06-17-first-class-references.md` Phase 3 at user request 2026-06-17 —
"complete ALL of the plan, but if Phase 3 needs to move to the end, give it its own plan").
This is the one genuinely-additive, separable piece: a new capability with a conservative
default, NOT a half-migration. It runs AFTER the first-class-references plan (Phases 2+4)
is complete and after the SFINAE/`vector<T*>` work.

## Why this is separate

The first-class-references plan makes references a real type. This plan adds the OTHER half
of clang's model: **expressions never have reference type; reference-ness on an expression is
a per-expression *value category*** (`VK_LValue` / `VK_XValue` / `VK_PRValue`,
`clang/include/clang/AST/Expr.h`, `Specifiers.h`). madc today has NO value-category tracking
on expression tokens (confirmed by recon — no `value_category`/`is_lvalue`/`xvalue` on
`TokenBase`); it approximates by structural pattern-matching (`TokenVar`/`TokenMember`/
`TokenDeref` ≈ lvalue).

The approximation already passes the whole current suite, so this is additive correctness,
not a fix for a regression — which is why it's safe to stage with its OWN trigger rather
than leave a hybrid.

## What it delivers

clang's forwarding-reference deduction rule ([temp.deduct.call]p3,
`AdjustFunctionParmAndArgTypesForDeduction`, `SemaTemplateDeduction.cpp:4004`): for a
forwarding reference `_Tp&&`, if the ARGUMENT is an **lvalue**, deduce `_Tp = A&`; if an
**rvalue/xvalue**, deduce `_Tp = A`. madc's `fn_template_deduce_param` currently strips the
reference UNCONDITIONALLY for `amps==2` (`parser.cpp:~28040`) — i.e. it cannot distinguish
the two, because it has no value category to read.

## Design (madc embedding)

- Add a 3-state `ExprValueKind` (`vcPRValue` / `vcLValue` / `vcXValue`) to `TokenBase`,
  defaulting to a conservative value that reproduces today's behavior.
- Set it at the producers: named var / `*p` / member access → lvalue; a call → from the
  callee's return ref-kind (lvalue-ref → lvalue, rvalue-ref → xvalue, value → prvalue);
  literals / arithmetic / `T(...)` → prvalue; `std::move(x)` / `static_cast<T&&>` → xvalue.
- Rewrite the forwarding-ref branch of `fn_template_deduce_param` to key on the argument
  token's value category instead of stripping unconditionally.
- Keep everything else (overload ranking, emission) reading the conservative default first,
  then migrate readers as needed.

## Trigger / definition of done

- DONE when: `ExprValueKind` exists on `TokenBase` and is set at the producer sites; the
  forwarding-ref deduction keys on it; a focused test of lvalue-vs-rvalue forwarding-ref
  deduction passes 3-oracle; fulltest green.
- CONCRETE TRIGGER to start (so this is not a vague "later"): begin the moment a
  forwarding-ref deduction *correctness* test fails that the structural approximation can't
  satisfy — OR immediately after the first-class-references plan + `vector<T*>` are both
  green, whichever comes first.

## Relationship to the rest

- Depends on first-class references (Phases 1/2/4) being done — value category and the
  reference type are the two halves of clang's model; do the type first, the category second.
- May intersect the SFINAE/`vector<T*>` work (`is_constructible<A*, A*&&>` keys on value
  category), but is not required for it — that blocker is the `__and_`/`enable_if` fold.
