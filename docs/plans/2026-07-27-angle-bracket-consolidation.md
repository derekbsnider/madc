# Angle-bracket disambiguation: one rule, one implementation

**Status:** planned, not started. This is the **first** item of the next
session — it blocks the libc++ track
(`docs/plans/2026-07-26-libcxx-flavor-plan.md`) and it is a live C++
correctness bug in its own right.

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

## The real finding: six copies of one rule

`grep -n "++angle_depth" src/parser.cpp` returns **six** sites. **One** is
guarded.

| Site | Function | `<` guarded by paren/square depth? | Introduced |
|---|---|---|---|
| **38782** | *(reference)* | **YES** — `&& paren_depth == 0 && square_depth == 0` | `2e853dee` 2026-06-07 |
| 3379 | `collect_template_argument_spelling` | no — *tracks `paren_depth` but does not apply it here* | `e7b06f30` 2026-06-05 |
| 38371 | `collect_template_default_argument` | no | `e7b06f30` 2026-06-05 |
| 45470 | `skip_template_suffix_tokens` | no | `e7b06f30` 2026-06-05 |
| 48486 | inner scope (real-header path) | no | `c9fd2227` **2026-06-09** |
| 18089 / 18114 | `namespace_scope_from_cpp_spelling`, `primary_name_from_cpp_spelling` | no — char-level, over a spelling string | `e7b06f30` 2026-06-05 |

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

**Therefore the fix is not "add the guard five more times."** That would leave
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

Migrate the token-level sites (3379, 38371, 38782, 45470, 48486) onto it,
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
