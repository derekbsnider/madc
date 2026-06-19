# map / set bring-up campaign — diagnosis & plan

## Update 29 — `_M_at_eof` wall FIXED (friend → private METHOD); Wall B (`_S_key`/`__is_invocable`) ROOT PINNED = forwarded class-template pack drops

**TWO commits this session.** (1) `6a029b8` C++17 init-statement (Update 28).
(2) `58ee962` **friend functions may call private METHODS** — `method_access_violation`
never received the current function's friend name, so the friend-function grant
(`function_is_friend_of`/`friend_function_names`) was checked only for DATA-member
access; a `friend` free fn calling a private METHOD was wrongly rejected. Fix: add
`cur_function` param to `method_access_violation`, pass
`current_function_friend_name(code)` at both the `.` and `->` method-call sites.
Cleared libstdc++ `istreambuf_iterator`'s C++20 `friend operator==` →
`__i._M_at_eof()` wall. Test tests/testfriendmethod.mad. fulltest 653/5/0/18.

**testmap --std=c++20 now: only `_S_key` (Wall B) remains** (+ recovered cosmetic
`:141 undeclared std` ×14, `:791/:817 Expecting integer constant expression` ×3).

**WALL B ROOT CAUSE PINNED (verify-over-stale-handoff — Update 10's diagnosis is
STALE; the minimal `_S_test` overload-selection reducer tmp/stest2.mad now PASSES,
11+ parser fixes since).** The CURRENT real-chain failure (MADC_DBG_WB probe in
`eval_void_t_detection_slot`, removed): `__invoke_result<Cmp&,const int&,const int&>`
is COMPLETE (not an empty shell — Update 9 held) but its **base = `__failure_type`**
(should be `__result_of_success<bool>`). So `__result_of_impl<false,false,Cmp&,…>::type`
= `decltype(_S_test<_Functor, _ArgTypes...>(0))` picked the FAILURE overload —
because the forwarded class-template pack `_ArgTypes...` does NOT expand into the
inner `_S_test<…>` call, so the success overload `decltype(declval<Cmp&>()(
declval<_Args>()...))` sees 0 args → ill-formed → SFINAE drops success → `__failure_type`
→ no `::type` → `__is_invocable` false.

