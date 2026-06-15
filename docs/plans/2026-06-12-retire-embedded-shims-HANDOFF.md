# HANDOFF — Retire embedded shims: real headers serve every mode

**Read this FIRST on resume/post-compaction.** Cold-start brief; assume you
remember nothing. Run `bash scripts/resume.sh` first (live git/build truth),
then read the **"SESSION 10 CLOSE"** block immediately below (current state +
NEXT), then "SESSION 9 CLOSE" for the prior chronology (parts 2a/2b + sub-gaps).
The "SESSION 8 CLOSE" block under it is older chronology (w2a). The
"SESSION 7 CLOSE" master section further down is the campaign primer
(partition model, user rulings, verified-working commands, traps) — still
valid for background; its §1 "Live state" git HEAD is STALE (use SESSION 8
CLOSE). The per-part STATUS banners are detailed chronology, read only for
depth. The governing process document is **`madc-header-partition-handoff.md`
(repo root)** — every decision here must trace to it. Companion memory:
`project_retire_embedded_shims` + `project_header_partition_architecture`.

---

# ★ SESSION 10 CLOSE — COLD-START REHYDRATION (READ FIRST) — 2026-06-15

## 0. Orientation
Same campaign (real glibc+libstdc++ every mode; sole backend cir_node→c2mir→MIR).
Integration is at **592 passed / 15 failed / 0 timed out / 18 skipped**
(+testmemberaliastmpl; the documented 15 unchanged, zero regressions). Last CODE
commit **`c3209d0`** (binary built from it). Working on **Stage 1 part 2c** of
`docs/plans/2026-06-14-template-instantiation-core-plan.md` — the tv1 std::vector
blocker. Part 2c is TWO features (as the SESSION-9 SIZING note predicted):

**FEATURE 1 — member-alias-template-id resolution — DONE @`c3209d0`, FULLY GATED.**
`Owner<...>::type<A,B>` (libstdc++ `__conditional<C>::template type<If,Else>`,
the `__conditional_t` building block) now resolves in DECLARATION and TYPEDEF
contexts, not just the typename-in-alias path. The resolution machinery already
existed (the chain-walker loop body → instantiate_template_id →
instantiate_template_alias_use; resolve_typename_type_token uses it). Two gaps in
REACHING it, both fixed (parser.cpp, 17 lines, no new path): (1)
`resolve_class_member_type_chain`'s first-segment probe now admits
`find_template_alias` so `Owner<...>::type<A,B>` chains descend; (2)
parseDeclaration's leading-chain block falls through to the CONSUMING chain-walker
for a member-template-id leaf (the non-consuming peek helper bails on the trailing
`<`). Reducers tmp/mat1-6 pass incl. real `<type_traits>` `std::__conditional_t`
(mat6); +tests/testmemberaliastmpl (g++/clang `7 3.5 9.5 4 6.5`). Gated: 592/15
zero regr, unit, gcc-torture 1571/51 byte-identical, SMAUG soak exit 124 + ready.

