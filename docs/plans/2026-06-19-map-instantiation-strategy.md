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

### 2026-06-19 — NEXT WALL PINNED (post-W1, `tmp/mapii.mad` = `map<int,int> m; m[1]=2`).

With W1 in, `map<int,int>; m[1]=2` now parses ALL THE WAY THROUGH to c2mir
(huge progress — pre-W1 it died early at the integer_pack wall). The remaining
blocker is now localized to ONE root cause, confirmed by BOTH c2mir and gcc-on-
emitted-C (the `--emit=c11` + `gcc -c` method):

  - c2mir: `bits/stl_map.h:102:13: too many arguments` (2 check errors; the
    other is a node_handle.h return-type WARNING, not fatal).
  - gcc on `tmp/mapii.c`: `too many arguments to function
    'pair_const_int32_t_int32_t__pair_const_int32_t_int32_t'` (the pair ctor),
    + `incompatible type for argument 2`.

ROOT CAUSE (pinned, evidence in the emitted C `tmp/mapii.c`): **the pair
PIECEWISE variadic constructor template is never instantiated.** The symbol
`pair_const_int32_t_int32_t__pair_const_int32_t_int32_t` is emitted ONLY as the
2-param COPY ctor `pair(const pair&)` (decl @ tmp/mapii.c:918: `(pair*, pair*)`),
but `std::construct_at`'s body calls it with FOUR args — `(location,
piecewise_construct_t, tuple<const int>, tuple<>)` (@ tmp/mapii.c:1777, inside
`__ns_std_construct_at__o2`). So the call `pair(piecewise_construct, t1, t2)`
mis-resolves to the copy-ctor symbol. There is NO 4-param (3-arg+this) piecewise
ctor definition anywhere in the emitted C (`grep -c pair..._pair...` = 2: one
copy-ctor decl + one call).

The needed work (this IS the heart of W4+W5, reached earlier than the worklist
implied — W2/W3 non-type-pack pieces did not block first):
  1. Overload resolution for the ctor call `pair(piecewise_construct_t,
     tuple<const int>&, tuple<>&)` inside the instantiated `construct_at` body
     must SELECT the variadic piecewise ctor template (not the copy ctor).
  2. Instantiate that ctor template with `_Args1={const int}`, `_Args2={}`,
     under a DISTINCT mangled symbol from the copy ctor (today they collide on
     the bare `ClassName__ClassName` name).
  3. Its body delegates to the private indexed ctor
     `pair(tuple&, tuple&, _Index_tuple<_Indexes1...>, _Index_tuple<_Indexes2...>)`
     -> `first(get<_Indexes1>(t1)...)` — which is where W2 (non-type pack
     `_Indexes1={0}`/`_Indexes2={}`), W3 (1-element expansion) and W4
     (`std::get<0>` body) actually get exercised.

REPRODUCE: `bin/madc --std=c++20 --no-embedded-headers tmp/mapii.mad` (2 c2mir
errors). Localize: `bin/madc --emit=c11 ... tmp/mapii.mad > tmp/mapii.c` then
`gcc -c -o /dev/null tmp/mapii.c 2>&1 | grep error:` (16 errors; most are
"conflicting types" from an EMIT-only struct-tag forward-decl ORDERING quirk —
`struct tuple_const_int32_t_` first appears inside a parameter list before its
file-scope definition; that quirk is NOT the c2mir blocker, only the
"too many arguments" pair-ctor one is).

#### REFINED (same day, with `MADC_DEBUG_CTORTMPL=1` — supersedes "never instantiated")

`instantiate_member_ctor_template_for_construction` (parser.cpp:32105) IS called
for the bare `pair_const_int32_t_int32_t` with `nargs=3` (the piecewise call),
finds the placeholder + ctor FuncDef, and DEDUCTION SUCCEEDS for both packs:
```
[ctortmpl] ENTER cdd=pair_const_int32_t_int32_t nargs=3 ... ctors=4
[tidpack] ...__o7 param[1] core='tuple<_Args1...>' concrete='std::tuple<const int32_t*>' uok=1 outpk=1
[tidpack] ...__o7 param[2] core='tuple<_Args2...>' concrete='std::tuple<>'           uok=1 outpk=1
[tidpack] ...__o7: _Args1={int32_t*} _Args2={}
[ctortmpl] try_instantiate ok=0 inst_name=...__pair..._o7
```
So the wall is NOT selection/deduction — it is **`try_instantiate_namespace_fn_template`
returning ok=0 AFTER successful deduction**, i.e. `instantiate_fn_template_binding`
fails on the piecewise ctor BODY. That body is exactly W4+W5:
`: pair(__first, __second, _Build_index_tuple<sizeof...(_Args1)>::__type(),
_Build_index_tuple<sizeof...(_Args2)>::__type())` delegating to the private
indexed ctor `first(get<_Indexes1>(t1)...)`. To resume: rebuild with
`OPTIONAL_CPPFLAGS=-DMADC_DEBUG_CTORTMPL` (and add a localized print/trace inside
`instantiate_fn_template_binding`) to pin WHICH step of that body fails — the
candidates are (a) `sizeof...(_Args1/2)`->1/0 count, (b) `_Build_index_tuple<N>::__type()`
value-construction (W1 handles the TYPE; verify the `()` call), (c) the non-type
`_Indexes` pack substitution in the delegated indexed ctor (W2), (d) `std::get<0>`
in the indexed ctor body (W4).

TWO observations to verify while fixing:
- The pair ctor templates REGISTER with `has_body=0` (parser.cpp:32680 debug =
  "no `{` in the registered token range"). EXPLAINED: in `<bits/stl_pair.h>` the
  piecewise + indexed ctors are DECLARED in-class (body-less); their bodies are
  OUT-OF-LINE member-template definitions
  (`pair<_T1,_T2>::pair(piecewise_construct_t, tuple<_Args1...> __first,
  tuple<_Args2...> __second) : pair(__first,__second,
  _Build_index_tuple<sizeof...(_Args1)>::__type(), ...) { }`). So instantiating
  from the in-class DECLARATION gives a body-less ctor — the out-of-line body
  must be matched + attached via `register_outofline_member_instantiations` (the
  P2 machinery, parser.cpp ~29018). STRONG LEAD: the ok=0 is likely the
  out-of-line piecewise-ctor body NOT being matched/attached to this
  `_Args1={int32_t*},_Args2={}` instantiation (cf. the prior "layer 8 out-of-line
  multi-overload attach" work). Check that path FIRST.
- Deduction yielded `_Args1={int32_t*}` — the `const` on the `const int&` key was
  DROPPED (`[tidpack] resolve 'const int32_t*' -> int32_t*`). Verify this does not
  break correctness of the stored key type (likely benign repr, but check).

### 2026-06-19 — FIX #1: tid-pack-bound typeparams no longer bail (ok=0 -> ok=1). Two new sub-walls.

ROOT CAUSE of the ok=0 FOUND + FIXED (parser.cpp ~30789, in `instantiate_fn_template_binding`):
the "unbound parameter" loop skipped only `binding` + `pack_param`, NOT typeparams
bound via `tid_packs`. The pair piecewise ctor's `_Args1`/`_Args2` are bound as
template-id packs (in `tidpack_one`/`tidpack_empty_names`), NOT in `binding` — so
they looked "unbound", had no default, and the whole instantiation returned false.
FIX: skip a typeparam that is in `tidpack_one` or `tidpack_empty_names` (it IS
bound; the body loop substitutes it). RESULT: `try_instantiate ok=1`,
`FNTPL inst ...__o7 ok` — the piecewise ctor body now parses. fulltest 656/6 (zero
regressions). Committed.

This UNCOVERED the next two sub-walls (map still fails; now 3 c2mir errors). Both
in the emitted C (`tmp/mapii2.c`), confirmed by gcc -c:

- **SUB-WALL A — the piecewise ctor `__o7` is DECLARED but NEVER DEFINED.** The
  in-class declaration instantiates (a prototype `void
  pair..._pair..._o7(pair*, piecewise_construct_t, tuple_int32_t_, tuple__empty_pack);`)
  but no `{ body }` is emitted — the OUT-OF-LINE piecewise-ctor body is not
  matched/attached to this instantiation. This is the `register_outofline_member_instantiations`
  (parser.cpp ~29018) path from earlier "layer 8/11 out-of-line attach" work; it is
  not firing for `__o7`. FIX HERE NEXT.
- **SUB-WALL B — const/ref dropped from the key, so the call doesn't select `__o7`.**
  The call site (`__ns_std_construct_at__o2`) still emits the BARE copy-ctor symbol
  `pair..._pair...(location, fwd(a0), fwd(a1), fwd(a2))` (4 args -> "too many
  arguments") instead of `__o7`, because `__o7`'s param is `struct tuple_int32_t_`
  (`tuple<int>` — const+ref stripped) while the call passes `struct tuple_const_int32_t_`
  (`tuple<const int&>`). Signature mismatch -> overload resolution falls back to the
  copy ctor. The strip is in the tid-pack element resolution
  (parser.cpp ~30594, `resolve_arg_spelling_datadef("const int32_t*") -> int32_t*`
  drops `const`; the tuple-instantiation naming then also drops the pointer:
  `tuple<int&>` -> `tuple_int32_t_`). _Args1 must stay `const int&` so `__o7`'s
  tuple param matches the call's `tuple<const int&>`.

ORDER TO FIX: A and B are independent and BOTH required (B makes the call select
`__o7`; A gives `__o7` a body). After both, the `__o7` body's delegation to the
indexed ctor (`_Build_index_tuple<...>::__type()` + `get<_Indexes>`) is the
remaining W2/W3/W4 work inside the (now-attached) body.

#### 2026-06-19 — B's ROOT is structural: madc has no const-qualified type identity.

Mechanism of the call-site fallback CONFIRMED by reasoning + grep: the emitted call
uses the BARE `ClassName__ClassName` name with 3 args, but the only bare ctor (the
copy ctor) takes 1 arg — so `select_ctor_overload` (cir_builder.cpp:5029) did NOT
match it by arity; it REJECTED `__o7` (signature `(tuple<int>, tuple__empty_pack)`
vs call `(tuple<const int&>, tuple)`) and returned NULL, and the construction fell
back to emitting the convention name. So **B (signature parity) is the gating fix
for call-site selection.**

ROOT of B: `grep` of include/madc.h shows madc has `getReferenceType`/`DataDefREF`
but NO `getConstType`/`DataDefCONST` — `is_const` exists ONLY in the parse-time
`ParsedParamSig`, not as a DataDef. So `const int` is NOT a distinct type from
`int` in madc's type system. Tuple instantiations get distinct names
(`tuple_const_int32_t_` vs `tuple_int32_t_`) only because `const` rides in the
INSTANTIATION-KEY SPELLING; but `resolve_arg_spelling_datadef` (parser.cpp:15041)
deliberately PEELS cv-qualifiers (15067-15077) and returns a DataDef that has lost
the `const`, so when `tuple<_Args1...>` re-instantiates from that DataDef it names
itself WITHOUT const -> mismatch.

TWO fix options for B (decide next session):
  (1) NARROW: carry the deduced pack element's full SPELLING (incl. `const`) so the
      downstream `tuple<_Args1...>` instantiation keys/names with `const int&` and
      matches the call arg. Localized to the tid-pack deduction + the place that
      re-instantiates `tuple<_Args1...>`. Lower risk, possibly fragile.
  (2) STRUCTURAL: add const-qualified type identity to madc (a `DataDefCONST` or a
      const flag in the type key, mirroring `DataDefREF`). Correct + reusable but a
      large change touching the type system broadly. This is the same class of work
      as the first-class-references campaign ([[project_retire_embedded_shims]]).
The empty-tuple naming split (`tuple__empty_pack` from W1's N=0 path vs `tuple` from
the value `tuple<>`) is a THIRD identity mismatch in the same signature — unify the
empty-`tuple<>` instantiation name too.

### 2026-06-19 — SUB-WALLS A+B CLEARED; W2 wall = indexed-ctor delegation. (commits 83c6e7a, 5a18b97)

DONE this session (all fulltest 656/6, on wip/tuple-instantiation-claude):
  - 83c6e7a: empty-tuple identity unify (UNCONDITIONAL — dropped opaque path's
    `__empty_pack` suffix so `tuple<>` is ONE type) + const-qualified deduction
    (GATED `FEATURE_CONST_TYPES`: resolve_arg_spelling_datadef re-applies a peeled
    leading `const` via getConstType). The THIRD identity mismatch (empty-tuple)
    AND sub-wall B's const half are both fixed.
  - 5a18b97: `sizeof...(pack)` folds to the pack's element count in
    instantiate_fn_template_binding (was substituting the pack NAME inside the
    sizeof). `sizeof` is a CONTEXTUAL-identifier keyword — match via
    is_contextual_identifier_token, not a bare ttIdentifier compare.

RESULT (flag-on, verified by --emit=c11 + the [ctortmpl] trace): the pair
piecewise ctor `__o7` now has `tuple_const_int32_t_` params, HAS A BODY, and the
construct_at call SELECTS `__o7` (no longer the bare copy-ctor "too many
arguments"). Sub-walls A and B are CLEARED.

**CRITICAL BUILD GOTCHA (cost a full mis-diagnosis this session):** `make` does
NOT track `OPTIONAL_CPPFLAGS` changes, so a flag-on rebuild reuses a STALE
`parser.o` compiled WITHOUT `-DFEATURE_CONST_TYPES`. ALWAYS `touch src/parser.cpp`
before `make -C src OPTIONAL_CPPFLAGS=-DFEATURE_CONST_TYPES`. Verify with
`strings bin/madc | grep -c "ctortmpl] ENTER"` (needs `-DMADC_DEBUG_CTORTMPL` too)
or by emitting C and checking `__o7`'s param is `tuple_const_int32_t_`, not
`tuple_int32_t_`.

**THE W2 WALL (pinned, evidence in the [ctortmpl] trace):** the private INDEXED
ctor `pair(tuple<_Args1...>&, tuple<_Args2...>&, _Index_tuple<_Indexes1...>,
_Index_tuple<_Indexes2...>)` (stl_pair.h:235, body tuple:2260) is REGISTERED
(trace: `REGISTER owner=pair_const_int32_t_int32_t ... has_body=0 tparams=4`) but
NEVER INSTANTIATED: the trace shows `ENTER cdd=pair_const_int32_t_int32_t nargs=3`
(piecewise → __o7, ok=1) but NO `nargs=4` ENTER. `__o7`'s body delegation
`: pair(__first, __second, _Build_index_tuple<1>::__type(),
_Build_index_tuple<0>::__type())` is DROPPED (emitted `__o7` body is empty `{}`),
so the 4-arg indexed ctor is never built. At CIR time the 4-arg call fails:
`no matching constructor for call to pair_const_int32_t_int32_t(tuple_const_int32_t_,
tuple, _Index_tuple_0, _Index_tuple)` (stl_map.h:102).

W2 worklist (in order):
  1. The delegating-ctor MEM-INITIALIZER `: pair(...)` in __o7's body must trigger
     `instantiate_member_ctor_template_for_construction` for the indexed ctor (the
     placement-`new` path triggers it for the piecewise ctor at parser.cpp ~5188;
     the mem-init delegation parse — parse_ctor_initializer_list, parser.cpp:23200,
     + how ctor_initializers compile — does NOT). Find where ctor_initializers
     resolve their target ctor and add the member-ctor-template instantiation
     trigger there.
  2. NON-TYPE parameter pack deduction: deduce `_Indexes1`/`_Indexes2` (size_t
     packs) from `_Index_tuple<_Indexes1...>` vs `_Index_tuple<0>`/`_Index_tuple<>`.
     Mirror the TYPE tid-pack code (tidpack_one/tidpack_empty_names) for a
     NON-TYPE pack: bind to a list of integer values, substitute as TokenInt.
  3. The body `first(std::forward<_Args1>(std::get<_Indexes1>(__tuple1))...)`:
     single-element parallel expansion of `_Args1`+`_Indexes1` (W3) + `std::get<0>`
     (W4). For map both packs are length 0/1, so no N-way machinery.

**PREREQUISITE for map to compile in the DEFAULT build:** the const fix is GATED
`FEATURE_CONST_TYPES`. map can only compile once that gate is removed — i.e. the
const-qualified-types campaign Phases 3-4 (THREAD const through every naming/
instantiation path so `tuple<const int&>` names consistently everywhere, +
const-transparency sweep) land and fulltest stays 656/6 with the gate OFF. See
docs/plans/2026-06-19-const-qualified-types.md. So the map-completion path is now:
(const Phases 3-4 → make default) ‖ (W2 indexed-ctor delegation) → (W3/W4 get<0>)
→ (W5 wire-up).

### 2026-06-20 — W2 COMPLETE (indexed ctor deduces end-to-end). W4 = std::get<0>/tuple_element.

DONE (commits 4b1aa57, 9b90d04, 247fee1; all fulltest 656/6, zero regr):
  - 4b1aa57: a delegating/base ctor mem-init `: Name(args)` triggers
    instantiate_member_ctor_template_for_construction (parse_ctor_initializer_list)
    — the `nargs=4` indexed-ctor delegation is now REACHED (was absent).
  - 9b90d04: NON-TYPE template-id parameter packs (`size_t... _Indexes`) in the
    fn-template engine — classification no longer bails, deduction folds elements
    to int64 (tid_packs_nontype), body substitution emits TokenInt + drops `...`,
    inst_key keys by value. Mirrors the TYPE tid-pack machinery.
  - 247fee1: (A) member-ctor overload selected by ARITY (count function params of
    each candidate; piecewise=3 vs indexed=4) — was always the first ctor; (B)
    capture per-param type-ness (FuncDef::template_param_is_type +
    OutOfLineMemberDef::inner_is_type via extract_inner_template_typeparams: a
    param introduced by typename/class is TYPE, by a type-name is NON-TYPE) so the
    indexed ctor's `_Indexes` reach the non-type engine (was hardcoded all-type).

VERIFIED ([ctortmpl] trace, flag-on): indexed ctor __o8 deduces ALL four packs —
`param[0] tuple<_Args1...> vs tuple<const int32_t*> uok=1` (resolve
'const int32_t*' -> const int32_t*, const PRESERVED), `param[1] tuple<_Args2...>
vs tuple<> uok=1`, `param[2] _Index_tuple<_Indexes1...> vs _Index_tuple<0> uok=1`
(NON-TYPE pack), `param[3] _Index_tuple<_Indexes2...> vs _Index_tuple<> uok=1` —
and its body instantiates.

**THE W4 WALL (new):** `cir error: no matching constructor for call to
'tuple(tuple_element_0_pair__Tp1__Tp2_)' @stl_map.h:102:13`. The indexed ctor body
`first(std::forward<_Args1>(std::get<_Indexes1>(__tuple1))...)` now runs;
`std::get<0>(tuple<const int&>)` / its `tuple_element` resolution produces
`tuple_element<0, pair<_Tp1,_Tp2>>` with UNSUBSTITUTED class params `_Tp1/_Tp2`
(note: pair, not tuple — suspect the wrong tuple_element specialization or a
get<>(pair) path). W4 worklist (the OPEN item from SETTLED): pin via emit-c11 +
gcc whether get<0> resolves the alias-return `__tuple_element_t<0, tuple<X>>`
(parser.cpp ~is_templateid_ret) or the body `__get_helper<0>` / `_Tuple_impl<0,X>`
terminal; reducer tmp/tupget2.mad + MADC_DEBUG_CTORTMPL. Then W5 (the delegation is
already wired). NOTE the const fix is still GATED — map's default-build completion
still also needs const Phases 3-4 (remove the FEATURE_CONST_TYPES gate).
BUILD: ALWAYS `touch src/parser.cpp` before flag-on rebuilds (make ignores OPTIONAL_CPPFLAGS).

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
