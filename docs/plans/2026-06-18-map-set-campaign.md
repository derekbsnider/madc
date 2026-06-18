# map / set bring-up campaign — diagnosis & plan

**Status:** IN PROGRESS — std::tuple bring-up done; map operator[] / set insert / contains remain (2026-06-18)

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
