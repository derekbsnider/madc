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

1. **A `<` opens a template-argument list only in a template-id head context
   AND outside `(...)` / `[...]`. Both tests are required** — each catches a
   shape the other misses, and this is the single most important thing in the
   file:
   - *head context* catches `declval<T>() < declval<U>()` — prev is `)`, so the
     `<` cannot begin a template-id. Real `<type_traits>` writes
     `integral_constant<bool, _Tp(-1) < _Tp(0)>`; counting that `<` as an open
     desynced the scan by one level for the next ~1300 header lines.
   - *nesting* catches `decltype(a < b)` — prev **is** an identifier, so the
     head-context test passes and only the paren test rejects it. Without it the
     angle opened there never closes (its `>` is inside the parens too), the
     depth stays stuck past the `)`, and a scan looking for a top-level `;` or
     body `{` runs to EOF.

   `DelimDepth` shipped with only the first test; the second lived in exactly
   one hand-rolled scanner (`skip_template_nonclass_declaration`, added by
   `2e853dee`). Adopting the shared tracker there would have **reintroduced**
   the bug that commit fixed. The union was merged into `DelimDepth` on
   2026-07-27 — see "The claim that was wrong" below.
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

## The claim that was wrong

During round 1 this document (and the commit message, and the handoff) said
`DelimDepth` was **"strictly more correct than any hand-rolled copy."** That was
overstated, and the overstatement was load-bearing: it would have justified a
migration that reintroduced a fixed bug.

`DelimDepth` had the head-context test but **not** the paren guard.
`skip_template_nonclass_declaration` had the paren guard but not the head-context
test. Neither subsumed the other — `decltype(a < b)` is caught only by the
guard, `declval<T>() < declval<U>()` only by the context test.

The lesson generalises past this family: **"the shared owner already exists" is
not the same as "the shared owner is complete."** Before adopting an owner at a
site that carries a local guard, diff the *rules*, not just the outcomes — and
ask what that site's guard was added to fix. Here the answer was a commit
message (`2e853dee`) naming the exact shape.

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

## The family is closed (round 6, 2026-07-27)

The ratchet baseline is **0**: `DelimDepth` is the only token-delimiter tracker
in `src/` and `include/`. Thirteen scanners were migrated over six rounds.

The last one — the constructor mem-initializer capture scan in `parseFunction`
— is the clearest illustration of why the count mattered. It held four
hand-rolled locals and counted **every** `<` as a template-argument open, with
neither the template-id head test nor the paren guard. So an ordinary
relational operator in an initializer argument:

```cpp
struct Foo { int v; Foo(int a, int b) : v(a < b ? 10 : 20) { } };
```

opened an angle level whose `>` never came. The depth stayed stuck past the
`)`, the body `{` never satisfied the "all depths zero" break test, and the
scan ran to EOF — reported as `Unexpected end of data` pointing at the
`struct`, six lines above the actual defect. Replacing `>` for `<` in the same
reducer compiled and ran correctly, which is what isolated it.

The same scan also decremented `brace_depth` on a `}` whose `{` it had never
counted (a nested brace inside a brace-init), dropping the depth to zero
mid-list so a following comma re-armed `expecting_initializer` and the body
brace was consumed as an initializer.

Both were fixed by adoption, not by new logic. Note the shape of that: the
rule was correct in `DelimDepth` and wrong in the copy, for **seven weeks**,
in a scanner nobody would think to look at while fixing angle brackets.

### What survives the migration

`expecting_initializer` — the flag distinguishing `m{1,2}` (a brace-init) from
the body's opening `{`, both of which appear at top level — stays in the
caller, tested against the depth *before* each token is applied. That is the
rule the file states generally: unify how depth is **updated**; leave each
site's *use* of the depth alone.

## Round 7 (2026-09-04): which `<` is a bracket at all is a NAME question

