# `rust::match`

A namespaced statement form modeled after Rust's `match`. v1 is a
no-fall-through, multi-pattern `switch` over integer constants, with
`_` as the wildcard arm.

```c
rust::match (expression)
{
    pattern => statement;
    pattern1 | pattern2 | pattern3 => { statements }
    _ => default_statement;
}
```

The `rust::` prefix is required — `match` outside this namespace
remains a usable identifier in user code.

## v1 surface

- **Patterns** are integer constant expressions (literals, enum
  constants, parenthesized arithmetic). `&` and `^` are allowed inside
  a pattern; `|` is reserved as the arm-OR separator.
- **Wildcard** `_` matches anything that wasn't matched by an earlier
  arm. Exactly one wildcard arm is allowed per `rust::match`.
- **Bodies** are a single statement, which can be a `{ ... }` block
  for multiple statements.
- **No fall-through.** Every arm body ends with an implicit jump out
  of the match, so the C `switch`-style fall-through bug class doesn't
  exist here.
- **`break`** inside an arm body exits the match (same semantics as
  `break` in a `switch` case).

## Wildcard placement

The wildcard arm may appear anywhere in source order. Explicit
patterns always take precedence over the wildcard, even when they
appear after it:

```c
rust::match (n)
{
    _  => result = -1;     // wildcard listed first
    10 => result = 999;    // still wins for n == 10
}
```

If no arm matches and there is no `_`, control falls through past the
closing brace of the match.

## Comparison with `switch`

| Aspect              | `switch`                 | `rust::match`                         |
|---------------------|--------------------------|---------------------------------------|
| Pattern types       | integer constants        | integer constants                     |
| Multi-value arm     | stacked `case` labels    | `1 \| 2 \| 3 => ...`                  |
| Default arm         | `default:`               | `_ =>`                                |
| Fall-through        | implicit (until `break`) | none                                  |
| Body                | bare statements          | one statement (use `{}` for multiple) |
| `break`             | exits the switch         | exits the match                       |

## Deliberately deferred

These would extend the surface but aren't in v1:

- **String patterns.** Today, string dispatch goes through `if`/`else
  if`/`strcmp` chains. Adding string patterns is a future revision once
  string-aware codegen joins the integer dispatch path.
- **Range patterns** (`1..=10 => ...`).
- **`rust::if let`**, destructuring, and any pattern that needs a
  proper tagged-union / `Option` / `Result` value model.
- **Match expressions.** v1 is statement-only. Match-as-expression
  (`int x = rust::match(...) { ... }`) requires a stack-slot merge
  similar to the ternary-operator scheme; not in scope yet.

See `docs/language/prefer.md` for the namespace-precedence directive
that the `rust::` namespace shares with `php::` / `python::` /
`ruby::`.
