# Angle-bracket disambiguation: one rule, one implementation

**Status: round 1 executed 2026-07-27.** The parse bug is FIXED, four scanners
are migrated, and the rule now has a gate. See "Round 1 outcome" at the bottom
— it corrects two load-bearing claims in the analysis below, which is kept as
written for the history.

> ### The correction that matters
>
> **The consolidated tracker already existed** — `DelimDepth`
> (`src/parser.cpp:2891`, forward-declared in `include/madc.h`), with an index
> helper `delim_scan_step()` and a stream helper `Program::delimStepStream()`.
> Slice 2 below says "extract the one tracker". It did not need extracting; it
> needed **adopting**. Its own header comment already said it was the single
> shared bookkeeping, and four call sites used it while twenty-five hand-rolled
> locals did not.
>
> `DelimDepth` is also **more complete than either site nominated as a
> reference** — it knows a `<` only opens after a template-id head
> (`_Tp(-1) < _Tp(0)`), records paren depth per angle open so `A<(B > C)>`
> balances, and consumes operator-function-ids opaquely. Copying either
> nominated site would have been a downgrade.

## The bug

Eight lines, no library headers, no libc++:

```cpp
template <class T> T declval();
template <class...> struct vt { typedef void type; };
template <class T, class U, class = void> struct cmp { static const int v = 0; };
template <class T, class U>
struct cmp<T, U, typename vt<decltype(declval<T>() < declval<U>())>::type> { static const int v = 1; };
```

madc: `Unexpected end of data`. g++ and clang: compile, print `1 0`.

The `<` in `declval<T>() < declval<U>()` is a **less-than operator**. C++
[temp.names] resolves this by nesting: inside `(...)` or `[...]`, `<` and `>`
are operators, never template brackets. A scanner that bumps an angle depth on
every `<` consumes `declval<U>`'s `>` as its closer and then runs to EOF looking
for a delimiter that does not exist.

## Why it matters beyond the reducer

libc++'s `__utility/is_pointer_in_range.h` contains exactly that construct. It
dies mid-header, so its `_LIBCPP_BEGIN_NAMESPACE_STD` never opens a namespace —
and the matching `_LIBCPP_END_NAMESPACE_STD` then **closes scopes that were
never opened**. From that point the scope stack is wrong: later global
declarations are unreachable from `::`, and `using ::isalnum;` fails at
`cctype:111`, **six headers downstream**, in a file that is itself fine.

Every "order-dependence" in the bisects that chased this was really just *which
header first drags in `is_pointer_in_range.h`*.

A parse error that silently corrupts scope nesting is the dangerous part. The
diagnostic pointed six headers away from the defect.

## The real finding: eight copies of one rule, and none of them is complete

> **Corrected 2026-07-27 by the first `/dupaudit` run.** This section first said
> "six copies, one guarded". Both numbers were wrong. There are **eight** sites,
> and the counting error had a cause worth keeping: the marker grepped for
> `++angle_depth`, but `Program::skip_template_id_suffix` (parser.cpp:38422)
> implements the same rule with a counter named plain `depth` — invisible to
> that grep, and it is precisely the site that knows something the others do
> not. **A marker must match the rule, not one spelling of its bookkeeping.**

**No single site has the complete rule.** Two different pieces are each
implemented exactly once:

- **the paren/square guard** (a `<` inside `(...)`/`[...]` is an operator) —
  only at parser.cpp:38783;
- **operator-function-ids** (`operator<`, `operator>>` are part of a *name*, not
  brackets; consumed via `isOperatorIdStart`/`parseOperatorId`) — only at
  parser.cpp:38422.

So the reference implementation cited below is itself incomplete, and any
consolidation must take the union of the two, not adopt either wholesale.

`grep -n "++angle_depth" src/parser.cpp` returns **seven** sites (the eighth
uses `depth`). **One** is guarded.

