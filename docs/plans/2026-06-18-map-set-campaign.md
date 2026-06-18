# map / set bring-up campaign — diagnosis & plan

**Status:** IN PROGRESS — std::tuple + A3.2 + multi-element packs + reference data
members done; Wall B (`__is_invocable`) underway — 4 layers fixed
(`integral_constant{}` fold; explicit-pack `_S_test` return type; **sub-wall 1
b0574ff** dependent-member-base forwarding traits real-instantiate; **sub-wall 3a
4911eae** partial specs real-instantiate in member-type context). The whole
forwarding+partial-spec chain now works; `__invoke_result` resolves its base — but
to `__failure_type`. NEXT = sub-wall 3b (Update 10): the `_S_test<F,Args...>(0)`
overload SELECTION picks the `(...)` failure overload because explicit args aren't
distributed into the success overload's trailing pack. map's `_S_key` still blocked;
contains (C) still remains (2026-06-18)

## Update 10 — sub-wall 3a LANDED (4911eae); 3b pinned to the `_S_test` overload SELECTION

**Sub-wall 3a DONE — commit `4911eae`, fulltest 641/5 zero regression.** While
resolving a member-type chain (allow/sticky) and only when a partial spec of the
name exists, the two opaque bails in instantiate_template_use are deferred so the
partial-spec match runs. `__result_of_impl<...>` now resolves to its partial spec
(`__result_of_impl<false,false,_Functor,_ArgTypes...>`) instead of an opaque
placeholder; its `::type` consumes cleanly (the type_traits:2583 parse derail is
gone). Verified via WB_DIAG probe: `__invoke_result<Cmp&,...>` now has a real
**base** — but it is **`__failure_type`** (should be `__result_of_success<bool>`),
so `__invoke_result::type` is still absent → `__is_invocable` folds false (gracefully).

**So the WHOLE forwarding + partial-spec machinery now works; only the FINAL
overload pick is wrong.** `__result_of_impl<...>::type =
decltype(_S_test<_Functor, _ArgTypes...>(0))` selects the FAILURE overload
(`template<typename...> static __failure_type _S_test(...)`) instead of the SUCCESS
overload (`template<typename _Fn, typename... _Args> static
__result_of_success<decltype(declval<_Fn>()(declval<_Args>()...)),__invoke_other>
_S_test(int)`). ROOT: explicit template args `<Cmp&, const int&, const int&>` (3)
are NOT distributed into the success overload's trailing `_Args` pack — its template
is `<_Fn, _Args...>` (2 typeparams, the 2nd a pack), and the arity gate rejects 3
explicit args, so the success overload is discarded and `_S_test(...)` (which takes
`<typename...>`) wins → return type `__failure_type`. This is the SAME explicit-pack
distribution class as aeec6a9, but in the OVERLOAD-SELECTION path, not the
return-type substitution path aeec6a9 fixed.

**Canonical instance of the gate: `resolve_fn_template_return_by_key`
(parser.cpp:~30400), line ~30432** — `explicit_args.size() > ft.typeparams.size()`
→ continue (rejects). The positional binding at ~30495 and the single-name
substitution at ~30504 likewise lack pack-awareness (port the pack-aware
binding+expansion already in `resolve_member_template_call_return_type` ~9019-9090:
fixed params singly + collect pack_elems + expand `pattern...` per element). NOTE
that fn is for FREE templates; `_S_test` is a MEMBER of the (inherited)
`__result_of_other_impl`, and the `[WB ovl]` probe in
`reselect_static_member_overload`'s body-less branch did NOT fire — so the exact
member-template-in-decltype selection site is a sibling not yet pinned to a line.
Next session: gdb-break where `__failure_type` is chosen for `_S_test` (set a
conditional probe in the method-overload selection / decltype-of-member-call path,
run tmp/invoc2.mad), then apply the pack-aware arity gate + binding + `pattern...`
expansion there (factor the aeec6a9 pack logic into a shared helper both call). Once
the success overload wins, `__result_of_impl::type = __result_of_success<bool>` →
`__invoke_result::type = bool` → `__void_t` true → `__is_invocable` true → `_S_key`
registers → map (static_assert) + set (insert overload res) advance.

Reducer `tmp/invoc2.mad` (folds false gracefully now). Probes to re-add: WB block in
`eval_void_t_detection_slot` (dump base_class/bases/aliases + resolve walk) and the
`[WB inst]` line at instantiate_template_use's pack_real_inst.

**3b SYMPTOM SITE PINNED (gdb, verified this session).** A conditional breakpoint
(`alias=="type" && dd->name=="__failure_type"`) caught the exact write:
`__result_of_impl_0_0_Cmp__int32_t__int32_t_`'s `type` alias is set to
**`__failure_type`** at **`TokenTYPEDEF::parse` parser.cpp:25540** (`record_typedef`),
with `base_dd` resolved just above via `resolve_declared_type_token(tn,true,true)` at
**~25316** (the typedef's type spec is `decltype(_S_test<_Functor,_ArgTypes...>(0))`).
Backtrace confirms the full chain: `__is_invocable_impl` → `__invoke_result`
(instantiate_template_use) → base `__result_of_impl<...>::type`
(resolve_declared_type_token ~4423) → `__result_of_impl` instantiates → TokenCLASS
member loop → this typedef. So `decltype(_S_test<Cmp&,const int&,const int&>(0))`
resolves to `__failure_type` (the `_S_test(...)` failure overload) instead of
`__result_of_success<bool>` (the `_S_test(int)` success overload).
SURPRISE / still-open: the `decltype`-in-type branch of resolve_declared_type_token
(parser.cpp ~4390/4411, `dd=expr->datadef()`) fires only ONCE in the whole run and
for a `void*` — NOT for this `_S_test` decltype. So `resolve_declared_type_token`
(called at 25316) resolves this `decltype(_S_test<...>(0))` by some OTHER sub-path
(NOT the ~4390 parseExpression branch). NEXT-SESSION (exact recipe): gdb-break
`parser.cpp:25316` conditioned on parsing `__result_of_impl`'s typedef (or break 25540
on the `__failure_type` condition, then step DOWN/`step` into the 25316
`resolve_declared_type_token` call) to find which sub-path resolves the `_S_test`
decltype to `__failure_type`; that sub-path is where the `_S_test(int)`-vs-`(...)`
overload selection happens and where explicit-pack distribution must be applied so
the success overload wins. Confirmed RULED OUT: reselect_static_member_overload,
resolve_member_template_call_return_type ([WB rsm]/[WB rmt] silent), and the
~4390 decltype-in-type branch (only void* once).

