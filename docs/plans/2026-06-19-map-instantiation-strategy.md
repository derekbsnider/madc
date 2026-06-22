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

### 2026-06-20 (later) — W4 COMPLETE (get<0> overload now correct). NEW LAYER: tuple<reference> get reference-collapse.

DONE (commit e8305a2; fulltest 656 passed / 6 failed / 0 timed out — same known map
cluster, ZERO regression):
  - resolve_fn_template_return_by_key (parser.cpp ~32759) now SKIPS any candidate
    whose return type references a template parameter NOT bound by the explicit
    template args. A return that depends on a DEDUCED param cannot be resolved from
    explicit args alone (binding `<0>` only). Previously it returned the FIRST such
    candidate with an opaque/incomplete return type — for `std::get<0>(tuple<const
    int&>)` that was the PAIR overload `get<_Int>(pair<_Tp1,_Tp2>&)` whose return
    `tuple_element<_Int,pair<_Tp1,_Tp2>>::type` resolved to the opaque
    `tuple_element<0,pair<_Tp1,_Tp2>>` (W4 wall). With no candidate resolvable from
    explicit args, the resolver returns NULL and the call DEFERS to full instantiation
    (try_instantiate_namespace_fn_template), which deduces _Elements from the arg and
    correctly rejects pair<> vs a tuple<> arg (unify uok=0) -> selects the tuple
    get<__i>(tuple<_Elements...>&).

VERIFIED: the W4 error `no matching constructor for call to
'tuple(tuple_element_0_pair__Tp1__Tp2_)'` is GONE; map<int,int> now compiles PAST the
get<0> wall, through pair piecewise+indexed ctor instantiation (both `first` and
`second` member-inits succeed), into a deeper c2mir type-check layer.

**THE NEW LAYER (W4b — tuple<reference> get reference-collapse):** map's
`operator[]` builds `tuple<const int&>` (the key, via forward_as_tuple) and the
indexed ctor does `first(std::forward<_Args1>(std::get<_Indexes1>(__tuple1))...)`.
`std::get<0>(tuple<const int&>)` is mis-instantiated: c2mir reports (JIT path)
5 check errors at stl_map.h:102 — "incompatible types in assignment to an arithmetic
type lvalue" (x2), "incomplete struct or union" (x2), "incompatible argument type for
struct/union type parameter" (x1).

TIGHT REDUCER (no map needed — a general tuple<reference>+get bug):
  tmp/getref.mad:
    #include <tuple>
    int main(){ int k=7; std::tuple<const int&> t(k); int x=std::get<0>(t); return x; }
  --std=c++20 --no-embedded-headers ->
    "tmp/getref.mad:5:26: lvalue required as unary & operand" +
    "returning integer without cast for pointer result"; 1 c2mir check error.

ROOT (hypothesis, well-evidenced): reference-COLLAPSE is mishandled in the
instantiated get<0>/_Tuple_impl/_Head_base chain. For element `const int&`,
`_Head_base<0, const int&>::_M_head_impl` is `const int&` (modeled as a pointer
`int *` — CORRECT). `_M_head()` returns `_Head&` = `const int& &` = `const int&`
(collapse), so it must return the pointer AS-IS (the reference). But madc's
instantiated body returns the int VALUE (auto-deref'd the reference member) where the
function's pointer/reference return type is expected ("returning integer without cast
for pointer result"), and the caller then takes `&` of that non-lvalue ("lvalue
required as unary & operand"). Localized site for the consuming side:
class_ctor_initializer_stmts (cir_builder.cpp ~4237-4250) value-inits a member from
a reference-returning call and relies on translate_expr AUTO-DEREF (cir_builder.cpp
~1057 ref_returning_call_type / ~1100) — which works for a plain reference-returning
fn (tmp/refinit.mad PASSES) but NOT for the instantiated get<0> whose return
reference-ness is evidently lost/mismodeled through the deferred-instantiation path.

NEXT (W4b worklist):
  1. Pin whether the instantiated `__ns_std_get__o2` has returns_reference()==true
     (its return `__tuple_element_t<0,tuple<const int&>>&` must resolve to a reference,
     NOT a value). The `__tuple_element_t` ALIAS resolution (-> `_Nth_type<0,const
     int&>::type` = `const int&`) is the suspect — if it drops the `&`, fix there.
  2. Then the _M_head reference-collapse: a `_Head&` return where `_Head` is itself a
     reference must return the stored pointer, not deref to the value.
  3. Re-run tmp/getref.mad to 0 errors, then tmp/mapii.mad.
  NOTE the const fix is still GATED (FEATURE_CONST_TYPES); map's DEFAULT-build
  completion still also needs const Phases 3-4. BUILD: always `touch src/parser.cpp`
  (and src/cir_builder.cpp if its flags change) before a flag-on rebuild.

### 2026-06-20 (W4b part a) — scalar reference-member value-read deref FIXED (@db71765)

W4b splits into sub-layers. Part (a) DONE @db71765 (fulltest 656/6/0, zero regr):
the reference-MEMBER value read had no is_reference()->N_DEREF arm (the reference
VARIABLE read has one at cir_builder.cpp ~8231; the member site was a
never-migrated first-class-refs value-lowering site — plan 2026-06-17 L57-60 keeps
value-lowering per-site in cir_builder keyed on is_reference()). A scalar reference
member thus read as the raw pointer `this->m`; returning it from a T&-returning fn
took `&(this->m)` = T** (std::tuple<const int&>'s _Head_base::_M_head_impl). Fix:
deref a SCALAR reference member on read (referent is_numeric()||is_pointer()),
EXCLUDING aggregates (struct&/class& use the object-access '->' path -> would
double-deref). KEY: test the REFERENT's scalar-ness, NOT class-ness — a ref to a
plain `struct A` (no object members) is a DataDefSTRUCT, class_behind()=NULL (the
testrefmember regression my first 2 attempts caused). Reducers: tmp/refmem.mad
(PASSES), tests/testrefmember.mad (PASSES, b:42 outer:10).

REMAINING **W4b part (b)** — tmp/getref.mad STILL fails: "lvalue required as unary &
operand" + "returning integer without cast for pointer result" at std::get<0>(t).
This is the INSTANTIATED get/__get_helper/_M_head chain's reference-return
recognition (NOT the member read part a fixed). Hypothesis: the instantiated
__ns_std_get__o2 (and __get_helper/_M_head instances) lose returns_reference()
through the deferred-instantiation re-parse — suspect the return
`__tuple_element_t<0,tuple<const int&>>&` alias resolution dropping/mis-collapsing
the `&`. NEXT: trace returns_reference() on each instantiated fn in the chain
(MADC_DEBUG_CTORTMPL getinst), find where the `&` is lost, fix in
try_instantiate_namespace_fn_template's return-type build. getref.mad is the
reducer (default build reproduces it — no flags needed).

### 2026-06-20 (W4b part b investigation) — REAL root is derived-to-base deduction + a BROAD reference-LOWERING gap (NOT "returns_reference lost"). Derived-to-base GATED behind FEATURE_DERIVED_TO_BASE_DEDUCTION (default OFF); part-b reframed as the prerequisite.

Systematic instrumentation CORRECTED the part-(b) hypothesis above. Two distinct
findings, both evidence-backed:

**(1) The `std::get<0>` wall was a DEDUCTION miss, not a lowering miss.**
`std::get<0>(tuple<const int&>)`'s body calls `std::__get_helper<0>(__t)`. `__t` is
`tuple<const int*>&` but `__get_helper`'s parameter is `_Tuple_impl<__i,_Head,
_Tail...>&` — and `tuple` DERIVES FROM `_Tuple_impl<0,...>`. So deducing __get_helper
requires **derived-to-base deduction** ([temp.deduct.call]/4), which madc's
`unify_nested_spec_pattern_arg` did NOT do (it failed at the arity check: pattern
`_Tuple_impl<__i,_Head,...>` has ≥2 fixed args vs the derived `tuple<...>` 1 arg).
FIX (implemented, ~40 lines, parser.cpp): after the two splits, when `pouter !=
couter` and it's not a template-template-param case, resolve the concrete to its
DataDefCLASS, walk its base chain (`base_spelling_for_template`) for a base whose
template outer == the pattern's, and re-unify against that base subobject. PLUS:
accept an integer-literal NON-TYPE index slot (`_Tuple_impl<0,...>`'s leading `0`)
instead of trying to resolve `0` as a type (the scalar non-type param is bound via
explicit args at the try_instantiate layer). With this, `__get_helper` deduces
(uok=1) and **tmp/getref.mad COMPILES and returns the CORRECT value (x=7)**.
map<int,int> advances PAST the get-overload wall to the 5 stl_map.h:102 errors.

**(2) `returns_reference()` is PRESERVED — the doc's part-(b) hypothesis was WRONG.**
Probe (MADC_DBG_RETREF in cir_builder func_def): the instantiated `Box<int&>::get`
has `returns.name='int32_t*' returns_ref=1`. The reference-ness survives the
re-parse. The real defect is in reference-LOWERING, and it is BROADER than the get
chain — at least two distinct bugs, BOTH reproduce WITHOUT derived-to-base:
  - **ref-var-init from a ref-returning call**: `int& r = b.get();` emits
    `int *r = &Box_int32_t___get;` (address-of-FUNCTION, no call) -> SEGFAULT.
    Reducer tmp/refcollapse.mad. NOTE the SAME call works as a value read
    (`int x = b.get()` -> 7) and as an lvalue write (`b.get() = 99` -> k=99); ONLY
    reference-variable BINDING mis-lowers.
  - **ref-return in an instantiated fn**: getline's reference return lowers to an
    integer ("returning integer without cast for pointer result" +
    "lvalue required as unary & operand"). This is what derived-to-base newly
    EXPOSES in getline (basic_istream& return) -> the testfstream/testloop
    regression below.

**Why derived-to-base can't land yet (gated):** enabling it routes getline /
std::forward / get reference-returning instantiations through bug (2), regressing
testfstream + testloop (fulltest 656/6 -> 654/8). Confirmed by bisect: `MADC_NO_D2B`
build -> testfstream GREEN. So derived-to-base is CORRECT but its prerequisite is the
reference-lowering fix. It is committed GATED behind FEATURE_DERIVED_TO_BASE_DEDUCTION
(default OFF -> default build stays 656/6); the helper + unifier branch + non-type
slot are all under the ifdef. Build flag-on: `touch src/parser.cpp` then
`make -C src OPTIONAL_CPPFLAGS=-DFEATURE_DERIVED_TO_BASE_DEDUCTION` (map also needs
-DFEATURE_CONST_TYPES).

**NEXT (reframed W4b part b) — fix reference-LOWERING first, THEN un-gate
derived-to-base:**
  1. Fix `int& r = <ref-returning call>` (tmp/refcollapse.mad): the reference-var
     initializer takes `&` of the function symbol instead of using the call result
     (the call result is ALREADY the pointer/address for a ref-returning call). The
     value-read and lvalue-write paths already handle this correctly — mirror them
     in the reference-binding init path.
  2. Fix ref-return lowering in instantiated reference-returning fns (getline path:
     "returning integer without cast for pointer result").
  3. Re-run the full set: tmp/refcollapse.mad, tmp/getref.mad, tmp/mapii.mad
     (flag-on), then `make -C src fulltest` with FEATURE_DERIVED_TO_BASE_DEDUCTION ON
     -> testfstream/testloop must stay GREEN. Then flip the flag default-on (or
     delete the gate) and re-run the default fulltest.
  These are real C++ reference-correctness bugs, independently valuable, and the
  gate to map<int,int>. Reducers live in tmp/ (gitignored): refcollapse.mad,
  getref.mad, getref2.mad, mapii.mad — run with --std=c++17/c++20 --no-embedded-headers.

### 2026-06-20 (W4b part b — bug 1 of 2 FIXED @6d95bc3; getref now COMPILES+RUNS)

DONE @6d95bc3 (unconditional, fulltest 656/6/0 zero regr): `int& r = <ref-returning
call>` no longer emits `&function`. ROOT: reference_bind_address_expr wrapped the RHS
in TokenAddrOf and took the address of the callee FUNCTION, because a call token
DERIVES FROM TokenVar (TokenCallFunc:TokenVar; TokenCallMethod:TokenMember:
TokenCallFunc) so the `dynamic_cast<TokenVar*>` matched the call. FIX: detect a
reference-returning call (callee FuncDef returns_reference()) BEFORE the TokenVar
branch and route it through TokenAddrExpr — the CIR builder auto-derefs a ref-returning
call and the N_ADDR cancels that, leaving the call-result pointer. A plain ref var / ref
member read is NOT a call (var.type not a FuncDef) → keeps its prior path. Reducer
tmp/refcollapse.mad: r=7, write-through k=99, no segfault.

MILESTONE — with FEATURE_DERIVED_TO_BASE_DEDUCTION + FEATURE_CONST_TYPES, the bug-1 fix
ALSO cleared getref's hard error: **tmp/getref.mad (std::get<0>(tuple<const int&>))
COMPILES + RUNS, x=7** (the "lvalue required as unary & operand" was bug 1). getref2.mad
prints x=7. (Residual NON-FATAL noise in getref: "Expecting integer constant expression"
at instantiated-body lines + an "incompatible return-expr type in function returning a
pointer" warning — both non-fatal, EXIT 0.)

STILL OPEN:
  - **map<int,int>** (tmp/mapii.mad, both flags): STILL 5 c2mir check errors at
    stl_map.h:102 (incompatible types in assignment to an arithmetic type lvalue x2;
    incomplete struct or union x2; +1) PLUS madc-side "Expecting integer constant
    expression" (instantiated body) + "undeclared identifier 'std'" at the call line.
    This is a DEEPER, map-SPECIFIC layer (pair piecewise→indexed ctor / const value_type
    member assignment), BEYOND getref — getref passing did NOT make map pass. NEXT map
    target.
  - **bug 2 (getline ref-return)**: testfstream STILL fails with D2B on
    ("returning integer without cast for pointer result" + "lvalue required as unary &
    operand", 1 check error); the bug-1 fix did NOT help it. NOT yet isolated — the
    obvious reducers PASS: tmp/headref.mad (static method returning a ref member of a
    ref param → H&, H=int&) runs correctly (y=7, k=55), and tmp/refcollapse.mad too. So
    bug 2 is a MORE SPECIFIC construct in the getline/istream& instantiation. Un-gating
    FEATURE_DERIVED_TO_BASE_DEDUCTION still requires testfstream/testloop green → fix
    bug 2 first. Reducers to extend toward getline: returning a reference to a CLASS
    object (not scalar) from an instantiated fn; returning *this / a ref PARAMETER.

### 2026-06-20 (map 5-error layer ROOT-CAUSED via emit-c11 + gcc) — non-type-pack index NOT substituted into the nested `std::get<_Indexes1>` in the pair indexed-ctor body.

Localized the map<int,int> 5 c2mir errors (both flags) by emitting `--emit=c11`
(tmp/mapii.c) and compiling with gcc (precise line numbers; c2mir's stl_map.h:102 is
coarse). gcc points at the pair indexed-ctor `__o8` body:
```c
void pair_..._o8(struct pair_... *__this, struct tuple_const_int32_t_ *__tuple1, ...) {
  struct tuple_const_int32_t_ __madc_objtmp_35;
  tuple_const_int32_t___tuple_..._o5((&__madc_objtmp_35), __ns_std_get((*__tuple1)));
  (__this->first = (*__ns_std_forward__o3((void *)(&__madc_objtmp_35))));   // <-- int = tuple  ERROR
  ...
}
extern long __ns_std_get();                                  // get<0> NEVER instantiated
struct tuple_const_int32_t_ *__ns_std_forward__o3(struct tuple_const_int32_t_ *);  // forward<TUPLE>, not <const int&>
```
This is libstdc++'s `first(std::forward<_Args1>(std::get<_Indexes1>(__tuple1))...)`
(tuple:2265). ROOT (single cause, cascades): the NON-TYPE pack `_Indexes1={0}` is NOT
substituted as `std::get`'s explicit template argument — the emitted call is
`__ns_std_get((*__tuple1))` with NO `<0>`, so get is never instantiated (falls to the
generic `long __ns_std_get()` stub). Downstream: `std::forward<_Args1>` then deduces
its type from get's (wrong, whole-tuple) result → `forward<tuple_const_int32_t_>`, a
tuple temp `__madc_objtmp_35` is constructed, and it is assigned to `first` (an int) →
"incompatible types in assignment to an arithmetic type lvalue" (×2 for first+second) +
"incomplete struct or union" (×2). NOTE this is NOT bug-2 (ref-return) and NOT
derived-to-base — standalone `std::get<0>(t)` instantiates fine (getref runs x=7); it is
specifically **non-type-pack index substitution into a NESTED template-id call inside a
pack expansion** (`get<_Indexes1>` where `_Indexes1` is a non-type pack `{0}`).

NEXT (map W5): in the indexed-ctor body substitution (instantiate_fn_template_binding /
the member-ctor instantiation path that emits `__o8`), when expanding
`std::forward<_Args1>(std::get<_Indexes1>(__tuple1))...`, substitute the non-type pack
element `_Indexes1[k]` as `std::get`'s explicit template-arg `<0>` so get<0> instantiates
(then forward deduces `const int&`, and `first(const int&)`→int via the value deref that
already works). The 1-element expansion count is already correct (single `first=...`); it
is the per-element NON-TYPE template-arg substitution into the nested call that is
dropped. Reducer: tmp/mapii.c (emitted) lines ~1804-1810; tmp/mapii.mad both flags.

### 2026-06-20 (map W5 refined — substitution CORRECT; real blocker = nested `std::get<0>` not instantiated in the re-parsed member-ctor body)

Dumped the SUBSTITUTED indexed-ctor `__o8` body (MADC_DEBUG_CTORTMPL [o8body] probe,
now removed). Deduction AND substitution are CORRECT:
```
: first ( std::forward < const int32_t* > ( std::get < 0 > ( __tuple1 ) ) ... ) ,
  second ( std::forward < > ( std::get < > ( __tuple2 ) ) ... )
```
(`_Args1`→`const int32_t*`, `_Indexes1`→`0` — the `#61` the probe printed is just
overload_token_spelling rendering TokenInt(0) by its token-id; value IS 0.)

Found + FIXED a real sub-bug along the way (bug A, parser.cpp instantiate_fn_template_
binding): the pattern-`...`-drop was gated on `!pack_param.empty()` (a trailing DIRECT
pack), so a pack expansion over only TEMPLATE-ID packs (`forward<_Args1>(get<_Indexes1>
(t1))...` — the pair indexed ctor has NO direct pack_param) left a STRAY `...` after the
substituted pattern. Relaxed the gate with `has_tidpacks` (any tidpack_one/empty or
nontype variant). The `...` is now correctly dropped.

BUT map STILL fails identically — bug A is necessary-not-sufficient. The EMITTED C body
(tmp/mapii.c ~1806) is UNCHANGED: `__ns_std_get((*__tuple1))` with NO `<0>` and get
declared `extern long __ns_std_get()` — i.e. **the nested `std::get<0>(__tuple1)` in the
re-parsed `__o8` body never instantiates get<0>; it binds the implicit `long` dlsym
fallback.** Cascade: forward then deduces the whole-TUPLE type, a `tuple<const int32_t*>`
temp is constructed, and it is assigned to `first` (an int) → the 5 c2mir errors.
Standalone `std::get<0>(t)` DOES instantiate (getref runs x=7) — so the bug is
specifically a NESTED template-id call inside an INSTANTIATED member-ctor body not
triggering its own instantiation (and/or the injected `< TokenInt(0) >` explicit
template-arg not being captured on re-parse).

REAL NEXT (map W5 core): make the re-parse/instantiation of the member-ctor body
instantiate nested template-id calls (`std::get<0>(__tuple1)`) — capture the injected
`<0>` as the call's explicit_template_args and trigger get<0> instantiation, instead of
falling to the `long __ns_std_get()` implicit. Then forward<const int*> deduces correctly
and `first(const int&)`→int. (Could NOT isolate with a user FREE fn template — those hit a
separate "undeclared identifier" call-resolution wall: tmp/nestget.mad, tmp/identref.mad.
The map path is a MEMBER template via instantiate_member_ctor_template_for_construction →
try_instantiate_namespace_fn_template → instantiate_fn_template_binding.) ALSO still
open: bug B (0-element tid-pack: `second(forward<>(get<>(t2)))` should elide the whole
pattern element → `second()`), and the non-fatal "Expecting integer constant expression"
noise from the <tuple> header instantiation.

### ⚡ POST-COMPACTION IMMEDIATE ACTIONS (2026-06-20, mid-task — read FIRST)

State at compaction: branch wip/tuple-instantiation-claude, last PUSHED commit a13c6c7.
UNCOMMITTED in working tree (survives compaction): **bug A** in src/parser.cpp
(instantiate_fn_template_binding — a `has_tidpacks` flag + relaxing the pattern-`...`-drop
gate from `!pack_param.empty()` to `(!pack_param.empty() || has_tidpacks)`; ~8 lines) PLUS
this doc's edits. No debug cruft remains (verified: git diff src/parser.cpp shows only the
has_tidpacks change). `mir-debug-support.md` is UNTRACKED and NOT ours — never `git add`
it; stage explicitly.

A background fulltest (default build, bug A active) was RUNNING at compaction; collect its
result from `/tmp/claude-1001/-workspace-madc/<session>/tasks/b3d9tb2in.output` if present,
ELSE just re-run `make -C src fulltest` (deterministic, ~10 min; cap it). Expected baseline
= **656 passed / 6 failed / 0 timed out** (same 6: testcontainerdtor, testmadc_ns, testmap,
testset, testsubscript, testtuple).

STEP 1 — ✅ DONE (2026-06-20). Fulltest after compaction = **656 passed / 6 failed / 0
  timed out** (zero regression; same 6 baseline fails). bug A committed + pushed. The
  gate-relax dropped no `...` any other tid-pack body needed.

STEP 2 — resume the map W5 CORE (the actual unblock; see the entry just above this one):
  **nested `std::get<0>(__tuple1)` in the re-parsed pair indexed-ctor `__o8` body does NOT
  instantiate get<0>** — it binds the implicit `long __ns_std_get()` fallback, so forward
  deduces the whole tuple, a tuple temp is built, and it's assigned to `first` (int) → the 5
  c2mir errors at stl_map.h:102. The substituted body is already CORRECT
  (`first(std::forward<const int32_t*>(std::get<0>(__tuple1)))`). Fix: when the member-ctor
  body is re-parsed/instantiated (instantiate_member_ctor_template_for_construction →
  try_instantiate_namespace_fn_template → instantiate_fn_template_binding), the nested
  `std::get<0>(...)` must capture the injected `<0>` as explicit_template_args and trigger
  get<0> instantiation. Standalone `std::get<0>(t)` works (tmp/getref.mad → x=7); the gap is
  the NESTED-in-instantiated-body call. Build flag-on:
  `touch src/parser.cpp; make -C src OPTIONAL_CPPFLAGS="-DFEATURE_DERIVED_TO_BASE_DEDUCTION -DFEATURE_CONST_TYPES"`.
  Reducers (tmp/, gitignored): mapii.mad (the target), getref.mad (works), refcollapse.mad
  (bug-1, works). Localize via `--emit=c11 tmp/mapii.mad > tmp/mapii.c; gcc -std=c11 -c
  tmp/mapii.c` (gcc gives precise lines; the bad assignment is the `first = tuple` at the
  emitted `__o8`). NOTE the free-fn-template reducer route (tmp/nestget.mad) hits a SEPARATE
  "undeclared identifier" wall — stay on the member-template path.
  Also still open after that: bug B (0-element pack `second(forward<>(get<>(t2)))` →
  `second()`), then bug-2 (getline ref-return) to un-gate FEATURE_DERIVED_TO_BASE_DEDUCTION.

This session's PUSHED commits: 954742a (derived-to-base GATED), 6d95bc3 (ref-init bug-1 →
getref runs x=7), a13c6c7 (doc). Memory: project_map_set_campaign.md UPDATE 58 + 59.

