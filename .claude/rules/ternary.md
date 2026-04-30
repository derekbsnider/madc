# Ternary Operator Rules

## Parsing

- When `?` is encountered in `parseExpression`, pop operators from
  `opStack` with precedence <= 13 only.
- Do NOT pop `=` (precedence 14) or other lower-precedence operators.
- Pop the condition from `exStack` after clearing higher-precedence
  operators.
- Parse the true expression with `conditional=true` — stops at `:`.
- Consume `:` via `nextToken()`, then parse the false expression.
- Push the `TokenTerQ` node onto `exStack` and set `done=true`.

## Colon as expression stop

- `:` (`tkTerC`) stops expression parsing in non-bracketed context.
- When encountered, push it back via `pushToken(tb)` and set `done=true`.

## Code generation

- The merge point MUST be a stack slot — not a shared virtual register.
  asmjit's register allocator cannot handle the same virtual register
  written on two divergent paths.
- Compile the condition with a clean `regdefp_t` (`first=NULL`) to
  avoid writing the comparison result into the caller's destination.
- Compile each branch with a clean `regdefp_t` — use fresh tmp
  registers; write to the stack slot.

See `docs/rules/ternary.md` for the full codegen pattern and the
register-convergence explanation.
