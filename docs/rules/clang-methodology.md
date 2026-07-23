# Clang Methodology — Reasoning

## Why clang is canon

madc's goal is C compatibility. clang is now the default build compiler
(since v0.23.0), and the macOS/ARM64 port targets clang exclusively.
When madc and clang disagree on a test's behavior, madc is wrong — not
clang. This is true even when madc's approach seems "simpler" or "more
efficient."

`clang -S -O0` is the primary reference because:
- `-O0` disables optimizations, showing the straightforward lowering.
- clang's output is clean and predictable at `-O0`.
- clang is the compiler madc actually ships with.

`gcc -S -fverbose-asm -O0` remains a useful supplementary tool because:
- `-fverbose-asm` annotates each instruction with the source variable and
  expression it came from. clang accepts the flag but does not produce
  comparable annotations.
- When tracing data flow through complex lowerings, GCC's annotations
  can be faster to read than unmarked clang output.

## Why fix at the deepest layer

In the GCC parity session (May 2026), several bugs were traced through
multiple layers before finding the root cause:

**Example: `(unsigned int) x / 2LL` producing -2 instead of 2147483646.**

The symptom was in division. The first-order cause was `safediv` using
`idiv` instead of `div`. The second-order cause was `result_type` being
signed. The root cause was the *caller* pre-setting `regdp.second` to a
signed type before the division's `settype()` could infer the correct
unsigned type. Fixing it at the `safediv` level (checking more args)
partially worked. Fixing it at the `settype` level (overriding the
pre-set) worked better. Fixing it at the `compile_token_normalized`
level (not passing the wrong type in the first place) was the right fix.

A shim at `safediv` would have hidden the bug for division but left it
present in every other operator that uses the same regdp.second flow.

**Example: `va_arg(aps[4], long)` failing to parse.**

The symptom was "Expecting ',' after va_list variable". The first
attempt extracted the Variable* from the parsed expression — a shim
that wouldn't generalize to arbitrary expressions. The right fix:
parse the first argument as a general expression (the deepest layer),
store the expression in TokenVaArg, and have the compile side use
`ap_expr->compile()`.

## Why think first

The `***f = 42` bug took careful trace-through to find: the recursive
`parseExpression` consumed the `=` operator as part of the inner
expression, so the outer dereference wrapped an assignment node instead
of a pointer. The fix (iterative star collection) was simple — the
reasoning to get there was not. Five speculative edits would have taken
longer and left more mess than one careful trace.