---

### STEP 2 ✅ ROOT-CAUSED + FIXED (2026-06-20) — uneval-depth leak suppressed nested instantiation

The W5-core hypothesis (above) was WRONG about the mechanism — it is NOT a `<0>`-capture
gap. SYSTEMATIC-DEBUGGING (built-in traces `MADC_DEBUG_CTORTMPL` + `MADC_DEBUG_GETREG`,
flag-on build) proved, step by step on tmp/mapii.mad:
- The substituted `__o8` body IS correct: `first(std::forward<const int32_t*>(std::get<#61>
  (__tuple1)))` (`#61` = `overload_token_spelling` of the injected `TokenInt(0)`). Deduction
  is correct: `_Args1={const int32_t*}` (= const int&), `_Indexes1={0}` (`[tidpack]` dump).
- `std::get` resolved fine in the body (`[getreg] RESOLVE std::get -> __ns_std_get`), captured
  its explicit arg (`[nsres] ntargs=1 peek=(`), reached the call-build (`[bcf]`) — but with
  **`uneval=1`**. The two increment sites of `unevaluated_operand_depth` are decltype (4964)
  and the speculative type-deducer (15855), both RAII-balanced; the `__o8` body INHERITED
  depth 1 from the (unevaluated) context that triggered the pair-ctor instantiation.
