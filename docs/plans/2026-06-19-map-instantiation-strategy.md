# Map instantiation strategy — research-driven reset (2026-06-19)

> **THIS IS A CONTRACT, NOT A SUGGESTION.** It is the output of a completed,
> grep-verified recon (madc engine + Clang Sema + GCC `cp/` + libstdc++-13 +
> literature). If you are reading this after a compaction/new session: the
> decisions in "SETTLED" below are CLOSED — do NOT re-investigate, re-derive, or
> "reconsider a better approach." VERIFY the factual build state at HEAD if you
> wish, but do NOT re-open settled decisions. Re-litigating them wasted days.
> Execute W1→W5 in order. (See memory: feedback_handoff_not_open_to_interpretation.)
>
> **COURSE-CORRECTION CLAUSE (settled ≠ immutable).** A SETTLED decision MAY be
> overturned — but ONLY when you have CONCLUSIVE EVIDENCE it is wrong (a failing
> reproducer, a grep/file:line that contradicts the stated evidence, a build/test
> result), NOT a hunch, preference, or "I think there's a cleaner way." If you
> overturn one: (a) state the conclusive evidence, (b) write the justification into
> THIS doc (amend the SETTLED line, don't silently diverge), (c) then proceed. The
> bar is "disproven by evidence," not "reconsidered." Absent that bar, execute as written.

## SETTLED — DO NOT RE-LITIGATE (each line + the evidence that closed it)

1. **The new builtin to implement is GCC's `__integer_pack`. Implement IT.**
   DO NOT implement `__make_integer_seq` or `__type_pack_element` as the path to
   map. EVIDENCE: `grep` of `/usr/include/c++/13` — `__type_pack_element` appears
   NOWHERE; `__make_integer_seq` is gated `#if __has_builtin(__make_integer_seq)`
   (Clang-only; false for madc). libstdc++-13's `_Build_index_tuple`
   (bits/utility.h:154) uses `_Index_tuple<__integer_pack(_Num)...>` on the GCC
   path madc takes. `__type_pack_element`/`__make_integer_seq` would be DEAD CODE
   here. (They are fine as optional forward-compat later; NOT on this path.)

2. **DO NOT rewrite or re-architect the instantiation engine.** madc's
   instantiate-real-libstdc++ approach is SOUND. EVIDENCE: the entire remaining
   `map<int,int>` chain is depth ≤ 1 (item 3) — ordinary single-step specialization
   madc already does for `vector`. The "days of grinding" were SCOPE
   MIS-ESTIMATION (reasoning about general `tuple<A,B,C,…>`), not a wrong design.

3. **`map<int,int>` hits ONLY depth-≤1 tuple base cases. Do NOT build general
   recursive tuple machinery to finish map.** EVIDENCE (libstdc++-13 line refs):
   `_Nth_type<0/1,…>` are explicit specializations (utility.h:235/241 — the `_Np-3`
   recursion starts at I≥3, never reached); `_Tuple_impl<0,X>` is the terminal
   single-element spec (tuple:489, no `_Inherited`); `get<0>` resolves by
   overload+base-convert (no tail walk, tuple:1785); `_Build_index_tuple` is
   non-recursive (utility.h:154); `__integer_pack` is called only with N∈{0,1};
   the indexed-ctor pack expansion is a SINGLE element (key) + an EMPTY one (value).

4. **The build/regression gate is fulltest 656/6.** DO NOT commit any W-step
   without `make -C src fulltest` == 656 passed / 6 failed (the 6 known fails). Flags
   for all map work: `--std=c++20 --no-embedded-headers`.

## OPEN (genuinely undecided — these you MAY investigate)

- W4: whether `std::get<0>` fails at the alias-return SFINAE (parser.cpp ~31084
  `is_templateid_ret`) or the body parse — PIN it with `tmp/tupget2.mad` +
  `MADC_DEBUG_CTORTMPL` before editing.
- Whether the get<0>/get<1> multi-value symbol collision is on map's path (probably
  NOT — the map key-tuple is 1-element, so only get<0> is used; confirm at W5).

---

## IMPLEMENTATION LOG (append-only — newest last)

### 2026-06-19 — W1 DONE (commit d7b121b). `__integer_pack(N)` implemented.

