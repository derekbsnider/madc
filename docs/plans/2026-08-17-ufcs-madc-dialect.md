# UFCS for the madc dialect (`--std=madc` only)

**Status:** U1 + U3 **LANDED** on branch `feature/ufcs-madc-claude`; U2 + U4
open. Owner supplied the ruleset 2026-08-17 and confirmed the direction against
Stroustrup's unified-call background note; the decisions below are mine unless
marked ⚠️.

## The ruleset (owner, verbatim intent)

```
// Dot syntax:
x.f(y)
  -> viable member f?  use it
  -> otherwise         try ordinary f(x, y)

// Function syntax, eventually:
f(x, y)
  -> viable declared free f?  use it
  -> otherwise                try ordinary x.f(y)
  -> otherwise                existing madc unresolved-symbol behavior
```

Member-first for dot, free-first for call syntax. One fallback each, tried in a
fixed order. **No merged overload set and no cross-kind ranking** — that is the
D/Nim rule, deliberately not C++'s N4165, and it is why this is implementable
without touching overload resolution at all.

## The safety property that makes phase 1 cheap

**Phase 1 fires ONLY where madc today raises a hard error.** If a viable member
exists, nothing changes; if none does, madc currently throws
`Unidentified member 'f' in 'T'` or `member reference is not a structure or
union`. So no program that compiles today can change meaning. That is the
invariant to hold — and to test.

Phase 3 (call syntax) does NOT have this property for free; see its section.

## Where it hooks (verified, `src/parser.cpp` @ e9e92af9)

The dot path is one contiguous decision, ~33066–33266:

```cpp
if ( !struct_type->is_struct() && !struct_type->is_object() )
    Throw(tb) << "member reference is not a structure or union" << flush;   // <- U2 hook
var = NULL;
string id = member_lookup_name;
DataDefCLASS *method_cls = struct_type->is_object() ? (DataDefCLASS *)struct_type : NULL;
if ( method_cls ) {
    call_follows = peek is '(' or '<';
    var = call_follows ? find_method_by_callable_arity(method_cls, id, argc, false)
                       : method_cls->findMethod(id);
    if ( !var && call_follows ) var = method_cls->findMethod(id);
}
if ( var ) { ...member call, unchanged... }                                 // <- U1 hook: the `else`
...
ssize_t ofs = ((DataDefSTRUCT *)struct_type)->m_offset(id);
if ( ofs == -1 ) { ... Throw "Unidentified member" ... }                    // <- U1 hook (data-member miss)
```