- parseCallFunc's instantiation gate is `if (unevaluated_operand_depth == 0)` (parser.cpp
  13186). With depth 1 it SKIPPED `instantiate_namespace_fn_template_for_call` → `std::get<0>`
  never instantiated → bound the `long __ns_std_get()` fallback (forward also skipped, but it
  resolved to an already-existing overload so it didn't error — get NEEDED a fresh inst).

**FIX (deepest layer, UNCONDITIONAL, parser.cpp instantiate_fn_template_binding ~31711):**
a template-body parse is a fresh EVALUATION context ([temp.inst] — an instantiated definition
ODR-uses its callees regardless of the triggering use's unevaluated-ness). SAVE + reset
`unevaluated_operand_depth = 0` around the body parse, RESTORE after — exactly like the
sibling state already saved there (compounds / class_scope_stack / cur_func_name /
current_linkage). It cannot cause EXTRA instantiations (the depth==0 gate at the call site
still decides WHETHER to instantiate; this only governs the body of an already-decided one).

RESULT: `std::get<0>` now instantiates (`getinst` 0 → 33; emitted C has a real
`type *__ns_std_get__o2(struct tuple_const_int32_t_ *)` with a body). map c2mir errors **5 → 4**.
Fulltest **656/6/0 — ZERO regression**. Committed unconditionally (not flag-gated).

### ⚠️ CORRECTION (2026-06-20, later) — item 1 below is SUPERSEDED; `tuple_element::type` RESOLVES FINE
The emit-c-based "layer 1" analysis (item 1 + the LAYER-1 NARROWED block) was chasing an
**emission artifact, NOT the tree bug.** PROVED via a trace in `resolve_class_type_alias`:
`C<std::tuple<int>>::type` (reducer ps3) and `D<nn::Box<int>>::type` (ps5) BOTH resolve
correctly to `int` (`found=1`, the `type` alias IS registered on the instantiated class — TYPEDEF
trace confirms `cls=C_std__tuple_int_ dd=int`). The `typedef char type;` + bare `type` in `--emit=c11`
output is a SEPARATE real bug: parsing `<tuple>`/`<compare>` registers a **namespace-scope**
`using type = …` (in `std::__cmp_cat`) into the GLOBAL `user_typedef_names` (parser.cpp ~21233 /
~26908 take the class-scope-empty branch for a namespace-scope alias), so the emitter renders ANY
`type`-named type as the bare `type` + a char fallback. ps5 avoids it only because it doesn't
`#include <tuple>`. → **Two takeaways: (A) `--emit=c11` is UNRELIABLE for judging instantiated
types while `<tuple>` is included — use the JIT/tree (c2mir errors) instead. (B) a genuine,
separable bug worth its own fix: a namespace-scope `using X = …;` must NOT pollute the global
`user_typedef_names` (it is qualified-only); this corrupts `--emit=c11` output (a first-class
output per backend-strategy).** Reducers ps1–ps6 reproduce the EMIT artifact, not the map tree bug.

**ACTUAL remaining map JIT blocker (tree-level, from `bin/madc … tmp/mapii.mad` WITHOUT --emit):**
4 c2mir check errors at `/usr/include/c++/13/bits/stl_map.h:102` —
`incompatible types in assignment to an arithmetic type lvalue` + `incompatible argument type
for struct/union type parameter` (+ a node_handle.h:64 warning). stl_map.h:102 is `operator[]`'s
piecewise-construct/emplace path. Investigate on the TREE (the `first`(int) = struct assignment
is real in the tree, not just emit) — do NOT trust `--emit=c11` here. Also still: "undeclared
identifier 'std'" (2×) on the JIT path. NEXT SESSION START THERE, not on item 1.

### ✅ REFERENCE-COLLAPSING FIX landed (2026-06-20, @7a95e79) — map 4 → 3; `first=tuple` gone
RECON (clang cross-ref `/workspace/llvm-clang-src`): the faithful JIT reducer is
**tmp/te_ref.mad** (`const int& r = std::get<0>(std::tuple<const int&>)`); the non-ref twin
`tuple<int>` works. ROOT: madc lowers refs to pointers (DataDefREF IS-A DataDefPTR), so a ref's
canonical spelling is the `*`-form, IDENTICAL to a real pointer's. `template_type_arg_spelling`
baked that into instantiations' `canonical_cpp_spelling` (`tuple<const int&>` → `tuple<const
int32_t*>`); the tid-pack deduction round-trips the element through that spelling
(`split_template_id_spelling`→`resolve_arg_spelling_datadef`) and rebuilds a PLAIN POINTER, losing
the `DataDefREF`. So libstdc++'s `_Head&` (`__get_helper`'s return, `_Head`=`const int&`)
double-wrapped to `const int**` → c2mir return-type mismatch → undefined `__ns_std_get`. **FIX
(@7a95e79, parser.cpp template_type_arg_spelling, gated FEATURE_CONST_TYPES):** render a reference
arg as `referent&`, so the canonical spelling round-trips back to a `DataDefREF` where
`getReferenceType`'s existing collapsing ([dcl.ref]p6) yields the single ref. CLANG PARALLEL:
clang keeps the ref in the QualType (`LValueReferenceType`) and `BuildReferenceType`
(SemaType.cpp:2256, [dcl.ref]p6) collapses STRUCTURALLY — never via a string. RESULT: `__get_helper`
now `int*` (was `int**`); the `first=tuple` *"assignment to an arithmetic lvalue"* error is GONE;
map 4 → 3; fulltest 656/6/0 zero regr (gated).

REMAINING map errors (3, flag-on): (a) `stl_map.h:102:0 incompatible argument type for
struct/union type parameter`; (b) JIT `undeclared identifier 'std'` (2×). Plus the std::get
`const-ref` reducer (te_ref) still not fully green: `get`'s DECLARED return
`__tuple_element_t<0, tuple<const int&>>` still resolves to the `type` fallback (warning
"incompatible pointer types of return-expr and function result"). The `type`(char) here is NOT the
`user_typedef_names` pollution — that was TESTED (gated user_typedef_names to file/block scope at
the 3 insert sites) and REVERTED: it changed te_aliasref `r=1`→garbage but did NOT fix te_ref or
reduce map (still 3), so it is a separate emit-only concern, NOT this blocker. The real next layer
is **the `__tuple_element_t<I, tuple<…>>` ALIAS / `tuple_element<0, tuple<const int&>>::type`
resolution for a REFERENCE element** (non-ref `tuple<int>` resolves fine; ref → the `type`
fallback). Reducer te_ref.mad / te_aliasref.mad (both wrong-value). Investigate
instantiate_template_alias_use + the `tuple_element` partial-spec `typedef _Head type` when
`_Head` is a `DataDefREF`. (Own-template reducers psref.mad diverge — use te_ref/te_aliasref.)

### REMAINING map<int,int> layers (SUPERSEDED emit-c analysis — see CORRECTION above)
+ flags `-DFEATURE_DERIVED_TO_BASE_DEDUCTION -DFEATURE_CONST_TYPES` (touch src/parser.cpp first):
1. **`get<0>` RETURN TYPE resolves to the `type` fallback (the real root of the `first`
   breakage).** Emitted: `type *__ns_std_get__o2(struct tuple_const_int32_t_ *)` where
   `typedef char type;` (mapii.c line ~30) is madc's UNRESOLVED-type fallback. get<0> on
   `tuple<const int&>` must return `const int&` (rendered `const int32_t*`), not `type`/char.
   Its body is `return &*__ns_std___get_helper__o2(__t)` and `__get_helper__o2` returns `int**`
   — so the chain `std::get -> __get_helper -> _Tuple_impl::_M_head` loses the type. Because the
   return is `type`(char), the enclosing `first(std::forward<const int32_t*>(get<0>(__tuple1)))`
   can't bind forward<const int&> to a `type` arg, so forward mis-resolves to the EXISTING
   `forward__o3` (= `forward<tuple<const int>>`, from the outer piecewise path) and madc wraps
   the value into a `tuple<const int>` temp `__madc_objtmp_35`, then `first = *forward__o3(&tmp)`.
   IMPORTANT: the WORKING control tmp/getref.c has the SAME `type*` get return — it only
   "works" (x=7) by luck (deref of a char* over a small int). So FIX get<0>'s return-type
   resolution; the forward mis-resolution + tuple temp are a downstream cascade that should
   vanish once get returns `const int&`. EXACT chain (libstdc++-13 /usr/include/c++/13/tuple
   :1802): `std::get<__i>(tuple<_Elements...>&)` returns `__tuple_element_t<__i,
   tuple<_Elements...>>&`. `__tuple_element_t` is an ALIAS template = `typename
   tuple_element<__i, _Tp>::type`. So the instantiated return must resolve
   `__tuple_element_t<0, tuple<const int&>>&` → `tuple_element<0, tuple<const int&>>::type` (=
   `const int&`) → ref-collapse `const int& &` = `const int&`. madc's fn-template
   return-type computation (`skipped_template_function_return_type`) is producing the `type`
   fallback instead — i.e. it is NOT resolving the dependent alias-template + class-template
   `::type` member with the substituted non-type index `__i=0` and type-pack `_Elements`.
   That is the deepest-layer fix for layer 1. Compare against gcc's
   `std::get<0>(std::tuple<const int&>)` → `const int&`.

   **LAYER-1 NARROWED TO A PRECISE ROOT (2026-06-20, systematic-debug w/ MADC_DEBUG_PSPEC
   traces I added then removed; reducers tmp/ps3.mad..ps6.mad).** The failure is NOT
   get-specific and NOT the alias chain per se — it is `tuple_element<I, std::tuple<...>>::type`
   resolving to the `type` (char) fallback. Reduced to a one-template partial spec:
   `template<typename T> struct C; template<typename H> struct C<std::tuple<H>>{typedef H type;};`
   then `C<std::tuple<int>>::type` → FAILS (emits `typedef char type;`). The DISCRIMINATOR
   (ps3 fails / ps5 works): the nested partial-spec arg is **std::tuple** specifically.
   - `B<std::tuple<H,R...>>` (ps2), `C<std::tuple<H>>` (ps3, no pack), `E2<tuple<H,R...>>`
     (ps6, unqualified via using) — ALL FAIL.
   - `A<0,H>` (ps1, non-type-0 + plain type), `D<Box<H,R...>>` (ps4, user variadic + pack),
     `D<nn::Box<H,R...>>` (ps5, qualified user template) — ALL WORK.
   So it is NOT the pack, NOT non-type, NOT qualification — it is std::tuple as the nested
   concrete arg. PROVED via traces that the two cases are IDENTICAL through the whole
   instantiation: match_partial_specialization SELECTS the spec for BOTH (`nest=1`); both reach
   the REAL body-parse (`opaque=0`, not the dependent-shell branch); both have `dep_surf=1`,
   `pack_real_inst=0`; both deduce `subst H=int`; both emit an EMPTY struct (typedefs aren't C
   struct fields). The ONLY divergence is the post-instantiation **`::type` member-type lookup**:
   `D<nn::Box<int>>::type` resolves to int, `C<std::tuple<int>>::type` resolves to the char
   fallback. NEXT: trace the use-site qualified-type resolution of `C<std::tuple<int>>::type`
   (how the instantiated class's `type` type-alias is registered during the spec body parse and
   looked up afterward) — the std::tuple arg makes that lookup miss where a plain user template
   hits. Suspect the instantiation KEY/mangling for a std::tuple-arg'd template differs between
   the body-parse registration and the use-site `::type` lookup (std::tuple<int>'s use-site
   spelling/mangled fragment vs the canonical used at instantiation). Reducers ps1–ps6 in tmp/.
2. **bug B — empty-pack `second`.** `_Args2={}`, `_Indexes2={}`; the 0-element expansion of
   `second(std::forward<_Args2>(std::get<_Indexes2>(__tuple2))...)` must collapse to `second()`
   (default-construct the mapped `int`). Emitted instead: `second = *forward__o5(&*get__o2(
   __tuple2))` with `__tuple2 : tuple<>` — a type mismatch (get__o2 wants tuple<const int>*).
   This is the empty-pack pattern-drop (sibling to bug A but for the WHOLE `name(pattern...)`).
3. **"undeclared identifier 'std'" (2×, location 141:69, injected-token loc unreliable).** NOT
   from parsePostfixChain 13681 (traced — did not fire). Re-locate among 13990/14341/19990/
   20061/20171/20246 once 1+2 are fixed (may be a cascade of 1/2).

Reducers (tmp/, gitignored, run WITH the two flags): mapii.mad (target); WORKING controls
memget.mad / memfwdget.mad (freshly-lexed nested get instantiates fine — proves the bug was
pack-substitution + uneval, now fixed); packget.mad / deleg.mad hit OTHER unsupported gaps
(two-pack ctor selection, make_index_sequence) — not faithful, don't chase them.

Built-in trace flags for this area (add to OPTIONAL_CPPFLAGS, then set the env var):
`MADC_DEBUG_CTORTMPL` (=1: `[tidpack]`, `[getinst]`), `MADC_DEBUG_GETREG` (=1: `[getreg]`
register/RESOLVE), `MADC_DEBUG_NS_RESOLVE`, `MADC_DEBUG_FNTPL`(+`MADC_DEBUG_FNTPL_DUMP=<key>`).

### 2026-06-20 (later) — parser-side noise CLEARED; bug-1 of W4b part(b) is ALREADY FIXED (stale-handoff correction)

Commit 9c6f820 (zero regression: default fulltest **656 passed / 6 failed / 0 timed
out / 18 skipped** — the same 6 known map-cluster tests) — two parser fixes that were
emitting spurious stderr into `mapii.mad`'s output and masking the real c2mir blocker:

1. **Deref of a namespace-qualified call (`*ns::f()`).** Was throwing "undeclared
   identifier 'ns'": the unary-`*` operand guard in `parseExpr_operatorArm` took the
   simple-variable path because its exclusion list (tkOpBrk/tkDeRef/tkDot/tkOpSqr)
   omitted the scope token **tkNS**. Added tkNS at both deref-operand guard sites
   (single-level ~19953, multi-level-inner ~20194); `*ns::...` now falls through to the
   general qualified-expression parse (the final-else `parseExpression`). This is the
   libstdc++ `iter_reference_t = decltype(*std::declval<_Tp&>())` alias-instantiation
   path — the source of mapii's 2× visible "undeclared identifier 'std'".
   KNOWN LIMITATION (pre-existing, shared with the `*tfunc<T>()` unbounded fallback at
   ~20357): `*ns::f() <binop> …` over-consumes (parses `*(ns::f() binop …)`); the map
   path (`*std::declval<_Tp&>()`, no trailing binop) is correct. Bound it later via the
   same fix that would bound the template-fn fallback.
2. **`capture_constant_initializer_value` cerr leak.** A dependent `static constexpr`
   member (libstdc++ `value = static_cast<…>(…)`) makes the speculative constant-fold
   Throw; the catch restores state but `throwbuf::sync()` printed to stderr first. Muted
   std::cerr around the speculative parse (same idiom as `fold_nontype_arg_constant` /
   `constraint_expression_well_formed`). Source of mapii's 3× "Expecting integer
   constant expression".

**STALE-HANDOFF CORRECTION (verified at HEAD, default build):** bug-1 of W4b part(b)
— *"ref-var-init from a ref-returning call: `int& r = b.get();` emits `int *r =
&Box…get;` (address-of-FUNCTION) → SEGFAULT"* — **is ALREADY FIXED.** `tmp/refcollapse.mad`
now runs and prints `r=7 / k=99`; emit-c is `int *r = (&(*Box_int32_t___get((&b))));`
(proper call+deref+address). Do not re-chase it.

**REMAINING map blocker (unchanged, now cleanly isolated):** `tmp/getref.mad`
(`int x = std::get<0>(std::tuple<const int&>)`) still fails on the DEFAULT build:
"lvalue required as unary & operand" + "returning integer without cast for pointer
result" (1 c2mir err). With `-DFEATURE_DERIVED_TO_BASE_DEDUCTION` the get-chain deduces
(__get_helper via derived-to-base) — but that flag stays gated until **bug-2** (ref-return
in an instantiated fn lowering to integer; exposed in getline/basic_istream& →
testfstream/testloop) is fixed. So the live worklist is: isolate+fix bug-2 → un-gate
derived-to-base → getref green → re-check mapii's 3 c2mir errors (stl_map.h:102 struct-arg
+ 2× incomplete struct).

### 2026-06-20 (later, cont.) — map's 3 c2mir errors RELIABLY narrowed to bug B (empty-pack pair indexed ctor); emit-c is a dead-end diagnostic here

Localized the 3 mapii flag-on c2mir errors (stl_map.h:102 incompatible-arg-for-struct/union
+ 2× incomplete struct) to **bug B — the empty-pack pair indexed ctor** (`_Args2={}`,
`_Indexes2={}`), corroborated three independent ways:
- emit-c shows a leaked `struct _Tp2 { pair _M_t; }` (pair's 2nd param unsubstituted; def
  + empty dtor only, never an arg) — a botched instantiation symptom in the pair-ctor area.
- The pair indexed ctor `__o8` takes the index tuples BY VALUE:
  `__o8(pair*, tuple_const_int32_t_* __tuple1, struct tuple* __tuple2, _Index_tuple_0, _Index_tuple)`.
  `__tuple2` is the EMPTY `tuple<>` (the mapped-value piecewise arg) and the 5th param is the
  EMPTY `_Index_tuple<>` — i.e. the empty-pack `second(std::forward<_Args2>(std::get<_Indexes2>(__tuple2))...)`
  must collapse to `second()` (default-construct the mapped int). That empty-expansion is the defect.
- Matches the original W3/bug-B prediction (empty-pack `name(pattern...)` → `name()`).

**DEAD-END WARNING for the next session — do NOT re-run emit-c+gcc here.** `--emit=c11` on
mapii is NOT faithful to the c2mir tree at this layer: gcc on tmp/mapii_flagon.c reports a
DIFFERENT error set (a cascade of "conflicting types" for __o8/_M_create_node/etc. + one
"formal parameter N is incomplete") than c2mir's actual errors — those are emission artifacts
(duplicate/inconsistent forward-decls in the rendered C), not the tree defect. Both empty
structs (`struct tuple {}` @823, `struct _Index_tuple {}`) ARE emitted complete, so the
"incomplete" gcc sees is an artifact too. The ONLY reliable localizer is **c2mir-tree
instrumentation**: temporarily print the tag name at c2mir.c:11960 ("incomplete struct or
union") and the arg/param type tags at c2mir.c:8473 ("incompatible argument type for
struct/union type parameter"), rebuild libmir + relink madc flag-on, run mapii, then REVERT
the fork edit. (I drafted that c2mir.c:11960 diagnostic this session but reverted it rather
than rebuild the pinned libmir.a in place — do it against a throwaway libmir copy, or accept
the in-place rebuild + restore from the pin.)

NEXT (precise): fix the empty-pack expansion of `second(...)` (and the empty `_Index_tuple<>`
by-value param threading) in the pair indexed-ctor instantiation — the empty `name(pattern...)`
must lower to `name()` with the empty `tuple<>`/`_Index_tuple<>` consistently complete. This is
the sibling of bug A (which dropped the pattern-`...` for the NON-empty single-element pack);
bug B is the ZERO-element case for a whole `name(pattern...)` member-init.

### 2026-06-20 (FINAL this session) — bug B FIXED; map's last 3 c2mir errors RELIABLY root-caused (via c2mir-tree instrumentation) to a DUPLICATE/incomplete `_Index_tuple<0>` instantiation. The `_Tp2`/empty-`tuple<>` emit-c signals were RED HERRINGS.

**Bug B FIXED (commit 8b6c7ab, zero regression 656/6/0/18):** a 0-element pack
expansion of a COMPLEX pattern (`second(std::forward<_Args2>(std::get<_Indexes2>
(__t2))...)`, `_Args2`/`_Indexes2` empty) now elides the WHOLE pattern → `second()`
(emits `__this->second = 0;`), instead of the malformed `second(forward<>(get<>()))`.
instantiate_fn_template_binding: track per-pattern empty/non-empty tid-pack substitution
since the last `...`; at the pattern-`...` drop, if empty-only, walk `inj` back (paren-
balanced) to the pattern's `(` (kept) / leading `,` (removed) and truncate.

**THE LAST 3 c2mir ERRORS — RELIABLY ROOT-CAUSED (do NOT trust emit-c here).**
Instrumented c2mir.c (temporary fprintf at :11960 "incomplete struct or union" and
:8473 "incompatible argument type for struct/union type parameter"; rebuilt libmir.a
via `make -f GNUmakefile libmir.a`, ran mapii flag-on, then REVERTED + rebuilt clean).
The diagnostic output:
```
MADC_DBG struct-arg mismatch code=65(N_CALL) left='_Index_tuple_0' right(mode=4)='_Index_tuple_0'
MADC_DBG incomplete struct: mode=4(TM_STRUCT) tag='_Index_tuple_0'   (×2)
```
So all 3 errors are about **`_Index_tuple_0` (= `std::_Index_tuple<0>`)** — NOT `_Tp2`,
NOT the empty `tuple<>` (those emit-c signals were artifacts; emit-c collapses two
distinct same-named DataDefs into one printed `struct _Index_tuple_0 {}`). c2mir's tree
has **TWO incompatible `_Index_tuple<0>` types**: one COMPLETE (the
`_Build_index_tuple<1>::__type()` temp `__madc_objtmp_33` built in the `__o7` body) and
one INCOMPLETE (the pair indexed-ctor `__o8`'s parameter type
`_Index_tuple<_Indexes1...>`, `_Indexes1={0}`). The `__o7`→`__o8` call passes the
complete one to a param typed as the incomplete one → "incompatible argument type" +
the param/temp are "incomplete struct" ×2.

ROOT (high-confidence): `_Index_tuple<0>` is instantiated from TWO syntactic origins
that don't share the instantiation cache (datatype_map keyed by `registered_mangled`,
parser.cpp ~3677): the `__o8` PARAM type `_Index_tuple<_Indexes1...>` is instantiated as
an INCOMPLETE dependent placeholder (parser.cpp ~3690-3704) DURING the ctor-signature
parse while `_Indexes1` is still an unbound non-type pack, and it is never completed/
superseded when the concrete `_Index_tuple<0>` (the complete `_Build_index_tuple::__type`
result) is later instantiated — OR the two get different mangled names (the non-type
`0` mangles differently between the param-pack path and the `__integer_pack`/`__type`
path), so the cache never dedupes them.

NEXT (map's final step — focused): make the two `_Index_tuple<0>` instantiations resolve
to ONE complete DataDef. Confirm the two `registered_mangled` strings (add a temp trace
at parser.cpp:3677 keyed on a name containing "_Index_tuple") — if they DIFFER, fix the
non-type-pack param-type mangling to match the `__type`/`__integer_pack` path; if they
MATCH, make the later complete instantiation REPLACE the incomplete placeholder in
datatype_map (the cache-check at 3681 already falls through when the cached type
`is_incomplete_template_class_type` + body non-empty — verify that fall-through actually
re-registers the COMPLETE type under the same key rather than minting a second DataDef).
This is the last bug for `map<int,int>` (W5 core); bug A + uneval + ref-collapse + bug B
are all done.

### 2026-06-20 (final, cont.) — the `_Index_tuple<0>` dedup is NOT at instantiate_template_use@3677 (verified by trace); two creation paths to reconcile

Added a temp trace at the instantiate_template_use cache check (parser.cpp:3677,
`datatype_map.find(registered_mangled)`, gated MADC_DBG_IDXT, reverted) — it did NOT
fire for any `_Index_tuple` instantiation on mapii flag-on. So `_Index_tuple<0>` is NOT
created/deduped through that standard template-use cache; do not look there.

The two `_Index_tuple<0>` DataDefs come from these paths (next session: instrument
each, compare the resulting DataDef identity + completeness):
- `_Build_index_tuple<1>::__type()` in the pair piecewise→indexed ctor mem-init
  delegation — resolved around parser.cpp:23397 (the `: pair(__first, __second,
  _Build_index_tuple<...>::__type(), ...)` handling) → the COMPLETE `_Index_tuple<0>`
  temp. Likely via a member-alias/`__type` resolution (resolve_class_type_alias /
  instantiate_template_alias_use) + the `__integer_pack` expansion (parser.cpp:2931).
- the pair indexed-ctor `__o8` PARAMETER type `_Index_tuple<_Indexes1...>` (non-type
  pack {0}) — the non-type-pack deduction/substitution around parser.cpp:30672/31525 →
  the INCOMPLETE copy.
FIX: make both resolve to one complete DataDef (shared cache key, or have the complete
instantiation supersede the param-type placeholder). This is the last bug for map<int,int>.

### 2026-06-20 (FINAL-2) — empty-struct completion attempt: necessary insight, NOT sufficient; the `_Index_tuple<0>` dedup is ARCHITECTURAL (instantiation identity). Reverted; bug B + parser fixes stay.

Pinned the incomplete-`_Index_tuple<0>` creation precisely (gdb on the DataDefSTRUCT ctor):
it's made an INCOMPLETE dependent placeholder in **`instantiate_opaque_template_use`
(parser.cpp ~3129)**, reached via the opaque routing at **3353-3355** — `_Index_tuple`
is variadic (non-type pack `size_t... _Indexes`), `template_pack_real_instantiable`
rejects non-type packs, so it never real-instantiates even with a concrete `<0>`. Both
`_Index_tuple` (empty pack, from a typedef in a template DEF) and `_Index_tuple_0`
(concrete, from a `using` alias) are created there once each (DataDefSTRUCT `(n,s,d)`
incomplete-path; the member-path ctor never fires; `TokenDataType::definition` is a
shared `DataDef&`, so clones share one DataDef).

ATTEMPTED FIX (reverted): in instantiate_opaque_template_use, when the body is an empty
`{ }` (tkOpBrc directly followed by tkClBrc) AND args are concrete (no `...`) AND the
template has NO partial spec, create a COMPLETE struct (`is_complete=true; finalize()`)
instead of a placeholder. Findings from 3 iterations:
1. **Unguarded → SIGSEGV.** Completing `_Nth_type<0,const int&>` (whose PRIMARY body is
   empty but whose partial SPECS supply `using type = ...`) drops its `::type`, so
   `tuple_element`'s `using type = typename _Nth_type<…>::type` resolves NULL →
   "Expecting type in using alias" Throw → a dangling `struct_map` entry → crash in
   `CirBuilder::translate_module` / `as_user_class` (cir_builder.cpp:375, NULL vtable).
   → Guard added: skip templates with `partial_spec_map.count(tname)`. (`typename` IS
   handled — resolve_declared_type_token:4994 → resolve_typename_type_token; the NULL
   came from `_Nth_type::type`, not the `typename` keyword.)
2. **Concrete-args guard** (`arg.find("...")==npos`) added so a dependent
   `_Index_tuple<_Indexes...>` (template-DEF parse) stays a placeholder.
3. **Guarded → back to 3 c2mir errors (NO crash).** Trace confirmed `_Index_tuple`
   and `_Index_tuple_0` BOTH hit the completion (empty_body=1 concrete=1 has_spec=0),
   yet c2mir STILL reports `_Index_tuple_0` incomplete. So setting `is_complete=true +
   finalize()` on the shared DataDef did NOT make the cir/c2mir emission treat it as a
   complete struct. ⇒ EITHER (a) the cir_builder's empty-struct completeness signal is
   something OTHER than `is_complete` (an empty completed struct may still emit as a
   forward `struct X;` because it has 0 members), OR (b) the `__o8` param-type
   `_Index_tuple_0` is a SECOND DataDef created via a path that is NOT the opaque ctor
   and NOT the member ctor (a DataDefCLASS COPY — untraced), so it never shares the
   completion. The c2mir diag `struct-arg mismatch left='_Index_tuple_0'
   right='_Index_tuple_0'` (same name, different type) supports (b).

NEXT (architectural, the real last step): reconcile `_Index_tuple<0>` to ONE complete
DataDef across creation paths. Two concrete sub-investigations:
  (i) Why `is_complete=true`+`finalize()` on the opaque struct doesn't yield a complete
      c2mir struct — inspect CirBuilder's struct-decl emission for a 0-member completed
      struct (does it emit a body `{}` or a forward decl?). Likely the real lever: make
      a spec-less, concrete, empty-body variadic instantiation emit a complete `{}`.
  (ii) Trace the DataDefCLASS COPY constructor for "Index_tuple" (the untraced path) to
      find the second representation and dedupe it against the opaque cache key.
This needs dedicated design + full regression (completing empty-primary-with-spec
templates regresses `_Nth_type`/`tuple_element` — the guard is mandatory). bug A +
uneval + ref-collapse + bug B remain DONE; this single instantiation-identity bug is all
that blocks `map<int,int>`.

### 2026-06-20 (FINAL-3) — SHARPEST root-cause: `_Index_tuple<0>` is NEVER in struct_map at emission ⇒ no definition emitted ⇒ c2mir "incomplete". Only the empty-pack `_Index_tuple<>` is registered.

Used the existing CirBuilder Pass-0.5 emission trace (MADC_DEBUG_TUPLE, extended to
"Index_tuple", then reverted). At c2mir-tree emission, iterating `prog->struct_map`, the
ONLY `_Index_tuple*` key present is the empty-pack **`_Index_tuple`**:
`[EMIT] key='_Index_tuple' as_user=1 base=3(btClass) raw=255(dtRESERVED) ref=1(rtValue)
members=0 size=0 dep_ph=1`. **`_Index_tuple_0` is NOT a struct_map key at emission** —
so its `struct _Index_tuple_0 {}` definition is never emitted, yet `__o8`'s param
(`_Index_tuple<_Indexes1...>`→`_Index_tuple<0>`) and the `__o7` temp both reference it →
c2mir "incomplete struct or union" (×2) + the same-tag "struct-arg mismatch" (the
referenced-but-undefined `_Index_tuple_0` vs anything). (emit-c TEXT renders a `{}` for
it — the emit-c and c2mir-NODE paths diverge; trust the c2mir node path.)

So the precise gap: the `_Index_tuple<0>` DataDef referenced by `__o8`'s param is created
but NOT registered in `struct_map` (the empty-pack `_Index_tuple<>` IS). Most likely the
`__o8` param-type resolution inside `instantiate_fn_template_binding` resolves
`_Index_tuple<0>` via a path that does NOT go through `instantiate_opaque_template_use`'s
`struct_map[mangled]=fwd` (or it's created in a speculative/deferred context whose
struct_map insert is rolled back while the DataDef pointer escapes into the persistent
param type). `is_complete`/`finalize()` on the opaque-cached struct (FINAL-2) couldn't
help because that's a DIFFERENT object from the param's unregistered one.

NEXT (precise, the last map step): gdb-break the `DataDefSTRUCT`/`DataDefCLASS` ctor for
`n=="_Index_tuple_0"` on the CURRENT default build and read the creation stack — it will
show the `__o8` param-type resolution path. Ensure that path registers
`_Index_tuple<0>` in `struct_map` (and emits it as a complete empty `{}` struct, with the
mandatory "no partial spec" guard so `_Nth_type`/`tuple_element` are untouched), OR
canonicalizes the param type to the same DataDef the `_Build_index_tuple::__type()` temp
uses. This is an instantiation-REGISTRATION fix (architectural), needs full regression.
bug A + uneval + ref-collapse + bug B remain DONE.

### 2026-06-20 (FINAL-4) — rollback RULED OUT ⇒ `__o8`'s param references a SECOND, unregistered `_Index_tuple_0` DataDef (a copy). Confirmed by elimination.

`grep` confirms `struct_map` (and `datatype_map`) are NEVER erased/saved/restored/swapped
anywhere in parser.cpp — so the `_Index_tuple_0` registered by
`instantiate_opaque_template_use` (`struct_map[mangled]=fwd`) cannot disappear. Yet
emission (Pass 0.5 iterating `prog->struct_map`) shows NO `_Index_tuple_0` key. The only
consistent explanation: `__o8`'s param type references a DIFFERENT `_Index_tuple_0`
DataDef object than the registered one — a SECOND DataDef that was never put in struct_map.
The `DataDefSTRUCT` ctor trace fired exactly once for `_Index_tuple_0` (the registered
opaque one), so the second object is NOT made via a DataDefSTRUCT/CLASS ctor → it is a
**default DataDefCLASS COPY** produced while cloning the fn-template declarator/param types
during `instantiate_fn_template_binding` (the `__o8` indexed-ctor instantiation).

THE FIX (precise, last map step): when `instantiate_fn_template_binding` materializes the
instantiated function's PARAM types, a class/struct param type must REFERENCE the
registered `struct_map` DataDef (resolve the substituted `_Index_tuple<0>` spelling
through `instantiate_template_id`/`resolve_declared_type_token` so it hits the opaque
cache), NOT carry a copied DataDef. Trace: add a temp copy-ctor to DataDefCLASS (or break
on the param-type build in instantiate_fn_template_binding) to find the clone site; make
it reuse the registered type. With the param and the `_Build_index_tuple::__type()` temp
both pointing at the one registered (and completed-empty, FINAL-2 spec-guarded) DataDef,
map<int,int>'s last 3 c2mir errors clear. Architectural (identity), needs full regression.

### 2026-06-20 (FINAL-5) — DECISIVE: the bug is cir_builder PASS-ORDERING. `_Index_tuple_0` IS registered + IS emitted complete by Pass 1.97 — but AFTER the early protos that use it by value.

Traced the full path (MADC_DBG_IDXT in instantiate_opaque_template_use + CirBuilder Pass 0.5
and Pass 1.97, all reverted). Definitive facts:
- `_Index_tuple_0` IS created via instantiate_opaque_template_use and IS in `struct_map`
  (`struct_map.count("_Index_tuple_0")==1` at Pass 1.97). struct_map is NEVER erased.
- It is NOT present at Pass 0.5's struct sweep (early) — it's instantiated LATE, during the
  deferred-lazy-body reachability fixpoint (the `__o7`/`__o8` pair-ctor bodies).
- **Pass 1.97 (cir_builder.cpp:12808) DOES emit its complete definition** (`as_user=1`,
  not-yet-emitted → `class_struct_def` → empty `struct _Index_tuple_0 {}`; a 0-member class
  emits a COMPLETE empty body, not a forward decl).
- BUT `__o8`'s prototype is emitted in **Pass 0.75 (extern protos, ~12413-12533)** —
  FAR earlier than Pass 1.97 — with `_Index_tuple_0` as a BY-VALUE param (`__o8(...,
  struct _Index_tuple_0 p3, struct _Index_tuple p4)`; emit-c line ~918). So in the c2mir
  top-decl stream the order is: [proto with by-value `_Index_tuple_0` param] … [struct
  `_Index_tuple_0 {}` definition] … [bodies]. c2mir processes in order ⇒ the by-value
  incomplete-struct param in the early proto + the `__o7`→`__o8` call (whose arg/param
  struct types don't reconcile) → "incomplete struct or union" ×2 + "incompatible argument
  type for struct/union type parameter". The completion attempts (FINAL-2) couldn't help
  because the struct WAS already complete-at-definition — the def is just too LATE.

So the real fix is **pass-ordering**: a late-instantiated struct's DEFINITION must precede
the (early) function prototype that takes it by value. Candidate fixes (each needs design +
full regression):
  1. Front-insert the Pass-1.97 late struct defs at the HEAD of `top_list` (before all
     protos). Needs a c2mir prepend (fork: add `c2mir_op_prepend` mirroring
     `c2mir_op_append`) and must preserve deps-first order WITHIN the late batch (prepend
     the batch as a unit). Safe for the map index-tuples (leaves / self-contained
     _Tuple_impl+_Head_base cluster; their only "members" are pointers/refs, no by-value
     dep on an already-emitted EARLY struct). Verify no by-value early-struct dep before
     generalizing.
  2. Eagerly instantiate the index-sequence types (`_Build_index_tuple<N>::__type`,
     `_Index_tuple<...>`) BEFORE Pass 0.75 so they're in struct_map for Pass 0.5 — defeats
     laziness; narrower.
  3. When emitting a proto with a by-value struct param whose def isn't emitted yet, hoist
     emit_class_struct_with_deps for that param's class first (only works if the struct is
     already in struct_map at proto time — it is NOT for the late case, so this needs the
     deferred bodies parsed before Pass 0.75; = option 2).
Option 1 is the most general. bug A + uneval + ref-collapse + bug B remain DONE; this single
cir_builder ordering fix is all that blocks `map<int,int>`.

### 2026-06-20 (★ MILESTONE) — map<int,int> COMPILES (0 c2mir errors). W5 COMPILE half DONE. Runtime: 2 further layers found.

**COMPILE FIXED (commit 54ff562 + fork 824c7c86, MIR_COMMIT→824c7c8; fulltest 656/6/0/18
ZERO regr).** The last 3 c2mir errors were a CirBuilder PASS-ORDERING bug (all the
UPDATE-60..FINAL-5 "instantiation-identity"/completeness framings were wrong): `_Index_tuple<0>`
IS registered + emitted COMPLETE by Pass 1.97, but appended to top_list TAIL — AFTER the
Pass-0.75/1/1.95 fn prototypes that take it BY VALUE. c2mir reads top-decls in order → incomplete
by-value param in a proto before its def. FIX: anchor the last EARLY struct def (end of Pass 0.5);
Pass 1.97 emits late structs into a temp list + splices them in right after that anchor (after early
structs, before protos) via two new c2mir helpers `c2mir_op_tail`/`c2mir_op_splice_after`.

**RUNTIME (map<int,int> `m[1]=2` compiles to 0 c2mir errors but ABORTS at runtime; empty
`map<int,int> m;` compiles+RUNS clean).** Two further layers, BOTH downstream of compile:

1. **operator delete overload mis-resolution (latent; NOT the active blocker).** g++ (CANON)
   DEFINES `__cpp_sized_deallocation` by default in C++14+ (`g++ -std=c++17/20 -dM -E` confirms;
   clang++ does NOT). madc matches g++ (the predefined-macros generator captures it) — CORRECT,
   keep it. But madc then mis-resolves libstdc++ `__new_allocator::deallocate`'s sized
   `::operator delete(__p, __n*sizeof(_Tp))` (a `size_t` 2nd arg) to the ALIGNED overload
   `operator delete(void*, std::align_val_t)` (`_ZdlPvSt11align_val_t`) instead of the sized
   `operator delete(void*, size_t)` (`_ZdlPvm`). [Removing the macro "fixes" the symptom but
   DIVERGES from g++ canon → rejected as a shortcut.] Fix the overload ranking:
   `align_val_t` (an `enum class : size_t`) must NOT match a `size_t` arg better-or-equal than
   `size_t` itself. NOTE this is NOT the active crash — see (2).

2. **★ THE active runtime blocker: _Rb_tree node-pointer / tree-structure corruption.** Even with
   the plain `operator delete(__p)` path (macro removed), `m[1]=2` STILL aborts: munmap_chunk
   "invalid pointer" in `_M_drop_node` (mapii) / SIGSEGV @0x8 in `map::~map`→_Rb_tree teardown
   (mapread). So the freed/traversed NODE POINTER is wrong, independent of operator delete. The
   insert path (`operator[]`→`_M_emplace_hint_unique`→`_M_create_node`(`_Znwm`)+`_M_insert_node`
   →`_Rb_tree_insert_and_rebalance`) leaves the tree links corrupt → teardown derefs/frees garbage.
   Tied to the lingering c2mir WARNINGS: stl_tree.h:427 "incompatible argument type for pointer
   type parameter" (×many), stl_map.h:102:13 "incompatible pointer types of argument and
   parameter"/"...return-expr", node_handle.h:64 "incompatible return-expr type" — the broad
   reference-as-pointer / node-base-vs-node pointer coercions (the W4b / "Layer 10" family).
   JIT'd code (no source gdb; emit-c isn't gcc-compilable here due to conflicting-types artifacts),
   so debug by instrumenting the node alloc/insert/erase pointer flow (print node ptrs) or by
   chasing each "incompatible pointer types" warning to the wrong-typed argument. This is the final
   map<int,int> step (RUN half of W5).

### 2026-06-20 (RUNTIME root-caused via gdb) — the crash is the pair VALUE getting a reference-POINTER, not the int (W4b reference-lowering), NOT node alloc/free.

gdb on the REAL libstdc++ `_Rb_tree_insert_and_rebalance` (a .so symbol with debug info —
the JIT'd code calls it, so it's breakable even without madc source symbols) gave runtime
visibility. Findings for `map<int,int> m; m[1]=2;`:
- Insert/rebalance is STRUCTURALLY CORRECT: new node @0x…ddd830, rebalance sets
  color, _M_parent=header, _M_left=_M_right=0. The tree is a valid single node.
- The NODE VALUE is WRONG. Node layout (40B): [color@0, parent@8, left@16, right@24,
  pair value@32]. The value word @32 = `0x0000555555ddd860` — a POINTER (≈node+48), NOT
  `{key=1, val=2}` (which would be `0x0000000200000001`). It does NOT change after the
  `=2` assignment. So `m[1]=2` constructs the pair's `first` (const int) from the
  reference `const int&` returned by `std::get<0>(forward_as_tuple(1))` by storing the
  REFERENCE POINTER, not the dereferenced int. Writing an 8-byte pointer into the 8-byte
  `pair<const int,int>` value (and likely past the 40B node) corrupts the adjacent heap
  chunk header → teardown's `free(node)` aborts with munmap_chunk "invalid pointer"
  (and the mapread variant SIGSEGVs traversing the corrupted node).
- So this is NOT the node alloc/free chain and NOT the operator-delete overload — it is
  the **reference-as-pointer LOWERING** (W4b part b / task #5): the pair piecewise→indexed
  ctor's `first(std::forward<const int&>(std::get<_Indexes1>(__tuple1))...)` keeps the
  reference as a pointer where a VALUE init is required. (emit-c shows `first =
  *forward(&*get(t))` — a deref — but at runtime `first` holds the pointer, so the deref
  is a no-op / the get/forward chain yields the address, not the referent. The
  reference-collapse compile fix made `get` return `int*`; the VALUE consumer must deref
  it.)

NEXT (map RUN, final step): fix the reference→value lowering so initializing a non-reference
member/temp from a reference-typed (DataDefREF, pointer-modeled) source DEREFERENCES it.
This is the broad W4b reference-lowering work (task #5), now pinpointed to the pair
value-construction in map's operator[]. Reducer: tmp/mapii.mad (gdb break the real
_Rb_tree_insert_and_rebalance, `x/5gx` the node — value@+32 should be {1,2}). The COMPILE
half of W5 is DONE (committed 54ff562); this reference-deref is the RUN half.

### 2026-06-20 (RUNTIME — deepest root: get__o2 returns the `type` FALLBACK in the map context)

Emit-c (flag-on) smoking gun: in map's deferred pair-indexed-ctor body, the instantiated
`std::get<0>` is `type *__ns_std_get__o2(struct tuple_const_int32_t_ *)` — return type is the
UNRESOLVED `type` fallback (char), NOT `const int&` (→ `int*`). `__o8` does
`__this->first = *__ns_std_forward__o5(&*__ns_std_get__o2(__tuple1))` (forward__o5 is
`int*(int*)`). With get mis-typed `type*` and the chain mishandled, `first` (const int) ends
up holding the reference POINTER (gdb: node value@+32 = ~node+48, an address) instead of the
int 1 → heap overflow → teardown free aborts.

So the deepest cause is **tuple_element<0, tuple<const int&>>::type resolving to the `type`
fallback specifically in map's DEFERRED-body instantiation context** — the standalone
reducer `getref.mad` (`int x = std::get<0>(tuple<const int&>)`) resolves it correctly
(returns int*, runs x=7), but the map path's get__o2 (instantiated lazily inside the pair
indexed ctor during the CIR reachability fixpoint) loses that resolution and falls to
`type`. This is the W4b ref-element/tuple_element chain in a deferred context. FIX directions:
ensure the deferred instantiation of `std::get<__i>(tuple<_Elements...>&)`'s return alias
`__tuple_element_t<__i,tuple<_Elements...>>` resolves the same as the eager path (carry the
instantiation context so `tuple_element<0,tuple<const int&>>::type` → `const int&` → int*),
and ensure the reference→value init derefs. Reducer: build flag-on, `--emit=c11 tmp/mapii.mad`,
grep `__ns_std_get__o2` — its return must be `int*`, not `type*`. This is the final RUN-half
blocker; COMPILE half done (54ff562).

### 2026-06-20 (RUNTIME — corrected reducer; use PRINTF not exit codes)

CORRECTION (exit-code-based reducers misled me — use printf):
- `tmp/tv.mad`: `std::tuple<int> t(7); printf("%d", std::get<0>(t))` → **7. tuple<int> WORKS.**
- `tmp/te_direct.mad`: `std::tuple<const int&> t(k=7); …::type r = std::get<0>(t); printf("%d", r)`
  → **r=1 (WRONG, should be 7). The REFERENCE element is mishandled.**
So the bug is REFERENCE-ELEMENT specific (NOT general tuple, NOT deferred-context — both
earlier framings were wrong): `std::get<0>(std::tuple<const int&>)` returns garbage because
`tuple_element<0, std::tuple<const int&>>::type` resolves to the `type` fallback (get's
emitted return is `type *__ns_std_get__o2(...)`, vs `int*` for the working tuple<int> case).
This is exactly the W4b "tuple_element ::type for a reference element → `type` fallback"
wall (UPDATE 56–59), now with a CLEAN eager reducer:
  WALL  tmp/te_direct.mad  (tuple<const int&> → r=1)
  CTRL  tmp/tv.mad         (tuple<int>       → 7)
map's operator[] builds `forward_as_tuple(key)` = `tuple<const int&>`, so the pair's `first`
gets that garbage → heap corruption → teardown abort. FIX te_direct (tuple_element-of-
reference `::type` → `const int&`→int*, and the ref deref into the value) and map RUN follows.
Both flag-on (-DFEATURE_DERIVED_TO_BASE_DEDUCTION -DFEATURE_CONST_TYPES). COMPILE half of W5
remains DONE (54ff562); this reference-element resolution is the RUN half.

### 2026-06-20 (RUNTIME — MAXIMALLY ISOLATED to a 2-line reducer: typedef/alias-to-reference var init misses the address-bind)

Drilled the map runtime crash all the way down (printf-based reducers; exit codes mislead):
- `tmp/tv.mad`  `std::tuple<int> t(7); get<0>` → 7 (tuple VALUE works)
- `tmp/te_direct.mad` `std::tuple<const int&>` get → garbage (reference element)
- `tmp/nth.mad`  user `template<...> struct Nth<0,T0,R...>{typedef T0 type;}; Nth<0,const int&>::type r=k;`
  → SIGSEGV — reproduces with NO libstdc++/tuple/map.
- **`tmp/tdref.mad` (THE reducer, 2 lines, no templates):**
  `typedef const int& cref; int k=7; cref r = k; printf("%d", r);`
  emits `cref r = k; … (*r)` — i.e. value-assign `= k`, NOT reference-bind `= &k`.
  Control `const int& r = k;` (plain, `&`-token) correctly emits `int *r = (&k);` and runs.

ROOT (precise): a variable whose declared type is a TYPEDEF/ALIAS resolving to a reference
(`cref`, or a member `typedef T0 type` with T0 a reference, e.g. _Nth_type/tuple_element) is
recognized as a reference on USE (emits `*r`) but its INITIALIZER is NOT address-bound —
emits `r = k` instead of `r = &k`. So the reference-bind-at-init is keyed on the `&`
declarator TOKEN, not on the resolved TYPE's is_reference(). map's operator[] uses
`std::get<0>(forward_as_tuple(key))` whose result type is exactly such an alias-reference, so
pair::first binds the value (an address) wrong → heap corruption → teardown abort.

FIX (next session — the RUN half of W5, a tiny reducer to iterate on): in the variable
INITIALIZER lowering, address-bind when the var's TYPE is_reference() (DataDefREF), not only
when the declarator had an `&` token. Verify with tdref.mad (`= &k`, runs) → nth.mad →
te_direct.mad → mapii.mad, then full regression (656/6). This touches the reference-lowering
(W4b) so it's regression-gated; the 2-line reducer makes it fast + safe to land. COMPILE half
of W5 DONE (54ff562); this is the last RUN-half bug.

### 2026-06-20 (resume) — bug#1 patch VERIFIED flag-on (prior "breaks control" call was a flags-OFF artifact); bug#2 refined to reference-COLLAPSE in `_M_head`'s return (part-a collision)

Re-applied docs/plans/2026-06-20-varinit-reference-fix.patch and BUILT FLAG-ON
(`-DFEATURE_CONST_TYPES -DFEATURE_DERIVED_TO_BASE_DEDUCTION`; the patch is unconditional).
Walked the handoff's ladder flag-on:
- tdref → r=7, nth → r=7 (bug#1 FIXED, as documented)
- **tv (control) → get0=7** — the control STILL WORKS. (An earlier same-session conclusion
  that "the patch breaks the control" was measured on a flags-OFF build, where tv.mad ALREADY
  fails at HEAD because the get-chain needs the flags — INVALID per `reducers-need-flags`. The
  patch does NOT regress tv flag-on.)
- te_direct → r=1 (STILL WRONG) — bug#2, INDEPENDENT of the patch (flag-on te_direct is r=1
  with AND without it; the patch only touches tdref/nth, which have no get()).
bug#1 is sound; gating it on the DEFAULT full regression (unconditional change) before commit.

**bug#2 REFINED ROOT (hypothesis, source+clang grounded — VERIFY before fixing):** the libstdc++-13
chain (utility.h:234, tuple:1778/1782/1802, :176-240) for `std::get<0>(tuple<const int&>&)`:
`get` returns `__tuple_element_t<0,tuple<const int&>>&` = `tuple_element<…>::type &` =
`_Nth_type<0,const int&>::type &` = `const int& &` = **`const int&`** (DOUBLE collapse — the
element is itself a reference). Body → `_Tuple_impl<0,const int&>::_M_head(__t)` → `_Head_base<0,
const int&,false>::_M_head(__b){ return __b._M_head_impl; }` where `_Head _M_head_impl` is
`const int&` (madc: `int*` slot storing `&k`) and the return `_Head&`=`const int&` (collapse).
So `return __b._M_head_impl` must return the STORED POINTER `&k` AS-IS (collapsed reference) —
NOT deref to the int, NOT take its address. CLANG (canon, SemaType.cpp:2262 BuildReferenceType):
`LValueRef = SpelledAsLValue || T->getAs<LValueReferenceType>()` — building a ref over an
already-ref type is idempotent (single lvalue ref, never ref-to-ref). madc's getReferenceType
already collapses at the TYPE level (@7a95e79); the defect is VALUE LOWERING.
SUSPECT: W4b part-a (@db71765) unconditionally derefs a scalar reference MEMBER on read; that is
WRONG when the member is returned/bound AS the collapsed reference — it yields the int value
where the `int*` (collapsed ref) is expected ("returning integer without cast for pointer result"
— the exact warning te_direct emits). part-a must deref only in a VALUE context, not when the
reference itself is wanted (reference-return / reference-bind / collapsed-ref). NEXT (verify, then
fix): (1) discriminator — run te_ref (explicit `const int&`) vs te_direct (`::type`) vs
te_aliasref (`_t`) flag-on; if te_ref also =1 the bug is the ref-return value-path generally, if
only te_direct/te_aliasref =1 it implicates the `tuple_element::type` resolution too. (2) confirm
the warning is at `_M_head`'s return. (3) make the reference-member read context-aware (no deref
when the consumer wants the reference). Reducers tmp/te_ref|te_direct|te_aliasref|getref.mad,
flag-on, PRINTF (exit codes mislead).

### 2026-06-20 (resume cont.) — bug#2 RE-ROOT-CAUSED bottom-up via zero-libstdc++ reducers. NOT reference-collapse in `_M_head`. It is a 2+ LAYER template-resolution chain. Layer 1 (variadic concrete-ref ARG SPELLING) FIXED.

The "_M_head reference-collapse" hypothesis above was WRONG (getref_p `int x = std::get<0>(tuple<const int&>)` RUNS x=7 — get<0> returns the correct reference; the value-read deref already works). The real defect is UPSTREAM of get, in resolving the variable's declared type `tuple_element<0,tuple<const int&>>::type`, which falls to the `type` FALLBACK so the var's initializer is silently DROPPED (`type r;` no init → r=uninitialized garbage; confirmed at TREE level: k=42 → r=1, r does NOT track k). Discriminator (all flag-on, PRINTF):
- getref_p `int x = get<0>(t)` → **x=7** (value-init works; get is fine)
- te_direct `tuple_element<0,tuple<const int&>>::type r = get<0>(t)` → **r=1** (init dropped)
- te_lval  `tuple_element<…>::type r = k` (plain lvalue init, NO get) → **r=41** (init dropped — so NOT a get bug)
- tv `tuple<int>` get → 7;  ps1–ps6 (tuple<int> value element, `= 9`) → all 9

**ZERO-libstdc++ reduction** (the key): `template<typename...E> struct V{}; template<typename T> struct C; template<typename H> struct C<V<H>>{typedef int type;}; C<V<const int&>>::type r=5;`
- `C<V<const int&>>::type` → PARSE ERROR "Expecting identifier after type"  (tmp/vmatch.mad)
- `C<W<const int&>>::type` (W non-variadic) → WORKS r=5  (tmp/wmatch.mad)
- `C<V<int>>::type` (value element) → WORKS  (tmp/vval.mad)
- `C<V<const int&>>` alone, no `::type` → OK  (tmp/vnotype.mad)
So the trigger is exactly **variadic-inner-template + reference element + `::type`**. tmp/vref.mad,
vmatch.mad, wmatch.mad, vval.mad, vnotype.mad, wref.mad (gitignored).

**LAYER 1 — variadic concrete-ref ARG SPELLING (FIXED, commit pending regression).** Traced
`unify_nested_spec_pattern_arg`: the concrete spelling of the variadic `V<const int&>` was
`V<constint&>` (NO space — `carg[0]='constint&'`) vs the non-variadic `W<const int32_t&>`. ROOT:
`instantiate_opaque_template_use` (the variadic/pack path, parser.cpp ~3053) builds its arg
spelling via `collect_template_argument_spelling` → raw `template_token_fragment` concat with NO
spaces (`const`+`int`+`&`=`constint&`), bypassing `template_type_arg_spelling` (the real path,
which yields `const int32_t&`). `resolve_arg_spelling_datadef("constint&")` can't peel `const `
(no space) → NULL → H deduction fails → partial spec REJECTED → C falls to the incomplete PRIMARY
→ no `type` alias → `::type` chain (parseDeclaration 36917) fails → "Expecting identifier". FIX
(parser.cpp collect_template_argument_spelling ~2407): insert a space between two adjacent WORD
fragments (`const`+`int`→`const int`; `::`/`<`/`&`/`*`/`,` untouched). sanitize/despace normalize
it away for mangled keys + canonical compares (no key churn within a path). RESULT: vmatch→r=5,
vref→r=7 (were parse errors); te_lval's concrete is now `std::tuple<const int&>` (round-trips);
**map<int,int> now PARSES through to runtime** (was a parse-level failure). DEFAULT regression
running (b91ql0m4n) to gate the commit.

**LAYER 2 — `_Nth_type<0,const int&>` instantiates OPAQUE (no `type` alias). STILL OPEN — the map RUN blocker.** With Layer 1 in, te_lval's `tuple_element<0,tuple<const int&>>` DOES match its
spec and instantiate; `resolve_class_type_alias` trace:
```
_Nth_type_0_const_int_                    find type -> <MISS> (n_aliases=0)   <-- empty opaque shell
tuple_element_0_std__tuple_const_int__    find type -> _Nth_type_0_const_int___type   <-- placeholder
```
So `tuple_element<…>::type` = `_Nth_type<0,const int&>::type`, but `_Nth_type<0,const int&>`
instantiated as an EMPTY opaque placeholder (n_aliases=0) — its partial spec `_Nth_type<0,_Tp0,
_Rest...>{using type=_Tp0;}` body was never parsed → `::type` MISS → tuple_element's `type` stays
the unresolved placeholder → the `type` (char) fallback → var-init dropped. `_Nth_type` is variadic
(pack `_Rest`) + has a non-type leading param (`size_t _Np`), so it routes to
`instantiate_opaque_template_use` UNLESS `allow_variadic_real_inst`/`try_spec_real_inst` is set
(parser.cpp ~3360-3383). `resolve_typename_type_token` (4770-4774) DOES set the flag around the
head instantiate, so `typename _Nth_type<0,const int&>::type` SHOULD real-instantiate the spec —
but the trace shows it didn't (opaque, n_aliases=0). NEXT (flag-on instrumentation, blocked by the
running regression): trace `instantiate_opaque_template_use` entry for tname=`_Nth_type` printing
`allow_variadic_real_inst`/`variadic_real_inst_sticky`/`try_spec_real_inst` + whether
`match_partial_specialization` is reached, to learn WHY `_Nth_type<0,const int&>` bails opaque
despite resolve_typename_type_token's flag. Candidates: (a) tuple_element's `using type = typename
_Nth_type<__i,_Types...>::type` body is parsed via a path that does NOT go through
resolve_typename_type_token (so the flag isn't set when _Nth_type instantiates); (b) the spec
match for non-type `0` + empty-pack `_Rest` still bails to opaque; (c) the flag is cleared (3372)
before _Nth_type and not re-set. Reducer to isolate WITHOUT libstdc++: a user `template<size_t N,
typename...Ts> struct Nth; template<typename T0,typename...R> struct Nth<0,T0,R...>{using type=T0;};`
inside another template's `using type = typename Nth<I,Ts...>::type;` — does the NESTED-in-body
Nth real-instantiate? (nth.mad — Nth used DIRECTLY at top level — already works r=7; the gap is
NESTED in a `using type=` member-alias body.)

#### LAYER 2 ROOT CONFIRMED + a working-but-UNSAFE fix (reverted). Refinement needed.
ZERO-libstdc++ reducer: **tmp/nestnth2.mad** (`template<unsigned long N,typename...Ts> struct Nth;
template<typename T0,typename...R> struct Nth<0,T0,R...>{typedef T0 type;}; template<typename...Es>
struct Wrap2{}; template<typename H,typename...R> struct Wrap<Wrap2<H,R...>>{typedef typename
Nth<0,H,R...>::type type;}; Wrap<Wrap2<const int&>>::type r=k;`) → r=garbage (init dropped),
reproduces Layer 2 with no libstdc++.

ROOT (traced, gated MADC_DBG_BUG2, reverted): candidate (a) CONFIRMED — `_Nth_type<0,const int&>`
(and user `Nth<0,const int&>`) is resolved through `resolve_typename_type_token` but via the
**4785 fallback** (`resolve_declared_type_token`), NOT the class-scope loop (4768). The loop only
looks up class MEMBERS, so a NAMESPACE/global template (`_Nth_type`, user `Nth`) returns
loop_owner=NULL **with the `<` intact** (trace: `rtt head='Nth' css=1 loop_owner=NULL peek=tkLT`),
and the fallback resolves it with `allow_variadic_real_inst=0` → opaque shell, no `type`
(trace: `inst tname='Nth' vri=0 ... has_pack=1 try_spec=0` → opaque bail at parser.cpp ~3374).

WORKING-BUT-UNSAFE FIX (tested, then REVERTED — do NOT re-apply as-is): wrap the 4785 fallback
`resolve_declared_type_token(first,...)` with `allow_variadic_real_inst=true` when `first` is a
template-id head (`is_contextual_identifier_token(first) && peek==tkLT`). RESULT: **nestnth2 → r=7
(Layer 2 fixed for the user case)**, BUT it REGRESSED the real `<tuple>` parse: `tv` (tuple<int>),
te_lval, getref_p ALL began failing "Expecting type in using alias" at /usr/include/c++/13/tuple:1782
(the `using type = typename _Nth_type<__i,_Types...>::type` in tuple_element's DEFINITION). Cause:
the fix sets vri even when the template-id args are **DEPENDENT** (`_Nth_type<__i,_Types...>` during
the tuple_element *definition* parse, __i/_Types unbound) — vri makes `try_spec_real_inst` true
(parser.cpp 3365, partial_spec_map has `_Nth_type`), which apparently routes the dependent-args case
to NULL instead of the opaque placeholder (3711 `td.body.empty()||(pack_real_inst&&dependent_surface)`
should bail to placeholder, but with a spec swap (3640) td.body becomes non-empty → it proceeds and
fails on dependent subst). The fix is UNCONDITIONAL so it would break the DEFAULT build's `<tuple>` too.

REFINEMENT (the correct Layer 2 fix — two candidate approaches, pick after verifying):
  (1) GATE on concrete args: set vri at the 4785 fallback ONLY when the upcoming `<...>` args are
      NON-dependent (no token names an in-scope template parameter). Needs a "template-id args are
      concrete" predicate at the resolve site (the active class-template params aren't in a single
      handy set — FuncDef.template_param_names is per-fn; class-template def params need locating).
  (2) DEEPER (preferred, per fix-at-deepest-layer): make `instantiate_template_use` return the
      opaque placeholder for DEPENDENT args even when a partial spec "matched" — i.e. at ~3711 the
      `td.body.empty()` bail must also fire when `dependent_surface` is true regardless of the spec
      swap (a dependent-arg instantiation must NEVER real-instantiate a spec body). Then vri can be
      set unconditionally at the 4785 fallback (concrete → real-inst, dependent → placeholder), and
      the regression vanishes. Verify: nestnth2 r=7 AND tv/te_lval/getref_p parse clean AND default
      fulltest 656/6. This is the precise next step; reducers tmp/nestnth2.mad (must→7),
      tmp/tv.mad (must stay get0=7), tmp/te_lval.mad/te_direct.mad (the goal). NOTE even after Layer 2,
      Layer 3 (reference→value deref in pair construction) likely remains for map RUN.

  ATTEMPT 2 RESULT (tried + REVERTED — both changes together): resolve_typename 4785-fallback vri
  (unconditional) + a NARROW bail at parser.cpp ~3711 `|| (try_spec_real_inst && dependent_surface)`
  (so a dependent-arg instantiation reached via vri/try_spec stays a placeholder). This made it
  WORSE: nestnth2 began erroring "Expecting identifier after type" AND tv/te_lval still errored
  "Expecting type in using alias" @tuple:1782. LESSON: the dependent-arg `_Nth_type<__i,_Types...>`
  path (parsed for EVERY `#include <tuple>`, concrete or not — the tuple_element DEFINITION body)
  previously produced a usable DEPENDENT `::type` placeholder (tv worked, get0=7); routing it through
  vri → my new placeholder bail changed HOW the placeholder is created, and its subsequent `::type`
  resolution (`resolve_class_type_alias` MISS → `materialize_dependent_member_type`) then returns
  NULL → "Expecting type in using alias". So the real fix must (i) leave the DEPENDENT-args
  `_Nth_type<__i,_Types...>::type` placeholder path EXACTLY as today (do not reroute it), and
  (ii) make ONLY the CONCRETE-args `_Nth_type<0,const int&>` (during the tuple_element
  INSTANTIATION body re-parse) real-instantiate its spec. The discriminator (dependent_surface) is
  only known AFTER arg-parse inside instantiate_template_use; but resolve_typename's fallback sets
  vri BEFORE that. A correct fix likely belongs INSIDE instantiate_template_use: when args are
  CONCRETE and a partial spec matches and the spec has a body, real-instantiate it REGARDLESS of the
  vri flag (drop the flag dependence for the concrete-spec case), so the resolve-site flag plumbing
  is unnecessary and the dependent path is untouched. That is the recommended next attempt — but it
  overlaps the FINAL-2 "empty-primary-with-spec completion" work (which regressed when unguarded), so
  it needs the partial_spec_map guard + careful full regression. Tree left at fd106c7 (Layer 1
  committed; Layer 2 NOT attempted in the committed tree). Reducers in tmp/ (gitignored).

  ATTEMPT 3 RESULT (tried + REVERTED) — deferred the EARLY opaque bails (parser.cpp ~3374/3377) when
  `partial_spec_map.count(tname)` (NO resolve_typename change), so a concrete-arg spec'd template
  reaches the spec-match + real-inst and a dependent one bails at the ~3711 `dependent_surface`
  placeholder. RESULT: **nestnth2 → r=7** (Layer 2 fixed for the user case!), BUT tv/te_lval still
  errored "Expecting type in using alias" @tuple:1782. DISCRIMINATOR PINNED: nestnth2's `Nth<0,H,R...>`
  has a CONCRETE leading non-type (`0`); the real `_Nth_type<__i,_Types...>` (tuple_element DEFINITION
  body) has a DEPENDENT non-type (`__i`). Routing a DEPENDENT-NON-TYPE instantiation through the main
  path (instead of `instantiate_opaque_template_use`) breaks its `::type` placeholder resolution —
  `_Nth_type<__i,_Types...>`'s `::type` must stay the opaque dependent-member placeholder it is today.

  ⚠️ ARCHITECTURAL WALL (3 attempts, all the same root): the opaque-vs-real decision must keep
  DEPENDENT-arg instantiations on `instantiate_opaque_template_use` (their `::type` placeholder is
  load-bearing for the tuple_element DEFINITION parse) while routing only CONCRETE-arg instantiations
  to spec-real-inst — but ARG CONCRETENESS (`dependent_surface`) is only known AFTER the arg parse
  INSIDE `instantiate_template_use`, whereas the early opaque bails (3374/3377) and the resolve-site
  vri plumbing both act BEFORE that. Every attempt that moves the decision earlier (vri at the resolve
  fallback; deferring the early bail) drags dependent args onto the main path and breaks their `::type`
  placeholder. THE CORRECT FIX (next session, fresh context — a real design task, NOT a quick patch):
  make `instantiate_opaque_template_use` itself, AFTER it has parsed its args and found them CONCRETE
  (no `...`, all resolvable) AND a partial spec matches AND the spec has a body, real-instantiate that
  spec body in place (register the `using type` alias) — i.e. the placeholder path gains a
  "concrete-args ⇒ complete via matched spec" branch. This is the FINAL-2 "empty-primary-with-spec
  completion" idea but driven by a PARTIAL-SPEC MATCH (not blanket empty-body completion, which
  regressed), guarded so dependent/no-match uses stay placeholders. Verify: nestnth2 r=7, te_lval/
  te_direct r=7, tv get0=7, default fulltest 656/6, THEN Layer 3. This is per systematic-debugging
  Phase 4.5 (3 fixes failed ⇒ the approach, not the spot, is wrong) — do it as a scoped design pass.

  ATTEMPT 4 (diagnostic only, reverted) — re-applied resolve_typename 4785 vri + a trace at the
  ~3711 bail gated on `tname.find("Nth_type")`. The trace **did NOT fire** for the failing tv/te_lval
  (dependent `_Nth_type` during the tuple_element DEFINITION parse), yet the "Expecting type in using
  alias" error still appeared. ⇒ The dependent `_Nth_type<__i,_Types...>` resolution does NOT reach
  `instantiate_template_use`'s 3711 — so my control-flow model is WRONG about where/how it's
  instantiated (it is NOT the resolve_typename→fallback→instantiate_template_use path I assumed; it
  may go through `instantiate_opaque_template_use` directly, or a member-alias body path that never
  re-instantiates _Nth_type, or resolve_declared_type_token via a different branch). MANDATORY FIRST
  STEP next session BEFORE any fix: MAP THE ACTUAL PATH — add an entry trace to BOTH
  `instantiate_template_use` AND `instantiate_opaque_template_use` (print tname + a backtrace marker)
  and to resolve_typename_type_token / resolve_declared_type_token / the using-alias resolver
  (parser.cpp 21214), run tv.mad (the dependent, currently-WORKING-without-changes baseline) to learn
  exactly which function instantiates `_Nth_type` and resolves its `::type` to the dependent
  placeholder — THEN compare to te_lval (concrete). Only once the real path is mapped should a fix be
  attempted. The naive resolve-site vri changes perturb a path that isn't the one in play.

  ATTEMPT 5 (path-mapping diagnostic — the useful one). Added entry traces to
  instantiate_template_id / instantiate_template_use / instantiate_opaque_template_use (filtered to
  `Nth_type`/`tuple_element`). FINDINGS:
  - BASELINE (no code change), te_lval AND tv show the SAME single sequence:
    `tid tuple_element vri=1 → USE tuple_element vri=1 → tid _Nth_type vri=1 → tid _Nth_type vri=0 →
     USE _Nth_type vri=0 → OPAQUE _Nth_type vri=0`. So the real `_Nth_type` instantiation reaches
    `instantiate_template_use`/`_opaque` via the resolve_typename FALLBACK with **vri=0** (the
    class-scope loop's first `tid _Nth_type vri=1` returns NULL — _Nth_type is not a class member —
    so it falls through to the fallback which does NOT set vri). The first `tid _Nth_type vri=1` is
    the class-scope-loop probe (NULL); the `vri=0` one is the fallback that actually instantiates.
  - With ATTEMPT-1 re-applied (vri at the fallback) + the traces: **NO trace fires at all** and the
    error "Expecting type in using alias" @tuple:1782 appears. ⇒ attempt-1's vri breaks the `<tuple>`
    HEADER parse (tuple_element's `using type = typename _Nth_type<__i,_Types...>::type` is resolved
    EAGERLY at definition time with DEPENDENT __i/_Types; vri makes that dependent resolution fail),
    aborting before main — which is why no main-level instantiation traces fire.
  ⇒ CONFIRMED ROOT + WALL: the `_Nth_type<...>::type` resolution happens BOTH (i) eagerly at <tuple>
  definition parse with DEPENDENT args (must yield a dependent placeholder — today's vri=0 fallback
  does this correctly) AND (ii) at the CONCRETE instantiation (`_Nth_type<0,const int&>`, must
  real-instantiate to get `type`=const int&, but today's vri=0 fallback leaves it OPAQUE → the bug).
  Same code site (resolve_typename fallback), opposite required behavior, discriminated only by
  arg-concreteness — which isn't known at that site. THE FIX is NOT at the resolve site. It must be
  inside the instantiation engine: when `instantiate_opaque_template_use` (or the early-bail path in
  `instantiate_template_use`) has PARSED its args and finds them CONCRETE (no `...`, every type-slot
  resolves, every non-type-slot constant-folds) AND a partial spec matches AND the spec has a body,
  real-instantiate that spec body in place (register `using type`); else keep today's placeholder.
  This makes concreteness drive the decision AFTER arg-parse (where it's known) and needs NO
  resolve-site flag. It overlaps FINAL-2's "empty-primary-with-spec completion" (which regressed when
  done as BLANKET empty-body completion) — the difference is it must be PARTIAL-SPEC-MATCH-driven and
  concrete-args-gated. This is the precise, bounded next implementation; verify nestnth2 r=7,
  te_lval/te_direct r=7, tv get0=7, default fulltest 656/6, then Layer 3.

ATTEMPT 6 (the gate — IMPLEMENTED behind FEATURE_OPAQUE_SPEC_COMPLETE, then REVERTED) — added a
CONSERVATIVE non-mutating peek `peek_template_args_clearly_concrete()` (pgm.tokens[0]==`<`; returns
true ONLY if every arg is a literal / builtin ttDataType / registered type-template-namespace name
AND there is NO `...` — any unregistered identifier or `...` ⇒ false) and gated the two early opaque
bails (parser.cpp ~3374/3377) with `partial_spec_map.count(tname) && peek_..concrete()`. RESULT:
**nestnth2 → r=7**, BUT real `<tuple>` (tv/te_lval/getref_p) errors "Expecting type in using alias"
@tuple:1782. DEEPER WALL EXPOSED: admitting the CONCRETE `tuple_element<0,tuple<int>>` to real-inst
makes its BODY parse — `using type = typename _Nth_type<__i,_Types...>::type` — and THAT body
real-instantiation resolves to NULL. The opaque path "worked" only by NEVER parsing tuple_element's
body (get<0>'s value path didn't need ::type concrete for a non-ref element). ⇒ Layer 2 is NOT just
routing: REAL-INSTANTIATING the libstdc++ `tuple_element`→`_Nth_type` chained `using type = typename
Inner<...>::type` member-alias body is itself broken. nestnth2 works because it lacks the non-type
leading index on the OUTER level. REVISED FIX SCOPE: the gate (attempt 6) is correct & needed, but
must be paired with making that chained member-alias body real-instantiate. FIRST reduce the body
failure with a zero-libstdc++ TWO-LEVEL chain mirroring tuple_element→_Nth_type EXACTLY (non-type
leading index on BOTH levels + `using type = typename Inner<I,Elts...>::type` forwarding). Multi-step
engine task. Default baseline 656/6/0/18 never touched (all six attempts gated/reverted).

**LAYER 3 (anticipated, not yet reached) — reference→value lowering in pair construction.** Once
Layer 2 lands (te_direct → r=7), map's `pair::first(get<0>(forward_as_tuple(key)))` must DEREF the
returned reference into the `const int` value (the handoff's gdb finding: node value@+32 held the
reference POINTER, not the int → heap corruption → teardown munmap abort). Re-verify with mapii.mad
+ gdb on `_Rb_tree_insert_and_rebalance` after Layer 2.

### 2026-06-20 (handoff prep) — ★ REFRAME: at CURRENT HEAD the map VALUE/get path appears CORRECT in emitted C; the runtime crash is likely the SEPARATE `_Rb_tree` node-pointer COERCION (stl_tree.h:427), NOT the tuple_element/_Nth_type Layer-2/Layer-3 chain. MUST gdb-verify.
Examined `bin/madc --emit=c11 tmp/mapii.mad` (`map<int,int> m; m[1]=2;`) at HEAD f913abe (flag-on).
mapii COMPILES (warnings only, 0 c2mir errors) and aborts at RUNTIME in the map DTOR/teardown
(`munmap_chunk: invalid pointer`; the read-back reducer tmp/mapval.mad SIGSEGVs @0x8 in
`map..._dtor`). KEY NEW EVIDENCE — the pair-construction VALUE path in the emitted C looks RIGHT now
(post Layer-1 + @7a95e79), CONTRADICTING the older "first = a pointer" gdb framing (that gdb was an
EARLIER state):
- `struct pair_const_int32_t_int32_t { int first; int second; }` (first is an int value).
- `_Head_base..._M_head_impl` is `int *` (the reference `&k`); `_Head_base/_Tuple_impl::_M_head`,
  `__get_helper__o2`, all return `int *` (= `&k`, the collapsed reference). `forward__o5` is
  `int *(int *)`. So the indexed-ctor body `__this->first = *forward__o5(&*get__o2(__tuple1))`
  computes `*(&k)` = the int. (get__o2's DECLARED return is still the `type` fallback `type*`, but the
  underlying pointer is &k and forward is correctly int*, so the *value* deref yields the int — the
  `type*` is a cosmetic mis-annotation here, NOT a value corruption.)
⇒ So map's RUNTIME crash is probably NOT the pair value. The prime suspect is the **`_Rb_tree`
node-pointer coercion**: 10× `stl_tree.h:427:18 incompatible argument type for pointer type
parameter` + `stl_map.h:102 incompatible pointer types of argument/parameter` + `node_handle.h:64
incompatible return-expr type in function returning a pointer`. These are `_Rb_tree_node_base*` vs
`_Rb_tree_node<_Val>*` (derived node) coercions in the insert/rebalance/erase path — a wrong-typed
node pointer corrupts the tree links → teardown `_M_erase`/`_M_drop_node` frees/derefs garbage
(munmap / SIGSEGV@0x8). This is the **"Layer 10" node-pointer family** (task #2), a DIFFERENT and
likely MORE TRACTABLE track than the deep tuple_element/_Nth_type Layer-2 engine work.
**NEXT SESSION — REPRIORITIZE (do this BEFORE the Layer-2 engine project):**
  1. gdb-VERIFY at HEAD: break the real `_ZSt29_Rb_tree_insert_and_rebalance...` (it has debug info),
     `x/6gx` the new node — is value@+32 actually `{key=1}` (int) or a pointer? This settles whether
     the value path is truly fixed (emitted-C suggests yes) or still wrong.
  2. If value OK → chase the `stl_tree.h:427` node-pointer coercion: emit-c the insert/rebalance/
     `_M_insert_node`/`_M_get_insert_unique_pos` path, find where a `_Rb_tree_node_base*` is passed/
     returned where a `_Rb_tree_node<pair>*` (or vice-versa) is expected, and fix the node-pointer
     up/down-cast lowering (the derived-node `static_cast<_Link_type>` / `_S_left`/`_S_right` casts).
  3. Only if (1) shows the value is still a pointer → resume Layer 2 (tuple_element/_Nth_type body
     real-inst) per the attempts above.
Reducers: tmp/mapii.mad (target), tmp/mapval.mad (read-back, SIGSEGV@0x8 in dtor), tmp/mapempty.mad.
Flag-on -DFEATURE_CONST_TYPES -DFEATURE_DERIVED_TO_BASE_DEDUCTION; emit-c is OK for STRUCTURE here but
gdb-confirm runtime values (emit-c warned unreliable for some type judgments).

### 2026-06-20 (handoff prep, CORRECTION) — ★★ the crash is in the map DTOR even for an EMPTY map (zero inserts); it is NOT insert-specific, and it is PRE-EXISTING (not a regression).
CORRECTS the entry directly above. I claimed `tmp/mapempty.mad` (`map<int,int> m;` with NO insert)
"RUNS clean" — that is FALSE. mapempty.mad **SIGSEGVs at address 0x8** in
`map_..._dtor+0x43 [JIT]`, the SAME crash as the read-back reducer. Verified at BOTH HEAD f913abe
AND at a1d11d3 (before this session's Layer-1 spelling fix fd106c7) — so the empty-map dtor crash is
**pre-existing**, not introduced by any commit this session.
**Consequences for the next session (supersedes the "REPRIORITIZE" plan above):**
- The runtime crash is NOT in `m[1]=2` insert, NOT in the pair value, NOT in tuple_element/_Nth_type.
  It is in map **construction and/or destruction of an empty tree**. The pair-value/get path may well
  be correct (emitted-C evidence above still stands) but it is not what crashes first.
- SIGSEGV @0x8 = deref of a near-null pointer (struct field at offset 8 of a NULL base). In an empty
  `_Rb_tree`, the dtor does `_M_erase(_M_begin())` where `_M_begin() = _M_root()` should be NULL for
  an empty tree; if the header/sentinel node (`_Rb_tree_header::_M_header`) is mis-initialized (so
  `_M_root()`/`_M_node._M_parent` is garbage instead of 0), `_M_erase` recurses into `_S_left(garbage)`
  → load at offset 8 of garbage → SIGSEGV@0x8. So the prime suspect is **`_Rb_tree_header` /
  `_Rb_tree_node_base` initialization or the empty-tree `_M_erase` traversal**, i.e. whether the
  default-constructed map's header links (`_M_left=_M_right=&_M_header`, `_M_parent=0`,
  `_M_node_count=0`) are laid out and set correctly — likely tied to the same `_Rb_tree_node_base*` vs
  `_Rb_tree_node<_Val>*` node-pointer coercion family (stl_tree.h:427 warnings).
**NEXT SESSION — start here:** gdb `map<int,int> m;` (no insert), break at the JIT'd map ctor and dtor,
`x/6gx` the `_Rb_tree_header`/`_M_impl` after construction — confirm `_M_parent==0`,
`_M_left==_M_right==&_M_header`, `_M_node_count==0`. Find which field is wrong → trace to the
node-base init/layout. This is more fundamental than (and gates) the insert/pair-value work.

### 2026-06-20 (session 2) — ★★★ RUNTIME CRASH FULLY LOCALIZED: null `this` in the `_Rb_tree` DTOR tree; LOWERING IS CORRECT (gcc runs the emit-C); bug is in madc's cir_node TREE→c2mir→MIR (JIT) path, NOT lowering, NOT c2mir-codegen-of-correct-C.
Systematic-debugging pass on the empty-container dtor crash (task #7). Findings, each with a reducer:

**1. The crash is `_Rb_tree`-GENERAL and DTOR-side, even for an EMPTY container; pre-existing.**
   - `std::map<int,int> m;` (no insert) AND `std::set<int> s;` (no insert) BOTH SIGSEGV @0x8 in
     `*_dtor+0x43 [JIT]`. Reducers tmp/mapempty.mad, tmp/setempty.mad.
   - Reproduced at a1d11d3 (before this session's fixes) ⇒ pre-existing, not a regression.

**2. The object is CORRECTLY CONSTRUCTED — this is NOT a ctor/`_M_header`-layout bug.**
   - tmp/mapsize.mad: `m.size()==0`, `m.empty()==1`. tmp/mapbe.mad: `m.begin()==m.end()` → `1`
     (so `_M_reset` ran: `_M_parent==0`, `_M_left==_M_right==&_M_header`). The ctor is fine.
   - SUPERSEDES the prior "REPRIORITIZE"/`_Rb_tree_header` init hypothesis — the header IS reset.

**3. The crash is a NULL `this` (gdb ground truth).** `gdb -batch` on bin/madc + mapempty:
   faulting insn `mov (%rax),%rax` with `rax=0x8`; preceding `mov 0x80(%rsp),%rax; add $0x8,%rax`,
   and `[rsp+0x80]==0`. That is `((_Rb_tree*)NULL)->_M_impl._M_header._M_parent` (`_M_parent`@+8 of a
   flattened, EBO-collapsed `_Rb_tree_impl`; `_M_impl`@0, `_M_header`@0). The `_Rb_tree*`/`this`
   threaded into the dtor chain (`_dtor → _Rb_tree_dtor(&this->_M_t) → _M_erase(this,_M_begin(this)) →
   _M_begin → _M_mbegin → this->_M_impl._M_header._M_parent`) is **NULL**.

**4. heap `new`/`delete` WORKS; only STACK cleanup + member-dtor-chaining crash.**
   - tmp/mapdtor.mad: `new map<int,int>()` + `delete` → "ctor ok"/"dtor ok" (delete passes the
     pointer *value*; works). (Note `new T()` calloc-ZEROES, so even a no-op dtor is safe there.)
   - tmp/wrapmap.mad: `struct W{ map<int,int> m; ~W(){...} }` — inside `~W` `&m` is valid and
     `size==0`, but the next injected member-dtor `map_dtor(&w.m)` crashes null. Bare `map m;`
     (cleanup) crashes too. Both paths pass `&object`.

**5. NOT reproducible with equivalent user/simple-template code (all pass, correct `this`):**
   simple struct cleanup (tmp/cleanptr.mad), template-class cleanup (tmp/tmpldtor.mad), 2-level
   nested member-dtor (tmp/nestdtor.mad), base-class member access in dtor (tmp/ebodtor.mad),
   template-method-chain in dtor (tmp/tmplmethod.mad), and a hand-written **map-shaped** dtor chain
   with `=default` ctors `~Tree(){ erase(begin()); } begin()→mbegin()→this->impl.header.parent`
   (tmp/dtornull.mad). EVERY one threads `this` correctly and does NOT crash.

**6. ★ THE LOCALIZATION — lowering is CORRECT; bug is in the TREE/MIR path:**
   - `bin/madc --emit=c11 tmp/setempty.mad > tmp/setempty.c`; `gcc -w setempty.c -lstdc++` → runs
     clean, `size=0`, exit 0. So the C madc LOWERS to is correct (set_dtor/_Rb_tree_dtor/_M_begin/
     _M_mbegin all thread `this` correctly in the rendered text).
   - The SAME program under madc's JIT crashes (null this). ⇒ the defect is in madc's cir_node
     TREE → c2mir → MIR (JIT) path, which DIVERGES from the rendered emit-C text.
   - CONFIRMED divergence: feeding the emit-C TEXT back to standalone `/usr/local/bin/c2m` yields
     syntax/incomplete-type errors (line 605 struct, incomplete `_M_rep`, etc.) — so emit-C text is
     NOT a faithful serialization of the internal tree (rendered with fixups). The internal tree is
     what MIR compiles and what crashes; emit-C cannot be used to inspect it.
   - ⇒ NOT a lowering bug, NOT a c2mir-miscompiles-correct-C bug. It is CirBuilder building the
     `_Rb_tree` dtor invocation tree (the `this`/first-arg operand of the dtor or a nested method
     call in the chain) as NULL for HEADER-INSTANTIATED template dtors invoked via cleanup /
     member-dtor-chaining (but NOT via `delete`).

**NEXT SESSION — start here (concrete):**
  - Need a NO-EXECUTE tree dump: `--dump-nodes`/`--dump-cir`/`--dump-cir-checked` currently EXECUTE
    the program (so they abort on the crash before flushing). Add a dump-then-exit path (or fflush
    the dump before run), then DIFF the checked tree of the crashing `set`/`map` dtor invocation
    against the working hand-written tmp/dtornull.mad — look at the `this`/first-arg operand of the
    `_Rb_tree`-dtor call (and the cleanup-handler registration node) for a null/missing operand.
  - Suspects, in order: (a) the cleanup-attribute handler node for a header-instantiated template
    dtor passes a null/wrong object operand at the TREE level (delete works because it passes a
    pointer value, not an address-of node); (b) c2mir MUTATES the tree during compile (per KG
    IDE-modes note) and the mutation nulls the dtor `this` — compare --dump-cir vs
    --dump-cir-checked; (c) the member-dtor-chaining injection for a header-instantiated member type
    computes `&this->member` from a null base.
  - Reducers (flag-on -DFEATURE_CONST_TYPES -DFEATURE_DERIVED_TO_BASE_DEDUCTION, --std=c++20
    --no-embedded-headers): CRASH = tmp/{mapempty,setempty,wrapmap}.mad; WORK (controls) =
    tmp/{dtornull,tmplmethod,nestdtor,cleanptr,tmpldtor}.mad; gcc-OK = tmp/setempty.c.

**BONUS — separate real bug found (NOT the map crash, but genuine; document/fix independently):**
  A 3-level **purely-implicit** default-ctor chain fails to construct the innermost member.
  tmp/bisect.mad: `Outer1{Impl impl}`/`Outer2{Impl impl;~Outer2(){}}` (2-level) → `Impl()` runs,
  `c=0`; but `Outer3{Mid m}` / `Outer4{MidD m;~Outer4(){}}` (3-level, `Mid{Impl impl}`) → `Impl()`
  does NOT run, `c=garbage`. `= default` ctors at every level FIX it (tmp/defctor.mad → `c=0`), which
  is why real map (uses `=default`) does NOT hit this. So: madc synthesizes a class's implicit
  default ctor's member-init calls only when the class is "directly needed"; an INTERMEDIATE class
  reached only as a subobject doesn't get its member-ctor calls injected. Fix: make implicit
  default-ctor synthesis recursive through subobject classes. Reducer tmp/bisect.mad.

### 2026-06-20 (session 2 cont.) — ★★★ EMPTY-CONTAINER DTOR CRASH **FIXED** (the null-`this`): synth aggregate dtor called its member dtor with no forward proto → implicit-variadic K&R call → ABI mismatch.
Continued from the localization above (lowering correct, bug in tree→MIR). Used a new
`MADC_DUMP_MIR=1` hook (MIR_output of the textual MIR before run; added to madc_cir.cpp) +
gdb disasm to pin it:
- gdb: the dtor reads `this` from **rcx** (SysV 4th arg reg) while the caller passes it in
  **rdi** (1st) — `rdi`=valid `&obj`, `rcx`=0 → null `this` → deref `_M_parent`@+8 = SIGSEGV@0x8.
- MIR dump: `set_dtor` calls `_Rb_tree_dtor` via `call proto0, _Rb_tree_dtor, i_0, U_1`, and
  `proto0: proto i32, ...` — the **implicit-int variadic** prototype (the undeclared-function
  fallback). The void `_Rb_tree_dtor(this)` got the implicit variadic ABI → `this` mis-placed.
ROOT CAUSE (ordering): the member-dtor-chaining call in `CirBuilder::class_member_destruct`
(non-external branch) only did `referenced_funcs.insert(sym)` — NO typed forward prototype. And
Pass 0.75 (typed protos for referenced funcs) runs BEFORE Pass 1.6 (synth aggregate dtor defs),
which is when the synth dtor first *references* the member dtor — so the member dtor (an
instantiated `_Rb_tree` dtor) never got a forward proto before the `set_dtor` definition that
calls it → c2mir parsed the call as implicit-int variadic. This is the documented
"implicit-int K&R call → struct args mis-wire" failure mode (cir_builder.cpp ~12304).
FIX (cir_builder.cpp, UNCONDITIONAL — not flag-gated):
  1. `class_member_destruct` non-external branch now emits a typed `void d(struct M *)` forward
     proto via `need_output_extern(sym, false, {{{}, true, mc}})` (matches the in-module def, so
     no conflicting-types).
  2. `need_output_extern` honors `ptr` when `cls` is set → emits `struct X *` (was: cls forced
     by-value). Safe: every existing cls caller passes ptr=false (native_param_shape).
  3. Pass 1.6 flushes newly-added `m_output_externs` protos to top_list BEFORE each synth dtor
     definition (they're added after the Pass-0.8 flush, so otherwise emitted too late).
Also: `MADC_DUMP_MIR` debug hook (madc_cir.cpp) + `cir_dump_nodes` cycle/depth guard.
VERIFIED (flag-on): tmp/{setempty,mapempty,wrapmap,mapsize,mapbe}.mad all run CLEAN (were
SIGSEGV@0x8). `map<int,int> m; m[1]=2;` (tmp/mapii.mad) now COMPILES past the dtor and fails only
at the SEPARATE insert/pair layer: `stl_map.h:102 too many arguments` + `incompatible argument
type for pointer type parameter` — the piecewise-pair/tuple_element W2/W4 wall (the Layer-2 work
documented above), NOT the dtor. Default fulltest regression: <pending>.
**NEXT for map RUN:** the insert path — `operator[]` piecewise pair construction at stl_map.h:102
("too many arguments" = wrong instantiation arity in the pair/tuple_element/get chain). That is the
W2/W4/Layer-2 track already mapped in the earlier entries.

### 2026-06-20 (session 2 cont.) — INSERT next-step precisely located (after the dtor fix). `m[1]=2` fails at the piecewise-pair ctor call.
With the dtor crash fixed, `tmp/mapii.mad` (`map<int,int> m; m[1]=2;`) compiles past the dtor and
fails with 2 c2mir check errors at `stl_map.h:102` ("too many arguments" — PRE-EXISTING, also
present at b690ead, NOT caused by the dtor fix). Located in the emitted C (tmp/mapii.c):
- `__ns_std_construct_at__o2` (line ~1796) calls:
  `pair_const_int32_t_int32_t__pair_const_int32_t_int32_t(location, *fwd(piecewise),
   *fwd(tuple_const_int32_t_), *fwd(tuple))` — 4 args.
- The DEFINED pair piecewise ctor (line ~1254) is
  `pair_const_int32_t_int32_t__pair_const_int32_t_int32_t__o7(pair*, piecewise_construct_t,
   struct tuple_int32_t_ __first, struct tuple __second)`.
TWO mismatches (both the W2/W4/Layer-2 piecewise-pair + tuple<const int&> track):
  (a) the CALL targets `...__pair...` (no `__o7` overload suffix) while the DEF is `...__o7` → the
      call resolves to an UNDECLARED symbol → implicit-int proto → "too many arguments". The
      ctor-call symbol generation must match the instantiated ctor's emit symbol (`__o7`).
  (b) the def's `__first` param is `tuple_int32_t_` (tuple<int>) but the call passes
      `tuple_const_int32_t_` (tuple<const int&>) — the const-ref element deduction
      (tuple_element/_Nth_type Layer-2) still mis-instantiates the first tuple as tuple<int>.
NEXT for map RUN: fix the piecewise-pair ctor call-symbol match (a) and the tuple<const int&>
first-arg instantiation (b). This is the existing W2/W4 track (UPDATE 53-65), now isolated to the
construct_at→pair-ctor call.

### 2026-06-20 (session 2 cont.) — INSERT root-cause REFINED: construct_at resolves the pair ctor to the COPY ctor, not the piecewise ctor.
Sharper than the entry above. The three pair ctor symbols in tmp/mapii.c:
- `pair..._pair...(pair *p0, pair *p1)` — the COPY ctor (2 params, bare name, line ~981).
- `pair..._pair...__o7(pair*, piecewise_construct_t, tuple<int> __first, tuple __second)` — the
  PIECEWISE ctor (line ~1254).
- `pair..._pair...__o8(pair*, tuple<int>*, tuple*, _Index_tuple_0, _Index_tuple)` — the indexed
  delegating ctor (line ~1253).
`__ns_std_construct_at__o2` (line ~1796) emits `pair..._pair...(location, *fwd(piecewise),
*fwd(tuple<const int&>), *fwd(tuple<>))` — i.e. it calls the **bare COPY-ctor symbol with 4
arguments** → c2mir "too many arguments" (4 vs the copy ctor's 2). So `std::construct_at`'s
`::new(p) pair(forward<Args>(args)...)` did NOT resolve to the piecewise ctor (`__o7`); the ctor
overload-resolution in the instantiated construct_at body picked (or defaulted to) the copy ctor's
bare symbol. NEXT (insert): make the placement-new/ctor call inside an instantiated std::construct_at
resolve `pair(piecewise_construct_t, tuple, tuple)` to the piecewise ctor `__o7` (overload by the
forwarded arg types), AND instantiate `__o7`'s first tuple param as tuple<const int&> not tuple<int>.
This is the core W2/W4 piecewise-pair work.

### 2026-06-20 (session 2 cont.) — INSERT COMPILE root-caused (const-ref pack element has EMPTY canonical); a BROAD fix WORKS for map but REGRESSES the baseline (11 timeouts) → REVERTED. Narrow fix needed.
ROOT CAUSE of the stl_map.h:102 "too many arguments" (the pair piecewise ctor mismatch): the pair
piecewise ctor's `_Args1` pack was DEDUCED correctly as `const int32_t&` (verified via a
MADC_DBG_TIDPACK trace), but when that captured spelling was re-resolved to a DataDef via
`resolve_arg_spelling_datadef('const int32_t&')`, the result (getReferenceType(getConstType(int)),
a CACHED DataDef) had an **EMPTY `canonical_cpp_spelling`**. The downstream re-instantiation that
renders this pack element into the ctor's `tuple<_Args1...>` param then loses the const/ref and
collapses `const int&` → `int`, so the instantiated piecewise ctor `__o7` takes `tuple<int>` not
`tuple<const int&>` → the 3-arg piecewise call doesn't match it → overload resolution falls back to
the copy ctor → "too many arguments".
ATTEMPTED FIX (parser.cpp resolve_arg_spelling_datadef): after re-applying the peeled const +
pointer/ref suffixes, set the result DataDef's `canonical_cpp_spelling` to the reconstructed
spelling (`const <core>&`). VERIFIED IT WORKS FOR MAP: `map<int,int> m; m[1]=2;` then COMPILES +
RUNS; insert+read+update execute (tmp/maprun.mad `m[1]=99 m[5]=50`). **BUT it REGRESSED the default
fulltest: 645/6/11/18 (was 656/6/0/18) — 11 TIMEOUTS** (testforeachref, testvectorptr, testinclude,
testretbufmethodinit, …). Cause: it MUTATES the SHARED CACHED const/ref/ptr DataDefs'
canonical_cpp_spelling globally, which changes type spelling everywhere and sends vector<T*>/ref
instantiation into infinite loops. **REVERTED** (parser.cpp back to HEAD 7d9927d; baseline restored,
those tests pass again).
NARROW FIX DIRECTION (next): do NOT mutate the shared cache. Fix the DOWNSTREAM rendering — when a
const/ref/ptr DataDef with an empty canonical is rendered into a template-id arg spelling (the
tuple<_Args1...> substitution in instantiate_fn_template_binding), reconstruct `const base&` from
the DataDef's base+qualifiers locally instead of falling back to rawtype `int`. Locate that
substitution site and fix it there, or carry the captured pack SPELLING ('const int32_t&') through
to the substitution rather than round-tripping spelling→DataDef→spelling.
ALSO FOUND (separate, surfaces once compile works): the `_Rb_tree` RUNTIME semantics are buggy —
tmp/mapuniq.mad: `m[1]=10` then read `m[1]=0` (value lost) while `m[2]=20` reads OK, and `m[1]=99`
INSERTS A DUPLICATE (size 2→3 same key). operator[]'s find/lower_bound + returned value-reference
`(*__i).second` are unstable (W4b reference-lowering + stl_tree.h:427 node-pointer-coercion). W2/W3.

### 2026-06-20 (Codex handoff) — local narrow pack-spelling fix applied; empty-class layout fixed; remaining failure is MIR/runtime, not lowered C

Branch/state: `wip/tuple-instantiation-claude` at `2bd68f6`. Uncommitted tracked edits:
`src/parser.cpp`, `include/datadef.h`, `src/cir_builder.cpp`. Pre-existing untracked
`mir-debug-support.md` is still not ours. MIR fork is `/workspace/mir` at `824c7c86`;
`MIR_COMMIT` in madc is `824c7c8`.

What changed locally:
- Replaced the broad `resolve_arg_spelling_datadef()` cache mutation with a narrow
  pack-spelling carry. `try_instantiate_namespace_fn_template()` now records
  `tid_pack_spellings`, and `instantiate_fn_template_binding()` uses the captured
  source spelling for one-element template-id packs when injecting `TokenDataType`.
  This preserves `const int&` for `tuple<_Args1...>` without mutating shared
  cached `DataDef` canonical spelling.
- Fixed empty C++ class storage/layout at the type/layout layer. Complete empty
  classes now have size 1 as objects/members; empty bases still use EBO. `addMember()`
  accounts for empty class member storage, and `CirBuilder::object_class_words()`
  avoids rendering a complete size-1 empty class as an opaque long-word object.
  This makes emitted `_Rb_tree_impl` match GCC's shape: the empty comparator occupies
  a byte plus padding, and `_M_header` is at offset 8, not offset 0.

Validation run with `OPTIONAL_CPPFLAGS="-DFEATURE_CONST_TYPES -DFEATURE_DERIVED_TO_BASE_DEDUCTION"`:
- Build succeeded.
- `tmp/mapempty.mad` now runs: `empty ok`.
- `tmp/mapii.mad` compiles/runs with warnings only.
- `tmp/maprun.mad` still has wrong semantics under madc/MIR:
  `m[1]=99 m[5]=50 size=3` (GCC/clang oracle is size 2).
- `tmp/mapuniq.mad` still shows the duplicate/update bug:
  `two distinct: size=2 m[1]=0 m[2]=20`, then `after update m[1]: size=3 m[1]=99`.
- Controls still pass: `tmp/tv.mad -> get0=7`, `tmp/tdref.mad -> r=7`.

Important diagnostic result: the exact emitted C for `tmp/maprun.mad` compiled and
ran correctly under GCC:
`gcc -w -x c -std=gnu11 -c tmp/maprun_current.c -o tmp/maprun_current.o`;
`g++ tmp/maprun_current.o lib/libmadc.a /workspace/mir/libmir.a -ldl -lz -lm -lpthread -o tmp/maprun_current_gcc`;
`tmp/maprun_current_gcc -> m[1]=99 m[5]=50 size=2`.
So the current wrong `size=3` behavior is NOT in the lowered C text. The remaining
gap is in the internal tree -> c2mir/MIR execution path.

Current hypothesis for the next session: focus on the `_Rb_tree` insert/search
runtime path, not the pair/tuple compile path. The emitted algorithms are structurally
close to libstdc++ and native GCC runs them correctly, while madc/MIR does not. The
remaining warnings are the strong lead:
`bits/stl_tree.h:427 incompatible argument type for pointer type parameter`,
`bits/stl_map.h:102 incompatible pointer types`, and `node_handle.h:64 incompatible
return-expr type`. Re-check `_M_get_insert_unique_pos`, `_M_insert_node`,
`_S_key`, `_S_left/_S_right`, and the `_Rb_tree_insert_and_rebalance` call in the
internal c2mir/MIR path. A quick bool-call suspicion was tested only partially:
internal c2m `_Bool` calls passed; the external-call test was not completed.

Full regression status: `make -C src fulltest` was NOT rerun after these uncommitted
changes. The last committed baseline remains the prior `656/6/0/18`.

### 2026-06-21 (Codex rehydration/fixup) — prior handoff recovered; regressions fixed; fulltest back to known 5 reds

Branch/state: `wip/tuple-instantiation-claude` at `2bd68f6`, upstream synced. The tree
is intentionally still dirty with the local map work in `src/parser.cpp`,
`include/datadef.h`, `src/cir_builder.cpp`, status/docs mirrors, and new
`tests/teststdmapint.{mad,flags,expect}`. `mir-debug-support.md` remains pre-existing
untracked work and is not ours.

Rehydration findings:
- Git history since `731d0eb` is mostly plan/root-cause notes; the last source commit is
  `f2a0f27` (typed forward prototypes for empty map/set member dtors). The prior session's
  actual code edits were uncommitted.
- The previous handoff text had been appended here and partly mirrored into
  `claude_status.json`, but the KG lookup by `id` missed it because the KG node exists by
  `name=session_2026_06_20_codex_map_runtime_handoff` with nil `id`.
- The user-provided later transcript was newer than that handoff: fulltest had found a
  `testforeach2` regression after the handoff text claimed fulltest had not been rerun.

Regression fixes applied on top of the dirty local work:
- `testforeach2` failed with MIR undefined import
  `_ZSt8_DestroyIPNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEES5_EvT_S7_RSaIT0_E`.
  Root cause: the dirty `std_free_function_instantiation()` path externalized body-bearing
  `void` libstdc++ function templates. Those are header bodies madc must instantiate locally,
  not exported libstdc++ symbols. Fix: when `fn_template_map` has a retained body and the
  selected template returns `void`, return NULL from the mangled-direct path so the existing
  local overload-instantiation/ranking path emits the body. This also fixed the true
  runner-order C++20 `teststdmapint` failure on `std::destroy_at`.
- `testtuple` failed with c2mir repeated declaration `_M_head_impl`. Root cause: flattened
  C++ base/member emission can put inherited base members and own members with the same C++
  name into one C struct. Fix: class struct emission now synthesizes unique C field names for
  duplicate flattened members, preferring an own member's source name when it hides inherited
  fields. Layout offsets and DataDefs are unchanged.

Focused validation after fixes:
- `bin/madc tests/testforeach2.mad` -> prints Alice/Bob/Charlie and lambda greetings; warning
  at line 21 remains.
- `bin/madc tests/testfstream.mad --std=c++17 --no-embedded-headers` -> expected file IO,
  `to_string: 42`, `stoi: 12345`, `strlen: 5`.
- `bin/madc tests/testloop.mad` -> `line1`, `line2`, `line3`, `lines: 3`.
- `bin/madc tests/testtuple.mad` -> `tuple ok`.
- `bin/madc --std=c++20 --no-embedded-headers tests/teststdmapint.mad` -> expected:
  `two distinct: size=2 m[1]=10 m[2]=20`, `after update: size=2 m[1]=99`.

Full validation:
- `timeout 1800 make -C src fulltest 'OPTIONAL_CPPFLAGS=-DFEATURE_CONST_TYPES -DFEATURE_DERIVED_TO_BASE_DEDUCTION'`
  -> `658 passed, 5 failed, 0 timed out, 18 skipped`.
- The 5 failures are the known branch reds:
  `testcontainerdtor`, `testmadc_ns`, `testmap`, `testset`, `testsubscript`.
- `testforeach2`, `teststdmapint`, and `testtuple` did not fail in the final fulltest.
- Follow-up rehydration sanity pass found the local binary had been left with
  `MADC_DEBUG_CTORINIT`; after `make -C src clean` and a normal feature-flag rebuild,
  focused checks stayed green. One fulltest run exposed `testmathh` as a passing test
  close to the default 5-second runner cap (`657 passed, 4 failed, 2 timed out,
  18 skipped`, with `testmathh` the only unexpected timeout). Added
  `tests/testmathh.timeout` and reran fulltest: `658 passed, 5 failed, 0 timed out,
  18 skipped`, with only the known branch reds above.

Remaining open work:
- The real-header `std::map<int,int>` canary now passes, but the broader existing branch reds
  still cover the map/set/container destructor surfaces.
- Pointer-type diagnostics remain in the map path:
  `bits/stl_tree.h:427`, `bits/stl_map.h:102`, and `bits/node_handle.h:64`.
- A proper future cleanup should capture function explicit-instantiation declarations
  (`extern template ... f(...)`) as data. That would let mangled-direct selection use the real
  libstdc++ export signal instead of the current conservative "retained body + void => local"
  rule, while preserving the externally-instantiated `std::getline<char>` path.

### 2026-06-21 Codex default-build promotion

The remaining feature-gate blockers were not in the parser-specific map path. The
flag-on build still broke string/stream focused tests because external libstdc++
method declarations erased typed pointer returns to `void *` and treated scalar
reference parameters as ordinary pointer parameters. Concrete failure:
`basic_string::_M_data()` emitted as `void *` and `_M_create(size_type&, size_type)`
received `__capacity` instead of `&__capacity`, producing the
`basic_string.h:87` c2mir incomplete-pointer error in `testforeach2`,
`testfstream`, and `testloop`.

Fixes applied:
- `CirBuilder::need_output_extern()` can now receive a concrete return `DataDef`
  and renders its pointer/reference stars, so `_M_data()` emits as `char *`
  instead of `void *`.
- `emit_symbol_method_call()` now routes scalar reference params through
  `ref_param_arg_addr()`, so `_M_create(size_type&, size_type)` receives an
  address.
- Parser gates `FEATURE_CONST_TYPES` and `FEATURE_DERIVED_TO_BASE_DEDUCTION`
  were removed. Const/reference template-argument spelling and
  [temp.deduct.call]/4 derived-to-base nested-template deduction are default
  behavior now.

Default-build focused validation:
- `bin/madc --std=c++20 --no-embedded-headers tests/teststdmapint.mad` -> expected
  two-line map insert/update output.
- `bin/madc tests/testtuple.mad` -> `tuple ok`.
- `bin/madc tests/testforeach2.mad` -> expected names/greetings; existing warning
  at line 21 remains.
- `bin/madc --std=c++17 --no-embedded-headers tests/testfstream.mad` -> expected
  file/string output.
- `bin/madc --std=c++17 --no-embedded-headers tests/testloop.mad` -> `lines: 3`.
- `bin/test_libmadc_program --test-case="madc C API can compile and call scalar and string results"`
  -> 1 passed.

Default-build full validation:
- `make -C src fulltest` -> `658 passed, 5 failed, 0 timed out, 18 skipped`.
- The 5 failures remain the known branch reds:
  `testcontainerdtor`, `testmadc_ns`, `testmap`, `testset`, `testsubscript`.

Remaining open work is unchanged: retire the broader map/set/container branch reds
and clean up the non-fatal libstdc++ pointer-type diagnostics in the map path.

### 2026-06-21 Codex handoff — `std::forward` fixed; scoped alias blocker remains

Branch/state: `wip/tuple-instantiation-claude` at `2bd68f6`, with the expected
dirty local map/tuple work. The debug-only instrumentation added during the
investigation was removed before handoff. No parser hardcoding for
`basic_string` was found or added; the new fixes are in generic namespace
overload, const-qualified type, and CIR rendering paths.

Fixes applied after the default-build promotion:
- Namespace function overload records now retain explicit template-argument
  spellings, and namespace overload lookup filters by those spellings. This
  prevents memoized calls such as `std::forward<T>` from reusing the wrong
  specialization.
- `DataDefCONST` now forwards structural predicates to its base type, and CIR
  type recovery unwraps const-qualified class/struct DataDefs before class-tag
  rendering and extern signature rendering.
- `std::forward<const ...>` now emits matching `struct basic_string... *`
  prototypes/definitions instead of the previous mismatched type path.

Validation/state:
- `make -C src` passed after the final debug-instrumentation cleanup and
  handoff/status edits.
- `bin/madc --std=c++20 --no-embedded-headers --emit=c11 tests/testmap.mad`
  succeeds.
- `bin/madc --std=c++20 --no-embedded-headers tests/testmap.mad` still fails
  in c2mir with the known `stl_map.h:102` lvalue/return warning cluster after
  repeated `stl_tree.h:427` pointer-parameter warnings.
- Fulltest was not rerun after this final cleanup/handoff pass; the last full
  validation for this branch remains the default-build `658 passed, 5 failed,
  0 timed out, 18 skipped` run described above.

Active blocker:
- `std::get` return alias preservation is still wrong in the real map path.
  The emitted C has a concrete helper returning `struct basic_string... *`, but
  `std::get__o2` keeps `return_typedef_name = "type"`, colliding with an
  unrelated global `typedef char type;`, so CIR emits `type *` / `char *`.
- Strong root-cause lead: `parseDeclaration` preserves `decl_typedef_alias`
  when `user_typedef_names.count(tb->str)` is true. For class-scope aliases
  named `type`, spelling alone is insufficient; it can match an unrelated
  global typedef. Replace these raw alias-spelling checks with a scoped helper
  that verifies the alias resolves to the same `DataDef` at the correct scope.
  Start at the return-declaration path around the raw `tb->str == "type"` /
  `user_typedef_names.count(tb->str)` preservation site, then sweep the other
  raw `user_typedef_names` alias-preservation checks.

Next validation after the alias fix: rebuild with `make -C src`, rerun
`--emit=c11 tests/testmap.mad`, rerun `tests/testmap.mad`, then rerun the
focused canaries (`teststdmapint`, `testtuple`, `testforeach2`, `testfstream`,
`testloop`) before another capped `make -C src fulltest`.

### 2026-06-22 Codex handoff — C++17 map target; alias wall moved forward

Course correction: map bring-up is C++17-first. Do not lean on C++20
`std::map::contains` for this branch. `tests/testmap.mad` now checks membership
with `find(key) != end()`, and both map canaries use
`--std=c++17 --no-embedded-headers`.

What changed since the previous handoff:
- Added `Program::typedef_alias_matches_datadef(alias, dd)` and replaced the
  raw alias-preservation checks that could keep a same-spelled unrelated typedef.
  This fixes the prior class-scope `type` vs global `typedef char type` wall
  generically, by identity rather than spelling.
- `instantiate_opaque_template_use` now detects when concrete arguments select a
  body-bearing partial specialization and replays through the real instantiation
  path. This completes the `_Nth_type<0, ...>` / nested alias cases that were
  previously left as opaque shells.
- The non-type argument probe in that helper is deliberately lexical (`tkInt`
  only) before constant folding so dependent names such as `__i` in headers do
  not get speculatively evaluated while still dependent.

Focused reducer state:
- `tmp/nestnth2.mad` -> `r=7`.
- `tmp/twolevel_nth.mad` -> `r=7`; GCC and clang accept the reducer.
- `tmp/twolevel_nth_using.mad` -> `r=7`; GCC and clang accept the reducer.
- `tmp/tv.mad` -> `get0=7`.
- `tmp/te_lval.mad` -> `r=7`.
- `tmp/te_direct.mad` still has a separate direct `std::get<0>(t)` declaration
  initializer issue: emitted C assigns `int *r = (&__ns_std_get);` instead of
  calling the selected suffixed overload.

Focused test state:
- `make -C src` passed.
- `timeout 180 bin/madc tests/teststdmapint.mad` passed under its C++17 flags,
  with the expected size/update output and existing non-fatal `stl_tree.h:427`
  pointer warnings.
- `timeout 180 bin/madc tests/testmap.mad` now gets past the old
  `_Nth_type`/`std::get` alias blocker and fails later:
  `MIR error: import of undefined item
  basic_string_char_std__char_traits_char__std__allocator_char___basic_string_char_std__char_traits_char__std__allocator_char___o15`.

Current root-cause lead:
- The emitted C for `testmap` has a correct concrete `std::get` helper returning
  `struct basic_string... *`.
- The failing pair indexed ctor still materializes:
  `__madc_objtmp_41` from `(*__ns_std_get__o2(__tuple1))`, calls the local
  wrapper `basic_string...__o15((&__madc_objtmp_41), ...)`, then forwards
  `__madc_objtmp_41` into the exported libstdc++ copy ctor.
- This points at CIR constructor-argument shaping, not parser alias resolution:
  inspect `object_arg_addr`, `ref_returning_call_type`, `class_ctor_call_addr`,
  and the parser-side return-reference annotation for namespace function
  templates. The fix should make `std::forward(std::get(...))` bind the
  reference-returning call directly or select the external copy ctor, without
  materializing a local undefined `basic_string` wrapper.

Next validation: rebuild with `make -C src`, rerun `tests/teststdmapint.mad` and
`tests/testmap.mad`, inspect `--emit=c11 tests/testmap.mad` around the pair
indexed ctor, then run the focused regressions (`testtuple`, `testforeach2`,
`testfstream`, `testloop`) before a capped `make -C src fulltest`.

### 2026-06-22 Codex continuation — anonymous aggregate layout fixed; `testmap` passes

Course correction after the `basic_string...__o15` wall: the undefined local
wrapper was not the final runtime root cause. Generic CIR reference-return /
constructor-argument handling moved that failure forward, then the
`std::map<std::string,int>` path corrupted tree storage at runtime.

Root cause:
- libstdc++ `std::basic_string` stores `_M_local_buf` and
  `_M_allocated_capacity` in an anonymous union.
- madc's class/struct emission flattened anonymous aggregate members as if they
  were sequential fields.
- That made the generated `basic_string` layout too large for the ABI. In the
  pair indexed ctor path, `std::pair<const std::string,int>` overflowed the
  `_Rb_tree_node` raw storage when writing `second`.

Fix:
- `DataDefSTRUCT` now records anonymous aggregate groups in
  `anonymous_aggregates` when `addAnonymousAggregate` flattens the semantic
  lookup surface.
- CIR struct/class member emission reconstitutes those groups as unnamed
  anonymous aggregate members instead of emitting each flattened child member at
  top level.
- The emitted C for `std::basic_string` now preserves the anonymous union:
  `_M_local_buf[16]` and `_M_allocated_capacity` share storage.

Focused validation:
- `make -C src` passed.
- `timeout 180 bin/madc --emit=c11 --std=c++17 --no-embedded-headers tmp/mapstr_noprint.mad`
  passed; the emitted C shows the preserved anonymous union.
- `timeout 180 bin/madc --emit=c11 tests/testmap.mad` passed; emitted C shows
  the same fixed `basic_string` layout.
- `timeout 180 bin/madc --std=c++17 --no-embedded-headers tmp/mapstr_noprint.mad`
  passed.
- `timeout 180 bin/madc tests/teststdmapint.mad` passed with expected
  size/update output.
- `timeout 180 bin/madc tests/testmap.mad` passed with expected
  size/Alice/Bob/Charlie output.
- Focused regressions passed: `tests/testtuple.mad`, `tests/testforeach2.mad`,
  `tests/testfstream.mad`, and `tests/testloop.mad`.

Remaining open work:
- Fulltest was not rerun after this fix; the last branch fulltest remains
  `658 passed, 5 failed, 0 timed out, 18 skipped`, and that count is now stale
  for `testmap`.
- Non-fatal libstdc++ `stl_tree.h` pointer-type warnings remain in the map
  path.

### 2026-06-22 Codex salvage continuation — C++20 enum and eval overload regressions recovered

The broad previous dirty state was preserved at
`failed/2026-06-22-map-cxx17-attempt-codex` commit `3534b44`; live work
continued on `wip/map-cxx17-salvage-codex` from that state rather than
destructively reverting.

Root causes fixed:
- C++20 `<compare>` regressions came from scoped enum constants being registered
  as `ddINT` even though their enum tag DataDef existed. `std::__cmp_cat`
  constructors then saw `int` where they needed the scoped enum type.
- `madc::eval_expression_ctx(std::string&, const char*, array&)` fell back to
  the `double&` overload because overload scoring stripped `array&` to
  `ddARRAY`, then only recognized exact `DataDefCLASS` object matches. The
  public `madc::array`/`madc::value` object is `ddARRAY`, not `DataDefCLASS`.

Fixes:
- Scoped enumerators now keep the enum DataDef, and `Variable::set`/`cmp`/`get`
  can store and read enum-backed integer constants.
- `score_arg_to_param` now ranks exact same-object DataDefs as exact matches
  before falling through to user-class identity, so `ddARRAY` binds `array&`
  generically.

Focused validation:
- `make -C src` passed.
- C++17 map canaries passed:
  `tests/testmap.mad` and `tests/teststdmapint.mad` under
  `--std=c++17 --no-embedded-headers`.
- C++20 comparison canaries passed:
  `tests/testcompare_realhdr.mad`, `tests/testspaceship_realhdr.mad`,
  `tests/testdefaultedcmp_realhdr.mad`, `tests/testrewritten_realhdr.mad`, and
  `tests/testinvocable.mad` under `--std=c++20 --no-embedded-headers`.
- Regression canaries passed:
  `tests/testtuple.mad`, `tests/testforeach2.mad`, `tests/testfstream.mad`,
  `tests/testloop.mad`, `tests/testifconstexpr.mad`,
  `tests/testmadcevalexpr.mad`, `tests/testmadcevalexprctx.mad`, and
  `tests/testmadcevalexprtyped.mad`.
- `--emit=c11 tests/testmadcevalexprctx.mad` now emits the
  `std::string& eval_expression_ctx` symbol for render calls, not the double
  reference fallback.

Fulltest status:
- First rerun: `657 passed, 4 failed, 2 timed out, 18 skipped`.
- Second rerun: `650 passed, 3 failed, 10 timed out, 18 skipped`.
- The timeout set shifted across unrelated tests; isolated timeout candidates
  pass sequentially under the runner's default 5-second cap. Treat this as host
  load/cap instability unless a timeout reproduces focused.
- Stable functional reds are now `testcontainerdtor`, `testmadc_ns`, `testset`,
  and `testsubscript`. `testmap` is no longer in the focused red set.

Remaining open work:
- Non-fatal libstdc++ `stl_tree.h` pointer-type warnings remain in the map
  path.
- `tmp/te_direct.mad` still has the direct `std::get` declaration-initializer
  overload-suffix/call-loss issue.

### 2026-06-22 — C++17 container campaign MERGED to develop + set-wall bug-1 FIXED, bug-2 ROOT-CAUSED (HANDOFF)

SETTLED / DO NOT RE-LITIGATE:
- The C++17 container campaign branch `wip/map-cxx17-salvage-codex` was merged
  into `develop` via `--no-ff` (commit on develop, baseline fulltest
  **660/4/0/18**). develop is **local, NOT pushed**. The merge was a deliberate
  decision to end the 10-day / 515-commit divergence; develop intentionally
  carries 4 known reds, all from the same "set wall".
- The "set wall" is **MULTIPLE bugs**, not one. Do not treat `testset`/
  `testsubscript`/`testcontainerdtor`/`testmadc_ns` as a single fix.

DONE THIS SESSION (committed on develop):
- Two salvage fixes (scoped-enum DataDefENUM typing; exact same-object overload
  scoring) — commits `b922be7`, `7c0a7c1`.
- **set-wall BUG-1 FIXED @ `cbd693a`**: a call whose `(` is immediately
  followed by a *substituted class-template type parameter* (a pre-resolved
  `TokenDataType`) was misparsed as a C-style cast `(Type)expr`, stealing the
  call's argument list and dropping arguments → "Incorrect number of
  parameters: expected N got N-1". This is `std::_Rb_tree::_M_insert_`'s
  `_M_impl._M_key_compare(_KeyOfValue()(__v), _S_key(__p))` (set's `_KeyOfValue`
  = `_Identity`). FIX: `Program::paren_opens_call_on_receiver()`
  (parser.cpp ~5354) — when the `(` binds (prevToken is the receiver's
  identifier/`)`/`]`) to a callable already on exStack (functor-class object
  with `operator()`, or a function/fn-ptr var), the cast/compound-literal
  detection in `parseExpr_operatorArm` (the `tb->id()==tkOpBrk` block, the
  `if (peek1 && !paren_opens_call_on_receiver(exStack))` guard ~parser.cpp
  19035) is skipped so the call paths run. Regression test
  `tests/testfunctorctorarg.mad` (default path). Verified: fulltest 660/4/0/18
  (+1 for the test); gcc torture non-timeout failset byte-identical to the
  51-name baseline (the 5 memcpy/memclr "new" entries were 5.0s TIMEOUTS =
  host-load noise, NOT regressions — pure-C tests the fix cannot touch).

NEXT — set-wall BUG-2 (ROOT-CAUSED, fix NOT yet written):
- ROOT CAUSE: a `return` statement does NOT apply an implicit user-defined
  conversion (a converting constructor) when the returned expression's type
  differs from the function's return type. Standard return-value
  copy-initialization ([stmt.return]/[dcl.init]). c2mir then rejects the tree:
  "incompatible return-expr type in function returning a struct/union"
  (c2mir.c:8476 — c2mir is RIGHT; madc emitted the wrong-typed return).
- 6-LINE REDUCER (default path, no flags needed):
    struct A { int v; };
    struct B { int v; B():v(0){} B(const A& a):v(a.v){} };
    B make(A a) { return a; }          // FAILS: implicit A->B ctor not applied
    int main(){ A a; a.v=42; B b=make(a); printf("%d\n",b.v); return 0; }
  Control `return B(a);` (explicit) WORKS → prints 42. So the conversion
  machinery EXISTS (functional construction / assignment / param-init all use
  it); `return` just doesn't invoke it.
- In std::set this is `iterator find(const key_type&){ return _M_t.find(__x); }`
  where set's `iterator` = `_Rep_type::const_iterator` but `_M_t.find` returns
  the tree's non-const `iterator` — implicit iterator->const_iterator
  conversion on return is dropped. The downstream "invalid types of comparison
  operands" on `s.find(x) != s.end()` is a CONSEQUENCE (find() returns the
  wrong type, so no matching operator!=); fixing the return conversion should
  fix both.
- FIX LOCUS: madc, where `return expr;` is typed against the function return
  type. Parser side preferred (where TokenObjTemp / converting-ctor machinery
  lives) — mirror how assignment/parameter-init apply a converting ctor, and
  insert the same construction when return-expr type != return type and a
  converting ctor exists. CirBuilder::translate_return is at
  cir_builder.cpp:9490; the return-type plumbing is around cir_builder.cpp:7710
  / 11210 (return_value_type/return_typedef_name). Reducer to drive TDD:
  `tmp/b2_min.mad` (recreate from the 6 lines above — tmp/ is gitignored).
- VALIDATION GATES (return stmts are everywhere — regression-sensitive):
  (1) the 6-line reducer prints 42; (2) probe `set<int>`/`set<string>`
  find/end compiles+runs under `--std=c++17 --no-embedded-headers`;
  (3) `make -C src fulltest` zero new reds; (4) FULL `scripts/run_gcc_testsuite.py`
  diff vs `docs/parity/torture-failset-current.txt` (51) — non-timeout set must
  be byte-identical; memcpy-a*/memclr 5.0s timeouts are load noise, ignore.

AFTER BUG-2 (to turn tests green):
- `testset.mad` uses C++20 `set::contains` — convert to C++17 `find/end` (or
  `count`), per user direction (same recipe as `testmap.mad`). Decide whether
  testset/testsubscript/testcontainerdtor/testmadc_ns get
  `.flags = --std=c++17 --no-embedded-headers` (real-header path, matches the
  shim-retirement direction) or must work on the default embedded path.
  `testmadc_ns` mixes `php::count` + `std::` — verify `--no-embedded-headers`
  doesn't disturb php:: before routing it that way.

MIRROR SYNC DEBT: develop not pushed; claude_status.json head=cbd693a updated;
README/CHANGELOG not yet updated for bug-1 (batch at set-wall completion).

### 2026-06-22 (cont.) — set-wall BUG-2 FIXED; BUG-3 root-caused to a 35-line reducer

SETTLED / DO NOT RE-LITIGATE:
- BUG-2 (return-value converting-ctor) is FIXED and verified. The earlier
  hypothesis "fixing the return conversion fixes BOTH find's type AND the
  `find()!=end()` comparison" was WRONG: bug-2 fixes find's RETURNED TYPE
  (CIR/c2mir level); the comparison is a SEPARATE parse-time operator-dispatch
  bug (BUG-3). The set wall is (at least) THREE bugs.

BUG-2 — FIXED (commit pending torture-gate; fulltest 661/4/0/18 ZERO regr):
- ROOT CAUSE refined: the NON-trivial (`__retbuf`) class-return path ALREADY
  applied the converting ctor (via `class_copy_construct_into_retbuf`'s
  `select_ctor_overload`). The bug was ONLY the TRIVIALLY-COPYABLE class return
  that uses c2mir's NATIVE struct return — `translate_return` emitted the raw,
  wrong-typed expression there.
- FIX (cir_builder.cpp): new member `m_cur_func_returns_value_class`, set in
  `func_def` to `as_class_instance(ret_dd)` when the fn returns a class BY VALUE
  and NOT via retbuf/ptr/ref/multi. In `translate_return`, BEFORE the plain
  `translate_expr`, when `operand_object_class(tr->returns) != rc` and
  `select_ctor_overload(rc, {expr})` finds a ctor, materialize a temp of the
  return class constructed from the expr (mirrors `object_arg_value`) and return
  it. Guard via `operand_object_class` (unwraps refs, leaves plain ptr/scalar
  NULL) so a `T*`/scalar return is NOT misfired on. Pure-C returns never enter
  (guard needs a user class). Regression test `tests/testreturnconvctor.mad`.
- VERIFIED in the REAL `set::find` emit (`--emit=c11`): find now gets the tree's
  `_Rb_tree_iterator`, runs the `_Rb_tree_const_iterator(const iterator&)`
  converting ctor, returns the `const_iterator`. Exactly right.

BUG-3 — ROOT-CAUSED, fix NOT written. `set<int>` STILL red after bug-2 because
`s.find(x) != s.end()` lowers to a RAW c2mir `!=` between two
`_Rb_tree_const_iterator` structs ("invalid types of comparison operands") —
the const-iterator's in-class FRIEND `operator!=` is never dispatched.
- 35-LINE REDUCER: `tmp/b3_C.mad` (recreate; tmp/ gitignored). Two class
  templates each with `typedef Self _Self;` and `_Self`-spelled in-class friend
  `operator==`/`operator!=`, where the const-iterator converts from the
  iterator and a Box returns them through a `_Rep_type::const_iterator` typedef.
  TRIGGER ISOLATED by bisection (b3_A works, b3_B works, b3_C fails): the bug
  fires only when BOTH classes in the chain spell their friend operators via a
  per-class `_Self` typedef. `b3_min`/`b3_min2` (single class, `_Self` or direct)
  both PASS — it is the MULTI-class `_Self` collision, not `_Self` alone.
- MECHANISM (parser.cpp): member `operator!=` lookup fails (it's a friend), so
  dispatch falls to `lower_free_operator_to_call` →
  `free_binary_operator_return_class` (line ~9619) which structurally CANNOT
  resolve a `bool`-returning operator (it only returns when `rethead` equals an
  operand class head — a class-by-value-returning op like `operator+`; for
  `bool` it returns NULL), then `find_free_operator_function` /
  `instantiate_free_operator_template` (line ~9417/9429) must find/instantiate
  the hoisted friend. For the iterator's `operator==` this WORKS (tree-internal
  code instantiated it). For the const-iterator's `_Self`-spelled friend `!=` it
  FAILS — the `_Self` parameter spelling apparently mis-resolves / collides with
  the other class's `_Self` during hoisted-friend registration
  (`parse_hoisted_friend_operator`, `register_skipped_friend_type`,
  `hoisted_friend_operator_defs`, friend_function_names — parser.cpp ~24165,
  ~25295-25330). NEXT: instrument `find_free_operator_function` /
  `instantiate_free_operator_template` for `operator!=` on b3_C to see whether
  the friend was registered with an unresolved/wrong `_Self` param type.
- LIKELY BUG-4 (after bug-3): the string-set + insert path shows
  "incompatible types in assignment to struct/union" at stl_set.h:96 (the
  `pair<const_iterator,bool>` insert return). Separate; revisit once bug-3 lands.

VALIDATION GATES already MET for bug-2: 6-line reducer (`tmp/b2_min.mad`) prints
42; non-trivial/dtor variant (retbuf path) prints 42; copy-init + param-init
unaffected; fulltest 661/4/0/18 zero new reds (the 4 are the known set wall);
torture diff vs the 51-name baseline byte-identical (the 5 memcpy-a*/memclr
entries are 5.0s host-load timeouts, not regressions). bug-2 COMMITTED @ 94d0798.

### 2026-06-22 (cont. 2) — set-wall BUG-3 FIXED; BUG-4 PINNED to set::insert pair-ctor arg

BUG-3 — FIXED (commit pending torture-gate; fulltest 662/4/0/18 ZERO regr).
- The earlier "MECHANISM" note above was on the right track but the precise
  cause was found by instrumentation, NOT a `_Self` type-RESOLUTION failure:
  the probe showed `_Self` resolves CORRECTLY per class scope (CIter↔CIter,
  RIter↔RIter; it is NOT in the global datatype_map). The real cause is overload
  IDENTITY: `peek_param_list_spelling()` builds the overload-dedup key from the
  RAW parameter TEXT, so both `_Rb_tree_iterator`'s and `_Rb_tree_const_iterator`'s
  hidden-friend `operator!=(const _Self&, const _Self&)` spell IDENTICALLY
  (`_Self`). The global-operator dedup (parser.cpp ~38990: `if
  (ovset[i].param_spelling == ns_overload_spelling) same = ...`) then treats the
  second as a RE-DECLARATION of the first — it reuses the first's symbol and the
  two collapse to ONE overload (`find_free_operator_function` saw n=1, should be
  2). The const-iterator's `!=` is lost; `s.find(x) != s.end()` falls back to a
  raw c2mir struct `!=` ("invalid types of comparison operands").
- FIX (parser.cpp `peek_param_list_spelling`): when inside a class scope
  (`class_scope_stack` non-empty — the friend/member parse), canonicalize a
  parameter token that names a current-class-scope alias to its RESOLVED type's
  unique name (`resolve_current_class_type_alias(tok)->name`) before appending
  to the identity spelling. Two classes' identically-spelled `_Self` params then
  get DISTINCT identities and stay separate overloads. Strictly SPLITS
  wrongly-merged overloads (consistent same-scope resolution → cannot cause a
  false NON-merge); plain namespace-function overloading is untouched
  (class_scope_stack empty). Regression test `tests/testfriendopself.mad`.
- VERIFIED: `b3_C` reducer now prints `ne`; `b3_distinct` (distinct alias names —
  the discriminating control) still `ne`; real `set<int>` find/end COMPILES AND
  RUNS (`tmp/set_findonly.mad` prints `no`). fulltest 662/4/0/18 (+1 test; same 4
  set-wall reds).

BUG-4 — PINNED to the REAL failing construct, fix NOT written. `set<int>` (with
insert) still red: 2 c2mir errors reported at stl_set.h:96. My FIRST hypothesis
(pair<const_iterator,bool> arg conversion) was WRONG — that path's emitted
`(void*)(&__p.first)` raw-member bind is c2mir-ACCEPTED (the void* cast masks it;
`tmp/b4_min2.mad` reproduces the identical emit and RUNS fine). The ACTUAL error
was localized by compiling the `--emit=c11` output (`tmp/seti_emit2.c`) with
`gcc -fsyntax-only`: emitted line ~1543, in `_Rb_tree::_M_insert_` (real source
stl_tree.h:1831):
    _Link_type __z = __node_gen(_GLIBCXX_FORWARD(_Arg, __v));
`__node_gen` is a `_NodeGen&` (== `_Alloc_node&`) FUNCTOR reference parameter;
`__node_gen(forward(__v))` is a functor `operator()` CALL. madc MIS-LOWERED it to
an ASSIGNMENT:
    struct _Rb_tree_node_int32_t *__z = ((*__node_gen) = (*__ns_std_forward__o2(&*__v)));
i.e. it constructed/assigned an `_Alloc_node` from the forward() result instead
of CALLING `__node_gen.operator()(...)` — gcc: "incompatible types when assigning
to type '..._Alloc_node' from type 'int'". So the functor `operator()` was NOT
dispatched; the `identifier(args)` was misparsed (a cousin of BUG-1 — likely the
functor VARIABLE `__node_gen` mistaken for a TYPE → functional-construction
`_Alloc_node(forward(__v))`).
- REDUCER STATUS: a SIMPLE non-template functor call via a reference param WORKS
  (`tmp/b4_functor2.mad`: `int doit(int v, Gen& gen){ return gen(v); }` -> 105).
  The TEMPLATE-member version (`tmp/b4_functor.mad`: `template<class NG> int
  doit(int v, NG& gen){ return gen(v); }`) FAILS differently ("use of undeclared
  identifier 'doit'") — so the trigger needs the template-member + forwarding-ref
  (`_Arg&& __v` + `std::forward`) context, not the bare functor-via-ref. NEXT:
  build a reducer with a template member `operator()`-functor-ref call whose arg
  is `std::forward<...>(v)`, then trace the parse of `gen(forward(v))` inside a
  template-member body (is `gen` mis-classified as a type? does the
  forwarding-ref arg derail the call-vs-construction disambiguation that BUG-1's
  `paren_opens_call_on_receiver` was meant to settle?).
- This is the LAST known set-wall blocker for `set<int>` find+insert. After it,
  re-check `testset.mad` (convert C++20 `contains`->C++17 find/end), then the 4
  reds' `.flags` routing.

### 2026-06-22 (cont. 3) — set-wall BUG-4 FIXED (functor template operator()); BUG-5c remains

BUG-4 was MIS-CHARACTERIZED earlier as "pair conversion". The real bug:
`_Rb_tree::_M_insert_`'s `__node_gen(forward(__v))` is a FUNCTOR `operator()`
call (`__node_gen` is a `_NodeGen&` = `_Alloc_node&` whose `operator()` is a
member TEMPLATE). madc never registered/dispatched a TEMPLATE `operator()`, so
the call fell through to a bogus assignment `*gen = v`. Root-caused to a 9-line
reducer (`tmp/b4_f.mad`) and FIXED — it took FIVE sub-fixes, all in parser.cpp,
all verified (tests `testfunctortmploperator.mad`, `testmembertmplptrret.mad`):
  1. `skipped_template_function_declarator_name_index` — recognize an
     operator-function-id declarator (`operator()`/`operator[]`/`operator==`):
     the operator-id IS the name, the param-list `(` follows it (operator()'s
     own `()` sits between the `operator` keyword and the params). Without this
     `register_skipped_class_template_function` bailed (empty name) so the
     template operator() was never a method -> findMethod("operator()") failed
     -> functor-call path skipped -> `g(args)` mis-lowered to `g = args`.
  2. New helper `skipped_template_function_param_lparen` — the param-list `(`
     is `name_idx + operator_id_span` for an operator-id (not `name_idx+1`); used
     so signature/return-spelling extraction reads the REAL params (was reading
     operator()'s own `()` -> 0 param spellings -> deduction failed).
  3. `instantiate_member_fn_template_for_call` rename — replace the WHOLE
     operator-id span with the unique `__mti` ident (was replacing only the
     `operator` token, leaving `()` as a spurious empty declarator -> parse fail).
  4. `skipped_template_function_return_type` — fold trailing pointer `*`s onto
     the base (getPointerType). A POINTER return (`_Link_type`/`Node*`) was
     collapsing to the base by-value type, which gave the instantiated method a
     by-value struct return and (cascading) dropped its hidden `__this`.
  5. `skipped_template_function_is_static` — scan only the declaration HEADER
     (before the param-list `(`), not the body. A `static` LOCAL in the body
     (`{ static Node n; ... }`) was wrongly marking the method static, dropping
     `__this` from the instantiated method — which broke the functor call (the
     functor dispatch always passes `__this`) with "too many arguments".
All functor reducers pass: b4_f/b4_e/b4_b/b4_ptr/b4_named/b4_core/b4_int2/b4_g.

BUG-5c — REMAINS (the set<int> runtime blocker now). After bug-4, `set<int>`
COMPILES but SIGSEGVs at runtime (address 0x3) in
`_Rb_tree::_M_construct_node__mti` (a VARIADIC member template). Source
(stl_tree.h:592): `get_allocator().construct(__node->_M_valptr(),
std::forward<_Args>(__args)...)` — a variadic-member-template + allocator
`construct` chain. The crash on a tiny address = a null/garbage pointer
(likely `__node`/`_M_valptr()` or the variadic forwarding mis-instantiated).
This is a SEPARATE variadic-member-template/allocator bug, not bug-4. NEXT:
reduce a variadic member template `construct(_Link* node, _Args&&... args)` that
forwards to `node->valptr()` + an allocator-like construct, and trace the
`__mti` instantiation. b4_a's free-function-template "undeclared identifier"
(`tmp/b4_a.mad`) is ALSO a separate (free-template registration) gap, unrelated.

### 2026-06-22 (cont. 4) — set-wall BUG-5c FIXED (retbuf-method ref-arg off-by-one); BUG-6 pinned

BUG-5c — FIXED (commit pending fulltest/torture gate). `set<int>` insert+find now
COMPILES AND RUNS (`tmp/b5_set.mad` prints `yes`). The SIGSEGV (addr 0x3/0x5) in
`_Rb_tree::_M_construct_node__mti` was a DOWNSTREAM symptom; the real bug was at
the CALL into `_M_insert_unique__mti` from `set::insert`.
- ROOT CAUSE (cir_builder.cpp): the copy-elision / NRVO init path for
  `T v = obj.m(args)` where `m` returns a non-trivial class BY VALUE (the
  __retbuf ABI) — cir_builder.cpp ~10970-11005 — prepends `&v` (retbuf) and the
  method's `__this`, then delegates the EXPLICIT args to `build_call_args`.
  But `build_call_args` (the FREE/STATIC-function arg builder) maps explicit arg
  `i` to `callee->parameters[i]` with NO offset, while a METHOD's
  `parameters[0]` is the hidden `__this`. So explicit arg 0 was coerced against
  `__this` (a plain pointer, `is_ref_param`==false) instead of the real
  reference parameter — the reference materialization was skipped and the arg
  was passed as a VALUE where the `int*` (lowered `const value_type&` /
  forwarding-ref) parameter expected a pointer. c2mir's "using integer without
  cast for pointer type parameter" warning; at runtime the int (e.g. 42==0x2a)
  is dereferenced as an address -> SIGSEGV. (The regular method-call arg loop at
  ~4061 already used `pi = i + 1` to skip __this; only the copy-elision path was
  wrong.)
- FIX: add an explicit `size_t param_base = 0` to `build_call_args` (the index in
  `callee->parameters` of the first explicit user arg). Per-arg coercion uses
  `pi = i + param_base` for both the type AND `is_ref_param`. The method
  copy-elision caller passes `param_base = 1` (skip __this); free/static callers
  keep 0. Deepest-layer fix — corrects the indexing at the single shared
  arg-coercion chokepoint, no symptom shim.
- REDUCTION LADDER (each `tmp/`): b5_fwd (in-class fwd-ref, WORKS) -> ... ->
  b5_min (plain function + retbuf + const-ref + prvalue, WORKS — only 1 hidden
  param) -> b5_min2 (METHOD + retbuf + __this + const-ref + prvalue, CRASHES) —
  the minimal trigger is two leading hidden params (retbuf + __this). Regression
  test `tests/testretbufrefarg.mad` (header-free; runs on the default path):
  exercises BOTH a prvalue (`make(105)`) and an lvalue (`make(n)`) bound to a
  `const int&` parameter of a retbuf-returning method.
- VERIFIED: b5_min2/b5_fwd6/b5_set all pass; testretbufrefarg prints `105 1` /
  `107 1`.

BUG-6 — PINNED, fix NOT written (the NEXT set-wall blocker for STRING-element
containers). After bug-5c, `testsubscript` / `testcontainerdtor` / `testmadc_ns`
(all use `map<string,int>` + `map<string,string>` / `set<string>`) now fail with
`MIR fatal error: Repeated item declaration _Tp2___dtor`.
- `_Tp2` is the nested helper struct inside `__gnu_cxx::__aligned_buffer<_Tp>`
  (`/usr/include/c++/13/ext/aligned_buffer.h:54`: `struct _Tp2 { _Tp _M_t; };`,
  used ONLY in `alignas(__alignof__(_Tp2::_M_t))`). madc instantiates a CONCRETE
  `_Tp2` per `__aligned_membuf<X>` instantiation but does NOT uniquify the nested
  type's name by its enclosing instantiation — so `__aligned_membuf<pair<const
  string,int>>::_Tp2` and `__aligned_membuf<pair<const string,string>>::_Tp2`
  both emit a bare `_Tp2___dtor`, colliding in the MIR module.
- TWO things to decide at fix time: (a) nested types defined inside a class
  template must get instantiation-unique emitted names (qualified by the
  enclosing concrete instantiation), and/or (b) `_Tp2` is never destructed as an
  object (only `alignof`'d) so a dtor should not be emitted for it at all. (a) is
  the general correctness fix; (b) may be a cheaper guard. NEXT: reduce two
  distinct `__aligned_buffer<A>` / `<B>` instantiations in one TU and trace where
  the nested-type emit name is chosen.
- testset's remaining red is UNRELATED to bug-6: it uses C++20 `names.contains()`
  (testset.mad:19) — convert to C++17 `find/end` once the container path is green.

### 2026-06-22 (cont. 5) — set-wall BUG-6 FIXED (nested-type dtor-symbol collision); testset converted; BUG-7 pinned

BUG-6 — FIXED (commit pending). `set<string>` + `map<string,string>` in one TU
gave `MIR fatal error: Repeated item declaration _Tp2___dtor`. `_Tp2` is the
nested helper struct inside `__gnu_cxx::__aligned_membuf<_Tp>`
(`ext/aligned_buffer.h:54`: `struct _Tp2 { _Tp _M_t; };`).
- ROOT CAUSE (parser.cpp ~22341): an EXISTING fix already qualified the nested
  struct's STORE KEY by the enclosing class (`Enclosing::_Tp2`) so the 2nd
  instantiation doesn't "Struct already defined". But it left `dds->name` as the
  bare tag `_Tp2`, and `class_dtor_symbol` (cir_builder.cpp:4796) emits
  `cdd->name + "___dtor"` — so BOTH `__aligned_membuf<string>::_Tp2` and
  `__aligned_membuf<pair<const string,string>>::_Tp2` emitted the SAME
  `_Tp2___dtor`. Collision in the MIR module.
- FIX (parser.cpp, same branch): when the store key is enclosing-qualified, also
  rename the EMITTED identity `dds->name` to a C-valid enclosing-qualified name
  (`class_scope_stack.back()->name + "__" + tag`). Parse-time `_Tp2::_M_t`
  resolution uses the store key (not dds->name), so the rename is transparent to
  lookups; only the emitted struct tag + dtor symbol change, and each
  instantiation's dtor is now unique. (The FIRST instantiation keeps the bare
  name; only 2nd+ collide, exactly when the qualification branch fires.)
- REDUCER: `tmp/b6_real.mad` (`set<string>` + `map<string,string>`) — recreate;
  tmp/ gitignored. (Hand-written `Buf<A>/Buf<B>` reducers hit an EARLIER parse
  variant "Struct 'Inner' already defined" / "nested type as member type" — a
  different parser limit on the hand-written instantiation path, NOT the MIR-time
  bug; use the real-header reducer to pin bug-6.)
- VERIFIED: b6_real compiles; `testcontainerdtor` + `testmadc_ns` now PASS (both
  use `set<string>`/`map<string,string>`). fulltest 667/2/0/18 (was 665/4) zero
  regressions; gcc torture summary byte-identical to baseline (the fixed branch
  only fires for nested-struct redefinition in a class scope — inert for C
  torture). testset.mad converted: C++20 `names.contains(key)` -> C++17
  `(names.find(key) != names.end()) ? 1 : 0` (member `.contains()` deferred to a
  future --std=c++20-flagged test).

BUG-7 — PINNED, fix NOT written (the LAST set-wall reds: testset + testsubscript).
Both now fail with `MIR error: import of undefined item ..._o<N>`:
- testset: `set_<string,...>__insert__o5` (an undefined `set::insert` overload
  wrapper).
- testsubscript: `basic_string_<...>__basic_string_<...>__o15` (an undefined
  `basic_string` CONSTRUCTOR overload wrapper #15).
A specific method/ctor OVERLOAD is REFERENCED (the call site emits the
`..._o<N>` symbol) but its BODY is never emitted into the MIR module — so MIR
import fails at link. (claude_status notes the `basic_string...__o15` wrapper was
"moved forward by generic CIR reference-return/constructor handling" — same
family.)

ISOLATED (3-line reducers, recreate; tmp/ gitignored). For `set<string>`:
- `s.insert("Alice")` (const char* literal) -> FAILS: `_insert__o5` undefined.
  The emitted call is `set_..._insert__o5((&s), "Alice")` — note the raw
  `const char*` arg is passed straight through; NO `std::string` temporary is
  materialized for the `value_type&&` / `const value_type&` parameter.
- `s.insert(t)` where `t` is a `std::string` LVALUE -> WORKS (prints size). The
  emit has NO `set::insert__o<N>` symbol at all — set::insert resolves/inlines
  straight to `_M_insert_unique__mti`.
- `s.insert(std::string("Alice"))` (EXPLICIT string temporary) -> WORKS.

So bug-7 is the IMPLICIT `const char*` -> `std::string` conversion at a header
method-call ARGUMENT. When the arg already IS the parameter's class type (lvalue
or explicit temp) madc inlines set::insert through to `_M_insert_unique`. When
the arg needs a user-defined conversion to bind the class-typed parameter, madc
instead emits a CALL to the method's `__o<N>` wrapper but (a) does NOT construct
the implicit string temporary (passes the raw const char*) and (b) does NOT
emit/schedule that wrapper's body. The two symptoms share one cause: the
conversion-requiring overload binding takes a fallback path that neither
materializes the conversion nor registers the overload body for emission.
NEXT: find where a class-typed method PARAMETER bound from a convertible scalar/
pointer arg is handled in the method-call arg build (class_method_call ~3955 /
build_call_args param_object_class branch) — compare the inline-able no-conversion
path vs the `__o<N>`-wrapper-call fallback, and ensure the conversion path both
emits the string-temp (the converting ctor, like object_arg_value/object temp)
AND adds the resolved overload symbol to referenced_funcs so its body emits.
testsubscript's `basic_string...__o15` is the ctor-side mirror (a string ctor
overload referenced via implicit conversion, body not emitted).

CRUCIAL REFINEMENT (emitted-C evidence — this narrows the fix target): the
failing `__o5` call is MALFORMED, not merely missing its body. In the WORKING
lvalue case the const-ref `set::insert` overload emits a BARE symbol (no `__o`
suffix) with a FULL definition and is called `_insert((&retbuf), (&s), (&t))` —
3 args: retbuf, this, &string. In the FAILING literal case the call is
`_insert__o5((&names), "Alice")` — only TWO args: this + the RAW `const char*`,
with NO retbuf (even though insert returns `pair<...>` via the retbuf ABI) and no
string-temp. So the rvalue overload was resolved to a SYMBOL (`__o5`) but never
bound to a parsed/emitted FuncDef: the call was lowered by a GENERIC fallback
path (no retbuf injection, no class-param conversion, no `referenced_funcs`
registration). The real defect is in the OVERLOAD RESOLUTION / method binding
(parser findMethodOverload selecting `insert(value_type&&)` for a const-char* arg
via the implicit string conversion) returning a half-bound result — fix there so
the selected overload is a fully-resolved FuncDef (correct retbuf/this/by-ref
arg shape AND body emission), OR is rejected in favor of a binding that
materializes the converting temporary first. Start by tracing how `__o5` is
chosen vs the bare const-ref overload (overload-disambiguator assignment +
whether the rvalue overload's FuncDef reaches the CIR call-lowering at all).

PRECISE ENTRY POINTS (verified this session):
- `DataDefCLASS::findMethodOverload` (parser.cpp:8752) ranks same-name overloads
  via `score_arg_to_param` (cir_builder.cpp:5061, `allow_udc` defaults TRUE).
  A const char* arg vs a `std::string` class param scores 2 (user-defined
  conversion through the `string(const char*)` ctor) — for BOTH `insert(const
  value_type&)` AND `insert(value_type&&)`. That is a TIE at 2; the non-template
  tiebreak (8849) only reorders member templates, so the FIRST-registered
  overload (`const value_type&`, declared first in stl_set.h:511) SHOULD win and
  emit the bare `_insert`. Yet the const-char* case selects `__o5` and emits a
  MALFORMED call (no retbuf, raw const char*). So FIRST INSTRUMENT
  findMethodOverload (MADC_DBG) on `tmp/b7_lit.mad` to see whether it (a) returns
  the `&&` overload (tie mis-resolved), or (b) returns NULL and a FALLBACK path
  emits the guessed `__o5`. The string-arg cases (b7_lv lvalue, b7_tmp explicit
  `std::string("Alice")` prvalue) BOTH bind the bare const-ref `_insert` (full
  3-arg retbuf/this/&string call + body) and WORK — so the divergence is solely
  the const-char* arg type, and the fix must make that arg either (i) bind the
  same const-ref overload after materializing the string temp, or (ii) bind the
  `&&` overload with a fully-resolved FuncDef + retbuf + the converting temp.
- Cross-check clang Sema (`/workspace/llvm-clang-src`, `lib/Sema/SemaOverload.cpp`
  — `TryUserDefinedConversion` / `CompareImplicitConversionSequences` /
  `CompareStandardConversionSequences`): for `insert("Alice")` clang forms ONE
  user-defined conversion to `std::string` (a prvalue), then prefers
  `insert(value_type&&)` over `insert(const value_type&)` (rvalue binds an
  rvalue-ref better). So the C++-correct pick IS the `&&` overload — meaning
  fix (ii) is the faithful path: select `&&`, materialize the `string("Alice")`
  temporary, bind it to the rvalue-ref param, and emit the body. madc currently
  reaches the `&&` symbol but drops the temp + body.

### 2026-06-22 (cont. 6) — set-wall BUG-7a FIXED (UDC ctor scan missed defaulted-param ctors); BUG-7b pinned (testsubscript)

BUG-7a FIXED (commit pending). Instrumenting findMethodOverload + score_arg_to_param
on tmp/b7_lit.mad cracked it: for `set<string>::insert("Alice")` the non-template
`insert(const string&)` / `insert(string&&)` overloads were REJECTED (score -1)
and a 1-param member-template insert wildcard-matched as `__o5` (the undefined
import). The rejection traced to score_arg_to_param's user-defined-conversion
ctor scan (cir_builder.cpp ~5098): it only considered ctors with EXACTLY 2
parameters (`__this` + one). Real libstdc++ `basic_string(const char* __s, const
_Alloc& __a = _Alloc())` has THREE parameters (__this + const char* + a DEFAULTED
allocator), so the converting ctor was skipped, `const char*` -> `std::string`
scored -1, and the proper insert overloads died.
- FIX (cir_builder.cpp): the UDC ctor scan now accepts any ctor CALLABLE WITH ONE
  explicit arg — `parameters.size() >= 2 && required_param_count() <= 2` (params
  after the first are defaulted) — and scores the arg against parameters[1].
  Deepest-layer fix at the single shared ranking function; no symptom shim.
- VERIFIED: b7_lit prints 1; testset.mad PASSES (size: 2 / has Bob: 1 / has
  Charlie: 0). fulltest 668/1/0/18 (was 667/2) — only testsubscript remains,
  zero regressions. (torture gate pending at write time.)

BUG-7b — PINNED (testsubscript, the LAST set-wall red). With bug-7a in, the
converting ctor is now correctly SELECTED, so testsubscript advances from the
`set::insert__o5` family to `MIR error: import of undefined item
basic_string..._basic_string..._o15` — a specific basic_string CONSTRUCTOR
overload (#15) is referenced (the implicit `const char*` -> `std::string`
conversion, e.g. `ages["alice"]` / `names.push_back("hello")` /
`map<string,string> dict[k]=v`) but its BODY is never emitted into the module.
This is the CTOR-side mirror of bug-7 (selection now works; emission of the
SELECTED converting-ctor overload does not). NOTE testset's insert conversion
emits its string ctor fine — so the gap is specific to which ctor overload /
which call context testsubscript hits (operator[] key conversion and/or
vector<string>::push_back(const char*)). NEXT: `--emit=c11` testsubscript, locate
the `_basic_string..._o15` decl with no definition, identify which basic_string
ctor overload #15 is (registration order) and which call site references it, and
trace why that overload's body is not scheduled for emission (referenced_funcs
registration of the converting-ctor symbol vs the emitted ctor-body symbol —
likely an `__o<N>` suffix mismatch between the call's converting-ctor symbol and
the emitted ctor definition, the ctor-side analogue of the method case).

BUG-7b REFINED (emitted-C evidence, tmp/tsub_emit.c). `__o15` appears EXACTLY
ONCE — a lone CALL in the PIECEWISE-PAIR construction, no decl/def. Two pair
piecewise ctors are instantiated for `pair<const string, string>`:
- `pair...__o19` constructs `pair.first` DIRECTLY via the MANGLED-DIRECT real
  libstdc++ copy ctor (`_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEC1ERKS4_`)
  on `forward(get(tuple1))` — WORKS.
- `pair...__o20` instead materializes an INTERMEDIATE temp
  `__madc_objtmp_94 = basic_string...__o15(*get(tuple1))` (a MADC-emitted ctor
  symbol, NO body) then copies that into `pair.first`. The bodyless `__o15` is
  the failure.
So bug-7b lives in the piecewise-pair member construction (std::pair(
piecewise_construct, tuple, tuple) -> construct each member from std::get<I>):
one pair-ctor instantiation (__o20) lowers the member-construct through a
madc-emitted basic_string ctor __o15 that is never defined, while the parallel
instantiation (__o19) correctly uses the mangled-direct real copy ctor. This is
the SAME construct_at / piecewise-pair wall the pre-session commits flagged
(7d9927d "construct_at resolves pair ctor to COPY ctor not piecewise", 5b67643
"piecewise-pair ctor call mismatch"). FIX DIRECTION: route __o20's member
construction through the SAME mangled-direct real ctor path as __o19 (no madc
__o15 intermediate), OR emit the selected __o15 body. The divergence likely
turns on the `*get(tuple1)` arg shape (deref vs forward) routing one path
through a converting-ctor temp and the other through the mangled copy ctor.
SUBSTANTIAL piecewise-pair cycle, separate from bugs 5c/6/7a — the LAST set-wall
red. map<string,string> + map<string,int> both hit it via operator[].

### 2026-06-22 (cont. 7) — BUG-7b INVESTIGATION COMPLETE (handoff for the fix; earlier hypothesis CORRECTED)

>> THIS SECTION SUPERSEDES the bug-7b fix-direction guesses in cont. 5 and cont.
>> 6. Those said "piecewise-pair / std::forward reference-loss". That hypothesis
>> was DISPROVEN by instrumentation (below). Do NOT chase std::forward.

DEFINITIVE ROOT CAUSE (confirmed by an MADC_DBG_O15 probe in object_arg_addr's
materializing tail, cir_builder.cpp ~1247, run on tests/testsubscript.mad):

The undefined symbol `basic_string..._basic_string..._o15` is a basic_string
CONSTRUCTOR overload that object_arg_addr's materializing tail emits (via
class_ctor_call) to build a `std::string` TEMP, but whose body is never emitted.
The probe at the materializing tail showed:
- The const char* literal conversions (`ages["alice"]`, `names.push_back("hello")`,
  etc.) materialize via ctor `__o9` = `basic_string(char* __s, allocator* __a)` —
  and `__o9` IS emitted WITH a body (tsub_emit.c:4740). Those WORK. (This is the
  bug-7a converting ctor — 2 params, allocator defaulted — now correctly found.)
- The FAILING `__o15` is a ONE-arg basic_string ctor selected to materialize a
  temp from `*std::get<_Indexes1>(__tuple1)` (a string), inside the piecewise-pair
  delegated ctor __o20. Call: `__o15(&objtmp_94, *get(tuple1))`. NO body emitted.
- CRUCIAL: at that materialization the arg's datadef was NULL (`arg_dd=0`) — i.e.
  `std::get<_Indexes1>(__tuple1)`'s return type is UNRESOLVED in this context. The
  plain copy ctor IS available mangled-direct (`_ZNSt..C1ERKS4_`, used 5x in the
  same emit), but because the get-result type is unknown, ctor selection cannot
  match the mangled copy ctor and instead emits an unbacked madc-local `__o15`.

So bug-7b is a `std::get` / tuple RETURN-TYPE RESOLUTION gap (the get<0>/_Nth_type
family from topic-file UPDATE 57-59), NOT a std::forward reference-loss and NOT a
piecewise-pair-specific bug. The piecewise ctor is just the context where an
unresolved-typed `std::get` result feeds a string temp materialization.

WHY the two pairs differ (pair<const string,int> WORKS, pair<const string,string>
FAILS): in the int-pair __o20 the `first` member-init arg stayed the resolved
`forward(get(tuple1))` call chain (object_arg_addr line ~1153 ref-bind path) and
bound directly via the mangled copy ctor; in the string-pair __o20 the get result
reached object_arg_addr as an unresolved-type value (arg_dd=0) and fell to the
materializing tail -> bodyless __o15. The difference is which instantiation
resolves `std::get<_Indexes1>(tuple<...>)`'s return type; for the string-valued
tuple element it comes back unresolved.

FIX PLAN (post-compaction session — DO THIS):
1. REPRODUCE: `tmp/b6_real.mad` (set<string>+map<string,string>) and
   tests/testsubscript.mad both hit it; flags = none (default path) OR
   `--std=c++17 --no-embedded-headers`. Confirm at live HEAD first (b5698d7+).
2. RE-INSTRUMENT if needed: re-add the MADC_DBG_O15 probe (see git history of this
   investigation — it printed target/argtype/var/vartype/is_obj_val/arg_dd right
   before the object_arg_addr materializing tail at cir_builder.cpp ~1247). The
   probe is REMOVED from the tree; recreate it. Confirm arg_dd=0 (null) for the
   `*get(tuple1)` materialization in the string-pair __o20.
3. ROOT-FIX (deepest layer): make `std::get<I>(tuple<_Elements...>)` resolve its
   return type (a reference to the I-th element) in this instantiation context, so
   `*get(tuple1)` has a basic_string datadef. With the type resolved, object_arg_addr
   binds the get reference directly to the mangled-direct copy ctor (line ~1153)
   like the int-pair — the bodyless __o15 disappears. Look at how get<>/tuple
   element types are resolved (parser: the _Nth_type / tuple-get return-type path,
   topic UPDATE 57-59; and cir_builder ref_returning_call_type / call_target_funcdef
   for the get call). The get return type is null specifically when the tuple
   element is a class type (string) reached through the piecewise _Index_tuple
   expansion.
4. FALLBACK (if the get-type fix is too broad): in object_arg_addr's materializing
   tail, when the selected ctor is a copy/move of a MANGLED-DIRECT class, emit the
   mangled-direct ctor symbol (the class's real Itanium C1 symbol) instead of a
   madc-local __o<N> — but this is a SHIM; prefer the type-resolution root fix.
5. VALIDATE: testsubscript green -> set wall FULLY cleared (0 reds). fulltest
   (expect 669/0/0/18), gcc torture failset byte-identical to the 51-name baseline,
   SMAUG soak. THEN batch the push + README/CHANGELOG/ROADMAP (set-wall complete).

STATE AT HANDOFF: develop @ (the doc commit after b5698d7), local NOT pushed.
bugs 1-6 + 7a fixed+committed; testsubscript is the SOLE red, on bug-7b above.
fulltest 668/1/0/18, torture byte-identical to baseline. The probe is removed;
tree clean. tmp/ reducers gitignored — recreate b6_real.mad / b7_lit.mad per the
descriptions in cont. 4/5/6.

### 2026-06-22 (cont. 8) — BUG-7b FIXED. SET WALL CLEARED (0 reds). Root cause CORRECTED again.

>> cont. 7's characterization ("std::get's return type unresolved, arg_dd=0") was
>> the SYMPTOM, not the mechanism. The real root cause, found by instrumentation
>> at live HEAD, is below. cont. 7's FIX PLAN step 3 (resolve get<I>(tuple) return
>> type) was aimed at the wrong layer — the get instantiation was ALREADY correct.

ACTUAL ROOT CAUSE (instrumented, confirmed):
- testsubscript fails ONLY when TWO `pair<const string,V>` instantiations coexist
  (e.g. map<string,int> AND map<string,string>); either map ALONE compiles+runs.
  Minimal reducer: tmp/sub_both.mad (both maps) fails; sub_si/sub_ss (one each) pass.
- The failing `__o15` is a bodyless 1-arg basic_string ctor that object_arg_addr's
  materializing tail (cir_builder.cpp ~1270) emits to build the `first` member of
  the piecewise pair from `std::get<0>(tuple<string&&>)`. It falls to the tail
  (instead of the ref-bind fast path at ~1153) because ref_returning_call_type
  returned a DataDef NAMED "0".
- ref_returning_call_type: `call_target_funcdef(tcf)` returns the CORRECT get
  instantiation (ret = `const basic_string&`). But the result type came from the
  `tcf->returns()` FALLBACK, not cfd, because the call carried a parse-time
  `return_override` whose type was the bogus "0", and the guard
  `(!return_override || cfd != raw_fd)` chose the override (cfd == raw_fd here).
- WHERE "0" came from: parser.cpp `resolve_fn_template_return_by_key` (called from
  ~line 13507 to set return_override on a template-id call with explicit args).
  std::get has TWO overloads: by-INDEX `get<size_t __i, _Elements...>` and by-TYPE
  `get<typename _Tp, _Types...>`. For `get<0>`:
    - by-index: return `__tuple_element_t<__i, tuple<_Elements...>>` references the
      UNBOUND pack `_Elements` -> correctly skipped (return_has_unbound_tp).
    - by-type: binds `_Tp` (a TYPE param) to the explicit NON-TYPE arg `0` (carried
      as a DataDef named "0" by capture_call_template_args, parser.cpp:29033),
      yielding return type `_Tp&` = "0&". NO kind check -> this bogus candidate won.
  This only surfaces with two maps because of instantiation/registration ordering
  that exposes the by-type candidate to the return-type resolver for the 2nd pair.

THE FIX (deepest layer, parser.cpp `resolve_fn_template_return_by_key`):
- New `datadef_is_nontype_constant(dd)` helper: a folded non-type arg is an
  integer-constant DataDef whose name is its decimal spelling (numeric name);
  genuine type args are never bare integers.
- In the positional explicit-arg binding loop: a non-type VALUE arg bound to a
  TYPE template parameter (`ft.typeparam_is_type[i]`) is a substitution failure
  ([temp.arg.nontype]) -> skip the candidate. This removes the by-type get<>
  overload for `get<0>`, leaving NO resolvable candidate -> returns NULL -> no
  bogus return_override -> ref_returning_call_type uses cfd's correct return.
- One-spot change; matches clang/gcc overload-removal-by-kind-mismatch.

VALIDATION: testsubscript GREEN, exact .expect match. sub_both/sub_rev exit 0.
fulltest 669 passed / 0 failed / 0 timed out / 18 skipped (set-wall TESTS = 0 reds).
gcc torture 51 non-timeout failures, BYTE-IDENTICAL to the baseline (5 host-load
timeouts memclr/memcpy-a* excluded). Zero regressions. Committed @ da96d7a.
Probes removed, clean rebuild. tmp/ reducers (sub_both/sub_si/sub_ss/sub_lit/
sub_rev) gitignored.

OUTSTANDING (pre-existing, NOT bug-7b, blocks a clean `make fulltest` exit 0):
the `call-emit-symbol` drift gate (scripts/check-call-emit-symbol.sh, added
4517ab9) now reports 6 raw `local_emit_name` value-reads (target 0) and exits 1.
ALL 6 are in committed HEAD (mid-June commits 06-14/15/16), NONE from bug-7b, and
all are FALSE POSITIVES vs the gate's documented intent ("read as a value, i.e.
used to BUILD A SYMBOL"): 3 are debug prints (parser.cpp ~29764/32925/33056,
inside #if/dbg_ blocks), 2 are name LOOKUPS (cir_builder.cpp 1409/4057,
findVariable(callee->local_emit_name) — metadata resolution, comment explicitly
says "emit symbol still comes from placeholder + local_emit_name"), 1 is a
field-COPY RHS (parser.cpp 35455, f->local_emit_name = src->local_emit_name in a
clone). None construct an emitted symbol. They were MASKED the whole set-wall
campaign because testsubscript kept `make` red; clearing it exposed them.
DECISION PENDING (user owns this drift invariant): refine the gate to its stated
intent (recommended — exempt lookup-key / debug-print / field-copy reads, e.g.
via a narrow `// drift-ok:<reason>` opt-out marker so new symbol-building sites
still fail) vs refactor the 6 benign sites. README/CHANGELOG/ROADMAP + the batch
push are HELD until this is resolved (true set-wall completion).