| Site | Function | `<` guarded by paren/square depth? | Introduced |
|---|---|---|---|
| **38782** | *(reference)* | **YES** — `&& paren_depth == 0 && square_depth == 0` | `2e853dee` 2026-06-07 |
| 3379 | `collect_template_argument_spelling` | no — *tracks `paren_depth` but does not apply it here* | `e7b06f30` 2026-06-05 |
| 38371 | `collect_template_default_argument` | no | `e7b06f30` 2026-06-05 |
| 45470 | `skip_template_suffix_tokens` | no | `e7b06f30` 2026-06-05 |
| 48486 | inner scope (real-header path) | no | `c9fd2227` **2026-06-09** |
| 18089 / 18114 | `namespace_scope_from_cpp_spelling`, `primary_name_from_cpp_spelling` | no — char-level, over a spelling string | `e7b06f30` 2026-06-05 |
| **38422** | `skip_template_id_suffix` — counter named `depth`, **missed by the original marker** | no — but it is the ONLY site handling `operator<` / `operator>>` as name tokens | audit 2026-07-27 |

The history is the point:

- **2026-06-05** — three token scanners written, all unguarded.
- **2026-06-07** — `2e853dee`, *"`'<'` in trailing-return decltype not
  template"*, fixes it **in one place**, with a comment naming the exact shape
  (`decltype(forward<_Tp>(t) < forward<_Up>(u))`) *and* the consumed-to-EOF
  symptom.
- **2026-06-09** — `c9fd2227` adds **another** scanner, two days *after* the
  fix, repeating the unguarded pattern.

So the correct rule has existed for seven weeks and five siblings never learned
it. This is the failure mode Rule #4 exists to prevent: new code written without
first reading what is already there. Site 3379 is the sharpest illustration — it
*computes* `paren_depth` and simply does not consult it on the `<`.

**Therefore the fix is not "add the guard six more times."** That would leave
six copies and guarantee a seventh. The fix is to make one implementation and
delete the rest.

## Plan

### Slice 1 — reducer, test, and the culprit (small, independently gateable)

1. `tests/testtemplateanglelt.mad` from the reducer above, plus the shapes that
   share the hazard: `decltype(a < b)` in a trailing return, `f<T>() > g<U>()`,
   a `>>` that really is a shift inside parens, and a nested
   `vt<decltype(...)>::type` in a partial specialization. Expected output taken
   from g++ **and** clang.
2. Identify which scanner the reducer actually reaches (instrument or bisect —
   `collect_template_argument_spelling` is the prime suspect for a partial
   specialization head).
3. Fix that one site so the test passes.

Gate: the new test, plus `make -C src test`. Do not run the full battery yet.

### Slice 2 — extract the one tracker

Introduce a single owner of the rule. Sketch:

```cpp
// THE angle-bracket depth tracker. C++ [temp.names]: within `(...)` or `[...]`
// a `<` / `>` is an operator, never a template bracket — `decltype(a < b)`,
// `f<T>() < g<U>()`. A scanner that bumps angle depth on every `<` consumes an
// operator `>` as a closer and runs to EOF ("Unexpected end of data"), and a
// parse that dies inside a header leaves the namespace/scope stack unbalanced.
class TemplateAngleTracker
{
	int angle_ = 0, paren_ = 0, square_ = 0, brace_ = 0;
public:
	void feed(TokenBase *t);            // one token; applies the nesting rule
	bool in_template_args() const;      // angle_ > 0
	bool at_top_level() const;          // all depths zero
	int  angle() const, paren() const, square() const, brace() const;
};
```

Migrate the token-level sites (3379, 38371, 38782, 38422, 45470, 48486) onto it,
deleting their inline bookkeeping. Build with `-Wall` and let
`-Wunused-variable` on the removed `angle_depth`/`paren_depth` locals confirm
each cut was complete (`no-parallel-implementations.md`).

**Care required — this is behaviour-preserving refactoring, not redesign.** The
sites do not all *use* the depths identically: some split `>>` (`tkBSR`) into
two closers, some test `brace_depth`, one pops a token from a `seen` vector. The
extraction unifies **the rule for updating depth**, and must leave each site's
*use* of the depths exactly as it is. Any behaviour change beyond the guarded
`<` is a bug in the refactor.

### Slice 3 — the char-level pair

`namespace_scope_from_cpp_spelling` and `primary_name_from_cpp_spelling` walk a
spelling string, not tokens, so they need a sibling that applies the same rule
over characters. Same nesting logic, different input alphabet. Keep it beside
the token tracker so the two are read together, and state in a comment that they
are two encodings of one rule.

Verify they *can* see the hazard before writing code: a spelling containing
`decltype(a < b)` may or may not reach them. If it cannot, say so in the comment
and leave them alone rather than adding speculative machinery — but do not leave
the question unanswered.

