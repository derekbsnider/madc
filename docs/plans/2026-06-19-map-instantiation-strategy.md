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