**LEAD (from READING TokenTYPEDEF::parse, verify next session):** that function has
NO decltype-parse branch — `base_dd` is set ONLY at parser.cpp:25276 (`tn` is a
`ttDataType`) or 25288 (contextual identifier), each via
`resolve_declared_type_token`. Since the decltype-in-type branch (~4390) never fires
for `_S_test`, the `decltype(_S_test<...>(0))` operand was almost certainly
**pre-resolved to `__failure_type` during INSTANTIATION CLONING** (the wrong
`_S_test` overload chosen at substitution time), so by the time the typedef is
parsed, `tn` arrives as an already-resolved `ttDataType(__failure_type)`. If so, the
3b fix is NOT in TokenTYPEDEF::parse at all — it is in the instantiation-time
decltype / member-type resolution during `__result_of_impl`'s body clone (where
`decltype(_S_test<F,Args...>(0))` is evaluated and the `_S_test(int)`-vs-`(...)`
overload picked). VERIFY: gdb-break parser.cpp:25276 (or 25540 cond __failure_type)
and inspect whether `tn` is a `ttDataType` named `__failure_type` (→ clone-time
pre-resolution confirmed) vs a raw `decltype` token; then grep/trace the
instantiation clone path (instantiate_template_use body loop + any decltype/
member-type pre-resolution it triggers) for where the `_S_test` overload is selected,
and apply explicit-pack distribution so the success overload wins.

**(earlier partial-narrowing notes — some superseded by the pin above)**
- CONFIRMED: the typedef `typedef decltype(_S_test<_Functor,_ArgTypes...>(0)) type;`
  resolves through `TokenTYPEDEF::parse` → `resolve_declared_type_token` decltype
  branch (parser.cpp ~4390: `++unevaluated_operand_depth; parseExpression(operand,
  true)`); `expr->datadef()` (~4411) is the resulting type = `__failure_type`.
- CONFIRMED (probes did not fire on tmp/invoc2.mad): the `_S_test` overload is NOT
  resolved via `reselect_static_member_overload` NOR
  `resolve_member_template_call_return_type` (`[WB rsm]`/`[WB rmt]` silent).
- NOT YET VERIFIED (earlier note over-claimed this): whether `_S_test` reaches the
  unqualified-static fallback at parser.cpp ~16842. The conditional gdb breakpoint
  there (cond `ident_tb->str=="_S_test"`) was set but the run was stopped at an
  UNRELATED earlier decltype typedef before reaching `__result_of_impl`'s typedef —
  so 16842 is neither confirmed nor ruled out. Re-run gdb with ONLY the 16842
  (cond _S_test) + the 4405 breakpoints and `continue` through the unrelated
  type_traits typedefs until the `__result_of_impl` one, to settle it.
3b fix (once the binding line is pinned): wherever parseExpression binds the
unqualified in-class member-FUNCTION-TEMPLATE `_S_test` call with explicit template
args + a trailing pack, it must distribute the explicit args into the pack and
prefer the success overload (or, if the success overload IS selected, its
return-type decltype `declval<_Fn>()(declval<_Args>()...)` must expand the pack to N
args). Method: gdb-break parser.cpp ~4411 (`dd=expr->datadef()`), `continue` past
the unrelated decltype typedefs until `dd` is `__failure_type` (or step into
parseExpression from 4405 for the `__result_of_impl` typedef), to pin the exact
member-template-call binding line; then apply the pack-aware distribution+expansion
(factor the aeec6a9 pack logic into a shared helper).

## Update 9 — Wall B sub-wall 1 LANDED (b0574ff); sub-wall 3 pinned at code level

**Sub-wall 1 DONE — commit `b0574ff`, fulltest 641/5 zero regression.** Variadic
forwarding traits whose meaning flows through a dependent-member base-specifier
(`__invoke_result : __result_of_impl<...>::type`, `__is_invocable :
__is_invocable_impl<...>::type`) now real-instantiate. Mechanism: a STICKY variant
of `allow_variadic_real_inst` (`variadic_real_inst_sticky`) that survives the
per-entry clear at instantiate_template_use and propagates real-instantiation
through the forwarding chain, turned on ONLY for templates whose base IS a
`...>::member` (`template_base_is_dependent_member` — cleanly excludes the
recursive `tuple : _Tuple_impl<...>` / `__and_ : conditional_t<...>` bases), bounded
by a `variadic_inst_in_progress` cycle guard (RAII around the class re-parse).
Verified via gdb on the real chain: `__is_invocable` → base `__is_invocable_impl<
__invoke_result<...>, void>::type` → arg `__invoke_result<Cmp&,...>` now
real-instantiates (frame 4) instead of going opaque.

**NEXT = sub-wall 3, pinned to ONE line.** `__invoke_result`'s body re-parse now
throws "Expecting identifier after type" at type_traits:2583 (`>::type`). gdb:
`parseDeclaration` is reached with the offending token = **`tkNS` (`::`)** — the
`::type` of `__invoke_result`'s base `: public __result_of_impl<...>::type` was
left UNCONSUMED, so it derails the class re-parse (TokenCLASS::parse takes the
"tag already a known type → parseDeclaration" branch at parser.cpp:~23033, then
chokes on the leading `::`). ROOT: `__result_of_impl<...>` resolves to an OPAQUE
placeholder with no `type` member, so `resolve_declared_type_token` can't consume
`::type`.

