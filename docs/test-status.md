# Test Status

> **Local branch update (2026-06-26, `feature/front-end-performance-claude` @
> converted system-header reference-forwarded placement-new pack slice,
> including the earlier reserved scalar/pointer helper call widening,
> with two-tree direct type-arg binding and direct
> value/ref/expression/forwarding-call/constructor pack fan-out plus covered
> system-header placement-new pack fan-out and simple class `_Up` placement-new
> tsubst plus direct `__destroy(T*)` helper tsubst and local non-pack nested
> namespace-call tsubst plus nested function-template instantiation, plus
> dependent-parse-error scope balancing and pointer-parameter-pack call
> expansion):** fulltest
> **670 passed, 0 failed, 0 timed out, 18 skipped** (exit 0, both check gates
> GREEN). The env-gated tsubst path is also green:
> `MADC_XTEST_DEP_PARSE=1 make -C src fulltest` reports **670/0/0/18**.
> `bin/test_cir` reports **83 test cases, 1026 assertions, 4 skipped** after
> adding coverage for direct `tsubst_type_args` binding of a body-only template
> parameter and direct `tsubst_type_arg_packs` capture for a variadic member
> template, plus direct CIR fan-out for value-pack call arguments like
> `sink(args...)` and reference-pack call arguments like
> `Args&... args` / `sink(args...)`, pointer-pack call arguments like
> `Args*... ps` / `sink(ps...)`, and expression-pattern packs like
> `sink((args + 1)...)`, plus the first forwarding-call pack pattern
> `sink(std::forward<Args>(args)...)`, plus covered local member-template
> constructors like `Holder(Args... args) { member = sink(args...); }`, plus
> system-header placement-new pack bodies, scalar `_Up` lowering, and simple
> class `_Up` lowering with scalar/pointer constructor pack elements for
> allocator-style `new ((void*)p) _Up(std::forward<Args>(args)...)`, plus
> direct `__destroy(T*)` helpers that defer pointee inspection until after
> substitution and lower class pointees to the concrete destructor, plus local
> non-pack nested namespace calls such as `sink(nn::ident(v))` whose copied
> callee ids re-resolve from substituted argument types, plus by-value
> class-object constructor packs such as `Box(Item)` and `PairBox(Item, Item)` inside
> allocator-style placement-new bodies, plus value-returning forwarded class objects
> bound to reference constructor parameters such as `PairRef(const Item&, const Item&)`,
> plus local reference-returning identity-forwarding constructor packs that pass
> `Args&...` through `std::forward<Args>(args)...` and bind to class-reference
> constructor parameters, plus simple system-header dependent calls
> whose substituted args/return are concrete non-class scalar/pointer/void shapes
> and whose resolved callees have a materializable body or external symbol,
> including reserved `__*` helper names and copied-call reachability for lazy
> body emission, plus direct system-header reference-forwarded placement-new
> constructor packs whose per-element nested call resolves through
> `resolve_copied_dependent_call` and returns the same/derived class expected by
> the constructor reference parameter, plus converted system-header
> reference-forwarded placement-new packs where that returned class is accepted
> by the target's single-argument converting constructor and a target temp is
> materialized before the outer constructor call. The latest robustness slice
> also adds `tests/testdependentparseerror.mad`, proving an env-gated dependent
> parse error balances the temporary parameter compound scope and exits
> nonzero without SIGSEGV. Phase 4 is now tracked at
> roughly **74% implemented** by coverage weight. Broader system-header
> destructor/object-address pack surfaces, class-valued placement-new
> constructor argument packs beyond those direct/converted apertures, broader
> system-header nested/dependent calls, and template-id body/return surfaces
> remain on the re-parse fallback.
>
> **Current (2026-06-22, `develop`, v0.30.0 — set wall CLEARED, pushed to
> origin/develop):** Fulltest **669 passed, 0 failed, 0 timed out, 18 skipped**
> (exit 0, both check gates GREEN); gcc.c-torture failset byte-identical to the
> 51-name baseline; zero regressions. The `std::set`/`std::map` "set wall" — a
> STACK of eight root-cause container bugs — is **fully cleared** on the default
> C++17 real-header path; `testset`, `testmap`, `testsubscript`,
> `testcontainerdtor`, `testmadc_ns` all green. **bug-7b @ `da96d7a`** (the final
> red, `testsubscript`): the earlier cont. 7 "`std::get` return-type unresolved"
> reading was the SYMPTOM — the get instantiation was already correct. Real cause:
> `resolve_fn_template_return_by_key` bound `std::get<0>`'s non-type arg `0` to the
> TYPE parameter of the by-type `std::get` overload, naming the return type `"0"`,
> which leaked into the call's parse-time `return_override` and broke `std::get`
> ref-binding in the piecewise pair ctor of `map<K,string>::operator[]` (undefined
> `basic_string…__o15`); surfaced only when two `pair<const string,V>`
> instantiations coexist. Fix (deepest layer): a non-type value arg cannot bind to
> a TYPE template parameter (`[temp.arg.nontype]` substitution failure → candidate
> removed), via `datadef_is_nontype_constant`. Drift gate refined with an audited
> `// allowed-exception` opt-out (`24e8125`) so `make fulltest` exits 0. Full
> trail: `docs/plans/2026-06-19-map-instantiation-strategy.md` (**cont. 8**,
> supersedes cont. 5/6/7). **bug-7a @ `b5698d7`** (`set<string>::insert("lit")`
> UDC ctor scan; cleared `testset`). **bug-5c @ `bc7693d`** (`set<int>` SIGSEGV: the
> copy-elision/NRVO init path for a retbuf-returning method delegated explicit
> args to `build_call_args` with no `__this` offset → a reference arg coerced
> against `__this`, passed as a value where an `int*` was expected → int-as-
> address deref; fix adds `param_base` to `build_call_args`; test
> `testretbufrefarg`). **bug-6 @ `9e959fd`** (`set<string>`+`map<string,
> string>` → MIR `Repeated item declaration _Tp2___dtor`: the nested
> `__aligned_membuf<_Tp>::_Tp2` had its store key enclosing-qualified but
> `dds->name` left bare so both emitted the same dtor; fix renames the emitted
> identity; cleared `testcontainerdtor` + `testmadc_ns`; `testset.mad` converted
> off C++20 `contains` to C++17 find/end). **bug-7 remains** (`testset`/
> `testsubscript`): MIR `import of undefined item ..._o<N>` — an implicit
> `const char*`→`std::string` conversion at a header method-call argument emits
> a `__o<N>` wrapper call but neither materializes the string temporary nor
> emits the wrapper body; see
> `docs/plans/2026-06-19-map-instantiation-strategy.md` (2026-06-22 cont. 5).
> Earlier this session: bugs 1–4. **bug-1 @ `cbd693a`** (call `(` after a substituted type-param
> misparsed as a cast; test `testfunctorctorarg`). **bug-2 @ `94d0798`**
> (`return` skipped the implicit converting-ctor on a trivially-copyable
> by-value class return; test `testreturnconvctor`). **bug-3 @ `7d53ed1`**
> (two classes' hidden-friend operators sharing a `_Self` typedef collapsed to
> one overload — identity used raw param text; test `testfriendopself`).
> **bug-4 @ `c781287`** (a functor whose `operator()` is a member TEMPLATE was
> never dispatched — `g(args)` mis-lowered to `g = args`; five parser sub-fixes
> covering operator-id declarator recognition, the param-list paren after an
> operator-id, the instantiation rename across the operator-id span, pointer
> return-type preservation, and `is_static` scanning only the header; tests
> `testfunctortmploperator`, `testmembertmplptrret`). With bugs 1–4, real
> `std::set<int>` now **compiles**. **bug-5c remains** (set's runtime blocker):
> SIGSEGV in `_Rb_tree::_M_construct_node__mti`, a variadic-member-template /
> allocator-`construct` bug; see
> `docs/plans/2026-06-19-map-instantiation-strategy.md` (2026-06-22 cont. 3).
> gcc torture non-timeout failset byte-identical to the 51-name baseline across
> all four fixes (zero regressions).
>
> **Prior WIP (2026-06-22, `wip/map-cxx17-salvage-codex` @ `3534b44`
> plus local recovery fixes):** the previous dirty session is preserved at
> `failed/2026-06-22-map-cxx17-attempt-codex` commit `3534b44`; live work
> continues on the salvage branch with dirty fixes in `include/datatokens.h`,
> `src/parser.cpp`, and `src/cir_builder.cpp`. Focused C++17 map validation is
> green: `teststdmapint` pins `std::map<int,int>` insert/update through real
> libstdc++ headers under `--std=c++17 --no-embedded-headers`, and
> `tests/testmap.mad` uses C++17 `find/end` rather than C++20 `contains` and
> passes for `std::map<std::string,int>`. Recovered regressions from the
> interrupted handoff: `testforeach2`, `testtuple`, `testfstream`, `testloop`,
> `testmadcevalexpr`, `testmadcevalexprctx`, and `testmadcevalexprtyped` are
> green. C++20 canaries `testcompare_realhdr`, `testspaceship_realhdr`,
> `testdefaultedcmp_realhdr`, `testrewritten_realhdr`, and `testinvocable`
> are also green under per-test `--std=c++20 --no-embedded-headers` flags.
> Latest fulltest attempts are noisy under the runner's default 5-second
> per-test integration cap on this host: run 1 reported
> **657 passed, 4 failed, 2 timed out, 18 skipped**; run 2 reported
> **650 passed, 3 failed, 10 timed out, 18 skipped** with shifting unrelated
> timeouts. Isolated timeout candidates pass sequentially under the default
> cap, so the stable functional red list is now `testcontainerdtor`,
> `testmadc_ns`, `testset`, and `testsubscript`; `testmap` is no longer in the
> focused failure set. The former
> `FEATURE_CONST_TYPES` and `FEATURE_DERIVED_TO_BASE_DEDUCTION` paths are now
> default after external-method typed-return/ref-argument lowering fixed the
> historical stream/string regressions. Remaining diagnostics: non-fatal
> libstdc++ `stl_tree`/`stl_map` pointer-type warnings. Latest local handoff
> update: the previous `std::get` scoped-alias blocker is fixed generically
> through same-DataDef typedef-alias preservation plus concrete partial-spec
> completion from the opaque template path; `_Nth_type`/tuple reducers and
> `teststdmapint` pass. The later undefined `basic_string...__o15` wrapper was
> moved forward by generic CIR reference-return/constructor-argument handling.
> The remaining runtime corruption was caused by flattening libstdc++ anonymous
> union members in `std::basic_string` as sequential fields, inflating the
> string layout and overflowing pair/tree storage. `DataDefSTRUCT` now records
> anonymous aggregate groups and CIR emits unnamed anonymous struct/union
> members, preserving the ABI layout. `make -C src` passed; `--emit=c11` for
> `tests/testmap.mad` shows `_M_local_buf`/`_M_allocated_capacity` inside an
> anonymous union; `tests/teststdmapint.mad` and `tests/testmap.mad` both pass.
> Focused regressions `tests/testtuple.mad`, `tests/testforeach2.mad`,
> `tests/testfstream.mad`, `tests/testloop.mad`, and the eval-expression
> regressions also pass after this fix. Revalidated after a clean non-debug
> rebuild; `tests/testmathh.timeout` keeps that passing math-header test off
> the default 5-second wall cap. gcc.c-torture and SMAUG were not rerun in this
> WIP validation.
>
> **Previous (2026-06-11, `feature/strict-equality-claude` @ `1ffbc8c`
> — `===`/`!==` strict equality, STD_MADC dialect):** fulltest
> **577 passed, 0 failed, 0 timed out, 18 skipped** (make exit 0, both
> check gates GREEN, clean rebuild zero warnings). New operators:
> type-domain identity AND value equality (`uint32_t === int32_t` false;
> `long === long long` true; enums/bool own domains; literals keep their
> C type), user `operator===`/`operator!==` overloading (vendor-extended
> manglings `v23eq3`/`v23ne3`), class domain rule via `operator==`
> (`string s === "x"` true), STD_MADC token gating (conformance fix),
> eval-DSL `!==` + comparison-result inference fix (a string `!==`
> previously segfaulted the host). New tests: `test3eq`, `test3eqclass`,
> `test3eqerr` (expect_err), `test3eqgate`/`test3noteqgate` (std-floor).
> gcc.c-torture failset **byte-identical** (1567/26/29/0/63); SMAUG soak
> green (exit 124 + ready line). Deferred (spec): script-side `array`
> strict equality (no whole-value scalar ops on that surface yet).
>
> **Previous (2026-06-11, `feature/template-instantiation-claude` @ `01528ed`
> — `= default` comparison synthesis; `<=>` track COMPLETE):** fulltest
> **572 passed, 0 failed, 0 timed out, 18 skipped** (make exit 0, both
> check gates GREEN). Ordering-vs-ordering `==`/`!=` and
> `auto operator<=>(const V&) const = default` (all six comparisons from
> one declaration) work — new test `testdefaultedcmp_realhdr` (8 shapes,
> g++-verified). gcc.c-torture failset **byte-identical**
> (1567/26/29/0/63); SMAUG green.
>
> **Previous (2026-06-11, `feature/template-instantiation-claude` @ `aff26fa`
> — `<=>` rewritten candidates):** fulltest **571 passed, 0 failed, 0 timed
> out, 18 skipped** (make exit 0, both check gates GREEN). `r != 0`,
> `0 == e`, and the only-`<=>`/`==` user-class idiom (all six comparisons)
> work — new test `testrewritten_realhdr` (9 g++-verified shapes).
> gcc.c-torture failset **byte-identical** (1567/26/29/0/63); SMAUG green.
> NEW known gap (pre-existing): `Q a(1), b(2);` ctor-arg multi-declarator
> hangs the parser — split declarations.
>
> **Previous (2026-06-11, `feature/template-instantiation-claude` @ `7a56d72`
> — `<=>` slice 3a, the token lowering):** fulltest **570 passed, 0 failed,
> 0 timed out, 18 skipped** (make exit 0, both check gates GREEN).
> `a <=> b` works on builtin scalars (byte-select into a category temp;
> partial_ordering unordered=2 for NaN) and class operands (hoisted friend
> operator<=>, forward + reversed). New test: `testspaceship_realhdr`
> (8 g++-verified shapes). gcc.c-torture failset **byte-identical**
> (1567/26/29/0/63); SMAUG soak green.
>
> **Previous (2026-06-11, `feature/template-instantiation-claude` @ `5f63a20`
> — `<=>` slice 2b COMPLETE, hidden-friend operator bodies):** fulltest
> **569 passed, 0 failed, 0 timed out, 18 skipped** (make exit 0, both
> check gates GREEN). `r < 0` on `std::strong_ordering` calls the compiled
> TU-local friend from the real `<compare>`. New tests: `testfreeop`
> (free-operator dispatch by overload resolution + literal-0
> null-pointer-constant overload choice, g++-verified), `testhiddenfriend`
> (user-code hidden-friend operators, default std), `testcompareops_realhdr`
> (`<compare>` strong/weak/partial orderings vs literal 0, reversed
> operands, unordered — 7 shapes, g++-verified). NOT in scope: C++20
> rewritten candidates (`r != 0`) — errors loudly, queued in
> cpp-support.md P2.15. gcc.c-torture failset **byte-identical** per
> ingredient commit (1567/26/29/0/63); SMAUG soak green per commit.
>
> **Previous (2026-06-11, `feature/template-instantiation-claude` @ `e124de5`
> — template-instantiation batch COMPLETE, 2a/2b/2c/2d):** fulltest **564
> passed, 0 failed, 0 timed out, 18 skipped** (make exit 0, both check gates
> GREEN). New tests this branch: `teststod{,_realhdr}` (2a fn-template
> empty-pack elision — std::stof/stod), `teststrlitplus_realhdr` (2b-i
> `"pre" + s` exported mangled-direct), `teststrplusbody_realhdr` (2b-ii
> `a + "bar"` free-operator BODY instantiation, foobar/prefoobar),
> `testctornomatch` (2c: a class declaration whose initializer matches no
> ctor is a LOUD compile error — first `.expect_err` compile-error test),
> `testctorrefarg` (`A local(p)` from `const A&` selects the copy ctor — the
> one real silent drop the 2c gate surfaced), `teststrrefparam_realhdr` (2d:
> reference operands resolve as the referenced class — `cout << s` with a
> `const string&` parameter, `a + b` on reference params). Includes the
> char_traits explicit-spec instantiation-key fix (`1abbee8` —
> `std::char_traits<char>::length` silently folded to 0 before).
> gcc.c-torture failset **byte-identical** to the post-audit baseline
> (1567/26/29/0/63); SMAUG soak green (exit 124 + ready line).
>
> **Previous (2026-06-11, `develop` @ `21bfec9` — failset classification
> audit):** fulltest **557 passed, 0 failed, 0 timed out, 18 skipped** (make
> exit 0, both check gates GREEN; re-verified at the audit commit — no
> compiler code changed). gcc.c-torture moves to the post-audit baseline:
> **1567 passed, 26 compile-failed, 29 runtime-failed, 0 timed out, 63
> skipped** — the 33 class-(c) gcc-internal/torture-only tests are now
> FORMAL skips via `docs/parity/torture-skip-manifest.txt` (user-signed
> audit `docs/parity/failset-classification.md`; in-scope denominator 1652,
> promote gate = all 41 class-(a) standard-C failures fixed = ≥1608).
> Regression baseline `tmp/failset_lsq.txt` regenerated (55 lines);
> `docs/parity/torture-failset-current.txt` synced.
>
> **Previous (2026-06-11, `feature/eval-leftovers-claude`
> against `/workspace/mir` `develop` @ `2ffebff`):** fulltest is
> **557 passed, 0 failed, 0 timed out, 18 skipped** (make exit 0, both check
> gates GREEN). The eval-leftovers branch landed packages B (DSL string
> compares via strcmp lowering), A0 (MadValue/MadArray → one `madc::value`
> end-to-end), and A (declaration-only mangled-direct `<ns_madc>` +
> user-call-site scope capture): `testmadcevalexprctx` extended (string
> value compares) and `testmadcevalscope` un-skipped — 42/42/echo plus the
> typed-out forms — and `test_libmadc_program` is **97 passed / 38 skipped**
> (the four script-side scope-access categories join). `test_mangle` pins
> the madc::value class-param symbols (the substitution back-ref N..E wrap
> fix) and the text-carrier marshalling predicate. torture **1567/31/56/1**,
> failset byte-identical to the baseline; SMAUG boots (soak exit 124 +
> ready line).
>
> **Previous (2026-06-10, `develop` @ `7e84242`
> against `/workspace/mir` `develop` @ `2ffebff`):** fulltest was
> **555 passed, 0 failed, 0 timed out, 20 skipped** (make exit 0, both check
> gates GREEN) — **ALL integration reds are green.** New this cycle:
> `teststringplus_realhdr.mad` pins real-header `std::string a+b` — by-value
> FREE-operator returns bind the exported `_ZStpl…` mangled-direct and decl
> inits copy-elide into the declared variable (the former handoff-§4 wall #1
> SIGSEGV, `23027f7`) — and `testcstdio.mad` pins default-mode
> `#include <cstdio>` over the completed embedded stdio.h shim (`8a897f8`).
> torture 1567/31/56/1, failset byte-identical after both changes.
>
> **Previous (2026-06-10, `feature/cpp-detection-idiom-claude` @ `883c26e`):**
> fulltest **547/0/0/26** — the first all-greens run.
> `testfstream.mad` (the last red) passes rewritten to standard
> C++ through REAL libstdc++ headers (`.flags` `--std=c++17
> --no-embedded-headers` + `.expect`), executing `ofstream`/`ifstream` file
> I/O, `std::getline`, and — via the NEW namespace function-template BODY
> instantiation — real-header `std::to_string(42)` and `std::stoi(string)`
> (the non-exported `__gnu_cxx::__stoa` / `std::__detail::__to_chars_*`
> template bodies compile on demand). The session's chain
> (plan `docs/plans/2026-06-10-testfstream-alias-reference-plan.md`, all
> tasks done): alias-spelled reference returns (DataDefREF; `s[1]` deref +
> `&s[1]`), fn-template instantiation (explicit args, packs, fn-ptr
> deduction), overload-ranking fixes (`user_argc` default-arg poisoning,
> fn-to-pointer decay), `current_namespace` restore in qualified statements,
> fortify `__builtin___mem*_chk`, and `[namespace.udir]` unqualified-call
> fallback (POSIX `::getline` vs `std::getline`). Standalone gcc.c-torture is
> **1567 passed, 31 compile-failed, 56 runtime-failed, 1 timed out, 30
> skipped** — failset byte-identical across the whole session. SMAUG boots.

> **Previous (2026-06-09 late):** fulltest **546/1/0/26**; the one red was
> `testfstream.mad`. `testlargesizeofquery.mad` went green via the 64-bit
> `carray_dim_t` array-dim widening, `testdefer.mad` via defer execution on
> CIR, and `testloop.mad` earlier via real headers; `991014-1.c` newly passed
> in torture. Real-header C++ canaries: `testcout_realhdr`,
> `test_extern_polymorphic`, `cout << std::string`, `std::getline`, the
> `inf.good()` loop; `std::to_string`/`std::stoi` resolved (overload sets)
> but their bodies still hit the alias-reference wall.

> **Previous SIMD baseline (2026-06-06, `feature/simd-consume-claude` against `/workspace/mir`
> `develop` @ `2ffebff`, `MIR_COMMIT` bumped `8864a73`→`2ffebff`):** integration
> **515 passed, 4 failed, 1 timed out, 26 skipped** on the latest capped
> `make -C src fulltest` — a session arc of 486→515 (+29): +18 from the MIR pin
> bump (un-skipping tests that needed c2mir features the old pin lacked) and +11
> from the madc SIMD frontend lowering (`DataDefSIMD` → c2mir vector type; all
> `testgccvector*`/`testsimd*` now pass, including one-lane `__int128` and the
> 32B/64B wide vectors via c2mir's scalar-lane fallback). Earlier additions:
> `testheaderstringops.mad`, `testclasscopyretbuf.mad`, and
> `teststdcppinclude.mad`, plus `testforeachheaderbody.mad` for range-for
> locals in included/header function bodies, `testexternclinkage.mad` for
> `extern "C"` linkage specs, and `testexterncstringptr.mad` for
> typedef-preserved string-pointer extern C prototypes. Embedded polyglot
> namespace headers and `<algorithm>` helpers now route calls through generated
> wrappers over explicit `extern "C"` ABI declarations, with PHP string helpers
> now importing real `_ZN3php...` C++ namespace symbols and `__php_*` kept as C
> ABI convenience wrappers. `test_mangle` covers the GCC-backed nested namespace
> symbol shape plus namespace variable symbols such as `_ZSt3cin`. `testcin.mad`
> is recovered on CIR: `std::cin` binds to the real libstdc++ global and string
> extraction delegates to real C++ iostreams. The parser/PCH checkpoint also
> rejects stale embedded PCH blobs and keeps real-header parsing on generic
> type/alias/member machinery. The known red tests are
> `testdefer.mad`, `testfortypedcomma.mad` (historically flaky fail/timeout;
> classified as `TIMEOUT` in the aggregate run),
> `testfstream.mad`, `testlargesizeofquery.mad`, and
> `testloop.mad`.
> MIR SIMD side checkpoint `c69f4da` imports the remaining 21 exact GCC vector
> torture fixtures found by the vector-construct scan, so all 37 GCC execute
> tests mentioning vector constructs are checked in and pass under C2MIR `-ei`
> and `-eg`. This follows the `59117d8` checked `__builtin_copysignf` /
> `__builtin_nan` lowering, one-lane unsigned `__int128` vector equality,
> union-array alias, leading GNU vector-attribute, `__builtin_memcmp`, and
> narrow address-taken register rvalue checkpoints. MIR SIMD side checkpoint
> `55c65ee` adds text and binary `MIR_T_V128` data I/O round-trip coverage via
> `mir-tests/scan-test.c` and `mir-tests/io.c`. MIR SIMD side checkpoint
> `e4a8945` adds direct MIR and C frontend coverage for v128 lane-count shift
> opcodes (`vlshvi*`, `vrshvi*`, `vurshvi*`) across i8/i16/i32/i64 lanes.
> MIR SIMD side checkpoint `360fdb5` extends one-lane `__int128` vector
> lowering to non-div/mod arithmetic, bitwise, unary, comparison, shift,
> compound, and GCC inc/dec operators in `c-tests/new/vector-size.c`.
> MIR SIMD side checkpoint `2ffebff` closes the final known <=16-byte SIMD
> gap by lowering one-lane signed and unsigned `__int128` vector div/mod
> through helper-call imports, with saved MIR/BMIR resolver support and
> additional `c-tests/new/vector-size.c` coverage.
> `/workspace/mir` `timeout 900 make test` passed at `2ffebff` with
> interpreter/O0 **Tests 1121, Success tests 2242** and generated-mode
> **Tests 1125, Success tests 2250** plus bootstrap checks.
> The 419/0 figures below are the
> *removed* asmjit/MIR-transpiler backend and are retained only as the C89
> coverage target the CIR path is climbing back to. ★ Milestone: SMAUG 1.8
> boots, runs as a live server, and is playable (character creation, world
> navigation, the Newgate serpent fight) through `cir_node → c2mir → MIR → JIT`.
> Canonical live state: `develop` live git and fulltest output; compiler
> warnings are release-prep blockers and should be cleaned rather than ignored.
> The clean `make -C src` rebuild on 2026-06-05 emitted no compiler warnings.
> The older parity snapshots below are retained as historical context.

Test results as of May 28, 2026 (v0.24.0, GCC parity 1649/1685 = 97.9%, 475 integration tests, 294 unit tests).

MIR default backend: 419 passed, 0 failed, 56 skipped.
All 12 _Complex tests now pass via native c2mir _Complex support (13 commits).

Run with: `bin/madc tests/<name>.mad` or `make -C src fulltest`

Operational default: when work is clearly limited to core `madc` /
  `libmadc` / parser / compiler surfaces, prefer a workspace configured
with `./configure --enable-madcdat=no` so builds and unit validation
stay on the smaller core footprint. Re-enable `madcdat` before final
validation when storage/federation code or shared surfaces may be
affected.

## Current Batch Status — 472 JIT pass / 0 fail

Latest results (2026-05-24):

### JIT mode (`scripts/run_tests.sh`)
- Passing: 452 integration tests
- Failing: none
- Note: test count previously dropped from 542 to 274 because 316 scratch/reducer files were moved to `tmp/` (gitignored). Dedicated regressions for function-pointer arrays, statement-expression member access, and nested flat struct initializers now bring the tracked integration count to 277.
  Additional tracked regressions for GNU designated initializers,
  nested designated initializers, file-scope compound-literal global
  pointers, struct-copy compound literals, union compound literals,
  nested deref post-increment, the `20060420-1.c` global array
  pointer-cast loop, `_Complex` / `iF` compatibility via
  `testcomplexkw.mad`, and VLA-sized local struct members via
  `testvlastructmember.mad`, and indirect function-pointer array calls via
  `testfnptrarraycall.mad`, plus K&R varargs function-pointer calls via
  `testkrfnptrvarargs.mad`, plus `_Complex unsigned short` compatibility via
  `testcomplexushort.mad`, plus GNU computed goto via
  `testcomputedgoto.mad`, plus the `20050502-1.c` deref-postinc read
  shape via `testderefpostincread.mad`, plus GNU `vector_size`
  compound-literal coverage via `testgccvectorlit.mad`, now bring the
  tracked integration count to 297. Additional tracked regressions for
  typedef'd VLA `sizeof(type)` handling via `testtypedefvlasizeof.mad`
  and SIMD integer/float vector cast lane preservation via
  `testgccvectorcasts.mad` now bring the tracked integration count to
  306. Additional tracked regressions for typedef'd array pointer-
  subscript decay via `testtypedefarrayptrsubscript.mad`, struct-by-
  value call copies via `teststructbyvaluecallcopy.mad`, `__real`
  address-taking via `testcomplexrealaddr.mad`, typedef-enum bitfield
  extraction via `testenumbitfieldalias.mad`, and repeated nested-
  function inline-asm barriers via `testnestedasmbarrier.mad` now bring
  the tracked integration count to 315. Additional tracked regressions
  for pure-imaginary complex literals via `testcompleximagadd.mad`,
  builtin/`~` conjugation via `testcomplexconjop.mad` and
  `testbuiltinconjf.mad`, component-wise complex `+=` via
  `testcomplexaddeq.mad`, and old-style forward declarations with
  complex-typed later definitions via `testcomplexfwddeclparams.mad`
  now bring the tracked integration count to 320. Additional tracked
  regressions for complex fixed-array decay / pointer comparison via
  `testcomplexptrcmpdecay.mad` and unsigned complex compound division
  via `testcomplexunsigneddiveq.mad` now bring the tracked integration
  count to 322. An additional tracked regression for split-line complex
  declarations plus complex truthiness in `if (c = f())` via
  `testcomplexsplitdeclcond.mad` now brings the tracked integration
  count to 323. The latest tracked regression covers embedded standard-
  header auto-inclusion for names like `size_t`, `intptr_t`, and
  `DBL_MIN` via `testautoincludestdheaders.mad`. An additional tracked
  regression for function `__alignof__` / `__attribute__((aligned(N)))`
  coverage via `testfunctionalignof.mad` now brings the tracked
  integration count to 353. An additional tracked regression for
  `long long` ternary width preservation under casts via
  `testternaryllcast.mad` now brings the tracked integration count to
  354. An additional tracked regression for C integer-promotion rules
  on signed/unsigned bitfield arithmetic via `testbitfieldpromote.mad`
  now brings the tracked integration count to 355. An additional tracked
  regression for wide unsigned bitfield arithmetic result precision via
  `testbitfieldwidearith.mad` now brings the tracked integration count
  to 356. Additional tracked regressions for GNU `optimize` attributes
  containing `-fno-strict-aliasing`, GCC byte-swap builtins,
  `__builtin_setjmp` / `__builtin_longjmp`, and integer bit-operation
  builtins plus unsigned shift-result typing now bring the tracked
  integration count to 360. An additional tracked regression for fixed-array struct assignment via
  `testfixedarraystructcopy.mad` now brings the tracked integration
  count to 329. An additional tracked regression for
  `-finstrument-functions` plus `no_instrument_function` handling via
  `testfinstrumentfunctions.mad` now brings the tracked integration
  count to 330. Additional tracked regressions for contextual `struct`
  tag parsing via `teststructtrytag.mad` and float-return indirect
  function-pointer comparisons via `testfnptrfloatretcmp.mad` now bring
  the tracked integration count to 369. Additional tracked regressions for typedef'd struct
  array aliases via `testtypedefstructarrayalias.mad`, nested
  multidimensional VLA locals via `testmultidimvla.mad`, preserving
  `defined(...)` operands in `#if` via `testifdefdefinedoperand.mad`,
  integer wrap-before-widen casts via `testuint32wrapbeforecast.mad`,
  nested VLA parameter declarators via `testnestedvlaparam.mad`,
  fixed-array struct assignment via `testfixedarraystructcopy.mad`, and
  `-finstrument-functions` / `no_instrument_function` coverage via
  `testfinstrumentfunctions.mad` now bring the tracked integration
  count to 337. Additional tracked regressions for the builtin
  `strcmp` macro cycle via `testbuiltinstrcmpmacrocycle.mad`, signed
  bitfield assignment-expression extraction via
  `testsignedbitfieldassignexpr.mad`, native string-literal subscript
  global pointer initialization via `teststrlitaddrsubscriptglobal.mad`,
  and multidimensional struct-member array decay via
  `teststructmembermultidimdecay.mad` now bring the tracked integration
  count to 341. An additional tracked regression for odd-sized local
  struct array direct/varargs pass-by-value handling via
  `testsmallstructarraycall.mad` now brings the tracked integration
  count to 342. Additional tracked regressions from the subsequent GCC
  parity slices now bring the tracked integration count to 351,
  including unsigned-char pointer string-literal coercion via
  `testucharptrstringlit.mad`, empty-template inline asm `"+r"`
  operand evaluation via `testasmrwoperand.mad`, size_t-width
  `sizeof` / `alignof` tokens via `testlargesizeofquery.mad`, exact
  decimal-real lexing via `testhexfloatcompare.mad`, global
  alias-backed array storage identity via `testglobalaliasarray.mad`,
  and scalar alias write-through via `testglobalaliasscalar.mad`.

  An additional tracked regression for file-scope `-0.0` sign
  preservation via `testnegzerostatic.mad` now brings the tracked
  integration count to 370.

  Additional tracked regressions for GNU compound-literal field
  designators via `testcompoundlitgnudesignator.mad`,
  `__builtin_types_compatible_p(...)` via
  `testbuiltintypescompatible.mad`, `__builtin_prefetch(...)` side
  effects via `testbuiltinprefetcheffects.mad`, and unsigned 32-bit to
  real coercions via `testuint32realcoerce.mad` now bring the tracked
  integration count to 364. An additional tracked regression for
  pointer-arithmetic member-address expressions like
  `&((array + 1)->field)` via `testconstaddrexprarrow.mad` now brings
  the tracked integration count to 365. An additional tracked
  regression for IEEE NaN comparisons and builtin predicates via
  `testieeefpcompare.mad` now brings the tracked integration count to
  366. An additional tracked regression for IEEE huge-value, infinity,
  finite, and NaN builtins via `testieeehugeval.mad` now brings the
  tracked integration count to 367.
  Subsequent GCC builtin and stdio regression coverage, including
  `testbuiltinunsignedabs.mad`, `testmacrovariadicfixedargs.mad`,
  `testconstptrarrayderef.mad`, and
  `teststdiobuiltinredirects.mad`, now brings the tracked integration
  count to 376.
  Additional tracked regressions for fixed-array pointer arithmetic over
  pointer elements via `teststrpbrklocal.mad` and postfix `++` / `--`
  before bitwise `&` via `testpostincbitand.mad` now bring the tracked
  integration count to 378. An additional tracked regression for GCC
  limit macros `__PTRDIFF_MAX__` and `__SIZE_MAX__` via
  `testgcclimitmacros.mad` now brings the tracked integration count to
  379.
  The std-surface cleanup updated legacy snippets to import std names
  explicitly and added `teststdstringconv.mad` for direct
  `std::string` / `std::to_string` / `std::stoi` / `std::stod`
  coverage, bringing the tracked integration count to 405. Additional
  focused regressions for volatile token-paste preservation, nested
  packed struct members, nested variadic function calls, multi-level
  pointer dereference chains, and output-only inline-asm operands bring
  the tracked integration count to 410.
  Additional focused regressions for unsigned division/modulo natural
  arithmetic type, narrow bitwise assignment, unsigned integer-to-real
  conversion, cast-precedence around unary dereference, small integer
  SIMD relational compares, SIMD scalar arithmetic splats, and explicit
  scalar-to-SIMD bitcast casts now bring the tracked integration count
  to 434.

### GCC torture sweep (`scripts/run_gcc_testsuite.py`)
- Passing: 1569 / 1685 (93.1%)
- Compile-failed: 38
- Runtime-failed: 48
- Timed out: 0
- Skipped: 30
- Current front edge: `gcc_testsuite/gcc.c-torture/execute/pr122000.c`

### Native EXE mode (`scripts/run_tests.sh --exe`)
- Passing: 434 (of 434 JIT-passing tests)
- Failing: none
- Requires: `sudo make -C src install-libmadc` and
  `LD_LIBRARY_PATH=/usr/local/lib` for libmadc.so
- The native EXE parity lane is currently fully green.
- File-scope compound literals that feed global pointer initializers now
  also relocate correctly in the EXE/AOT lane.
- The new `_Complex` arithmetic / conjugation regressions are green in
  native EXE mode too.
- A fresh `smaug.exe` probe also now survives the room 109 serpent fight
  and serpent death on the standalone executable path.

### Unit tests
- 80 datadef + 24 IR + 133 libmadc_program + 5 libmadc_error + 19 libmadc_value (261 total)
- Installed-library smoke: `make -C src libmadc-smoke` passes, staging
  `libmadc.so` plus public headers under `/tmp/madc-libstage/usr/local/`
  and then compiling/running both `tests/libmadc_cpp_smoke.cpp` and
  `tests/libmadc_c_smoke.c` against that staged install.

The latest IR-focused validation batch passes directly, including:

- `testassignexprmem.mad`
- `testcompoundassignmem.mad`
- `testderefarray.mad`
- `testassign.mad`
- `testassigninexpr.mad`
- `testc23_bool.mad`
- `testcin.mad`
- `testfnptrtypedef.mad`
- `testint.mad`
- `testpostfix.mad`
- `test_ptr_fn_deref.mad`
- `test_get_argv_deref.mad`
- `test_errno_deref.mad`
- `testfnptrmemberarrow.mad`
- `testglobalptrarrayarrow.mad`
- `testmapidentifier.mad`
- `testderefparenarrow.mad`
- `testfnptrcast.mad`
- `testcaseconstexpr.mad`
- `testneginit.mad`
- `testdupliteral.mad`
- `testderefmember.mad`
- `testdirtype.mad`
- `testternaryvalue.mad`
- `testternarystring.mad`
- `testsizeofexpr.mad`
- `testarrayc.mad`
- `testcompoundnarrow.mad`
- `teststringcast.mad`
- `teststrcmpret.mad`
- `teststrcharptrarr.mad`
- `testptrarith.mad`
- `testdoublestore.mad`
- `testdoublecompound.mad`
- `teststrarrinit.mad`
- `testsigneddiv.mad`
- `teststructptrsub.mad`
- `testfloat.mad`
- `testintsuffix.mad`
- `testdoubleptr.mad`
- `testderefeq.mad`
- `testderefcmp.mad`
- `teststructdoublecompound.mad`
- `testdoubleptrwrite.mad`
- `testfloatvarargs.mad`
- `testderefpostincstore.mad`
- `teststructcopy.mad`
- `testparenderefmember.mad`
- `testleadingdotfloat.mad`
- `testsubscriptexprmember.mad`
- `teststructarrsub.mad`
- `testrealconstfold.mad`
- `testclassident.mad`
- `testreturnnextident.mad`
- `testcompoundsubexpr.mad`
- `testnegbraceInit.mad`
- `testcharnoterm.mad`
- `testgoto.mad`
- `testmadcevalscope.mad`

## Passing Tests — 185 integration (latest batch)

`scripts/run_tests.sh` drives `testcin.mad` with piped stdin (`Alice 42
hello world`) and `testargv.mad` with argv (`hello world`), asserting
on their output instead of skipping. The runner now also reports
`TIMEOUT: tests/...` explicitly when `timeout 5` kills a spinning test,
instead of collapsing that case into a generic `FAIL`.

### New post-v0.8.0 (SMAUG Phase F regressions — hashstr.mad runs)

| Test | What it tests |
|------|--------------|
| `testincmember.mad` | Prefix/postfix inc/dec on struct members (`++ptr->links`, `obj.f--`), including if-guarded for the size-aware load/store path |
| `testunsignedcmp.mad` | Unsigned comparisons in if-conditions (setb/seta path) for short and int |
| `testglobalptr.mad` | Global pointer variable read/assign (DataDefPTR qword overrides) |
| `testsubtomember.mad` | `p->next = arr[i]` — subscript result into a struct member Mem |
| `testcastargcomma.mad` | Cast+arith as first call arg with a following comma, e.g. `strcpy((char *)h+8, "x")` |
| `testcommaincrement.mad` | `for (...; ptr = ptr->next, c++)` — SMAUG's comma-increment pattern |
| `testpostdeclstr.mad` | `char *p; p = "literal";` and `r->name = "literal";` |
| `testcoutcstr.mad` | Chained `cout << char*` output, including function-returned `char*` and mixed string-prefix chains |
| `testdeclassignexpr.mad` | Assignment as an expression inside declaration initializers (`int y = (x = 42)`) |
| `testprintfmember.mad` | Varargs wrapper calls with `->` member arguments, macro-expanded nested members, and plain `printf` mixes |
| `testprintfdouble.mad` | `%f` / `%e` / `%g` formatting through direct `printf` and `...` wrappers, including mixed args and multiple doubles |
| `testsmaug_requests.mad` | Upstream SMAUG `requests.c` compatibility test with a minimal `mud.h` shim and embedded POSIX/C headers |
| `testc23_bool.mad` | C `_Bool` keyword aliasing to madc's bool type, including scalar and fixed-array initialization |
| `teststaticassert.mad` | `_Static_assert` / `static_assert` with arithmetic, `sizeof`, and `alignof` constant expressions |
| `testalignof.mad` | `alignof` / `_Alignof` on primitive, pointer, struct, array, and member expressions |
| `testtypeof.mad` | `typeof(expr)` / `typeof(type)` driving global and local declarations |
| `testnullptr.mad` | Typed `nullptr` literal in pointer declarations and boolean tests |
| `testdigitsep.mad` | C23 digit separators in decimal, hex, binary, and floating literals |
| `testbinlit.mad` | C23-style binary integer literals (`0b...` / `0B...`) in assignments, expressions, and conditions |
| `testrestrict.mad` | `restrict` as a parsed no-op qualifier in pointer declarations and function parameters |
| `testflock.mad` | Embedded `<sys/file.h>` and `flock()`/`LOCK_*` constants via dlsym fallback |
| `testincludeonce.mad` | `#include` include-once behavior for repeated local includes within a single compile |
| `testassigninexpr.mad` | Assignment expressions used in `while` / `if` conditions and chained assignment value flow |
| `testassignexprmem.mad` | Stack-local Mem destinations on plain arithmetic / `%` expressions |
| `testcompoundassignmem.mad` | Stack-local compound assignment with Mem-backed LHS (`*=`, then `+=`) |
| `testderefarray.mad` | Unary `*` on fixed arrays (`!*buf`, `*word`) via array-to-pointer decay |
| `test_ptr_fn_deref.mad` | Dereference of a user-function `char *` return (`*get_msg()`) |
| `test_get_argv_deref.mad` | Dereference of a method-call `char *` return (`*(version.c_str())`) |
| `test_errno_deref.mad` | Dereference of builtin/external pointer-return path via `errno` / `__errno_location()` |

### New in Phase E / F session

| Test | What it tests |
|------|--------------|
| `testchain.mad` | Chained `->` and `.` member access (a->b->c, a->b.c, a.b.c) |
| `testfixedarr.mad` | C fixed-size arrays (1D + multi-dim), brace init, char* init, string-literal init |
| `teststructinit.mad` | Struct initializer lists and array-of-structs init |
| `teststructinterop.mad` | struct tm, struct timeval, struct fd_set + FD_* macros, select() |
| `testfileline.mad` | `__FILE__` / `__LINE__` predefined macros, including inside function-like macros |

### New tests added in this session

| Test | What it tests |
|------|--------------|
| `testcompoundassign.mad` | All 10 compound assignment operators (+=, -=, *=, etc.) |
| `testfortypedcomma.mad` | Typed `for` initializer with comma-separated declarations (`for (int i = 0, j = 10; ...)`) |
| `testhex.mad` | Hex integer literals (0xFF, 0xDEAD, 0X1A) |
| `testpostfix.mad` | Postfix x++/x-- with old-value-return semantics, including `for` and `while (x--)` |
| `testdefine.mad` | #define, #undef, #ifdef, #ifndef, #if, #elif, #else, #endif |
| `testlibc.mad` | dlsym fallback: getpid(), sleep(), getuid(), getppid() |
| `testmathh.mad` | #include <math.h>: M_PI, sqrt, floor, ceil, fabs, pow, sin, cos |
| `testargv.mad` | int main(int argc, char **argv) — requires cmd args (manual) |
| `teststruct3.mad` | C ABI alignment, __attribute__((packed)), mixed field sizes |
| `testsizeof.mad` | sizeof(type), sizeof(struct), sizeof in expressions |
| `testshadowlocalglobal.mad` | A local variable may shadow a same-named global without rebinding later codegen to the global slot |
| `testparamshadowglobalcharptr.mad` | A `char *` parameter may shadow a same-named global pointer without aliasing the global |
| `testaotdamageextern.mad` | AOT/native executable path keeps function-scope global pointer-table lookups stable across repeated branches, matching SMAUG `damage()`-style access |
| `testaotexternarray.mad` | AOT/native executable path preserves global struct-array layout and function-scope extern access across repeated lookups |
| `testaotsysdataextern.mad` | AOT/native executable path preserves `sysdata`-style extern struct storage and member reads across branchy control flow |
| `testbugbufbranch.mad` | Branch-skipped buffer write followed by later `strcpy`/`strcat` on the same stack buffer |
| `testsetcharcolor_noprint.mad` | Simplified `set_char_color()` `sprintf` formatting path across two color modes |
| `testsprintf4str.mad` | `sprintf` with four `%s` arguments in one formatting call |
| `testtypedefptrmemberchain.mad` | Typedef-backed pointer-member chain through `pcdata->learned[idx]` |
| `testtypedefptrmemberchain_smaugshape.mad` | Deeper SMAUG-shaped typedef pointer-member chain through a larger `CHAR_DATA` layout |
| `testvariadicterstrtwice.mad` | Repeated ternary string arguments inside a variadic `sprintf` call evaluate once per live branch |

### Notes

- `testcin.mad` is driven by `scripts/run_tests.sh` with piped stdin
- `testargv.mad` is driven by `scripts/run_tests.sh` with argv
- `include_helper.mad` is not standalone (included by testinclude.mad)
- `include_once_helper.mah` is not standalone (included by testincludeonce.mad)
- All tests that use `cout`/`cin`/`cerr`/`endl` now require
  `#include <iostream>` plus explicit `std::` qualification or `using`
  import.

## Previously Passing Tests — 54/54 integration + 25/25 unit

| Test | What it tests | Output |
|------|--------------|--------|
| `test.mad` | String variable, puts() | Prints string |
| `test2.mad` | Large loop (100M iterations) | `100000000` |
| `test3.mad` | Basic program structure | Runs silently |
| `test4.mad` | Char literals, putchar(), user-defined string funcs | `Hello, World!`, `hi`, `test`, `Hello, World!`, `HEY`, `hey 123`, `v0.0.1` |
| `test5.mad` | String ops | `Hello, World!`, `hi` |
| `testassign.mad` | Variable assignment | `456` |
| `testbsl.mad` | Bit shift operators (`<<` and `>>`) | `200`, `40`, `16`, `15` |
| `testcout.mad` | cout stream output | `This is a test, x = -1` |
| `testfor.mad` | For loop | `a == 5` |
| `testfunc.mad` | User-defined functions | `10`, `15` |
| `testif.mad` | If/else | `this is a test` |
| `testif2.mad` | If with integer condition | `1` |
| `testinc.mad` | Increment/decrement | `1`, `0` |
| `testint.mad` | Integer types, assignment | `123: 123`, `i: 456`, `j: 456` |
| `testlocal.mad` | Local string variable | `Hello, World!` |
| `testmath.mad` | Integer arithmetic | `0`, `-2` |
| `testmath2.mad` | More arithmetic | `15`, `5` |
| `testnot.mad` | Bitwise NOT | `-2`, `-1` |
| `testprint.mad` | String print | `Hello, World!` |
| `testreturn.mad` | Function return values | `100`, `101` |
| `testsstream.mad` | Stringstream | `456`, `123`, `5`, stream content, `This is a test to cout: 5` |
| `teststruct.mad` | Struct member access | `test.name: Joe Blow`, `test.id: 2`, `test.age: d` (uint8=char in stream) |
| `testversion.mad` | Version string | `v0.0.1` |
| `testns.mad` | Namespace resolution (std::) | `Hello from std::cout!`, `x = 42`, stderr output, `using namespace std` imports unqualified stream names |
| `testphp.mad` | php:: namespace functions | trim/ltrim/rtrim, ucfirst/lcfirst, str_replace, str_repeat, explode/implode, sort, nested-array `array_column` |
| `teststruct2.mad` | User-defined structs | `p.x: 10`, `p.y: 20`, `bob.name: Bob Smith`, `bob.age: 42`, `bob.id: 1001` |
| `testclass.mad` | Class definitions with data members | `p.x: 100`, `p.y: 200`, `bob.name: Bob`, `bob.age: 30` |
| `testinclude.mad` | `#include` directive | `Hello, World!`, `Hello, Mad-C!`, `include works!` |
| `testusing.mad` | `using namespace std` | `using namespace std works!` |
| `testwhile.mad` | While loop | `100000000` |
| `testcapture.mad` | Lambda capture of outer variables | Captured values printed |
| `testcin.mad` | `cin >>` input from stdin | Reads and echoes user input (needs stdin) |
| `testcolon.mad` | `:=` short variable declaration (Go-style type inference) | Inferred-type variables |
| `testdefer.mad` | `defer` statement (Go-style deferred execution) | Deferred output at scope exit |
| `testdlcall.mad` | `dlcall()` through function pointer | Calls C library function via pointer |
| `testdlopen.mad` | `dlopen`/`dlsym`/`dlclose` | Loads and calls shared library symbols |
| `testescape.mad` | Escape sequences in string literals (`\n`, `\t`, etc.) | Formatted output with escapes |
| `testforeach.mad` | Range-based `for (type var : array)` | Iterates over MadArray elements |
| `testforeach2.mad` | Range-based for with STL containers | Iterates over vector/map/set |
| `testfstream.mad` | File I/O with ifstream/ofstream/fstream | Read/write file operations |
| `testfuncptr.mad` | Function pointers via `auto fn = func` | Calls through stored function pointer |
| `testlambda.mad` | Lambda expressions with `auto` and `[]` | Defines and calls inline lambdas |
| `testlang.mad` | Multi-language namespace usage in one program | php/perl/python/ruby/js functions together, including `ruby::chars` |
| `testloop.mad` | Loop constructs (for, while, do-while) | Various loop patterns |
| `testmadc_ns.mad` | `madc::` namespace (regex, array) | madc::regex_match, regex_search, regex_replace |
| `testmap.mad` | `map<K,V>` typed STL container | Insert, find, erase, iterate |
| `testmethod.mad` | Class methods with `this` pointer | Method call compiles and dispatches |
| `testmultiret.mad` | Multiple return values (Go-style) | Function returns multiple values via `__retbuf`; runtime output asserted via `.expect` |
| `testprefer.mad` | Namespace precedence directives | `prefer rust, c;` and `#pragma prefer rust, c` change bare identifier lookup order |
| `testrust.mad` | rust:: namespace helpers | trim/contains/replace, split/join, first/last/get, push/pop |
| `testrubycharsshadow.mad` | Namespace-call argument shadowing | `ruby::chars(chars, s)` resolves local arg, not namespace function |
| `testperl.mad` | perl:: namespace functions | chop, chomp, split, join, grep, glob |
| `testregex.mad` | Regex functions (match, search, replace) | Pattern matching and substitution |
| `testset.mad` | `set<T>` typed STL container | Insert, find, erase, iterate |
| `testsubscript.mad` | `[]` subscript operator on strings and containers | Indexed access |
| `testswitch.mad` | `switch`/`case`/`default` statement | Branch selection by value |
| `testternary.mad` | Ternary operator (`cond ? a : b`) | Conditional expression |
| `testvector.mad` | `vector<T>` typed STL container | push_back, size, at, iterate |

## Phase 1 Fixes Applied

| Fix | Status | Details |
|-----|--------|---------|
| `-v/--verbose` flag | ✓ Done | `DBG()` macro gated on `madc_verbose`; parse `-v`/`--verbose` in `main()` |
| Char literal compile | ✓ Done | Added `TokenChar::compile()` and `operand()`; `case ttChar:` in `TokenBase::compile()` |
| Error reporting | ✓ Done | `throwbuf::sync()` prints before throwing; catch block was correct |
| Struct member access | ✓ Done | Fixed `addOffset` vs `setOffset`; load numeric members into Gp; LEA for string members; construct/destruct string members in struct |
| `register` keyword | ✓ Done | Added `vfREGISTER` flag, `TokenREGISTER` token, parsed in `TokenREGISTER::parse()` |
| doctest framework | ✓ Done | `include/doctest.h`, `tests/unit/test_datadef.cpp` (25 tests), `make test` |

## Unit Tests

Run with: `make -C src test`

| Test File | Tests | Status |
|-----------|-------|--------|
| `tests/unit/test_datadef.cpp` | 25 | All pass |
| `tests/unit/test_ir.cpp`      | 23 | All pass — IR Stage 0 scaffolding + Stage 1/2 coerce coverage |

## Phase 2 Fixes Applied

| Fix | Status | Details |
|-----|--------|---------|
| String parameter pass-by-ref | ✓ Done | `voperand()` creates bare Gp for `vfPARAM` non-numeric vars; `cleanup()` skips param destruction |
| `dtSTRING → dtCHARptr` coercion | ✓ Done | `string_cstr()` helper auto-converts string args to `const char*` when calling `puts()` etc. |
| User-defined structs (2.1) | ✓ Done | `TokenSTRUCT::parse()` parses `struct Name { type member; ... };`, builds `DataDefSTRUCT` dynamically, registers in `struct_map` |
| Namespace resolution (2.3+2.4) | ✓ Done | `namespace_map` registry, `::` resolution in `parseExpression()`, `std::` namespace with cout/cerr/endl |
| `#include` + `using` (2.5) | ✓ Done | Lexer handles `#include "file.mad"` with relative paths; parser handles `using namespace X;` and `using X::member;` |
| Class definitions (2.2) | ✓ Done | Data members, `class Name { ... };` syntax, type registered in `datatype_map` for prefix-free use |

## Known Issues

- String pass-by-value is implemented as pass-by-reference (caller's string is shared, not copied)
- No true string copy semantics yet for function parameters
