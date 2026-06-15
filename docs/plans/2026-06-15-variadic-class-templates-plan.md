# Plan — Variadic class templates done right (the `__and_`/`__or_`/`_List`/`tuple` family)

**Status:** design/research plan (not immediate coding). Authored 2026-06-15 while
context on the SESSION-10 part-2c Feature-2 attempt is fresh. Companion artifacts:
`docs/plans/2026-06-12-retire-embedded-shims-HANDOFF.md` (§SESSION-10),
`docs/plans/2026-06-14-template-instantiation-core-plan.md` (§4 part 2c feature 2),
WIP patch `tmp/feature2_variadic_partialspec_wip.patch`, reducers `tmp/db1-6.mad`.

---

## 1. The problem

madc cannot resolve a **variadic-primary** class template-id followed by `::member`
when the real body lives in a **partial specialization** — the libstdc++ trait
idiom:

```cpp
template<typename...> struct __and_;                         // variadic primary = fwd-decl
template<typename _B1> struct __and_<_B1> : _B1 {};          // 1-arg partial spec
template<typename _B1, typename _B2>                         // 2-arg partial spec
  struct __and_<_B1,_B2> : conditional<_B1::value,_B2,_B1>::type {};
```

This blocks `tmp/tv1.mad` (`std::vector<int>::push_back`): the
`__make_move_if_noexcept_iterator` default `__conditional_t<__move_if_noexcept_cond
<_Tp>::value, …>` needs `__move_if_noexcept_cond<_Tp>` whose base clause is
`__and_<__not_<…>, is_copy_constructible<_Tp>>::type` (bits/move.h:102-104).

**Reducers (tmp/, DEFAULT mode, g++/clang accept all):**
- `db1`/`db2` — NON-variadic `And<…>::type` base clause → **WORK** (incl. nested
  template-ids, dependent param, `::value` inheritance). Establishes the
  dependent-base/`::value` shape is already fine.
- `db3` — variadic `And2<…>::type` as a base specifier → **FAILS**.
- `db4` — variadic `And2<X,Y>::type` as a TYPEDEF → **FAILS**.
- `db5` — `And2<X,Y>::n` static member → prints 0, proving the partial spec is not
  selected (the primary, or opaque, is used).
- `db6` — non-variadic typedef `And<X,X>::type` → **WORKS** (control).

## 2. Why madc fails today — three coupled gaps

1. **Opaque short-circuit on pack-ness** (`instantiate_template_use`,
   parser.cpp:2659). A variadic primary returns `instantiate_opaque_template_use`
   (a dependent placeholder with no members) *before* the arg-parse loop and the
   partial-spec selection (parser.cpp:2848). So `::member` has nothing to resolve.
2. **`has_non_type_params` mis-set for a TYPE pack** (parser.cpp:28519). A
   `template<typename...>` wrongly sets `has_non_type_params=true`, so the SECOND
   opaque short-circuit (`has_non_type_params && body.empty()`, parser.cpp:~2675)
   ALSO fires for the bare `__and_`/`__or_` fwd-decls. (Genuine bug independent of
   the rest.)
3. **`match_partial_specialization` requires EXACT arity** (parser.cpp:13275:
   `spec.spec_pattern.size() != arg_spellings.size()` → skip). A pack-containing
   spec pattern such as `__and_<_B1,_B2>` (2 slots) — or `_List<_Tp,_Up...>` (2
   slots, one a pack) — can therefore NEVER match a different arg count (e.g. 5
   args). madc has NO pack-expansion unification in partial-spec deduction.

## 3. How clang and g++ do it (recon, with citations)

Two independent source recons (clang `/workspace/llvm-clang-src`, g++
`/workspace/gcc/gcc/cp/pt.cc`) **converge** on the same model. The four load-bearing
lessons:

**L1 — Pack-ness is IRRELEVANT to opaqueness.** Neither compiler "goes opaque"
because the primary is variadic. Opaqueness is gated on **dependence of the
arguments**, not on packs.
- clang: a `TemplateSpecializationType` stays unresolved sugar iff
  `isDependentType()` (Type.h:2419, 5636); a non-dependent specialization is
  subject to completeness/instantiation.
- g++: `lookup_template_class` forms the type for any arg set; `uses_template_parms`
  (the dependence test) is what defers (instantiate_class_template entry guard
  pt.cc:12768-12770).

