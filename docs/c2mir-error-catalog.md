# Learning from c2mir's checker — error catalog & pre-catch strategy

c2mir's context checker runs on **madc's own `cir_node` tree, with madc's
original source positions** (errors point at `db.c:531`, not the emitted C).
So c2mir is, in effect, madc's **reused** C11 semantic checker — duplicating its
~67 checks in madc would be anti-DRY. The question "which c2mir errors should
madc pre-catch?" splits three ways.

## Category A — madc bugs, NOT user errors (the bulk of SMAUG bring-up)

These fire because madc emitted a **malformed tree** (a type erased in the
builder/parser). The fix is **correctness** — stop emitting the bad tree — not
pre-catching (which would just move a false positive earlier). Examples fixed
during SMAUG bring-up (159 → 25 c2mir errors):

| c2mir message | root cause | fix |
|---|---|---|
| `excess elements in … initializer` | `char *x[N][M]` treated as a char array, strings expanded to bytes (`rawtype()` strips pointer-ness) | `is_char_array_element_type` `!is_pointer()` guard |
| `subscripted value is neither array nor pointer` | forward-typedef'd struct dropped `member_dims` on completion; 2-D member flattened to `[3969]` | copy `member_dims`/`member_access` in forward completion |
| `assignment of incompatible value` | `T *m[N]` member emitted `T (*m)[N]` (pointer/array order swapped) | emit `[ARR, POINTER]` in `member_node` |
| `subscripted value …` (params) | array param `char m[50][50]` collapsed to `int *map` | array-parameter decay in `param_decl` |

**Lesson:** during bring-up of known-valid C (SMAUG is valid C89 — gcc accepts
it), *every* c2mir error is a madc bug. The fidelity gate (`make cirfidelity`)
operationalizes this: gcc accepts the original, so any c2mir/asm divergence on
madc's tree localizes a madc defect.

## Category B — genuine user errors worth pre-catching with rich diagnostics

Real C constraint violations a user could write. madc has the high-level parse
tree + source, so it can give a better message than c2mir's terse one. madc
**already** pre-catches several (e.g. "Too many initializers for array
(expected N)" with a caret, beating c2mir's "excess elements"). Candidates not
yet pre-caught at the madc layer:

- `too many/too few arguments` (call arity)
- `called object is not a function or function pointer`
- `lvalue required as left operand of assignment`
- `case-expr is not a constant expression`
- `void must be the only parameter`
- `repeated declaration` / `incompatible types of X declarations`

These are the right place to invest in front-end diagnostics — but c2mir remains
a correct backstop, so it's a UX improvement, not a correctness requirement.

## Category C — preprocessor / macro

`macro name is not an identifier`, `too many args for macro`, `repeated macro
parameter`, etc. — handled by madc's lexer/preprocessor.

## Diagnostics already in place

- c2mir check errors carry the **original** file:line:col (via `set_pos` on each
  `cir_node`).
- Untranslatable CIR nodes now name the concrete `TokenBase` subclass + lexeme
  via RTTI (`describe_token`), e.g. `unhandled expression: TokenCpnd …` — madc
  is open source, so internals are surfaced, not hidden.

## Recommendation

1. Keep fixing **Category A** by correctness (the SMAUG worklist).
2. Treat c2mir as the reused semantic backstop; do **not** reimplement its 67
   checks.
3. Incrementally add **Category B** front-end diagnostics where madc's context
   yields a materially better message than c2mir's.