### Slice 4 — make the seventh copy impossible

Add `scripts/check-one-angle-tracker.sh`, wired into `fulltest`, failing if
`angle_depth` bookkeeping appears anywhere outside the tracker's own
translation unit. Precedent: `scripts/check-no-std-hardcoding.sh` already does
exactly this kind of structural guard and is already in `fulltest`.

This is the part that actually addresses the root cause. The bug was not a typo;
it was that nothing stopped a sixth copy from being written two days after the
fix.

### Slice 5 — collect the payoff

Re-run the libc++ probes. Expect `__utility/is_pointer_in_range.h` to parse and
the whole `cctype:111` cascade to clear. The next blocker behind it is already
known: `<compare>` → `math.h:414: Unknown namespace 'std::__math'`, a **nested**
namespace qualifier — same family as the `&S::n` fix (`cf8f035c`), which taught
that a qualifier before `::` may be more than a plain namespace name.

Gate: full battery (`fulltest` + `--exe` + `--obj`) once, at the end. The
tracker is on the shared parse path, so this one genuinely earns the battery.

## Gates

- Slice 1: new test + unit tests.
- Slices 2–3: incremental build, the new test, and the C++ template tests
  (`grep -l template tests/*.mad`) — the tracker's blast radius.
- Slice 4: `fulltest` (the new check runs inside it).
- Slice 5: full battery, once.

Baseline to hold: **761/0/0/9 JIT, 745/0 EXE, 745/0 OBJ, `libcxx_gate` 11/11**.

## Notes for whoever picks this up

- The reference implementation and its reasoning are at `parser.cpp:38774–38786`.
  Read that comment first; it already explains the rule and the symptom.
- Do not "fix" this by adding the guard at the culprit and stopping. That is the
  move that produced the current state.
- `2e853dee` is worth reading for the prior effort that went into getting this
  right the first time.

---

## Round 1 outcome (2026-07-27)

### The culprit

`Program::collect_template_argument_spelling` (`src/parser.cpp:3316`).
Confirmed by backtrace, not inference:

```
#1 Program::nextToken()                             <- include/madc.h:4357 throw
#2 Program::collect_template_argument_spelling(...)
#3 TokenTEMPLATE::parse(Program&)
```

The "Unexpected end of data" text is `nextToken()`'s generic exhaustion throw,
not a scanner-specific message — which is why grepping the message found only a
comment. That comment (at the reference site) was itself the strongest clue.

### What changed

Four scanners migrated onto `DelimDepth`:

| Site | Function | Note |
|---|---|---|
| 3316 | `collect_template_argument_spelling` | **the culprit** — the fix |
| 38422 | `skip_template_id_suffix` | `depth` counter + operator-ids → one `do/while` |
| 45460 | `skip_template_suffix_tokens` | index form via `delim_scan_step` |
| 46855 | `scan_old_style_definition_suffix` | paren/square only |

Plus:

- `tests/testtemplateanglelt.mad` — five hazard shapes, expected output taken
  from **g++ and clang++-18** (byte-identical to madc).
- `.claude/rules/delimiter-tracking.md` + `docs/rules/delimiter-tracking.md`.
- `scripts/check-one-delim-tracker.sh`, wired into `fulltest` — a **ratchet**
  at 25 that fails on any increase and tells you to lower it on any decrease.
- `AGENTS.md`: P4 index entry, and Top-10 Rule #4 now demands that the search
  be **stated** before a new helper is introduced.

### The family is bigger than the audit said — 25, not 8

The gate's first run found **27** hand-rolled delimiter-depth locals (25 after
this round), including one in `src/madc_program.cpp` — a file no angle-bracket
audit had ever looked at.

Three counts, three undercounts, one cause: **the marker matched a spelling,
not the concept.**

| Count | Marker | Missed |
|---|---|---|
| 6 | `++angle_depth` | a counter named plain `depth` |
| 8 | `++angle_depth` | `DelimDepth`'s own `++angle` — the fix itself |
| 27 | `int *_depth` | (found `madc_program.cpp`) |

Every paren/square/brace-only scanner is the same rule with one axis dropped.
The angle-specific marker was blind to all of them.

### What the test asserts, and what it deliberately does not

The partial-specialization hazard is wrapped in a `namespace` with a probe
declared *after* it (`ns::after_hazard`). That is the real libc++ failure mode
in miniature: a runaway scan leaves the namespace unclosed and everything
after it unreachable from `::`. If the bug returns, the probe cannot be found
and the test fails to compile.