**Minimal reducers (sizeof-/SFINAE-free):** tmp/packbind2.mad — `direct=22`,
`fwd2_fixed=22` (controls pass), **`fwd_pack=0`** (`template<typename... Args>
struct fwd { typedef picksecond<Args...> ps; }` — pack forwarded into a member
typedef's nested template-id expands to EMPTY). tmp/roi6.mad — faithful partial-spec
+ dependent-member-base chain, `tag=0`.

**MECHANISM (MADC_DBG_WB2 probe at the instantiation gate parser.cpp:~2987, removed):**
`[WB2] inst 'fwd': has_pack=1 allow=0 sticky=0 pack_real_inst=0 try_spec=0
real_instable=1 -> OPAQUE`. So these variadic templates ARE
`template_pack_real_instantiable` (real_instable=1) but bail to
`instantiate_opaque_template_use` (pack dropped) because `allow_variadic_real_inst`/
`variadic_real_inst_sticky` is NOT set when they instantiate. The flag is set only
in narrow contexts (member-type-chain head INSIDE class_scope_stack at
parser.cpp:4237; `::value`-fold; statement-level var-decl). It is NOT set for many
`Template<args>::member` resolution paths (top-level expr, `resolve_typename_type_token`
fall-through at 4246, the void_t `_Result::type` walk). **NOTE the real chain has a
2nd layer:** `__invoke_result` IS real-instantiated (sticky), yet its partial-spec
BODY `decltype(_S_test<_Functor,_ArgTypes...>(0))` STILL drops `_ArgTypes...` — so
the partial-spec body token substitution (pack_subst expansion, parser.cpp:3060-3118
+ the decltype/`_S_test` template-arg-list) also fails to expand the forwarded pack.

**NEXT (careful — blanket variadic real-inst REGRESSES testsstream, per vector<T*>
A1):** the fix is NOT one-line. Two coupled sub-problems: (a) set
`allow_variadic_real_inst` for `Template<args>::member` resolution wherever the
template is `pack_real_instantiable` (generalize the 4237 logic beyond class_scope_stack
— but scoped to avoid the pure-trait `__or_` regression); (b) make the partial-spec
BODY substitution expand a forwarded trailing pack `_ArgTypes...` inside a nested
template-id / function-template-call decltype (pack_subst body-loop coverage). Build
a faithful reducer that reaches the partial-spec body WITH sticky on (the void_t
detection wrapper) — synthetic `X<...>::member` exprs go OPAQUE (different path) and
mislead. Deep template-machinery work; multi-step, fulltest-gated for regressions.

**UPDATE 29-B — DEEPER: the REAL-chain root is a MISSING PARTIAL-SPEC REGISTRATION,
not pack expansion.** A `MADC_DBG_WB` capture probe at the template-body capture site
(parser.cpp:~32518, removed) dumped EVERY `__result_of_impl` registered while parsing
real `<type_traits>`: the PRIMARY (`typedef __failure_type type;`, type_traits:2536)
and TWO partials — `<true,false,_MemPtr,_Arg>` (`: public __result_of_memobj<…>`, 2542)
and `<false,true,_MemPtr,_Arg,_Args...>` (`: public __result_of_memfun<…>`, 2548). The
THIRD partial — **`<false,false,_Functor,_ArgTypes...> : private __result_of_other_impl
{ typedef decltype(_S_test<_Functor,_ArgTypes...>(0)) type; }` (type_traits:2565) — is
NEVER REGISTERED** (absent from partial_spec_map). So `__result_of_impl<false,false,
Cmp&,…>` finds no matching spec and falls back to the PRIMARY → `typedef __failure_type
type;` → `__invoke_result`'s base = `__failure_type` → no `::type` → `__is_invocable`
false. The two registered specs come BEFORE `struct __result_of_other_impl` (2554); the
missing one comes AFTER it — so parsing `__result_of_other_impl` (a NON-template struct
whose two `_S_test` static member-function templates have a `decltype(declval<_Fn>()(
declval<_Args>()...))` return + a `template<typename...> _S_test(...)` variadic overload)
likely derails/consumes past the following partial-spec declaration. NO visible parse
error (invoc2.mad emits only the final `must be invocable`) — a silent skip.

**Synthetic reducers DIVERGE (do NOT chase them):** tmp/roi7.mad reproduces the SHAPE
(primary + `<true,false>` + `__result_of_other`-style struct + `<false,false,F,A...>`
spec with a decltype-pack body) but madc REGISTERS all three specs there (WB-cap fires
3×) and the failure is then the OPAQUE-instantiation path (top-level `X<…>::member`
doesn't set allow_variadic_real_inst → tag=0), NOT the missing-registration path. So
roi7 is a DIFFERENT bug. The real-header skip needs the full type_traits context.

**NEXT-SESSION (ordered):** (1) instrument the template-DECLARATION parse entry (where
`template<…> struct __result_of_impl<pattern>` is recognized and spec_pattern collected,
+ the registration at parser.cpp:32530) — print every `__result_of_impl` spec_pattern
ATTEMPTED, and whether parsing `__result_of_other_impl`'s `_S_test` overconsumes (the
variadic `_S_test(...)` + the declval-call decltype return). Pin WHY the 2565 spec decl
is skipped. (2) Once registered, the SECONDARY bug (roi7: opaque path / pack-body
expansion) likely also needs the flag-scoping fix above. Both fulltest-gated. Reducers:
tmp/invoc2.mad (real, folds false), tmp/roi7.mad (shape, opaque-path tag=0),
tmp/packbind2.mad (forwarded-pack-drops, fwd_pack=0).

## Update 28 — if-condition wall FIXED (C++17 init-statement support for `if` AND `switch`); cascade now at `_S_key` / `_M_at_eof`

**FIXED, fulltest 652/5/0/18 (+1 = new tests/testifinit, ZERO regression; EXE
path also validated).** The Update-27 frontier — `testmap.mad` under
`--std=c++20` throwing `Expecting ')' after if condition declaration` — mapped
(via an `MADC_DBG_IFCOND`-gated probe at the throw) to `system_error:318`:

```cpp
if (auto __c = __lhs.category() <=> __rhs.category(); __c != 0)
```

This is the **C++17 init-statement** form (`if (init-statement; condition)`),
which madc did not support. The old condition-declaration path parsed the init
clause AS the condition-declaration, then hit `;` where it expected `)`. The
apparent `if (int c = foo(); cond)` "success" was an ILLUSION — `parseDeclaration`
stopped with last-consumed = the `)` of `foo()`, which the
`curToken()->id() != tkClBrk` check mistook for the if's `)`, silently dropping
the condition (proven by an inverted-condition reducer returning the wrong
branch).

**Fix (root-cause, deepest layer — 5 files, +142 lines):**
- New `Program::parse_optional_init_statement()` (parser.cpp, before
  `TokenIF::parse`): detects a top-level `;` before the matching `)` by a
  bracket-depth scan of the lookahead (robust regardless of how the init
  clause's own parser stops), parses the init-statement (simple-declaration or
  expression-statement), consumes its `;`, and leaves the stream at the
  condition. ONE implementation, called by both `if` and `switch` — `switch
  (init; cond)` had the identical gap ([stmt.pre] covers both).
- `TokenIF` / `TokenSWITCH` gained an `init_stmt` member; `TokenIF::parse` and
  `TokenSWITCH::parse` call the shared helper right after consuming `(`.
- `translate_if` / `translate_switch` (cir_builder.cpp) lower
  `if (init; cond) S else E` to `{ init; if (cond) S else E }` — the
  init-statement (and its temps) emitted in a wrapping `N_BLOCK` before the
  conditional, sharing the enclosing scope. `translate_if`'s existing body was
  split into `translate_if_core`.

Verified against gcc -std=c++17 (decl-init, expr-init `if (x = foo(); x>5)`,
else-branch sees the init var, `switch (init; expr)`) — byte-identical output.
New regression test: tests/testifinit.mad (+.expect).

**NEW FRONTIER:** with the if-condition wall gone, `testmap.mad --std=c++20`
advances to: `use of undeclared identifier '_S_key'` (×1) and
`'_M_at_eof' is a private member of istreambuf_iterator<…>` (×2, char + wchar_t).
Plus the pre-existing 3× `Expecting integer constant expression` (iostream
:791/:817) and the 14× recovered cosmetic `undeclared identifier 'std'` at :141.
NOTE: under madc's DEFAULT std (`__cplusplus==201703`, C++17), `testmap` fails
earlier at `map::contains` (a C++20 method gated `#if __cplusplus > 201703L`) —
so the map/set tests inherently require a C++20 std to exercise `contains` etc.;
the campaign's reductions run under `--std=c++20`.

## Update 27 — `ranges::get` wall FIXED (using-decl ns lookup order); cascade now at an if-condition-declaration parse wall

**FIXED (`6456641`, fulltest 651/5/0/18 zero regr).** A `UG`-gated probe at the
using-declaration throw (parser.cpp:20438) showed `ns_name='ranges'`,
`have_var/type/template=0`, BUT `namespace_map["std::ranges"].count("get")==1` —
i.e. `get` IS in std::ranges; the lookup just used the wrong namespace.
`resolve_source_namespace` (parser.cpp:20109) checked the BARE name FIRST, and a
stray empty `namespace_map["ranges"]` shadowed `std::ranges`. Fix: search
ENCLOSING-scope candidates (`current_ns::name`, innermost-out) BEFORE the
bare/global fallback — matching C++ unqualified namespace lookup (and the
function's own documented intent). `using ranges::get;` now resolves to
`std::ranges`.

**NEW FRONTIER: `testmap.mad:318:57 "Expecting ')' after if condition
declaration"`** (parser.cpp:25320). An `if (...)` condition is being parsed as a
DECLARATION (the `condition_declarator_follows()` heuristic at ~25288), then the
declaration parse doesn't end at `)`. Likely a complex C++ if-condition in the
ranges/algorithm headers (a call/expression misdetected as a decl, or a C++17
`if (init; cond)` / structured-binding condition). NEXT: gdb/diag the throw at
25320 (print the condition tokens / what condition_declarator_follows matched) or
grep the include path near line 318's header for the `if (...)` form; decide
whether to tighten the declarator heuristic or handle the init-statement form.
12× recovered "undeclared std" at :141 still leak (cosmetic).

## Update 26 — `ranges::get` frontier: `using ranges::get;` at ranges_util.h:780 fails (4 reductions don't reproduce)

The wall is `bits/ranges_util.h:780` — `using ranges::get;` (in `namespace std`,
AFTER `namespace ranges` closes at 777), importing the subrange `get` overloads
(ranges_util.h:435/445, `template<size_t _Num, …> requires (_Num<2) constexpr auto
get(const subrange<…>&)`). madc's using-declaration handler (parser.cpp:20438)
throws "'get' is not a member of namespace 'ranges'" — i.e. neither have_var (a
function in `namespace_map[ns_name]`) nor have_type nor have_template is set.

FOUR reductions ALL PASS (do NOT reproduce): `tmp/rg1.mad` (`using ranges::get`,
get = plain fn template in std::ranges), `tmp/rg2.mad` (non-type leading param +
subrange param), `tmp/rg3.mad` (requires-constrained fn template get). So the
isolated shape registers fine and `using ranges::get` finds it — the real failure
is entangled with the full ranges header (like the iterator_category wall was).

NEXT (gdb/diag, needs `make clean && make debug` or an env-gated fprintf at
parser.cpp:20438): for `member_name=="get"`, print `ns_name` (is it `std::ranges`
or just `ranges`? the error text says `ranges` — an ns-resolution smell) and
whether `namespace_map[ns_name].count("get")` / `namespace_datatype_map` /
template maps hold it. Two likely causes: (a) ns_name isn't resolved to
`std::ranges` in THIS context (enclosing-scope lookup differs after the long
ranges parse), or (b) the subrange `get` overloads never registered into
`namespace_map["std::ranges"]` (a parse/registration gap deeper in the ranges
body, possibly related to the 8× recovered "undeclared std" at :141). Pin which,
then fix.

## Update 25 — iterator_category wall FIXED (base-clause promotion); cascade now at C++20 `ranges::get`

**FIXED (`9fb0e8a`, fulltest 651/5/0/18 zero regr).** The Update-24 root cause is
resolved. The base-specifier handler initialized `base_name` to the FIRST token
of the base (the namespace QUALIFIER for a qualified base like
`: __detail::__cond_value_type<_Tp>`), and called
`promote_struct_base_to_class(base_name, …)` with that stale qualifier BEFORE the
`base_name = bdt->definition.name;` update on the next line. promote registered
the qualifier `__detail` into `namespace_datatype_map["std"]`, poisoning it →
later breaking `__detail::__clamp_iter_cat<…>`. Fix: move the
`base_name = bdt->definition.name;` adoption BEFORE the promote call
(parser.cpp:~23524). Found by RD-instrumenting every `namespace_datatype_map`
write site (braced-block injection) → writer = promote at parser.cpp:31524 ←
base-clause at ~23525.

**NEW FRONTIER: C++20 `ranges` surface.** testmap now advances to
`testmap.mad:780:19 'get' is not a member of namespace 'ranges'` (was line 141) —
i.e. `std::ranges::get` / a `ranges` CPO is referenced but `namespace ranges` is
not populated with it. This is the ranges bring-up Update 11 anticipated
("Unknown namespace 'ranges'"). NEXT: determine whether it's a SINGLE missing
`ranges::get` (bounded — register/lower it) or broad `namespace ranges` support;
reduce by grepping the map include path for the `ranges::get` use, gdb/diag the
"not a member of namespace 'ranges'" throw to find the real header:line. The 8×
recovered "undeclared std" at :141 remain (cosmetic, parse continues).

## Update 24 — ROOT CAUSE PINNED: corrupt `namespace_datatype_map["std"]["__detail"]` entry

Env-gated `RD` instrumentation in `resolve_declared_type_token` (entry + every
candidate return, cond `tname=="__detail"`) pinned it DEFINITIVELY:

```
[RD ENTRY] tname=__detail next=:: ...
[RD A_nsloop] scope=std -> __cond_value_type_char
```

**The failing `iterator_category` resolution of `__detail::__clamp_iter_cat<…>`
hits the namespace-scope loop (`A_nsloop`, parser.cpp:~4533): it looks up the
bare qualifier `__detail` in `namespace_datatype_map["std"]` and FINDS a corrupt
entry `__detail -> __cond_value_type_char`, returns it (via
resolve_member_chain_or_type, which can't consume `::__clamp_iter_cat` off that
unrelated type), leaving `:: __clamp_iter_cat` → "Expecting ';'".** This branch
runs BEFORE the correct 4579 `::`-block, so it preempts the right answer.

STATE-DEPENDENCE EXPLAINED: the corrupt `["std"]["__detail"]` entry does not
exist when the FIRST `reverse_iterator` resolves (it correctly takes the 4579
block → `iterator_traits_char___iterator_category`, verified by RD `C_nsblock`).
The entry is written LATER, during a `__detail::__cond_value_type<…>` /
`__detail::__iter_value_t<…>` resolution (RD shows those C_nsblock fires); a
subsequent `iterator_category` then reads the now-corrupt entry.

WRITER not yet caught, but RULED OUT: the instantiation sites (2773/3311/3925 —
they key by the MANGLED type name in the DEFINING namespace, not `__detail` in
`std`) and the `using`-declaration import (20272/20276 — RD writer-probe silent).
Remaining candidate `namespace_datatype_map[...][key]=` sites (key could be
`__detail`): tag (23669/23823/23839), alias (20375/20861/25885/32584),
31542/31544, enum (26489/26540), concept (32216). A regex auto-injection to catch
the writer broke `else if` single-statement bodies — instrument with PROPER
BRACES, or use a gdb watchpoint.

**FIX (next session): find the site registering the NAMESPACE name `__detail`
as a type entry (→ a sibling `__cond_value_type<char>`) in
`namespace_datatype_map["std"]` and stop it** — almost certainly a
namespace-path-splitting bug where a `std::__detail::X` instantiation/registration
keys `X`'s result under the qualifier `__detail` in `std` instead of under `X`'s
mangled name in `std::__detail`. Verify: `tmp/ic4.mad` already passes (the
partial-spec-constraint fix); the corruption is specific to the real
`__cond_value_type`/`__iter_value_t` instantiation registering wrongly. Once
fixed, `__detail::__clamp_iter_cat<…>` resolves via the 4579 block again and the
iterator_category alias completes. gdb needs `make clean && make debug`.

## Update 23 — `iterator_category` wall narrowed further (open puzzle for a fresh session)

More gdb/diag on the failing `iterator_category` resolution
(`__detail::__clamp_iter_cat<typename __traits_type::iterator_category,
random_access_iterator_tag>` → `__cond_value_type_char`, leftover `::
__clamp_iter_cat`). SOLID new facts:
- `lazy_resolve_type` is a NULL stub (parser.cpp:11046) — RULED OUT as the source.
- A `DT_DIAG` env-gated probe at the two bare-`__detail` type-lookup branches
  (the namespace-scope loop ~4533 and `datatype_map` ~4548) did NOT fire for
  `__detail` — so `__cond_value_type_char` does NOT come from those branches.
- The leftover `:: __clamp_iter_cat` means the 4579 `::`-qualifier block AND
  `resolve_namespaced_type_token` (4615) were NOT the producer either (both
  CONSUME the `::member` on success).
- Yet `resolve_declared_type_token` returns `__cond_value_type_char` (non-NULL)
  consuming only `__detail`. NONE of the obvious branches fit — so the model is
  incomplete. CANDIDATES not yet instrumented: the `<`-template branch (~4488),
  `instantiate_template_id(tname)` at ~4573, or `resolve_member_chain_or_type`
  doing something unexpected with the chain.
- The FIRST reverse_iterator instantiation resolves correctly (4579 block fires);
  the FAILING one is a LATER instantiation — STILL state-dependent.

NEXT (fresh session, focused — I hit reasoning fatigue here): instrument
`resolve_declared_type_token` with an env-gated fprintf at the FUNCTION ENTRY
(after `tname` is set) AND at EVERY `return` site, gated on `tname=="__detail"`,
printing a per-branch tag + the returned type name. ONE optimized rebuild then
`<env>=1 ./bin/madc --std=c++20 --no-embedded-headers tests/testmap.mad` pins the
exact branch definitively (no more guessing). Then fix that branch to not
mis-resolve the `__detail` namespace qualifier to the `__cond_value_type<char>`
sibling, and/or let the correct 4579 `::`-block path win.

## Update 22 — gdb on the `iterator_category`/`__clamp_iter_cat` wall: state-dependent qualified-alias-template resolution

gdb (clean debug build) on the `Expecting ';' after using alias` throw
(parser.cpp:20318) for `iterator_category` pinned the mechanism:

- The FAILING alias `using iterator_category = __detail::__clamp_iter_cat<
  typename __traits_type::iterator_category, random_access_iterator_tag>`
  resolves (resolved=1) to **`__cond_value_type_char`** and leaves a trailing
  **`:: __clamp_iter_cat`** unconsumed → "Expecting ';'".
- `__detail` is NOT a class-alias (resolve_current_class_type_alias=NULL) nor in
  datatype_map (count 0). resolve_declared_type_token reaches the `::`-qualifier
  block (parser.cpp:~4579): it checks `tokens[j+1]==tkLT &&
  template_declared_in_namespace(member_name, ns_name)`.
- For the FIRST `iterator_category` instantiation that block FIRES correctly:
  member_name=`__clamp_iter_cat`, ns_name=`std::__detail`,
  `instantiate_template_id("__clamp_iter_cat", …, "std::__detail")` returns
  `iterator_traits_char___iterator_category` and consumes the `<…>` (verified by
  a manual gdb call → stream left at `;`). So the FIRST one is fine.
- The FAILING (LATER) instantiation does NOT enter that block (the `::
  __clamp_iter_cat` is left unconsumed), so it falls through to
  `resolve_namespaced_type_token` (parser.cpp:~4615), which returns
  `__cond_value_type_char` consuming only `__detail`.

**So the bug is STATE-DEPENDENT: `template_declared_in_namespace("__clamp_iter_cat",
"std::__detail")` (or the `ns_name` from `resolve_namespace_name_in_scope`) holds
for the first reverse_iterator instantiation but FAILS for a later one** — and
`resolve_namespaced_type_token` then mis-resolves `__detail` to the unrelated
`__cond_value_type<char>` (a sibling in std::__detail, from the value_type /
indirectly_readable_traits chain). NEXT (gdb recipe — needs `make clean && make
debug`): catch the FAILING `iterator_category` (it leaves `::` unconsumed;
condition a breakpoint at 4593/4615 on the SECOND+ hit, or on
`template_declared_in_namespace(...)==false` for `__clamp_iter_cat`), find WHY it
differs between instantiations (registration timing? scope-stack difference?),
AND separately make `resolve_namespaced_type_token` return NULL rather than
mis-resolve a namespace qualifier (`__detail`) to a sibling type when the member
isn't found. NOTE: bin/madc may be `-g -O0` debug during this work; `make clean
&& make` restores optimized.

## Update 21 — partial-spec `requires`-constraint NOW EVALUATED (Update-20 fix landed); next wall pinned

**Update-20 fix LANDED (`41cbf56`, fulltest 651/5/0/18 zero regr).** A partial
specialization's C++20 `requires`-clause is now captured on `TemplateDef::constraint`
(at the template-decl handler, saved-prefix of the tokens skip_requires_clause
consumes) and EVALUATED in `match_partial_specialization` (substitute the
deductions, fold via the concept evaluator, reject the candidate when false).
`tmp/ic7.mad` now selects primary-for-char*/spec-for-WithVT correctly; `ic4`/`ic1`
pass. Cleared the testmap :505 wall (`__iter_traits<char*>::iterator_concept` —
`__iter_traits_impl`'s `requires __primary_traits_iter<_Iter>` and
`indirectly_readable_traits`'s `__has_member_value_type` now select right).

**NEXT WALL (pinned via UA diag): qualified alias-template with a `typename
T::member` argument.** testmap fails at `using iterator_category =
__detail::__clamp_iter_cat<typename __traits_type::iterator_category,
random_access_iterator_tag>;` (stl_iterator.h:~218, testmap.mad:141 attribution).
UA diag: this alias `resolved=1` but `next='::'` — i.e. the resolver consumed only
PART of the qualified alias-template-id and left a trailing `::`, then "Expecting
';' after using alias" (parser.cpp:20318). The `__clamp_iter_cat<…>` is a QUALIFIED
(`__detail::`) alias-template whose FIRST arg is `typename __traits_type::
iterator_category` (a typename-qualified dependent member). Hypothesis: the
qualified-alias-template-id arg parsing mishandles a `typename X::Y` argument
(stops at the inner `::`). NEXT: reduce `NS::alias<typename T::member, U>` at
--std=c++20 (mirror tmp/qa3.mad but with a `typename`-qualified first arg), find
where arg collection stops early, fix. Also still: 1× recovered "undeclared std"
leak at :141 (cosmetic, parse continues).

UPDATE 2 (reductions EXHAUSTED — gdb required): FOUR synthetic reducers ALL PASS
and do NOT reproduce, progressively matching the real `__clamp_iter_cat` shape:
`tmp/qta.mad` (bare `NS::alias<typename Concrete::m, U>`), `tmp/qtb.mad`
(`__traits_type` = template-id typedef `iterator_traits<It>`), `tmp/qtc.mad`
(default 3rd param `Otherwise = Cat`, called with 2 args), `tmp/qtd.mad` (FULL
shape: qualified alias + default param + `typename T::member` arg + concept-
condition body `condt<derived<Cat,Limit>, Limit, Otherwise>`). Since even the
full synthetic shape passes, the wall is entangled with the REAL header chain
(real `iterator_traits<__normal_iterator<const char*>>::iterator_category`, real
`derived_from`, real `random_access_iterator_tag`, the iter_traits selection my
fixes touch). DO NOT re-try black-box reductions — go straight to gdb: debug
build — NOTE: `make -C src debug` ALONE reuses stale optimized .o files (no symbols; gdb says "No source file named parser.cpp"); you MUST `make -C src clean && make -C src debug` for a debuggable binary, then `make -C src clean && make -C src` to restore optimized — break `parser.cpp:20318` ("Expecting ';' after using
alias"), conditional on `alias_name=="iterator_category"`, then inspect what
`resolve_declared_type_token` consumed of `__detail::__clamp_iter_cat<typename
__traits_type::iterator_category, random_access_iterator_tag>` and WHY it stopped
leaving a trailing `::` (UA diag showed resolved=1 next='::'). That pin tells
whether the leftover `::` is the inner `typename __traits_type::iterator_category`
or the qualified `__detail::` head.

UPDATE 1 (reduction tried): the SIMPLE form does NOT reproduce — `tmp/qta.mad`
(`foo::clamp<typename __traits_type::iterator_category, long>` with
`__traits_type` a DIRECT typedef to a concrete struct) PASSES. So the wall is
specific to the REAL chain where `__traits_type = iterator_traits<_Iterator>`
(a typedef to a TEMPLATE-ID), i.e. `typename iterator_traits<_It>::
iterator_category` as the alias arg — the qualified member off a template-id
typedef, likely through the iter_traits selection. Bare `typename Concrete::m`
works; the gap is `typename (template-id typedef)::m` as a qualified-alias-tmpl
ARG. NEXT: gdb the real `iterator_category` using-alias resolution (break
parser.cpp:20318 "Expecting ';' after using alias", inspect what
resolve_declared_type_token consumed and why it stopped at the inner `::`), or
build a closer reducer with `__traits_type = some_traits<X>` (template-id typedef).

## Update 20 — ROOT CAUSE of the current frontier: partial-spec `requires`-constraint is ignored

The Update-19 frontier (`tmp/ic4.mad`) is now root-caused precisely
(`tmp/ic7.mad` confirms): **madc does not evaluate the `requires` constraint on
a partial specialization.** When a partial spec has the SAME argument pattern as
the primary and differs ONLY by a constraint —
`template<class I,class T> struct iti{...};` (primary) +
`template<class I,class T> requires HasVT<I> struct iti<I,T>{...};` (spec) —
madc always applies the spec, ignoring `HasVT<I>`. `tmp/ic7.mad`:
`iti<char*,char*>::which` is **2 (spec)** but must be **1 (primary)** because
`HasVT<char*>` is false. This is exactly libstdc++'s `__iter_traits_impl`
(`requires __primary_traits_iter<_Iter>`) and `indirectly_readable_traits`
(`__has_member_value_type _Tp`, …) selection — so `__iter_traits<char*>` picks
the wrong arm and the trailing `::iterator_concept` then fails.

**FIX RECIPE (next session — bounded, ~3 sites):**
1. `include/madc.h` — add `std::vector<TokenBase *> constraint;` to
   `struct TemplateDef` (the requires-clause tokens of a partial spec; empty for
   unconstrained).
2. **Capture** the partial-spec's requires-clause instead of discarding it. The
   template-declaration handler currently calls `skip_requires_clause()` at
   ~parser.cpp:31663 (between the template-param-list and the `struct`). When the
   declaration turns out to be a partial specialization, the clause tokens must
   be CAPTURED (a capturing variant of skip_requires_clause / skip_constraint_
   expression) and stored on the `TemplateDef` registered at ~32358
   (`partial_spec_map[class_name].push_back(td)`).
3. **Evaluate** in `match_partial_specialization` (~15416): after a candidate
   spec's pattern matches and `out_subst` (typeparam→arg) is built, if
   `td.constraint` is non-empty, substitute `out_subst` into the constraint
   (`clone_template_tokens_with_type_subst`) and fold it as a constant in an
   isolated stream (reuse the concept structural-arm path —
   `parse_constant_integer_expression`). If it folds to 0, REJECT the candidate
   (treat as non-matching) so a more-specialized or the primary is chosen.
   The concept evaluator (Updates 18–19) already folds `HasVT<…>` etc., so step
   3 is mostly wiring. Verify with `tmp/ic7.mad` (which must become 1/2),
   `tmp/ic4.mad`, then testmap's :505 wall; fulltest; commit.

This unblocks the `__iter_traits`/`indirectly_readable_traits`/iterator-traits
selection that the whole reverse_iterator/iterator-concept tail depends on.

## Update 19 — five C++20 walls cleared; cascade deep in the iterator/concepts surface

This session landed FIVE regression-free fixes (all pushed, fulltest 651/5/0/18
throughout), each clearing one wall of the `--std=c++20 <map>` parse cascade:

1. **Concept structural arm** (`ad2fe83`) — concept-id `Name<Args>` as non-type
   bool folds 0/1. Test `testconcepteval`.
2. **requires-expression arm** (`484f04d`) — `evaluate_requires_expression_constant`
   + `constraint_expression_well_formed`; params modeled as `std::declval<T&>()`,
   each requirement checked well-formed. Cleared `iterator_concept`/
   `random_access_iterator`. Test `testrequiresexpr`.
3. **Qualified-alias-body namespace fallback** (`35f7d54`) — when a qualified
   `NS::alias<X>`'s body references an unqualified sibling, retry body resolution
   in the alias's defining namespace (strictly additive, only on prior failure).
   Cleared `iter_value_t<…>`. Reducer `tmp/qa3.mad`.
4. **Trailing requires-clause on member fn** (`2b3ad5d`) — skip `requires …`
   between declarator and body (e.g. `operator->() const requires …`). Reducer
   `tmp/trc2.mad`.
5. **Strict type-requirement** (`16b16ac`) — `typename T::m` requires FULL type
   consumption; `typename char*::value_type` is now correctly ill-formed, so
   `HasVT<char*>` folds false (was lenient → wrong concept-partial-spec pick).
   Reducer `tmp/hasvt.mad`; testrequiresexpr strengthened.

**CURRENT FRONTIER (precise, reduced): concept-constrained partial-spec
`::type::member` chaining.** `tmp/ic4.mad` minimally reproduces. With a
concept-constrained partial spec
`template<class I,class T> requires HasVT<I> struct iti<I,T>{using type=T;};`
(primary `using type = iterator_traits<I>;`), resolving `iti<char*,char*>::type`
ALONE works (`tmp/ic5.mad` passes → iterator_traits<char*>), but
`iti<char*,char*>::type::iterator_concept` (one more member hop, in one alias)
FAILS "Expecting type in using alias". Without the concept spec it works
(`tmp/ic2.mad`). So a concept-constrained partial-spec instance resolves its
`::type` but chaining a FURTHER member off that result fails — the `::type`
result is left dependent/opaque past the first hop. This is the libstdc++
`__iter_traits<char*>::iterator_concept` line (reverse_iterator's
`iterator_category`/`__iter_concept`), testmap.mad:505 attribution. NEXT: gdb
how `iti<…>::type` resolves under a concept-constrained partial spec and why the
trailing `::iterator_concept` isn't applied to the concrete result.

**ALSO NOTED (cosmetic, recovered — not the blocker): 4× leaked "undeclared
identifier 'std'" at testmap.mad:141.** The parse RECOVERS (continues to :505),
so these are leaked-but-caught throws (like the 791/817 soft errors), most
likely the requires-evaluator's `std::declval` qualified lookup in a fold
context where `std` is momentarily unresolvable. Benign for the cascade;
suppress later (or root-cause if it ever becomes load-bearing).

## Update 18 — CONCEPT EVALUATOR LANDED (structural arm + requires-expr); cascade advanced past iterator_concept

Both halves of the C++20 concept-satisfaction evaluator are committed and the
`reverse_iterator::iterator_concept` wall (the Update-16/17 frontier) is CLEARED:

- **Structural arm** (`ad2fe83`) — concept-id `Name<Args>` used as a non-type
  bool folds to 0/1 in `parse_constant_primary`: substitute args into the stored
  `concept_map` constraint, fold recursively in an isolated stream. Nested
  concept-ids recurse; trait atoms (`derived_from`→`__is_base_of`,
  `same_as`→`__is_same`, `__is_*`) fold via existing paths; `&&`/`||`
  short-circuit. Test `testconcepteval`.
- **requires-expression arm** (THIS commit) — `evaluate_requires_expression_constant`
  + `constraint_expression_well_formed`. Each parameter is modeled as
  `std::declval<Type&>()` substituted into the requirement bodies; a requirement
  is satisfied iff its expression/type/constraint is WELL-FORMED (checked via
  `parseExpression` in unevaluated context). Handles all four requirement kinds
  (simple, type, compound `{E}->C`, nested `requires CE`). Test
  `testrequiresexpr`.

**Verified on the REAL chain (CE_DIAG/UA instrumentation, since removed):**
`random_access_iterator<map_iter>` folds to **0** (correct — false via the
strict `derived_from<__iter_concept, random_access_iterator_tag>` conjunct),
WITHOUT throwing, and `using iterator_concept = __conditional_t<…>` now
resolves. The diag trace showed `iterator_concept`/`iterator_category`/`type`/
`value_type`/… all resolving on the first reverse_iterator instantiation.

**KNOWN LIMITATION (documented, NOT a shim):** the requires-expression
well-formedness check is only as strict as madc's underlying expression/type
resolver, which currently OVER-ACCEPTS two cases: (a) operators on a class that
lacks them (`++x` on a type with no `operator++` does not error — verified
`tmp/incr_check.mad`), and (b) a nonexistent nested member type
(`typename NoFoo::kind` resolves non-NULL). Member-CALL requirements ARE strict
(`x.foo()` correctly fails on a type without `foo` — `testrequiresexpr` asserts
both directions). For the map/set goal this over-acceptance is benign: the
iterator concepts' decisive conjunct is the strict `derived_from`/`__is_base_of`,
so `random_access_iterator<map_iter>` folds false regardless of the (lenient)
requires values. Tightening the resolver (reject operators/member-types that
don't exist) is a separate follow-up, tracked here — it would make concept
satisfaction fully precise but is orthogonal to map/set.

**NEW FRONTIER (UA diag, this session — precisely pinned): `using value_type =
iter_value_t<const_iterator>` resolved=0**, where `const_iterator =
__gnu_cxx::__normal_iterator<const char*, basic_string<char>>` (the failing
reverse_iterator is `reverse_iterator<basic_string::const_iterator>`, reached
during `<string>`/`<iterator>` parse). The NON-const sibling
`iter_value_t<__normal_iterator<char*>>` resolved=1 in the SAME trace — so the
gap is specifically the **const-pointer** `__normal_iterator<const char*, …>`.
The preceding `iterator_concept = __conditional_t<random_access_iterator<
const_iterator>,…>` resolved=1 (concept eval works); ONLY the `iter_value_t`
line fails. `iter_value_t<_Tp> = typename __iter_traits<_Tp,
indirectly_readable_traits<_Tp>>::value_type` (iterator_concepts.h:303) — and
`__normal_iterator<const char*>` HAS a `::value_type` (= char via
iterator_traits), so `indirectly_readable_traits`'s `__has_member_value_type`
partial spec should select. NOTE: concept-constrained partial-spec selection
ALREADY works in isolation (`tmp/concept_partial.mad` passes), so the gap is
narrower: either (a) `__normal_iterator<const char*, …>::value_type` doesn't
resolve in madc (const-pointer template-arg member-typedef instantiation), or
(b) `__iter_traits`/`remove_cvref_t` mis-handles the const-pointer iterator.
NEXT: reduce `iter_value_t<__normal_iterator<const char*>>` (or the
`__normal_iterator<const char*>::value_type` member-typedef directly) at
--std=c++20, find the bounded gap, fix, continue the cascade. Then
`tests/testmap.flags`/`testset.flags` = `--std=c++20` once it fully clears.
SIMPLE REDUCTION DOES NOT REPRODUCE (`tmp/ivt_const.mad`: a hand-rolled
`iter_traits`-free `ivt<normal_iterator<const char*>>` over a
`HasVT`-constrained partial spec resolves fine for BOTH char* and const char*).
So the gap is in the REAL machinery — most likely the `__iter_traits<_Tp,
Default>` indirection (iterator_concepts.h ~290: it prefers `iterator_traits
<_Tp>` when user-specialized, else the Default) or the const_iterator's
instantiation STATE at that point in the <string> parse (forward/incomplete),
NOT the bare concept-constrained partial-spec selection. gdb the real
`iter_value_t<const_iterator>` resolution (break the using-alias throw, cond
alias=="value_type", step into resolve_declared_type_token → __iter_traits).

## Update 17 — concept evaluator DESIGNED + verified; storage foundation committed

Concept storage committed (5fcb35e): madc now captures each concept's constraint
tokens + typeparams in `concept_map` (keyed name and ns::name). The EVALUATOR is
fully designed and its every mechanism verified this session:

**The structural arm (in parse_constant_primary, beside the fix-9 var-template arm,
~7001):** when an ident `Name` is in `concept_map` and followed by `<`, capture the
args (capture_call_template_args), substitute typeparams→args into the stored
constraint (clone_template_tokens_with_type_subst), and fold it in an ISOLATED
stream (swap `tokens`, parse_constant_integer_expression, restore) → return 0/1.
This is ~30 lines, identical in shape to the committed fix-9 var-template arm.
VERIFIED it suffices for the goal:
- nested concept-ids recurse (re-enter this same arm);
- `derived_from<D,B>` IS a concept → recurses to its constraint `__is_base_of(_Base,
  _Derived)`, a builtin that ALREADY folds in parse_constant_primary (tmp/baseof.mad
  → `1 0`);
- `&&`/`||` SHORT-CIRCUIT (parse_constant_land/lor, line ~7232) and
  skip_const_logical_operand skips balanced `(){}` — so a short-circuited
  `requires(...){...}` operand is skipped cleanly;
- so `random_access_iterator<map_iterator>` = `bidirectional_iterator<It>(true) &&
  derived_from<__iter_concept<It>, random_access_iterator_tag>(FALSE) && …` →
  short-circuits to **false** at derived_from, requires-expr never reached. CORRECT.

**The one remaining sub-feature = `requires`-expression evaluation** (reached inside
the input/forward/bidirectional sub-evaluations for map, which are NOT short-
circuited): evaluate `requires(_Iter __i, …){ ++__i; --__i; *__i; … }` by registering
the params as concrete-typed locals and checking each requirement's expression is
WELL-FORMED (try-resolve via parseExpression, catch → unsatisfied). ~80–100 lines,
touches local-scope setup + expression resolution — delicate, hence a focused effort
(NOT an optimistic-true stub, which would be a shim). Add it as
`parse_constant_primary`'s `requires`-arm (`if ident=="requires" → evaluate`).

Once both land: `random_access_iterator<It>` folds correctly → reverse_iterator's
`iterator_concept` using-alias resolves → testmap.mad compiles further at
--std=c++20. Then continue the cascade; finally add `tests/testmap.flags` /
`testset.flags` = `--std=c++20` so the tests run where `contains` is available.


## Update 16 — MILESTONE: `<map>` parses at --std=c++20; `contains` EXPOSED (fix 10)

**Fix 10 (e79f572)** — accept TEMPLATE members in using-declarations. `using
std::__detail::__range_iter_t;` (ranges_base.h:93) threw "is not a member of
namespace": the resolver only matched var/type members, never alias/class/variable
templates. Added those (template_alias_map / template_map / var_template_map by
defining namespace). **Result: `#include <map> --std=c++20` now PARSES end-to-end
(exit 0 — only RECOVERED soft constexpr-fold messages print, see below), so
`map::contains` is EXPOSED.** testmap.mad at `--std=c++20` advances PAST `contains`
to a NEW wall: `testmap.mad:141 "Expecting type in using alias"` (a template
using-alias in the contains/ranges region; throw parser.cpp:19815). fulltest 648/5.

**IMPORTANT — the 791/817 "Expecting integer constant expression" messages are
RECOVERED SOFT ERRORS, not blockers.** capture_constant_initializer_value catches
the throw and TokenCLASS::parse structurally skips the unfoldable
`numeric_limits<__max_size_type>::digits10 = static_cast<int>(digits * numbers::ln2
/ numbers::ln10)` initializer; the message LEAKS to stderr (Throw prints before the
catch) but the parse CONTINUES. So the FP-constexpr wall (Update 15) is NOT on the
critical path — do not spend effort folding it; optionally suppress the leaked
message later (cosmetic). The earlier Update-14/15 framing of 791 as the blocker was
superseded by running map to completion: the real hard blocker was `__range_iter_t`
(fix 10), now cleared.

**To actually PASS testmap/testset:** they need `--std=c++20` (for `contains`), so
once the cascade fully clears, add a `tests/testmap.flags` / `testset.flags` =
`--std=c++20` fixture.

**NEXT FRONTIER = CONCEPT EVALUATION (the hard tail — a major C++20 feature, NOT a
quick syntactic fix).** gdb pinned the testmap.mad:141 wall to
`<bits/stl_iterator.h>:166`, inside `std::reverse_iterator`'s instantiation:
`using iterator_concept = __conditional_t<random_access_iterator<_Iterator>,
random_access_iterator_tag, bidirectional_iterator_tag>;`. The condition
`random_access_iterator<_Iterator>` is a **C++20 concept used as a bool non-type
argument** to `__conditional_t`. madc REGISTERS concept names (fix 3) but does NOT
EVALUATE concept satisfaction (requires-expressions / nested-concept checks), so the
bool can't fold → the `__conditional_t<...>` target doesn't resolve → "Expecting
type in using alias". Real concept evaluation is a large feature (evaluate a
concept's requires-clause for concrete args → true/false). PRAGMATIC INTERIM to keep
the cascade moving: when `__conditional_t`/`conditional_t`'s bool condition is an
unevaluable concept-id, pick a branch heuristically — for `random_access_iterator
<It>` on a map/list iterator the answer is FALSE → `bidirectional_iterator_tag`
(correct here; would be wrong for vector — so scope/flag it, or evaluate the
specific iterator concepts properly). This wall warrants a dedicated, careful
effort rather than the rapid syntactic loop.


## Update 15 — Wall C cascade: 9 fixes landed (fix 9 = var-template in constant ctx)

**Fix 9 (97eef98)** — variable-template use in a CONSTANT expression. The
`<numbers>:122` `inline constexpr double e = e_v<double>;` wall: fix 7 resolved
`e_v<double>` → `_Enable_if_floating<double>` → `enable_if<is_floating_point_v
<double>, double>`, and folding enable_if's non-type bool arg `is_floating_point_v
<double>` (a variable template) threw in `parse_constant_primary` — fix 7's
use-resolution is only in `parseExpression`. Added a var-template arm to
parse_constant_primary: substitute the args into the init and fold recursively in
an isolated stream (`is_floating_point<double>::value` → 1). gdb full-backtrace
nailed it (frames: parseDeclaration → parseExpression → fix-7 `e_v` →
`_Enable_if_floating` → `enable_if_t` → `enable_if` → fold_nontype_arg_constant →
parse_constant_primary throw). New test testvartemplateconst.mad. fulltest 648/5.

Progress (`#include <map> --std=c++20`): iterator_concepts → uses_allocator →
`<numbers>` (now PARSED) → **next frontier PINNED (gdb): `<bits/max_size_type.h>`
:778** `static constexpr int digits10 = static_cast<int>(digits * numbers::ln2 /
numbers::ln10);` inside `numeric_limits<__max_size_type>`. Thrown in
parse_constant_named_cpp_cast("static_cast") → parse_constant_integer_expression →
parse_constant_primary. 9 Wall C fixes deep; 9 new conformance tests this session.

**This wall is QUALITATIVELY HARDER (floating-point / nested-constant folding) —
flag for a focused effort, not the quick syntactic loop.** The static_cast operand
`digits * numbers::ln2 / numbers::ln10` folds nested constexpr constants:
`numbers::ln2`/`ln10` are var-template-backed constexpr DOUBLES (`inline constexpr
double ln2 = ln2_v<double>;`), and `digits` = `__gnu_cxx::__int_traits<_Sp::__rep>
::__digits + 1`. madc's constant parser is INTEGER-only — folding doubles to int
gives 0 and `digits * 0 / 0` would divide-by-zero, so naive int-folding is wrong.
PLAIN-double reducers (tmp/fpconst.mad, fpconst2.mad: `static_cast<int>(64 *
n::ln2 / n::ln10)` with `n::ln2 = 0.6931`) do NOT reproduce the throw — they PARSE
and fail later in c2mir ("initializer ... should be a constant expression"). So the
throw is specific to `<numbers>`'s var-template-backed `numbers::ln2`/`ln10` or the
trait-based `digits` not folding. NEXT: gdb the EXACT failing sub-expression at the
throw (is it `numbers::ln2`, or `digits`/`__int_traits<...>::__digits`?) to decide:
(a) fold qualified var-template-backed constexpr-double constants (needs FP
constant values — bigger), or (b) since `digits10` is unused by map/set, a way to
parse-past an unfoldable constexpr member initializer (best-effort/deferred value)
without aborting. Likely (b) is the pragmatic path for the cascade.

Off-path quirks NOTED (not on the map/set path, do not chase): `is_integral_v`
folds wrong in constant context; a non-type bool-arg class instantiation
`Tag<is_floating_point_v<T>>` leaves the member opaque.

## Update 14 — Wall C cascade: 8 fixes landed; frontier = constexpr folding

Continued driving the cascade (each committed + fulltest-green + zero-regression):
7. **C++14 variable templates** (249b7a5) — `template<...> [inline constexpr] T
   name = init;` register (var_template_map) + use-site `name<Arg>` resolves to the
   arg-substituted initializer (parsed inline). Fixed `<numbers>` e_v/pi_v. New
   test testvartemplate. (Narrow gap NOT on path: `(int)name<T>` cast-operand use
   hits a different undeclared-id path.)
8. **C++20 bit-field default member initializers** (e340f70) — `unsigned m:1 = 0;`
   skip the `= init` after each width. Fixed `<bits/max_size_type.h>`:428. New test
   testbitfieldinit.

Progress (`#include <map> --std=c++20`): iterator_concepts.h → uses_allocator.h →
`<numbers>` → max_size_type.h → now **"Expecting integer constant expression"
(~line 791, the numeric_limits<__max_size_type/__max_diff_type> constexpr region)**
(frontier @ e340f70). 8 new conformance tests total. fulltest 647/5.

**NEXT = fix 9, constexpr path for variable-template use. PINNED (gdb):** the wall
is `<numbers>`:122 `inline constexpr double e = e_v<double>;` — thrown in
`parse_constant_primary` (parser.cpp:7015), the INTEGER constant-expression parser.
gdb at the throw: `var_template_map.size()==208` (so `e_v` IS registered by fix 7),
`current_namespace()=="std::numbers"`. ROOT: fix 7's variable-template use-site
resolution lives ONLY in `parseExpression` (parseExpr_identifierArm); the constant
parser `parse_constant_primary` has no var-template arm, so `e_v<double>` there is
unresolved → "Expecting integer constant expression". CAUTION: standalone reducers
(tmp/ced_a..d.mad — constexpr double in a namespace + var-template, even with the
`_Enable_if_floating<T>(literal)L` shape) ALL PASS, because they route the
initializer through `parseExpression` (fix 7 resolves, value 2). `<numbers>` routes
the SAME-shaped decl through `parse_constant_primary` instead — so EITHER (a) fix
the routing (find why a `constexpr double` namespace-scope init goes to the integer
constant parser for `<numbers>` but not the reducers — gdb the DECLARATION parse,
not just the throw — then route it through parseExpression so fix 7 handles it
cleanly, no constant-parser work), OR (b) mirror fix 7 into parse_constant_primary:
on a registered var-template ident followed by `<args>`, substitute the init and
fold it — but the init `_Enable_if_floating<double>(2.718L)` is a functional cast
`TypeId(literal)` the integer constant parser doesn't currently fold, and the value
is a double the int parser can't represent, so (a) is the cleaner path. The method
remains proven: 8 Wall-C fixes deep, each a general C++-conformance win, driving
toward exposing `contains`.

## Update 13 — WALL C is an ACTIVE CASCADE now (6 fixes landed), not a deferred unknown

Reframing: testmap/testset are blocked ONLY by `contains` (Update 11), and `contains`
needs `--std=c++20`, which fails while PARSING the C++20 header surface. That parse
failure is a CASCADE of bounded, general parser-correctness gaps — each a normal
C++11/17/20 feature madc simply hadn't implemented — NOT a monolithic "implement
ranges." This session drove the cascade down fix-by-fix, each committed +
fulltest-green + zero-regression:

1. **Multi-level `using A::B::C;`** (c73413e) — full `::`-chain; all-but-last =
   nested namespace path. Was: "'B' is not a member of 'A'".
2. **Enclosing-scope namespace lookup** (c73413e) — `resolve_source_namespace`
   walks ancestor scopes, so `ranges::__detail` from inside `std::__detail`
   resolves to `std::ranges::__detail`.
3. **Concept-name registration** (6a46af2) — concept defs were parsed-and-discarded;
   now register an inert dtRESERVED placeholder so `using NS::Concept;` resolves
   (madc still doesn't EVALUATE concepts).
4. **C++11 `final` on class/struct heads** (6a46af2) — `struct/class X final {}` was
   mis-parsed; consume the contextual `final` (guarded by a following `{`/`:`) in
   both TokenSTRUCT::parse and TokenCLASS::parse.
5. **Storage specifiers before a post-class-def declarator** (eb3eafa) —
   `struct X {...} inline constexpr x{};` (the CPO shape); skip
   inline/constexpr/static/etc. between `}` and the instance name.
6. **Decl-specifier AFTER the return type in a member decl** (84f1560) —
   `void constexpr operator=(...)` (decl-specifier-seq is unordered); skip the
   no-op specifier before the member-name read. Fixed uses_allocator.h:81.

Progress (`#include <map> --std=c++20`): iterator_concepts.h 616→969→974→done →
uses_allocator.h:81 (fix 6) → now **`<numbers>`:59 — VARIABLE TEMPLATE**
`template<typename _Tp> inline constexpr _Tp e_v = _Enable_if_floating<_Tp>(...);`
(frontier as of 84f1560). New tests: testusingchain, testfinalclass,
testinlineconstexprvar, testspecifierafterrettype. fulltest 644/5 (5 = known).

**NEXT = fix 7: VARIABLE TEMPLATES (C++14).** madc handles class/function/alias
templates + concepts, but NOT `template<...> [inline constexpr] T name = init;`
— so `<numbers>`'s `e_v`/`pi_v`/… never register and `inline constexpr double e =
e_v<double>;` fails "use of undeclared identifier 'e_v'". This is a larger feature
than fixes 1–6 (register a variable-template name; resolve `name<Arg>` uses;
madc need not fully evaluate the initializer). Reproduce: any of tmp/map_inc.mad,
tmp/str_inc.mad, tmp/ios_inc.mad at `--std=c++20`. The template-decl dispatch is
~parser.cpp:31403 (`class_kw = nextToken()`; handles concept/using/class/struct —
add a variable-template arm when what follows the `template<>` is a typed
declarator with `=`/`;`, not class/struct/concept/using).

**This means Wall C is tractable incrementally** — keep running
`./bin/madc --std=c++20 tmp/map_inc.mad` (also tmp/str_inc.mad, tmp/ios_inc.mad),
take the first error, reduce it to a tiny .mad, fix the bounded gap, fulltest,
commit, repeat — until `<map>`/`<set>`/`<string>`/`<iostream>` all parse at C++20,
then `contains` is exposed and testmap/testset can pass (modulo `contains`
compiling+running, likely trivial: `_M_t.find(k) != end()`). Each fix is a general
C++-conformance win independent of the container goal. NEXT: reduce the line-81
"Expecting member name in class definition" wall (identify the header via gdb on the
throw, or bisect includes) and continue.

## Update 12 — 3b causal model RESOLVED (clone-time collapse), exact mechanism still 1 layer down

gdb settled the contradiction from Updates 10–11. At `TokenTYPEDEF::parse` for
`__result_of_impl_0_0_Cmp__int32_t__int32_t_`'s `type` member, the base type
**arrives already collapsed to the identifier `__failure_type`** (gdb: base-id at
parser.cpp:25291 is `__failure_type`, NOT `decltype`; a SEPARATE typedef carries the
`decltype` base-id and is the lone `void*` the ~4390 branch resolves). Backtrace
confirms it is inside `__result_of_impl`'s instantiation (instantiate_template_use
frame). So `decltype(_S_test<_Functor,_ArgTypes...>(0))` is collapsed to
`__failure_type` DURING `__result_of_impl`'s body instantiation, BEFORE the typedef
is parsed — and NOT via the `resolve_declared_type_token` ~4390 decltype branch
(verified: that branch fires once, for `void*`). The clone loop in
instantiate_template_use (parser.cpp ~3275–3414) does pure token substitution with
no decltype resolver, so the collapse happens in a decltype-resolution path invoked
during the instantiation body re-parse that is NEITHER ~4390 NOR
reselect_static_member_overload NOR resolve_member_template_call_return_type (all
three verified silent/void* for `_S_test`). NEXT (one more focused step):
break `parser.cpp:25291` cond `tname=="__failure_type"`, then work BACKWARD (the
base token is already `__failure_type` here, next token id=60 — decode it) to find
where, during `__result_of_impl`'s instantiation, `decltype(_S_test<...>(0))` was
evaluated and the `_S_test(...)` failure overload chosen over `_S_test(int)`. That
is the 3b fix site; apply explicit-pack distribution so the success overload wins.
(3b gates testsubscript/testcontainerdtor/testmadc_ns — NOT testmap/testset, which
need Wall C; see Update 11.)

## Update 11 — VERIFIED per-test wall mapping (CORRECTS earlier assumptions)

Ran each failing test at HEAD (`48dde1e`) and read the FIRST error. The walls are
NOT distributed as the original diagnosis assumed:

| test | first error at HEAD | blocking wall |
|------|---------------------|---------------|
| `testmap`  | `Unidentified member 'contains'` (testmap.mad:25) | **Wall C ONLY** |
| `testset`  | `Unidentified member 'contains'` (testset.mad:19) | **Wall C ONLY** |
| `testsubscript`     | `undeclared '_S_key'` (stl_tree.h:427) | **Wall B (3b)** |
| `testcontainerdtor` | `expected 3 got 2` + `undeclared '_S_key'` (:96/:427) | **Wall B (3b)** |
| `testmadc_ns`       | `expected 3 got 2` + `undeclared '_S_key'` (:96/:427) | **Wall B (3b)** |

So **Wall B (3b) gates 3 of the 5 tests** (testsubscript, testcontainerdtor,
testmadc_ns) — landing it could turn 3 green (modulo further sub-walls). **Wall C
gates the 2 literally-named tests** (testmap, testset). testsubscript reaches
`_S_key` (not Wall A3 as previously assumed) because `ages["alice"]` (a const-char*
literal key, vs testmap's string-variable key) drives the operator[]/emplace path
that pulls `_Rb_tree::_S_key`'s `__is_invocable` static_assert.

**KEY:** `testmap` and `testset` — the literal Stop-hook goal ("set and map
working") — are blocked ONLY by `contains` (Wall C). Their construction,
`operator[]`, `insert`, and `size` ALL COMPILE now (after sub-walls 1/3a + the
prior tuple/pack/ref-member work). So for the two named containers, **Wall B is
NOT a blocker** — only Wall C is. Wall B (3b, the `_S_test`/`__is_invocable`
chain I drilled into across this session) blocks ONLY `testcontainerdtor` and
`testmadc_ns`.

**Wall C is confirmed LARGE and unavoidable for `contains`** (measured, not
assumed): `contains` is `#if __cplusplus > 201703L`, purely `__cplusplus`-gated
(NOT `__cpp_lib_*`), so the two-axis model can't expose it. `--std=c++20`
DOES expose the gate but pulls the full C++20 ranges/concepts surface —
`echo '#include <map>' | g++ -std=c++20 -E` shows `namespace ranges`,
`requires`-expressions, `ranges::swap`/`ranges::less`, etc.; madc errors
"Unknown namespace 'ranges'". So exposing `contains` REQUIRES a real C++20
ranges/concepts bring-up (its own multi-session track), OR a legitimate
mechanism to present C++20 to the container headers without the ranges surface
(no clean one exists today — a single `__cplusplus` value can't separate
`contains` from ranges). Do NOT hand-roll a `contains` shim (violates
retire-embedded-shims; the method must come from the real header).

CONSEQUENCE for sequencing: to make the two NAMED containers pass, the priority
is Wall C (C++20/ranges), not Wall B. Wall B remains the right next step only for
testcontainerdtor/testmadc_ns. Neither is a single-session item.


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

---

## Update 30 (2026-06-18) — Wall B CLEARED (`__is_invocable`) + `__is_pod` builtin added; map/set now blocked on a C++20-header wall set

Three deepest-layer fixes this session (all root-caused via probes on the REAL
`<type_traits>`/`<map>` parse, gcc-verified, fulltest + torture-failset clean):

1. **`4da86a0` — partial-spec matching absorbs a trailing parameter pack.**
   `match_partial_specialization` arity-gated on `spec_pattern.size() ==
   arg_spellings.size()`, wrong when the LAST pattern slot is a pack (`_Types...`).
   `__result_of_impl<false,false,_Functor,_ArgTypes...>` (4 slots) vs the concrete
   `<false,false,Cmp*,int*,int*>` (5 args) was arity-rejected → fell to the primary
   `typedef __failure_type type;` → `__invoke_result` had no `::type` → `__is_invocable`
   false. Fix: detect a trailing pack slot, relax the gate to `arg_arity >= fixed`,
   match only the fixed leading slots, absorb the tail into `pack_ded` (consumed by
   the existing `spec_pack_subst -> pack_subst` body expansion).

2. **`14bdc63` — `resolve_arg_spelling_datadef` resolves `T*`/`T&` pack elements.**
   The absorbed pack element spelling `"int32_t*"` resolved to NULL (the fn only
   matched a bare name, else required `<`), so `pack_subst` collapsed to empty, the
   class body DROPPED `_ArgTypes...`, and `_S_test<Cmp*>(0)` called the comparator's
   `operator()` with ZERO args ("expected 3 got 1"). Fix: peel trailing `*`/`&`/`&&`
   + leading cv, resolve the core, re-apply ptr/ref. `tests/testinvocable.mad` added.

   Net: `__is_invocable<F, Args...>` folds true for a callable functor (`Cmp`,
   `std::less<int>`, `std::less<string>` all verified). **Wall B is cleared.**

3. **`90ad946` — `__is_pod` / `__is_standard_layout` builtin type traits.**
   `_Rb_tree_impl`'s defaulted `bool = __is_pod(_Key_compare)` (stl_tree.h:660)
   threw "Expecting integer constant expression" → `_Rb_tree` incomplete → `_S_key`
   undeclared. Added a faithful `trait_is_standard_layout` (uses the already-tracked
   `member_access` + `BaseSpec::is_virtual`) and `trait_is_pod = trivial &&
   standard_layout`. gcc-verified. `tests/testispod.mad` added.

fulltest **655/5/0/18** (the two new tests pass), torture failset **51 unchanged**.

### REMAINING walls for `map<int,int>` at `--std=c++20 --no-embedded-headers`
Pinned via the WB-const probe on the real parse; these are DISTINCT C++20-header
walls beyond Wall B (the campaign at c++20 drags in the whole C++20 ranges support):

- **`numbers`/`digits` const-fold (`bits/max_size_type.h:802-804`).** `static
  constexpr int digits = numeric_limits<_Sp>::digits - 1;` and `digits10 = ... *
  numbers::ln2 / numbers::ln10` — madc's constant evaluator can't fold
  `std::numbers::ln2` (constexpr var template) or `numeric_limits<>::digits` in
  these specializations for `ranges::__detail::__max_size_type/__max_diff_type`.
- **Non-fn-ptr parenthesized member declarator** (`parser.cpp:21905` /
  `24730`): a struct member `Type (name)...` whose inner token isn't `*` is
  rejected; some C++20 header member uses this shape.
- **`__detail::…` / `is_object` / `__assignable` in constant context** (iterator
  concepts, stl_iterator.h:141, tuple:746) — concept/requires constant evaluation.
- **NEGATIVE-case `__is_invocable` (correctness, does NOT block map/set since
  comparators are always callable):** `__is_invocable<NonCallable, …>` wrongly
  folds TRUE — the `_S_test(int)` decltype must SFINAE-fail (ill-formed call) and
  fall to `_S_test(...)` → `__failure_type`. Needs proper SFINAE on member-template
  decltype-overload fallback. Reducer `tmp/nocall.mad`.

Reducers (run with `--std=c++20 --no-embedded-headers`): `tmp/invoc2.mad`
(now compiles), `tmp/invocless.mad`, `tmp/mapii.mad`, `tmp/podtest.mad`.

---

## Update 31 (2026-06-19) — map PARSE phase fully cleared (Wall B `_S_key` RESOLVED); now at CIR codegen ctor-resolution

Four more deepest-layer fixes (all probed on the REAL `<map>`/`<type_traits>`
parse, gcc-verified, fulltest + torture-failset clean, pushed). `map<int,int>`
(`m[1]=2`) now parses end-to-end and reaches CIR codegen.

1. **`0db5133` — variable-template trailing parameter pack.** The var-template
   use-site arms substituted typeparams 1:1, so `is_invocable_v<_Fn, _Args...>`
   left `_Args...` unexpanded → `is_invocable<_Fn, int...>` → "Expecting ',' or
   '>'". Added `VarTemplateDef::typeparam_is_pack` + `substitute_var_template_init`
   + `clone_template_tokens_with_pack_subst` (4th trailing-pack-absorption site).
2. **`0db5133` — braceless `if constexpr (C) static_assert(...);`.**
   `parse_static_assert_statement` returns NULL on success (compile-time, no
   node); the if-constexpr taken/else branch treated NULL as a parse failure.
   Exactly `_S_key`'s shape (stl_tree.h:770). A clean NULL is now an empty branch.
3. **`8f03905` — deferred method body restores the full ENCLOSING class scope.**
   THE Wall B resolution. A lazily-materialized body restored only the namespace,
   not `class_scope_stack`. `_Rb_tree::_Auto_node`'s body calls `_Rb_tree::_S_key`
   (an OUTER-class static, no `this`); with no class scope it was "undeclared
   '_S_key'". Fix: walk `owner_class->enclosing_class` and push the whole chain.
4. **`d3a6226` — member access on a `ttMultiOp` result.** The `.`/`->` arm
   accepted `ttOperator` objects but not its sibling `ttMultiOp` (`!=`, `<=>`,
   overloaded `operator*`/`++` …). `__j._M_node` on an iterator from a multi-op
   was rejected; now treated identically to `ttOperator`.

CORRECTS the earlier "missing partial-spec registration" and pack-drop
hypotheses — Wall B was a chain of: arity gate (Update 30) → arg-spelling NULL
(Update 30) → __is_pod builtin (Update 30) → var-template pack → if-constexpr
NULL → **deferred-body enclosing scope** (the keystone) → ttMultiOp member access.

fulltest **656/5/0/18**, torture failset **51 unchanged** throughout.

### NEW FRONTIER — CIR codegen constructor overload resolution (reference-repr)
`map<int,int>` now fails at CIR build (`cir_builder.cpp` select_ctor_overload →
score_arg_to_param), NOT parse:
- `cir error: no matching constructor for call to 'tuple<const int32_t>(int32_t*)'`
  (map::operator[]'s `std::tuple<const key_type&>(__k)`) — a reference-repr'd
  `int*` arg must bind a `const int&` param.
- `pair<_Rb_tree_node_base*, _Rb_tree_node_base*>(node**, node**)` (×2 shapes).
- `_Auto_node(_Rb_tree&...)` no matching ctor.
These are the reference-as-pointer representation (first-class refs) interacting
with ctor-argument scoring — `score_arg_to_param` (cir_builder.cpp:4815) unwraps a
ref PARAM to base but doesn't reconcile a ref-repr POINTER arg against it. Multiple
distinct ctor shapes → a focused codegen-level effort. Reducer: `tmp/mapii.mad`
(`map<int,int>; m[1]=2;`), `--std=c++20 --no-embedded-headers`.

### Update 31-B — the CIR ctor frontier is TWO distinct root causes (probed)
A `MADC_DBG_WB` probe in `select_ctor_overload` (dumping candidate params vs args
on NO-MATCH) pinned exactly two issues for `map<int,int>; m[1]=2;`:

1. **Implicit copy constructor not matched.** `in_place_t`, `piecewise_construct_t`,
   `_Rb_tree_iterator<...>`, `pair<...>` are constructed from a SAME-CLASS arg
   (`args=[in_place_t]` etc.), but the class's only ctors are the default (0 user
   params) and/or a non-copy ctor — so `select_ctor_overload` finds no match. madc
   must match (or synthesize) the implicit copy ctor for same-class construction
   when no explicit copy ctor exists. (Affects the tag-type globals + iterator copies
   pervasively.)

2. **Reference-to-pointer param unwrap goes one level too deep.**
   `tuple<const int32_t>` has a forwarding ctor whose param prints as `int32_t*&`
   and the arg is `int32_t*` (the reference-repr of `const int&`); `pair<node*,node*>`
   has `node**&` params vs `node**` args. `score_arg_to_param` (cir_builder.cpp:4824)
   unwraps a ref param via `DataDefPTR::base_type` — stripping `int32_t*`→`int32_t`,
   one level too far for a reference-TO-pointer. It also does not symmetrically
   unwrap a reference-repr ARG. Fix likely: unwrap via `referent_if_reference` for
   BOTH param and arg (first-class-refs DataDefREF), not ad-hoc base_type stripping —
   but first confirm whether these params/args carry DataDefREF or a DataDefPTR+ref-flag
   (the probe showed the NAME `int32_t*` + a separate is_ref_param flag, so the repr
   distinction needs checking before the fix).

Both are needed for map/set. Start with #2 (more contained) using reducer
`tmp/mapii.mad`; #1 (implicit copy ctor) is the broader one.

### Update 32 — CIR ctor frontier: reference-unwrap FIXED (6ffabf2); 5→1 ctor errors. Last blocker = variadic member-template ctor.

The "two root causes" of Update 31-B were really ONE defect: the constructor
ARGUMENT side unwrapped references inconsistently with the parameter side
(`score_arg_to_param` unwraps a `T&` param to its referent T, line 4824).

**`6ffabf2` — ctor-arg reference unwrap is exactly one level.** Two spots:
- `ref_returning_call_type` (cir_builder.cpp:1069) DOUBLE-unwrapped: `cfd->
  return_value_type()` already returns the referent, then an unconditional
  `rp->base_type` strip removed a second level — a `base*&`-returning call
  (`_M_rightmost`) resolved to `base` instead of `base*`. Gated the strip on
  `r->is_reference()` (preserve a pointer referent).
- `ctor_arg_datadef` TokenVar branch (cir_builder.cpp:4978) unwrapped only
  CLASS references (via `class_behind`, which digs through a pointer to the
  class), leaving a scalar `const int&` arg as `int*`. Replaced with a single
  reference-level unwrap (`DataDefREF::base_type`) for ANY referent.

Probed live on the real `<map>` parse with `-DMADC_DEBUG_CTORINIT`. Cleared
4 of 5: `tuple<const int&>(const int&)` and ALL THREE `pair<base*,base*>`
shapes (the `int` first-arg was a null-pointer literal, binds via the
zero-literal rule). fulltest 656/5/0/18 (zero regression); gcc.c-torture
failset 51 names UNCHANGED (C has no class ctors — this path is C++-only).

**LAST blocker (1/5) — `_Auto_node` variadic member-template ctor never
instantiated.** stl_tree.h:1635 `template<typename... _Args> _Auto_node(
_Rb_tree& __t, _Args&&... __args)`, constructed at stl_tree.h:2434 inside
`_M_emplace_hint_unique` with `_Args = {piecewise_construct_t,
tuple<const int&>, tuple<>}`. The probe shows `_Auto_node` has nctors=1 (only
the implicit copy/move ctor `o2(_Auto_node*)`); the variadic ctor is absent
from `cdd->ctors`. ROOT (parser): a member template ctor whose params USE the
template pack is NOT handled by `try_parse_defaulted_member_template_constructor`
(parser.cpp:23538 bails when the declarator uses any template-param name — it
only takes the degenerate "params are concrete" case). It instead falls to
`register_skipped_class_template_function` (parser.cpp:31666), which registers
it as a member-template METHOD named `_Auto_node` (is_member_template, retained
body) in `owner->methods`/`method_map` — but **never pushes it to
`owner->ctors`**, so `select_ctor_overload` (which iterates `cdd->ctors`) can't
see it.

DESIGN (two parts, both needed):
- **Part A (parse, contained):** in `register_skipped_class_template_function`,
  when the declarator name equals the class's constructor name, also push the
  var to `owner->ctors` and set `has_user_ctor`. Makes the variadic ctor
  discoverable as a ctor.
- **Part B (construction-site instantiation):** an object declaration
  `_Auto_node __z(args…)` is NOT a `TokenCallFunc`, so the existing
  `instantiate_member_fn_template_for_call` hook (call-only) never fires.
  Need to: at the construction site (parse time — the enclosing
  `_M_emplace_hint_unique` deferred body is instantiated with concrete `_Args`,
  so the construction args' types ARE known), detect that the only viable ctor
  is a variadic member-template ctor, deduce the pack from the construction
  args, instantiate a concrete ctor (mirror `instantiate_member_fn_template_for_call`,
  which already honors `template_param_is_pack`), add it to `ctors`, and select
  it. Find the object-decl ctor-construction parse path first; that is Part B's
  hook. Reducer: `tmp/mapii.mad` (`map<int,int>; m[1]=2;`), `--std=c++20
  --no-embedded-headers`. Probe: rebuild cir_builder.o with
  `OPTIONAL_CPPFLAGS=-DMADC_DEBUG_CTORINIT`.

### Update 33 — variadic member-template CTOR instantiation LANDED (99f089b); map clears CIR, now at c2mir.

The last CIR ctor blocker (Update 32) is fixed. `_Auto_node`'s variadic
member-template ctor (`template<class... A> _Auto_node(_Rb_tree&, A&&...)`,
stl_tree.h:1635, constructed at :2434 in `_M_emplace_hint_unique`) is now
instantiated on demand.

**Two parts (one commit, 99f089b):**
1. `register_skipped_class_template_function` now also pushes the placeholder to
   `owner->ctors` (+ `has_user_ctor`) when the declarator names the ctor — a
   member-template ctor that USES its pack lands here (the defaulted-ctor path
   bails on pack-using declarators). The varargs declaration-only placeholder is
   harmless in `select_ctor_overload` (its tiny arity fails the `args > pn` gate).
2. New `Program::instantiate_member_ctor_template_for_construction(cdd, ctor_args)`
   — deduces the ctor's template params from the construction args and
   instantiates the retained body under a CONCRETE ctor symbol
   (`ClassName__ClassName__oN`, so the mem-init list parses + it is recognized as
   a ctor), then registers that instantiation as a real ctor. Reuses the shared
   free-fn-template machinery via a synthetic `TokenCallFunc` carrying the args; a
   `void` return is synthesized before the declarator (ctors have none, but
   `extract_free_signature` requires one — that was the first try_instantiate=0).
   Memoized per (class, arg types) → also breaks the construct→forward→construct
   recursion. Wired into all 3 construction parse paths (the local-var `TokenDecl`
   form + both `TokenObjTemp` functional-construction forms).

`map<int,int>; m[1]=2;` now passes the madc CIR gate (0 untranslatable, was 1)
and reaches **c2mir**. fulltest 656/5/0/18 (zero regr); torture failset 51 unchanged.

**NEXT FRONTIER — c2mir check errors (6).** The dominant one: `undeclared
identifier __op_expr` (×2, stl_tree.h:427:18). `__op_expr` is the synthetic
rvalue-member-access receiver (parser.cpp:16563 / 12605) for member access on an
operator / functional-construction temp; its `parent_expr` materialization is not
reaching the c2mir tree in `_M_get_insert_*_pos`'s pair-returning member access
(NOT the _Auto_node ctor). Also: `incompatible argument type for pointer type
parameter` (many, the pair<base*,base*> construction) and an
`incompatible return-expr type` warning (node_handle.h:64). Plus the pre-existing
recovered parse diagnostics (791/817 Expecting-integer-constant-expression, 141
undeclared-std). Reducer `tmp/mapii.mad --std=c++20 --no-embedded-headers`.

### Update 34 — __op_expr leak FIXED (acb9906); map c2mir errors 6→2. Last 2 = reverse_iterator's std::iterator base-clause `std`.

`acb9906` — wire parent_expr for member access on a ttMultiOp rvalue. The
d3a6226 ttMultiOp fix added ttMultiOp to the member-access ACCEPT list but not
to the parent_expr WIRING (both the data-member TokenMember construction and the
method-call recv_parent list). So `(--__j)._M_node` (the _Rb_tree iterator
decrement in `_M_get_insert_*_pos`) got no parent_expr and CIR emitted its
synthetic `__op_expr` receiver as a bare undeclared identifier. Added the
operator-rvalue cases (ttOperator/ttMultiOp/tkObjTemp) to both wirings.
`map<int,int>; m[1]=2;` c2mir check errors 6→2; fulltest 656/5/0/18; torture 51.

**LAST 2 c2mir errors localized — `undeclared identifier 'std'` at
stl_iterator.h:141:69** (found via a throwbuf::sync probe printing the token's
OWN file, not the source file the printer shows). Line 137-141 is
`reverse_iterator`'s base-clause `: public iterator<typename
iterator_traits<_Iterator>::iterator_category, …, …::reference>` — the deprecated
`std::iterator` base. madc's expression-context identifier resolver fails on
`std` while processing this base-clause; the Throw is RECOVERED (compilation
continues to c2mir). NOTE: not yet confirmed whether these 2 recovered parser
diagnostics ARE the 2 c2mir check errors or independent noise — and reverse_iterator
may only be instantiated via map's `typedef reverse_iterator<…>` (not actually
needed for `m[1]=2` codegen). NEXT: confirm the link, then fix the base-clause
`std`/std::iterator resolution (or suppress the unused reverse_iterator
instantiation). Reducer tmp/mapii.mad --std=c++20 --no-embedded-headers; probe
the real token file via throwbuf::sync (the printer shows the SOURCE file, not the
token's origin header).

### Update 35 — CORRECTION + precise localization of the last 2 errors: alias-template instantiation, NOT base-clause.

A backtrace probe in `throwbuf::sync` (filtering OUT muted constraint-eval frames
— `constraint_expression_well_formed` / `evaluate_requires_expression_constant`
mute cerr + catch, so they don't leak) pinned the TWO non-muted `undeclared
identifier 'std'` errors to:

`TokenUSING::parse → resolve_declared_type_token → instantiate_template_id →
instantiate_template_alias_use → resolve_declared_type_token → parseExpression →
parseExpr_operatorArm` — where bare `std` fails to resolve.

So the real blocker is an **alias-template instantiation** (`using` alias in the
C++20 `<iterator>` machinery, parsed at `#include <iterator>` — a clean
deterministic reducer `tmp/itonly.mad`, NOT instantiation-specific): while
resolving the alias's TARGET type, `resolve_declared_type_token` invokes
`parseExpression`, whose operator arm sees `std` (of a `std::X` in the target) as
a bare operand and throws "undeclared identifier 'std'". NOT muted (in_constraint=0)
→ prints + leaves an error node → the 2 c2mir check errors. (Update 34's
"reverse_iterator base-clause" guess was the misattributed token location
stl_iterator.h:141:69, corrected here by the backtrace.)

CONFIRMED: the 2 c2mir check errors ARE these 2 `std` errors (the only ERRORS;
everything else from c2mir is a tolerated warning). So fixing `std::` resolution
in the alias-template target parse should let `map<int,int>; m[1]=2;` compile.

NEXT: in `instantiate_template_alias_use` (parser.cpp:3764) the alias TARGET is
resolved via `resolve_declared_type_token`→`parseExpression` in an isolated token
stream; `std` (the top-level namespace) is not recognized there — likely the
namespace-scope (`std::`) handling in `parseExpr_operatorArm` isn't engaging in
that context (it treats `std` as a value identifier before consuming the `::`).
Probe: re-add the cerr-muting-aware backtrace in throwbuf::sync (lexer.cpp:5407)
+ `#include <execinfo.h>`; build with `CXXFLAGS="-std=c++11 -Wall -g -O0"` for
symbols; gate on `getenv("MADC_DBG_STD")`. Reducer `tmp/itonly.mad` (just
`#include <iterator>`), `--std=c++20 --no-embedded-headers`.

### Update 36 — CORRECTION (Update 35 was WRONG) + lvalue error fixed (ea6ffb9); map c2mir 2→1.

Update 35's claim — "the 2 c2mir check errors ARE the 2 `undeclared identifier
'std'` errors" — is **FALSE**. Verified at live HEAD: `tmp/itonly.mad`
(`#include <iterator>`) **exits 0** while printing the *identical* `undeclared
identifier 'std'` noise. So that "std" output is RECOVERED parser noise, not a
fatal error. (Root of the noise, for the record: the alias
`iter_reference_t = decltype(*std::declval<It&>())` — the deref-operand path in
`parseExpr_operatorArm` routes `*std::declval<…>()` to `parseExpression(std)`
instead of the namespace-aware `parsePostfixChain`, so `std::declval<It&>()`
[note the explicit template args] isn't resolved as a qualified call. It's
caught at `instantiate_template_alias_use`'s try/catch and recovered to the
opaque placeholder; harmless. Lower priority — cosmetic.)

The REAL 2 fatal c2mir errors for `map<int,int>; m[1]=2;` (found by emitting
`--emit=c11` and compiling the C with **gcc -c**, which is stricter than c2mir
and names them precisely):

1. **`lvalue required as unary '&' operand`** (mapii.mad:3) — `m[1]` bound the
   prvalue literal `1` to `operator[](const key_type&)` and madc emitted
   `operator_lb_rb(&m, &1)` — the address of a literal. **FIXED in ea6ffb9**:
   `class_subscript_addr_on` (cir_builder.cpp:7198) now routes the reference
   index through `ref_param_arg_addr` (the existing helper that materializes a
   temp for non-addressable rvalues), so it emits `&__madc_objtmp_N`. c2mir
   errors **2→1**. fulltest 656/5/0/18, zero regr.

2. **`incomplete struct or union`** (stl_map.h:102 per c2mir; gcc localizes it
   to `storage size of '__madc_objtmp_5' isn't known`) — `std::tuple<const int&>`
   (emitted name `tuple_const_int32_t_`, from `forward_as_tuple(__k)` inside
   `operator[]`) is referenced as a local but its struct body is NEVER emitted:
   the variadic `tuple` class template stays an opaque body-less shell for the
   `<const int&>` arg. **This is the same wall the vector<T*> work cleared**
   (eb578af: `allow_variadic_real_inst` for concrete trailing-pack class
   templates). NEXT = extend that real-instantiation to `tuple<const int&>`.

METHOD LESSON: `--emit=c11` + `gcc -c` on the emitted C is the fastest way to
get *precise, well-located* error messages for a JIT-path c2mir failure — gcc is
stricter and labels the exact incomplete type / bad operand, where c2mir only
says "incomplete struct or union" at a misleading location. (Also surfaced a
gcc-strict-only duplicate/conflicting proto for the `_Auto_node` variadic ctor +
`_M_emplace_hint_unique` that c2mir tolerates — separate cleanup, not gating.)

### Update 37 — remaining map blocker fully root-caused: std::tuple<const int&>'s _Tuple_impl base needs mixed non-type+type-pack partial-spec real-instantiation.

Instrumented `instantiate_template_use` (gate flags + parsed args + post-parse
class size/members/base; #ifdef MADC_DEBUG_TUPLE). Findings for `map<int,int>;
m[1]=2` (HEAD ea6ffb9, c2mir at 1 error):

- `std::tuple<const int&>` (emit name `tuple_const_int32_t_`, from
  `forward_as_tuple(__k)`) IS real-instantiated (pack_real_inst=1, registered,
  dep_ph=0) — NOT opaque. But it has **size=0, 0 direct members, ONE base
  `_Tuple_impl_0_int32_t_`**. The size-0 base is the incompleteness.
- That base is `std::_Tuple_impl<0, const int&>` — a MIXED **non-type**
  (`size_t _Idx`) + trailing **type-pack** template whose real body lives in a
  PARTIAL SPECIALIZATION `_Tuple_impl<_Idx, _Head, _Tail...>`. At its
  instantiation point (resolving tuple's base clause):
  - `pack_real_inst=0` — `template_pack_real_instantiable` rejects it for the
    non-type `_Idx` param; it must use the partial-spec path
    (`try_spec_real_inst = (allow_vri||sticky) && partial_spec_map.count`).
  - Even forcing sticky ON through tuple's body (broadened
    `want_sticky=pack_real_inst` probe), `_Tuple_impl` stayed **OPAQUE**: its
    primary is a body-less forward decl (`body_empty=1`), and the arg loop
    captured only `<0>` — it **DROPPED the `int32_t*` type-pack element** — so
    the partial spec never real-instantiated.

So the remaining work is SUBSTANTIAL (a proper feature, not a one-liner), and
is the same class of variadic-instantiation depth as the vector<T*> wall:
1. Arg loop must absorb a trailing TYPE pack alongside a leading NON-TYPE arg
   (`_Tuple_impl<0, const int&>` → _Idx=0, pack={const int&}).
2. Match + real-instantiate the `_Tuple_impl<_Idx,_Head,_Tail...>` PARTIAL SPEC
   body for a mixed non-type/type-pack shape.
3. Recursive base layout: `_Tuple_impl<0,H,T...> : _Tuple_impl<1,T...>,
   _Head_base<0,H>` — instantiate the chain + compute size.
4. Likely also make `want_sticky` engage for a DIRECT variadic-template base
   (currently only dependent-member `…>::` bases), so the base clause of a
   real-instantiated variadic template propagates real-instantiation.

Each step is regression-prone (the real-inst flags are deliberately narrow) —
design carefully and run fulltest per step. The `--emit=c11` + `gcc -c` recipe
(Update 36) localizes the result precisely. Debug recipe preserved in task #4.

### Update 38 — last map error pinpointed to ONE function: non-type partial-spec param deduction.

Drilled the `incomplete struct or union` all the way down (instrumented
`instantiate_template_use` + `match_partial_specialization`):

`std::tuple<const int&>` → base `std::_Tuple_impl<0, const int&>` → that base
is a MIXED non-type (`size_t _Idx`) + type-pack template whose real body lives
in the partial spec `_Tuple_impl<_Idx, _Head, _Tail...>`. The spec is REJECTED
by `match_partial_specialization` because **`non_type_partial_spec_arg_matches`
(parser.cpp:15073) cannot DEDUCE a non-type parameter** — it only succeeds when
the pattern spelling equals the concrete spelling, or when both
`parse_simple_template_non_type_value` to the same literal. For the `_Idx` slot
(pattern `_Idx` vs concrete `0`): `"_Idx" != "0"` and
`parse_simple_template_non_type_value("_Idx")` fails (it is a param name, not a
literal) → returns false → `ok=false` → the spec is dropped → `_Tuple_impl`
stays the body-less primary → opaque shell → tuple size 0 → incomplete.

(enable_if/`__result_of_impl` partial specs work because their non-type slots
hold CONCRETE values (`true`, `false`) in the pattern, not a deduced non-type
PARAM. `_Tuple_impl` is the first case needing real non-type-param deduction.)

FIX (multi-part, all touching shared partial-spec machinery → regression-prone,
fulltest each step) — see task #4 for the detailed 5-step plan:
1. `non_type_partial_spec_arg_matches`: deduce a bare non-type param-name slot.
2. `match_partial_specialization`: thread + check + return a non-type deduction map.
3. caller (~3324): apply the non-type deduction into the body subst.
4. `want_sticky` (3071): engage for a DIRECT variadic-template base, so
   real-instantiation reaches `_Tuple_impl` during tuple's base-clause parse.
5. recursive `_Tuple_impl` base layout + size.

This is a focused next-session feature. Session net so far: map c2mir 6→1
(ea6ffb9 fixed the lvalue/&literal error), remaining error pinpointed to one
function. (NB: a throwaway probe's arg-print bug — iterating `type_args.size()`,
which excludes non-type args — briefly suggested the arg loop dropped the pack;
the mangled name `_Tuple_impl_0_int32_t_` proved both args ARE captured. Index
`arg_spellings.size()` when printing.)

### Update 39 — tuple<const int&> instantiation: full path mapped (4 layers), but REVERTED (regressed testtuple.mad). Roadmap for next session.

Implemented and tested the variadic/partial-spec instantiation needed for
`std::_Tuple_impl<0, const int&>` (tuple<const int&>'s base). It DROVE map's
c2mir errors to 0 (the `incomplete struct` disappeared) and the frontier moved
4 layers deep into the recursive hierarchy — but it **regressed
testtuple.mad** (fulltest 655/6 vs baseline 656/5), so it was **reverted**
(HEAD back at the clean committed state; ea6ffb9's lvalue fix retained). The
changes are correct-DIRECTION but not correct-as-is. Precise roadmap:

The recursion is `tuple<const int&>` → base `_Tuple_impl<0, const int&>` →
bases `_Tuple_impl<1>` (empty tail) + `_Head_base<0, const int&>` (stores the
element). Four fixes, each unblocking the next layer (ALL needed together):

1. **Non-type partial-spec param DEDUCTION** — `_Tuple_impl`'s real body is the
   partial spec `_Tuple_impl<_Idx, _Head, _Tail...>`;
   `non_type_partial_spec_arg_matches` (parser.cpp:15073) only matched CONCRETE
   non-type values, never DEDUCED a non-type param (`_Idx`). Fix: in
   `match_partial_specialization`, when a non-type slot's pattern is a bare
   non-type spec typeparam, record the concrete arg tokens into a new
   `nontype_ded` map; add it to the "all typeparams deduced" check; thread a new
   out-param `out_nontype_subst` (also update the header decl + the single call
   site ~3311) and apply it into `token_subst` in the caller (~3374) so the body
   specializes (`_Idx`->`0`, and `_Idx+1` folds).
2. **want_sticky from the SPEC body** — `want_sticky` (3071) was computed from
   the body-less PRIMARY (no base clause) → false, so the spec body re-parsed
   without sticky and its bases went opaque. Fix: recompute `want_sticky` right
   after the spec swap (`td = *spec`, ~3368) from the spec's body.
3. **want_sticky for a DIRECT template-id base** — new predicate
   `template_base_clause_has_template_id(td)` (a `<` after the base-intro `:`
   before the body `{`); OR it into `want_sticky` so real-instantiation stays on
   for a variadic/partial-spec base (`_Tuple_impl<...>`), not only
   dependent-member `…>::` bases.
4. **Recursive self-template BASE kept distinct** — the self-name rename loop
   (~3580) collapsed `_Tuple_impl<_Idx+1,_Tail...>` (a DIFFERENT specialization)
   into the mangled self → "Unknown base class". Fix: precompute
   `body_brace_index` (first top-level `{`); when the self-name is in the BASE
   CLAUSE (`bi < body_brace_index`) and followed by `<`, emit the class_name
   as-is and DON'T swallow the `<...>` — let the clone loop substitute the args
   so it re-parses as a fresh template-id (instantiates the right base). Body
   self-template-ids stay collapsed as before.

With all four, `_Head_base_0_int32_t__0` registered as a complete CLASS (size 8,
1 member — the element), `_Tuple_impl_1` went opaque (correct empty tail base),
and the remaining error became `Failed to find type when parsing function
parameters` inside `_Tuple_impl<0,...>`'s body — i.e. LAYER 5: `_Tuple_impl`'s
MEMBER FUNCTIONS (ctors / `_M_head` / `_M_tail`, signatures involving the pack
and `_Head`/`_Tail`) don't all resolve. That member-set instantiation is the
next layer.

WHY REVERTED: regressed testtuple.mad. The likely culprit is fix #4 (changing
self-template-base handling) or #3 (broadening want_sticky) altering an existing
tuple instantiation path. NEXT SESSION: re-apply the four fixes, then (a) narrow
#3/#4 until testtuple.mad passes again (diff the testtuple emission before/after
each fix to find which one breaks it), (b) tackle layer 5 (member-fn param
resolution). Debug recipe: #ifdef MADC_DEBUG_TUPLE prints keyed on tname in
{tuple,_Tuple_impl,_Head_base} at the gate / opaque-gate / body-parse / post-
parse (size,members,base). The exploratory patch was reverted but this Update
records every edit precisely enough to re-apply.

### Update 47 — layer-9 `map<int,int>` segfault ROOT-CAUSED + FIXED: it was INFINITE RECURSION (`_Tuple_impl<0,_Head>` ctor self-delegation), not a wild pointer. Now layer 10 = node-pointer args to the REAL `_Rb_tree_insert_and_rebalance`.

**CORRECTION of the Update 45/46 hypothesis.** The `map<int,int>; m[1]=2`
runtime crash was NOT (primarily) the node-pointer/base-derived conflation those
updates chased — it was a **STACK OVERFLOW from infinite recursion**. gdb on the
crash: `rsp` sat at the exact bottom of the 8 MB `[stack]` mapping and the stack
was filled with ONE return address repeated every 16 bytes (the recursion
signature), faulting on a `call *%rax`.

**Tooling (kept — a permanent debugger improvement).** madc's crash handler
(`src/madc.cpp`) could not run on a stack overflow: no alternate signal stack, so
it re-faulted on the exhausted stack and the kernel killed us silently with no
backtrace. Added `sigaltstack` + `SA_ONSTACK` in `install_crash_handler()`. The
handler now survives the overflow and symbolizes the faulting JIT frame via
`madc_jit_symbolize` — it named the culprit immediately:
`_Tuple_impl_0_int32_t____Tuple_impl_0_int32_t_+0x11 [JIT]` (the default ctor of
`_Tuple_impl<0,const int&>`, recursing at the base-delegation).

**ROOT CAUSE.** `map::operator[]` builds the key via `tuple<const int&>` =
`_Tuple_impl<0, const int&>` (the PRIMARY `_Tuple_impl<_Idx,_Head,_Tail...>` with
`_Tail` empty). Its member typedef `typedef _Tuple_impl<_Idx+1, _Tail...>
_Inherited;` is the EMPTY `_Tuple_impl<1>` recursion-terminator. During
instantiation the clone-time self-name collapse (instantiate_template_use)
collapsed `_Tuple_impl<_Idx+1,_Tail...>` in the BODY to the injected-class-name
and SWALLOWED its `<...>` args — so `_Inherited` resolved to the OWNER itself. The
ctor mem-init `: _Inherited(), _Base()` then matched `_Inherited` as a DELEGATING
constructor (`find_delegating_initializer` → owner), emitting a `__this`-on-`__this`
self-call → infinite recursion → stack overflow. Confirmed with the existing
`MADC_DEBUG_CTORINIT` instrumentation (added `base-construct` + `DELEGATING-EMIT`
traces): `DELEGATING-EMIT owner=_Tuple_impl_0_int32_t_ ci=_Inherited` for both the
default and head-taking ctors (the head ctor likewise never stored its `__head`).

**FIX (`src/parser.cpp`, `instantiate_template_use` clone loop + new static helper
`self_template_id_keep_distinct`).** Per [temp.local], a self-name template-id is
the injected-class-name (collapse to the mangled current type) ONLY when its args
are the template's OWN parameter list. A self-name with DIFFERENT args is a
distinct specialization. The body now keeps such a self-id DISTINCT (re-instantiate)
when its args (a) differ from the params and (b) are expressed PURELY in the
class's own template parameters (concretely substitutable, e.g.
`_Tuple_impl<_Idx+1,_Tail...>` → `_Tuple_impl<1>`). The true injected-class-name
(`tuple<_Tp>` == own params) and self-ids referencing an OUTER/dependent name
(`_UseOtherCtor<_Tuple, tuple<_Up>>`, `_Tuple_impl<_Idx,_UElements...>`) STILL
collapse — conservative, because re-instantiating a dependent self-id crashes type
resolution. The base-clause path (`self_template_base`) is unchanged.

Verified: the self-call is GONE; `_Tuple_impl<0,const int&>` now constructs its two
real bases (`_Tuple_impl<1>` empty terminator + `_Head_base<0,const int&>`); the
stack overflow is eliminated.

**NEW FRONTIER (layer 10).** With recursion gone, `map<int,int>; m[1]=2` now
NULL-DEREFS inside the REAL libstdc++ `std::_Rb_tree_insert_and_rebalance`
(`_ZSt29_Rb_tree_insert_and_rebalance...+0x33`, resolved mangled-direct), called
from the madc-instantiated `_M_emplace_hint_unique`. This IS the node-pointer
correctness Update 45/46 flagged: the `_Base_ptr`/`_Link_type` node-pointer args
(the freshly allocated node / its parent / the header) are wrong-typed or null.
NEXT: trace the node allocation (`_M_create_node`/`_Auto_node`) + `_M_insert_node`
args feeding rebalance.

**testtuple regression status.** testtuple was ALREADY red on the wip branch before
this work (the wip layers 1-9 regressed it, 655/6 per Update 44). With this fix its
failure mode is `repeated declaration _M_head_impl` on `tuple<int,int>`: the
MULTI-element `_Tuple_impl<0,int,int>` flattens base `_Tuple_impl<1,int>`'s
`_M_head_impl` AND `_Head_base<0,int>`'s `_M_head_impl` into one struct (name
collision). Orthogonal to the map/set goal (map needs only 1- and 0-element
tuples); a separate sub-task to get testtuple green before the wip→feature squash.

**fulltest (clean build, warning-free): 655 passed, 6 failed, 0 timed out, 18
skipped** — the 6 fails are EXACTLY the wip baseline (testcontainerdtor,
testmadc_ns, testmap, testset, testsubscript, testtuple). ZERO new regressions
from the parser change. gcc-torture is unaffected by construction: the change
lives entirely inside `instantiate_template_use` (C++ class-template
instantiation), which C torture tests never invoke — full torture failset-diff
deferred to the wip→feature squash gate.

NOTE on branch layout: plan Updates 42-46 are DOC-ONLY commits on
`feature/retire-embedded-shims-claude`; the layer 1-9 CODE (and this Update 47 +
the fix) live on `wip/tuple-instantiation-claude`. Keeping code+doc together here
to avoid the cross-branch confusion that produced the Update-46 branch mix-up.

### Update 48 — layer 10 ROOT-CAUSED: `_Auto_node`'s variadic member-template ctor is an EMPTY STUB (node never allocated).

With the layer-9 recursion fixed (Update 47), `map<int,int>; m[1]=2` null-derefs
inside the REAL libstdc++ `std::_Rb_tree_insert_and_rebalance` (+0x33), called from
the madc `_M_insert_node`. The rebalance call args are structurally correct
(`insert_left, (_Rb_tree_node_base*)__z, __p, &_M_header`) — the fault is that
`__z` (the new node) is NULL.

`__z` = the `_Auto_node`'s `_M_node`. The `_Auto_node` ctor body in the emitted C
is **EMPTY**:

```c
void ..._Auto_node___Rb_tree..._Auto_node(_Auto_node *__this, _Rb_tree *__t,
        piecewise_construct_t *, tuple_const_int32_t_ *, tuple *) {
}
```

It should be the variadic member-template ctor's mem-init list:

```cpp
template<typename... _Args>
  _Auto_node(_Rb_tree& __t, _Args&&... __args)
  : _M_t(__t),
    _M_node(__t._M_create_node(std::forward<_Args>(__args)...))   // allocate
  { }
```

So `_M_node` is never assigned (stays 0) -> `__z` is null -> rebalance null-deref.
This is the prior handoff's "last blocker": `_Auto_node`'s VARIADIC MEMBER-TEMPLATE
CTOR (stl_tree.h:~1635, constructed at ~2434 in `_M_emplace_hint_unique`) is
registered but its body/mem-init list is never instantiated — emitted as a stub.
(NOT the node-pointer/base-derived type conflation Updates 45/46 theorized; that
theory is superseded.)

FIX (next session): instantiate the variadic member-template ctor's mem-init list
at the construction site — deduce `_Args` from the construction args, fill
`_M_t(__t)` and `_M_node = __t._M_create_node(std::forward<_Args>(__args)...)`.
This mirrors the 2-part plan in claude_status.json's layer-8 note (Part B: the
object-decl ctor-construction path must deduce the pack + instantiate a concrete
member-template ctor, like instantiate_member_fn_template_for_call does for calls).
Deep template-instantiation work, fulltest-gated.

### Update 49 — layer 10 FIXED (map node now allocated) + layer 11 (pair piecewise ctor) ROOT-CAUSED into 4 sub-problems; problem 1 (ctor registration) FIXED.

**Layer 10 FIXED (committed c9adf96).** The `map<int,int>; m[1]=2` runtime
null-deref was the `_Auto_node` ctor emitted as an EMPTY stub — `_M_node` never
allocated → null node → null-deref in the real `_Rb_tree_insert_and_rebalance`.
Root cause: a NAMING mismatch. `instantiate_member_ctor_template_for_construction`
names the instantiated ctor `<full-class>__<full-class>`, but the ctor-mem-init
parse gate (parseFunction, tkColon) only recognized a ctor whose tail resolves as
a TYPE-ALIAS to the owner. A monomorphized class registers only its SHORT
injected name as an alias, not its full mangled identity `name`, so
`resolve_class_type_alias` returned NULL → the mem-init list was consumed without
parsing → empty body. FIX (deepest layer): `resolve_class_type_alias` now
resolves a class's OWN `name` to itself (the injected-class-name, [class.pre]/2).
The ctor now emits `_M_node = _M_create_node__mti(...)`; the node is allocated and
the crash is gone. fulltest 655/6 (exact wip baseline, ZERO regressions).

**Layer 11 — pair piecewise member-template ctor.** With the node allocated,
`map<int,int>; m[1]=2` now hits a c2mir compile error (`too many arguments` at
stl_map.h:102). Root cause: `std::construct_at`'s body
`::new(loc) _Tp(forward(args)...)` calls the pair ctor with 4 args
(`this, piecewise_construct_t, tuple<const int&>, tuple<>`) but binds the 2-arg
COPY ctor `pair(const pair&)` — the pair PIECEWISE member-template ctor
`pair(piecewise_construct_t, tuple<_Args1...>, tuple<_Args2...>)` was never
instantiated. Decomposed into 4 sub-problems:

- **Problem 1 — ctor not registered (FIXED this session).** On instantiating
  `pair<const int,int>`, the ctor declarator name is substituted to the full
  mangled `pair_const_int32_t_int32_t`, but `register_skipped_class_template_function`
  only added a member-template ctor to `owner->ctors` when
  `name == ctor_source_name` (`pair`). Mismatch → not registered → construction
  sites couldn't discover it. FIX (mirror of layer 10): accept
  `name == owner->name` (the post-substitution injected-class-name) too. Verified:
  pair's ctors 2 → 4 (both the tparams=2 converting ctor and the tparams=4
  piecewise ctor now registered).

- **Problem 1b — placement-new construction site (FIXED this session).**
  `TokenNEW::parse` (which lowers `::new(loc) T(args)` inside construct_at) was the
  one construction site NOT calling
  `instantiate_member_ctor_template_for_construction`. Added the call (mirrors the
  variable-decl + functional-cast sites). Now fires with
  `alloc_class=pair_const_int32_t_int32_t nargs=3`.

- **Problem 2 — out-of-line body not attached (REMAINS).** The piecewise ctor is
  DECLARED in-class (stl_pair.h:200-202) but DEFINED out-of-line IN `<tuple>`
  (tuple:2248-2258), a TWO-LEVEL template head
  (`template<class _T1,_T2> template<typename... _Args1,_Args2> pair<_T1,_T2>::pair(...)`).
  So in-class `register_skipped_class_template_function` retains no body
  (`has_body=0`) → `member_template_decl.empty()` → the placeholder search in
  `instantiate_member_ctor_template_for_construction` skips it (`placeholder=nil`).
  The out-of-line attach machinery (`register_outofline_member_instantiations`,
  parser.cpp ~29080-29112) DOES handle two-level member-template heads, but matches
  the in-class method by `cfd->method_display_name == def.member_name`; the ctor's
  display name is the full mangled name while the OOL def's member_name is `pair`
  — a THIRD name mismatch to resolve (and confirm the ctor's OOL def is captured
  at all).

- **Problem 3 — two-pack deduction (REMAINS).** `instantiate_member_ctor_template_for_construction`
  → `try_instantiate_namespace_fn_template` deduces a single trailing pack
  (`_Args&&...`). The piecewise ctor has TWO packs (`_Args1...`, `_Args2...`)
  deduced from the TEMPLATE ARGUMENTS of `tuple<_Args1...>` / `tuple<_Args2...>`
  parameters matched against `tuple<const int&>` / `tuple<>` — nested
  class-template-id-arg pack deduction, not yet supported.

- **Problem 4 — piecewise body instantiation (REMAINS).** The out-of-line body
  DELEGATES to a private indexed ctor (tuple:2260-2269)
  `pair(tuple<_Args1...>&, tuple<_Args2...>&, _Index_tuple<_Indexes1...>, _Index_tuple<_Indexes2...>)`
  via `_Build_index_tuple<sizeof...(_Args1)>::__type()`, whose body is
  `first(std::forward<_Args1>(std::get<_Indexes1>(t1))...), second(...)`. Needs the
  `_Build_index_tuple`/`_Index_tuple`/`std::get<I>` index-sequence machinery to
  instantiate — a deep chain of its own.

This session committed problem 1 + 1b (correct, additive, root-caused; map still
fails identically at problem 2 — neutral user-level change, but the structural
prerequisites are now in place). Problems 2/3/4 are each substantial and remain
for the next session(s).