WHY `__result_of_impl` goes opaque (the exact code site): its PRIMARY
`template<bool,bool,typename,typename...> struct __result_of_impl;` has NON-TYPE
(`bool`) params, so `template_pack_real_instantiable` returns false ("every
parameter must be a type parameter"), and instantiate_template_use bails to
`instantiate_opaque_template_use` at **parser.cpp:~2970**
(`template_has_parameter_pack && !pack_real_inst`) — BEFORE the partial-spec match
at parser.cpp:~3217 can run. So it never reaches its partial spec
`__result_of_impl<false,false,_Functor,_ArgTypes...>` (whose own params ARE
type+pack and IS real-instantiable) and never computes that spec's
`typedef decltype(_S_test<_Functor,_ArgTypes...>(0)) type;`.

Sub-wall 3 fix (two parts, each its own commit + fulltest, sticky-scoped):
- **3a — reach the partial spec:** under sticky (member-type context), do NOT bail
  to opaque at ~2970 solely because the PRIMARY has non-type params; let the
  partial-spec match at ~3217 run, and if a matched spec is itself
  real-instantiable (type params + trailing pack, the bools concrete in the
  pattern), proceed to real body parse. Scope tightly (sticky only) to protect
  testsstream / the trait fold paths.
- **3b — compute the member typedef:** the matched spec body's
  `typedef decltype(_S_test<_Functor, _ArgTypes...>(0)) type;` needs the
  explicit-pack expansion inside a CLASS-template member typedef + the
  `_S_test(int)` / `_S_test(...)` int-vs-ellipsis SFINAE overload selection
  (sibling of the aeec6a9 return-type fix, on the member-typedef path). Yields
  `__result_of_success<bool>` → `__success_type<bool>` → `type=bool`.
Then `__invoke_result` inherits `type=bool`; `__void_t` detection succeeds;
`__is_invocable` true; `_S_key` registers; map (static_assert) + set (insert
overload resolution) advance. Reducer: `tmp/invoc2.mad` (re-add the fprintf probes
in eval_void_t_detection_slot + instantiate_template_use to watch each layer flip);
gdb breakpoint at the parseDeclaration throw confirms the `::`-derail.

## Update 8 — Wall B ROOT CAUSE confirmed ON THE REAL CHAIN (instrumented, not reduced)

Per the Update-7 method warning, this session instrumented the REAL libstdc++
`__is_invocable` chain (env-gated fprintf, run on `tmp/invoc2.mad`) instead of
building synthetic reducers. The exact failing layer is now pinned:

**`__invoke_result<Cmp&, const int&, const int&>` instantiates as an EMPTY OPAQUE
SHELL** — `base_class=(none), bases=0, aliases=0`. Its dependent-member
base-specifier `: public __result_of_impl<...>::type` is never resolved, so it
never inherits `type=bool`. Therefore the `__void_t<typename _Result::type>`
detection slot in `__is_invocable_impl<_Result, void, true, __void_t<...>>` finds
`_Result::type` **ABSENT** → the true_type partial spec is rejected → the primary
`false_type` is selected → `__is_invocable` folds **false**. (`__is_invocable`
ITSELF real-instantiates fine; the empty arg `_Result` is the break.)

Instrumentation that produced this (both removed; tree clean):
- in `eval_void_t_detection_slot` (parser.cpp ~14595), the `_Result::type` walk:
  dumped `pit->second` name + its `base_class`/`bases`/`type_aliases`, and each
  `resolve_class_type_alias(cur, seg)` result. Output:
  `base=__invoke_result_Cmp__int32_t__int32_t_ … bases=0 aliases=0 … seg 'type' -> (ABSENT)`.
- in `instantiate_template_use` (parser.cpp ~2929): `pack_real_inst` per template.
  Output: `__is_invocable pack_real_inst=1`, **`__invoke_result pack_real_inst=0`**.

WHY `__invoke_result` goes opaque: `template_pack_real_instantiable(__invoke_result)`
returns TRUE (body non-empty — `td.body` holds the whole `class … : base { }`; pack
is the last param; all params are type params). The block is that
`allow_variadic_real_inst` is **cleared at parser.cpp:2931** before the NESTED
arg-instantiation of `__invoke_result` (it is instantiated as the template ARG
`_Result` of `__is_invocable_impl`, deep inside the outer `__is_invocable` base
resolution, where the flag is already off).

DIRECTION CONFIRMED + NEXT WALL EXPOSED: forcing the flag on for `__invoke_result`
(throwaway `WB_FORCE` hack on `tname=="__invoke_result"`) makes it real-instantiate
and pull `__result_of_impl` — proving base resolution is the right lever — but
exposes the NEXT layer: the dependent-member base-specifier
`__result_of_impl< is_member_object_pointer<…>::value,
is_member_function_pointer<…>::value, _Functor, _ArgTypes... >::type` fails to
parse/resolve in the instantiation RE-PARSE path (cascades to a param-declarator
throw at parser.cpp:32731, surfaced at type_traits:2583 `>::type`). NB
`tmp/basequal.mad` (`struct D : holder<Base>::type`) parses fine at TOP level — the
gap is the combination: partial-spec template-id + non-type `::value` trait args +
trailing pack + the instantiation re-parse path.

### ORDERED sub-walls for next session (each its own commit + fulltest, A1-style scoping)
1. **Scoped real-instantiation of `__invoke_result`-shaped templates** — variadic,
   value carried by a dependent-member BASE-specifier (not body members). The
   capability already exists (`template_pack_real_instantiable` is true); the fix is
   to let `allow_variadic_real_inst` reach this nested arg-instantiation in a
   member-type-resolution context ONLY (scope like A1 `f40ba5e` so a pure trait /
   `testsstream` does NOT regress — a blanket flag-drop regressed it before).
2. **Dependent-member base-specifier resolution** in the instantiation re-parse:
   `PartialSpecTemplate< nontype::value, nontype::value, Type, Pack... >::type` as a
   base clause. Compute the two non-type bool args (member-pointer-trait `::value`),
   match the partial spec, take `::type`.
3. **`__result_of_impl<false,false,_Functor,_ArgTypes...>` partial-spec member
   typedef** `typedef decltype(_S_test<_Functor, _ArgTypes...>(0)) type;` — the
   explicit-pack expansion inside a CLASS-template member typedef (sibling of the
   aeec6a9 return-type fix, on the class-member-typedef path). Yields
   `__result_of_success<bool>` → `__success_type<bool>` → `type=bool`.
Then `__invoke_result` inherits `type=bool`; `__void_t` detection succeeds;
`__is_invocable` true; `_S_key` registers; both map (static_assert) and set
(insert overload resolution) advance. Reducer (REAL chain, do NOT synthesize):
`tmp/invoc2.mad` — re-add the two fprintf probes above to watch each layer flip.

## Update 7 — Wall B: 2 layers fixed; remaining layers mapped

`__is_invocable` is a DEEP NESTED trait; this session fixed two of its layers
(each general + tested + zero-regression):
- **`integral_constant{}` fold** (659752e) — `Trait{}`→`value` in constant context.
- **explicit-pack `_S_test` return type** (aeec6a9) — explicit template args fill a
  trailing pack; `pattern...` in a member-template return type expands per element.
  `resolve_member_template_call_return_type`. Test `testexplicitpack.mad`. 641/5.

`__is_invocable<Cmp&,...>{}` still folds to **false** → map's `_S_key` still
"undeclared". REMAINING `__is_invocable` layers (each a distinct sub-wall, work
in-to-out from the reducers in `tmp/`):
1. **`__result_of_impl` member-typedef pack expansion** — `template<typename _Fn,
   typename..._ArgTypes> struct __result_of_impl<false,false,...> { typedef
   decltype(_S_test<_Functor,_ArgTypes...>(0)) type; };`. The pack `_ArgTypes...`
   must expand inside the `_S_test<...>` template-id of a CLASS-template member
   typedef (the clone-loop / class-instantiation path — distinct from the
   return-type path just fixed). Reducer: build `tmp/resimpl.mad` faithfully with a
   `typedef decltype(tester::test<Fn,Args...>(0)) type;` inside a class template
   (NOT `using R = X<...>::type`, which hits a separate using-alias `::member`
   parse gap — a reducer artifact, NOT on the real chain which uses typedef +
   `__void_t<typename _Result::type>` detection).
2. **`__is_invocable_impl` `__void_t` selection** — `__is_invocable_impl<_Result,
   void, true, __void_t<typename _Result::type>> : true_type`. madc HAS `__void_t`
   detection (iterator_traits); verify it selects the true_type partial spec once
   `_Result::type` (from layer 1) exists.
3. **`__invoke_result`/`__is_invocable` wrappers** — thin inheriting structs; should
   fall out once 1+2 work.
Then `_S_key`'s static_assert is true → `_S_key` registers → both map (static_
assert) and set::insert (overload resolution) advance. After Wall B: map's
_Rb_tree insert chain (_M_insert_node/node alloc/_Construct) + Wall C (contains/
C++20 ranges).

