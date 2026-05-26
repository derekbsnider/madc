# Enums Over Strings — Reasoning

## The failure case

The transpiler's type classification system was introduced as a quick
hack using single chars (`'c'`, `'s'`, `'d'`, `'i'`) to tag variable
types for `cout` format selection. This spread through the sema pass
and emitter — ~50 call sites doing char comparisons instead of enum
dispatch.

Problems:
- **No compile-time checking.** A typo like `'C'` instead of `'c'`
  silently produces wrong output.
- **Not extensible.** Adding a new type class (e.g. `TC_CLASS`,
  `TC_POINTER`) means inventing a new char and hoping it doesn't
  collide.
- **Slow in hot paths.** The `classify_type()` function does 6+
  `std::string::find()` calls per invocation. In the emitter's cout
  chain, this runs per-expression per-value. An enum comparison is
  one integer compare.
- **Gecko AST node names are already strings.** Converting them to
  enums at the boundary (one `strcmp` per node) and dispatching on
  the enum internally is strictly faster than doing `strcmp` at every
  use site.

## The rule

Use enums for any classification that appears in more than one place.
Convert string-based discriminators to enums at system boundaries
(lexer output, Gecko AST nodes, user input) and never pass the string
deeper.

## When strings are OK

- One-off debug/error messages
- User-facing output
- Map keys for genuinely dynamic lookups (symbol tables, etc.)
