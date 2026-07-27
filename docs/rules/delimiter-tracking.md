# Delimiter Tracking — the reasoning

## The rule in one line

`(` `[` `{` `<` nesting bookkeeping belongs to **one** implementation,
`DelimDepth` in `src/parser.cpp`. Everything else layers on top of it.

## Why this needs to be a rule at all

`DelimDepth` was written *as* the consolidation. Its header comment has said,
from the day it landed:

> Balanced-delimiter depth for token scans: (), [], {}, <>. The hand-rolled
> "++paren … --angle … >>" if-else chain is copy-pasted across many scanners;
> this is the single shared bookkeeping.

Four call sites adopted it. **Twenty-five hand-rolled delimiter locals stayed**,
and more were added afterwards. A comment inside a function is only read by
someone already in that function — which is exactly not the person about to
write the next scanner somewhere else. That is the whole argument for
`scripts/check-one-delim-tracker.sh`: the rule now has a gate, so the next copy
fails the build instead of surviving for seven weeks.

## What the shared tracker knows that a hand-rolled copy does not

Each of these is a real bug that a fresh `++angle_depth` chain reintroduces:

1. **A `<` opens a template-argument list only in a template-id head context.**
   After `)`, `]`, or a literal it is less-than. Real `<type_traits>` writes
   `integral_constant<bool, _Tp(-1) < _Tp(0)>`; counting that `<` as an open
   desynced the scan by one level for the next ~1300 header lines.
2. **A `>` inside parens opened *within* the list is greater-than**, not a
   close. `DelimDepth` records the paren depth at each angle open
   (`angle_paren`) precisely so `A<(B > C)>` balances.
3. **Operator-function-ids are names.** `operator<`, `operator>>`, `operator()`
   must be consumed opaquely; their symbols are not delimiters. Both step
   helpers do this via `operator_id_token_span` / `isOperatorIdStart`.
4. **`>>` splits into two closers** — once, correctly, in one place.

No hand-rolled copy in the tree had all four. The two that were nominated as
"the reference implementation" during the 2026-07-27 audit each had exactly one
of them, which is why consolidation had to take the **union** rather than copy
either.

## The incident

`docs/plans/2026-07-27-angle-bracket-consolidation.md` has the full history.
Short version: `declval<T>() < declval<U>()` inside a partial-specialization
head made `collect_template_argument_spelling` consume the rest of the file.
Inside libc++'s `__utility/is_pointer_in_range.h` that meant
`_LIBCPP_BEGIN_NAMESPACE_STD` never opened a namespace, the matching END then
closed scopes that were never opened, and the *reported* error was
`using ::isalnum;` failing at `cctype:111` — **six headers downstream**, in a
file that was itself fine.

A parse error that silently corrupts scope nesting is the dangerous part. The
diagnostic pointed six headers away from the defect.

## Marker discipline (why the audits kept undercounting)

This family was counted three times and was wrong each time, always for the
same reason — **the marker matched a spelling, not the concept**:

| Count | Marker | What it missed |
|---|---|---|
| 6 | `++angle_depth` | a counter named plain `depth` |
| 8 | `++angle_depth` | `DelimDepth`'s own `++angle` — i.e. the fix itself |
| 25 | `int *_depth` | nothing yet; also found a site in `madc_program.cpp` |

The concept is *"a local balanced-delimiter counter"*, not *"angle brackets"*.
Every paren/square/brace-only scanner is the same rule with one axis dropped,
and the angle-specific marker was blind to all of them.

## Migrating a scanner

Behaviour-preserving refactor, not redesign. The sites do **not** all use the
depths identically — some split `>>` into two output tokens, some test
`brace_depth`, one pops from a `seen` vector, one rewrites `>>` into `TokenGT`
and pushes one back to the stream. Unify **the rule for updating depth**; leave
each site's *use* of the depths exactly as it is.

Prefer `d.paren` / `d.square` / `d.angle` over `d.top()` when the original did
not track braces — `top()` also requires `brace == 0` and would silently widen
the condition.

See `.claude/rules/delimiter-tracking.md` for the bare rules.
