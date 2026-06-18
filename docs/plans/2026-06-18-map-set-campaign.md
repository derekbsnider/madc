# map / set bring-up campaign — diagnosis & plan

**Status:** IN PROGRESS — std::tuple bring-up + A3.2 + multi-element parameter
packs done; map `operator[]` now instantiates the emplace body and reaches the
`_Rb_tree::_Auto_node::_M_insert` receiver-type wall; set insert (B) / contains
(C) remain (2026-06-18)

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