**FEATURE 2 — variadic-primary template-id `::member` (the tv1 wall) — ATTEMPTED,
REVERTED (clean), FULLY ROOT-CAUSED + clang-grounded. WIP patch saved.** After
feature 1, tv1 still fails: its `_ReturnType` default is
`__conditional_t<__move_if_noexcept_cond<_Tp>::value, ...>`, and madc cannot even
PARSE `__move_if_noexcept_cond`'s definition (bits/move.h:102-104): base clause
`__and_<__not_<is_nothrow_move_constructible<_Tp>>, is_copy_constructible<_Tp>>::type`.
**PRECISE ROOT (reduced, 3-oracle):** a VARIADIC-PRIMARY template-id followed by
`::member` does not resolve the member — generally (typedef + base clause), not
just base clauses. Reducers (DEFAULT mode, g++/clang accept, madc errors before the
fix): `tmp/db3.mad` (variadic `And2<...>::type` as a base specifier), `tmp/db4.mad`
(same as a TYPEDEF), `tmp/db5.mad` (`::n` static member, proves the partial spec
isn't selected). Non-variadic `And<...>::type` WORKS (tmp/db1/db2/db6) incl. nested
template-ids + dependent param + `::value` inheritance — the trigger is purely the
VARIADIC primary. **TWO coupled causes:** (1) `instantiate_template_use`
(parser.cpp:2659) short-circuits a variadic primary to `instantiate_opaque_template_use`
(opaque placeholder, no members) BEFORE the arg loop + partial-spec selection (2848);
(2) a genuine BUG at parser.cpp:**28519** — a TYPE parameter pack
(`template<typename...>`) wrongly sets `has_non_type_params = true`, so the SECOND
opaque short-circuit (`has_non_type_params && body.empty()`, ~2675) ALSO fires for a
bare variadic fwd-decl (the real `__and_`/`__or_` shape). The `__and_` primary is a
fwd-decl; its 1-/2-arg PARTIAL SPECS hold the `::type` bodies. **CLANG MODEL
(grounded, /workspace/llvm-clang-src/clang/lib/Sema/SemaTemplateInstantiate.cpp:3670
`getPatternForClassTemplateSpecialization` + :3816):** clang NEVER goes opaque on
pack-ness — it `DeduceTemplateArguments(Partial, concrete-args)` for each partial
spec, most-specialized wins (`getMoreSpecializedPartialSpecialization`), no match →
primary; opaqueness is gated on DEPENDENCE of the args, not packs. madc's
`match_partial_specialization` (13254) is the equivalent of that deduction. **THE
ATTEMPTED FIX (works on db1-6, but REVERTED — saved `tmp/feature2_variadic_partialspec_wip.patch`,
93 lines):** (a) both opaque short-circuits skip when `partial_spec_map.count(tname)`;
(b) the arg loop binds extra args to a trailing pack slot (clamped slot index)
instead of erroring; (c) the default-fill loop treats a trailing pack reaching zero
args as a valid EMPTY pack (`tuple<>`) — without (c), `tuple<>` regressed. **WHY
REVERTED — the cascade:** turning on variadic-partial-spec instantiation GLOBALLY
made previously-opaque libstdc++ variadic traits in the `<iostream>` closure
instantiate eagerly and hit NEW downstream walls — `tmp/db6.mad` (and every test)
then failed at a header `using X = Variadic<...>;` alias → "Expecting type in using
alias" (parser.cpp:17798, the alias target resolved NULL). This is the deep cascade
the SIZING note predicted: the fix is architecturally right but destabilizes the
trait machinery; it needs the downstream walls cleared incrementally under the full
gate, OR narrowing (only fall through in a `::member` context — needs lookahead past
the balanced `<...>` for a trailing `::`, fragile). **NEXT-SESSION PLAN:** reapply
`tmp/feature2_variadic_partialspec_wip.patch`, then drive the header cascade down
one wall at a time (start: the `using X = Variadic<...>` NULL at parser.cpp:17798),
each behind the 4-gate. This is MULTI-SESSION. Even when tv1's `__conditional_t`
chain clears, tv1 may not flip (further `_M_realloc_insert`/`__uninitialized_copy_a`
walls; testvector L17 / testvectorptr L28 are separate roots). **RECOMMENDATION
(re-affirmed):** strongly weigh the ~20-min survey of the other 14 failures first —
headline drops come from independent singletons, not the tv1 chain (SESSION 9 §3).
madc exit code = RUN-SUCCESS not main()'s return — verify reducers via cout/`--emit=c11`.

---

# ★ SESSION 9 CLOSE — COLD-START REHYDRATION (READ FIRST) — 2026-06-14

## 0. Orientation
Same campaign as SESSION 8 (real glibc+libstdc++ every mode; sole backend
cir_node→c2mir→MIR). w2a (`std::vector<int> v;`) compiles+runs. Integration is at
**591 passed / 15 failed / 0 timed out / 18 skipped** (failures down from 27 across sessions; +testalignas,
+testtransform, +testtmpldefaultparam, +testtmpldefaultcond). The last CODE commit is **`cd8dc11`** (binary
built from it); tree HEAD is the docs commit `d19a682`. **DONE this session, all 4-gates green, zero
regressions:** Stage 0 (`f03cfb4` array→ptr decay), Stage 1 part 1 (`a12314b` reference-to-pointer return
value typing), Stage 1 part 2a (`5104dc1` multi-token defaulted type params `R = T*`), Stage 1 part 2b
(`cd8dc11` trait-expression defaulted type params `R = conditional<...>::type`). Integration now 591/15
(+testtransform, +testtmpldefaultparam, +testtmpldefaultcond).
**★ THE PLAN IS IN PROGRESS:** `docs/plans/2026-06-14-template-instantiation-core-plan.md` — the staged plan
for the deep template-instantiation core behind all 15 remaining failures. READ IT (§4 Stage 1) before
resuming. **Stage 1 part 1 (`a12314b`)** fixed `operand_value_datadef` stripping a pointer level off a
reference-to-pointer return (`int*&`→`int`), which broke `T*` deduction from `__normal_iterator::base()`; it
advanced all 3 vector tests (tv1/testvector/testvectorptr) past the "cannot dereference non-pointer type" wall
(no flip yet). **Stage 1 part 2a+2b DONE** — defaulted template type params now resolve: 2a (`5104dc1`) base+suffix folds
(`R = T*`), 2b (`cd8dc11`) full trait-expression resolution via `resolve_template_param_default_type` in an
isolated stream (`R = conditional<...>::type`, +testtmpldefaultcond). **NEXT = Stage 1 part 2c: MEMBER ALIAS
TEMPLATE `__conditional<C>::template type<If,Else>`** — tv1's real default is `_ReturnType = __conditional_t<
__move_if_noexcept_cond<_Tp>::value, move_iterator<_Tp*>, _Tp*>` (`bits/stl_iterator.h:1828`), and
`__conditional_t` = `typename __conditional<C>::template type<If,Else>` (type_traits:134) where
`__conditional<bool>` holds a member alias template `template<class T,class> using type = T;`
(type_traits:119). After 2b, tv1 advanced to **"type<> expects 0 type argument(s), got 2"** — madc treats the
member `type<...>` as a 0-arg template. Part 2c = member-alias-template-with-args resolution + the
`__move_if_noexcept_cond<_Tp>::value` bool trait. Reducer to build: `__conditional<false>::type<A,B>` analogue.

**★ SIZING (assessed 2026-06-14, before diving in): part 2c is a FULL SESSION, likely more than one — NOT a
contained slice like 2a/2b.** It is TWO distinct substantial features: (1) **member-alias-template-id
resolution** `Class<...>::type<Args>` — madc cannot even PARSE this today (user-defined reducer `tmp/mat1.mad`
`Sel<false>::type<double,int>`, NO headers, fails "Expecting identifier after type"); needs a new parser
grammar form (qualified member *template*-id access) PLUS a resolution/instantiation path — comparable to or
bigger than 2b. (2) **`__move_if_noexcept_cond<_Tp>::value`** is NOT a literal — it derives from
`__and_<__not_<is_nothrow_move_constructible<_Tp>>, is_copy_constructible<_Tp>>::type` (`bits/move.h:102`);
evaluating it needs two type-traits + the `__not_`/`__and_` metafunctions + inheriting `::value` from an
`integral_constant` base — its own trait-evaluation feature. (3) Even with both, tv1 may NOT flip — `_ReturnType`
then flows back into `__uninitialized_copy_a`/`_M_realloc_insert` (possible further walls), and testvector L17 /
testvectorptr L28 are confirmed SEPARATE roots. **RECOMMENDATION before sinking a session into 2c: first spend
~20 min surveying the other 14 failures (testmap/testset = ref data members [orig plan Stage 4];
teststringref; testforeach2; testsubscript*) to confirm tv1's remaining chain is the best return — a different
failure may be closer to a headline flip.** A reference default `R = T&` is a separate deferred gap. Also independent walls once the deref chain clears:
testvector L17 "incompatible types in assignment to arithmetic lvalue", testvectorptr L28 "expression before
'->' must be a pointer" (vector<T*> subscript loses pointer-ness — check if the `returns`-strip recurs in
`operator[]`). NB madc's
exit code is RUN-SUCCESS not `main()`'s return — verify reducers via cout/`--emit=c11`. The plan's ORIGINAL
Stage-1 hypothesis (declarator-suffix re-application) is DISPROVEN; the real chain is reference/return-type
value-typing fidelity. The clean singletons are exhausted; this plan is the path forward.

**WHAT THIS SESSION ADDED (10 code commits + 3 test fixes, all FULLY GATED), latest first:**
- `77a7983` **function→fn-ptr argument decay in namespace-fn-template deduction (feature; +testtransform, NO headline flip).**
  [temp.deduct.call]: deducing a by-value type param from a FUNCTION arg decays it `R(Args)`→`R(*)(Args)`.
  `try_instantiate_namespace_fn_template` bound the param to the bare FuncDef (`operand_value_datadef`
  returns the function) → an instantiated `std::transform(b,e,fn)` emitted an incomplete non-callable
  `void __op` param. Fix: a function arg vs a by-value (no `&`) bare type param decays to `DataDefFPTR`.
  Verified vs g++ (testtransform: 2 4 6 8, warning-free). **testforeach2 still FAILS** — the
  std-algorithm-with-functor area is a DEEP multi-bug region (see §3 post-mortem): this fix is one of
  ~5 walls. **CRITICAL deferred findings** (do not re-discover): (a) BARE C-array arg
  (`transform(a,a+4,b,fn)`) hits a separate call-site overload-binding gap — the array arg isn't decayed
  when re-selecting the instantiated overload (`import of undefined item __ns_std_transform`); the
  array→pointer deduction decay was DROPPED from this commit (untestable-to-green without that call-site
  fix). (b) `std::for_each` returns its functor BY VALUE → a function returning an ANONYMOUS fn-ptr
  renders its return type as `long` (DataDefFPTR's dtINT64 base; `func_def` has NO fn-returning-fn-ptr
  declarator — only typedef'd fn-ptr returns render, fpr2) → c2mir "returning pointer without cast for
  integer result" warning; that's why the test uses transform (iterator return), not for_each.
  (c) std::string BY VALUE through an instantiated for_each body CORRUPTS (fe14). (d) count_if wraps the
  predicate in `_Iter_pred` (another instantiation wall). (e) user GLOBAL free fn templates with
  deduction (`twice(21)`) don't resolve at all ("undeclared identifier") — only CLASS templates +
  NAMESPACE fn templates + operator templates do. Gated 589/15 zero regr, torture 51 byte-identical
  (one memclr.c suite-timeout flake, passes standalone), SMAUG.
- `9b7b944` **C++11 `alignas` keyword support (feature; +testalignas, NO headline flip).** madc mapped
  the C11 `_Alignas`→`__attribute__` (lexer consumes specifier+parens) but had ZERO handling for the
  C++11 bare keyword `alignas` → `alignas(N) <decl>` parsed `alignas` as a TYPE name, then `(` tripped
  the class-member parser ("Unsupported parenthesized member declarator in class definition"). Surfaced
  instantiating std::set/map: libstdc++'s `__aligned_membuf<_Tp>` (the `_Rb_tree` node storage)
  declares `alignas(__alignof__(_Tp)) unsigned char _M_storage[...]`. Fix (DRY, lexer define_map): map
  `alignas` to the same path as `_Alignas` — both spellings consumed identically at every scope
  (`alignas` is a C++ keyword + a C11 `<stdalign.h>` macro, never a real identifier). Verified vs g++.
  ADVANCES the set/map chain to the NEXT `_Rb_tree` wall ("Reference data members (T&) are not
  supported" — a genuine reference-data-member feature), so testset/testcontainerdtor do NOT flip yet
  (deep chain, not a singleton). Like the byte guard / tv1 sub-gaps: real fix, no count drop. NOTE:
  alignment value not yet recorded on the member (stripped, same as `_Alignas`) — shared follow-up.
  Gated 588/15 zero regr, torture 1571/51, SMAUG.
- `c93c5e1` **eval wrapper hoists leading #include out of the function body — flips testmadceval (16 → 15).**
  `build_eval_body_wrapper_source` wrapped the ENTIRE eval source — incl. a leading `#include` — INSIDE
  the synthetic `double __madc_eval() { ... }`. The lexer inlines header tokens wherever the directive
  sits, so `<math.h>`→`<cmath>`'s `namespace std` declarations were parsed as function-local statements
  → "Failed to find type 'std' when parsing function parameters", and eval_double returned garbage
  (eval_int/bool/string include no header, so they passed). `#include` in a function body is invalid C++
  anyway (can't open `namespace std` in a function); the eval API's intent is file-scope includes. Fix:
  hoist the CONTIGUOUS leading run of preprocessor directives (blank lines OK, `\`-continued lines kept
  whole) to file scope before the function. NB a SEPARATE pre-existing non-fatal diagnostic ("cannot
  dereference non-pointer type" parsing `<cmath>`) still prints to stderr — it fires at top-level
  `#include <math.h>` too (sub-gap-13 class, independent of eval), does NOT affect correctness
  (eval_double now returns 4, exits 0; runner discards stderr). Gated 587/15 zero regr, torture 1571/51, SMAUG.
- `a47c86e` **inherited-operator return type searches ALL bases — flips testsstream (17 → 16).**
  `DataDefCLASS::binary_operator_return_type` walked only the SINGLE primary base (`base_class`)
  for an inherited binary operator's return type; for MI it missed an operator inherited from a
  NON-primary base. `basic_iostream : basic_istream, basic_ostream` keeps `operator<<` in base #2,
  so a chained `stringstream << a << b` lost its parse-time type — the inner `ss<<a` emitted the
  correct `_ZStls...basic_ostream(&ss+16,&a)` but its RESULT type was NULL (primary `basic_istream`
  has no `<<`), so the outer `<<` saw a non-class operand → builtin shift ("shift operands should be
  of an integer type"). ostringstream (single base, offset 0) worked, isolating the MI base-walk as
  root. Fix: iterate the full `bases` list (MI direct-base set incl. primary) like `is_or_derives_from`.
  Combined TEST FIX (INVALID-TEST class): g++/clang reject `stringstream` without `#include <sstream>`,
  and the test used the retired `printstream(ss)` builtin (removed hardcoded dtSSTREAM tag) → added
  `<sstream>` + `cout << ss.str() << endl`; +tests/testsstream.expect. Gated: 586/16 zero regr, torture 1571/51, SMAUG.
- `b3beadb` **free-operator concrete-param match guard (correctness; NO count drop yet).** A free
  operator whose param is a CONCRETE type (no template param) was accepted against ANY arg
  (`fn_template_deduce_param` returns 0 without checking). So `std::operator<<(byte, _IntegerType)`
  spuriously matched a `stream << x` and FATALLY failed instantiating `__byte_op_t<x>`. New
  `free_operator_concrete_param_matches`: a concrete NAMED (class/enum) param must BE the arg's class
  or a base of it (`is_or_derives_from`); scalars/pointers stay permissive. g++/clang reject the byte
  op on that param. **testsstream still fails on a SEPARATE deeper bug** (see §3) — this is a
  standalone correctness fix + prerequisite. Gated: 585/17 FAIL-list byte-identical, torture 1571/51, SMAUG.
- `fe2417a` **teststdstringconv — TEST FIX: standard 1-arg `std::to_string` (18 → 17).** Called
  `std::to_string(s, 42)` (a 2-arg (out,value) writer) — does NOT exist in real libstdc++ (to_string
  is the 1-arg value→string converter that RETURNS the string); g++ AND clang++ both reject it. Only
  worked under the old shims. Rewrote to `std::string s = std::to_string(42);` (stoi/stod were already
  valid + work). Same INVALID-TEST class as teststringglobal. Test-only; binary unchanged.
- `73cff0a` **teststringglobal — TEST FIX: global `empty` was ambiguous vs C++17 `std::empty` (19 → 18).**
  `cout << empty` errored "shift operands should be of an integer type". NOT an operator<</global-string
  bug (the prior hypothesis was wrong): the file-scope global named `empty` is ambiguous against the
  C++17 free fn `std::empty` (`<bits/range_access.h>`, pulled via `using namespace std`); g++ AND clang++
  both REJECT it ("reference to 'empty' is ambiguous"). Only passed against the old shims (no std::empty).
  Renamed the global `empty`→`blank` (comment added). madc has a LATENT gap here — it silently resolves
  the bare name to `std::empty` (a fn → shift) where the canon compilers hard-error the ambiguity; proper
  ambiguity detection in unqualified lookup under a using-directive is a separate, unstarted feature.
- `c518a00` **test3eqclass — strict-equality `===` domain rule for a FREE `operator==` (20 → 19).**
  `s === s2` / `s === "x"` on std::string errored "no match for strict equality … (no operator===
  or operator==)". `===`/`!==` had no parse-time domain-rule fallback to `operator==`: a member ==
  bound via the CIR builder, a user === bound directly, but std::string's FREE bool-returning
  `operator==` never bound (the CIR free-operator path only binds ref/by-value-class returns).
  `rewritten_operator_candidate` now lowers `x === y`→`x == y` / `x !== y`→`!(x == y)` (spec
  §2.2/2.4/2.5) through the SAME `lower_free_operator_to_call` normal `==` uses (new helper
  `lower_strict_equality_domain_rule`). Returns NULL (token kept for CIR member-== / different-domain
  constant) when no == covers the pair — Plain/Money/Tag untouched. Flips test3eqclass.
- `390d8a0` **teststringrel — reference-transparency in method-overload reselection (21 → 20).**
  Inside the instantiated `operator<(const string&, const string&)` body `__lhs.compare(__rhs)`
  mis-bound `compare(const char*)` because `__rhs` (a `const string&` param) was scored as a raw
  `string*`. `reselect_method_overload` now types args via `operand_value_datadef` (reference
  transparency). Flips teststringrel. (test3eqclass `===` and teststringglobal `operator<<` are
  separate roots, still failing.)
- `f120470` **sub-gap 12 — `__is_trivial(T)` builtin** (was missing from the trait table →
  `_ValueType1` undeclared). Advances `tv1` to sub-gap 13; **0 test flips**.
- `ce4313a` **fn-ptr-param overload fix — the `_ZNSolsESo` wall, the FIRST headline drop in ~4
  sessions: 27 → 21** (six tests). `cout << (long)` mis-bound the ostream manipulator because
  `DataDefFPTR::is_numeric()` is true; a fn-ptr param now rejects a non-function arg.
- `17677f2` **sub-gap 11 — direct-initialization of a non-class variable** (`T name(expr)`;
  C++/madc-only `!is_c_mode()` gate). +testdirectinit.

**THE KEY STRATEGIC FINDING (read §3):** the container chain is DEEP and FORKS per-operation, so
sub-gap work advances the `tv1` REDUCER but does NOT flip tests — only the INDEPENDENT singletons
flip the headline count (this session `_ZNSolsESo` 27→21, teststringrel 21→20, test3eqclass 20→19,
teststringglobal 19→18, teststdstringconv 18→17, testsstream 17→16). **Headline-count drops come from
the INDEPENDENT singletons**, NOT container sub-gaps. **A SUB-CLASS of those singletons = INVALID-TEST
fixes** (teststringglobal, teststdstringconv, testsstream's test side): tests written against old-shim
convenience signatures that real libstdc++ + g++/clang reject — fix the test to standard usage, NOT
madc. **THE CLEAN SINGLETONS ARE NOW EXHAUSTED; the remaining 16 are the HARD cluster** — each
compound or deep, NOT ~1-fix drops:
- **testsstream — DONE @a47c86e** (all 3 parts): (1) byte concrete-param guard @b3beadb; (2) the MI
  base-walk fix — `binary_operator_return_type` now searches ALL `bases`, so the `stringstream <<`
  chain keeps its basic_ostream result type (operator<< is in the 2nd base basic_ostream, not the
  primary basic_istream); (3) test fix `<sstream>` + `cout << ss.str()`.
- **testmadceval — DONE @c93c5e1:** eval wrapper now hoists a leading `#include` out of the synthetic
  function body to file scope (it was inlining `<cmath>`'s `namespace std` as function-local statements
  → "Failed to find type 'std'"). eval_double returns 4. (Pre-existing non-fatal `<cmath>` "cannot
  dereference non-pointer type" diagnostic remains — sub-gap-13 class, fires at top-level math.h too.)
- **(superseded NEXT line — kept for context):** the runtime-eval child TU loses `std::` only for `eval_double` (`#include
  <math.h>` in the eval'd source): `Failed to find type 'std' when parsing function parameters` in
  `__madc_runtime_eval`. eval_unit/int/bool work; the math-include eval child doesn't see std::.
- **testforeach2 — compound:** (a) `std::for_each(names, fn)` is a 2-arg call g++ rejects (real
  for_each is 3-arg) AND (b) VALID 3-arg `std::for_each(a,a+3,pr)` fails with `istreambuf_iterator<>
  expects 2 argument(s), got 1` (free-fn-template instantiation/overload bug).
- The rest = the deep container chain (testvector/map/set/subscript*/template*/refreturn — _S_destroy
  + tv1 sub-gap 13) + "parenthesized member declarator" (testcontainerdtor/testset) + "operator-> on
  object" (testsubscriptarrow/testvectorptr).

**a47c86e POST-MORTEM (the MI base-walk class of bug):** the dual representation `base_class` (single
primary) + `bases` (full MI direct-base list, populated together at the flatten site) means ANY
helper that walks inheritance via `base_class` alone is silently MI-incorrect. `binary_operator_return_type`
was one; AUDIT the others (`is_virtual_method` still uses `base_class`-only — fine for the single-base
streams it sees today, but a latent MI gap; flag if an MI virtual-through-secondary-base case surfaces).
The canonical correct walk is `is_or_derives_from`'s `for (b : bases)`.

## 1. Live state (verify `bash scripts/resume.sh` + `git status`)
- **Branch** `feature/retire-embedded-shims-claude` @ **`77a7983`** (LOCAL ONLY; develop
  untouched). `77a7983` = function→fn-ptr argument decay in namespace-fn-template deduction (CODE;
  +testtransform, NO headline flip; std-algorithm/functor area is deep — see §0/§3). Prior `9b7b944` =
  C++11 `alignas` keyword support (CODE; +testalignas, advances set/map chain, NO headline flip). Prior
  `c93c5e1` = eval wrapper hoists a leading `#include` out of the synthetic
  `__madc_eval` body to file scope (CODE); flipped testmadceval 16→15. Prior `a47c86e` = inherited-operator return type
  searches ALL bases (CODE) + testsstream test fix (`<sstream>` + `cout << ss.str()`); flipped
  testsstream 17→16. Prior `b3beadb` = free-operator concrete-param match guard (CODE; the byte
  prerequisite for testsstream). Before that, two TEST FIXES (binary was unchanged at
  `c518a00`): `fe2417a` (teststdstringconv 2-arg→1-arg std::to_string, 18→17), `73cff0a`
  (teststringglobal `empty`→`blank`, 19→18). Prior code: `c518a00` (test3eqclass — strict-equality `===` domain rule for a
  free `operator==`, 20→19); prior `390d8a0` (teststringrel — ref-transparency in method-overload
  reselection, 21→20), `f120470` (sub-gap 12 — `__is_trivial(T)` builtin; advances tv1, 0
  flips), `ce4313a` (fn-ptr-param overload fix → cleared the `_ZNSolsESo` wall, 27→21),
  `17677f2` (sub-gap 11, direct-init), `83a05d7`
  (sub-gap 10), `011769f` (sub-gap 9), `f644bb9` (sub-gap 8), `2e8abc1` (sub-gap 7),
  `7202523` (sub-gap 6), `7760712` (sub-gap 5), `58d6c7f` (sub-gap 4) + docs mirrors. Tree is
  CLEAN. MIR fork `/workspace/mir` @ `5df536f` == `MIR_COMMIT` → satisfied.
- **All gates GREEN at `77a7983`**: integration **589 passed / 15 failed / 0 timed out / 18
  skipped** (failures this session: **27 → 15**; +testalignas +testtransform new passes; flips: `ce4313a` six —
  testmultiret/testrust/teststruct3/testprefer/testrubycharsshadow/testmadcevalexprctx; `390d8a0`
  teststringrel; `c518a00` test3eqclass; `73cff0a` teststringglobal; `fe2417a` teststdstringconv;
  `a47c86e` testsstream; `c93c5e1` testmadceval; zero regressions); unit all-pass; gcc-torture **1571 / 51-name failset byte-identical** to
  `docs/parity/torture-failset-current.txt`; SMAUG soak exit 124 + ready. (`b3beadb` re-gated all four
  green — it's a code change; it adds no flip. `73cff0a`+`fe2417a` were test-only.)
- **NOTE on the count:** the sub-gap work (4-12) all lands on `tv1` (a reducer), so the headline only
  drops on the INDEPENDENT singletons (`_ZNSolsESo`, teststringrel, test3eqclass, teststringglobal,
  teststdstringconv so far). See §3.6b below for the current 17 clustered.
- Build: `make -C src` (clang++ default), bin at `bin/madc`. Clean, no warnings. NOTE: after a
  `FEATURE_DEFINES=-D…` debug build, `touch src/parser.cpp` before a plain `make -C src` or the
  stale debug `.o` is reused.

## 2. What session 9 landed (parts 21-24, each FULLY GATED) — the testvector member-overload chain
| Commit | What |
|---|---|
| `39cb5d2` | **part 21 — [temp.func.order] for STATIC member-template calls.** `findMethodOverload` partial-ordering TIEBREAK (`member_tmpl_more_specialized`, reuses part-18 `po_pattern_match`) + `reselect_static_member_overload` (post-arg reselect+rebuild for `Owner::m(args)`, wired at both qualified-call builders via owner/member out-params on `resolve_class_qualified_expression`). `take(U*)` beats `take(P)`. +testmemtmplorder. |
| `746aaa7` | **part 22 — class-scope `using Base::member;` imports base overloads.** Was skipped (name hiding dropped them); `try_import_using_base_member` pushes the base's same-name overloads into the derived set ([namespace.udecl]). Now the BASE `allocator_traits::construct` is selected over `__alloc_traits`'s own SFINAE one. +testusingbasemember. |
| `505fd1f` | **part 23 — preserve parameter-pack-ness for member-template instantiation.** B-feature hardcoded `typeparam_is_pack=false`; threaded real pack flags (`FuncDef::template_param_is_pack` ← `last_skipped_template_typeparam_is_pack`). A by-value variadic static member template now instantiates+runs. +testvariadicmembertmpl. |
| `5a15685` | **part 24 — reference params + fwd-ref packs + class-scope typedef params in member-tmpl instantiation.** (1) ref args were passed BY VALUE (call bound to the varargs PLACEHOLDER, no ref_params → SIGSEGV) — `reselect_static_member_overload` now binds the call to the instantiated DEFINITION (`<placeholder>__mti`, real params+ref_params) for EVERY member-tmpl static call (selection + rebind SEPARATED). (2) `Args&&...` fwd-ref packs: `fn_template_param_is_pack` accepts ref-quals + the 1-elem substitution copies `&&` through before dropping `...`. (3) class-scope typedef params (`allocator_type&`): instantiate with owner on `class_scope_stack` (`FnTemplateDef::owner_class`) + parseFunction `resolve_current_class_type_alias` fallback. +testmemtmplrefparam/fwdrefpack/typedefparam. REJECTED: mutating the placeholder signature (corrupts findMethodOverload arity gate, flips selection — do NOT reintroduce). |
| `58d6c7f` | **sub-gap 4 — PATTERN pack expansion in a member-tmpl body.** `instantiate_fn_template_binding` dropped a one-element pack `...` only after a bare pack name (`args...`/`Args&&...`); a PATTERN expansion `std::forward<Args>(args)...` (type pack as a template-arg + value pack, `...` after `)`) left the three `tkDot` in the stream → "Missing operand" + undefined extern. The pattern is already substituted inline for the single bound element, so the fix DROPS a three-`tkDot` run that doesn't follow an emitted comma (a C varargs `, ...` does) — generalizes the identifier-anchored drop to pattern-anchored. +testmemtmplpackexpand. `tmp/vmt10` runs; tv1 advances to the instance-member-template wall (sub-gap 5). |
| `7760712` | **sub-gap 5 — INSTANCE member-template instantiation AS A METHOD.** An instance member template of a LOCAL class (`__a.construct(...)` on `__new_allocator<int>`) emitted an undefined extern (the B-feature only did STATIC `Owner::m(args)`, as free functions). Clang/gcc research (recon /workspace/llvm-clang-src + /workspace/gcc): both instantiate a member fn template AS A METHOD of the class (static-ness = a flag; `this` intrinsic, never a free-fn receiver). Fix: `FnTemplateDef::instance_method`; retain body for instance templates too; `instantiate_fn_template_binding` parses the instance case via `parseFunction(retdd, inst_name, owner)` (hidden `__this`, member resolution) instead of `parseStatement`; `parseCallMethod` calls the hook; cir_builder Pass 0.75 SKIPS member-template placeholders (their `(struct C*, ...)` extern conflicted with the definition). +testmemtmplinstance (member access proves the method model). tv1 advances to sub-gap 6 (placement-new). |
| `7202523` | **sub-gap 6 — global-qualified new/delete `::new`/`::delete`.** `::new(p) T(args)` failed "use of undeclared identifier 'new'": the leading-`::` (global scope) expression branch read the token after `::` via `contextual_identifier_name`, which maps the `new`/`delete` keywords to strings, then looked them up as identifiers. Fix (parser.cpp leading-`::` block): intercept a following `new`/`delete` and delegate to `TokenNEW`/`TokenDELETE::parse` (same dispatch as the unqualified form). The construct body uses `::new((void*)__p) _Up(...)`. +testplacementnewglobal. PRE-EXISTING shared gaps (off-fix): scalar HEAP `new int(7)` + CLASS placement `new(p) T()` both fail in CIR for the unqualified form too. tv1 advances to sub-gap 7. |
| `2e8abc1` | **sub-gap 7 — placement-new void* store.** The instantiated construct body lowered scalar placement new as `*(void*)__p = value` → c2mir "assignment of incompatible value". Fix (cir_builder.cpp ~7649, scalar placement-new branch): cast the placement address to the CONSTRUCTED type's pointer `(T*)addr` before the deref-store (placement new constructs a `T` regardless of the pointer's static type; a no-op when addr is already `T*`; handles `alloc_type`'s own pointer depth). +testplacementnewvoidp. tv1 advances to sub-gap 8 (`_M_realloc_insert`). |
| `f644bb9` | **sub-gap 8 — out-of-line member DEFINITIONS of a class template.** `template<class T> RET S<T>::member(...){body}` (the bits/*.tcc shape) was never captured — a class instantiation re-parses only the class BODY, so `TokenTEMPLATE::parse`'s non-class branch mis-registered the out-of-line def as a NAMESPACE free fn (`std::member`), never called → real member an undefined import. Fix (reuses the lazy-body machinery, NO parallel path): (1) `skipped_template_outofline_member` detects the `Class<...>::member` declarator (Class a registered template) → the non-class branch stores `{member,typeparams,decl}` in `Program::out_of_line_member_defs` keyed by `<ns>::<class>` instead of a free fn; (2) `register_outofline_member_instantiations` (from `instantiate_template_use` post-monomorphize) substitutes def type-params (positional to use-site args) + class-id→mangled tag, registers a `full_definition` `deferred_lazy_bodies[member->name]` (the SAME path `parse_deferred_lazy_body` uses for defaulted member-tmpl ctors; lazy = ODR-use only, unused members never instantiate), and CLEARS the member's `emit_symbol`/`declaration_only` so the call uses the LOCAL emit name (== the deferred key) not the mangled-direct extern; (3) `parse_deferred_lazy_body` full_definition re-parses with the member's REAL return type, not hardcoded `ddVOID` (else a non-void out-of-line member's return was rewritten to void; ctor caller unchanged). Out-of-line member TEMPLATES (two-level head) EXCLUDED → sub-gap 9. +testoutoflinemember. tv1 advances to sub-gap 9. |
| `011769f` | **sub-gap 9 — out-of-line member TEMPLATE definitions.** A two-level-head out-of-line def (`template<class T> template<class U> RET S<T>::f(U){body}`, e.g. vector::_M_realloc_insert's C++11 variadic form) was EXCLUDED by sub-gap 8. The in-class member-tmpl DECLARATION (re-parsed at monomorphization) creates the member as an `is_member_template` placeholder with an EMPTY `member_template_decl`, so `instantiate_member_fn_template_for_call` (sub-gap 5) bailed → undefined import. Fix combines sub-gap-8 capture + sub-gap-5 per-call instantiation (NO new machinery): `skipped_template_outofline_member` now REPORTS `is_member_template` instead of rejecting a `template`-first decl (the class-id walk-back already finds `Class<...>::member` — inner head has no top-level `(` and sits left of the class-id); records inner type-params + pack-ness via new `extract_inner_template_typeparams`. `register_outofline_member_instantiations` member-tmpl branch substitutes the CLASS params (inner pass through) + class-id→mangled, transforms the substituted `template<U...> RET <mangled>::name(params){body}` into the in-class member-decl form `RET name(params){body}` (strip inner head + `<mangled>::` qualifier), fills the monomorphized member's `member_template_decl`/`owner`/`template_param_names`/`is_pack`. The call then instantiates per inner-arg binding (existing sub-gap-5 path). +testoutoflinemembertemplate. tv1 advances to sub-gap 10. |
| `83a05d7` | **sub-gap 10 — auto-return functions with a trailing return type.** `auto f(params) -> T {...}` (C++11; e.g. libstdc++'s cross-type `__normal_iterator operator-` returning `decltype(__lhs.base() - __rhs.base())`, reached by `_M_realloc_insert`'s `__position - begin()`) was rejected — `parseDeclaration` treated `auto` only as a deduced VARIABLE ("'auto' requires an initializer"). Fix: (1) parseDeclaration auto-var block guarded `&& nt->id() != tkOpBrk` so `auto f(` falls through to the function path (decl_type stays ddAUTO); (2) parseFunction's trailing-qualifier loop CAPTURES a `-> T` (tkDeRef) as raw balanced tokens, RESOLVED below after the param variables enter scope (so `-> decltype(params)` names them; resolve_declared_type_token handles decltype) — FuncDef::returns is a reference so the resolved type rebuilds the FuncDef via new `clone_funcdef_with_return` + re-points Variable/funcdef_map; (3) deferred (in-class method) bodies carry the captured tokens on `DeferredFunctionBody::trailing_ret_tokens` and resolve at materialization (`parse_deferred_function_body`, params back in scope). Deduced-return `auto f(){return e;}` (no `-> T`) is a follow-up. +testautotrailingreturn. tv1 advances to sub-gap 11. |
| `17677f2` | **sub-gap 11 — direct-initialization of a non-class variable.** `T name(expr)` for a non-class type (`pointer __new_start(this->_M_allocate(__len));` in `_M_realloc_insert`; `typedef int* P; P p(q);`) always parsed as a function declaration → first paren token `this`/`q` threw "Failed to find type … when parsing function parameters". madc did ctor-style direct-init for CLASS types only. Fix (parser.cpp, deepest layer, reuses the `= expr` machinery — no new init path): new non-consuming `paren_group_is_nonclass_direct_init()` returns true ONLY when the token after `(` cannot begin a parameter-declaration (a literal, `this`, or a ttIdentifier naming an in-scope value — type names tokenize as ttDataType), a SOUND under-approximation (every diversion is unambiguously direct-init; param-clause-capable starts keep the most-vexing-parse default = function decl). A parseDeclaration branch (after the class ctor-call branch) injects a synthetic `=` before the `(` so `(expr)` parses through the scalar/pointer `= expr` path (mirrors `T x{...}` brace-init). **C++/madc only (`!is_c_mode()`)** — load-bearing: a K&R def `void f(x,v) union u *x, v;` whose param names shadow file-scope globals (gcc-torture `921112-1.c`) otherwise diverted (the regression the gate fixes, caught by the torture failset diff). Verified vs clang++ AND g++. +testdirectinit. tv1 advances PAST the `this`-param wall to `use of undeclared identifier '_ValueType1'` (sub-gap 12). |
| `ce4313a` | **fn-ptr-param overload fix — cleared the `_ZNSolsESo` wall (27→21).** `cout << (long)` mangled the bogus import `_ZNSolsESo` (`operator<<(ostream)`) instead of `_ZNSolsEl`. Root: `DataDefFPTR::is_numeric()` returns true, so in `score_arg_to_param` a function-pointer PARAMETER fell through to the numeric-vs-numeric branch and a 64-bit integer arg matched the ostream manipulator `operator<<(ostream& (*)(ostream&))` with score 5 (rawtype 64==64), TYING `operator<<(long)` and — registered first — winning. Only 64-bit ints hit it (int/short scored 4 there; float/double non-integer). Fix (cir_builder `score_arg_to_param`): a fn-ptr param now handles every arg shape — a function/fn-ptr arg binds by signature, the null-pointer constant scores 3, any other arg is REJECTED (-1). Surgical + general (matches gcc/clang). Cleared 6 tests (testmultiret/testrust/teststruct3/testprefer/testrubycharsshadow/testmadcevalexprctx); testmadceval advances to a separate runtime-eval `std` wall. +`tests/testmadcevalexprctx.timeout=30` (it now RUNS — 12 runtime `eval_*_ctx` calls, each a full child-Program compile→JIT ~0.36s, ~5.5s total; per test-fixtures.md). Verified vs clang++ AND g++. |
| `f120470` | **sub-gap 12 — `__is_trivial(T)` builtin.** `_M_realloc_insert`→`uninitialized_copy` gates a memmove opt on `__is_trivial(_ValueType1)` (where `typedef typename iterator_traits<It>::value_type _ValueType1;`). madc's trait table lacked `__is_trivial`, so it parsed as a call to undeclared fn `__is_trivial` and its type-arg `_ValueType1` was looked up as a value → "undeclared identifier '_ValueType1'" (the typedef itself registers fine). Fix: add `__is_trivial` to `type_trait_arity` + `evaluate_type_trait` + the constexpr-fold path; new `trait_is_trivial` ([basic.types]: non-class trivial; class trivial iff no user ctor/dtor, non-polymorphic, all bases/members trivial). Verified vs g++ AND clang++ (int/ptr/POD=1, user-ctor/dtor/virtual=0). **0 test flips** (advances tv1 like sub-gaps 4-11). tv1 advances PAST `_ValueType1` to **`cannot dereference non-pointer type`** (sub-gap 13). |
| `390d8a0` | **teststringrel — reference transparency in method-overload reselection (21 → 20).** `a < b` instantiates the free `operator<(const string&, const string&)` whose body is `__lhs.compare(__rhs)`; madc bound `string::compare(const char*)` instead of `compare(const basic_string&)` and jammed the dereferenced string into the char* param → "incompatible argument type for pointer type parameter". The bug was ONLY in the instantiated body — the reference param `__rhs` (`const string&`) was scored as a raw `string*` in `reselect_method_overload` (which built argtypes from raw `p->datadef()`). Fix: `reselect_method_overload` now types each arg via `operand_value_datadef` (reference transparency — a reference var/param arg denotes the referenced OBJECT, not the pointer; the same helper already used at parser.cpp ~26117). compare(const basic_string&) now scores an exact object match and wins. Verified vs clang++/g++. Flips teststringrel. test3eqclass (`===`) + teststringglobal (`operator<<` on a global) are SEPARATE roots, still failing. |
| `c518a00` | **test3eqclass — strict-equality `===` domain rule for a FREE `operator==` (20 → 19).** `s === s2`/`s === "x"` on std::string errored "no match for strict equality … (no operator=== or operator==)". `===`/`!==` (madc dialect) had NO parse-time DOMAIN-RULE fallback to `operator==`. A user operator=== bound directly (Money member, Tag free); a class with a MEMBER operator== bound via the CIR builder (Plain). But std::string's operator== is a FREE bool-returning template, and `strict_equality_lowering`'s `class_operator_call(top,"==")` delegates the free shape to `std_free_operator_instantiation`, which only binds reference-returning (W2) + by-value-class (operator+) free operators — never a bool-returning one. Normal `a == b` works because the PARSER lowers it to a call via `lower_free_operator_to_call`; `===` was left raw. Fix (parser, deepest layer, reuses the ONE free-operator lowering — no new CIR machinery): `rewritten_operator_candidate` applies the domain rule (spec §2.2/2.4/2.5) after the direct user operator===/!== sets miss — `x === y`→`x == y`, `x !== y`→`!(x == y)` via new `lower_strict_equality_domain_rule` (lowers a synthetic TokenEquals exactly as `==`). Returns NULL (token kept for CIR member-== dispatch / different-domain constant) when no == covers the pair, so member-== (Plain) and member-=== (Money) are untouched; only the free/value-returning == lowers to a call. All 4 paths correct (Plain/string/string-literal via ==, Money via member ===, Tag via free ===); verified vs the g++-canon .expect 16/16. |
| `73cff0a` | **teststringglobal — TEST FIX, ambiguous global `empty` vs C++17 `std::empty` (19 → 18).** `cout << empty` on a file-scope global `string empty;` errored "shift operands should be of an integer type". NOT a operator<</global-string bug (the prior §3 hypothesis): with `using namespace std`, the bare name `empty` is genuinely AMBIGUOUS against the C++17 free fn `std::empty` (`<bits/range_access.h>`, pulled transitively by `<iostream>`). Both g++ AND clang++ REJECT the original: "reference to 'empty' is ambiguous" (candidates `::empty`, `std::empty`). The test only passed against the old embedded shims (no `std::empty`) — it is invalid C++17 under real headers, so this is a TEST bug. Renamed the default-ctor global `empty`→`blank` (+ comment), preserving the test's intent (file-scope std::string + operator<<); madc now compiles+runs it g++-faithfully. LATENT madc gap (NOT fixed, noted for later): madc silently resolves the bare name to `std::empty` (a fn → shift) where the canon compilers hard-error the ambiguity — proper ambiguity detection in unqualified lookup under a using-directive is a separate unstarted feature. Test-only change: binary unchanged at `c518a00`, so unit/torture/SMAUG carry. Gate: integration 584/18. |
| `fe2417a` | **teststdstringconv — TEST FIX, standard 1-arg `std::to_string` (18 → 17).** Called `std::to_string(s, 42)` — a 2-arg (out, value) writer that does NOT exist in real libstdc++ (to_string is the 1-arg value→string converter that RETURNS the string). g++ AND clang++ both reject it ("no matching function for call to to_string(std::string&, int)"); only worked under the old shims (same INVALID-TEST class as teststringglobal). The other calls (std::stoi/std::stod on a std::string) are valid + already work. Rewrote `std::string s; std::to_string(s, 42);` → `std::string s = std::to_string(42);` (+ comment); both canon compilers accept the rewrite; madc runs it (42/12345/3.5). Test-only: binary unchanged at `c518a00`. Gate: integration 585/17. |
| `b3beadb` | **free-operator concrete-param match guard (correctness; NO count flip).** A free-operator candidate whose param is a CONCRETE type (no template param) was accepted against ANY arg in that position: `fn_template_deduce_param` returns 0 ("no deduction") without checking, and `try_instantiate_free_operator_template` only rejected on a negative result. So libstdc++ `std::operator<<(byte, _IntegerType)` (param0 = enum class `byte`, param1 deduced) spuriously matched `stream << x` (param0 never checked) then FATALLY failed instantiating `__byte_op_t<x>` (the std::byte shift machinery: `__byte_operand<x>` has no `::__type` for non-integer x). g++/clang reject it on the byte-vs-stream param. Surfaced on a std::stringstream `<<` chain (testsstream): stringstream is basic_iostream, so its `<<` is NOT taken by the cout/cin fast-path and falls to general free-op resolution. Fix: new `free_operator_concrete_param_matches` — when `fn_template_deduce_param` returns 0, a concrete NAMED (class/enum) param must BE the arg's class or a base (`is_or_derives_from`); scalars/pointers/unresolvable stay permissive. Removes the fatal byte mis-instantiation (`__byte_op_t` gone). testsstream STILL fails on a separate deeper bug (see §0/§3) — standalone correctness fix + prerequisite, NO count flip. Gated: 585/17 FAIL-list byte-identical, unit, torture 1571/51, SMAUG. |
| `cd8dc11` | **Stage 1 part 2b — evaluate TRAIT-EXPRESSION defaulted template type params (feature; +testtmpldefaultcond, NO headline flip).** Extends 2a: when a defaulted type param's default is not a plain base+suffix run (so the fold bails) — e.g. `R = typename std::conditional<COND,A,B>::type` — resolve it fully. New `Program::resolve_template_param_default_type` substitutes the bound params into the default's tokens (param name → its DataDef) and resolves via `resolve_declared_type_token` in an ISOLATED token stream (the recursion-safe alias-resolver pattern: a trait/template-id default recurses into more instantiation, sentinel-draining the shared `tokens` would desync a suspended outer parse); trailing `*`/`&` fold. Wired as the fallback in `instantiate_fn_template_binding` after the 2a fold. `resolve_declared_type_token` already evaluates conditional/conditional_t (verified plain) — the only gap was the defaulted-param slot never invoking resolution. +testtmpldefaultcond (both branches, g++-verified); reducer tmp/cond1.mad passes. Strictly additive (fires only where the loop returned false). **Advances tv1 past the int64_t-fallback wall to a NEW wall (part 2c): `__conditional_t` = `__conditional<C>::template type<If,Else>` (type_traits:134/119), a MEMBER ALIAS TEMPLATE instantiated with args → "type<> expects 0 type argument(s), got 2".** Gated: integration 591/15 (+testtmpldefaultcond, FAIL-list byte-identical, zero regr), unit, gcc-torture 1571/51 byte-identical, SMAUG soak exit 124 + ready. |
| `5104dc1` | **Stage 1 part 2a — evaluate multi-token defaulted template TYPE params (feature; +testtmpldefaultparam, NO headline flip).** `instantiate_fn_template_binding` filled an unbound type param from its default ONLY when the default was a SINGLE token (bound-param name `_Ret=_TRet` or concrete type); any MULTI-token default fell through to `return false` → param unbound → `int64_t` fallback → wrong return + undefined import. Fix (mirrors the class-template default path parser.cpp ~2762): a default is now a BASE token (bound-param name / `ttDataType`) + declarator suffixes (`*`→getPointerType, `&`/`&&`→getReferenceType) + cv-quals, folded as pure DataDef ops (no live token stream). `template<typename T, typename R = T*>` now seats `R=int*`. +testtmpldefaultparam (R=T*, 11/33, g++-verified); reducer tmp/cond2.mad. Strictly more permissive (single-token unchanged), no regression surface. **Layer (b) — trait-expression defaults `_ReturnType=__conditional_t<...>` (stl_iterator.h:1828 __make_move_if_noexcept_iterator, tmp/cond1.mad) — STILL the tv1 blocker** (needs __conditional_t + __move_if_noexcept_cond trait eval; layer (a) alone does not flip tv1). A reference default `R=T&` exposed a separate reference-return-from-defaulted-param gap (deferred). Gated: integration 590/15 (+testtmpldefaultparam, FAIL-list byte-identical, zero regr), unit, gcc-torture 1571/51 byte-identical, SMAUG soak exit 124 + ready. |
| `a12314b` | **Stage 1 part 1 — reference-to-pointer return value type in deduction (feature; reducers pass, NO headline flip).** `operand_value_datadef`'s `returns_ref` branch stripped a pointer level off a reference-return whose referent is a pointer: `int*&` → `int` instead of `int*`. By the `parseFunction` (parser.cpp:29341) / trailing-return (19856) convention `FuncDef::returns` stores the REFERENT directly (`int&`→int, `int*&`→int*, `string&`→string) with `returns_ref` a separate flag, so `returns` ALREADY IS the value type. The strip only fired when the referent was a `DataDefPTR` and was always wrong there; it never served the case it shipped with (c8870aa's `std::move(__x)` returns a CLASS, falls through to `return r`). Fix: return the referent `r` as-is (one spot). Root on tmp/tv1.mad: `__normal_iterator::base()` returns `const _Iterator&`, `_Iterator=int*`; as a deduction arg its value read as scalar `int` → `T*`/`_InIt` deduction failed → `int64_t` default → deep `decltype(*__first)` in `std::uninitialized_copy` threw "cannot dereference non-pointer type". NOT template-specific (reducer tmp/ntref.mad is a plain struct `int*& base()`). Reducers now pass (g++-verified): ntref / ref2 (`const It&`) / ref5 (`It&`); ref3 (assignment) unchanged. Advances all 3 vector tests past the deref wall to NEW walls (no flip, like the sub-gaps): tv1/testvector → `__make_move_if_noexcept_iterator` conditional_t-defaulted RETURN → int64_t (move_iterator conditional_t wall); testvector L17 "incompatible types in assignment to arithmetic lvalue"; testvectorptr L28 "expression before '->' must be a pointer". See plan §4 Stage 1 for the follow-on walls. Gated: integration 589/15 byte-identical (zero regr), unit, gcc-torture 1571/51 byte-identical, SMAUG soak exit 124 + ready. |
| `f03cfb4` | **Stage 0 — array→pointer argument decay in fn-template deduction + call binding (feature; +testtransform bare-array coverage, NO headline flip).** [temp.deduct.call]/[conv.array]: a bare C-array arg used as a VALUE decays to a pointer-to-element. madc models a fixed array as a Variable with the `vfFIXEDARRAY` flag + `var.type` = ELEMENT type, so the arg typed as `int` (not a `DataDefCArray`); THREE layers missed the decay → `transform(a, a+4, b, dbl)` failed: (1) deduction (parser.cpp ~26557) bound the bare type-param to the element type → body `*__first` hit "cannot dereference non-pointer type"; (2) parse-time re-rank (parser.cpp ~26178 `resolved_call_funcdef`) + (3) emit-time ranker (cir_builder.cpp ~659 `call_target_funcdef` non-template path) typed the array as `int` → no match for the instantiated `__o2(int*,...)` overload → call fell back to the declaration-only placeholder → undefined `__ns_std_transform` import. Fix: decay via `array_decay_pointer(operand)` (handles TokenVar fixed-array flag / fixed-array member / DataDefCArray uniformly) at by-value deduction AND both overload-arg-typing sites — NOT in `operand_value_datadef` itself, so reference-to-array deduction keeps the array type. Sibling of the 77a7983 function→fn-ptr decay (the spot the 77a7983 comment flagged "deferred"). Reducer tmp/tr2.mad (bare arrays) → "2 4 6 8"; testtransform.mad extended to cover BOTH pointer-expression and bare-array forms (stale "deferred" comment updated). Stage 0 of docs/plans/2026-06-14-template-instantiation-core-plan.md — contained correctness win; per the plan the 15-count does NOT move on Stage 0 (Stage 1, the declarator-suffix core, is the cascade). Gated: integration 589/15 FAIL-list byte-identical (zero regressions), unit all-pass, gcc-torture 1571 / 51-name failset byte-identical, SMAUG soak exit 124 + "Realms of Despair ready ... port 4000". |
| `77a7983` | **function→fn-ptr argument decay in namespace-fn-template deduction (feature; +testtransform, NO headline flip).** [temp.deduct.call]: deducing a by-value type param from a FUNCTION arg decays it `R(Args)`→`R(*)(Args)`. `try_instantiate_namespace_fn_template` bound the param to the bare FuncDef (`operand_value_datadef` returns the function), so an instantiated `std::transform(b,e,fn)` emitted an incomplete non-callable `void __op` param. Fix: a function arg deduced against a by-value (no `&`) bare type param decays to `DataDefFPTR` → the param is a real `R (*__op)(Args)` and `__op(*__first)` calls through it. Verified vs g++ (testtransform 2 4 6 8, warning-free, via POINTER iterators). DEEP-AREA findings deferred (see §0): bare-array call-site binding gap; for_each anonymous-fn-ptr RETURN renders as `long` (func_def has no fn-returning-fn-ptr declarator → warning, why the test uses transform not for_each); std::string-by-value through an instantiated body corrupts; count_if's `_Iter_pred` wrapper; user GLOBAL free fn templates with deduction don't resolve. Gated: integration 589/15 (+testtransform, zero regressions), unit, torture 51-name failset byte-identical (one memclr.c suite-timeout flake — passes standalone), SMAUG soak exit 124 + ready. |
| `9b7b944` | **C++11 `alignas` keyword support (feature; +testalignas, NO headline flip).** madc mapped the C11 `_Alignas`→`__attribute__` (lexer consumes the specifier + its parens, preserving only layout-attribute names) but had ZERO handling for the C++11 bare keyword `alignas` → `alignas(N) <decl>` parsed `alignas` as a TYPE name and the following `(` tripped the class-member parser: "Unsupported parenthesized member declarator in class definition". Surfaced instantiating std::set/std::map — libstdc++'s `__aligned_membuf<_Tp>` (the `_Rb_tree` node storage, `<bits/aligned_buffer.h>`) declares `alignas(__alignof__(_Tp)) unsigned char _M_storage[sizeof(_Tp)];`. Fix (deepest layer, DRY): map `alignas` to the same lexer define_map path as `_Alignas` so both spellings are consumed identically at member/file/local scope (`alignas` is a C++ keyword + a C11 `<stdalign.h>` macro, never a real identifier). Verified vs g++ (testalignas: `alignas(16)`/`alignas(double)` members parse+run). ADVANCES the set/map chain past the alignas wall to the next `_Rb_tree` wall ("Reference data members (T&) are not supported", a genuine reference-data-member feature), so testset/testcontainerdtor do NOT flip yet (deep chain, not a singleton). NOTE: the alignment value is not yet recorded on the member (stripped, same as `_Alignas`) — a shared follow-up. Reducers tmp/al1 (alignas members, =g++), tmp/set2/set4 (set advanced to the ref-member wall). Gated: integration 588/15 (testalignas new pass, 15-cluster unchanged, zero regressions), unit, torture 1571/51 byte-identical, SMAUG soak exit 124 + ready. |
| `c93c5e1` | **eval wrapper hoists leading #include out of the function body — flips testmadceval (16 → 15).** `build_eval_body_wrapper_source` wrapped the ENTIRE eval source — incl. a leading `#include` — INSIDE the synthetic `double __madc_eval() { ... }`. The lexer inlines header tokens wherever the directive sits, so `<math.h>`→`<cmath>`'s `namespace std` declarations were parsed as FUNCTION-LOCAL statements → "Failed to find type 'std' when parsing function parameters", and eval_double returned garbage (eval_int/bool/string include no header so they passed). `#include` inside a function body is invalid C++ anyway (can't open `namespace std` in a function); the eval API's intent is file-scope includes, and the wrapper is madc's own synthesis (the correct layer). Fix: hoist the CONTIGUOUS leading run of preprocessor directives (blank lines allowed, `\`-continued lines kept whole) to file scope before the function; only the leading run moves so code order is preserved and a mid-body #if/#endif is untouched. NB a SEPARATE pre-existing non-fatal diagnostic ("cannot dereference non-pointer type" parsing `<cmath>`) still prints to stderr — it fires at top-level `#include <math.h>` too (sub-gap-13 class, independent of eval), does NOT affect correctness (eval_double returns 4, exits 0; the runner discards stderr). Reducers tmp/ev1 (eval_double w/ math.h, now returns 4), tmp/ev2 (eval_double no-include control), tmp/mm1 (top-level math.h — shows the same independent <cmath> diagnostic). Gated: integration 587/15 FAIL-list = prior 16 minus testmadceval (zero regressions), unit, torture 1571/51 byte-identical, SMAUG soak exit 124 + ready. |
| `a47c86e` | **inherited-operator return type searches ALL bases — flips testsstream (17 → 16).** `DataDefCLASS::binary_operator_return_type` recursed into only the SINGLE primary base (`base_class`) to find an inherited binary operator's return type — for multiple inheritance that misses an operator inherited from a NON-primary base. `basic_iostream : basic_istream, basic_ostream` keeps `operator<<` in base #2, so a chained `stringstream << a << b` lost its parse-time type: the inner `ss<<a` emitted the correct `_ZStls...basic_ostream(&ss+16,&a)` call but its RESULT type was NULL (primary `basic_istream` has no `<<`), so the outer `<<` saw a non-class operand → builtin shift ("shift operands should be of an integer type"). `ostringstream` (single base `basic_ostream`, offset 0) worked, isolating the MI base-walk as root. Fix (deepest layer): iterate the full `bases` list (the MI direct-base set, which includes the primary) exactly as `is_or_derives_from` does, returning the first base that declares the operator; the legacy single-`base_class` walk is kept only for the (today unreachable) bases-empty path. Combined TEST FIX (INVALID-TEST class): g++/clang both reject `stringstream` without `#include <sstream>` (incomplete type), and the test used the retired `printstream(ss)` builtin (tied to the removed hardcoded dtSSTREAM tag) → added `<sstream>` + standard `cout << ss.str() << endl` (output matches g++ canon); +tests/testsstream.expect. Gated: integration 586/16 FAIL-list = prior 17 minus testsstream (zero regressions), unit, torture 1571/51 byte-identical, SMAUG soak exit 124 + ready. |

## 3. THE NEXT TASK — the clean singletons are EXHAUSTED; remaining = HARD cluster (see §0)
**STRATEGIC FINDING (2026-06-14, proven this session):** the container chain is DEEP and FORKS
per-operation — `tv1` (push_back of int) is at sub-gap 13 while `testvector` (and 3 others) are at a
DIFFERENT wall (`_S_destroy`), each many sub-gaps from green. Container sub-gaps advance the `tv1`
REDUCER but do NOT flip tests. **Headline-count drops come from the INDEPENDENT singletons**: this
session `_ZNSolsESo` (27→21, 6 tests) and `390d8a0` teststringrel (21→20) both did. **Keep picking
count-droppers** from the §3.6b clusters; deepen the container chain only when the independents run out.

**The clean singletons are EXHAUSTED. ALL 15 remaining are deep multi-wall chains** — proven this
session by drilling three of them (set/map, operator->, testforeach2), each gated on multiple distinct
deep features, NOT ~1-2-fix slices:
- **testset / testcontainerdtor** (set/map `_Rb_tree`): alignas (done @9b7b944) → now "Reference data
  members (T&) are not supported" (a `T& member;` in `_Rb_tree` — needs the reference-data-member
  feature: store as pointer, bind in ctor init-list, access transparently; c11-transpiler rule =
  references→pointer) → then more `_Rb_tree`/`_S_destroy` walls.
- **testsubscriptarrow / testvectorptr** ("operator-> on object"): GATED on the `vector<A*>` container
  chain — `vb[0]` must type as `A*&` before `->` works. NOT independent.
- **testforeach2** (std::for_each over functor): function-decay (done @77a7983) is only 1 of ~5 walls —
  bare-array call-site binding, for_each anonymous-fn-ptr RETURN rendering (→`long`, warning),
  std::string-by-value through an instantiated body (corrupts), lambda-as-functor. See §0 `77a7983`.
- **the container/string chain** (testvector/map/teststringref/testsubscript*/testtemplate*/testrefreturn
  /testmadc_ns/testforeachref): the `tv1` chain at sub-gap 13 (`cannot dereference non-pointer type`) +
  `_S_destroy` + `Missing operand` in instantiated bodies — the deep template-instantiation core.

**★ NEXT TASK IS NOW PLANNED — see `docs/plans/2026-06-14-template-instantiation-core-plan.md` @ec93049.**
3-way source recon (clang TreeTransform/SemaTemplateInstantiate, gcc cp/pt.cc tsubst, madc's own token-
substitution web) produced a staged plan for the deep template-instantiation CORE behind all 15 remaining
failures. **Convergent finding:** clang/gcc do SEMANTIC substitution — substitute types into DECLARATIONS
first (concrete-typed params/locals seated in a per-instantiation scope), then re-invoke the SAME
expression builders so result types are recomputed from concrete operands (clang RebuildUnaryOperator→
BuildUnaryOp; gcc INDIRECT_REF→build_x_indirect_ref). madc re-parses substituted TOKENS, so the body's
types are re-derived context-free + lossily. **KEY:** madc already owns operator self-determination (half
the model); the missing half is seating the substituted DECLARATIONS with their concrete `DataDef` before
the body is typed — the `subst` map already HOLDS the right DataDef, it's just flattened to a token.
**Tier 1 (incremental, do first) — 6 gated stages** (full detail + file:line in the plan): Stage 0
array→pointer decay (one spot, `operand_value_datadef`/`fn_template_deduce_param` ~26263, where 77a7983's
comment deferred it; unblocks transform/for_each on arrays); **Stage 1 = THE CORE** — re-apply declarator
suffixes (`*`/`&`) onto the substituted base in the clone loop (mirror `fold_template_arg_declarator`
~10546 at the body USE site) so `_Tp* first` seats as `int*` → kills `cannot dereference non-pointer type`
→ unblocks the ~10-test std::vector chain; Stage 2 iterator-proxy deref (operator*/pointer in
`deref_type_for_variable` ~1277); Stage 3 complete-class identity for std::string by-value (retbuf ABI,
`class_return_via_retbuf` cir_builder ~4191); Stage 4 reference data members (parser ~21932); Stage 5
residuals (`_S_destroy`, fn-returning-fn-ptr RETURN rendering in func_def ~10520). **Tier 2 (durable,
LATER, do NOT start before Tier 1 pays off):** tsubst over the retained `cir_node` parse subtree (MC11-IR
already keeps it) — one `substitute_parse_subtree` primitive replacing the ~86-fn clone-and-reparse web.
RECOMMENDED ORDER: Stage 0 (quick) → Stage 1 (highest leverage) → 2 → 3 → 4 → 5. Confirm scope with user
before Tier 2.

Method: reduce in `tmp/` (DEFAULT mode, no flags), attribute via the 3 oracles + `--emit=c11`/
`--dump-cir`, fix the deepest layer, full 4-gate. Reducer template: `tmp/sr1.mad` (string `a<b`,
fixed) / `tmp/sc1.mad` (direct compare, the control).

**sub-gap 13 (the container chain, if continued):** `tmp/tv1` now → **`cannot dereference non-pointer
type`** (a `*X` where X isn't typed as a pointer, deeper in `_M_realloc_insert`'s body). Reduce from
tv1, 3 oracles + `--emit=c11`/`--dump-cir`, deepest layer. Note testvector etc. ALSO need `_S_destroy`
(`std::allocator_traits<...>::_S_destroy`, a separate static-member wall) — so the container tests
need BOTH, multiple sub-gaps. Also off-path: deduced-return `auto f(){return e;}` (no `-> T`).

### 3.6b — THE CURRENT 15 FAILURES, clustered by first-error (at `77a7983`)
~4 distinct root-cause walls (measured, not assumed — the earlier "container+string share one
root, drops fast" was over-optimistic lumping; the real container chain is ~4 tests, the rest are
independent):
- **container/string template INSTANTIATION (~10)** — the live `tv1` chain + faces: `_S_destroy`
  undeclared (testvector, testforeachref, teststringref, testsubscriptmember); `Missing operand`
  in an instantiated body (testrefreturn, testtemplatecontainer, testtemplatestring); `Expecting
  ',' or '>' in template parameter list` (testmap, testsubscript); `… did not register`
  (testmadc_ns). [teststringrel FIXED @390d8a0; teststringglobal FIXED @73cff0a (test rename).]
- **`Reference data members (T&) are not supported` (2)** — testcontainerdtor, testset (the std::set/map
  `_Rb_tree` chain; ADVANCED @9b7b944 past the prior `alignas`/parenthesized-member-declarator wall —
  a `T& member;` in libstdc++; needs the reference-data-member feature, references→pointer semantics).
- **`expression before '->' must be a pointer` (2)** — testsubscriptarrow, testvectorptr (operator-> after a `vector<A*>` subscript element; GATED on the vector container chain — `vb[0]` must type as `A*&` first, so NOT independent singletons).
- **std-algorithm-with-functor (1)** — testforeach2 (DEEP, ~5 walls: function-decay done @77a7983,
  bare-array call-site binding, for_each anonymous-fn-ptr RETURN rendering, std::string-by-value through
  the instantiated body, lambda-as-functor; see §0/§3). NOT compound-2-fix as earlier believed.
[test3eqclass FIXED @c518a00; teststringglobal FIXED @73cff0a; teststdstringconv FIXED @fe2417a;
testsstream FIXED @a47c86e (byte guard @b3beadb + MI base-walk + test `<sstream>`); testmadceval
FIXED @c93c5e1 (eval wrapper #include hoist); alignas keyword @9b7b944 (+testalignas; advanced set/map
to the ref-data-member wall, no flip); function→fn-ptr decay @77a7983 (+testtransform; advanced
testforeach2, no flip).]
ALL 15 remaining are deep multi-wall (see §3 RECOMMENDATION — pivot to the template-instantiation
CORE). NB the INVALID-TEST class
(teststringglobal/teststdstringconv/testsstream-test-side): always 3-oracle the failing call FIRST —
several of these tests use old-shim convenience signatures real libstdc++ rejects, so the fix is the
test, not madc.

## 4. METHOD + GATES — unchanged from SESSION 8 CLOSE §5 below. DO NOT reapply
`tmp/nontype_fold_v2_wip.patch` (the reverted 202-regression).

## 5. SESSION-9 REDUCER INVENTORY (tmp/ is gitignored — recreate; all DEFAULT mode, no flags)
The promoted ones are now tests (`tests/testmemtmpl*`/`testusingbasemember`/`testvariadicmembertmpl`).
The live wall + the sub-gap-4 probes are NOT tests (they fail) — recreate:
- `tmp/tv1.mad` — `#include <vector>\nint main(){ std::vector<int> v; v.push_back(10); return 0; }`
  (THE wall; now fails at sub-gap 12 — `use of undeclared identifier '_ValueType1'`, see §3).
- `tmp/di1.mad` `typedef int* P; P p(q)` → `*p`=7 · `tmp/di2.mad` `int *p(q)` → 7 · `tmp/di3.mad`
  `int x(5)` → 5 · `tmp/di8.mad` `enum E e(raw)` → 2 — direct-init forms, all **WORK** (sub-gap 11).
  MVP defaults preserved: `tmp/di9.mad` `int f(P)` (typedef param) + `tmp/di10.mad` `int g()` stay
  function declarations. `tmp/k1.c` = gcc-torture `921112-1.c` (K&R def w/ global-shadowing param
  names) — the C-mode-gate regression repro; **WORKS** under `--std=c17`. Promoted: `tests/testdirectinit`.
- `tmp/au1.mad` `auto f(int,int) -> int` → **WORKS** (2). `tmp/au3.mad` `-> decltype(a-b)` → **WORKS**
  (5). `tmp/au4.mad` member `-> decltype(x)`/`-> int` → **WORKS** (21 42). `tmp/au5.mad` member
  `-> decltype(this->x)` → **WORKS** (7). Promoted: `tests/testautotrailingreturn`. (sub-gap 10.)
  `tmp/au2.mad` `auto f(){return a+1;}` (deduced, no `-> T`) → **FAILS** "returning void" (the
  deduced-return follow-up, off the critical path).
- `tmp/ool1.mad` — out-of-line VOID member of a class template (`S<T>::set`) → **WORKS** (7).
  `tmp/ool2.mad` — out-of-line NON-VOID member (`Box<T>::fetch`) → **WORKS** (41, return-type fix).
  `tmp/ool_nt.mad` — out-of-line member of a NON-template class (control) → **WORKS** (9, unchanged).
  Promoted: `tests/testoutoflinemember` (void + non-void + this-access). (sub-gap 8 reducers.)
- `tmp/oot1.mad` — out-of-line member TEMPLATE (`template<class T> template<class U> U S<T>::combine(U)`)
  → **WORKS** (42). Promoted: `tests/testoutoflinemembertemplate` (void + non-void + this-access).
  (sub-gap 9 reducer.)
- `tmp/vmt9.mad` — body `sink(args...)` (plain value-pack expansion) → **WORKS** (the control).
- `tmp/vmt10.mad` — body `sink(std::forward<Args>(args)...)` (PATTERN expansion, type+value pack)
  → **WORKS** (sub-gap-4 repro, fixed @58d6c7f; needs `#include <utility>`).
  Promoted to `tests/testmemtmplpackexpand`.
- `tmp/im1.mad` — instance member tmpl, `this`-independent body (`*p=5`) → **WORKS** (x=5).
- `tmp/im2.mad` — instance member tmpl, member access via this (`*p=base+5`) → **WORKS** (x=35).
  Promoted to `tests/testmemtmplinstance` (the sub-gap-5 proof of the method model).
- `tmp/pn1.mad` — `::new(&buf) int(42)` → **WORKS** (v=42; sub-gap-6 repro, the `int*` placement).
- `tmp/pn4.mad` — `new int(7)` (unqualified HEAP scalar new) → **FAILS** "new without a class
  type" (PRE-EXISTING CIR gap, shared with `::new int`; off the critical path).
- `tmp/pn5.mad` — `new(&b) Pt()` (unqualified CLASS placement) → **FAILS** "incompatible types
  in assignment to struct/union" (PRE-EXISTING CIR gap, shared with `::new(p) T()`; off path).
- (sub-gap 7) `tests/testplacementnewvoidp` (`::new((void*)&buf) int(42)`) → **WORKS** (v=42) once
  the working-tree fix is built; was the void*-store repro.
- `tmp/vmt2.mad` — multi-element pack (pre-existing, off the critical path: deduction binds
  only zero/one pack element, parser.cpp ~25876).
- u3/u4/u5, refm1, vmt5/6/7, mft3 — covered by the promoted tests; recreate from the design doc
  if needed for bisection.

---

# ★ SESSION 8 CLOSE — COLD-START REHYDRATION — 2026-06-13

## 0. One-paragraph orientation
madc is a C/C++ dialect compiler whose front end builds a `cir_node` tree
(== c2mir `node_t`, the MC11-IR) → c2mir → MIR → JIT (sole backend, ADR 0001).
The **retire-embedded-shims campaign** deleted all hand-rolled bucket-3 stdlib
shims, so madc parses the REAL installed glibc + libstdc++ in EVERY mode incl.
default (`presents_as_cpp()`). The live proving wall is `tmp/w2a.mad` =
`#include <vector>\nint main(){ std::vector<int> v; return 0; }`. **Session 8
took w2a from its last face to FULLY COMPILING + RUNNING (exit 0).**

## 1. Live state (verify with `bash scripts/resume.sh`)
- **Branch:** `feature/retire-embedded-shims-claude` @ **`c875fe1`** (LOCAL ONLY,
  never push; develop untouched).
- **MIR fork** `/workspace/mir` @ `5df536f` == `MIR_COMMIT` → satisfied.
- **All gates GREEN** at HEAD: integration **561 passed / 27 failed / 18 skipped**
  (FAIL list byte-identical to `tmp/baseline_fails_s7.txt`); unit (`make -C src
  test`) all-pass; gcc-torture **1571 passed / 51-name failset byte-identical**
  to `docs/parity/torture-failset-current.txt` (ZERO regressions); SMAUG soak
  exit 124 + "Realms of Despair ready ... port 4000".
- Build: `make -C src` (clang++ default), bin at `bin/madc`. Clean, no warnings.

## 2. What session 8 landed (each individually FULLY GATED)
| Commit | What |
|---|---|
| `dd1bc31` | array→pointer decay ([conv.array]) in additive pointer arithmetic (`Program::array_decay_pointer`; `arr+n`/`n+arr`/`arr-n` self-type as `element*` in self-determined ctor/overload-arg contexts). +`tests/testarraydecayadd` |
| `90e9dcd` | **part 19 — non-type template-arg CANONICALIZATION** (Phase 1, canonical-forms discipline). Keyed instantiation identity by raw arg SPELLING → now by VALUE: `Program::canonical_arg_key_fragment` + the NON-re-entrant local-cursor `Program::fold_nontype_template_arg` (int/bool literals + type-trait builtins; `read_local_type_operand` read-only, never touches `Program::tokens`/never instantiates = immune to the 202-regression's two faults). Routed ALL SIX per-arg key-build sites (all-or-nothing). w2a key folded `_Destroy_aux<__has_trivial_destructor(_Value_type)>` → `_Destroy_aux_1`. `typeparam_types` DEFERRED (evaluator self-gates). |
| `fd9b4de` | **part 20 — STATIC MEMBER FN-TEMPLATE INSTANTIATION ("B")** → w2a COMPILES + RUNS. `register_skipped_class_template_function` retains a static body-bearing member template's decl+owner on the FuncDef; `Program::instantiate_member_fn_template_for_call` (parser.cpp, at the namespace-hook point ~10880; LOCAL owner only — exported keep `member_template_method_call`) builds a one-shot `FnTemplateDef` (strip `static`, rename declarator → `<call-sym>__mti` to KEEP real params vs the varargs placeholder) via the shared `try_instantiate_namespace_fn_template`. CRUX (resolved): `tc->var` is a `Variable&` (not rebindable) and the late lib_funcs def emits its raw var name, so alias the CALL — set the placeholder FuncDef's `local_emit_name = inst_name`. |

Plus docs: `037c717` (filed embedded-AST design under docs/plans/), `399ec5b`
(B design doc), `2ac66c7`/`c875fe1` (handoff/status banners).

## 3. THE NEXT TASK — container cluster (testvector etc.)
w2a was a minimal proving ground (`vector<int> v;`); the container TESTS do more
(push/access/iterate). **`testvector` now advances past the `_Destroy_aux` wall to
its next face:** `use of undeclared identifier '_S_destroy'` (`tests/testvector.mad:428`)
— the `std::allocator_traits<...>::_S_destroy` link of the
`_Destroy`→`_S_destroy`→`allocator::destroy` chain. Likely another static-member /
member-template or member-access resolution gap; **reduce from testvector** (default
mode, real headers), attribute via the 3 oracles + `--emit=c11`, fix deepest layer,
gate hard. The ~12-test container cluster (testvector/vectorptr/map/set/
containerdtor/templatecontainer/templatestring/subscript*/teststruct3/test3eqclass)
+ the 5 string-class tests likely share roots. Full failing set: §3.6 of SESSION 7 CLOSE.

## 4. KNOWN gate-clean gaps surfaced this session (NOT blockers; separate slices)
- **Array→pointer decay in template DEDUCTION**: an ARRAY arg (`a`, `a+4`) deduces
  `It=int` not `int*` (`tmp/mft2.mad` → "cannot dereference non-pointer"); pointer
  args work (`tmp/mft3.mad` → sum=100). The deduction analogue of the dd1bc31
  operator fix. (Distinct from the dd1bc31 fix, which is operator-operand typing.)
- **Multi-type instantiation of ONE static member template**: a single
  `local_emit_name` alias slot per placeholder (first wins). A second DISTINCT type
  needs per-type call overload re-selection (like free fns). Rare in the corpus.
- **Trait canonicalization of template-id TYPE args**: `read_local_type_operand`
  doesn't resolve template-id types, so `__has_trivial_destructor(Aux<false>)`
  doesn't fold (`tmp/mft1.mad` prints GENERIC vs g++ TRIVIAL). Phase-1 follow-up.
- Pre-existing (off the live path): reference-typedef-LOCAL binding fuzzy
  (`tmp/ref3`/`ref4`); caught-instantiation stderr noise (wall-7, exit-neutral).

## 5. METHOD + GATES (mandatory)
Per fix: reduce in `tmp/` (DEFAULT mode, no flags = the campaign point, real headers)
→ attribute with the 3 oracles (`clang++ -fsyntax-only`/`-ast-dump`, `gcc -S
-fverbose-asm`, stock `/workspace/mir/c2m FILE -ei`; for madc-path bugs use
`--emit=c11` + `--dump-cir`, NOT emit-C-as-truth) → DEEPEST-layer fix, no shims,
extract shared helpers over copy-paste, REUSE the one instantiation mechanism (the
embedded-AST trajectory guardrail — no parallel instantiator) → rebuild
(`make -C src`) → re-probe reducers → FULL GATE, ONE heavy job at a time, capped
`( ulimit -t 3600; timeout 3000 … )`, FULL logs to `tmp/` (never `| tail`):
1. `bash scripts/run_tests.sh` → diff `^FAIL:` vs `tmp/baseline_fails_s7.txt` (byte-identical; pass count may only rise).
2. `make -C src test` (unit).
3. `python3 scripts/run_gcc_testsuite.py` → failset basename diff vs `docs/parity/torture-failset-current.txt` (51 names; extract via `grep -E '^FAIL\((compile|runtime)\):' | sed -E 's#.*/([^/ ]+\.c) .*#\1#' | sort -u`).
4. SMAUG soak: `cd /workspace/MadSMAUG/runtime/area && ( ulimit -t 120; timeout 50 /workspace/madc/bin/madc --project /workspace/MadSMAUG/compile_commands.json -lcrypt 4000 )` — exit 124 + ready line.
Then commit on THIS branch (Co-Authored-By: Claude Opus 4.8) + sync mirrors (this
banner, `project_retire_embedded_shims` memory + MEMORY.md index < ~24985 bytes).
After killing a run, `pgrep -f run_tests|run_gcc|c2m|bin/madc` to confirm no leftovers.

## 6. REDUCERS (tmp/ is gitignored — recreate if reset)
- `tmp/w2a.mad` — `#include <vector>\nint main(){ std::vector<int> v; return 0; }` (NOW PASSES, exit 0).
- `tmp/nt1.mad`/`nt2.mad` — non-type canonicalization (part 19): nt1→TRIVIAL, nt2 picks `<true>` spec.
- `tmp/mft1.mad` (member-tmpl, runs both branches), `tmp/mft2.mad` (param-using body, ARRAY args — hits the deduction-decay gap), `tmp/mft3.mad` (param-using body, POINTER args → sum=100).
- Older: `tmp/baseline_fails_s7.txt` (27-name FAIL baseline), `tmp/nontype_fold_v2_wip.patch` (the reverted 202-regression — DO NOT reapply).

## 7. MERGE GATE (do NOT merge to develop before ALL)
fulltest 100% green + both check gates + P1 partition gate · torture zero-regr vs
51-name baseline · SMAUG soak · `bash scripts/run_tests.sh --exe` · mirrors synced
· user approval (develop is shared/stable). Promote develop→master is a SEPARATE,
later gate (gcc-torture parity, `.claude/rules/branching.md`).

---

## STATUS UPDATE 2026-06-13 (session 8, part 20) — static member fn-template instantiation (the "B" feature); w2a (`std::vector<int> v;`) COMPILES + RUNS end-to-end

**COMMITTED `fd9b4de`, FULLY GATED** (integration 561/27 FAIL-list byte-identical,
unit all-pass, gcc-torture 1571 / 51-name failset byte-identical, SMAUG soak exit
124 + ready). Design + as-built: `docs/plans/2026-06-13-member-fn-template-instantiation-design.md`.

**THE WALL IS DOWN:** `tmp/w2a.mad` (`#include <vector>\nstd::vector<int> v;`) now
compiles AND runs (exit 0). A class's STATIC member FUNCTION template of a madc-LOCAL
monomorphized class (`_Destroy_aux<true>::__destroy<int*>`) was emitted as a bare
undefined extern; it now instantiates on ODR-use.
- `register_skipped_class_template_function` retains the static body-bearing member
  template's decl + owner on the FuncDef.
- `instantiate_member_fn_template_for_call` (parser.cpp, at the namespace-hook point
  10880): LOCAL owner only (exported → `member_template_method_call`); builds a
  one-shot `FnTemplateDef` (strip `static`, rename declarator to a DISTINCT
  `<call-sym>__mti` to KEEP real params vs the varargs placeholder) and instantiates
  via the shared `try_instantiate_namespace_fn_template`. Reuses the ONE instantiation
  mechanism — no parallel instantiator (embedded-AST trajectory guardrail).
- CRUX (resolved): `tc->var` is a `Variable &` (not rebindable) and the late
  (lib_funcs) definition emits its raw var name, so alias the CALL not the def — set
  the PLACEHOLDER FuncDef's `local_emit_name = inst_name`.

**NEXT (container cluster — testvector etc.):** w2a was a minimal proving ground; the
container TESTS do more (push/access/iterate). `testvector` now advances past the
`_Destroy_aux` wall to the next chain link: **`use of undeclared identifier '_S_destroy'`**
(testvector.mad:428) — `std::allocator_traits<...>::_S_destroy` (or the `_Destroy`→
`_S_destroy`→`allocator::destroy` chain). Likely another static-member / member-template
or a member-access resolution gap; reduce from testvector. Known separate gaps surfaced
(NOT blockers, gate-clean): array→pointer decay in template DEDUCTION (array args →
`It=int` not `int*`, `tmp/mft2.mad`); multi-type instantiation of one static member
template (single `local_emit_name` slot); trait canonicalization of template-id type
args (`__has_trivial_destructor(Aux<false>)`).

---

## STATUS UPDATE 2026-06-13 (session 8, part 19) — non-type template-arg CANONICALIZATION (Phase 1, the canonical-forms discipline); w2a folds to `_Destroy_aux_1`

**COMMITTED `90e9dcd`, FULLY GATED** (integration 561/27 FAIL-list byte-identical
to `tmp/baseline_fails_s7.txt` +testarraydecayadd, unit all-pass, gcc-torture 1571 /
51-name failset byte-identical, SMAUG soak exit 124 + ready line). Implements
**Phase 1** of `docs/plans/2026-06-13-canonical-forms-on-mc11ir-sketch.md`.

**The fix:** madc keyed a template instantiation's identity by the raw SPELLING
of its non-type args, so `<true>`, `<1>`, `<__has_trivial_destructor(int)>` were
THREE different instantiations — the w2a use never selected the matching `<true>`
spec. Now a foldable non-type arg keys by its VALUE.
- `Program::fold_nontype_template_arg()` — a **NON-re-entrant LOCAL-CURSOR**
  evaluator over the already-collected arg tokens. Folds int/bool literals (reuses
  `parse_simple_template_non_type_value`) + the type-trait builtins (reuses the
  `trait_*` DataDef predicates). `read_local_type_operand` resolves type operands
  read-only (ttDataType / datatype_map / trailing `*`). NEVER touches
  `Program::tokens`, never instantiates — the two faults behind the **202-regression**.
- `Program::canonical_arg_key_fragment()` — foldable arg → normalized value
  (identifier-safe), else `sanitize_template_fragment(spelling)` (byte-identical).
- **Routed ALL SIX per-arg key-build sites** (use, opaque, both alias paths, both
  explicit-spec registration keys). All-or-nothing is mandatory (the 202-regression
  lesson); opaque/dependent paths pass empty tokens → spelling fallback.

**Effect:** `tmp/nt1.mad` → TRIVIAL (was NONTRIVIAL); `tmp/nt2.mad` selects the
`<true>` spec. **w2a ADVANCED**: key folded
`_Destroy_aux___has_trivial_destructor__Value_type_` → **`_Destroy_aux_1`** (spec
chain selected). NEXT = the separate **"B" feature**: member-template instantiation
of `_Destroy_aux<true>::__destroy<int*>` (currently a bare undefined import
`_Destroy_aux_1____destroy`) — capture the member-fn-template body + monomorphize
per ODR-use, reusing `instantiate_fn_template_binding`. **GROUNDED DESIGN (read
first):** `docs/plans/2026-06-13-member-fn-template-instantiation-design.md` — body
capture + a parse-time `instantiate_member_fn_template_for_call` hook (sibling of
the namespace one at parser.cpp:10880) + a parser-side deducer + THE CRUX
(register/rebind the instantiated method under the name the already-bound call
resolves to). Reducer `tmp/mft1.mad` (library-independent). Static-only first;
gate HARD (this is the 202-regression / SIGSEGV-revert area). DEFERRED: `typeparam_types`
(bool-normalizing nonzero ints, `<2>`==`<true>`) — the evaluator self-gates so it
is not needed for the cleared corpus.

---

# ★ SESSION 7 CLOSE — COMPREHENSIVE REHYDRATION (READ THIS FIRST) — 2026-06-13

## 0. One-paragraph orientation

madc is a C/C++ dialect compiler whose front end builds a `cir_node` tree
(== c2mir `node_t`, the MC11-IR) fed to c2mir → MIR → JIT (sole backend; see
`docs/adr/0001`). The **retire-embedded-shims campaign** deleted all hand-rolled
"bucket-3" stdlib shims so madc now parses the REAL installed glibc + libstdc++
(`/usr/include/c++/13`) in EVERY mode incl. default (default mode `presents_as_cpp()`).
Every integration failure since is a *latent real-header bug* the shims used to
hide. The campaign's live proving ground is `tmp/w2a.mad` =
`#include <vector>\nint main(){ std::vector<int> v; return 0; }` — getting the real
`std::vector<int>` to compile+run end-to-end. **This session drove w2a from 35
c2m check-errors down to 1.**

## 1. Live state (verify with `bash scripts/resume.sh`)

- **Branch:** `feature/retire-embedded-shims-claude` @ **`a64fd00`** (LOCAL ONLY —
  never push; develop untouched). 
- **MIR fork** `/workspace/mir` @ `5df536f` == `MIR_COMMIT` pin → satisfied.
- **All gates GREEN** at HEAD: integration suite **560 passed / 27 failed / 18
  skipped** (FAIL list byte-identical to `tmp/baseline_fails_s7.txt`; +`testnestedtemplatedtor` +`testpartialorder`);
  unit (`make -C src test`) all-pass; gcc-torture **1571 passed / 51-name failset
  byte-identical** to `docs/parity/torture-failset-current.txt` (ZERO regressions);
  SMAUG soak green (exit 124 + "Realms of Despair ready ... port 4000").
- Build: `make -C src` (clang++ by default). bin at `bin/madc`.

## 2. What session 7 landed (parts 11-14, each individually FULLY GATED)

| Commit | What | w2a |
|---|---|---|
| `e048e9e` | **call a parenthesized function designator `(E)(args)`** — `(std::max)(a,b)` (libstdc++ parenthesizes to suppress macros/ADL) was dropping the callee. New parseExpression branch keyed on `prevToken()==)` + a function-typed `TokenVar` + following `(`; no `src_node` so parseCallFunc rebinds the instantiated template funcdef. | 5→4 |
| `8ba77dc` | **scope a non-compound if-branch's materialized temps** — new `CirBuilder::translate_branch_stmt` wraps a single-statement then/else + its temps in one block (the `true_type()` temp in `vector::_M_move_assign(false_type)` was leaking into the else block). | 4→2 |
| `4a50ae0` | **instantiate alias templates with a non-type param** — `conditional_t`/`enable_if_t<bool,…>` were opaque placeholders; the `has_non_type_params` path of `instantiate_template_alias_use` now token-substitutes args into the alias body + resolves in an **isolated token stream** (save `tokens` / swap a fresh deque / restore — shared-stream push/drain SIGSEGV'd, reverted). +`tests/testconditionalt`. (General fix; did not itself clear w2a.) | 2→2 |
| `47e2f89` | **reference-qualified type as a template argument** — extracted ONE `Program::fold_template_arg_declarator(adt,origin)` helper (consumes `*`/`&`/`&&`, wraps via getPointer/getReferenceType) and replaced the COPY-PASTED `*`-only folds in `instantiate_template_use` (explicit) + `instantiate_template_alias_use` (type-only). **Cleared w2a's move_iterator `conditional_t` wall.** +`tests/testreftemplatearg`. | 2→1 |

Plus docs commits `554e5d2`/`38f90eb`/`97bdb9d`/`468d5d8`/`74f061e` (banners, the
research doc, status/memory sync).

## 3. THE NEXT TASK — w2a: non-type template-arg CANONICALIZATION + member-template instantiation

> **READ FIRST: `docs/plans/2026-06-13-nontype-template-arg-canonicalization-research.md`** (part-19
> research). A first attempt to fold the non-type arg at the collection site **regressed 202 tests**
> (reverted; saved at `tmp/nontype_fold_v2_wip.patch` — do NOT reapply). Root cause: madc keys
> instantiations by raw arg SPELLING in MANY places, so folding at a couple of sites makes keys
> inconsistent across the header corpus; plus the isolated-stream evaluator is re-entrancy-fragile
> during header parsing. clang canonicalizes a non-type arg to **(value, param-type)** once at SEMA
> (`<true>`==`<1>`==`<trait()>`). The fix is two independent, non-trivial pieces: **(A)** canonicalize
> at the ONE key-construction chokepoint with a NON-re-entrant local-cursor evaluator + record
> non-type param TYPES in `TemplateDef`; **(B)** member-template instantiation of the selected spec's
> `_Destroy_aux<true>::__destroy<int*>` (still a bare extern — the p18-reframed feature). w2a needs
> BOTH. Reducers: `tmp/nt1.mad` (member-access, → must print TRIVIAL), `tmp/nt2.mad` (direct decl).
> The older framing below predates the research; the research doc supersedes it.



**THE part-18 reframing (`a64fd00`, gated):** part-17's "member-template instantiation
during late materialization" hypothesis was the WRONG LAYER. `allocator_traits::destroy`
is `[[__gnu__::__always_inline__]]` — libstdc++ exports NOTHING for it (`nm -D` count 0).
g++ never calls it: it selects the more-specialized `_Destroy(_FwdIt,_FwdIt,allocator<_Tp>&)`
(alloc_traits.h:942), which delegates to the trivially-destructible-aware 2-arg
`_Destroy(first,last)`. madc picked the general `_Destroy(...,_Allocator&)` because it had
**no function-template partial ordering** ([temp.func.order]) — `instantiate_namespace_fn_template_for_call`
took the first candidate that deduced (registration order). Fix = partial ordering +
template-id param deduction + `__has_trivial_destructor` builtin (see the part-18 banner).
DEEPEST LAYER lesson: don't build machinery (member-template instantiation) to service a call
g++ never makes — fix the overload mis-selection.

**Repro (current single face — the wall ADVANCED):**
```
rm -f tmp/*.madh
bin/madc tmp/w2a.mad 2>&1 | grep -v -i 'warning\|setrlimit'
#  -> MIR error: import of undefined item _Destroy_aux___has_trivial_destructor__Value_type_____destroy
```
**Diagnosis (from `bin/madc --emit=c11 tmp/w2a.mad > tmp/w2a_emit.c`):** the specialized
`_Destroy` chain is now selected — `__ns_std__Destroy__o2` (3-arg, ~L804) → `__ns_std__Destroy__o3`
(2-arg, ~L807) → `typedef int _Value_type;` (L808, _Value_type RESOLVES to int) →
`_Destroy_aux___has_trivial_destructor__Value_type_____destroy(__first, __last)` (L809).
`struct _Destroy_aux_true` already EXISTS (L60). The bug: the template-id
`_Destroy_aux<__has_trivial_destructor(_Value_type)>` flattens its non-type arg
expression RAW into the instantiation name instead of EVALUATING it to `true` (→ `_Destroy_aux_true`).
`__has_trivial_destructor(int)` DOES fold in expression position (the new builtin), but is NOT
reached in template-argument position.

**The slice:** evaluate non-type template-ARGUMENT *expressions* during class template-id arg
collection so `_Destroy_aux<__has_trivial_destructor(_Value_type)>` selects the `<true>`
partial spec. Today `non_type_partial_spec_arg_matches` (parser.cpp ~12808) only matches via
`parse_simple_template_non_type_value` (literal ints) — the unevaluated trait-call spelling never
matches `_Destroy_aux<true>`. Find where the `_Destroy_aux<...>` member-access template-id's
concrete args are collected/sanitized (the flattened name `_Destroy_aux___has_trivial_destructor__Value_type_`
is built by `sanitize_template_fragment` on the raw arg) and route a non-type arg through the
constant/expression evaluator (which now knows `__has_trivial_destructor`) BEFORE building the
instantiation key. Attribute on the REAL w2a path with `--emit=c11` (reducers diverge: `tmp/po4.mad`
hit the global/ns fn-template instantiation gap `__ns_ns_destroy_one`; `tmp/po2.mad` user-class arg
gives `acls=null` — user template-ids carry NO `canonical_cpp_spelling`, unlike libstdc++ classes).

After it: `std::vector<int> v;` should COMPILE, then RUN it — unblocking the ~12-test container
cluster + testmadc_ns per §3.6.

**KNOWN pre-existing limitation surfaced (NOT triggered by w2a; follow-up):** the fn-template
`inst_key` memo (`fn_template_instantiated`, parser.cpp ~25771) encodes only typeparam VALUES, so
two same-name overloads sharing a typeparam that binds to the same type COLLIDE (the second reuses
the first's instantiation). w2a's `_Destroy` overloads bind DIFFERENT typeparams (`_Allocator=allocator<int>`
vs `_Tp=int`), so unaffected. Fix later: key on the overload signature too.

## 4. RESEARCH — reference types as template arguments (DONE this session)

Full writeup: **`docs/plans/2026-06-13-reference-template-args-research.md`**. Summary:
- The entire move_iterator blocker reduced to ONE feature — a **reference-qualified type
  as a template argument** (`Tmpl<int&>`, `Tmpl<T&&>`). Verified everything else already
  works (member alias templates, gcc-13 internal `__conditional_t`, traits, `__base_ref`,
  reference RETURN types).
- **Recon assets (KEEP):** clang-18.1.3 binary is the **oracle** (`clang++ -Xclang
  -ast-dump` / `-fsyntax-only`; confirmed `move_iterator<int*-iter>::reference` == `int&&`).
  Sparse clang **frontend source** at **`/workspace/llvm-clang-src`** (55M, Apache-2.0,
  `clang/lib/Sema` + `clang/lib/AST` + includes) — RECON ONLY, not vendored, madc stays lean.
  Canonical reference-collapsing rule from `clang/lib/Sema/SemaType.cpp:2250`
  `Sema::BuildReferenceType`: `LValueRef = spelledAsLValue || base isa LValueReferenceType`
  (so `T& &`/`T& &&`/`T&& &`→`T&`; only `T&&`-of-nonref stays `T&&`). madc models ALL
  references as one `DataDefREF` (no lvalue/rvalue split in the type) and `getReferenceType`
  already collapses, so madc's version is simpler but correct for its pointer-model.
- **Design principle reaffirmed by the user:** keep folding/reusing/condensing — one shared
  helper per rule, less hard-coding, because the **polyglot** intent (one IR, many
  source/target languages) needs a modular, general parser. The `fold_template_arg_declarator`
  extraction is the template of this: it REMOVED duplication while adding a feature.

## 5. KNOWN GAPS / SEPARATE BUGS (off the w2a path — do not conflate)

- **reference-typedef/alias LOCAL binding is fuzzy/broken** (PRE-EXISTING, no templates):
  `typedef int& RT; RT r = n; r = 7;` prints 5 not 7 ("assigning integer without cast to
  pointer") — `tmp/ref3.mad`/`tmp/ref4.mad`. A reference reached via a typedef/alias isn't
  given reference-BINDING semantics for a LOCAL (works for a DIRECT `int& r` and for RETURN
  types — `tmp/ref1.mad`/`tmp/ref2.mad`). Its own slice (reference-aliased-local binding).
- **`remove_reference<int&>::type` as a bound local** has the same fuzzy semantics (newly
  reachable after p14; off w2a path).
- **`a + N` array-decay** as a ctor argument types as INT (pointer decay missing in that
  position) — surfaced by w17/w19, not in w2a.
- **Caught-instantiation stderr noise** (wall-7 class): the p13 isolated resolve's caught
  exceptions still PRINT ("Expecting…type<…>") because `throwbuf::sync` writes
  unconditionally; exit-code-neutral. A discarded candidate should print nothing — fix at
  emission/diagnostics suppression.
- **Further condense candidate:** the builtin-trait `*`-fold (parser.cpp ~4757,
  `__is_pointer(int*)`) is DataDef-based and could share a DataDef-level variant of
  `fold_template_arg_declarator`. Continues the fold/reuse mandate.

## 6. METHOD + GATES (mandatory, unchanged)

Per fix: reduce in `tmp/` (DEFAULT mode — no flags — is the campaign point; real headers)
→ attribute with the **3 oracles** (`clang++ -fsyntax-only`/`-ast-dump`,
`gcc -S -fverbose-asm`, stock `/workspace/mir/c2m FILE -ei`; for madc-path bugs use
`--dump-cir`, NOT emit-C-as-truth) → DEEPEST-layer fix, no shims/special-cases, extract a
shared helper over copy-paste → rebuild (`make -C src`) → re-probe reducers → FULL GATE,
ONE heavy job at a time, capped `( ulimit -t 3600; timeout 3000 … )`:
1. `bash scripts/run_tests.sh` → diff `^FAIL:` list vs `tmp/baseline_fails_s7.txt` (must be
   byte-identical; pass count may only rise).
2. `make -C src test` (unit).
3. `python3 scripts/run_gcc_testsuite.py` → basename failset diff vs
   `docs/parity/torture-failset-current.txt` (51 names, byte-identical).
4. SMAUG soak: `cd /workspace/MadSMAUG/runtime/area && ( ulimit -t 120; timeout 50
   /workspace/madc/bin/madc --project /workspace/MadSMAUG/compile_commands.json -lcrypt 4000 )`
   — exit 124 + ready line = good.
Then commit on THIS branch (Co-Authored-By: Claude Opus 4.8) and sync the mirrors
(claude_status.json head line REPLACED not appended; this handoff banner; memory
`project_retire_embedded_shims` + MEMORY.md index — MEMORY.md must stay < ~24985 bytes).
Background long runs (`run_in_background`), FULL logs to `tmp/` (never `| tail` — truncates
the failset). After killing a run, `pgrep -f run_tests|run_gcc|c2m|bin/madc` to confirm no
leftovers before restarting.

## 7. REDUCER INVENTORY (tmp/ is gitignored — recreate if the tree was reset)

- `tmp/w2a.mad` — `#include <vector>\nint main(){ std::vector<int> v; return 0; }` (THE wall).
- `tmp/mi2.mad` — make_move_iterator + `*mi` (move_iterator operator* instantiation).
- `tmp/mi5.mad` — exact gcc-13 move_iterator mirror (internal `__conditional_t` member-alias
  template + member-alias base_ref + `remove_reference<base_ref>::type&&` true branch); PASSES.
- `tmp/rt1.mad` `conditional_t<true,int&&,long>`, `tmp/rt2.mad` `conditional_t<true,int&,long>`,
  `tmp/rr1.mad` `remove_reference<int&>::type` — reference-arg parse; PASS.
- `tmp/ct1..ct5.mad` — conditional_t / is_reference probes. `tmp/mat1.mad` member alias
  template, `tmp/ic1.mad` internal __conditional_t form — PASS.
- `tmp/ref1..ref4.mad` — reference local/return/typedef-binding (ref1/ref2 pass; ref3/ref4
  show the pre-existing typedef-local gap). `tmp/mi3.mad`/`tmp/mi4.mad` bridge reducers.
- WIP patches (NOT committed, in tmp/): `conditional_t_alias_wip.patch` (superseded by
  4a50ae0), `reftemplatearg_wip.patch` (the one-loop version, superseded by 47e2f89's helper).

## 8. CAMPAIGN CONTEXT (condensed; full detail in §0-§8 of the original handoff below)

- **Partition model** (`madc-header-partition-handoff.md`): madc owns ONLY bucket-1/2
  freestanding/compiler headers (float/limits/stdarg/stdbool/stddef/stdint) + madc's own
  `ns_*`; ALL glibc + ALL libstdc++ are consumed REAL/unmodified (bucket 3, DELETED shims).
  Authority for bucket 1/2 = `gcc -print-file-name=include`.
- **User rulings (binding):** no shims / no per-case hacks / fix at the deepest layer
  (categorical); K&R recovery only under explicit `--std=c78..c17`; don't lift
  `__STRICT_ANSI__` from STD_MADC/C++ modes until `__float128`/`_FloatN` land (it opens
  glibc float regions madc can't type — cost a 222-fail run once); develop is LOCAL/stable,
  promote only at the gcc-torture parity gate (`.claude/rules/branching.md`).
- **Walls already cleared this campaign:** K&R gate, presents_as_cpp, the shim sweep, PP
  root causes, SFINAE pre-check, wall-4 (sys headers / SMAUG boots on real glibc), eval
  scope capture, and the session-7 w2a faces above.
- **Merge gate (do NOT merge to develop before ALL):** fulltest 100% green + both check
  gates + P1 partition gate · torture zero-regr vs 51-name baseline · SMAUG soak ·
  `bash scripts/run_tests.sh --exe` · mirrors synced · user approval.

---

## STATUS UPDATE 2026-06-13 (session 8, part 18) — function-template PARTIAL ORDERING + template-id deduction + __has_trivial_destructor (the part-17 "next task" was the wrong layer)

**COMMITTED `a64fd00`, FULLY GATED** (integration 560/27 byte-identical +`testpartialorder`,
unit all-pass, gcc-torture 1571 / 51-name failset byte-identical, SMAUG soak green).

**The reframing:** part-17's NEXT-task (member-template instantiation of `allocator_traits::destroy`)
was the WRONG LAYER. `nm -D libstdc++` exports ZERO `allocator_traits` symbols — `destroy` is
`[[__gnu__::__always_inline__]]`, header-only; mangling/instantiating it services a call **g++ never
makes**. g++ selects the more-specialized `_Destroy(_FwdIt,_FwdIt,allocator<_Tp>&)` (alloc_traits.h:942)
→ trivially-destructible-aware 2-arg `_Destroy` → empty for int. madc picked the general
`_Destroy(...,_Allocator&)` because it had **NO function-template partial ordering**
(`instantiate_namespace_fn_template_for_call` took the first registration-order candidate that deduced).

**Three coupled pieces (all parser.cpp), deepest layer, no shims:**
1. **Partial ordering ([temp.func.order])**: `po_more_specialized` / `po_at_least_specialized` —
   symbolic deduction of one template's params (deduction vars) against the other's parameter list
   (its params = unique opaque atoms), with backtracking + binding consistency in `po_pattern_match`.
   `instantiate_namespace_fn_template_for_call` now orders candidates most-specialized-first;
   incomparable candidates keep registration order (selection changes ONLY on a genuine specialization
   relationship — surgical, hence zero regressions). Handles pointer (`T*`>`T`) AND template-id
   (`allocator<_Tp>`>`_Allocator`).
2. **Template-id param deduction**: `try_instantiate_namespace_fn_template` deduces inner params from a
   template-id parameter (`allocator<_Tp>&` ← `allocator<int>&`) by reusing
   `Program::unify_nested_spec_pattern_arg` against the arg class's `canonical_cpp_spelling`
   (`fn_template_deduce_param` only did single-word cores). This makes the specialized `_Destroy`
   deducible, hence selectable.
3. **`__has_trivial_destructor(T)` builtin** (gcc-13 intrinsic, faithful [class.dtor] triviality):
   added to the `type_trait_arity` / `evaluate_type_trait` table.

**w2a arc:** `allocator_traits::destroy` (undefined) → `_Destroy_aux<__has_trivial_destructor(_Value_type)>`
(undefined). The specialized chain is selected, `_Value_type` resolves to int, `struct _Destroy_aux_true`
exists — the remaining gap is non-type template-ARG expression evaluation (see §3, the NEW next task).
Reducers: `tmp/po5.mad` (T*>T, → `tests/testpartialorder`), `tmp/po2.mad`/`tmp/po4.mad` (diverge — user-class
no canonical_cpp_spelling / global-fn-template gap). ROADMAP 2.10 added (flattened→mangled naming unification).

---

## STATUS UPDATE 2026-06-13 (session 8, parts 16-17) — EAGER→LAZY template member instantiation + late free-fn emission (w2a advances deep)

**COMMITTED `f76c07c` (part 16) + `4cde0e2` (part 17), each FULLY GATED** (suite 559/27
byte-identical, unit all-pass, gcc-torture 1571 / 51-name failset byte-identical, SMAUG
soak green).

**Part 16 (`f76c07c`) — the deep one. Template member bodies now instantiate LAZILY**
([temp.inst]p2: implicit class instantiation instantiates member DECLARATIONS, not
DEFINITIONS). madc was EAGER — `std::vector<int> v;` force-parsed all 70 vector members
incl. never-ODR-used ones (`operator=(vector&&)`'s `constexpr __move_storage`,
`__alloc_traits::construct`), which were w2a's "2 faces". The lazy machinery existed but
was gated on `from_system_header`, WRONG for instantiated templates. DEEPEST root cause:
`TokenBase`'s ctors stamp file/line/column from the STATIC `_parse_file`, and `clone()`
builds tokens through them — so cloning a template's body during instantiation stamped
every clone with the INSTANTIATION SITE (`tmp/w2a.mad`), discarding the system-header
origin → `from_system_header=0` → eager. (Same bug = the long-standing errors
misattributed to "2:28".) FIX (instantiate_template_use): point `_parse_file`/line/column
at the template body's defining file across the clone loop, then restore; `nextToken()`
propagates it from each consumed token during the re-parse, so `from_system_header`, lazy
deferral, AND error attribution all see the true origin. Also decide lazy-vs-eager PER BODY
(each body's own origin file) not the class-level flag. User-template instantiation stays
eager (verified `tmp/lazy1.mad`).

**Part 17 (`4cde0e2`) — late free-fn-template emission.** A lazily-materialized body
(vector's dtor, parsed by the cir fixpoint) can instantiate a free fn template it calls
(`std::_Destroy`); that new TokenFunc appends to `prog->pending_funcs` but the fixpoint
only consumed `parse_deferred_lazy_body`'s returned `.back()`, orphaning `_Destroy`
(referenced extern, never defined → MIR "import of undefined item __ns_std__Destroy__o2").
FIX: `materialize_and_lower` drains TokenFuncs appended past the entry boundary into
`lib_funcs` each round (deduped, recorded as materialized-lib syms); the reachability loop
defines them when ODR-used; transitive instantiations caught by the grew-loop.

**w2a arc this session:** 35 → 1 → (p15 dtor-dedup) → 2 faces → (p16 lazy-inst) → 1 →
(p17 drain) → `allocator_traits::destroy` (member-template in late materialization — see
§3, the NEXT slice). Reducers: tmp/ff1 (free-fn-tmpl, works), tmp/lazy1 (user template
stays eager), tmp/mt1 (member-tmpl reduce — hit a DIFFERENT bug, area has multiple issues).

---

## STATUS UPDATE 2026-06-13 (session 8, part 15) — emit a nested-template class's dtor once (w2a 1→2-new-faces; dup-emission wall DOWN)

**COMMITTED `2a46bed`, FULLY GATED.** suite 558/27 (FAIL list byte-identical;
`tests/testnestedtemplatedtor` added post-snapshot → 559 next run), unit all-pass,
gcc-torture 1571 / 51-name failset byte-identical (ZERO regressions), SMAUG soak
green. **w2a's "Repeated item declaration ..._Temporary_value___Storage___dtor"
wall CLEARED.**

ROOT CAUSE (part-14 hypothesis (b) "two instances" was WRONG — verified by pointer:
ONE `DataDefCLASS`): a nested class inside a class template keeps its SOURCE-TAG
`method_map` keys (`Inner`/`~Inner`) while its composed name becomes
`Outer_int32_t__Inner`. Every emitter dtor lookup did `method_map.find("~"+cdd->name)`
= `"~Outer_int32_t__Inner"` → MISS → `class_has_own_user_dtor` false → the
synthesized-dtor pass wrongly fired → the dtor was emitted TWICE (an empty synth body
AND the full user body). LIBRARY-INDEPENDENT — reduced to a 13-line pure-user template
(`tmp/dup3.mad`, no libstdc++).

FIX (deepest layer, name-independent): new `Variable *CirBuilder::class_own_dtor(cdd)`
— the own dtor is the `~`-prefixed `method_map` entry whose Variable is ALSO in
`cdd->methods` (inherited dtors are copied into `method_map` ONLY by the base-merge at
parser.cpp ~20600, never into `methods`). Mirrors how a destructor is identified
structurally (by kind) rather than by reconstructing the key from `cdd->name`. Routed
all four `"~"+cdd->name` dtor-lookup sites (class_member_destruct, class_dtor_symbol,
the external-class dtor binding, class_has_own_user_dtor) through it — net LESS code.
Diagnostics were `#ifdef MADC_DEBUG_DTOREMIT` gated (build with
`make -C src FEATURE_DEFINES=-DMADC_DEBUG_DTOREMIT`), then removed. New regression test
`tests/testnestedtemplatedtor` (dtor fires exactly once: `val 7` / `dtor 7`).
Reducers: `tmp/dup1.mad` (by-value Inner return), `tmp/dup2.mad` (Inner unused → 1 dtor,
the negative control), `tmp/dup3.mad` (Inner referenced by a member → 2 dtors, the bug).

### w2a's 2 NEW faces (see §3 above — start there next session)

`std::vector<int> v;` now advances past the dup-emission wall to two NEW, distinct,
dtor-UNRELATED faces (previously masked): (1) `Expecting integer constant expression`
at the decl site (a constexpr in the instantiated vector body not folding); (2) undefined
`__alloc_traits…construct` (a member TEMPLATE referenced but never instantiated — cf. its
`destroy` sibling that DOES emit). w2a arc: 35 → 1 → (dup wall down) → 2 new faces.

---

## STATUS UPDATE 2026-06-13 (session 7, part 14) — reference-qualified type as a template arg (shared fold helper); w2a move_iterator wall DOWN

**COMMITTED `47e2f89`, FULLY GATED.** suite 558/27 (+testreftemplatearg, failure list
byte-identical), unit all-pass, gcc-torture 1571 / 51-name failset byte-identical
(zero regressions), SMAUG soak green. **w2a's 2 move_iterator conditional_t errors
CLEARED**; w2a now advances to a NEW, distinct error (see below).

ROOT CAUSE: a reference-qualified TYPE is a valid template argument
(`conditional<b, int&, long>`, `__conditional_t<…, remove_reference_t<R>&&, …>`),
but the explicit template-arg parsers folded only a trailing `*` and threw on
`&`/`&&` — blocking move_iterator::reference (the conditional_t went opaque).
LEAN FIX (per the polyglot/modular steer — the `*`-fold was copy-pasted across the
arg loops): extract ONE `Program::fold_template_arg_declarator(adt, origin)`
(consumes `*`/`&`/`&&`, wraps via getPointerType/getReferenceType; the latter already
collapses, and madc models all references as one DataDefREF) and replace the
duplicated `*`-only loops in instantiate_template_use (explicit) +
instantiate_template_alias_use (type-only). Net: LESS code + reference support
uniformly. Reducers tmp/{rt1,rt2,rr1,mi5} pass (mi5 = exact move_iterator mirror).
Research/design: `docs/plans/2026-06-13-reference-template-args-research.md`. Clang
oracle confirmed the canonical type is `int&&`; clang BuildReferenceType gave the
collapsing rule (recon at /workspace/llvm-clang-src, Apache-2.0, not vendored).
KNOWN narrow corner (NOT on w2a path): `remove_reference<int&>::type` as a bound
LOCAL still has fuzzy reference semantics (the pre-existing reference-typedef-local
gap, tmp/ref3/ref4) — own slice. Further condense candidate: the 4757 builtin-trait
`*`-fold (DataDef-based) could share a DataDef-level variant of the helper.

### w2a's NEW blocker (1 error) — duplicate emission of a nested template class

`tmp/w2a_emit.c` now compiles past move_iterator and fails MIR-link with **"Repeated
item declaration …_Temporary_value___Storage___dtor"**. BOTH `vector::_Temporary_value`
AND its nested `union _Storage` are emitted TWICE (≈ lines 984 and 1470), so their
dtors are defined twice. The two `_Temporary_value::dtor` bodies even DIFFER — the
first just calls `_Storage::dtor`, the second has the real
`__alloc_traits::destroy(...)` + `_Storage::dtor` — i.e. an early/incomplete emission
AND the full one both land. This is an EMISSION-DEDUP bug for a nested class inside a
monomorphized template (NOT reference/template-arg related). NEXT SLICE: find why
`_Temporary_value` (a nested class of vector used by `_M_insert_aux`/temporaries) is
instantiated/emitted twice and dedup it (one definition per monomorphized member).
Reducer: minimize from tmp/w2a.mad (it's the only w2a error now). w2a arc: 35 → 1.

---

## STATUS UPDATE 2026-06-13 (session 7, part 13) — instantiate alias templates with a non-type param (general; w2a stays 2)

**COMMITTED `4a50ae0`, FULLY GATED.** suite 557/27 (+testconditionalt, failure list
byte-identical), unit all-pass, gcc-torture 1571 / 51-name failset byte-identical
(zero regressions), SMAUG soak green. **w2a UNCHANGED at 2** — this is a general
fix, not a w2a-clearing one (see the blocker chain below).

ROOT CAUSE: an alias template with a NON-TYPE param (`conditional_t<bool,T,F>`,
`enable_if_t<bool,T>`, …) collapsed to an opaque placeholder struct instead of
expanding — `conditional_t<true,int,long>` stayed `struct conditional_t_true_int_long`.
`instantiate_template_alias_use`'s type-params-only path (~3043) substitutes args
into the alias body and resolves; the `has_non_type_params` path (~2948) did not.
FIX: the non-type path now token-substitutes the collected args (type AND non-type,
each as its raw token sequence — `_Cond` -> the `true` token) into `td.target` and
resolves, in an ISOLATED token stream (save tokens / swap fresh deque / restore) —
NOT push/drain on the shared stream, which desynced the suspended outer template
parse and SIGSEGV'd (the reverted first attempt). Falls through to the placeholder
on resolution failure (dependent use / SFINAE absent-`::type`). New test
`tests/testconditionalt.mad`. Verified `conditional<true,int,long>::type` (direct,
no alias) already worked — only the alias was broken.

### w2a's LAST 2 — RESEARCHED to a single root feature (2026-06-13)

**FULL RESEARCH + LEAN DESIGN: `docs/plans/2026-06-13-reference-template-args-research.md`
(read it first).** Both errors reduce to ONE missing feature: **a reference-qualified
TYPE as a template argument** (`Tmpl<int&>`/`Tmpl<T&&>`). Everything else the
move_iterator `__conditional_t` needs already works (member alias templates, the
internal __conditional_t form, traits, `__base_ref`, reference RETURN types — all
verified with reducers tmp/mat1,ic1,mi3,ref1,ref2). The parse gap is duplicated
`*`-only declarator folds across ~4 template-arg loops; LEAN fix = extract ONE
`fold_template_type_arg_suffix` helper (collapsing per clang BuildReferenceType) and
replace the copy-pasted folds (net: less code). Reference-typedef-LOCAL binding
(tmp/ref3,ref4) is a SEPARATE pre-existing gap, NOT needed for w2a. Clang frontend
source checked out at /workspace/llvm-clang-src (recon only). Older chain notes below
are superseded by the research doc.

### (superseded) earlier blocker-chain notes — move_iterator conditional_t reference type

Both remaining errors (`tmp/w2a_emit.c:~1639` conversion-to-non-scalar,
`:~1699` struct-return) are `move_iterator<__normal_iterator<int*>>::operator*`
returning `reference = conditional_t<is_reference<__base_ref>::value,
remove_reference_t<__base_ref>::type&&, __base_ref>` (= `int&&`), left an opaque
struct. Reducer **`tmp/mi2.mad`** (make_move_iterator + `*mi`) reproduces it
minimally. The chain, INNERMOST-first (each a prerequisite for the next):

1. **Reference-qualified TYPE as a template argument** (`conditional<b, int&, long>`,
   `conditional<b, remove_reference_t<R>&&, R>`). Reducers `tmp/rt1.mad`
   (`int&&`), `tmp/rt2.mad` (`int&`). The explicit-arg template-id parser
   (parser.cpp ~2646) folds only `*` (pointer); the DEFAULT-arg path (~2715-2743)
   ALREADY folds `&`/`&&` via getReferenceType. WIP `tmp/reftemplatearg_wip.patch`
   mirrors that fold into the explicit-arg path → rt1/rt2 PARSE. **But it's
   parse-only: the resulting `int&` local has BROKEN reference SEMANTICS** (modeled
   as a raw pointer; `r = n` assigns the pointer, doesn't bind — silent
   miscompile). REVERTED: a loud parse error became silent wrong code, and it
   didn't clear w2a. The real fix must also give a template-arg-derived reference
   type correct binding/return semantics — a reference-semantics slice, not just a
   parse fold.
2. **`__base_ref` member-alias resolution INSIDE the isolated conditional_t
   resolve.** Even with (1) parsing, w2a's conditional_t args
   (`is_reference<__base_ref>::value`, `remove_reference<__base_ref>::type&&`)
   name `__base_ref` — move_iterator's `using __base_ref =
   iterator_traits<It>::reference`. In the part-13 isolated resolve, `__base_ref`
   does NOT resolve to `int&` (the isolated token stream + only conditional_t's
   own owner pushed; the move_iterator instantiation scope/alias isn't reachable
   there) → resolve fails → placeholder. Simplified reducer `tmp/mi3.mad` (member
   alias `R=T&` + conditional_t, NO rvalue-ref branch) WORKS, so the gap is
   specifically (1)+(2) together in the real shape (`tmp/mi4.mad` is the bridge
   reducer; it also needs `#include <utility>` for std::move).

NEXT-SESSION PLAN: this is a genuine multi-piece slice (reference-as-template-arg
WITH correct reference semantics + member-alias resolution in the alias-instantiation
context). Do (1)-with-real-semantics first (reducer rt1/rt2 + a binding test), then
(2). Until then w2a sits at 2 and the move_iterator emit carries caught-but-printed
"Expecting ',' or '>' in type<...>" stderr noise (wall-7 class: a CAUGHT isolated-resolve
exception that Throw prints unconditionally; exit code unaffected). WIP patches:
`tmp/conditional_t_alias_wip.patch` (superseded by 4a50ae0), `tmp/reftemplatearg_wip.patch`.

---

## STATUS UPDATE 2026-06-13 (session 7, part 12) — scope a non-compound if-branch's materialized temps (w2a 4→2)

**COMMITTED `8ba77dc`, FULLY GATED.** suite 556/27 byte-identical, unit all-pass,
gcc-torture 1571 / 51-name failset byte-identical (zero regressions/swaps), SMAUG
soak green. w2a **4 → 2** — BOTH `__madc_objtmp_75` errors cleared (the session-4
condition-temp gap, branch-statement variant).

ROOT CAUSE: a then/else branch that is a single (NON-compound) statement
materializing a temporary leaked that temp past the branch. `translate_stmt_required`
does not flush `m_pending_stmts` (a compound branch self-flushes inside
`translate_block`; a bare statement has no scope), so the temp's decl was swept
into the NEXT translated block's flush — landing, undeclared at its use, inside the
ELSE block. Real `vector::_M_move_assign(false_type)`:
`if (alloc==alloc) _M_move_assign(std::move(__x), true_type()); else {...}` — the
`true_type()` temp decl landed in the else block.
FIX: new `CirBuilder::translate_branch_stmt` wraps a controlled statement + any
temps it materialized into ONE block (temps declared before the stmt, cleaned up at
branch exit), only when `m_pending_stmts` is non-empty (compound / temp-free
branches unchanged). `translate_if` uses it for then/else in both arms. The
branch-statement analogue of the session-4 condition-temp flush. **Loop bodies are
the same class — left for when one actually manifests** (the helper is ready).

### w2a's LAST 2 — the move_iterator `conditional_t` reference-type cluster (deep; was "niche")

Both remaining errors are ONE cluster: `move_iterator<It>::operator*` returns
`reference = __conditional_t<is_reference<__base_ref>::value,
remove_reference_t<__base_ref>::type&&, __base_ref>` which for `vector<int>` is
`int&&`. madc left it an OPAQUE struct type, so:
- `tmp/w2a_emit.c:~1627` `conversion to non-scalar type requested` — `operator*`
  C-casts `(*operator*(...))` to `(struct __conditional_t_...)` (can't cast to a
  struct in C).
- `tmp/w2a_emit.c:~1687` `incompatible return-expr type in function returning a
  struct/union` — same `operator*`, the `std::move(*__x)` return-expr vs the
  conditional_t struct return type.

**INVESTIGATION DONE THIS SESSION (root cause located, fix attempted + REVERTED):**
The foundational gap is alias-template instantiation with a NON-TYPE param.
`instantiate_template_alias_use` (parser.cpp ~2936) has TWO paths: the
type-params-only path (~3043) substitutes args into the alias body `td.target` and
resolves; the `has_non_type_params` path (~2948) does NOT — after a
`__detected_or_t` special case it collapses straight to an OPAQUE placeholder
struct (~3031). So `conditional_t<bool,T,F>` / `enable_if_t<bool,T>` never expand.
Verified with reducers (tmp/ct1..ct5):
- `conditional<true,int,long>::type` (DIRECT, no alias) resolves fine — partial-spec
  selection WORKS (ct4=int, ct5=long).
- `conditional_t<true,int,long>` (the ALIAS) → opaque `struct conditional_t_true_int_long`
  (ct1, the bug).
- `is_reference<int&>::value` in EXPRESSION position → PARSE error "Expecting ',' or
  '>'" (ct2) — a SEPARATE gap: a reference template arg `int&` in expression context.
  (As a conditional_t template ARG it parses fine — ct3.)

A WIP fix (token-substitute args into `td.target` + resolve in the non-type path,
fall through to placeholder on NULL) made ct1/ct3/ct4/ct5 PASS but **CRASHED on the
real w2a header** — token-stream desync + SIGSEGV deep in re-entrant template-alias
instantiation (backtrace through instantiate_free_operator_template →
DeferredFunctionBody copy ctor). REVERTED to keep the tree stable; WIP saved at
**`tmp/conditional_t_alias_wip.patch`**. NEXT-SESSION PLAN: the simple-case fix is
correct in shape but not re-entrancy-safe for the real header — the body resolution
recurses into more alias/template instantiations that interact with the pushed-token
sentinel. Make the substitution re-entrant (resolve the substituted body in an
ISOLATED token context, like the SFINAE pre-check `instantiate_fn_template_binding`
sandbox at parser.cpp ~1b91e9f, rather than push/drain on the shared `tokens`
stream), and SFINAE-guard `enable_if_t` (absent `::type` must fail softly). This is a
genuine multi-step slice (alias-template non-type instantiation + re-entrancy), not a
one-liner — the handoff was right to mark it lowest-priority; budget it as its own
session.

---

## STATUS UPDATE 2026-06-13 (session 7, part 11) — call a parenthesized function designator `(E)(args)` (w2a 5→4)

**COMMITTED `e048e9e`, FULLY GATED.** suite 556/27 byte-identical, unit all-pass,
gcc-torture 1571 / 51-name failset byte-identical (zero regressions/swaps), SMAUG
soak green. w2a **5 → 4** — the std::max `lvalue-required-as-assign-LHS` cleared.

ROOT CAUSE (general, NOT std::max-specific): madc had no path to CALL a
PARENTHESIZED function DESIGNATOR. libstdc++ writes `(std::max)(size(), __n)`
(parens suppress macros/ADL). A bare function name followed by `)` decays to a
function-designator `TokenVar` (function-to-pointer decay, parser ~14526). When
the next `(` was processed, only `DataDefFPTR` *variables* matched the fptr-call
branches — a `FuncDef` designator fell through to grouping, so the `(` opened a
comma-expression and the callee was orphaned (`(std::max)(a,b)` → `(a , b)`, or
the bare template name `__ns_std_max` leaked as an undefined import). Confirmed
general with `tmp/mx6.mad` (`(mymax)(a,b)`, a plain user function — same drop).

FIX: a new branch in parseExpression's `(`-handler (right after the var-fptr
branch, ~15514) forms a `TokenCallFunc` when exStack top is a function-typed
`TokenVar` AND the just-consumed token is the `)` that closed the grouping.
That pair — `prevToken()==tkClBrk` + a function-typed designator + a following
`(` — uniquely identifies `(E)(args)`: a bare `func(args)` never reaches here
(the direct-call path forms the call before pushing onto exStack), and a
function name decays to its address only when followed by a value-context token
(`,`/`)`/`]`/operator), never `(`. Two deliberate choices: (a) NO
`!opstack_has_pending_op` gate — the postfix call binds tighter than any pending
binary op, so `size() + (std::max)(...)` must still form the call (the functor
branch already omits it); (b) NO `src_node` — parseCallFunc's overload re-rank /
template instantiation rebinds the call to the instantiated funcdef
(`__ns_std_max__o2`) instead of pinning the un-instantiated designator name.
Works for plain functions and namespace function templates alike.

**Session arc: w2a 35 → 4** across parts 5-11. Reducers tmp/mx1..mx6.

### NEXT SESSION — w2a's last 4, LINE-ATTRIBUTED (start here)

Workflow unchanged: `bin/madc --emit=c11 tmp/w2a.mad > tmp/w2a_emit.c` then
`/workspace/mir/c2m tmp/w2a_emit.c -ei 2>&1 | grep -v warning`. The 4 remaining
(line numbers drift ~1 per slice — re-attribute each run):

1. **`tmp/w2a_emit.c:~2173` — `__madc_objtmp_75` undeclared + struct/union arg**
   (two errors on one line, in the allocator `operator=` arg of
   `_M_move_assign`). The session-4 while/for CONDITION-TEMP placement gap: a
   materialized temp is referenced but its decl landed in the wrong scope
   (pending-stmt placement for a temp created inside a condition/sub-expression).
   Likely the highest-value next target (closes 2 of the 4 at once).
2. **`tmp/w2a_emit.c:~1627` — `conversion to non-scalar type requested`** (NEW
   face this session; was masked behind the std::max error). Attribute the
   construct (a cast/conversion to a class type emitted as a scalar cast); reduce
   independently before designing.
3. **`tmp/w2a_emit.c:~1687` — std::move / move_iterator reference return**
   `return (*__ns_std_move(&(*operator[](...))))` in move_iterator::operator*
   returning `__conditional_t<...>&`: std::move yields `T&&` and the
   by-value/ref-return shape mismatches the conditional reference type. Niche.
- Plus 2× pointer-type-param WARNINGS — verify they actually block before
  spending a slice (likely benign).
- Separate known bug (surfaced by w17/w19, NOT in w2a's 4): `a + N` array-decay
  ctor arg types as INT (pointer decay missing).

---

## STATUS UPDATE 2026-06-13 (session 6, part 10) — receiver on a materialized by-value method (w2a 8→5)

**COMMITTED `14c0c25`, FULLY GATED.** suite 556/27 byte-identical, unit all-pass,
gcc-torture 1571 / 51-name failset byte-identical (zero regressions), SMAUG soak
green. w2a **8 → 5** — too-few-args cluster **3 → 0**.

ROOT CAUSE: `object_call_temp_addr` materialized a by-value object-returning call
with the FREE-function arg shape (`&__retbuf` + explicit args, no receiver). For a
METHOD (`get_allocator()` returning allocator by value via sret) that dropped the
hidden `this`, emitting the call one arg short of its `(__retbuf, __this)` body
("too few arguments"). FIX: a by-value-returning method call now delegates to
`class_method_call` (which already materializes its own sret temp and passes the
Itanium (sret, this, args) shape with base-subobject `this` adjustment);
object_call_temp_addr addresses the resulting lvalue. Only reached for retbuf
returns (object_returning_call_class gates on class_return_via_retbuf), so
class_method_call always takes its materializing sret branch.

**Session arc: w2a 35 → 5** across parts 5-10.

### NEXT SESSION — w2a's last 5, LINE-ATTRIBUTED (start here)

Workflow: `bin/madc --emit=c11 tmp/w2a.mad > tmp/w2a_emit.c` then
`/workspace/mir/c2m tmp/w2a_emit.c -ei 2>&1 | grep -v warning`. (The emit-C path
still line-attributes; the JIT path reports all at file:2:28.) Three distinct,
somewhat deeper root causes remain — each its own slice, full gate each:

1. **`tmp/w2a_emit.c:2121` — `std::max` free-fn-template not CALLED** (in
   `_M_check_len`). `std::max(size(), __n)` mis-lowered to
   `size(__this) = (__ns_std_max + (size(__this) , __n))` — the function name
   `__ns_std_max` leaks as a bare identifier (uninstantiated/uncalled), a bogus
   `+`, a comma expr, and an assignment whose LHS is the `size()` call ("lvalue
   required as left operand of assignment"). Root: the std::max/min free-function
   template isn't being resolved+instantiated+called at this site (cf. the
   getline / std_free_function_instantiation machinery). Likely the highest-value
   next target (a real free-fn-template call gap, not just a shape).
2. **`tmp/w2a_emit.c:2162` — `__madc_objtmp_74` undeclared + struct/union param**
   (in `_M_move_assign__o2`, the allocator `operator=` arg). The session-4
   while/for CONDITION-TEMP placement gap: a materialized temp is referenced but
   its decl landed in the wrong scope (pending-stmt placement for a temp created
   inside a condition/sub-expression). Two errors on one line.
3. **`tmp/w2a_emit.c:1686` — std::move / move_iterator reference return** (the 1
   remaining struct-return). `return (*__ns_std_move(&(*operator[](&_M_current,
   __n))))` in move_iterator::operator* returning `__conditional_t<...>&`:
   std::move yields `T&&` and the by-value/ref-return shape mismatches the
   conditional reference type. Niche; lowest priority of the three.
- Plus 2× "incompatible argument type for pointer type parameter" — emitted as
  WARNINGS by c2mir (may be benign / not among the 5 hard errors; verify whether
  they actually block before spending a slice).
- Separate known bug (surfaced by w17/w19, NOT in w2a's 5): `a + N` array-decay
  ctor arg types as INT (pointer decay missing).

---

## STATUS UPDATE 2026-06-13 (session 6, part 9) — operator[] on a sub-expression receiver (w2a 11→8)

**COMMITTED `9f4ce08`, FULLY GATED.** suite 556/27 byte-identical, unit all-pass,
gcc-torture 1571 / 51-name failset byte-identical (zero regressions), SMAUG soak
green. w2a **11 → 8** — subscripted-value cluster **3 → 0**.

ROOT CAUSE: a subscript on a class-object SUB-EXPRESSION (`(*this)[n]` in
vector::at, `__this->_M_current[n]` in a move_iterator) parses as a
TokenSubscriptExpr, whose CIR lowering fell straight to a raw N_IND — c2mir
rejects it (the base is a struct, not array/pointer). The named-variable
TokenSubscript path already dispatches to operator[]; the expression-receiver
path did not. FIX: the TokenSubscriptExpr handler now dispatches to the class's
operator[] via `class_subscript_addr_on` (the shared receiver-generic core) when
base_expr is a class object declaring operator[], deref'ing a T& return; receiver
address from object_arg_addr. Falls through to the raw/VLA-flatten lowering
otherwise.

**w2a REMAINING (8 errors):** 3× too-few-args, 2× pointer-type-param, 1×
__madc_objtmp (while/for condition-temp, session-4 gap), 1× lvalue-as-assign-LHS,
1× struct/union return-expr (std::move/move_iterator). **Session arc: w2a 35 → 8**
across parts 5-9.

---

## STATUS UPDATE 2026-06-13 (session 6, part 8) — by-value class return on a method extern (w2a 15→11)

**COMMITTED `a7448fe`, FULLY GATED.** suite 556/27 byte-identical, unit all-pass,
gcc-torture 1571 / 51-name failset byte-identical (zero regressions), SMAUG soak
green. w2a **15 → 11** — struct/union return-expr cluster **5 → 1**.

ROOT CAUSE: an external mangled-direct libstdc++ method returning a trivially-
copyable class BY VALUE (register-returned, no retbuf — `vector::_M_erase` /
`_M_insert_rval` return `__normal_iterator`) had its extern return type fall
through `emit_symbol_ret_specs` to a VOID base (a class is neither pointer/ref nor
integer). `return <call>(...)` in a struct-returning caller then mismatched. FIX:
`need_output_extern` grew an optional `ret_cls` — when set it declares the return
as the class's struct/union tag via `class_tag_ref` (the RETURN mirror of the
part-4 `ExternParam::cls` by-value-PARAM fix), taking precedence over
ret_specs/ret_ptr. `emit_symbol_method_call` passes it for a by-value (non-ref,
non-pointer) class return that is NOT retbuf-returned. The remaining 1
struct-return is the std::move/move_iterator rvalue-ref-return shape (distinct).

**w2a REMAINING (11 errors):** 3× too-few-args, 3× subscripted-value-not-array
(operator[] on object), 2× pointer-type-param, 1× __madc_objtmp (while/for
condition-temp, session-4 gap), 1× lvalue-as-assign-LHS, 1× incompatible
struct/union return-expr (std::move/move_iterator). **Session arc: w2a 35 → 11**
across parts 5-8 (operator member-vs-free, prvalue ref-args, method receiver,
by-value class return).

---

## STATUS UPDATE 2026-06-13 (session 6, part 7) — by-value-call method receiver (w2a 17→15; lvalue-& cluster CLOSED)

**COMMITTED `1ff43b5`, FULLY GATED.** suite 556/27 byte-identical, unit all-pass,
gcc-torture 1571 / 51-name failset byte-identical (zero regressions), SMAUG soak
green. w2a **17 → 15** — the last 2 "lvalue required as unary &" cleared (cluster
now 0; parts 6+7 together cleared all 6).

ROOT CAUSE: a method on a by-value object-returning call (`*begin()` lowered as
`begin().operator*()`) computed `this` as `&translate_expr(begin())`; begin()
returns the iterator by value (a prvalue, REGISTER-returned for a trivially-
copyable iterator so `object_returning_call_class` returns NULL — no retbuf),
making `&call` invalid. FIX: `class_this_arg` routes a by-value object-returning
CALL receiver through `object_arg_addr` (which materializes BOTH the retbuf case
via object_call_temp_addr AND the trivially-copyable register case via a copy into
a temp). Gated to the CALL case (ttCallFunc/ttCallMethod, non-ref return), detected
BEFORE translate_expr (emit the call once). An lvalue receiver keeps its direct
&obj — routing ALL value receivers through object_arg_addr would copy a
non-matching lvalue into a temp and call the method on the COPY (mutation bug).

**w2a REMAINING (15 errors):** 5× incompatible struct/union return-expr, 3× too-
few-args, 3× subscripted-value-not-array (operator[] on object), 2× pointer-type-
param, 1× __madc_objtmp (while/for condition-temp, session-4 gap), 1×
lvalue-as-assign-LHS, 1× struct/union-param.

---

## STATUS UPDATE 2026-06-13 (session 6, part 6) — prvalue ref-arg materialization (w2a 21→17)

**COMMITTED `839a3de`, FULLY GATED.** suite 556/27 byte-identical, unit all-pass,
gcc-torture 1571 / 51-name failset byte-identical (zero regressions), SMAUG soak
green. w2a **21 → 17** — 4 of the 6 "lvalue required as unary & operand" cleared.

ROOT CAUSE: a non-class reference param (`const T&`, T scalar/pointer) was lowered
as `&translate_expr(arg)` unconditionally; for a prvalue arg (`__normal_iterator(
_M_current++)` postfix++, or a by-value-call result) `&(prvalue)` is rejected.
FIX: new `CirBuilder::ref_param_arg_addr` materializes the prvalue into a temp and
passes its address. Fires ONLY for unambiguous prvalues
(`expr_is_nonaddressable_rvalue`: by-value call, postfix ++/--, builtin binary
arith/bitwise, literal) — forms `&expr` already rejected, so lvalue args keep the
direct-address path and nothing previously-compiling changes (the safety
principle: opt-IN materialization of already-broken forms). Applied at all 9
non-class ref-param address-of sites (do NOT touch the `node1(N_ADDR,
translate_expr(arg), arg)` occurrences INSIDE object_arg_addr / ref_param_arg_addr
itself — line ~1245 is the helper's own fallback, replacing it self-recurses).

**w2a REMAINING (17 errors):** 5× incompatible struct/union return-expr, 3× too-
few-args, 3× subscripted-value-not-array (operator[] on object), 2× lvalue-& (the
METHOD-THIS-on-rvalue case — `front()`/`back()` = `&begin(...)` as the operator*
this-pointer; a DIFFERENT site than the ref-arg path, needs this-arg rvalue
materialization), 2× pointer-type-param, 1× __madc_objtmp (while/for condition-temp),
1× lvalue-as-assign-LHS, 1× struct/union-param.

---

## STATUS UPDATE 2026-06-13 (session 6, part 5) — operator member-vs-free FIXED (w2a 35→21)

**COMMITTED `0ae2a07`, FULLY GATED.** suite 556/27 (failure list byte-identical),
unit all-pass, gcc-torture 1571 / 51-name failset byte-identical (ZERO regressions/
swaps), SMAUG soak green (exit 124 + `Realms of Despair ready ... port 4000`). w2a
check errors **35 → 21** — all 10 operator-overload-selection errors cleared.

ROOT CAUSE (the part-4 NEXT-SLICE, now solved differently than scoped): `iter - iter`
on `__gnu_cxx::__normal_iterator` (member ONLY has `operator-(difference_type)`)
wrongly bound the arithmetic member. The C++-correct binding is the free cross-type
template `__gnu_cxx::operator-(const It&, const It&)` — and it is an **inline template,
NOT an exported symbol**, so the part-4 plan's "mangled-direct" framing was WRONG: it
must be BODY-INSTANTIATED via the existing `lower_free_operator_to_call` →
`instantiate_free_operator_template` path (the same machinery that handles `a+"lit"`).

THREE coupled, narrowly-scoped fixes (`src/parser.cpp` + `include/datadef.h`):
1. **`lower_free_operator_to_call` member guard** — the name-only "member owns it" bail
   now yields ONLY when: rhs is a class object AND every same-name binary member takes a
   non-class param (new `DataDefCLASS::binary_operator_only_takes_nonclass`) AND NO
   reference-returning free overload exists for the op AND a retained free-operator
   template actually binds. Else it bails exactly as before.
2. **`resolve_canonical_type_spelling`** — resolve POINTER template args (`int*`,
   `const int*`, `T**`); cv dropped for base resolution (madc tracks const only in a
   type's identity, like `instantiate_template_id`); fails safe.
3. **`free_binary_operator_return_class`** — reject dependent/nested-member returns
   (`__normal_iterator<...>::difference_type`) and deduced returns (`auto`/`decltype`):
   they don't name an operand class by value (else the same-type free `operator-`
   wrongly claimed the expr, blocking lowering).

**HARD-WON LESSON (regression bisect, ~4 full-suite runs):** the FIRST instinct —
broadly relaxing the member guard to "viable member owns it" via `findMethodOverload`
— regressed **32 tests** (string/struct/class/stream). Cause: `ostream << string` ALSO
fits "member takes non-class param + class rhs" (member `operator<<` takes scalars), but
its correct binding is the EXPORTED reference-returning W2 `operator<<` done by the cir
builder — NOT a body instantiation. The ref-return guard (#1's "NO reference-returning
free overload") is what distinguishes the two. Changes #2/#3 are independently safe
(556/27 each); #1 is the one that needed the narrowing. Reducers: tmp/w17 (iter-const_iter),
tmp/w18 (iter-iter), tmp/w19 (real libstdc++ iterators).

**w2a REMAINING (21 errors, next slices):** 6× lvalue-required-&, 5× incompatible
struct/union return-expr, 3× too-few-args, 3× subscripted-value-not-array, ~comparison,
1× `__madc_objtmp` (while/for condition-temp placement, session-4 gap). Separate known
bug surfaced by w17/w19: `a + N` array-decay arg to a ctor types as INT (pointer decay
missing) — blocked the first w17 form. 3-way oracle usable via emit-C `safe_ident`.

---

## STATUS UPDATE 2026-06-13 (session 6, part 4) — w2a: struct-by-value extern params (35 errors)

**COMMITTED `bce0b07`.** Suite 556/27 (failure list byte-identical) + unit 10/10 GREEN;
torture+SMAUG running (tmp/gcctest_s7.log) — the fix only changes emitted externs for
by-value class params (a real-libstdc++-container category absent from the C torture
suite), so regression is unlikely; VERIFY the failset still = 51-name baseline + SMAUG
soak before treating bce0b07 as fully gated. w2a check errors **42 → 35**.

### ~~NEXT SLICE~~ DONE in part 5 (`0ae2a07`) — kept for the recon detail; superseded by the part-5 banner above
**Operator overload member-vs-free mis-selection** (the remaining 10 arithmetic-arg
errors). REDUCER `tmp/w16.mad` (g++ → `3`; madc → "incompatible argument type for
arithmetic type parameter"). Shape: a class with BOTH a member `operator-(long)` (the
arithmetic/difference_type overload) AND a free/friend `operator-(const It&, const
CIt&)`; calling `x - y` where y is a DIFFERENT type (real: `__position - cbegin()`,
iterator − const_iterator) must pick the free operator, but madc binds the member and
jams the struct into the `long` param.
ROOT CAUSE: `select_operator_overload` (cir_builder.cpp) — its final KEYED-LOOKUP
fallback `Variable *mv = cls->findMethod(key); return ...` returns the first by-name
member IGNORING arg viability, even when scoring already rejected every member
(`best == NULL` because `operator-(long)` scored <0 against the class rhs). The member
then gets the struct rhs jammed into its `long` param.

**NEGATIVE RESULT (tested 2026-06-13, REVERTED — do NOT re-attempt the one-liner):**
making the keyed fallback `if (rhs_dd) return NULL;` (reject non-viable member, expecting
`class_operator_call` to fall through to `try_free_operator_call`) made things WORSE —
w2a **35 → 39** errors, and w16 turned into "invalid operand types of -". Reason:
`try_free_operator_call` does NOT actually bind the cross-type `operator-` for these
cases (its general binary path doesn't match `iterator − const_iterator`; only the
`operator<<`/`a+"literal"` shapes are handled). So rejecting the member just converts a
wrong-but-compiling bind into a hard "no operator" failure. **The REAL fix is bigger:**
extend `try_free_operator_call`'s general binary path to match a captured namespace
operator TEMPLATE (`__gnu_cxx::operator-`) against TWO differently-typed class args
(deduce both iterator params), bind it mangled-direct, AND THEN reject the non-viable
member. Design the free-operator-template binary matching FIRST; the keyed-fallback
viability guard is only safe once the free path provably binds. w16's file-scope
NON-template friend is a separate, lower-priority gap (ordinary free-function operator
lookup, not W2-captured). RISK: broad overload-resolution change — full torture gate
mandatory.

**RECON DONE 2026-06-13 (real libstdc++ `/usr/include/c++/13/bits/stl_iterator.h`):**
The two `operator-` forms overload resolution chooses between for `__position −
cbegin()` (iterator − const_iterator):
- MEMBER `:1157` — `__normal_iterator operator-(difference_type __n) const`; param is
  arithmetic (`ptrdiff_t`). NON-viable for a const_iterator arg (no conversion to
  ptrdiff_t) → madc's scorer correctly returns `best==NULL`.
- FREE cross-type TEMPLATE `:1321` (namespace `__gnu_cxx`):
  `operator-(const __normal_iterator<_IteratorL,_C>&, const __normal_iterator<_IteratorR,_C>&)`
  → `decltype(__lhs.base() - __rhs.base())` (= difference_type). TWO DISTINCT iterator
  template params, SHARED `_Container`. This is the C++-correct selection.
DESIGN for the fix (harder than the existing W2 `operator<<` path, which deduces ONE
param from the RHS with the stream as the matched class): the binary path must deduce
BOTH operands as template-ids of the SAME primary template (`__normal_iterator`) with a
consistency constraint on the shared `_Container`, then mangle/bind. IMPLEMENTATION
STEP 0 — **DONE/VERIFIED 2026-06-13**: `capture_free_operator_overload` (parser.cpp
~24762) captures ANY namespace-scope binary operator (`param_spellings.size() >= 2`),
NOT gated to `<<`/`>>` — so `__gnu_cxx::operator-` IS already in
`free_operator_overloads`. **The fix is MATCHING-ONLY** (no capture extension): extend
`try_free_operator_call`'s general binary section (cir_builder.cpp, after the
`operator<<` manipulator special-case) to, for a binary op whose LHS is a class and
whose member overload is non-viable, scan `free_operator_overloads` for a same-`opname`
2-param entry and deduce BOTH operands as template-ids of the SAME primary template with
a shared trailing param (`_Container`) — model it on the existing `deduce_free_stream_call`
/ `targs_from_binding` / `itanium_mangle_std_free_template` machinery (which already does
ONE-param deduction for `operator<<`). Then the keyed-fallback viability guard becomes
safe. The recon pointer + method is in memory `feedback_research_and_gcc_recon`
(/workspace/gcc + installed gcc-13 headers).
After it: 6× lvalue-&, 5× struct/union return-expr, 3× too-few-args, 3× subscript,
3× comparison, 1× `__madc_objtmp_66` (while/for condition-temp placement, session-4 gap). The 3-way oracle is now usable (emit-C `safe_ident` from part 3), so
errors are line-attributed via `/workspace/mir/c2m tmp/w2a_s7b.c -ei`.

ROOT CAUSE fixed: a by-VALUE class/struct PARAM to a mangled-direct libstdc++ method
(real `vector::_M_fill_insert(__normal_iterator __position, ...)`) was declared
ARITHMETIC in the emitted extern prototype — `native_param_shape` fell through
`param_object_class` (ref-only) to `native_scalar_specs` → `{N_LONG}`, while the call
passed the struct value (`object_arg_value`). c2mir: "incompatible argument type for
arithmetic type parameter". Fix: `ExternParam` grew a `DataDefCLASS *cls` field (NO
default member initializer — keeps it a C++11 aggregate so `{specs, ptr}` inits still
work, `cls` value-inits null); `native_param_shape` returns `{ {}, false, vc }` for a
by-value class; `need_output_extern` renders `cls` via `class_tag_ref` (struct/union
tag, no pointer). The arithmetic-arg family dropped 17 → 10.

**w2a REMAINING (35 errors, next slices, each a DISTINCT root cause):**
- **10× arithmetic-arg, now OPERATOR OVERLOAD SELECTION** (NOT the extern shape):
  `__position - cbegin()` bound `__normal_iterator::operator-(difference_type)` (the
  arithmetic-param overload) instead of `operator-(const __normal_iterator&)` — wrong
  overload picked when the arg is an iterator struct. Line 1946 etc. **Best next target.**
- 6× lvalue-required-&, 5× incompatible struct/union return-expr, 3× too-few-args,
  3× subscripted-value-not-array, 3× invalid comparison operands, 1×
  `__madc_objtmp_66` undeclared (pending-stmt placement — the while/for condition-temp
  gap from session 4), 1× lvalue-required-as-assign-LHS, 1× struct/union-param.

## STATUS UPDATE 2026-06-13 (session 6, part 3) — MIR upstream sweep (PRs #437-440)

**Commits `56ee053` (MIR pin 545ad46→5df536f) + `eeed70a` (emit-C hygiene).**
Adopted 4 upstream vnmakarov/mir PRs onto fork develop (pushed) + one madc
refinement. Headline: **#438 fixed a LIVE x86-64 generator bug** — a struct
param before `...` made `va_arg` read the named struct's register-save slot
as a vararg (3-way oracle: `MIR_gen` gave 60/3.0/3018 vs gcc 330/3.8/3018).
Pinned by `tests/testvastruct.mad`; also fixed gcc-torture `pr117432.c`
→ **failset 52→51, ZERO regressions**. #437's 128MB code-holder reservation
**arch-gated OFF x86-64** (fork `5df536f`, proposed upstream): rel32 reaches
±2GB so it's unneeded there, and it OOM-crashed the leaky-VLA torture case
`20040811-1.c` by eating commit headroom (that VLA leak is a NEW known gap —
see claude_status: VLA not freed on backward goto). #439 C23 paramless
variadic; #440 block-arg copy (aarch64/riscv64/s390x/ppc64, inert on x86-64).
emit-C `eeed70a`: `safe_ident` (per-byte mnemonic flatten of operator
spellings) + DOTS-only param list → `()`, unlocking the 3-way c2m oracle for
the w2a faces (the JIT tree path is untouched — c2mir never sees emitted C).
Gates: fulltest 556/27 (failure list byte-identical, +testvastruct), unit
10/10, torture 1571/failset 51, cir-fidelity exit 0, SMAUG soak green.
Triage: docs/parity/mir-fork-community-patches.md (round 3).

## STATUS UPDATE 2026-06-13 (session 6, part 2) — w2a CIR FACES: 83 → 42 CHECK ERRORS

**Commits `dd27c5e` → `ba6dd30` → `87cc363`** (after the eval fix below).
All three gated identically: suite 555/27 failure-list byte-identical,
unit 10/10 binaries, torture failset = the 52-name baseline (1570
passed), SMAUG soak green. `vector<int> v;` (tmp/w2a.mad) went from 7
ctor no-match errors + 83 c2mir check errors to **42 check errors, all
7 no-matches cleared**. Root causes, all general mechanisms:

1. `dd27c5e` — **implicit copy ctor** ([class.copy.ctor]) in BOTH
   ctor-call builders via shared try_implicit_copy_construct, gated on
   new recursive class_trivially_copyable (class_needs_dtor was the
   wrong predicate — any object member counted as non-trivial, but
   move_iterator{__normal_iterator{int*}} is bit-copyable); arg typing
   via the promoted CirBuilder::ctor_arg_datadef (the ONE resolver).
   **Delegating ctors** ([class.base.init]p6): a mem-init naming the
   ctor's own class is delegation, never a base initializer (the alias
   clause matched `: vector(__rv,__m,true_type{})` against _Vector_base
   and fired 3 args at 2-param base ctors); the prologue is ONLY the
   delegation call. **Identity-return inference restricted**: the
   backward scan skipped non-identifier tokens, so make_move_iterator's
   `move_iterator<_Iterator>` return matched as bare `_Iterator` —
   return_override became the ARG type, __uninitialized_copy_a deduced
   wrong and silently REUSED the plain-copy instantiation. Now fires
   only when the return IS the bare param (std::move/forward keep it).
2. `ba6dd30` — **one aggregate-tag-kind owner** (class_tag_ref):
   ~20 sites hardcoded N_STRUCT while branching sites followed
   union_layout → class-parsed unions (_Temporary_value::_Storage)
   emitted mixed kinds ("kind of tag unmatched"); class_struct_def's
   DEFINITION now branches too. **ttVariable discriminates operator
   receivers** (prefix/postfix/binary): TokenMember/TokenCallFunc
   DERIVE from TokenVar; the downcast emitted an implicit-this member's
   bare name (`--current` → "undeclared identifier current/_M_current",
   16 errors). Reducer tmp/w12a.mad (plain user class — general bug).
3. `87cc363` — **type()-gated object classification**
   (is_class_object_value + object_arg_addr's member/var arms:
   TokenCallMethod passed the TokenMember downcast) and
   **trivially-copyable rvalue-call receivers materialize**: raw-call
   rvalues (`__y.base() - __x.base()`) fall to object_arg_addr's
   materializing tail (implicit-copy assign into a temp). The gate MUST
   be class_trivially_copyable: external sret calls already yield a
   temp lvalue inside translate_expr — routing them into the tail
   recursed object_arg_addr→class_ctor_call through the copy ctor's
   const-ref param (teststringplus SEGFAULT, caught by the suite gate).

**SYSTEMIC TRAP (watch for more):** the token hierarchy
TokenCallMethod : TokenMember : TokenCallFunc : TokenVar means EVERY
`dynamic_cast<TokenVar*>`/`<TokenMember*>` classification site is
suspect — gate on type()==ttVariable/ttMember. Remaining un-audited
downcast sites in cir_builder.cpp: ~566(fixed)/1104(fixed)/3299/3313/
4662/5276/5718/7684/8111/8218/9260.

**w2a REMAINING (42 check errors, next session's entry):**
17× "incompatible argument type for arithmetic type parameter" (biggest
— start here), 11× int-without-cast-for-pointer warnings, 6× lvalue-&,
5× "incompatible return-expr type in function returning struct/union",
3× too-few-arguments, 3× subscripted-value-not-array, 3× invalid
comparison operands, 1× "undeclared identifier __madc_objtmp_66"
(pending-stmt placement — likely the while/for condition-temp gap noted
in session 4). Reducers: tmp/w12a.mad, tmp/w12b.mad (both g++-matched
green), tmp/w12c.mad free. KNOWN SEPARATE BUG found en route: `N n2(arr
+ 3);` — array+int as a ctor arg types as INT (decay missing in that
position); sidestepped in w12b, unfixed.

---

## STATUS UPDATE 2026-06-13 (session 6) — EVAL SCOPE CAPTURE FIXED; UNIT SUITE FULLY GREEN

**Commit `83c0ba4`** (this branch). Queue item (a) closed: runtime-eval
scope capture no longer sweeps parse-time constants. Root cause was in
`is_runtime_eval_scope_supported_variable` (parser.cpp ~9381), NOT the
CIR lowering: bare-`vfCONSTANT` globals (glibc anonymous-enum constants
`_ISupper` et al., `PTHREAD_*`) have no declaration in the emitted
module — reads of them FOLD — so the TokenScopeContext by-name capture
emitted undeclared identifiers and the PARENT TU failed to compile at
every scope-access eval call site. Fix: the collector's predicate
excludes `vfCONSTANT`-without-`vfCONSTDECL` (a value, not runtime scope
state); const-DECLARED vars (real storage) stay capturable.

**Gates:** unit `test_libmadc_program` **132/0/11** (was 128/4; only
deferred-AOT skips remain — unit phase fully green again). Integration
**555/27** (+3: testmadcevalexpr/testmadcevalexprtyped/testmadcevalscope,
zero new; log tmp/runtests_s6a.log). Torture failset **byte-identical to
the 52-name baseline** (1570 passed, tmp/gcctest_s6.log). SMAUG soak
green (exit 124 + ready line).

**Eval cluster re-attribution:** the 3 still-failing eval tests are NOT
scope-capture: testmadceval + testmadcevalexprctx die on wall 5
(`_ZNSolsESo` — `cout << endl` overload mis-pick, same as
testmultiret/testrust); testmadc_ns dies on wall 2 (`std::vector<int>`
instantiation). Wall 3 as a distinct wall is CLOSED.

**Queue item (c) verified done:** no `cc_*.json` scratch manifests remain
in /workspace/MadSMAUG.

**REMAINING QUEUE:** (b) w2a CIR faces per the session-4 banner (implicit
copy ctor `__normal_iterator(__normal_iterator)` first, then the
`_Vector_base` 3-arg mem-init mis-route) — unblocks the ~12-test
container cluster + testmadc_ns; then wall 5 (`_ZNSolsESo`, +4 tests).

---

## STATUS UPDATE 2026-06-12/13 (session 5) — WALL 4 CLOSED; SMAUG BOOTS ON REAL GLIBC

**Commits `63e3efb` → `3b460ea` → `ea3078a`** (this branch). All three
individually gated, zero regressions; suite improved to **552/30** (+3:
testservent/teststat/teststatret), torture failset **byte-identical to the
52-name pre-drift baseline** (wall 4's 20101011-1/loop-2f/loop-2g recovered),
**SMAUG 1.8 boots end-to-end on REAL glibc and survives the soak** (the
canonical `MadSMAUG.sh` invocation; exit 124 + ready line).

1. `63e3efb` **STEP 0 namespace-stack refactor** (as planned): vector +
   RAII `NamespaceScope`; `current_namespace()` is a read accessor; the
   qualified-stmt flag pair is replaced by `stmt_callee_namespace` +
   `QualifiedCalleeScope` (head-resolution override via
   `active_cpp_lookup_namespace()`; parseCallFunc/Method clear the spent
   override — args read the untouched lexical stack).
2. `3b460ea` **unit-suite SEGFAULT root-caused + fixed**: Pass-1.9
   fixpoint-materialized bodies were defined module-tail UNDECLARED
   (both declaration passes had already run) → implicit-int K&R calls,
   truncated pointer returns, mis-wired struct args (the __madc_shim
   wild store; alone-pass/full-crash = stale-stack dependence). New Pass
   1.95 emits late protos + late externs. ALSO: the eval policy gate now
   keys on the tokenized SOURCE FILE (real header paths broke the old
   policy-header-name exclude). Unit suite: crash → **128/4**.
3. `ea3078a` **wall 4, the whole chain** (each a real-glibc construct the
   shims had hidden): FF/VT = whitespace · fn-ptr members in
   nested/anonymous aggregates (shared parse_fnptr_member_tail) · arity
   checks via Program::call_signature_funcdef (blind (FuncDef*) casts on
   DataDefFPTR were UB — order/cwd-shapeshifting failures) · multi-star
   returns (dd_peel_pointers ×3 emit sites) · C++-only predefines gated
   out of C modes (predefine_is_cpp_only) · **GNU dialects**
   (--std=gnu89..gnu17, gnu++NN; gnu_dialect modifier) with gcc-parity
   strictness (__STRICT_ANSI__ strict-only, __STDC_VERSION__ per C std) ·
   project driver no-std .c → gnu17.

**TRAP LEARNED (cost a 222-failure suite run, caught by the gate,
uncommitted):** lifting __STRICT_ANSI__ from STD_MADC/C++ modes opens
glibc's `!__STRICT_ANSI__` float regions → `__float128`/`_FloatN`
declarations madc cannot type. The strictness lift is C-gnu-modes-only
until __float128/_FloatN land (noted in strict_ansi_mode()'s comment).

**REMAINING QUEUE:** (a) eval scope capture sweeps real-header constants
(`_ISupper` enum constants, interference-size constexprs) into
TokenScopeContext — emits identifiers that don't exist in C; the 4
remaining test_libmadc_program failures (walls 3-adjacent; root-cause
located in collect_runtime_eval_scope_variables/its CIR lowering at
cir_builder ~8482). (b) w2a CIR faces per the session-4 banner below
(implicit copy ctor first). (c) `cc_skfirst.json`/`cc_bis*.json` scratch
manifests in /workspace/MadSMAUG — delete when done.

---

## SESSION-5 ENTRY PLAN (decided with user 2026-06-12, end of session 4)

**STEP 0 — NAMESPACE-STACK REFACTOR (do FIRST, fresh context):** replace
the single mutable `Program::current_namespace` string (135 refs, ~10
hand-rolled save/restore sites, some not exception-safe) with
`std::vector<std::string> namespace_stack` + an RAII guard
(`NamespaceScope`), the idiomatic twin of `class_scope_stack` (vector,
NOT deque — back-ops only, tiny depth, needs iteration; std::stack
forbids the enclosing-chain walk). `current_namespace` becomes a read
ACCESSOR (back() or empty) so the ~125 read sites don't change; the ~10
mutation sites become guards. DELETE the `qualified_stmt_callee_ns` /
`qualified_stmt_lexical_ns` flag pair: the statement-level qualified-call
(`php::foo(args)`) callee-namespace override moves OUT of lexical-scope
state (a parameter to head resolution / its own member), and argument
parsing just reads the lexical stack top. WHY FIRST: walls 3 (eval-TU ns
context) + 4 (sys headers) are namespace-adjacent — do not stack more
save/restore patches; the conflation (lexical scope vs qualification
override) caused both the 2023 clear() hack and session-4's bug. GATE:
full suite (549/33 byte-identical) + torture name-diff vs the 52-name
baseline (+ the 3 known wall-4 names), committed alone before any other
work.

**THEN session-4's queue:** w2a CIR faces (implicit copy ctor
`__normal_iterator(__normal_iterator)` — [class.copy.ctor] same-class
arg + no user copy ctor → implicit memberwise/bit-copy in
select_ctor_overload's no-match tail; then the
`_Vector_base(__normal_iterator, allocator*, integral_constant_bool_true)`
3-arg mis-route — read the MADC_DEBUG_CTORINIT NO-MATCH dumps) → walls
3/4/5 per the session-4 banner below.

---

## STATUS UPDATE 2026-06-12 (session 4) — w2c GREEN; w2a PARSES fully; 13 root causes

**Commits `c8870aa` → `03d5990` → `a6c9d72`** (this branch). `tmp/w2c.mad`
(`_Vector_base<int,allocator<int>> b;`) constructs + destructs END-TO-END
(exit 0). `tmp/w2a.mad` (`vector<int> v;`) now PARSES the complete real
`<vector>` chain and stops at CIR overload selection. Suite re-verified
TWICE at this state: **549/33/0/18, failset byte-identical to the
session-3 baseline** (tmp/runtests_s4a.log @ c8870aa, tmp/runtests_s4b.log
@ a6c9d72) — zero regressions incl. all polyglot-namespace tests.

The 13 root causes, in landing order (all general mechanisms):

1. **ref_returning_call_type types by the RESOLVED callee** (CIR): a
   late-bound overload set leaves tcf->var on an arbitrary member; the
   token's returns() said `allocator&&` where the re-rank winner returned
   `_Vector_impl&&` → the `_Vector_impl_data(allocator)` no-ctor-match.
2. **flush_pending_stmts** (new helper): ctor/dtor prologue+epilogue
   builders splice materialized temp decls into THEIR OWN list — they
   leaked into the NEXT translated function (undeclared `__a` in
   _Vector_base's dtor).
3. **translate_if flushes condition temps ahead of the IF** (both arms) —
   they landed inside the then/else block (undeclared objtmp). NOTE:
   while/for conditions NOT yet covered (temp would hoist wrongly —
   semantics: per-iteration construction; revisit when hit).
4. **`= default` DEFAULT ctor parses as `{}`** ([dcl.fct.def.default],
   defaulted_member_parses_empty): the prologue machinery IS the implicit
   definition. `= delete` + defaulted copy/move stay declaration-only —
   defaulted COPY/MOVE need memberwise synthesis (OPEN; vector's
   `_Vector_base(_Vector_base&&) = default;` will need it).
5. **class_method_call __retbuf ABI at the CALL SITE** (direct + vtable):
   by-value non-trivial class returns materialize a cleanup-tagged temp,
   pass &temp as the hidden LEADING arg; expression value = temp lvalue
   (`__x.get_allocator() == __a`).
6. **Empty mem-initializer = value-initialization** ([dcl.init]p8):
   scalar/pointer member zero-assign (`_Vector_impl_data() : _M_start()…`
   left garbage the dtor freed → abort).
7. **Union with class-only syntax delegates to the class parser**
   ([class.union]; parsing_cpp_union_class → DataDefCLASS::union_layout;
   layout + CIR emission already branch on it). Real vector's
   `union _Storage` in _Temporary_value.
8. **operand_value_datadef types CALL operands by the re-ranked winner**
   via new `Program::resolved_call_funcdef` — the ONE parse-side re-rank
   (parseCallFunc's arity block refactored onto it). Fixes
   `__relocate_a_1<auto,…>` deduced from __niter_base's bound placeholder.
9. **Class-template-id qualified EXPRESSIONS keep the resolved class**:
   parseStatement's decl probe consumed `allocator_traits<_A>` then handed
   parseExprStmt the BARE ident → "Unknown namespace 'allocator_traits'".
   Pass the resolved TokenDataType (dataType arm owns Type::member(...)).
   Reducer tmp/w8d.mad.
10. ***member dispatches operator*** on a CLASS member reached via
    implicit this (member twin of the variable arm) — move_iterator's
    `*_M_current`.
11. **`typename X::type{...}` in EXPRESSION position** ([expr.type.conv]):
    resolve the dependent type, Redo through the dataType arm (vector
    swap's `typename _Alloc_traits::is_always_equal{}`).
12. **Unqualified type-name functional-construction fallback** in the
    ident arm (`true_type()` inside std bodies): resolve through the one
    shared resolver, Redo.
13. **ARGUMENT LEXICAL NAMESPACE** ([basic.lookup]) — THE BIG ONE:
    parseCallFunc/parseCallMethod CLEARED current_namespace around every
    argument parse (a polyglot-era artifact: statement-level
    `php::foo(args)` carries the CALLEE's ns in current_namespace, args
    are user-scope). The qualified-stmt arm now RECORDS the lexical ns
    (qualified_stmt_callee_ns / qualified_stmt_lexical_ns) and argument
    parsing restores THAT. This is why `true_type()` was "undeclared"
    ONLY inside instantiated member bodies (ns='' mid-body).

**WHERE w2a STOPS (next session entry):** 7 untranslatable nodes, all
CIR-side overload selection:
- `no matching constructor '__normal_iterator(__normal_iterator)'` —
  the IMPLICIT COPY ctor: candidates are default + `(int* const&)` only;
  same-class arg must select implicit memberwise copy
  ([class.copy.ctor]) — likely fix in class_ctor_call_addr/
  select_ctor_overload's no-match tail: same-class arg + no user copy
  ctor → bit-copy (the class is trivially copyable).
- `__normal_iterator(move_iterator<…>)` and
  `_Vector_base(__normal_iterator, allocator*, integral_constant_bool_true)`
  — ctor-initializer arg ROUTING (3 args at a 2-param ctor: looks like a
  delegating-ctor or mem-init arg mis-split; instrument with
  MADC_DEBUG_CTORINIT and read the NO-MATCH dumps).

**Diagnostics added (all gated MADC_DEBUG_NS_RESOLVE /
MADC_DEBUG_CTORINIT):** unknown-ns + deref-fail + undeclared-ident dumps
with instantiation context + upcoming-token stream; deferred-body entry
prints owner/spelling/derived-ns; typedef error names the offending token
+ stream; verbose no-ctor-match candidate dump (hardened — an earlier
version crashed on a dangling string).

**Session reducer inventory additions (tmp/):** w8a-w8d
(allocator_traits qualified-expr; w8d = the 14-line repro), w9a-w9c
(true_type resolution; w9c exposes OPEN gap: static constexpr member
`value` — "Unidentified member 'value' in integral_constant_bool_true").

**OPEN gaps queued (hit but not yet blocking w2a):** defaulted copy/move
ctor memberwise synthesis · static constexpr data members
(integral_constant::value) · static member-template instantiation via
class-qualified call (w8d's residual: `import of undefined item
allocator_traits_…_destroy`) · while/for condition temp placement ·
global-scope (non-namespace) fn templates never instantiate (w7e/w7g).

**TORTURE (full run @ a6c9d72, tmp/gcctest_s4.log): 1567/37/18/0/63 — 3
names OVER the 52-name baseline: `20101011-1.c` (real `<signal.h>` chain,
"Expecting member name in anonymous struct definition", reducer
tmp/w10a.mad) and `loop-2f.c`/`loop-2g.c` (`<sys/mman.h>`, "unexpected
token type 10" = the wall-4 sys-header desync family). ATTRIBUTED by
rebuild-at-6cb9003: all 3 fail at session-3 HEAD too — session-2/3 drift
(those sessions never ran torture), NOT today's fixes (today = ZERO
torture regressions). Fold all 3 into wall 4; they are MERGE-GATE
blockers (zero-regression rule).

---

## STATUS UPDATE 2026-06-12 (session 2) — WALL 2 CORE BROKEN; one residual

**12 root-cause fixes landed this session** (WIP commit on this branch; all
general mechanisms, no shims). The `vector<int>` chain now gets through
`__alloc_traits` rebind, `std::move`/`__alloc_on_swap` instantiation, the
nested `_Vector_impl`/`_Vector_impl_data` classes, and the late-declared
`_M_impl` member. What landed, in dependency order:

1. **`__builtin_addressof`** registered as a core builtin (parser
   `populate_builtin_registry`, zero-param variadic convention, NULL sym;
   CIR already lowered it to N_ADDR by name).
2. **`resolve_member_chain_or_type`** — the ONE seam: every
   `resolve_declared_type_token` branch (incl. all template-id
   instantiation paths + the ns-qualified branch) now consumes
   `Tmpl<Args>::member` chains. With a **non-destructive first-segment
   probe** (member must be a REAL alias/template — expression-position
   `Type::static_member` and if-condition heads stay untouched; the opaque
   escape deliberately NOT probed).
3. **Global operator overload sets rank** — the 3 gates
   (`namespace_overload_set_accepts_more`, parse re-rank,
   `call_target_funcdef`) accept EMPTY namespace_name (set key
   "::operatornew"); declaration-only global operators bind Itanium
   mangled-direct: `operator_code` got nw/na/dl/da,
   `mangle_nested_function` got the global `_Z<code><params>` form →
   `_Znwm`/`_ZdlPvm`/`_ZdlPvmSt11align_val_t` resolve real libstdc++.
4. **Block-scope `using X = T;`** registers flat like a local typedef
   ([dcl.typedef]); block-scope typedef/using no longer LEAK into
   namespace_datatype_map (fn-template instantiation runs with
   current_namespace set — `__alloc_on_swap`'s `__pocs`).
5. **Ident → type re-dispatch**: an identifier naming a datatype_map type
   followed by `::` Redo's through the ttDataType arm (post-tokenization
   registrations: block-scope aliases in instantiated bodies).
6. **`operand_value_datadef`** (Program static): value view of
   reference-typed/vfREFERENCE operands; used by fn-template DEDUCTION
   arg typing AND both overload-ranking arg lists (parser + CIR). Fixes
   `__alloc_on_swap<allocator<int>*>` (pointer-model leak).
7. **`_Tp&&` deduction** — fn_template_deduce_param accepts amps==2
   (rvalue/forwarding refs deduce the VALUE type); std::move/forward
   instantiate.
8. **`DelimDepth` C++ angle rules** — `<` opens ONLY after a name-like
   token (ident/type/`template`); `>` closes only when not inside parens
   opened within the list (per-open paren-depth stack). Real
   `integral_constant<bool, _Tp(-1) < _Tp(0)>` no longer desyncs the
   template scanner (it ate type_traits lines 874→2141: ALL the
   remove_*/add_*/make_* transforms were silently lost).
9. **Reference-cast = no-op on the object**: `static_cast<T&&>(x)` parses
   the target via getReferenceType; CIR cast arm emits the operand lvalue
   unchanged (is_reference target). `ref_returning_call_type` helper:
   ctor-arg typing + `object_arg_addr` bind a ref-returning call's value
   as the object address directly (`&*` folds) — no temp-construct
   recursion (that was a stack-overflow segfault).
10. **Derived-to-base ctor binding** — score_arg_to_param scores a derived
    class arg to a base class param 3 (slicing via base copy/move ctor);
    `object_arg_addr` upcasts ref-returning-call receivers with base
    offset.
11. **Nested-class fixes**: struct member-type slot resolves through the
    one shared resolver (enclosing-class aliases per [basic.scope.class]);
    base-clause nested structs delegate to the class parser (predicate
    pre-guard dropped); NAMED `struct Q {...};` member-less definitions no
    longer inline as anonymous aggregates (`is_anonymous` gate); renamed
    nested classes' FIRST ctor carries local_emit_name
    (Class__SourceName ≠ Class__Class); implicit base default-ctor calls
    resolve via select_ctor_overload + ctor_call_symbol (not blind
    Class__Class composition).
12. **Deferred ctor mem-initializers** — [class.base.init] complete-class
    context: in-class ctor init-lists are token-CAPTURED
    (DeferredFunctionBody::ctor_init_tokens) and parsed with the deferred
    body at class completion via the extracted
    `parse_ctor_initializer_list` (out-of-class ctors still parse eagerly).
    Real `_Vector_base(..., _Vector_base&& __x) : _M_impl(...,
    std::move(__x._M_impl))` names the member declared AFTER the ctor.

Also: "Unidentified member" diagnostic now names member + class.

**WHERE IT STOPS (next session entry point):** `tmp/w2c.mad`
(`std::_Vector_base<int, std::allocator<int> > b;`) now fails ONLY with
`cir error: no matching constructor for call to
'_Vector_base..._Vector_impl_data(allocator_int32_t)'` — a ctor-INITIALIZER
mis-route at CIR time: something constructs the `_Vector_impl_data` BASE
with the `__a` allocator argument. `_Vector_impl`'s ctors registered
correctly (o2 = `(const _Tp_alloc_type&)` verified in --dump-cir).
Hypothesis space (verify, don't trust): (a)
`ctor_initializer_targets_base`'s alias clause
(`class_alias_lookup_cir(owner,"_Tp_alloc_type")` walks enclosing_class →
_Vector_base's alias = allocator; allocator does NOT derive from
_Vector_impl_data, so on paper it shouldn't match — CHECK what it actually
returns, esp. whether `enclosing_class`/`base_class` are even set on the
delegated nested class); (b) `class_ctor_initializer_stmts`' member loop
with flattened base members; (c) `class_member_construct` default-
constructing `_M_impl` with stale explicit args. Instrument
ctor_initializer_targets_base with a gated fprintf and run tmp/w2c.mad.
After w2c: w2a (`vector<int> v;`) is the next face up.

**Diagnostics added (gated)**: `MADC_DEBUG_TYPEDEF_PARSE` (TokenTYPEDEF
enter/record + USING-ALIAS record), `MADC_DEBUG_FNTPL` now also dumps
injected instantiation tokens when env `MADC_DEBUG_FNTPL_DUMP=<substr>`
matches the inst key. TokenTEMPLATE::parse DBG prints file:line — the
GAP-detection one-liner that found fix 8:
`grep "TokenTEMPLATE::parse() at" log | awk -F: '{...}'` (see git log).

**Session reducer inventory (tmp/, all default-mode)**: w2a..w2s
(vector/alloc_traits chain), w3a..w3l (__alloc_on_swap/using-alias),
w4a..w4m (nested _Vector_impl shapes), w5a..w5i (std::move,
remove_reference, full _Vector_impl replica w5a = GREEN), w6a/w6b
(declval/array-spec probes — green). w2c/w2a are the live walls.

**Follow-up noted (user question)**: whether madc-LOCAL template
instantiations should be NAMED with their Itanium mangling (instead of
`__ns_std_*`/flattened keys) for --emit=c11 diffability and
auto-resolution of library-exported explicit instantiations — naming
fidelity only, linkage semantics unchanged.

**PRE-EXISTING unit-test crash (verified NOT this session)**:
`bin/test_libmadc_program` SEGFAULTS in the full run (inside a JIT'd
`__madc_shim_*` during the string-call shim tests; backtrace:
`gdb -batch -ex run -ex bt bin/test_libmadc_program`). It kills
`make -C src fulltest` at the UNIT phase before integration tests run.
Verified by stashing this session's diff + rebuilding: the BASELINE
branch crashes identically — so this branch's recorded 549/33 came from
`bash scripts/run_tests.sh` directly. Run integration that way until
root-caused (own wall; state-dependent: single `-tc=` runs pass, the
full sequence crashes).

---

## 0. TL;DR

Branch **`feature/retire-embedded-shims-claude`** off develop @ `2832fc0`
(develop untouched). ALL bucket-3 shims are DELETED (23k lines):
`include/madc/` holds ONLY bucket-1/2 compiler headers
(float/limits/stdarg/stdbool/stddef/stdint) + madc-owned `ns_*`. Real
glibc/libstdc++ serve every mode **including default STD_MADC** (which now
presents as g++ — `presents_as_cpp()`). Real `<iostream>/<string>/<cmath>`
compile AND run g++-identically in default mode. Integration was 546/36
before the latest (uncommitted-at-writing) stream-boundary fix; expect
~548+/34− after. The work remaining is (a) the wall list in §4, (b) the
PROCESS conformance items in §5 that institutionalize the partition doc.

**User rulings (binding):**
- K&R-era recovery (old-style params, implicit-int defs) ONLY under
  explicit `--std=c78..c17`. Never STD_MADC, never C++ (`knr_supported()`).
- No shims, no per-case hacks, fix at the deepest layer — categorical.
- All bucket-3 hand-rolled headers stay deleted; never re-author them.

## 1. The partition model (from madc-header-partition-handoff.md)

A header is madc's ONLY if its correctness requires codegen-private facts
(size_t identity, va_list layout, limits, intrinsics). Bucket 1 = pure
compiler headers (madc supplies fully). Bucket 2 = layering shims that
`#include_next` to the system copy (stdint/limits/float). Bucket 3 =
EVERYTHING else — all glibc + all libstdc++ — consumed REAL and unmodified.
The authority for bucket 1/2 membership is `gcc -print-file-name=include`
(the `$OWN` dir), NOT the standard's freestanding list.

## 2. What landed (commit chronology on this branch)

- `fa25e7f` **K&R gate**: `Program::knr_supported()`; harness `--std=c17`;
  9 K&R-era tests got `.flags`. GATED GREEN (fulltest 582, torture 52-name
  baseline ZERO regr +1 fixed → `docs/parity/torture-failset-current.txt`
  now 52 names, SMAUG soak green).
- `13383b7` **presents_as_cpp()**: STD_MADC seeds `__cplusplus` (201703L
  floor) + `__GNUG__` like explicit C++ modes; C modes stay plain gcc.
  Pin tests: testpredefmacros (defined) / testpredefmacros_c17 (absent).
  GATED GREEN (same three gates).
- `2d61556` **the sweep**: all bucket-3 shims deleted; `#include_next`
  made positional (never consults named PCH/embedded caches); baked PCH
  table EMPTIED (stale single-mode `gcc -E` captures that shadowed real
  headers; `gen_precompiled_headers.sh` HEADERS=() with rationale; lookup
  machinery kept for the proper PCH track). Plus 3 root-cause fixes:
  typedef_emit_name chokepoint for extern-proto RETURN types
  (cir_builder ~11289); shim text-ctor requires `required_param_count()<=2`
  (cir_builder `class_text_ctor`); template DEFAULT-arg declarator
  suffixes `_Tp*`/`_Tp&`/`_Tp&&` fold into the arg type (parser ~2660,
  mirrors the explicit-arg star fold; the suffix used to LEAK into the
  live token stream).
- `bb8083b` **preprocessor root causes**: #if expands function-like
  macros WITH arguments (expandIfMacros); gcc's guard-aware
  multiple-include optimization for SYSTEM headers (guard-less
  bits/mathcalls.h re-tokenizes per `_Mdouble_` pass — float/ldouble math
  decls were silently lost) while user `"..."` includes keep require-once
  (testincludeonce); generic `__builtin_X -> X` libc-twin dlsym fallback
  (emit_symbol = twin; kills the grow-forever hand list); FP-classify
  builtin family as sizeof-dispatched statement-expr macros onto REAL
  glibc exports (`__fpclassify*`/`__isnan*`/`__isinf*`/`__finite*`).
- `1b91e9f` **SFINAE pre-check** ([temp.deduct]):
  instantiate_fn_template_binding resolves a substituted
  `typename Q::X<args>::member` RETURN type in a sandboxed token push
  BEFORE the body parse; unresolvable → silent candidate discard. Real
  <cmath>'s integer-only `__gnu_cxx::__enable_if` overloads no longer
  hard-error float calls.
- (latest) **instantiation stream-boundary fix** (parser
  instantiate_fn_template_binding tail): the injected token run is
  restored to `base_depth` UNCONDITIONALLY after the parse — an "ok"
  `__hypot3<float>` instantiation left 2 trailing inj tokens that the
  resumed outer parse consumed, shifting every later declaration
  ("__z undeclared" two functions later). Cleared testmathh +
  testieeehugeval. `#if MADC_DEBUG_FNTPL` now also reports any
  imbalance (the diagnostic that found this).

## 3. Diagnostics arsenal (all gated, compile with -D<flag>)

- `MADC_DEBUG_FNTPL=1` — fn-template instantiation outcomes + STREAM
  IMBALANCE reports (parser.cpp).
- `MADC_DEBUG_NS_RESOLVE` — unknown-namespace throws with instantiation
  depth (parser.cpp ~13755).
- `MADC_DEBUG_TYPEDEF_EMIT` — typedef_emit_name alias→tag decisions
  (cir_builder.cpp).
- `MADC_DEBUG_BASE_CLAUSE` — base-clause first-lookup resolutions
  (parser.cpp ~19545).
- `madc -E` — preprocessed token stream (the bisect substrate;
  tmp/m4_pp.txt is `#include <math.h>` in default mode).
- Reducers in tmp/ (gitignored), ALL default-mode no-flags unless noted:
  realios*.mad (iostream), p2.mad, c9/c11.mad (extern-proto string),
  d1-d3.mad (string by-value), v1-v6.mad (vector/iterator bases),
  m1-m4.mad (math.h), h1-h4.mad (hypot shape), pfx1/pfx2.mad
  (m4_pp.txt prefixes), bisect.sh (prefix bisector).

## 3.5 VERIFIED WORKING (do not re-litigate; re-prove with these exact commands)

All in DEFAULT mode (no flags) unless noted — that is the point of the campaign:

```bash
# Real <iostream>/<string>/cin/getline/cerr, g++-byte-identical:
printf '#include <iostream>\n#include <string>\nint main(){ std::string s; std::cin >> s; std::cout << "got: " << s << std::endl; return 0; }\n' > tmp/ok1.mad
echo hello | bin/madc tmp/ok1.mad          # -> "got: hello", exit 0
# Real <math.h>/<cmath> incl. float pass + SFINAE abs/sqrt + hypot3:
printf '#include <math.h>\n#include <stdio.h>\nint main(){ printf("%%f\\n", HUGE_VAL); return 0; }\n' > tmp/ok2.mad
bin/madc tmp/ok2.mad                        # -> inf, exit 0 (stderr noise = wall 7)
# Real <vector> HEADER parses+compiles (instantiation = wall 2):
printf '#include <vector>\nint main(){ return 0; }\n' > tmp/ok3.mad
bin/madc tmp/ok3.mad                        # exit 0
# K&R gating (user ruling):
printf 'int f(a,b) int a; int b; { return a+b; }\nint main(){ return f(1,2)==3?0:1; }\n' > tmp/ok4.mad
bin/madc --std=c17 tmp/ok4.mad; echo $?     # 0 (accepted)
bin/madc tmp/ok4.mad; echo $?               # 1 (rejected in dialect)
# Suite baseline at branch HEAD: 549 passed / 33 failed / 0 timed out / 18 skipped.
# Phase 0+1 gates (recorded green 2026-06-12): torture 1570/34/18 = the 52-name
# baseline docs/parity/torture-failset-current.txt; SMAUG --project soak green.
```

The 18 pre-campaign `--no-embedded-headers` tests (testfstream/testloop/
testdefer/test_extern_polymorphic/*_realhdr/3-way gates) all still pass.

## 3.6 THE EXACT 33 FAILURES at HEAD (tmp/runtests_p2h.log), by cluster

- **Container instantiation (12)** — wall 2: testvector testvectorptr
  testmap testset testcontainerdtor testtemplatecontainer
  testtemplatestring testsubscript testsubscriptarrow testsubscriptmember
  teststruct3 test3eqclass. First error: `vector<int> v;` → "Expecting
  type after 'typedef'" inside the monomorphized real template body.
- **String-class behaviors (5)** — likely same root as containers (real
  basic_string member-template instantiation): teststdstringconv
  teststringglobal teststringref teststringrel testrefreturn.
- **madc eval surface (6)** — wall 3 (_ISupper ctype enums in the eval
  TU): testmadceval testmadcevalexpr testmadcevalexprctx
  testmadcevalexprtyped testmadcevalscope testmadc_ns.
- **sys headers parse (3)** — wall 4: teststat teststatret testservent.
- **operator<< mangle (2)** — wall 5 (_ZNSolsESo): testmultiret testrust.
- **sstream (1)** — `__byte_op_t` undeclared: testsstream.
- **foreach/php array (2)**: testforeach2 testforeachref ("too few
  arguments" class — check shim/trampoline interplay with real string).
- **misc (2)**: testprefer (prefer directive + real headers),
  testrubycharsshadow.

## 3.7 TRAPS REDISCOVERED THIS SESSION (cost real time; don't repeat)

- **Log truncation**: `cmd | tail -N > log` in a background task loses
  the failset head. ALWAYS `cmd > tmp/x.log 2>&1` then inspect.
- **tmp/*.madh shadowing**: find_filesystem_precompiled_header includes
  the CURRENT SOURCE DIR in its candidates — stale .madh files next to
  tmp/ reducers silently hijack `#include <...>`. `rm tmp/*.madh` first.
- **Stale-binary fulltest lie**: a fulltest summary that contradicts a
  by-hand run of the same binary = NAS mtime staleness. `make -C src
  clean` + full rebuild, then re-run by hand before trusting either.
- **Error-position misattribution**: errors from header-origin tokens
  print the MAIN file's name with the header's line number (e.g.
  "tmp/x.mad:3567"). The line number belongs to the real header — find
  it with `grep -n` in the suspect header, or via `madc -E` output.
- **Exit codes through pipes**: `bin/madc x | head; echo $?` reports
  head's status. Use `>/dev/null 2>&1; echo $?`.
- **DBG() is dead on worker threads** (thread_local) — use the gated
  `#ifdef MADC_DEBUG_*` fprintf diagnostics (§3) instead.
- **Throw prints unconditionally** (throwbuf::sync → stderr) even when
  the exception is caught and tolerated — printed error ≠ fatal error;
  check the EXIT CODE.

## 4. REMAINING WALLS (attack order; per-fix METHOD in §6)

1. ~~typename dependent return types~~ CLEARED @1b91e9f.
   ~~math param-scope leak~~ CLEARED @d11f5a3 (stream boundary).
   ~~class-scope alias in hidden-friend bodies~~ CLEARED @ the
   parse_hoisted_friend_operator owner-scope fix ([class.friend] lookup:
   hoisted parse runs with the owner class pushed on class_scope_stack).
   Real `#include <vector>` now PARSES AND COMPILES clean (tmp/v1.mad).
2. **Real container template INSTANTIATION** — `vector<int> nums;`
   (testvector:8) now fails with "Expecting type after 'typedef'" while
   monomorphizing the REAL vector template body (a typedef inside the
   instantiated body doesn't resolve). This is the new face of the
   container cluster (testvector/vectorptr/map/set/containerdtor/
   templatecontainer/templatestring/subscript* — ~12 tests). See memory
   `project_template_instantiation` (Borland monomorphize is THE model;
   string already works this way). Separately testsstream fails earlier
   on `__byte_op_t` undeclared (std::byte operator machinery in real
   <sstream>/<ostream> chain) — reduce independently.
   ALSO: a known LATENT gap from the same friend machinery — hidden
   friend NON-operator definitions (`friend iterator mk(...) {...}`) are
   skipped but never hoisted (tmp/w1.mad: "mk undeclared" at use).
   libstdc++ uses hidden-friend swap() widely; generalize the hoist
   predicate from operator-definitions to ANY friend definition with a
   body.
3. **testmadceval\*** (6 tests) — emitted eval code references `_ISupper`
   etc.: glibc ctype.h's anonymous enum constants don't reach the child
   eval TU. Likely the eval-TU synthesis (`<ns_madc>` path) needs the
   same real-header include context as the parent.
4. **teststat/teststatret/testservent** — parse error in real
   sys/stat.h chain under default mode ("unexpected token type 10" near
   EOF = stream desync; instrument like wall 1 — possibly another
   boundary/recovery leak).
5. **testmultiret/testrust** — bogus mangled import `_ZNSolsESo`
   (ostream<<ostream by value — overload resolution mis-pick on the
   real-header operator<< set; reduce `cout << <multi-ret-call>`).
6. **--emit=c11 hygiene** (non-blocking): `operatornew[]__o5`,
   `operator""s` leak as raw C identifiers in emitted text (JIT tree
   unaffected). safe_ident()-class fix at emission.
7. Stderr NOISE from caught/discarded instantiation attempts
   (throwbuf::sync prints unconditionally): wrap candidate-scoring
   instantiation in a diagnostics-suppressed mode so SFINAE discards are
   silent (currently they print scary-but-harmless errors, e.g. m1's
   "cannot dereference non-pointer type"). Principle: a DISCARDED
   candidate prints nothing; the CHOSEN candidate's errors are real.

## 5. PROCESS CONFORMANCE (institutionalize the partition doc — overdue)

These make the model self-enforcing instead of memory-dependent:

- **P1. Step-1 discovery gate**: new `scripts/check-header-partition.sh`
  — enumerate `gcc -print-file-name=include`, record GCC version +
  listing checksum in `docs/parity/header-partition-baseline.txt`;
  verify `include/madc/` ⊆ {bucket-1/2 names from $OWN} ∪ {ns_*}; FAIL
  on any bucket-3 reappearance. Wire into `make -C src fulltest` next to
  check-no-std-hardcoding.sh. THIS is the unfakeable "shims stay dead"
  contract.
- **P2. Step-4 macro parity**: madc has NO `-dM` yet (gap). Add
  `--dump-macros` (trivial: dump define_map/macro_map after init), then
  diff against `gcc -dM -E -x c /dev/null` and `g++ -dM -E -x c++` for
  the macros real headers branch on; record the accepted-diff baseline
  in docs/parity/. (gen_predefined_macros.sh captures build-time values;
  the diff verifies nothing load-bearing is missing.)
- **P3. Acceptance oracle (partition doc "Acceptance tests")**: freeze
  the C smoke (stdio/stdlib/string/stdarg/stddef/limits) and C++ smoke
  (type_traits/utility/tuple/vector/string/memory) as permanent
  tests/*.mad fixtures in BOTH default and --std=c++17 modes, once wall
  2 falls.
- **P4. Bucket-2 conformance**: current stdint.h/limits.h/float.h are
  FULL shims; the doc prescribes thin `#include_next` chaining shims.
  Convert + verify madc's #include_next semantics against each.
- **P5. Step-5 builtins checklist**: enumerate the `__is_*`/`__has_*`
  intrinsics the installed libstdc++ calls (command in the doc) and
  track implemented-vs-missing in docs/parity/ (drives <type_traits>
  conformance work).

## 6. METHOD (mandatory — unchanged)

Per fix: reduce (tmp/, NO flags = default mode is the point) → attribute
(gcc + clang + stock `/workspace/mir/c2m FILE -ei`; for madc-path bugs use
`--dump-cir`, NOT emit-C-as-truth) → DEEPEST-layer fix, no shims, no
per-name special cases → rebuild (`touch` the .cpp first — NAS mtime trap;
clean-rebuild if results look impossible) → re-probe reducers →
`make -C src fulltest` (cap: `( ulimit -t 3600; timeout 3000 ... )`, ONE
heavy job at a time) → full torture ALONE, failset-name diff vs
`docs/parity/torture-failset-current.txt` (52 names) → SMAUG soak
(`cd /workspace/MadSMAUG/runtime/area && timeout 50 /workspace/madc/bin/madc
--project /workspace/MadSMAUG/compile_commands.json -lcrypt 4000`; exit 124
+ "Realms of Despair ready at" = good) → commit on THIS branch → update the
STATUS block in docs/plans/2026-06-12-retire-embedded-shims-plan.md.
Background long runs (`run_in_background`), capture FULL logs to tmp/
(never `| tail` into the log — it truncates the failset!).

## 7. MERGE GATE (do not merge to develop before ALL of)

fulltest 100% green (582+ incl. re-greened tests) + both check gates +
P1 partition gate · torture zero regressions vs 52-name baseline · SMAUG
soak · `bash scripts/run_tests.sh --exe` (shared-codegen surfaces moved)
· mirrors synced (claude_status.json head, CHANGELOG, ROADMAP, KG via
scripts/kg_query.sh, agent memory) · user approval (develop is the
shared stable branch).

## 8. Why the failures were "new" (user question, answered 2026-06-12)

Only 18 tests ever ran `--no-embedded-headers` (iostream/fstream/string/
compare families, under --std=c++17). vector/map/set/sstream and the
whole madc-dialect surface (eval, php arrays, foreach) had ONLY ever run
against the embedded shims. The sweep put all 582 tests through real
headers in default mode for the first time; every failure is a latent
real-header bug, not a regression of proven coverage.