Which specialization `cmp` **selects** needs decltype-SFINAE partial-spec
matching. madc parses the head correctly now but still picks the primary
(`0 0 0` where g++/clang give `1 0 1`). That is the deferred
`is_destructible` / SFINAE-`declval` tsubst item, a different family — so the
test does not assert it rather than enshrine the wrong value.

Also found and **out of scope**: trailing-return function templates
(`template <class A, class B> auto f(A,B) -> decltype(a < b)`) do not register
the function name at all. A negative control (`decltype(a + b)`, no comparison)
fails identically, so this is not an angle-bracket defect.

### Remaining — round 2

25 locals across ~9 functions. The three intricate ones need care because they
do not merely *track* depth, they *rewrite* output while tracking it:

- `collect_template_default_argument` (38343) — rewrites `>>` into `TokenGT`s
  and pushes one back to the stream.
- `skip_template_nonclass_declaration` (38684) — the body-brace detector plus
  an operator-spelling state machine; this is the site that carries the
  paren/square guard.
- the ctor-initializer scan (48396) — `expecting_initializer` is driven by
  brace transitions, so `DelimDepth`'s unconditional brace counting is a
  behaviour change unless handled.

Then the char-level pair (`namespace_scope_from_cpp_spelling`,
`primary_name_from_cpp_spelling`) — same rule, character alphabet — and the
paren/square-only scanners at 11776, 19169, 35358, 46584, and
`madc_program.cpp:1119`.

### Slice 3 answered: the char-level scanners are a SEPARATE, larger family

Slice 3 said to verify the char-level pair can actually see the hazard before
writing machinery for it. Answered — and the pair is not a pair.

Searched for an existing char-level delimiter scanner (the Rule #4 step, stated
so its absence is checkable):

```
grep -rn "== '<'" src/*.cpp
grep -rn "spelling.*depth\|depth.*spelling" src/*.cpp include/*.h
```

**No shared owner exists** — unlike the token family, where `DelimDepth` was
already there. And the family is **19 lines across three files**, not two
functions in one:

| File | angle-scan lines |
|---|---|
| `src/parser.cpp` | 10 (18073, 18095, 21395, 24060, 24156, 40865, …) |
| `src/madc_mangle.cpp` | 6 (305, 326, 348, …) |
| `src/cir_builder.cpp` | 3 (11561, 11573, …) |

Every one of them was invisible to all three earlier audits: they count with
`depth` or `d`, never `angle_depth`, and two of the files were never in scope.

**Can they see the hazard?** Yes. These scan a canonical C++ type spelling for
the top-level `::`, `,` or `<`, and a template-argument spelling can legitimately
contain `vt<decltype(declval<T>()<declval<U>())>::type`. A spurious angle open
makes them pick the wrong `::`.

Writing a char-level sibling of `DelimDepth` **is** justified new machinery —
the search was performed and came back empty. That is round 2's job, not a
bolt-on: it needs its own reducer, its own three-way g++/clang comparison, and
its own gate. Deliberately not started here, because a half-migration across
three files with the battery already running is how the original mess was made.

No gate was added for this family yet, on purpose. The obvious markers
(`== '<'`, a bare `depth`) are exactly the spelling-keyed kind this document
argues against, and a bad marker is worse than none — it reports a smaller
family with confidence.

### Slice 5 — payoff collected

Measured in the container (the NAS's `sys_include_paths.cpp` is a stale
pre-P2.0 flat table and libc++ headers exist only in the container, so a
`-stdlib=libc++` probe there fails with "Failed to open include file" for
reasons that have nothing to do with this fix).

| Probe | Before | After |
|---|---|---|
| `<cctype>` + `using ::isalnum;` | `cctype:111` failure | **rc=0** |
| `<type_traits>` | — | **rc=0** |
| `<string_view>` | `cctype:111` cascade | rc=1, new + deeper defect |

**The `cctype:111` cascade is cleared.** `<string_view>` now dies at
`.../c++/v1/cwchar:202: 'wcslen' is not a member of namespace 'std'` — wide-char
declarations, a genuinely new blocker rather than a remnant. That is the next
libc++ item, ahead of `<compare>` → `math.h:414 'std::__math'` (which belongs to
the `qualifier_before_scope_resolution` family, not this one).