DONE: `Program::expand_integer_pack_template_args()` (src/parser.cpp, declared
include/madc.h). Rewrites the live token stream in place between a just-consumed
`<` and its matching close, splicing each `__integer_pack(E)...` into literal
integer args (`0,1,...,N-1`; N==0 -> empty pack + drop adjacent comma). Folds E
via `fold_nontype_arg_constant`. Called from `instantiate_template_use` and
`instantiate_opaque_template_use` right after each consumes `<` (the deepest
shared chokepoint — `__integer_pack` only ever appears as an arg to a CLASS
template-id like `_Index_tuple<>`/`integer_sequence<>`).

VERIFIED (map's exact shape, `tmp/w1_bit.mad`): `std::_Build_index_tuple<1>::__type`
-> `_Index_tuple<0>` and `<0>::__type` -> `_Index_tuple<>`, both compile clean.
Also `std::make_integer_sequence<unsigned long,1>` -> `integer_sequence<unsigned long,0>`.
fulltest 656/6 (zero regressions).

### GAPS FOUND during W1 (NOT on map's critical path — do NOT let them block W2–W5)

These surfaced while testing the `make_index_sequence` *wrapper* API. Map does
NOT use that wrapper — map uses `_Build_index_tuple<N>::__type` directly with a
BARE-LITERAL `_Num` (SETTLED item 3), which W1 already handles. Recorded so a
future session neither re-discovers them nor mistakes them for map blockers.

- **GAP-A — `fold_nontype_arg_constant` can't fold a qualified-type functional
  cast `std::size_t(1)`.** EVIDENCE: `std::make_integer_sequence<std::size_t,1>`
  resolved to the GARBAGE opaque type
  `integer_sequence_std__size_t___integer_pack_std__size_t_1_____` (the
  `__integer_pack` left unexpanded because the fold of `std::size_t(1)` returned
  false). `std::make_integer_sequence<unsigned long,1>` folds fine -> the gap is
  specifically the QUALIFIED-NAME functional cast `std::size_t(N)`. Fix in
  `fold_nontype_arg_constant` / `parse_constant_integer_expression`
  (src/parser.cpp ~15482). Needed for: the public `make_integer_sequence` /
  `make_index_sequence` API. NOT map.

- **GAP-B — nested UNqualified alias lookup fails inside an alias body.** EVIDENCE:
  `make_index_sequence<size_t,_Num>` body substitutes to `make_integer_sequence<size_t,1>`,
  which resolves to NULL — `make_integer_sequence` (unqualified) is never found
  because `instantiate_template_alias_use`'s body-resolve (src/parser.cpp ~4330)
  does NOT establish the defining-namespace (`std`) scope, unlike the fn-template
  path which pushes `NamespaceScope(pgm, ft.ns)`. Fix: wrap the body `nextToken()`
  / `resolve_declared_type_token` in a `NamespaceScope(*this, td.defining_namespace)`
  (or `ns_hint`). Needed for: `make_index_sequence`. NOT map.

- **GAP-C — silent garbage on fold failure (low priority).** When `__integer_pack`
  is present but E does NOT fold, `expand_integer_pack_template_args()` leaves the
  pattern untouched, so a bogus opaque type carrying `__integer_pack` in its name
  is produced (see GAP-A symptom) instead of a clean failure. Consider: if the
  `__integer_pack` head matches but the fold fails, treat it as a hard
  substitution failure (return the region unchanged is fine for map; only the
  GAP-A path hits this). Revisit only after GAP-A.

- **NON-GAP (decided): W1 is wired ONLY into the two class-template arg sites.**
  `__integer_pack` is never an ALIAS argument (it only appears inside a class
  template-id in the alias BODY), so the alias arg loop intentionally does NOT
  call the expander. Do not add it there.

---

Result of a recon pass (madc engine + clang Sema + GCC cp/ + libstdc++-13 +
literature) on *how* to finally compile+run `std::map<int,int>; m[1]=2`. The
headline: **the remaining work is much smaller and shallower than the
layer-by-layer grind implied — because `map<int,int>` only ever hits the trivial
base cases of the tuple machinery (recursion depth ≤ 1), and the one genuinely
new primitive is a single ~50-line substitution-time builtin (`__integer_pack`).**

## The hypothesis that was WRONG (and the correction)

Hypothesis going in: implement clang's `__make_integer_seq` / `__type_pack_element`
builtins to sidestep recursive template instantiation.

Correction (grep-verified against /usr/include/c++/13):
- libstdc++-13 uses those builtins ONLY under `#if __has_builtin(__make_integer_seq)`
  — the **Clang** path. madc is on the **GCC** path (`__has_builtin` is false), where:
  - `make_index_sequence` / `_Build_index_tuple` use **`__integer_pack(N)`** (GCC builtin).
  - `tuple_element` uses the recursive **`_Nth_type`** (NO `__type_pack_element` anywhere
    in c++/13 — `__type_pack_element` would be dead code here).
- So the live lever is **`__integer_pack`**, and `__type_pack_element`/`__make_integer_seq`
  are not on the critical path for these headers (implement `__type_pack_element`
  later only for forward-compat with newer libstdc++/libc++).

## The decisive finding: `map<int,int>` hits only depth-≤1 base cases

`map<int,int>::operator[]` piecewise-constructs `pair<const int, int>` from
`tuple<const int&>` (the key) and `tuple<>` (empty, for the mapped value). Tracing
every entity (GCC cp/ + libstdc++-13 line refs in the recon), the depths are:

- `_Build_index_tuple<1>::__type` → `_Index_tuple<__integer_pack(1)...>` → `_Index_tuple<0>`;
  `_Build_index_tuple<0>` → `_Index_tuple<>`. **Non-recursive** (one-shot). Needs `__integer_pack`.
- `__integer_pack(N)` called only with **N ∈ {0,1}** → packs `[]` and `[0]`. Non-dependent.
- `tuple_element<0, tuple<const int&>>::type` → `_Nth_type<0, const int&>::type` = `const int&`.
  `_Nth_type` has **explicit specializations for 0/1/2** — I=0 is **depth 0** (the `_Np-3`
  recursion only starts at I≥3, never reached by map).
- `__tuple_element_t<0, tuple<const int&>>` = the alias → that `::type` (depth 0).
- `_Tuple_impl<0, const int&>` = the **terminal single-element spec** (tuple:489) — no
  recursive `_Inherited` base layer. `get<0>`→`__get_helper<0>`→base-convert→`_M_head`→
  `_Head_base<0, const int&>::_M_head_impl`. **Zero tail walks.**
- Indexed ctor `first(forward<_Args1>(get<_Indexes1>(t1))...)` with `_Args1={const int&}`,
  `_Indexes1={0}` (and `_Args2/_Indexes2` empty): the pack expansion is **a single element**
  — no real lockstep multi-element expansion needed; `second(...)` is an **empty** expansion.

So the feared "deep recursive instantiation + N-way parallel pack expansion +
non-type packs" reduces, for `map<int,int>`, to **single-step specialization
matching madc already does for vector, plus one new builtin and 0/1-element packs.**

## Minimal worklist to a RUNNABLE `map<int,int>` (re-scoped, ordered)

1. **W1 — `__integer_pack(N)` builtin. [DONE 2026-06-19, commit d7b121b — see
   IMPLEMENTATION LOG below.]** A substitution-time pack producer, valid
   ONLY as the entire pattern of a pack expansion (`X<__integer_pack(N)...>`). When N
   folds to a concrete `len ≥ 0`, the expansion yields the constant pack `[0..len-1]`
   (size_t). For map, N ∈ {0,1}. Model: GCC `cp/pt.cc:3846-3912` (`expand_integer_pack`
   builds a vec of `size_int(i)`), recognized via `cp/pt.cc:33231` (`declare_integer_pack`),
   dispatched in `tsubst_pack_expansion` (`cp/pt.cc:14191`). In madc: recognize the call
   `__integer_pack(<const-int>)` inside a template-arg pack-expansion (`X<… …>`) and emit
   the integer-constant pack. This is the only entirely-new front-end primitive.

2. **W2 — non-type parameter packs (`size_t... _Indexes`) at 0/1 elements.** The fn-template
   gate currently bails on a non-type pack. Extend the non-type-param support (already
   landed for scalars: `nontype_subst`/`nontype_params`) + the 0/1-element tid-pack
   handling (already landed for TYPE packs: `tidpack_one`/`tidpack_empty_names`) to a
   non-type pack: bind `_Indexes` to a list of integer values, substitute each as a
   `TokenInt`. For map: `_Indexes1={0}`, `_Indexes2={}`. Mirror the type tid-pack code in
   `instantiate_fn_template_binding`.

3. **W3 — parallel pack expansion at 1 element.** `first(forward<_Args1>(get<_Indexes1>(t1))...)`
   expands `_Args1` and `_Indexes1` in lockstep. For map both have length 1 (or 0), so the
   "lockstep" is a single element — reuse the existing single-element `pattern...` drop +
   per-element substitution; no N-way machinery needed yet. (General N-way lockstep =
   later, for tuple<A,B,...>; clang's model is `TreeTransform::TransformExprs`,
   TreeTransform.h:4163 — same-arity rule — for when it's needed.)

4. **W4 — `std::get<0>` body instantiation (the current live wall).** `get<0>(tuple<X>)`
   now deduces + reaches `instantiate_fn_template_binding` but fails in the body. Needs:
   (a) the alias `__tuple_element_t<0, tuple<X>>` → member-type resolution of the
   instantiated `tuple_element<0, tuple<X>>::type` (depth-0 `_Nth_type<0,X>`); (b) the body
   `__get_helper<0>(__t)` → resolve via overload + derived-to-base to `_Tuple_impl<0,X>`
   (terminal spec) → `_M_head`. Pin whether the failure is the alias-return SFINAE
   (parser.cpp ~31084 `is_templateid_ret`) or the body parse (reducer tmp/tupget2.mad,
   MADC_DEBUG_CTORTMPL).

5. **W5 — piecewise→indexed ctor delegation.** With W1–W4, the piecewise ctor body
   `: pair(__first, __second, _Build_index_tuple<1>::__type(), _Build_index_tuple<0>::__type())`
   instantiates (`sizeof...(_Args1)`→1 via the existing tid-pack count; `_Build_index_tuple`
   via W1) and delegates to the indexed ctor (W2 non-type packs + W3 single-element
   expansion). Then `map_insert.mad` → 0 c2mir errors → RUN.

6. **(parallel/independent) multi-VALUE symbol identity** — only needed if a single TU
   instantiates the SAME fn template at two non-type values with identical signatures
   (`get<0>` and `get<1>`). For `map<int,int>` the key-tuple is 1-element so only `get<0>`
   is used — **likely not on the map<int,int> critical path**; defer unless W4/W5 surface it.

## Architectural verdict

madc's incremental, instantiate-real-libstdc++ approach is **sound** — no rewrite
needed. The "days of grinding" reflected scope mis-estimation (reasoning about
general tuple<A,B,C,...> machinery) rather than a wrong architecture: `map<int,int>`
only exercises the depth-0/1 base cases. The remaining path is W1 (one small
builtin) + W2/W3 (extend the just-landed non-type/tid-pack code to non-type packs +
single-element lockstep) + W4 (the shallow `std::get<0>` body) + W5 (wire the
delegation). Each is a bounded, fulltest-gated commit, not a sub-campaign.

Builtins to implement: **`__integer_pack`** (critical path). Optional/forward-compat:
`__type_pack_element`, `__make_integer_seq` (Clang path; dead for libstdc++-13, cheap
if ever wanted). Do NOT gate on them.

## Source references (for the implementer)
- GCC `__integer_pack`: cp/pt.cc:3846-3912 (expand), :14191-14202 (pack-expansion
  dispatch + "must be entire pattern"), :33231-33242 (declare), :4035-4038 (find-packs);
  cp-tree.h:7239.
- Clang builtins (forward-compat model): SemaTemplate.cpp:3749-3832
  (`checkBuiltinTemplateIdType`), SemaLookup.cpp:917-935 (inject on lookup-fail).
- Clang parallel-pack expansion: TreeTransform.h:4163-4238 (`TransformExprs`, same-arity).
- libstdc++-13: bits/utility.h:140-156 (`_Index_tuple`/`_Build_index_tuple`, the
  `#if __has_builtin(__make_integer_seq)` guard), :230-263 (`_Nth_type`), :84
  (`__tuple_element_t`); tuple:258-278/489-515 (`_Tuple_impl`), :1776-1810
  (`tuple_element`/`__get_helper`/`get`), :2248-2269 (pair piecewise + indexed ctors).
- madc engine: instantiate_fn_template_binding + try_instantiate_namespace_fn_template
  (src/parser.cpp), the just-landed non-type (`nontype_subst`) + 0/1-element tid-pack
  (`tidpack_one`/`tidpack_empty_names`) code; instantiate_template_id / alias resolution.
