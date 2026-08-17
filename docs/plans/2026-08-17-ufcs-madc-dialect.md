# UFCS for the madc dialect (`--std=madc` only)

**Status:** design, awaiting owner go-ahead. Owner supplied the ruleset
2026-08-17; the decisions below are mine unless marked ⚠️.

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
7. **One shot, no recursion.** The fallback sets a flag so a UFCS-formed call
   cannot itself trigger UFCS. Without it, phase 3's `f(x,y)` → `x.f(y)` →
   no viable member → `f(x,y)` loops.
8. **Diagnostics name both attempts.** `no member 'f' in 'T', and no function
   'f' taking (T, U)` — a single error, not two. A UFCS miss must never read
   like a plain typo.

## Slices

| slice | scope | gate |
|---|---|---|
| **U1** | `ufcs_enabled()` + dot fallback for **class** receivers with no viable member | reducers: free fn found; member wins over free; both miss → one clear error; `--std=c++17` negative control |
| **U2** | dot fallback for **non-class** receivers (primitives, pointers, arrays); the `->` rule from decision 4 | reducers per receiver kind, incl. `fp->fclose()` |
| **U3** | call syntax `f(x,y)` → `x.f(y)`, inserted **before** the dlsym fallback | reducers + the ordering test below |
| **U4** | docs (`docs/language/`), `--std=` matrix test, `scripts/ufcs_gate.sh` wired into `fulltest` | the gate is the deliverable |

Each slice is its own commit with trailers, its own reducers in `tests/`, and
g++/clang++ oracles where the construct is legal C++ (the member-wins case is;
the fallback case is madc-only and its oracle is "g++ rejects it", which is the
point of the dialect gate).

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