Classify these probes **by exit status, never by scraping output**: the first
run of this table piped through `head` and reported `rc=0` for a failing
compile, because that was `head`'s status.

### Round 1 validation

`fulltest` rc=0 · JIT **762**/0/0/9 (baseline 761) · EXE **746**/0 (745) ·
OBJ **746**/0 (745) · packed **762**/0 · `libcxx_gate` **11/11**.
Every lane +1 — `tests/testtemplateanglelt.mad`. No regressions.

---

## Round 2 (same session) — ratchet 25 → 13 → 9

Five more scanners migrated, all behaviour-preserving:

| Site | Function | Care taken |
|---|---|---|
| 11776 | `constant_initializer_has_runtime_access` | explicit `d.paren/square/brace`, **not** `d.top()` |
| 19169 | `consume_unresolved_dependent_call` | `d.update(open)` seeds paren at 1 |
| 35333 | static-member-initializer skip (`TokenCLASS::parse`) | same `top()` caution |
| 46559 | `old_style_parameter_head_has_declaration_suffix` | close decided BEFORE update — DelimDepth clamps a close at depth 0 |
| 38305 | `collect_template_default_argument` | see below |

**`d.top()` is a trap for every one of these.** It also requires `angle == 0`,
and these scans never tracked angle brackets. `a < b;` opens one, so `top()`
would silently stop the `;` from terminating the scan. Always use the explicit
axes the original tracked.

**`collect_template_default_argument` was the interesting one** — it *rewrites*
`>>` into separate `TokenGT`s and pushes one back to the stream. Instead of
assuming how many levels a `>>` closes, it now asks:

```cpp
int before = d.angle;
d.update(t);
int closed = before - d.angle;   // 0, 1 or 2
```

`closed == 0` means the `>>` was a genuine shift (e.g. inside parens opened
within the list, `A<(x >> y)>`) and must survive verbatim — which the original
got **wrong**, always treating a `>>` at `angle > 0` as a closer.

### `madc_program.cpp:1119` is a FALSE POSITIVE — excluded from the gate

`validate_expression_source()` is a raw-**source** mini-lexer with quote and
comment states that also tracks whether each paren level was preceded by an
identifier (commas are legal in call args, not in grouping parens). The
tie-breaker — *would a change to the delimiter rule require editing it?* — says
no: it evolves with expression validation, not with [temp.names]. Merging it
into `DelimDepth` would be worse than the duplication. Excluded in the gate with
that reason written down.

### Deliberately NOT migrated — read this before attempting them

- **The ctor-initializer scan.** `expecting_initializer` is driven by brace
  transitions, and a `{` at top level means *brace-init* or *body start*
  depending on that flag. A `{` inside parens is counted by `DelimDepth` but was
  NOT counted by the original. Getting this subtly wrong breaks constructor
  parsing everywhere; it needs its own cycle with room to iterate.
- **`skip_template_nonclass_declaration`** — body-brace detector plus an
  operator-spelling state machine, and it pops from a `seen` vector.
- **The char-level pair.** A shared walk is the obvious move, but
  `primary_name_from_cpp_spelling` assigns `name_end` at **every** top-level
  `<`, so it is the **LAST** one, not the first. A helper that returns "the
  first top-level `<`" silently changes `A<B>::C<D>`. Whoever writes the
  char-level sibling must preserve that.

---

## Round 3 (same session) — the char-level pair, ratchet 10 → 8

`SpellingDelimDepth` + `scan_cpp_spelling()` (parser.cpp, beside the two
readers) now own the char-level rule, and `namespace_scope_from_cpp_spelling`
and `primary_name_from_cpp_spelling` share one walk.

This is the campaign's **first genuinely new machinery**, and it is justified by
a search that came back empty: unlike the token side, no char-level owner
existed. It is named and commented as the **sibling** of `DelimDepth` so the two
read as one rule in two alphabets, not as a second opinion.

**The trap, recorded because it is invisible on a skim.**
`primary_name_from_cpp_spelling` assigned `name_end` at **every** top-level `<`,
so it meant the **LAST** one. A shared helper returning "the first top-level
`<`" silently breaks `A<B>::C<D>`, which must yield scope `A<B>` and primary
name `C`. The field is `last_template_id` and says so in a comment.