**L2 — Partial-spec selection is by DEDUCTION, and it is PACK-AWARE.** Both deduce
each partial spec's params against the concrete arg list; a trailing pack in the
spec pattern absorbs the remaining args.
- clang: `getPatternForClassTemplateSpecialization` (SemaTemplateInstantiate.cpp:
  3670) loops partials calling `DeduceTemplateArguments(Partial, args)`; pack
  deduction via `PackDeductionScope` (SemaTemplateDeduction.cpp:707-900) collects
  unexpanded packs and accumulates deduced elements. Most-specialized wins; no
  match → primary.
- g++: `most_specialized_partial_spec` (pt.cc:27781) → `get_partial_spec_bindings`
  → `unify` with `unify_pack_expansion` (pt.cc:25781-25978), building a
  `TYPE_ARGUMENT_PACK`. `more_specialized_partial_spec` breaks ties.

**L3 — Argument collection PACKS trailing args.** When a variadic primary is used,
trailing args are gathered into one pack argument.
- clang: `CheckTemplateArgumentList` (SemaTemplate.cpp:6016-6162) keeps collecting
  into `SugaredArgumentPack` while the param `isTemplateParameterPack()`, then
  `TemplateArgument::CreatePackCopy`.
- g++: `coerce_template_parms` → `coerce_template_parameter_pack` (pt.cc:9122-9247)
  builds a `TYPE_ARGUMENT_PACK` from `make_tree_vec(nargs - arg_idx)`.

**L4 — THE LINCHPIN: type FORMATION is separate from body INSTANTIATION, and
formation is LAZY.** Forming `__and_<A,B>` (or aliasing it) does **not** instantiate
its body. Body instantiation is deferred until **completeness is demanded** (member
access, `sizeof`, base-class use).
- clang: type formed by `getTemplateSpecializationType` (sugar). Body only on
  `RequireCompleteType` → `InstantiateClassTemplateSpecialization` (SemaType.cpp:
  9486-9495). A bare `using X = __and_<A,B>;` does NOT instantiate (SemaType.cpp:
  9427, 9482-9485).
- g++: `lookup_template_class` returns the `TYPE_DECL` with NO body (pt.cc:10762);
  body only via `complete_type` → `instantiate_class_template` (typeck.cc:137-138).

## 4. Why the SESSION-10 attempt cascaded (and what it proves)

The reverted WIP patch (`tmp/feature2_variadic_partialspec_wip.patch`) did: (a) skip
both opaque short-circuits when partial specs exist; (b) bind extra args to a
trailing pack slot in the arg loop; (c) allow an empty trailing pack (`tuple<>`).
It made db1-6 pass **but broke every test** at a HEADER alias:

```
type_traits:1760  template<typename...> struct _List { };
type_traits:1763  template<typename _Tp, typename... _Up> struct _List<_Tp,_Up...> : _List<_Up...> {…};
type_traits:1785  using _UInts = _List<unsigned char, …, unsigned long long>;   // 5 args
```

`resolve_declared_type_token(_List<5 args>)` returned NULL → "Expecting type in using
alias" (parser.cpp:17798). Root: (i) gap #3 — `_List<_Tp,_Up...>` (2 slots) cannot
match 5 args, so no spec is selected; (ii) madc's `using X = T;` handler EAGERLY
calls `resolve_declared_type_token`, i.e. eagerly FORMS+instantiates the type — there
is no lazy "form sugar, defer body" layer (violates **L4**). g++/clang never even
touch `_List` for `tmp/db6` because nothing demands `make_unsigned<enum>`; madc
instantiates it merely because the header *declares* the alias.