The counting was solid by round 6 and the bug came back anyway, because the
recurring class was never the counting. Before `DelimDepth` counts a `<` it
decides whether that `<` is a bracket, and until this round that decision was
made from the token before it: any identifier opened a template-argument list.
C++ does not decide it that way. [temp.names]/3 says a `<` begins a
template-argument-list only after a name that IS a template — one the keyword
`template` introduces, or one for which **name lookup** finds a template (or,
for an unqualified name, functions or nothing). Both canons implement exactly
that: gcc's `cp_parser_template_name` looks the name up and returns "expected
template-name" (so the `<` is less-than) whenever lookup finds no template,
including a dependent scope without the keyword; clang's `Sema::isTemplateName`
returns `TNK_Non_template` for the same cases. Neither compiler's skip or cache
scanners count angle brackets at all — they track `(` `[` `{` only, and where a
`,` could be inside a template-argument list they *parse* tentatively.

The shape that exposed it was libstdc++ `<ratio>`:

```cpp
struct __ratio_less_impl<_R1, _R2, true, false>
  : integral_constant<bool, _R1::num < _R2::num> { };
```

`_R1` is a template parameter, so `_R1::num` is a dependent name and cannot be
a template-name without `template` — the `<` is less-than. Reading it as an
opener left the class-prefix capture one level deep past the real `>`, and it
ran through every header after `<ratio>` to a stray `>` in `<bits/types.h>`,
swallowing `extern "C" {` and namespace braces on the way. `std` stayed open,
`clock` registered as `std::clock`, and `<ctime>`'s `using ::clock;` failed in
every TU that included `<chrono>` or `<filesystem>`.

The first fix added the dependent-qualifier rule to the tracker — and only to
scans that passed the Program handle through `delimStepStream`. The
template-argument splitter (`collect_template_argument_spelling`) built a bare
tracker and called `update()` itself, so the include-free reducer
(`struct Less : BC<A::num < B::num>`) still failed: "BC<> expects 1
argument(s)". Two more reducers showed the gap was the whole rule, not the
dependent case: `struct L : BC<K::num < 5> {}` with a concrete class K, and the
plain expression `int x = K::num < 5;` — the expression parser's class-qualified
site skipped `<...>` as template arguments on sight of the `<`.

What round 7 changes:

- The decision has ONE owner, in the Program, answered by lookup:
  `class_member_lt_reading(owner, name)` (a member template opens; a data /
  static member, enumerator or typedef is less-than; nothing or only
  non-template functions is Unknown) and `unqualified_name_lt_reading(name)`
  (a type or value template parameter is less-than, a template template
  parameter opens; templates in scope open; a variable is less-than; functions
  and undeclared names keep the opening reading, as 3.3 says).
- `DelimDepth::lt_reads_as_less_than()` reads them for every scan that carries
  the Program, walking the `q0::q1::name` chain behind the `<`: a dependent
  qualifier anywhere in the chain is less-than; a concrete chain resolves to a
  class and asks; a type-only context (`typename`, an elaborated-type keyword, a
  base-specifier head) keeps the opening reading exactly as gcc's `tag_type !=
  none_type` arm does.
- The expression parser's class-qualified site reads the same predicate
  instead of its own `<` test.
- Every stream scan constructs its tracker with the Program (`DelimDepth
  d(this)` / `d(&pgm)`), and `check-one-delim-tracker.sh` fails on a bare
  tracker driven by `update()` inside a function that walks the stream — the
  precise shape the splitter bug lived in. Index scans over stored token runs
  keep the token-only reading; they can only ever be less strict than lookup.

Residue, stated: a qualifier that is itself a template-id (`I<7>::num < 5`
spelled in source inside an argument list) is not resolvable from tokens and
keeps the opening reading; the expression parser handles that spelling by
resolving `I<7>` first. The clone lane's substituted body carries `I<7>` as one
type token, so the reducer's shape is covered.

Reducers: `tests/testtplargless.mad`, `tests/testlessthanqualified.mad`
(g++ == clang++).