The call path's fallback ordering lives at ~34091, `dlsym fallback: resolve
known libc/system functions early` — phase 3 must land **before** it.

Gate helper follows `Program::madc_dialect_type_spelling` (parser.cpp:2522),
the existing STD_MADC owner: a named predicate that returns early outside the
dialect. **`Searched:` "a madc-dialect feature gate" →
`grep -rn STD_MADC src include` → 24 sites, all `language_std == STD_MADC`;
`madc_dialect_type_spelling` is the model. No UFCS machinery exists.**

## Owners to ADOPT (Rule #4 — searched, do not re-invent)

| concept | grep | existing owner |
|---|---|---|
| can this callable take N args (defaults + varargs aware) | `callable_arity\|accepts_arity\|arity_matches\|_arity(` | **`method_callable_with_arg_count()`** parser.cpp:16356 — generic over any `Variable` holding a `FuncDef`; `hidden` is 0 for a free function, so despite the name it serves BOTH sides. Misnamed, not missing. |
| member lookup filtered by callable arity | `find_method_by_callable_arity` | parser.cpp:16383 (8 call sites) — delegates to the above; deliberate `any_named` short-circuit stops base-class search |
| count the args queued at a call site | `count_queued_call_arguments` | parser.cpp:24854 — **heuristic**; its comment at :24874 records a real miscount (template-argument commas), which is WHY the by-name recovery exists behind it |
| select a free-function overload from arg TYPES | `findFunctionOverload\|select_overload\|best_overload\|resolve_overload` | **absent** — only `findMethodOverload` (class-scoped) and `namespace_overload_set_accepts_more` (parser.cpp:24799). This bounds the call side to ARITY viability. |
| fallback chain when a call name won't resolve | the dlsym region | an existing ordered chain at ~34140: `lazy_resolve` → expression-context → implicit-complex-builtin → dlsym → `__builtin_` twin → implicit `int`. **U3 inserts a link into this chain; it does not build a parallel one.** |

## Decisions

1. **Gate.** `Program::ufcs_enabled() const { return language_std == STD_MADC; }`.
   Every strict `--std=c*` / `--std=c++*` mode is byte-identical to today. A
   negative-control test asserts the same source fails under `--std=c++17`.
2. **Tier 1 lowering** (`.claude/rules/lowering-vs-raising.md`). UFCS resolves at
   parse time into an ordinary `TokenCallFunc`. Nothing new reaches `cir_node`,
   c2mir or MIR; `--emit=c11` is unaffected because by emission time there is no
   trace of the syntax. No fork cost, no new IR.
3. **The receiver is argument 0, passed EXACTLY as written.** No implicit `&`,
   no implicit `*`. Existing overload resolution and reference binding decide
   everything else — UFCS contributes syntax, not conversions.
4. ⚠️ **`p->f(y)` lowers to `f(p, y)`, not `f(*p, y)`.** This is the one place
   I made a call a reader could dispute. It breaks the C++ identity
   `p->f(y)` ≡ `(*p).f(y)` for the *fallback* leg only (member lookup, tried
   first, keeps the identity). I chose it because one rule — "the receiver
   expression is argument 0, unmodified" — is predictable, and because it is
   what makes C APIs read well: `fp->fclose()` → `fclose(fp)`,
   `s->strlen()` → `strlen(s)`. Under `f(*p, y)` every pointer-taking C
   function would need `&` back at the call site, which defeats the point.
   Say so if you want the other rule; it is a one-line change in U2.
5. **Non-class receivers participate** (U2): `int`, `char *`, arrays, enums.
   This is most of the value in a scripting dialect — `n.abs()`,
   `s.strlen()`, `buf.memset(0, n)` — and it is exactly the case that is a hard
   error today, so it is free under the safety property.
6. **No new lookup rule for the free function.** The fallback resolves `f`
   through madc's ordinary unqualified chain (current namespace → `using` →
   global), identical to what `f(x, y)` would have found written by hand. So
   `php::`/`perl::` helpers become extension methods when — and only when —
   they are already visible. No per-namespace special-casing
   (`design-principles.md` #7).
7. ~~**One shot, no recursion.** The fallback sets a flag…~~ **NOT NEEDED — and
   the reason matters.** The feared loop (`f(x,y)` → `x.f(y)` → no member →
   `f(x,y)` → …) can only happen if a fallback *rewrites tokens and re-parses*.
   Neither does: each helper resolves its target first and builds the finished
   call node directly (`TokenCallFunc` / `TokenCallMethod`), so a fallback that
   fires has already succeeded, and one that cannot find its target returns
   false without entering the other arm. No flag, no re-entry, by construction.
   **If a future slice ever re-parses to implement a fallback, this decision
   comes back.**
8. **Diagnostics name both attempts.** `no member 'f' in 'T', and no function
   'f' taking (T, U)` — a single error, not two. A UFCS miss must never read
   like a plain typo.
9. **The two sides test viability DIFFERENTLY, and the asymmetry is forced by
   what the code already does — not by taste.**
   - **Dot side: BY NAME.** The site already runs
     `find_method_by_callable_arity()` (arity-viable, default-argument aware)
     and, on a miss, falls back to a by-name `findMethod()`. That second lookup
     is a **recovery net**, not sloppiness: `count_queued_call_arguments()`
     carries a comment at parser.cpp:24874 recording that it once counted
     *template-argument* commas as call arguments, which broke the arity lookup
     and made libc++ calls report themselves undeclared. UFCS inserted ahead of
     that net would silently re-form as a free call something that recovers
     today — breakage. So UFCS sits BEHIND both member attempts. Consequence to
     accept: `p.x(3)` where `x` is an `int` member and a free `x(T&, int)`
     exists still errors.
   - **Call side: BY ARITY.** There is no recovery net to preempt, and the free
     candidate pool is the whole preference chain (`c`, `std`, `php`, …), so
     name-presence is meaningless there — `std::count` would swallow
     `count(m, k)` and the motivating example would never reach `m.count(k)`.
     Arity is also exactly enough: `count(m,k)` is 2 args vs `std::count`'s 3
     (not viable → falls back); `begin(v)` is 1 vs `std::begin`'s 1 (viable →
     uses `std::begin`, which is the right answer anyway).

   This is Stroustrup's "try the other syntax if the first one failed" made as
   precise as madc's existing predicates allow — no new judgment invented.

## Slices

| slice | scope | gate |
|---|---|---|
| **U1** ✅ | `ufcs_enabled()` + dot fallback for **class** receivers with no viable member | reducers: free fn found; member wins over free; both miss → one clear error; `--std=c++17` negative control |
| **U2** | dot fallback for **non-class** receivers (primitives, pointers, arrays); the `->` rule from decision 4 | reducers per receiver kind, incl. `fp->fclose()` |
| **U3** ✅ | call syntax `f(x,y)` → `x.f(y)`, inserted **before** the dlsym fallback | reducers + the ordering test below |
| **U4** | docs (`docs/language/`), `--std=` matrix test, `scripts/ufcs_gate.sh` wired into `fulltest` | the gate is the deliverable |

Each slice is its own commit with trailers, its own reducers in `tests/`, and
g++/clang++ oracles where the construct is legal C++ (the member-wins case is;
the fallback case is madc-only and its oracle is "g++ rejects it", which is the
point of the dialect gate).

## What U1 landed

- `Program::ufcs_enabled()` (`include/madc.h`, beside `auto_includes_enabled()`)
  — the gate, same shape as `madc_dialect_type_spelling`.
- `Program::ufcs_dot_fallback()` (`src/parser.cpp`, immediately above its only
  caller `parseExpr_identifierArm`) — resolves `f` through
  `resolve_preferred_identifier(ident_tb, false)` (the SAME resolver a
  hand-written `f(x, y)` gets from that position: the receiver already occupies
  the expression head), builds an ordinary `TokenCallFunc` with the receiver as
  `parameters[0]`, then lets `parseCallFunc` read the rest of the argument list.
  Requires a `(` to follow — `x.f` with no argument list keeps its old error.
- The hook is the **data-member-miss throw** (`Unidentified member '<id>' in
  '<T>'`). Both misses funnel there: a class with no such method leaves `var`
  NULL and falls through to the offset lookup, so ONE hook covers method and
  data-member misses for class *and* plain-struct receivers.
- Reducers: `tests/testufcs.mad` (+`.expect`) — free fn at 1 and 2 args, plain
  `struct` receiver, member-wins-over-free; `tests/testufcsmiss.mad`
  (+`.expect_err`) — the combined diagnostic; `tests/testufcsstrict.mad`
  (+`.flags` = `--std=c++17`, +`.expect_err`) — the negative control.
- Oracle: g++ and clang++ both print `magsq=25 / scaled=70 / member wins: 42` for the
  written-out calls, and both REJECT `p.magsq()` (`has no member named 'magsq'`
  / `no member named 'magsq' in 'Point'`) — which is the whole point of the gate.

Not yet covered by U1, deliberately: `x.f<T>(y)` (explicit template arguments —
`peekToken()` is `<`, not `(`, so it takes the old error path), and receivers
whose class has an unresolved dependent surface (the dependent-call placeholder
at the same site consumes those first).

## What U3 landed — and what measuring first changed

**Measured before building** (`bin/madc` at U1, probes in `tmp/ufcs/`):

| call | today, before UFCS |
|---|---|
| `size(v)` on `std::vector<int>` | **works** — `3` |
| `begin(v)` | **works** — `7` |
| `empty(v)` | **works** — `1` |
| `count(m, 3)` on `std::map` | **hard error**, `use of undeclared identifier 'count'`, rc=1 |

Three of the four motivating calls already worked, because libstdc++ really
does declare `std::size` / `std::begin` / `std::empty` — which is precisely the
duplication the proposal complains about ("why do we/someone have to write
both?"). Only the member-only operation, `count`, had no free counterpart.

That reshaped the trigger. **U3 fires only when NO free `f` is declared at
all** — not the stronger "declared but not arity-viable". The stronger trigger
would require judging a whole namespace overload set, which risks stealing a
call that resolves today, and the evidence says it buys nothing: the one call
that fails, fails because the name is entirely undeclared.

- `Program::ufcs_call_fallback()` — `src/parser.cpp`, above its only caller.
  Receiver is read by **lookahead, not by parsing**: the first argument must be
  a single identifier naming a class-typed variable (`tokens[0]` is the `(` —
  the indexing `count_queued_call_arguments()` already uses). Arity viability
  goes through the adopted owner `find_method_by_callable_arity()` with
  `argc - 1` (argc counts the receiver; the member call does not).
- Hooked after `lazy_resolve` / expression-context and **before** the
  unresolved-symbol guesses, giving the owner's order: declared free → member →
  dlsym → C89 implicit `int`.
- **Access control is enforced on the selected overload** — UFCS must never
  become a way to reach a private member from outside its class.
- **A STATIC member declines the fallback** — and the negative control
  *corrected the reason*. A static has no `this` slot, so a method call built
  around it passes the receiver as a real argument. I claimed removing the guard
  would make `reading(g)` compile and silently print `-1`. **Measured, it does
  not:** it dies at the downstream arity check with `Incorrect number of
  parameters for 'Gauge__reading': expected 0 got 1`. So this is a DIAGNOSTIC
  guard, not a silent-wrong-answer guard — it keeps a mangled internal name out
  of a message about source that never mentioned it, and declines to build a
  node already known to be mis-shaped. Still worth keeping; the justification
  was just weaker than written. `tests/testufcsstatic.mad` pins it, and the
  message differs with and without the guard, which is what makes it able to
  fail.
- A **pointer** receiver (`Bag *p; tally(p)`) does not engage: a pointer is not
  a class, so the fallback declines. That is the same question as U2's `->`
  rule and belongs there, not here.
- Reducers: `tests/testufcscall.mad` (user class, `count(m,k)` on a real
  `std::map`, `size(m)` still resolving to `std::size`, free-beats-member, and
  a `char *` receiver left untouched), `tests/testufcsorder.mad`
  (member-beats-dlsym, with `<string.h>` deliberately absent so `strlen` really
  is undeclared), `tests/testufcscallstrict.mad` (`--std=c++17` negative
  control). Oracle: g++ 13 and clang++ 18 agree on all eight values via the
  written-out calls, and both reject `tally(b)`.

Not covered: a first argument that is a compound expression (`count(get_map(),
k)`) — it keeps its old behaviour rather than being guessed at. Widening that
means parsing the argument before choosing the callee, which is a real design
step, not an increment.

## Risks, stated

- **U3 changes existing behavior, U1/U2 do not.** Today an undeclared `f(x, y)`
  silently reaches the dlsym fallback and calls a libc symbol with a generic
  `long` return. Putting member lookup ahead of that — which the owner's rule
  says to do — means a program relying on the blind libc guess would instead
  find a member. I think that is right (your own type's member should beat an
  unresolved-symbol guess), but it is a real change and U3 must ship with a
  test pinning the order: declared free → member → dlsym.
- **Future code, not existing code, gains a footgun.** Once UFCS exists, adding
  a member `f` to `T` silently captures calls that used to reach a free `f`.
  Inherent to member-first; C++ has the same hazard with ADL. Scoped by
  `--std=madc`.
- **Not in scope:** operators (`a.operator+(b)`), constructors, `.` on a
  namespace, template argument deduction changes. UFCS is call-syntax sugar
  over existing resolution; if a slice starts needing overload-resolution
  surgery, stop — that means the rule drifted toward N4165.

## Invariant check (`docs/plans/madc-vision-and-invariants.md`)

Gated on the `LanguageStd` enum (I: no hardcoded dialects) · one IR, one emitter,
nothing new below sema (I: one lowering) · no name-keyed special cases (I: no
special-casing) · strict modes bit-identical (I: standards stay clean).
Does not block the polyglot-transpiler arc: a UFCS call renders as the ordinary
call it resolved to.