**Intended behaviour change:** a `<` now opens only after a name character and
outside `()`, `[]`, `{}`. `vt<decltype(declval<T>()<declval<U>())>::type`
previously desynced the angle count and returned the wrong `::`. Spaces do not
reset the name-character test (`A <B>` still opens), and the boundary is
recorded only when the rule actually opened a level, so a `<` classified as
less-than can never move the name boundary.

Validation: `fulltest` rc=0, JIT 762/0/0/9, EXE 746/0, `libcxx_gate` 11/11.

### Where the two families stand

| Family | Was | Now | Owner |
|---|---|---|---|
| token delimiter depth | 27 locals | **8** | `DelimDepth` (pre-existing) |
| char-level angle scanning | 19 lines | **9** | `SpellingDelimDepth` (new, this round) |

Remaining token sites are the two entangled ones only:
`skip_template_nonclass_declaration` and the ctor-initializer scan. Remaining
char-level sites are `madc_mangle.cpp` (6) and `cir_builder.cpp` (3) — and
those are now **adoption**, not extraction, because the owner exists.

---

## Round 4 — the union was NOT what round 1 claimed. Ratchet 8 → 4

Migrating `skip_template_nonclass_declaration` (the site holding the paren
guard) onto `DelimDepth` **would have reintroduced the bug `2e853dee` fixed.**
Traced before editing, not discovered after.

`DelimDepth::update` opened an angle on `angle_open_context(prev)` alone, with
no paren guard:

- `declval<T>() < declval<U>()` — prev is `)`, context test rejects it. ✅
- `decltype(a < b)` — prev **is** an identifier, so the context test *passes*.
  The angle opens inside the parens, its `>` is also inside the parens, and the
  depth stays **stuck at 1 past the `)`** — after which no `;` or body `{` ever
  reads as top-level and the scan runs to EOF.

So the two tests are independent and both are required. Round 1's claim that
`DelimDepth` was "strictly more correct than any hand-rolled copy" was **wrong**,
and wrong in the direction that would have caused damage. Corrected in
`docs/rules/delimiter-tracking.md` under "The claim that was wrong".

**The real union is now merged into `DelimDepth`:**

```cpp
case TokenID::tkLT:
    if ( !paren && !square && angle_open_context(prev) )
    { ++angle; angle_paren.push_back(paren); }
```

Because angles now only ever open at paren depth 0, `angle_paren` is uniformly
zero and `close_angle()`'s existing `paren > angle_paren.back()` test becomes
the matching guard on `>` for free.

`skip_template_nonclass_declaration`'s delimiter counters then migrated onto
`DelimDepth`. Its **operator-id skip did NOT migrate**, and that is a second
finding, caught by the suite rather than by reading:

> Replacing the hand-rolled `consume_operator_spelling` machine with
> `delimStepStream()` broke `testifconstexpr` and `testinvocable` —
> `/usr/include/c++/13/bits/max_size_type.h:75: operator _Tp() const noexcept`
> → **"Unrecognized operator symbol"**.

`delimStepStream()` calls `parseOperatorId()`, which is **strict**: it throws on
anything that is not a known operator symbol, and a **conversion-function-id**
(`operator _Tp()`) is not one. The hand-rolled machine was **tolerant** — it
consumed whatever followed `operator` without validating. For a *skipper*,
walking a declaration madc has already decided not to parse, tolerance is the
correct behaviour.

**The two shared step helpers disagree**, which is the real defect:
`operator_id_token_span()` (index form) is tolerant; `parseOperatorId()` (stream
form) is strict. Until that asymmetry is fixed in the shared layer — teach
`parseOperatorId` conversion-function-ids, or give `delimStepStream` a tolerant
path — this site cannot fully migrate. The tolerant block stays with a comment
naming the exact blocker and the two ways out.

Underneath it is a genuine language gap worth its own work: **madc cannot parse
a conversion operator anywhere `parseOperatorId` is used.** It only ever went
unnoticed because the one place that met `operator _Tp()` in real headers was a
skipper that never validated.

### The generalisable lesson

**"The shared owner already exists" is not the same as "the shared owner is
complete."** Before adopting an owner at a site that carries a local guard, diff
the *rules* rather than the outcomes, and ask what that guard was added to fix.
Here the answer was sitting in a commit message naming the exact shape.

Only the ctor-initializer scan (4 locals) remains in the token family.
