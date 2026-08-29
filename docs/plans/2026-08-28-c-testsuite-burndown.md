# c-testsuite burndown — 198/220 → 220/220

**Progress: 207/220 (2026-08-29, wave 2 — declarators).** Wave 1 (2026-08-28)
fixed: 00204 (MIR va_block_arg SSE exhaustion — upstream PR candidate), 00210
(cast-to-fnptr call), 00205 (three deep: exponential operator datadef()
recursion, unary-plus-under-cast, flat struct-array count), 00143 (Duff's
device: in-place labels + top-of-switch goto jump table; also unblocked
00213's Duff half — its remaining root is a stmt-expr value corner,
reclassified below). Wave 2 (2026-08-29) fixed five of section C's six:
00170, 00162, 00209, 00130, 00089 (see section C for the revised mechanism
note), plus two silent-wrong-answer defects found en route (member fn-ptr
called under a cast bound the bare NAME; enum-typed struct members compared
UNSIGNED). Validated: JIT suite 1215/0/0 + c-testsuite lane 207/13, 0 outside
baseline. Remaining 13: 00124 + sections D/E/F.

Owner (2026-08-28): "we want c-testsuite to be 220/220 ideally."

The lane (`scripts/c_testsuite_lane.sh`, ratchet against
`docs/parity/c-testsuite-baseline.txt`) went green at 198/220 on its seeding
run (@1add3d11 content). This doc is the burndown worklist for the 22
baseline entries. Every fix: gcc/clang oracle on a reducer, own commit with
trailers, baseline line removed in the same commit (the lane reports
baseline-now-passing loudly — a fix that lands without shrinking the
baseline is a claim, not a fix). Suite tally only ever moves 198 → 220.

Ordering law: wrong-output before everything (exit 0 with the wrong value is
worse than a crash), then the hang, then parse gaps by area, then semantics.

## A. Wrong output — silent-wrong-answer class (FIRST)

- **00204** — giant varargs/HFA torture; output matches except TWO lines in
  the "HFA float" section: expected `13.1,13.3`, got `14.3,<garbage>`. A
  float HFA (struct of floats) passed variadically corrupts one member.
  Suspect the win/SysV vararg spill or float-pair packing in the CIR call
  lowering. Reducer shape: variadic fn taking float-pair structs, printf the
  members.
- **00210** — `((ATTR int(*)(void)) function_pointer)()` (ATTR = an
  `__attribute__`'d fn-ptr type): call result lost, prints nothing useful,
  rc=0; madc warns "assigning pointer without cast to integer" — the cast
  expression is being read as a value, not a call target.

## B. Hang

- **00205** — J-interpreter snippet: `PT cases[] = {...}` flat initializer,
  brace elision across struct member boundaries (each PT eats 7 flat
  initializers), members are huge paren/cast constant expressions. Killed at
  the 10s CPU cap. TIMEOUT ⇒ CALLGRIND (owner law) — profile before
  hypothesizing; likely quadratic re-scan in initializer consumption.

## C. Parse gaps — declarators

- **00089** — `typedef struct S * (*fty)();` fn-ptr typedef returning
  struct ptr ("Expecting identifier after").
- **00124** — `int (* (*p)(int a, int b))(int c, int d) = f1;` fn-ptr
  returning fn-ptr.
- **00130** — `char arr[2][4], (*p)[4], *q;` array-pointer declarator
  mid-declarator-list.
- **00162** — `void fooc(int x[const 5]);` qualifier inside array param
  brackets (C99 6.7.5.3p7).
- **00170** — `int (*fmember)(enum efoo x);` fn-ptr struct member whose
  param list declares an enum param.
- **00209** — `int f1 (int (), int);` abstract function-type parameter.

FIVE OF SIX FIXED 2026-08-29 (00170, 00162, 00209, 00130, 00089 — gates:
testenumtagtype, testparamarrayqual, testabstractfnparam, testcommaptrarray,
teststructfnptrtypedef; annotations in the baseline file). The original
hypothesis here — "one grammar spine, fix it once" — was REVISED by the
investigation: there is no missing spine. The declarator grammar already has
FOUR shared owners (`parseFnPtrParams` for param-list grammar,
`parse_fnptr_member_tail` for the one `( * name ) ( params )` tail,
`parse_ptr_array_suffix`, and the comma-continuation re-entry), and each
fix ADOPTED the owner its context had failed to route through, extending
the owners in place (enum arm, abstract-param arm, VLA-qualifier skip,
`(`-starts-declarator at the comma). Still zero point patches — the fixes
live in the owners, not at the call sites. Remaining: **00124** — fn-ptr
returning fn-ptr needs the fn-ptr declaration arm to recurse through the
stream (stash inner declarator groups, parse the outer tail, re-push, goto
the arm head — one nesting level per pass; spiral-stash precedent is 5
lines below the throw site).

## D. Parse gaps — statements/expressions

- **00038** — `if (sizeof 0 < 2)` sizeof unary-expression without parens.
- **00051** — `switch (x) case 0: ...` non-compound switch body.
- **00143 + 00213** — case labels inside nested statements (Duff's device;
  case-in-`if`-block). Same root: madc's switch wants cases at body top
  level; C allows labels on any statement in scope. One grammar fix, two
  tests.
- **00103** — `return **(int**)bar;` double-deref of a cast.
- **00120** — `enum { X } x;` anonymous enum as a member/declarator type.
- **00150** — `{[0] = 1, 1+1}` designated initializer followed by
  positional (resume at index 1).
- **00219** — `_Generic(a, int: a_f, const int: b_f)()` — const-qualified
  association + selecting a function designator then calling it.

## E. Preprocessor

- **00152** — `#error` inside a SKIPPED conditional group must not
  diagnose (directive processing in false groups).
- **00202** — `A ## B` with an empty argument: placemarker semantics
  (C11 6.10.3.3). `P(jim,)` → `jim`; `Q(+,)3` → `+ +3`.

## F. Semantics / CIR

- **00053** — struct tag shadowing: inner-block `struct T { int y; }` must
  be a NEW type, not collide (c2mir: 3 check errors — madc likely emits one
  scope's tag table).
- **00095** — tentative definitions: `int x; int x = 3; int x;` at file
  scope merge to one definition (c2mir: 2 check errors — likely duplicate
  emissions). Also takes `&main`.
- **00216** — empty struct `struct {}` (GNU, sizeof 0/1 per gcc), compound
  literal `(empty_s){}` in a static initializer, `{ 1 }, 2` brace elision.
  CIR "unhandled expression: TokenBase, token type 0".

## Sequencing

Runs AFTER the win-VT merge wave (owner release frame: next release =
madcide on win VT; this arc must not stall that). Each fix is an ordinary
develop-branch slice: reducer → oracle → deepest-layer fix → test in
tests/ (fixture conventions) → baseline shrink → lane re-run. The lane is
push-gated, so every develop push re-proves the current tally.