## Update 5 — reference data members DONE (commit bd9918d)

Two reference-data-member bugs (both hit by `_Rb_tree::_Auto_node`'s
`_Rb_tree& _M_t` member, reproduced by minimal non-template reducers):
- **Access**: member/method access through a reference member threw "member
  reference is not a structure or union" — `parseExpr_identifierArm`'s ttMember
  branch didn't unwrap a reference receiver before the is_struct/is_object check
  (the ttVariable branch already did). Fixed: mirror the unwrap in ttMember.
- **Init**: initializing a reference member emitted "incompatible types in
  assignment to a pointer" — `emit_member_inits` lowered `m(e)` as
  `m = translate_expr(e)`, but a reference member binds to its initializer's
  ADDRESS (`m = &e`). Fixed: wrap the init in N_ADDR for reference members.
New test `tests/testrefmember.mad`. fulltest 639/5, zero regr. map advanced past
both _Auto_node walls.

### NEXT WALL = Wall B (`__is_invocable` FOLD), CONFIRMED shared map+set
`tmp/map_min2.mad` fails "use of undeclared identifier '_S_key'" — downstream of
`_Rb_tree::_S_key` NOT registering, because its body holds
`static_assert(__is_invocable<_Compare&, const _Key&, const _Key&>{}, ...)`
(stl_tree.h:764) and madc cannot FOLD `__is_invocable` to a constant.

Sharply isolated (next-session start): reducer **`tmp/invoc2.mad`** — just
`#include <type_traits>` + a user functor `struct Cmp { bool operator()(const
int&, const int&) const; };` + `static_assert(__is_invocable<Cmp&, const int&,
const int&>{}, ...)` — fails **"Expecting integer constant expression"** at the
`{}`. `<type_traits>` parses fine; the wall is purely the trait FOLD.
- `__is_invocable<_Fn, _Args...> : __is_invocable_impl<__invoke_result<_Fn,
  _Args...>, void>::type` (type_traits:2989). True iff
  `__invoke_result<_Fn,_Args...>::type` EXISTS (the `__void_t` detection picks the
  true_type partial spec at type_traits:2934). madc HAS `__void_t` detection
  (iterator_traits) — the missing piece is `__invoke_result` computing `::type`,
  i.e. decltype of the INVOKE expression `declval<_Fn>()(declval<_Args>()...)` via
  overload resolution on the functor's `operator()`. That overload-resolution-as-
  SFINAE is the deep core (comparable to the vector<T*> `__construct_helper`
  engine); a multi-step effort of its own.
- `_S_key` is used all over the insert/compare path AND set::insert's overload
  resolution needs the same trait → ONE shared blocker for both containers.

NOT on the critical path (a `<functional>` artifact, do NOT chase for map):
`tmp/invoc.mad` (which `#include <functional>`) fails earlier on a
pointer-to-member-function member declarator `void (_Undefined_class::*
_M_member_pointer)()` in std::function's `_Nocopy_types` (std_function.h:80) —
TokenSTRUCT::parse handles `T (*p)()` but not `T (C::*p)()`. A real general
parser gap (worth fixing for std::function someday) but map's `_S_key` path does
NOT pull std_function, so it is NOT what blocks map. Use `tmp/invoc2.mad` (no
`<functional>`) as the Wall-B reducer.

After Wall B: map's remaining _Rb_tree insert chain (_M_insert_node body, node
alloc, _Construct) + Wall C (`contains`, C++20/ranges).

## Update 6 — `integral_constant{}` fold DONE (commit 659752e); Wall B core pinned