**The attempt proves:** the opaque short-circuit removal is necessary but NOT
sufficient. Without (gap #3) pack-aware deduction the right spec is never chosen, and
without (L4) lazy formation the change instantiates a cascade of header traits that
were previously — correctly — left opaque/untouched.

## 5. The plan — staged, each behind the full 4-gate

Ordering is chosen so each stage is independently gateable and the cascade is
controlled. **This is multi-session.**

### Stage A — fix the type-pack `has_non_type_params` bug (parser.cpp:28519)
A `template<typename...>` (type pack) must NOT set `has_non_type_params`. Pack-ness
is already carried by `typeparam_is_pack`; variadic-ness by
`template_has_parameter_pack`. **Risk:** the flag feeds the second opaque
short-circuit and the alias `has_non_type_params` path — removing it changes which
templates opaque-out. Gate hard (torture + SMAUG + integration). Land alone; it is a
correctness fix and shrinks the surface for later stages. *Likely no test flip.*

### Stage B — pack-aware partial-spec deduction (`match_partial_specialization`)
Make a spec pattern with a trailing pack match a **range** of arg counts, deducing
the pack. This is the `unify_pack_expansion` (g++) / `PackDeductionScope` (clang)
equivalent. Concretely (parser.cpp:13254+):
- Replace the exact-arity gate (13275) with: fixed-slot count `F` = pattern slots
  before the pack; require `args ≥ F` (or `== F` when no pack). Match the first `F`
  slots elementwise; bind the trailing pack param to the remaining `args - F`.
- Extend `unify_spec_pattern_arg` / add a pack case so a pattern slot that is a pack
  (`_Up...`) deduces to the arg tail; record the bound pack for substitution.
- Tie-break by specificity (already partly present via `score`); a fixed slot is
  more specialized than a pack slot (mirror `more_specialized_partial_spec`).
**Risk:** moderate — touches the shared partial-spec matcher used by non-variadic
specs too; keep the non-pack path byte-identical (only the new "pattern has a pack"
branch changes behavior). *Testable in isolation only once Stage C lets variadic
primaries reach the matcher — so B+C land together, but B is the larger/riskier half.*

### Stage C — variadic primary fall-through + pack arg collection
The WIP patch's (a)+(b)+(c): skip both opaque short-circuits when a partial spec
exists; bind extra args to the trailing pack slot in the arg loop; allow an empty
trailing pack. **Gate the fall-through on "a spec actually matched"** (L1+L2): if
pack-aware deduction (Stage B) selects a spec, instantiate it; **if NO spec matches,
keep the opaque placeholder** (do NOT fall to the empty primary → NULL). This single
rule is what stops the `_List`-style NULL: an unmatched variadic use stays opaque
exactly as today.

### Stage D — lazy formation / cascade control (THE LINCHPIN, L4)
Even with B+C, instantiating recursive variadic traits at every header alias site is
too eager (madc forms+instantiates inside `using X = T;`). Options, cheapest first:
- **D1 (narrow trigger):** only fall through (Stage C) when the template-id is
  immediately followed by `::member` (the construct that actually needs the body) —
  detect a trailing `::` after the balanced `<…>` via lookahead. A bare
  `using X = __and_<A,B>;` with no `::member` keeps the opaque placeholder (matching
  today's behavior, no cascade). This is the **smallest** change that unblocks the
  `__and_<…>::type` / `::value` chain without eagerly instantiating every header
  alias. Fragile-ish (balanced-`<>` lookahead) but well-scoped.
- **D2 (defer body):** reuse madc's existing lazy member-body machinery (SESSION-8
  part 16, `deferred_lazy_bodies`) so forming a variadic specialization registers
  the type but defers its body (and base-class chain) until ODR-use/completeness.
  Closer to clang/g++ (L4) but a larger change to the formation path.
- **D3 (full sugar layer):** a genuine lazy `TemplateSpecializationType`-equivalent.
  Out of scope; record as the long-term direction.

**Recommended:** D1 first (unblocks tv1's chain with minimal blast radius), measure
the residual cascade under the gate, escalate to D2 only if D1's narrowing is
insufficient for the real tv1 path.

### Stage E — residual walls
Once `__and_`/`__or_`/`__not_` evaluate and `__move_if_noexcept_cond<T>::value`
folds, expect further tv1 walls (`__uninitialized_copy_a`, `_M_realloc_insert`).
testvector L17 and testvectorptr L28 are SEPARATE roots. Drive each under the gate.

## 6. Risks & method
- The cascade is the dominant risk; Stages C-gate-on-match and D1-narrow-trigger are
  the controls. After each stage, FULL 4-gate (integration vs the 15 baseline; unit;
  torture 51-name failset byte-identical; SMAUG soak) — a header-wide regression
  shows up as a mass integration failure (as the attempt did).
- Reduce in `tmp/` DEFAULT mode; 3-oracle (g++/clang + stock c2m); verify reducers
  via cout/`--emit=c11` (madc exit code = RUN-SUCCESS, not main()'s return).
- `match_partial_specialization` / `unify*` are shared with non-variadic specs —
  keep those paths byte-identical.

## 7. Honest assessment / recommendation
This is a **multi-session** feature (Stages A-E), and even when complete tv1 may not
flip (Stage E walls; testvector/testvectorptr separate roots). Per the standing
strategic finding (HANDOFF §SESSION-9 §3), headline drops have come from INDEPENDENT
singletons, not the tv1 chain. **Recommendation:** do the ~20-min survey of the other
14 failures first (next task) to confirm whether a different failure is a closer
headline flip before committing the multi-session variadic effort. When the variadic
work is taken up, start at Stage A, then B+C (gated on match), then D1.
