# Clang Methodology Rules

## Clang is canon

- Before fixing ANY codegen or runtime bug, run `clang -S -O0`
  on the failing test (or a minimal reducer) and study the output.
- For richer annotations, also run `gcc -S -fverbose-asm -O0` — GCC
  labels every instruction with source variable names; clang does not.
- madc's output must match clang's lowering shape for the same input.
- If madc produces different results from clang, the bug is in madc.

## Compare before changing

- Write a minimal `.c` reducer that isolates the failing behavior.
- Compile the reducer with both `clang -O0` and `bin/madc`, compare outputs.
- Read the assembly to understand what instructions it chose and why.
- Only then form a hypothesis about what madc is doing differently.

## Fix at the deepest layer

- Never shim a symptom at a higher layer when the root cause is lower.
- If a type is wrong, fix where the type is determined — not where it's consumed.
- If an operator produces the wrong result, fix the operator — not the caller.
- If expression parsing mis-consumes tokens, fix the parse grammar — not the
  compile side.
- "Shortcuts make for long delays." A shim today is a debugging session tomorrow.

## Operator type self-determination

- Arithmetic and bitwise operators must compute at their natural operand type.
- Callers (assignments, comparisons, casts) must NOT override the operator's
  signedness via `regdp.second`. The operator infers its own type via
  `settype()` / `infer_numeric_type()`, then the caller coerces the result.
- Bitwise operators (&, |, ^, <<, >>) ALWAYS produce integer results, even
  when the enclosing expression wants a double.
- Unsigned operands must zero-extend when widened, never sign-extend. If
  `is_unsigned()` is true, use `mov r32, r32` (not `movsxd`).

## Think twice, code once

- Form a hypothesis about the root cause before editing any file.
- If the first hypothesis is wrong, stop and re-examine — don't chain
  speculative micro-fixes.
- One reasoned fix beats five iterative attempts.

See `docs/rules/clang-methodology.md` for worked examples.