**DONE:** `Trait{}`/`Trait()` value-init in a constant expression now folds to the
trait's static `value` (the integral_constant conversion). Was: only `Trait::value`
folded; `static_assert(is_same<int,int>{})` / `true_type{}` threw "Expecting integer
constant expression". Fix in `fold_constant_qualified_member` (after it resolves the
leading type/template-id to a scope class): empty `{}`/`()` + a (possibly inherited)
static `value` member → fold to it. Test `tests/testtraitvalue.mad`. fulltest 640/5.

This was the FIRST half of Wall B. `__is_invocable<...>{}` now FOLDS — but to
**false**, because `__invoke_result<F,Args>::type` isn't computed, so map's
`_S_key` static_assert fails (still "undeclared _S_key" downstream).

### Wall B DEEP CORE (next-session start) — `_S_test(int)`/`_S_test(...)` SFINAE
madc CAN already evaluate `decltype(declval<Cmp&>()(declval<const int&>(),
declval<const int&>()))` → `bool` (reducer tmp/declcall.mad passes). The gap is one
layer up — `__result_of_other_impl` (type_traits:2554) selects the result via an
int-vs-ellipsis SFINAE overload pair:
```
template<typename _Fn, typename..._Args>
  static __result_of_success<decltype(declval<_Fn>()(declval<_Args>()...)),…> _S_test(int);
template<typename...> static __failure_type _S_test(...);
// __invoke_result::type = decltype(_S_test<_Functor,_ArgTypes...>(0))
```
**Minimal reducer `tmp/sfinae_ovl.mad`** (a `success<decltype(...)> test(int)` /
`failure test(...)` pair + `decltype(test<Cmp&,const int&,const int&>(0))`) fails
**"Incorrect number of parameters: expected 3 got 2"** — the SAME error the plan
flagged for set::insert, confirming this overload-resolution gap is the shared
map+set core. madc mishandles an explicit-template-arg variadic static-member call
(`test<F, A...>(0)`: F + a 2-element EXPLICIT pack, one `(int)` function param)
against the `(...)` ellipsis fallback — it conflates the 3 explicit template args
with function-param arity. Fixing this (explicit-template-arg variadic call + int-
vs-ellipsis SFINAE tiebreak returning success/failure types) makes `__invoke_result`
compute `::type`, hence `__is_invocable` TRUE, hence _S_key registers and both map
(static_assert) and set::insert (overload resolution) advance. Deep overload-
resolution feature; its own focused effort. (Note: distinct from the deduced-pack
work already done — here the pack is EXPLICIT in the template-id.)

#### EXACT MECHANISM (traced via SF-DIAG fprintf at the throw, parser.cpp:12653)
The "expected 3 got 2" is thrown on **`Cmp__operator()`** (nparams=3 = hidden
`this` + 2 declared, argc=**1**) — NOT on `test`. So `test<Cmp&, const int&, const
int&>`'s return-type expression `success<decltype(declval<F>()(declval<A>()...))>`
instantiated with the EXPLICIT pack `A...={const int&, const int&}`, but
`declval<A>()...` expanded to only **ONE** argument, so the synthesized call
`declval<Cmp&>()(declval<const int&>())` hit `Cmp::operator()` with 1 arg vs 2.
ROOT: **explicit template arguments that fill a trailing parameter pack are not
distributed to the pack.** `test<Cmp&, const int&, const int&>` binds `F=Cmp&` but
leaves `A` with ≤1 element. (`try_instantiate_namespace_fn_template`'s explicit-arg
block at parser.cpp:~28509 even REJECTS `explicit_template_args.size() > nonpack`
at 28507 — but `test` here is resolved via the decltype return-type path, NOT
try_instantiate, so the fix must reach wherever explicit template args bind to a
function template's params for return-type/signature computation —
`resolve_member_template_call_return_type` (parser.cpp:8897) and the explicit-
template-id instantiation, distributing args beyond the leading non-pack params
into the pack — mirroring the DEDUCED-pack `pack_elems` distribution already done
for call args). This is the single concrete fix that unblocks `__invoke_result` →
`__is_invocable` → both containers. Reducer: tmp/sfinae_ovl.mad (15 lines).

---

## Update 4 — multi-element parameter packs DONE (commit 1bbb5b8)

Variadic member function templates with a deduced **N≥2** parameter pack now
monomorphize. Previously `try_instantiate_namespace_fn_template` only supported a
single-element trailing pack (arg-count guard at parser.cpp:28496 rejected N≥2),
so `_M_emplace_hint_unique(__pos, piecewise_construct, tuple<const key&>,
tuple<>)` emitted an undeclared symbol → c2mir "incompatible types in assignment
to struct/union".
Fix: collect every pack-absorbed argument's type into a `pack_elems` vector,
thread it into `instantiate_fn_template_binding`, and a dedicated N-element
builder expands the pack DECLARATION `_Args [&|&&]... __args` → N typed params and
every `pattern...` expansion → N comma-separated copies (pack type name → element
type, value name → `__args__k`; pattern extent found by backward scan to the
nearest arg delimiter). Handles bare (`args...`), forwarded
(`std::forward<Args>(args)...`), and leading-fixed-param (`f(first, rest...)`)
shapes. New test `tests/testvariadicfn.mad` (byval=6, fwd=60, lead=10).
fulltest 638/5, zero regr.

