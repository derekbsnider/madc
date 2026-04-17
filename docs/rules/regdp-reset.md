# regdp Reset — Reasoning

See `.claude/rules/regdp-reset.md` for the rule itself.

## What `regdp` carries

`regdefp_t regdp = { first, second, object };`

- `first` — the destination `Operand *` where the next compile should
  put its result.
- `second` — the expected `DataDef *` (the type).
- `object` — the implicit `this` / `__retbuf` for method calls and
  multi-return.

When a node compiles into `regdp.first`, it's asserting "my result
lives here." Any later node that reads `regdp.first` then sees that
stale destination.

## Why loops and conditionals are special

Each sub-expression of a loop / if (the condition, the body, the
increment, the else-branch) is independent — they do NOT feed each
other values. Without resetting, the condition's destination register
would be reused as the body's destination, and the body would
overwrite the comparison result. Or worse, the body's result would
flow back into the condition on the next iteration.

## Historical bugs this rule fixed

- **For-loop counter clobber** (Phase 3.5+): the loop counter was
  being written to the comparison result's destination register,
  because the condition's stale `regdp.first` leaked into the
  increment's compile. The loop ran forever.
- **Do-while(0) infinite loop when two do-whiles in sequence** (Phase
  A): similar — the first do-while's regdp leaked into the second,
  corrupting the zero-comparison.
- **Inc on short struct member inside if-condition** (Phase F): the
  condition's regdp leaked into the statement's compile, interacting
  with unsigned-comparison bugs and member-inc bugs to produce
  silently-dropped writes.

Each time, "add the regdp reset" was the minimal fix.

## Why every new loop / conditional node needs this

If you add a `TokenSwitchCase`, `TokenLoopUntil`, or any other control-
flow node that takes sub-expressions, it MUST follow the same
reset-before-sub-compile pattern. Forgetting shows up as
hard-to-trace wrong values in contexts that happen to reuse a
register.