### NEXT WALL (precise, next-session start point): `_Auto_node::_M_insert`
`tmp/map_min2.mad` now advances PAST emplace to a new parse-time error: "member
reference is not a structure or union" (parser.cpp:15640, `parseExpr_identifierArm`),
reported at the inherited clone position stl_tree.h:427. Traced via gdb on the
LIVE failing call: the deferred body being parsed is
`_Rb_tree<…>::_Auto_node::_M_insert` (the C++17 nested RAII insert helper). The
failing access is `__t._M_insert_node(...)` where the receiver `__t` (a
`_Rb_tree&` member of `_Auto_node`) resolves to a **dtRESERVEDptr (10255)
placeholder** — a pointer to an incomplete/dependent `_Rb_tree`, not the complete
instantiated class — so `is_struct()`/`is_object()` both fail.
Next-wall task: when instantiating a NESTED helper class (`_Auto_node`) inside an
instantiated outer class template (`_Rb_tree<…>`), its member that references the
enclosing type (`_Rb_tree& __t`) must bind to the COMPLETE instantiated outer
class, not a dtRESERVEDptr placeholder. Investigate how `_Auto_node`'s `__t`
member type is resolved during the outer instantiation (likely the enclosing-
class back-reference isn't mapped to the concrete instantiation). Then the rest
of the `_Rb_tree` insert chain (`_M_insert_node` body, node alloc, `_Construct`)
likely each surface further sub-walls — this is a long internal tail.

---

## Update 3 — A3.2 DONE (commit 32d7ccb)

**A3.2** `32d7ccb` — pack-expansion `...` in EXPRESSION context. The orphaned
`...` was NOT in the clone loop (the base-clause/typedef `_Elements...` ARE
consumed there). gdb on the live failing call (not the inherited clone position)
showed it is in the LAZY-INSTANTIATED tuple ctor's mem-initializer list:
`parse_deferred_lazy_body → parseFunction → parse_ctor_initializer_list`, parsing
`_Inherited(std::forward<_UElements>(__elements)...)`. The deduced parameter pack
is materialized as concrete param(s) the pattern already names, but the trailing
`...` survived; `parseExpression` (conditional) ran the completed operand into the
`...` as member-access `.` → "Missing operand".
Fix (deepest correct layer): in `parseExpression`'s post-operand peek, recognize a
pack-expansion `...` (three dots, via existing `consume_ellipsis()`) — distinct
from a single `.` member access — consume it and end the expression. General to
every expression-context pack expansion. `tmp/tup_ref.mad`/`tmp/tup_direct.mad`
compile+run; fulltest **637/5/0/18**, zero regressions.

### NEXT WALL (precise, next-session start point): map `_M_emplace_hint_unique`
`tmp/map_min2.mad` now advances PAST the incomplete-struct tuple wall to c2mir
CHECK errors: "incompatible types in assignment to struct/union" + "incomplete
struct or union" at the `operator[]` emplace site. Root cause (traced via
`--emit=c11`): `_M_emplace_hint_unique` appears ONLY at the call site in the
emitted C — never declared, never defined. It is a **variadic member function
template** (`stl_tree.h:1086` `template<typename... _Args> iterator
_M_emplace_hint_unique(const_iterator, _Args&&...)`). madc does not instantiate
it for the deduced `_Args = {piecewise_construct_t, tuple<const key_type&>,
tuple<>}`, so the call emits as an undeclared (implicit-int) function → `__i =
<int>` assigned to the struct iterator `__i` → "incompatible types in assignment
to struct/union".
The fix is **variadic member-FUNCTION-template instantiation** (sibling to the
variadic CLASS-template work A1–A3, and to Wall B's `__is_invocable`): deduce the
pack from the call args, instantiate + emit the member body (`stl_tree.h:2459+`),
which then pulls in `_M_get_insert_hint_unique_pos` / `_M_insert_` / node alloc /
`_Construct` with `piecewise_construct` pair construction. Large; genuinely the
plan's "emplace/piecewise codegen" sub-wall. Reducer `tmp/map_min2.mad`,
`--emit=c11` to `tmp/map_min2.c` shows the bare call at the `__i = ..._M_emplace_
hint_unique(&__this->_M_t, __i, piecewise_construct, __madc_objtmp_6,
__madc_objtmp_7)` line.

#### PRECISE ROOT CAUSE (traced 2026-06-18, MFI-DIAG instrumentation)
The emplace call DOES route correctly: `parseExpression`→ method-call resolution →
`instantiate_member_fn_template_for_call` (parser.cpp:29776). It PASSES every entry
guard — fd `is_member_template=1`, `member_template_decl` non-empty,
`member_template_owner` set, `template_param_names`=1 (`_Args`), owner `_Rb_tree<…>`
NOT externally-defined (ext=0, extinst=0). It then calls
`try_instantiate_namespace_fn_template` (parser.cpp:28459) with `typeparams=1,
pack0=1` — and that **returns false**.

`try_instantiate_namespace_fn_template` only supports a trailing parameter pack
bound to **exactly ONE element** (its own comment, 28463-28464: "one parameter
pack allowed in the LAST position (bound to exactly one element — `__stoa`'s
`_Base...` shape)"). The emplace call passes a **3-element** pack
(`piecewise_construct_t, tuple<const key_type&>, tuple<>`), so the guard at
**parser.cpp:28496** `if (tc->parameters.size() > ov.param_spellings.size())
return false;` (4 call args > 2 param spellings `[__pos, _Args&&...__args]`)
rejects it immediately. The single-element pack binding (28538-28551) likewise
binds the pack to ONE remaining arg.

So the real next-wall task is **multi-element (N≥2) trailing parameter packs** in
member-function-template deduction + instantiation:
1. parser.cpp:28496 — let a trailing pack absorb `args.size() - (nonpack params)`
   arguments instead of rejecting arg-count > spelling-count.
2. parser.cpp:28538-28551 — bind the pack to the VECTOR of remaining arg types
   (not one), like the class-template `pack_subst` does.
3. Body instantiation — expand each `pattern...` (in args AND mem-init) to N
   copies with per-element substitution. NOTE: A3.2's `parseExpression`
   `consume_ellipsis()` only DROPS the `...` (correct for a size-1 collapsed
   pack); multi-element needs real repetition — the same N-element gap the clone
   loop has. Decide whether to expand at substitution time (preferred, mirrors
   `pack_subst` in `instantiate_template_use`) so the body parser sees N concrete
   args and no `...`.
Then the downstream `_M_get_insert_hint_unique_pos`/`_M_insert_`/node-alloc/
`piecewise` pair-construction chain (each likely its own sub-wall).

---


## Progress log (2026-06-18)

Real libstdc++ `std::tuple<T>` / `tuple<T1,T2>` now instantiate and run.
Commits on `feature/retire-embedded-shims-claude`, each fulltest-green:
- **A1** `f40ba5e` — concrete variadic templates real-instantiate as OBJECTS
  (scoped to statement-level var-decl sites; trait/value-fold stay opaque so
  `__or_` etc. don't force their `auto` base). test `testvariadicobj`.
- **A2.1** `bdcf504` — resolve forward-declared template-ids at decl sites
  (`template<...> class tuple;` registers both a datatype_map placeholder AND a
  template_map entry; prefer template-id resolution when `<` follows).
- **A2.2** `26656bd` — preserve the leftover `>` when the injected-class-name
  swallow hits `>>` (`_UseOtherCtor<_Tuple, tuple<_Up>>`). test extended.
- **A2.3** `b75a2f0` — single-element pack-expansion PATTERNS
  (`const _Elements&...`). test `testtuple`. Multi-element pattern repeat = gap.

fulltest **637 / 5 / 0 / 18** (the 5 below; +2 new passing tests, zero regr).

### Remaining walls per failing test (re-measured at b75a2f0)
- `testmap`, `testset` → **Wall C** (`contains`, `__cplusplus>201703L`) FIRST,
  then map needs A3 / set needs B.
- `testsubscript` → **Wall A3**: map `operator[]` builds the tuple as an
  EXPRESSION temporary (`tuple<const key_type&>(__k)`); the statement-decl flag
  scoping (A1) doesn't reach it, so it stays opaque → c2mir incomplete-struct.
  Then `_M_emplace_hint_unique` + `piecewise_construct` codegen.
- `testcontainerdtor`, `testmadc_ns` → **Wall B**: `__is_invocable` /
  `__invoke_result` SFINAE chain → bool (deep, like the vector<T*> trait
  engine). "Incorrect number of parameters: expected 3 got 2" pairs with it.

No single wall makes a test pass (each test needs 2+). Next: A3 (expression-
temporary object-context real-instantiation), then B, then C.

### Update 2 — A3.1 done, map chain advanced (a65d59d)
- **A3.1** `a65d59d` — real-instantiate variadic templates in CONSTRUCTION
  expressions (`ns::Tmpl<...>(args)`): parseExpr_identifierArm's ns-qualified
  template-id arm now peeks past the balanced `<...>` and, if a `(` follows
  (construction), sets the object-context flag. map's `operator[]` tuple
  temporary `std::tuple<const key_type&>(__k)` now real-instantiates (was opaque
  + key_type-textual). fulltest 637/5, zero regr. No standalone test (user-ns
  construction-expr hits a separate "not a member" wall; std::/map exercises it).
- map `operator[]` now advances PAST the c2mir incomplete-struct codegen wall to
  a deeper PARSE wall: instantiating `std::tuple<const key_type&>` (reference
  element). Reduced to `tmp/tup_ref.mad` (`tuple<const string&> t(s);` alone).

### A3.2 PRECISE DIAGNOSIS (next-session start point)
The error is **"Missing operand"** at the LEFT-operand branch of `popOperator`
(parser.cpp:~11929) — the failing operator is **`.` (id 35)**, i.e. an orphaned
pack-expansion **`...`** (three tkDot) reaching the EXPRESSION parser as
member-access `.` operators with no operand. Backtrace: `parse_namespace_block`
→ `TokenCppKeyword::parse` (an ignored decl-specifier, e.g. `constexpr`) →
`parseDeclaration` → `parseExpression` → `popOperator`. So a `constexpr <decl> =
<expr-with-orphaned-...>` in tuple's instantiation.
- The `...` is a pack expansion in a NESTED/value context (e.g.
  `__and_<X<_Elements>...>::value`, or a member-fn-template's own `_UElements...`)
  that the **class-body clone-loop pack substitution does NOT consume**.
- Two earlier diagnoses turned out to be RED HERRINGS: (a) an `integral_constant
  <bool, true>` non-type fold throwing in `parse_constant_primary` (caught
  internally — `true`/`false` lex as TokenCppKeyword, ttKeyword, not handled by
  the int-const folder; a real but INERT bug, fix had no observable effect);
  (b) a `_Ptr<...>` non-type fold in map's deeper chain. The ACTUAL blocker is
  the orphaned-`...`-in-expression above.
- A2.3's single-element fix (forward-scan from the pack NAME, class-body clone
  loop) does NOT reach this `...` — it sits past a closing `>` (negative relative
  depth) and/or in a different substitution path. A **flag-based** consume (arm
  when a single-element pack expands in place, drop the next `...`, reset at
  `;`/`{`/`}`) is more robust for nested patterns IN the clone loop, but did NOT
  fix tup_ref — confirming this `...` is in ANOTHER path (member-fn-template
  binding / a `<string>`-pulled construct). Find WHICH substitution emits the
  `...`: add the [A3-DIAG-L] fprintf at parser.cpp:~11929 (op id/char/pos) +
  `catch throw` gdb; then consume the `...` where that pack is substituted.
- Then still: `_M_emplace_hint_unique` + `piecewise_construct` codegen.

### Session-end state (2026-06-18)
6 code commits (A1, A2.1, A2.2, A2.3, A3.1) + 2 doc commits, all pushed, fulltest
**637/5/0/18**, zero regressions throughout. Real `std::tuple<T>`/`<T1,T2>`
instantiate+run. The 5 map/set/ns tests remain (each needs 2+ deep walls):
map → A3.2 (`_Ptr` non-type fold) + tuple-reference-element + emplace/piecewise;
set → B (`__is_invocable`); both map/set also → C (`contains` needs `ranges`).
This is a genuine multi-session campaign; the std::tuple foundation is the big
unlock done here.

---

**Original diagnosis (2026-06-18)**
**Branch:** feature/retire-embedded-shims-claude
**Goal (user contract):** get `std::map` and `std::set` working end-to-end —
`testmap`, `testset`, `testsubscript`, `testcontainerdtor` pass (and the
namespace test `testmadc_ns`).
**Baseline at diagnosis:** fulltest **635 / 5 / 0 / 18**. The 5 failures are
exactly these tests.

This is **not** a single bug — it is a multi-wall campaign comparable in size to
(or larger than) the `vector<T*>` effort, because `map::operator[]` depends on a
working `std::tuple`, which is itself broken in direct use.

---

## The walls (each independently blocks)

No single fix makes any test pass — the walls are layered. `testmap` needs
BOTH Wall A and Wall C; `testset` needs BOTH Wall B and Wall C.

### Wall A — `map::operator[]` requires a real `std::tuple` (deepest)
- Reducer: `tmp/map_min2.mad` (`map<string,int>; ages[key]=30;`).
- At HEAD: c2mir codegen error `stl_map.h:102: incomplete struct or union`.
- Root cause (traced via `--emit=c11` + gcc): the emitted `operator[]` body
  declares `struct tuple_constkey_type_ __madc_objtmp_6;` — i.e.
  `std::tuple<const key_type&>` was instantiated as an **opaque dependent
  placeholder** (`is_dependent_placeholder=true`, size 0, no members), so the
  local has no storage → "storage size isn't known" (gcc) / "incomplete struct"
  (c2mir). The C++11 `operator[]` path
  (`_M_emplace_hint_unique(__i, piecewise_construct, tuple<const key_type&>(__k),
  tuple<>())`) genuinely needs two **complete** tuple objects passed by value.
  The C++98 `#else` path (`insert(__i, value_type(__k, mapped_type()))`) avoids
  tuple but is gated out at `__cplusplus >= 201103L` (we present 201703L).
- Sub-wall A1 — **concrete variadic templates used as objects go opaque.**
  Minimal: `template<typename...Ts> struct V{int x;}; V<int> v; v.x=5;` →
  "Unidentified member 'x' in 'V_int'" (opaque shell has no members).
  Also: a member typedef used as a *variadic* template arg in a method body is
  NOT resolved — `Tup<const key_type&>` emits `Tup_constkey_type_` (key_type
  left textual), whereas the non-variadic path resolves it (`Box_const_int32_t_`).
  The opaque path (`instantiate_opaque_template_use`) collects arg spellings
  **textually** (`collect_template_argument_spelling`) and never resolves them;
  the real path uses `resolve_declared_type_token` + `template_type_arg_spelling`.
- Sub-wall A2 — **std::tuple's real body does not parse.** `tuple<int> t;` fails
  at HEAD with "Expecting identifier after type" at the `>` (template-id close
  not consumed). tuple = recursive variadic inheritance
  (`tuple<_Elements...> : _Tuple_impl<0,_Elements...>`) + heavily-SFINAE'd
  constrained constructors (`_TupleConstraints`, `__enable_if_t`,
  `_ImplicitDefaultCtor`) + explicit specs `tuple<>` and `tuple<_T1,_T2>`. When
  forced to real-instantiate, the body parse over-consumes the token stream
  ("Unexpected end of data" from `nextToken()` on an empty deque).

### Wall B — `set::insert` needs the `__is_invocable` SFINAE chain
- Reducer: `tmp/set_min.mad` (`set<string>; names.insert(a);`).
- `insert(lvalue string)` → "Incorrect number of parameters: expected 3 got 2"
  + "use of undeclared identifier '__is_invocable'" (stl_tree.h:427). Overload
  resolution among set::insert's overloads requires evaluating the
  `__is_invocable` / `__invoke_result` SFINAE constraints, which madc can't yet
  fold → wrong overload / undeclared identifier. This is the long-pending
  **Task #26** (Wall (f)).
- `insert(rvalue literal)` → MIR link error `undefined item …insert__o5` (the
  selected overload's body is declared but never emitted).

### Wall C — `contains` is C++20-gated (config / std-bar)
- `map::contains` / `set::contains` live under `#if __cplusplus > 201703L`
  (`stl_map.h:1284`). madc's default `STD_MADC` mode presents
  `default_cpp_std = STD_CPP17` → `__cplusplus = 201703L` → the region
  preprocesses away → "Unidentified member 'contains'".
- The bar deliberately LAGS (raising it pulls in the C++20 ranges/concepts
  header surface). This is **Task #33** (two-axis model: a language bar
  `__cplusplus` + a per-feature `__cpp_lib_*` registry). `contains` is purely
  `__cplusplus`-gated, so the two-axis split alone does not expose it — raising
  the language bar to C++20 (and absorbing the ranges/concepts parse cost) is
  what exposes it.

---

## Why the obvious one-line fix does NOT work (measured)

Dropping the `allow_variadic_real_inst` gate in `instantiate_template_use`
(line ~2914) so *shape alone* (`template_pack_real_instantiable`) drives
real-instantiation:
- FIXES the trivial `V<int> v; v.x` object case (Sub-wall A1).
- Does NOT fix any of the 5 map/set tests — their deeper walls (A2/B/C) remain.
- REGRESSES `testsstream` → fulltest **634 / 6** (net −1).

So the flag-drop is necessary-but-insufficient for Wall A AND has a cost. The
real fix must real-instantiate concrete variadic-as-object types **without**
disturbing the opaque-shell contexts that currently-passing tests rely on
(testsstream), AND must make std::tuple's body actually parse (A2).

Diagnostic aid noted: bare `throw "string"` sites (e.g. `nextToken()`'s
"Unexpected end of data", `include/madc.h:2122`) are caught by `madc_cir.cpp`'s
`catch(...)` and reported as the opaque "compile error in instantiated template
body". A `catch(const char*)` arm there surfaces the real message — worth adding
as a standalone diagnostics improvement.

---

## Proposed sequencing (deepest-first, each its own commit + fulltest)

1. **Wall A1 (contained):** resolve type args in the opaque path AND
   real-instantiate concrete variadic-as-object types only where a complete type
   is required — scoped narrowly so testsstream does not regress. Reducer:
   `tmp/var_obj.mad`.
2. **Wall A2:** make `std::tuple` real-instantiate (body parse of the
   recursive/SFINAE'd ctors + the `tuple<>`/`tuple<_T1,_T2>` specs). Biggest
   piece; likely several sub-fixes like the vector<T*> SFINAE work.
3. **Wall B:** `__is_invocable` / `__invoke_result` SFINAE → bool (Task #26),
   then set::insert overload resolution + body emission.
4. **Wall C:** raise the default C++ bar to C++20 (Task #33) so `contains`
   compiles — do LAST, after the ranges/concepts parse surface is affordable.

Reducers live in `tmp/`: `map_min2.mad`, `set_min.mad`, `var_obj.mad`,
`memtypedef2.mad` (variadic member-typedef arg), `tup_direct.mad`.
