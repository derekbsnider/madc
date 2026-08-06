# Test Status

> **Current (2026-08-06, `feature/libcxx-parity7-claude` @429842b4 —
> session #66 close):** fulltest **997/0/9skip**; lane
> **993/0/13skip**; session-end native legs EXE **976/0**, OBJ
> **976/0** (of 997 JIT-passing tests). Battery log:
> `tmp/logs/rb-20260806-194726.log` (fulltest, libcxx jit, exe, obj
> all rc=0). +13 fulltest tests over session #65: the multi-return
> struct-transport gates (testmultiret double/ptr/string/hetero +
> reject/exprpos/bare — class values, Go-style heterogeneous
> `(int, string) f()` signatures, loud rejects; @a369cb17), the
> zero-ceremony gates (testautoincludecpp/ns, testpreferdefault,
> testautoceremonystd, testnsheaderfirst; @1fafe265 @0bc6ce07
> @715fbadb), and testsizeofvaluepack (@b411715c). Also @429842b4:
> the enum-constant parse-time slot heap overflow fixed
> (Variable::slot_size owns the 64-bit ddINT slot contract;
> valgrind-verified). Doc-example harness: **53/53** fenced examples
> green at this HEAD. Ships as **v0.68.0**.
>
> **Previous (2026-08-06, `feature/libcxx-parity7-claude` @520e77d6 —
> session #65 close: 🏁 LANE ZERO):** fulltest **984/0**; lane
> **980/0/13skip** — the `-stdlib=libc++` flavored lane's failing set
> is **EMPTY** for the first time: full behavior-parity with the
> default libstdc++ lane. testtuple (the #110 pack wall's last
> standing test) FIXED; the other +4 passes are the four new session
> gates (testctortemplatetrait, testusingfnoverload,
> testexternblockbody, testctorttpdefault). Four fixes: (30)
> `trait_class_constructible` no longer refuses same-class
> constructibility when the ctor set contains a TEMPLATE (the -1
> refusal laundered through a failed static-const capture into a
> silent 0 — `is_move_constructible<allocator<T>>` folded false);
> (31) a using-declared function JOINS the target namespace's
> overload set ([namespace.udecl] — `std::swap(int,int)` with
> `<memory>` bound the exception_ptr overload); (32) extern
> linkage-block context no longer leaks into function bodies
> ([dcl.link]p7 — "__tmp in block scope with external linkage");
> (33) THE WALL: a template-template parameter defaulted to a
> DIFFERENT named template binds as a template NAME ([temp.param]p11
> — libc++ tuple()'s `_IsDefault = is_default_constructible` idiom
> captured as a never-foldable non-type default, so the ctor never
> instantiated and construction called the never-defined
> placeholder). The 13 lane skips = the 9 baseline `.mir_skip` + the
> 4 documented `.libcxx_skip`. `docs/parity/libcxx-failset.txt`
> records the ZERO and becomes the P2.7 gate per its charter.
> Battery log: `tmp/logs/rb-20260806-145634.log` (fulltest rc=0,
> libcxx jit rc=0, total rc=0). Session-end native legs GREEN: EXE
> **967/0**, OBJ **967/0** (of 984 JIT-passing tests;
> `tmp/logs/rb-20260806-151939.log`, total rc=0).
>
> **Previous (2026-08-06, `feature/libcxx-parity7-claude` @01e0e7d7 —
> session #64 close):** fulltest **980/0**; lane **975/1/13skip**
> (+5 gates: testaggrdecl, teststructbraceexpr, testint128global,
> testemptystructret, testcomplexretconv, testcastcallpostfix — the
> last five landed after the 970/1 checkpoint). The only remaining lane
> failure is **testtuple** (#110 pack wall). Six fixes: (24) DECL-lane
> braced aggregate init of object-member aggregates; (25) `P{7, 3.5}`
> braced functional construction of plain structs parses
> (parse_compound_struct_lit — one brace reader for `(T){...}` and
> `T{...}`); (26) const `__int128` file-scope initializers (fork:
> gen_initializer int128 data arm — was the pack-freeze SEGV);
> (27) empty-struct call results reserve a real call-arg slot (fork:
> "undeclared func reg fp" at pack-thaw); (28) `_Complex` return-value
> conversion (fork: `return 3.0;` loaded components from absolute
> address 0); (29) cast operands continue the postfix chain
> (`(int)getb().n`). Follow-ons recorded: swap<allocator> return-type
> mistyping (tsubst), duration<double>::operator%= drain
> instantiation (pack gate is check-only), dependent-decltype
> pattern-freeze. Session-end native legs GREEN: EXE **963/0** and OBJ
> **963/0** (of 980 JIT-passing tests).
>
> **Previous (2026-08-06, `feature/libcxx-parity7-claude` @8f8f4009 —
> DECL-lane braced aggregate init):** fulltest **975/0**; lane
> **970/1/13skip** (+1 gate: testaggrdecl). The only remaining lane
> failure is **testtuple** (#110 pack wall). Residual (a) resolved:
> braced aggregate init of OBJECT-member aggregates in the DECLARATION
> lanes — var_decl's C INIT list bit-copied the class member and ordered
> the materialized temp's decl after the SPEC_DECL ("undeclared
> identifier __madc_objtmp_0"), and the decl copy-elision arm
> `S v = S{a, b}` silently DROPPED the full list (garbage, exit 0).
> Storage stays bare (braced_aggregate_needs_construction); the three
> FULL-list declaration sites claim via decl_aggregate_claim →
> class_aggregate_init; multi-element aggregate-shaped declines fail
> loud.
>
> **Previous (2026-08-06, `feature/libcxx-parity7-claude` @e658a5b8 —
> testfreezerun FLIPPED):** fulltest **974/0**; lane **969/1/13skip**
> (+1 gate: testaggrinit). The only remaining lane failure is
> **testtuple** (#110 pack wall). Four fixes: the flavor-runtime dlopen
> moved into `cir_translate_guarded` (the freeze lane's CIR-time dlsym
> probes shaped a different tree — facet-id externs unrecorded); frozen
> containers carry the flavor `link_libs` and the thaw reopens them (16
> trap-bound imports → 0); nested-class ctor/dtor no longer false-match
> the owner's out-of-line defs (sentry's Itanium bind restored); and
> aggregate list-init of ctor-less classes stops DROPPING initializers
> (class_aggregate_init, [dcl.init.aggr] — was silent garbage in the
> PLAIN lane too, S{string,42} printed junk with exit 0).
>
> **Previous (2026-08-06, `feature/libcxx-parity7-claude` @588d9e73 —
> one-key fix for typedef'd anon-aggregate template args):** fulltest
> **973/0**; lane **967/2/13skip** (+1 gate: testanontypedefspec). The
> fix removed a SILENT wrong value in plain JIT (explicit spec invisible
> behind a typedef'd anon-struct key — 0 for 7, exit 0) and pushed the
> testfreezerun libc++ frontier from the ClassPattern-base error to
> thaw-time static-member facet imports (num_put/ctype `id`). Remaining
> 2: testfreezerun, testtuple.
>
> **Previous (2026-08-06, `feature/libcxx-parity7-claude` @4e1a4004 —
> testsysobject FLIPPED):** fulltest **970/0**; lane **966/2/13skip**
> (+2 gates: testfriendnonmember, testfreeoptemplate). Two fixes: a
> class-body FRIEND template never registers as a MEMBER ([class.friend] —
> libc++ string:1762's `bool friend operator==` poisoned
> `method_map["operator=="]` with a basic_string return, so
> `string == "lit"` typed as basic_string and `cout <<` bound the string
> inserter over a bool rvalue), and GLOBAL-scope free operator templates
> bind (retained-body key walk dropped the exact `"::operatorX"` key,
> `<=` vs the sibling walk's `<`; plain C struct operands now engage the
> lowering via operand_value_datadef + DataDefSTRUCT). Remaining 2:
> testfreezerun, testtuple.
>
> **Previous (2026-08-06, `feature/libcxx-parity7-claude` @022cbb3b —
> testmathheader FLIPPED):** fulltest **968/0**; lane **963/3/13skip**
> (+2 gates: testcastmembertype, teststaticoverload). Two fixes:
> qualified member-TYPE casts (`(typename __promote<T>::type)x` — the
> __math::isinf undefined-import root) and non-template static overload
> ranking by argument types + [expr]/5 argument reference-collapse (the
> silent `__promote<double>::type == long double` wrong value). Residual
> filed: dependent-decltype pattern-freeze (tmp/r58b.mad). Remaining 3:
> testfreezerun, testsysobject, testtuple.
>
> **Previous (2026-08-05, `feature/libcxx-parity7-claude` @bd6fed08 —
> testifconstexpr FLIPPED; `<format>` chain THROUGH):** fulltest **968/0**;
> lane **960/4/13skip** (+5 gates: testnsdmineg, testenumqualcase,
> testinlinenstype, teststaticbraceinit, testvartemplatefold). Five fixes:
> NSDMI isolated-parse context reset, ns-qualified scoped-enum case labels,
> inline-namespace type descent, static brace-or-equal-init brace form
> (the flip), ns-qualified variable-template constant fold (transactional
> `fold_constant_qualified_member` + `inline_namespace_descendants`
> consolidation). testinvocable reclassified `.libcxx_skip` (clang++
> -stdlib=libc++ rejects its libstdc++-internal `__is_invocable` source).
> Remaining 4: testfreezerun, testmathheader, testsysobject, testtuple.
>
> **Previous (2026-08-05, `feature/libcxx-parity7-claude` @1de7b430 —
> fixes 8-11; parser_std_format_spec.h open to :339):** fulltest **963/0**;
> lane **954/6** byte-identical failset (+4 gates: testparenctor,
> testanonbitfield, testenumsize, testtraitcopyable); enum fixed bases now
> drive layout ([dcl.enum]p8, freeze-carried); EXE **938/0**, OBJ **938/0**
> (session-end legs, of 954 JIT-passing).
>
> **Previous (2026-08-05, `feature/libcxx-parity7-claude` @aaee9009 —
> seven front-end fixes; unicode.h through):** fulltest **959/0** (rc=0,
> forest oracles green). Seven oracle-verified fixes advanced
> testifconstexpr's chain five links (ranges_construct_at.h:94 →
> buffer.h:62 → unicode.h:51/:70/:302 → parser_std_format_spec.h:58):
> nested-name-specifier head vs the auto fn-ptr shortcut, concept-headed
> template params, braced NSDMI, enum trailing declarator, bit-field
> brace-init skip, u/U/u8 literal prefixes + UCNs, and the scoped-enum
> pseudo-namespace bridge for using-declarations. The flavored
> measurement is **950 passed / 6 failed / 0 timed out / 12 skipped**
> (byte-identical failing set at every batch checkpoint; +6 = the new
> gates testnsfncollide, testconceptparam, testbracensdmi, testenumdecl,
> testbitfieldinit, testcharlit, testusingenum), eligible EXE **934/0**,
> OBJ **934/0**. NEW TEST PROTOCOL (owner): per fix targeted globs + one
> frontier test; per batch fulltest + `libcxxjit`; EXE/OBJ legs at
> session end.
>
> **Previous (2026-08-05, `feature/libcxx-parity7-claude` @179d1ab0 —
> C++20 abbreviated function templates, member form):** fulltest green
> (rc=0) and default EXE leg green. [dcl.fct]/18 lands as a token-level
> desugar: `auto` parameter placeholders become invented identifiers
> under a synthesized `template<...>` head, so the member-template
> capture + tsubst own the rest. libc++'s `dangling(auto&&...)`
> (testifconstexpr's first blocker) is THROUGH; the chain moved to
> ranges_construct_at.h:94, so zero flips: the whole flavored
> measurement is **944 passed / 6 failed / 0 timed out / 12 skipped**
> (byte-identical failing set; +2 = gates testbarestring +
> testabbrevtpl), eligible EXE **928/0**, OBJ **928/0**. New gate
> `testabbrevtpl` (pack ctor + bodied auto ctor, both oracles, both
> flavors).
>
> **Previous (2026-08-05, `feature/libcxx-parity7-claude` @bb435bfd —
> namespace-scope using-aliases flat-register when free):** fulltest
> **950 passed, 0 failed, 0 timed out, 9 skipped** (rc=0) and default EXE
> leg green (freeze/forest gates included). The dialect's unqualified
> visibility for namespace-scope type names is a flat `datatype_map`
> write that only the TYPEDEF lane performed; the USING-ALIAS lane was
> cut from the flat map after `std::pmr::string`'s alias clobbered the
> real `string`. libc++ spells `std::string` as a using-alias
> (`__fwd/string.h`) where libstdc++ uses a typedef, so bare `string`
> was unresolvable in every declaration context only under
> `-stdlib=libc++`. The alias arm now flat-registers only when the name
> is FREE (primary wins; pmr stays namespace-only). testexterncstringptr
> and testforeachheaderbody flip: the whole flavored measurement is
> **942 passed / 6 failed / 0 timed out / 12 skipped**, eligible EXE
> **926/0**, OBJ **926/0**, zero newly broken (two-way name diff). New
> gate `testbarestring` (file-scope var + fn decl/def + block local,
> both flavors).
>
> **Previous (2026-08-05, `feature/libcxx-parity7-claude` @075c7f81 —
> system-header global C++ overloads register distinctly):** fulltest
> **950 passed, 0 failed, 0 timed out, 9 skipped** (rc=0) and default EXE
> leg green. libc++'s `stdlib.h` declares five inline C++ `abs` overloads
> at GLOBAL scope after glibc's extern-C `int abs(int)`; plain globals
> were excluded from the tracked-overload arm, so all five spliced into
> one shared-id FuncDef and the last body (long double, `fabsl`) emitted
> as a plain-named linkonce `abs` clobbering the libc import — `abs(-7)`
> silently returned 0 under `-stdlib=libc++` (testincludenext "42 0" vs
> oracle "42 7"). A system-header plain global C++ function whose name is
> already taken now joins the per-overload model; first/solo declarations
> keep the source name (dlsym imports intact). testincludenext flips: the
> whole flavored measurement is **939 passed / 8 failed / 0 timed out /
> 12 skipped**, eligible EXE **923/0**, OBJ **923/0**, zero newly broken
> (two-way name diff). New gate `testglobaloverload` (abs/labs values
> against both oracles, validated in default JIT + libc++ JIT + EXE).
>
> **Previous (2026-08-05, `feature/libcxx-parity7-claude` @01d774fe —
> transitive secondary vtable groups):** fulltest **950 passed, 0 failed,
> 0 timed out, 9 skipped** (rc=0, all forest gates green). Itanium gives
> every polymorphic base subobject off the primary chain its own vtable
> group + ctor vptr stamp — TRANSITIVELY; madc collected only direct
> non-primary bases, so `E : D` (D : A, B) left the B-subobject on B's
> standalone vtable with wrong vbase-offset slots. Under libc++ that was
> stringstream (`basic_ostream` = `basic_iostream`'s second base): the
> virtual `basic_ios` resolved at +24 vs clang's +128 through any
> `basic_ostream` view — real libc++ code and emitted bodies read an
> uninitialized `basic_ios`, every insert silently lost, `<< 42` SIGSEGV
> in the locale copy ctor. testsstream and testopinherit flip: the whole
> flavored measurement is **938 passed / 9 failed / 0 timed out /
> 12 skipped**, eligible EXE **922/0**, zero newly broken (two-way name
> diff). New gate `testtranssecondary` (plain depth-2 + template stream
> shapes, both oracles, both flavors).
>
> **Previous (2026-08-05, `feature/libcxx-parity7-claude` @41cbb2c5 — the
> bucket-A chain):** fulltest **949 passed, 0 failed, 0 timed out, 9 skipped**
> (rc=0, all forest gates green). Session #58 bucketed the 15 remaining
> flavored failures by first error and cleared the largest bucket in five
> commits: class-typed `return {...}` selects a constructor (the bare `{`
> unbalanced the scope stack — libc++ `proximate()` lost its parameters);
> conversion-type-ids take cv-qualifiers and reference conversions route
> through `returnDecl` (six copy-pasted cv-skip loops consolidated into
> `Program::skip_cv_qualifier_tokens`); `friend` may follow other
> declaration-specifiers (one friend-decl owner, both entry arms); using-alias
> targets take east-cv suffixes via `consume_declarator_stars`; and the
> SILENT-WRONG headline — the free-operator body deduction lacked the
> derived-to-base receiver walk, so `ofstream << "text"` bound the member
> `operator<<(const void*)` and wrote pointer values into files. A sixth
> commit restored the identity-return pattern recording @7b63f8c6 had
> accidentally severed.
>
> The whole flavored measurement is **935 passed / 11 failed / 0 timed out /
> 12 skipped**: testdefer, testfstream, testloop, testmanipview FIXED with
> zero newly broken (two-way comm-diff against the 15-name set); eligible EXE
> and OBJ each **919/0**. New gate `testofstreamwrite`; extended gates
> `testbracedreturn`, `testconvopclass`, `testfriendkeyword`,
> `testaliasptrtarget` — all match g++ AND clang++ in both stdlib flavors.
> Next: `libcxx_stringstream_construction_state` (testsstream + testopinherit
> share the locale-copy-ctor SIGSEGV; minimal reducer tmp/r19.cpp).
>
> **Previous (2026-08-04, `feature/libcxx-parity7-claude` @7b63f8c6 — the
> noexcept operator):** fulltest **948 passed, 0 failed, 0 timed out,
> 9 skipped** (rc=0, all forest gates green). The `[expr.unary.noexcept]`
> operator is implemented: `noexcept` is a reserved C++11 keyword (the lexer
> erasure destroyed the operator — an expression-context `noexcept(e)`
> SIGSEGV'd and a template-argument `BC<noexcept(e)>` lost the argument);
> `evaluate_noexcept_operator` folds the noexcept-spec conjunction over the
> unevaluated operand's parsed tree, instantiating conditional-spec callees on
> demand ([temp.inst]/14 — caught by `forest_selfexe_gate` when the refusal
> dropped `_S_nothrow_relocate`'s body). Registration placeholders capture
> declaration exception specs; a qualified template-id pack expansion
> (`std::declval<_Args>()...`) is one unit including its qualifier chain.
> New gates: `testnoexceptop`, `testqualpackelide`.
>
> The whole flavored measurement is **930 passed / 15 failed / 0 timed out /
> 12 skipped**: `testconstructible` FIXED with zero additions (two-way
> comm-diff), eligible EXE and OBJ each **914/0**. madc-as-GCC compiles
> libc++'s non-builtin nothrow-trait arm (`_LIBCPP_COMPILER_GCC` at
> `__config:38`), whose `integral_constant` base is exactly the noexcept
> operator over a ctor call — the whole family escaped as silent 0s before.
> The recorded DataDefREF `T&`/`T&&` gap was NOT this test's cause and stays
> open only for the ungated `is_nothrow_move_constructible<std::string>`.
>
> **Previous (2026-08-04, `feature/libcxx-parity6-codex` @672a0966 —
> forwarding-reference deduction owners):** fulltest **946 passed, 0 failed,
> 0 timed out, 9 skipped**, warning census **0**, tsubst fallback **0**;
> `forest_index_oracle` is **5227 indexed names / 3521 registered lookups**.
> `testfwdpackvaluecategory` proves a named lvalue and a value-returning call
> with the same value type deduce `T=int&` and `T=int`: GCC, Clang, and madc
> print `1 0`. Its focused ten-test blast radius passes **10/0** in JIT, EXE,
> and OBJ. `testmembertmplctor` remains `10 400`, proving dependent `sizeof(U)`
> measures the referent when forwarding deduction binds `U=tag&`. The function-
> template and dependent-type-query ownership gates are green alongside the
> existing reference-argument gate.
>
> Real libc++ `testcontainerdtor` now completes in production: vector size 4,
> integer vector size 3, set size 2, map size 2, then `done`; the experimental
> `MADC_FWDREF_ARM` is deleted. The whole flavored measurement is **927 passed /
> 16 failed / 0 timed out / 12 skipped**. Eight prior failures cleared with zero
> additions: `testcastarrow`, `testcontainerdtor`, `testforinitscope`,
> `testmadc_ns`, `testmap`, `testmapiter`, `teststdmapint`, and `testsubscript`.
> Eligible EXE and OBJ are each **911/0**. Logs:
> `tmp/logs/rb-20260804-193248.log` (fulltest),
> `tmp/logs/rb-20260804-194241.log` (whole libc++ battery), and
> `tmp/logs/rb-20260804-193158.log` (focused JIT/EXE/OBJ).
>
> **Previous (2026-08-03, `feature/libcxx-parity6-claude` @ba7517b4 —
> pack/variadic correctness, unreleased):** fulltest **922 passed, 0
> failed, 0 timed out, 9 skipped**, unittest rc=0, `--exe` **875/0** and
> `--obj` **875/0** (of the 891 JIT-passing). All gates green (delimiter
> ratchet 0, rule-trailer gate 207/0 since epoch, tsubst fallback 0,
> warning ratchet 0). Three new gates: `testvariadicmember`,
> `testbasepacktwo`, `testsizeofpack` — each carries at least TWO pack
> elements with DIFFERENT values, because at arity 1 splice and
> replicate are indistinguishable and three real defects shipped green
> behind arity-1 gates.
>
> The flavored parity lane was unchanged at 891 passed / 28 failed / 0
> timed out / 12 skipped** (`run_tests.sh --stdlib=libc++`, measured at
> @ba7517b4; the failing set is comm-diffed BOTH WAYS against
> `docs/parity/libcxx-failset.txt` — 28 vs 28, no new, no fixed). All
> three commits this session are default-lane correctness; none of them
> moved the lane. The 28 are now bucketed into named roots (see
> `claude_status.json`): `__tree` tsubst (5), the retbuf-ABI predicate
> disagreement pinned to `cir_builder.cpp:5074` (best next target),
> free functions not overloading, C++20 abbreviated templates (2),
> `basic_string_view(__long**)` (3, untriaged), the
> `filesystem/operations.h:240` group (4, mechanism unconfirmed), and
> ~9 singles including a SIGSEGV and a silent wrong answer.

> **Previous (2026-08-01, v0.67.0 — the flavor-ABI release, pre-merge
> battery on `feature/libcxx-parity5-claude` @190ff9d2 + release
> files):** fulltest **911 passed, 0 failed, 0 timed out, 9 skipped**,
> `--exe` **894/0**, `--obj` **894/0**, and the packed release arbiter
> **911/0/0/9**. All gates green (delimiter ratchet 0, rule-trailer
> gate 180/0 since epoch, tsubst ratchet 0, retire-std-hardcoding,
> forest self-exe). The flavored parity lane stands at
> **880 passed / 26 failed / 2 timed out** (`run_tests.sh
> --stdlib=libc++`, measured @e09c5381; the failing set is banked and
> comm-diffed in `docs/parity/libcxx-failset.txt` — 21 net flips over
> v0.66.0, zero regressions at every measured step). Nine new gates:
> `testarrayparam`, `testcinstr_libcxx`, `testclassproto`,
> `testconstaccess`, `testconstovl`, `testdeductionguide`,
> `testpacktypedef`, `testprivmethod`, `testtypedefarg`. Fork
> unchanged (**1.0-madc.0.63.0** @8f3934ac). Battery logs:
> `tmp/logs/rb-20260801-231615.log` + `rb-20260801-234210.log`.

> **Previous (2026-07-30, v0.63.0 — the libc++ parity-lane burn-down,
> pre-merge battery on `feature/libcxx-string2-claude` @c4a98c9c):**
> fulltest **856 passed, 0 failed, 0 timed out, 9 skipped**, `--exe`
> **840/0**, `--obj` **840/0**, and the packed release arbiter
> **856/0/0/9**. All gates green (libcxx_gate incl. the operator+ leg,
> delimiter ratchet 0, rule-trailer gate clean, tsubst ratchet 0).
> The flavored parity lane stands at **747 passed / 108 failed**
> (`run_tests.sh --stdlib=libc++`; the failing set is banked and
> set-diffed in `docs/parity/libcxx-failset.txt` — zero regressions at
> every measured step from the 534/282 baseline). Fork at
> **1.0-madc.0.63.0** @8f3934ac (zero-length-array diagnostic parity).
> Battery log: `tmp/logs/rb-20260730-192345.log`.

> **Previous (2026-07-28, `develop` — v0.57.0, the libc++ burn-down: eight
> core-C++ defects in one chain):** fulltest **784 passed, 0 failed,
> 0 timed out, 9 skipped**, `--exe` **768/0**, `--obj` **768/0**, and the
> packed release arbiter **784/0/0/9**. All gates green: `libcxx_gate` OK
> (two new legs: `<cwchar>` compiles AND runs; the CRTP base-arg shape is
> bounded on the forced-legacy lane and `<string>` terminates loudly, never
> with a signal), delimiter ratchet at 0, rule-trailer gate clean, tsubst
> ratchet 0. Fork unchanged (**1.0-madc.0.52.0** @ba216dea).
> Eight new gates this release, each byte-identical across g++ 13 and
> clang++-18: `testinlinensopen`, `testusingaliasfnptr`,
> `testnestedinlinens`, `testcrtpbasearg`, `testnestedtagctor`,
> `testbracedctor`, `testunderlyingtype`, `testdeclonlyspec`.
> The `-stdlib=libc++` parse frontier now stops at ONE recorded defect for
> both `<string_view>` and `<string>`:
> `Gap{common_type_dependent_member_key_explosion}`.
> Battery log: `tmp/logs/rb-20260728-162624.log`.

> **Previous (2026-07-28, `feature/class-static-alias-claude` — eval scope
> capture + the instantiation-product lookup surface):** fulltest **770 passed,
> 0 failed, 0 timed out, 9 skipped**, `--exe` **754/0**, `--obj` **754/0**, and
> the packed release arbiter **770/0/0/9**. All gates green: `libcxx_gate` OK,
> `forest_index_oracle` OK (5180 indexed names cover 3487 registered lookups,
> 40 allowlisted), delimiter ratchet at 0, rule-trailer gate clean.
> Fork unchanged (**1.0-madc.0.52.0** @ba216dea).
> New gate: `testevalexterncapture` (+1 baseline), plus `.expect_quiet` added
> to the four `testmadceval*` tests — they had none, so a flood of
> "undeclared identifier" diagnostics on stderr passed on stdout alone.
> Worth keeping from the run: **`fulltest` went rc=2 with every one of the 770
> tests passing in all four lanes.** The failure was `forest_index_oracle`, and
> the only reason it was actionable is that the driver now prints a per-stage rc
> summary — the previous battery reported a bare `total rc=1` through a `tail`
> pipe that had discarded the stage, and the whole ~30-minute run was wasted.
> The tooling fix (self-logging, stage summary, `TESTS=` subset runs) shipped
> with the same batch; targeted runs are the inner loop now, full suite is the
> pre-merge gate.

> **Previous (2026-07-27, `develop` — v0.54.0, six C++ correctness fixes,
> four of them silent wrong answers):** fulltest **769 passed, 0 failed,
> 0 timed out, 9 skipped**, `--exe` **753/0**, `--obj` **753/0**, and the
> packed release arbiter **769/0/0/9**. All gates green: the delimiter
> ratchet at 0, the rule-trailer gate clean, `libcxx_gate` OK.
> Fork unchanged (**1.0-madc.0.52.0** @ba216dea).
> Four new gates this release: `testqualifiedpostfix`,
> `testclassqualifiedcall`, `teststaticmemberstorage`,
> `testnestedtypescope` — each byte-identical across g++ 13, clang++-18
> and madc, each with empty stderr.
> Worth keeping from the run: **two of the three batteries on the
> static-member fix went RED**, each naming a different class of
> static-member storage symbol that nothing in the translation unit
> defines, and **45 green reducers said nothing about either** — a user
> class's static always has its definition in the same file, so that
> shape is unreachable from a reducer. The suite found what the reducers
> structurally could not, which is the argument for running it rather
> than trusting a reducer sweep.

> **Previous (2026-07-26, `develop` — v0.52.0, Mach-O axis B step 4:
> the darwin `.o` lane is real — axis B is DONE):**
> fulltest **756 passed, 0 failed, 0 timed out, 9 skipped**, `--exe`
> **740/0**, and `--obj` **740/0** — the ELF `.o` lane run explicitly,
> because `MIR_object_read` was refactored onto a format-neutral input
> view so the fork keeps ONE merge implementation behind two container
> fronts. Unit tests unchanged (`test_object_load` among them, exercising
> the refactored reader). Packed release arbiter **756/0/0/9** (`make
> release`: 240 units + the ledger, 2 modules / 22 symbols / 3175 bytes) —
> the fork's ELF merge path rides libmir into the release binary too.
> NEW gate `scripts/macho_obj_gate.sh` / `make -C src machogate`:
> **30 assertions, 15 per arch (arm64 + x86_64)**, over TWO INDEPENDENT
> AUTHORITIES. (a) `ld64.lld-18` + the macOS SDK: Apple's own linker
> links our `MH_OBJECT`, including a MIXED link where a clang-18 TU calls
> into the madc-compiled one, and the relocations it applies land where
> they must — pool slots inside `__text` and `__bss`, the import slot as a
> real dyld bind, a global constructor's entry kept in
> `__mod_init_func`. (b) madc's own read-back: `-c` then link
> disassembles IDENTICALLY to the direct `-o` emit, pool contents
> included (a full-file `cmp` differs only in the code-signature
> identifier, which is the output basename). The gate is NOT in fulltest —
> its cross-madc / llvm-18 / SDK prerequisites would make it silently
> skip there — so the make target rebuilds both cross madcs first and it
> can never validate a stale binary.
> That equivalence leg is the one that earned its keep: it caught a real
> read-back bug every structural check passed over. Mach-O has ONE
> `ARM64_RELOC_PAGEOFF12` where ELF has `LDST64_LO12` and `ADD_LO12`, so
> the reader recovers the kind from the instruction's opcode — and the
> first mask dropped bit 31 (`sf`), reading every `add Xd, Xn, #imm12`
> back as a scaled load: immediate `#0x1` where the direct emit had
> `#0x8`. Structurally valid, silently wrong arithmetic. Lesson kept in
> the plan doc: for a format round trip, assert EQUIVALENCE against the
> path that does not round-trip, not just "the linker accepted it".
> Gate-craft trap found the same way: `llvm-otool -s` dumps section bytes
> as 4-byte WORDS on arm64 and single BYTES on x86-64, so a byte-only
> parser silently finds zero slots and fails on one arch only.
> Still the owner's Mac: RUNNING any emitted Mach-O binary (every darwin
> slice's RUN leg).
> Fork release **1.0-madc.0.52.0** (@ba216dea).

> **Previous (2026-07-26, `develop` — v0.51.0, forest-carriers S6
> complete — the carriers track is DONE):**
> fulltest **756 passed, 0 failed, 0 timed out, 9 skipped** with every
> forest gate green, including the NEW `forest_config_gate` — the
> `madc.ini` promise: **39 checks over 18 legs**, and every settings leg is
> PAIRED with a baseline that would fail without the config file. The
> dialect baseline is the ABSENCE of `__STDC_VERSION__` (so `std = c99` →
> `199901L` cannot pass by accident), the include fixture is unreachable
> without the ini, and `mem-limit = 24` trips the address-space guard with
> the message NAMING the ini value — then `MADC_MEM_LIMIT=4096` overrides
> it, which is the precedence rule proved rather than asserted. The
> `forest` key is discovery **arm 5** and gets the S3 ordering treatment:
> a valid `$MADC_FOREST` binds with NO not-a-container notice (proving arm
> 5 was never probed), while the same junk ini path with an empty
> environment IS reached and IS loud. Strictness has its own legs: unknown
> key (naming file:line + the accepted set), foreign section, missing
> `=`, non-numeric limit, and a named `--config=` that does not exist all
> refuse nonzero.
> Unit tests **+4 cases** (`test_config_file`: 19 cases / 86 assertions),
> including a **schema-blind reader reuse** suite that drives the reader as
> `"madcdat"` with madcdat's own keys — the only test that proves the
> reader is reusable rather than merely generic-shaped, and the guard that
> fails if anyone re-welds madc's schema into it.
> Suite hermeticity: `run_tests.sh` now passes `--no-config` on EVERY madc
> invocation (including the AOT compile legs, which take no
> `$BACKEND_FLAG`), and both pack scripts do too — an ambient `madc.ini`
> would otherwise change the frozen corpus's producer config and send every
> ordinary compile through the dialect gate.
> `--exe` lane **740/0** and packed arbiter **756/0/0/9** (measured at the
> feature commit `3edccef2`; the layering re-cut after it touches no
> codegen, emit path, forest format or pack script, and was covered by a
> grouped fulltest — test scoping by blast radius, owner directive
> 2026-07-26).
> Configure-axis evidence, which no gate can produce because a gate cannot
> reconfigure the tree: `--disable-config-file` → `ENABLE_CONFIG_FILE=0` in
> config.mk → the define drops and the axis stamp flips; an ambient
> `./madc.ini` is not read; `--config=` refuses naming
> `enable-config-file`; `--no-config` stays a no-op; bare `./configure`
> restores. That exercise is what caught the missing `config.mk.in`
> substitution — without it `--disable-config-file` would have been a
> SILENT no-op.
> Also fixed: the installed `madcdis/snapshot.h` did not compile downstream
> (it names `PchCompression` in public signatures but `madc_pch.h` was
> never installed) — proven both ways by staging an install and compiling a
> TU that includes only `<madcdis/snapshot.h>`, then reproducing the
> original `fatal error` with the header removed.
> Fork unchanged (`1.0-madc.0.47.0` @74e705e4).

> **Earlier (2026-07-26, `develop` — v0.50.0, forest-carriers S5
> complete):**
> fulltest **756 passed, 0 failed, 0 timed out, 9 skipped** with every
> forest gate green, including the NEW `forest_ledger_gate` — the
> `-static-libmadc` promise: 14 checks over a container the gate freezes
> itself with `--freeze-ledger=` (the same call the release pack makes).
> A **baseline leg per program** proves the program is genuinely
> runtime-needing (it keeps `libmadc.so.0` WITHOUT the flag), so the
> `-static-libmadc` legs cannot pass vacuously; then try/catch and VLA
> each emit with **no madc library and no `__madc_*` imports**, output
> byte-identical to the JIT run, and the try/catch binary runs under an
> **empty library path**. Failure surfaces are separated: a Tier-B (C++
> script-lane) program refuses NAMING its symbols, while a carrier with
> no ledger gets the BUILD-side message and never blames Tier B; `-c`
> and the `.o` link lane refuse at their own layers.
> **PRODUCT path** (the real one, no `--forest-bind`): `make release`
> packs `bin/madc-release` with 240 units **plus the ledger** (2 modules,
> 22 symbols), that packed binary reports it via `--dump-forest`, emits a
> try/catch program with **0 `libmadc` DT_NEEDED entries**, and the
> program runs correctly under `env -i LD_LIBRARY_PATH=/nonexistent`.
> Packed arbiter **756/0/0/9**; `--exe` lane **740/0**. Unit tests +1
> (`test_forest_policy`: the ledger carrier probe is silent and
> policy-free on an empty chain). Fork unchanged (`1.0-madc.0.47.0`
> @74e705e4).

> **Previous (2026-07-26, `develop` — v0.49.0, forest-carriers S4
> complete):**
> fulltest **756 passed, 0 failed, 0 timed out, 9 skipped** with every
> forest gate green, including the NEW `forest_library_gate` — the
> shared shape's carrier: nine legs over a staged `bin/` + `lib/`
> install (thin-CLI live parity; **library-image** bind with `-v` arm
> naming + byte parity vs `--no-forest-bind`; arm order — the library
> image beats a present `<exe>.forest` AND a junk `MADC_FOREST`;
> `<lib>.forest` bind; and the embedding-host legs with no CLI knob:
> strict+sandboxed binding THROUGH the library image, the
> `enable_external_forest=false` refusal the S3 slice owed (same env,
> knob flipped, opposite outcome), strict-on-empty, silent library
> default). Thin-CLI parity suite (`MADC_BIN=bin/madc-thin`)
> **756/0/0/9** — the shared-linked CLI behaves exactly like the
> monolithic one. PRODUCT `--enable-shared` shape: `make release`
> packs `lib/libmadc.so` (**240 units**), packed arbiter
> **756/0/0/9** through the library carrier, and the installed tree
> (133 KB `usr/bin/madc` + 11.5 MB `usr/lib/libmadc.so.0`) binds 240
> units via `[library-image]`. The DEFAULT monolithic product shape was
> re-run too (`forest_pack.sh` was refactored this slice): release packs
> `bin/madc-release` (240 units), packed arbiter **756/0/0/9**.
> `--exe` lane **740/0**. Unit tests +1
> (`test_forest_policy`: monolithic image identity). Fork unchanged
> (`1.0-madc.0.47.0` @74e705e4).

> **Earlier (2026-07-25, `develop` — v0.48.0, forest-carriers S3
> complete):**
> fulltest **756 passed, 0 failed, 0 timed out, 9 skipped** with every
> forest gate green, including the NEW `forest_sidecar_gate` — the
> carrier discovery chain: sidecar (`<exe>.forest`) and `$MADC_FOREST`
> arms bind with `-v` engagement evidence + byte parity vs
> `--no-forest-bind` live parse, arm order pinned (sidecar before
> env), junk-sidecar and explicit-miss failure surfaces LOUD. The
> `forest_emitpack_gate` Mach-O legs are now rev-skew-immune (each leg
> freezes with the same cross madc that emits/dumps). Packed arbiter
> through **BOTH carriers**: embedded **756/0/0/9** and sidecar
> (`forest_pack.sh --sidecar`, 240 units in `bin/madc-release.forest`)
> **756/0/0/9** + smokes (sidecar bind, loud-on-missing,
> quiet-on-config-mismatch). `--exe` lane **740/0**. Unit tests +6
> (`test_forest_policy`: policy triad, one-shot notice,
> config-mismatch matrix). Mac hardware (`~/s3side`): **7/7 legs per
> arch** (A64 native + X64-Rosetta) — embedded self-dump/bind/run
> regression, sidecar bind + parity, loud-on-missing,
> quiet-on-mismatch; AMFI accepted all four binaries. Fork unchanged
> (`1.0-madc.0.47.0` @74e705e4).

> **Previous (2026-07-25, `develop` — v0.47.0, forest-carriers S2
> complete):**
> fulltest **756 passed, 0 failed, 0 timed out, 9 skipped** with every
> forest gate green, including the NEW `forest_emitpack_gate` — the
> `--pack-forest` carrier: ELF leg RUNS the packed emitted executable
> (rc + output) and pins `--dump-forest` byte-parity container vs
> packed image plus both refusal arms; Mach-O legs (both arches, cross
> madcs) pin the same parity through the writer-laid `__MADC,__forest`
> section. Packed arbiter **756/0/0/9** through `bin/madc-release` +
> `forest_pack: OK (240 units; bind cache == no-cache)`. `--exe` lane
> **740/0**. Full battery run twice (pre- and post- macro-collision
> fix), green both times. Mac hardware (`~/s2pack`): packed emitted
> binaries carrying the real 30-unit darwin groves ran `emitpack ok`
> rc=42 under AMFI (A64 native + X64-Rosetta); hosted `--dump-forest`
> over packed files byte-identical to the containers; full native loop
> (freeze → pack-emit → AMFI → run → read-back) green on both arches.
> Fork release `1.0-madc.0.47.0` (@74e705e4 — the extra-section
> carrier seam).

> **Previous (2026-07-25, `develop` — v0.46.0, forest-carriers S1
> complete):**
> fulltest **756 passed, 0 failed, 0 timed out, 9 skipped** with every
> forest gate green, including the NEW `forest_bind_gate` case
> `[fnptrbody]` (typedef-of-fnptr-member-struct — the darwin `FILE`
> shape that the class-parser typedef branches under-registered).
> Packed arbiter re-proven at the parser fix: `forest_pack: OK (240
> units; bind cache == no-cache)` + **756/0/0/9** through
> `bin/madc-release`. `--exe` lane **740/0**. Hosted darwin binaries
> ship PACKED (`__MADC,__forest` section, 30 units per arch): Mac
> hardware matrix GREEN on both architectures — section read-back,
> all lanes (JIT/AOT × `.mad`/`.c`), grove bind engaged and
> byte-identical to live parse. Fork unchanged
> (`1.0-madc.0.45.0` @a3cf84ae).

> **Previous (2026-07-25, `develop` — v0.45.0, madc-on-macOS Route 1
> Phase 1 complete):**
> fulltest **756 passed, 0 failed, 0 timed out, 9 skipped** (+2 this
> release: `testpragmapack` — GCC pack semantics with parse-time
> application, gcc-oracle-matched — and `testfnptrdecl` — fn-ptr
> declarator breadth incl. the C spiral and deref-postinc binding).
> `--exe` lane **740/0**. G2 on Apple hardware GREEN in every lane on
> both architectures (hosted arm64 native + x86-64 under Rosetta):
> JIT, native Mach-O AOT emit+run, labeled POSIX symbols, ctype
> inlines, stdio macros. Fork release `1.0-madc.0.45.0`
> (@a3cf84ae).

> **Previous (2026-07-20, `develop` @71a36e9d — component-correct GNU
> integer `_Complex`, task #69):**
> fulltest **729 passed, 0 failed, 0 timed out, 13 skipped** (+2:
> `testcomplexint` — the integer-complex lock, JIT and
> gcc-on-emitted-C both green — and `testvarargsstructcomplex`, its
> `mir_skip` lifted). Packed suite (`MADC_BIN=bin/madc-release bash
> scripts/run_tests.sh`) also **729/0/0/13**; `forest_pack: OK (240
> units; bind cache == no-cache)`. gcc-torture **1614 passed**
> steady-state, failset **11 names** in
> `docs/parity/torture-failset-current.txt` (3 integer-complex tests
> UNSKIPPED — skip manifest 33 → 30 — and `20020227-1` FIXED by the
> fork's complex-compare conversion fix; `memclr`/`memcpy-a*` run
> 3.4–3.7 s against the 5 s cap and can flap under neighbor load —
> they pass solo). MIR fork test battery green at fork develop
> @a4a7aa32 (integer-complex specifier rejection + stmt-expr
> init-slot layout fix + mixed-width complex compare conversions, each
> with a c-tests/new regression test). SMAUG `--project` soak green on
> both binaries ("ready at address"). The `--exe` lane is structurally
> unavailable on the CIR backend (`-o` says so explicitly) until AOT
> R4 lands `--emit-object`.

> **Previous (2026-07-19, `develop` @daed32ce — AOT R1: madc `-g`
> source-level gdb on the JIT lane, task #82):**
> fulltest **727 passed, 0 failed, 0 timed out, 14 skipped** (+1:
> `testdebuginfo`, the `-g` pipeline lock). Packed suite
> (`MADC_BIN=bin/madc-release bash scripts/run_tests.sh`) also
> **727/0/0/14**; `forest_pack: OK (240 units; bind cache == no-cache)`.
> gcc-torture **1610 passed**, failset **12 names byte-identical** to
> `docs/parity/torture-failset-current.txt`. MIR fork test battery green
> at fork develop @b6a411fa (upstream sync with vnmakarov master
> a8ab7c31 + the 13-commit debug-support arc + `.debug_frame` CFI +
> the force_val pr34099-2 restoration). SMAUG `--project` soak green on
> both binaries ("ready at address"). Interactive gdb gate:
> `break file.mad:line`, named typed frames across JIT + host, and
> `info locals` in any frame — verified on the final binaries.

> **Previous (2026-07-15, class-KIND parse-once B2 on
> `feature/class-parse-once-codex`):**
> The fulltest component matrix is green with **696 passed, 0 failed, 0 timed
> out, 16 skipped**. Every static and forest gate is green, including
> `forest_bind_gate [subbind]`, the full bind matrix, and both forest oracles.
> The initial fulltest invocation used the census cap (`MADC_CPU_LIMIT=30`), so
> its final `testsubscript --freeze` process hit that cap; only the interrupted
> forest bind gate and the two unreached tail oracles were resumed at the
> documented `MADC_CPU_LIMIT=120`, and all passed.
> `make -C src release` exits 0 and appends 240 packed units; the full packed run
> (`MADC_BIN=bin/madc-release bash scripts/run_tests.sh`) is also
> **696/0/0/16**. `bin/test_class_pattern` is **2/2** with **159 assertions**,
> covering structural-versus-legacy metadata equivalence, GCC/Clang Itanium
> symbols, and loud rollback without parser retry. `bin/test_cir_freeze` is
> **36/36** with **740 assertions**,
> including class-pattern semantic/token fingerprint and forest round-trip
> coverage; `bin/test_stringpool` is **7/7** with **10,032 assertions**,
> including scoped keyed-map transactions. The tsubst matrix is **13/13** and
> the suite ratchet remains **10 hit / 0 fallback**. The full class census is
> pattern **3**, parse **48604**, cache **99334**, opaque **24431**. B2 admits
> the narrow aliases/member/layout/simple-method/forward-completion subset;
> every other shape stays on the single parser lane under a typed reason. Both
> the debug/PIC build and the `-Wall -O2` release build emit **0 host compiler
> warnings**; the source warning census compiles **712** tests with **0
> warnings**. The packed artifact carries a readable 240-unit forest, is
> **10,219,496 bytes**, and has `MADCSNAP` footer magic.
>
> **Local branch update (2026-07-19, `feature/class-parse-once-codex`,
> task #68 — real SysV `__builtin_va_list`, 20041214-1 flips):**
> gcc-torture **1610 passed, 2 compile-failed, 10 runtime-failed, 0
> timed out, 63 skipped** — name-set diff vs the post-#78 baseline is
> EXACTLY {20041214-1.c}, zero regressions; failset refreshed to **12**
> names = 11 class-(b) GNU-ext + **1 class-(a) single** (pr22061-1 VLA
> param bound). The lexer's `__builtin_va_list` → `long` macro is gone:
> the compiler owns the type (`Program::builtin_va_list_type()`, the
> SysV `struct __madc_va_list_tag[1]` singleton), embedded <stdarg.h>
> aliases it, va_end/va_copy macro bodies are array-correct, and the
> singleton is PINNED as type-id slot 34 so frozen typedefs restore in
> any process (the pre-pin packed run failed 10 varargs tests with
> "undeclared identifier va_list"; packed is now **726/0/0/14 == dev**
> with forest_pack OK). The synthesized tag is a Class-5
> forest_index_allowlist entry. testbuiltinvalisttypedef reworked to
> the gcc-parity `.expect_err` (`ap = 0` on an array-typed va_list must
> reject; stale "ok" .expect + .mir_skip removed → +1 pass, −1 skip).
> Fulltest **726/0/0/14**, tsubst ratchet green, SMAUG soak GREEN
> dev+packed. Pre-existing and unchanged: testvarargsstructruntime
> (c2mir lacks VLA-in-struct layout — fork work, pr41935/pr82210
> family) and testvarargsstructcomplex (integer `_Complex` MIR-gen
> fatal = task #69, fork-or-clean-reject) — both verified independent
> of the va_list model.
>
> **Local branch update (2026-07-19, `feature/class-parse-once-codex`,
> task #78 — array-typedef dims order + &array-lvalue typing, strlen-4
> flips):** gcc-torture **1609 passed, 2 compile-failed, 11
> runtime-failed, 0 timed out, 63 skipped** — name-set diff vs the
> post-singles baseline is EXACTLY {strlen-4.c} removed, zero
> regressions; `docs/parity/torture-failset-current.txt` refreshed to
> **13** names = 11 class-(b) GNU-ext + **2 class-(a) singles**
> (20041214-1 va_list delegation, pr22061-1 VLA param bound). Two
> stacked fixes: (1) parser dims order for `A28 row[3]` with
> `typedef char A28[28]` — the declarator's dims are the OUTER
> dimensions; the peeled typedef dims now rotate behind them
> (`sizeof(row[0])` was 3, initializers truncated); (2) c2mir fork
> @8a6a6c57 — `&a[i]` on a decayed array lvalue now constructs the true
> pointer-to-array type instead of copying the decayed element pointer
> (`*(&a[i] + k)` yielded a char scalar; strlen crashed at 0x31).
> Fulltest **725/0/0/15** (+`testarraytypedef`, gcc-oracle byte-equal),
> packed arbiter **725/0/0/15** with forest_pack OK (240 units, bind
> cache == no-cache), SMAUG soak GREEN dev+packed. Adjacent gap filed
> as #79: the CAST form `(char (*)[28])expr` is rejected by the fn-ptr
> cast arm.
>
> **Local branch update (2026-07-19, `feature/class-parse-once-codex`,
> task #77 — liberal default resource guards, owner directive):** the CLI's
> self-limits no longer throttle legitimate work: `MADC_CPU_LIMIT` default
> 60 s → **0 (disabled, opt-in)** — madc RUNS the program, so any finite
> CPU default eventually kills a legitimate long-running server with
> SIGXCPU; an armed CPU guard now trips LOUDLY (new SIGXCPU handler names
> the knob via the crash-write plumbing, then re-raises so the shell sees
> the real signal status). `MADC_MEM_LIMIT` base **2048 → 4096 MB**
> (+128 MB/TU `--project` scaling and the knob-naming `bad_alloc`
> attribution unchanged); both knobs are now documented in `--help`
> (Environment section). Probes: `MADC_CPU_LIMIT=2` + spin → knob-named
> trip, exit 152; **65-CPU-second spin survives the default env** (died
> at 60 s before); malloc-loop NULLs at exactly the 4096 MB ceiling
> (4032 MB allocated over a ~64 MB baseline) and honors a 256 MB override
> (192 MB). Guards install only in `main()` — libmadc embedding hosts set
> their own. Fulltest and the packed arbiter re-verified green (counts
> unchanged: 724/0/0/15 dev + packed).
>
> **Local branch update (2026-07-19, `feature/class-parse-once-codex`,
> promote-gate singles — 🏁 THE ≥1608 THRESHOLD IS MET):** gcc-torture
> **1608 passed, 2 compile-failed, 12 runtime-failed, 0 timed out, 63
> skipped** — name-set diff vs the post-#74 baseline is EXACTLY
> {20030714-1.c, struct-ret-1.c} removed, zero regressions;
> `docs/parity/torture-failset-current.txt` refreshed to the **14** remaining
> names = 11 class-(b) GNU-ext + **3 class-(a) singles** (strlen-4,
> 20041214-1 va_list delegation, pr22061-1 VLA param bound). Two fixes:
> (1) fn-ptr declarations whose RETURN type is a typedef (`X (*fp)(void)`)
> emitted `X fp` — the alias swallowed the declarator; new
> `fnptr_alias_is_fn()` gates the alias-spec form to typedefs that name the
> function type itself, applied to both the variable and MEMBER arms
> (members emitted `bool *m` via the unknown-alias star fallback).
> (2) `_Bool` bit-fields: the signedness reconciliation emitted
> `unsigned _Bool` (rejected by c2mir, C11 6.7.2p2) — `N_BOOL` now counts
> as sign-complete. Fulltest **724/0/0/15** (+2: `testfnptrtypedefret`,
> `testboolbitfield` — the latter locks VALUE semantics only; the
> pre-existing `_Bool:1`-then-wider-type allocation-unit divergence
> (sizeof 8 vs gcc 4) is filed as task #76). The branching.md gate reads
> "ALL class-(a) fixed (≥1608)": the NUMBER is met; 3 class-(a) singles
> remain — the promote call is the owner's.
>
> **Local branch update (2026-07-19, `feature/class-parse-once-codex`, task #74
> dead-branch fold keeps function-scope labels):** gcc-torture **1606 passed,
> 2 compile-failed, 14 runtime-failed, 0 timed out, 63 skipped** — name-set
> diff vs the post-#73 baseline is EXACTLY {pr17078-1.c, vla-dealloc-1.c}
> removed, zero regressions; `docs/parity/torture-failset-current.txt`
> refreshed to the **16** remaining names (11 class-(b) GNU-ext + 5 class-(a)
> singles). vla-dealloc-1's VLA-dealloc half already worked — the label drop
> was its whole story. Fix: `stmt_contains_label()` walk guards BOTH constant
> fold arms in `translate_if_core` (a label makes a dead arm a live goto
> target, C11 6.2.1p3 — the fold falls through to the full N_IF, gcc -O0's
> shape). Fulltest **722/0/0/15** (+1: new `tests/testgotodeadarm.mad`,
> gcc-oracle byte-equal). Gate math: 1606 + 5 class-(a) singles (va_list
> delegation 20041214-1, VLA param bound pr22061-1, _Bool bitfield
> 20030714-1, strlen-4, struct-ret-1) = 1611 ≥ 1608 — TWO more singles cross
> the promote gate.
>
> **Local branch update (2026-07-19, `feature/class-parse-once-codex`, task #73
> wide string literals):** gcc-torture **1604 passed, 2 compile-failed, 16
> runtime-failed, 0 timed out, 63 skipped** — name-set diff vs the post-#72
> baseline is EXACTLY {20010325-1.c, widechar-3.c} removed, zero regressions;
> `docs/parity/torture-failset-current.txt` refreshed to the **18** remaining
> names (memcpy-a8 passed this sweep — the documented load-margin flake, never
> in the failset). Fulltest **721/0/0/15** (+1: `tests/testwideconcat.mad`
> lifted, its `.mir_skip` removed). The fix is the Tier-1 wide-literal
> lowering in the CIR builder: content-hash-named
> `static int __wlit_<fnv1a64>[]` definitions emitted from the parser's baked
> UTF-32 data, uses routed through `var_emit_name`, the constant-scalar READ
> fold now excludes fixed arrays, and each definition rides the rung-3
> referenced-surface filter (`cond_mark_sym`) so dead literals from
> live-parsed-but-unused template bodies can't break the `forest_bind_gate`
> byte-identity oracle (caught by [strbind] during development, fixed before
> landing). Gate math: 1604 + 7 remaining class-(a) (#74 ×2, va_list
> delegation, VLA param bound, _Bool bitfield, strlen-4, struct-ret-1) =
> 1611 ≥ 1608.
>
> **Local branch update (2026-07-19, `feature/class-parse-once-codex`, task #75
> SMAUG --project soak restored):** the soak is GREEN again on the dev binary —
> `Realms of Despair ready at address madc-dev on port 4000` under DEFAULT
> guards. Root cause was madc's own `RLIMIT_AS` resource guard
> (`install_resource_guards()`, fixed 2048 MB `MADC_MEM_LIMIT` default, commit
> @1713e2ba 2026-04-30 — present while the soak was green): the `--project`
> driver holds all 51 parsed Programs at once and legitimately peaks at
> **~2.9 GB VA** (measured `VmPeak` 3,039,872 kB with the guard off; maxrss
> only 985 MB — RLIMIT_AS counts address space, not residency), so natural
> footprint growth crossed the 2 GB line at ~TU #44 (stances.c) and the
> guard's ENOMEM surfaced as an UNPRINTED `std::bad_alloc`. NOT cross-TU state
> poisoning (gdb catchpoint: healthy token arena, 452 × 1 MB chunks,
> `malloc(1 MB)` → NULL; peak maps 105 of 65530). Fixes: workload-scaled
> guard default (+128 MB per manifest TU; guards now install after argument
> parsing since RLIMIT hard limits can never be raised), a `set_new_handler`
> that names `MADC_MEM_LIMIT` when the guard trips (verified: `MADC_MEM_LIMIT=512`
> prints the guard line + `comments.c:...: error: std::bad_alloc` + rc=1),
> and `Program::print_unrendered_diagnostic()` in all four
> `catch(std::exception&)` phase arms — a tokenize/parse failure can never
> again exit silent.
>
> **Local branch update (2026-07-19, `feature/class-parse-once-codex`, task #72
> implicit-int/K&R function definitions):** gcc-torture **1601 passed, 2
> compile-failed, 18 runtime-failed, 0 timed out (memcpy-a8 timed out under
> box load during the sweep; verified passing 4× standalone at 3.6–4.1s vs the
> 5s cap — load-margin flake, not counted), 63 skipped** — all **30** cluster-1
> names flipped, zero regressions (byte-identical name-set diff);
> `docs/parity/torture-failset-current.txt` refreshed to the **20** remaining
> names. Fulltest **720/0/0/16** (+1 = `tests/testknrdef.mad`, gcc-oracle
> byte-equal under `--std=c17`). The three arms all sit behind the existing
> `knr_supported()` gate. Adjacent std-gating fix: C89 implicit function
> declarations in expression context were gated on the `.c` filename
> extension only — `--std=c17` on a `.mad` file now behaves as C17 (the
> extension predicate stays for default-dialect C sources). ⚠️ The mandatory
> SMAUG soak FAILED — and fails BYTE-IDENTICALLY at the pre-#72 baseline
> (stash-rebuild A/B proof): "stances.c: tokenize failed" with no diagnostic,
> only under --project after ~40 green TUs; standalone-with-flags compiles
> clean. Pre-existing madc-side breakage (MadSMAUG tree untouched) — filed as
> **P0 task #75**; #72 introduces no soak delta.
>
> **Local branch update (2026-07-19, `feature/class-parse-once-codex`, task #64
> gcc-torture re-baseline):** full sweep at HEAD @1aa53a4e via
> `scripts/run_gcc_testsuite.py` (defaults: dev binary, `--std=c17`, formal
> skip manifest): **1572 passed, 32 compile-failed, 18 runtime-failed, 0 timed
> out, 63 skipped** — the 50-name failset is **byte-identical to
> `docs/parity/torture-failset-current.txt`** (name-set diff empty both ways;
> ZERO regressions across the #35–#63 span; the previously recorded
> "1571/33, 51-name" banner was one stale against the file). Cluster refresh
> of the 50: **39 class-(a)** + 11 class-(b) GNU-ext (aligned>16 ×3,
> packed/misalign ×2, SIMD vector_size ×3, __sync_* ×1, empty-union ABI ×1,
> one-void-arm conditional ×1). The class-(a) map COLLAPSED on evidence: the
> old "implicit-decl forward call" cluster (5) is a SYMPTOM of implicit-int
> definitions failing to parse (`mpn_print (){}` defines nothing → "import of
> undefined item"), so ONE parser work item — implicit-int function
> definitions (bare K&R identifier lists `f(x){}`, empty-parens `dummy(){}`,
> and typed-param defs after first use misparsed as calls) — covers **30 of
> 39**; the declaration-list K&R form `f(x) int x; {...}` ALREADY parses at
> HEAD. Remainder: wide literals ×2 (undeclared `__wliteral__*`, same cause
> as testwideconcat), labels-in-if-arm ×2 (pr17078-1 attributed to the CIR
> builder — stock c2m passes it), va_list delegation ×1, VLA param bound ×1,
> _Bool sign-qualifier bitfield ×1, strlen-4, struct-ret-1. Gate math:
> 1572 + 39 = 1611 ≥ 1608 — the promote gate is reachable on class-(a) alone.
> Follow-on tasks filed: #72 (the 30-test parser lever, SMAUG-soak-gated),
> #73 (wide literals + testwideconcat lift), #74 (if-arm label drop).
> Reducers banked: tmp/s64_knr.c, tmp/s64_implicitint.c, tmp/s64_wlit.c,
> tmp/s64_labelscope.c (passing control), tmp/s64_failset_new.txt.
>
> **Local branch update (2026-07-19, `feature/class-parse-once-codex`, task #61
> mir_skip audit):** all **16** `tests/*.mir_skip` fixtures re-run at live HEAD
> with runner-equivalent fixture handling — **all 16 still fail; zero lifted**;
> the suite surface is unchanged (arbiter remains 717/0/0/16). Five recorded
> reasons verified still-true (testbitfieldwidearith, testbuiltinllabsoverride,
> teststructleadingattrmember, testunionscalarcast, testvarargsstructruntime);
> **eleven fixtures reworded** because the recorded reason had drifted or was
> wrong: `testvarargsstructcomplex` ("no _Complex" — stale; true cause is GNU
> INTEGER complex `_Complex int` hitting a MIR gen fatal even as a scalar),
> `testvlastructmember` (c2mir now accepts VLA struct members but miscompiles
> the stmt-expr copy — runtime abort, not a reject),
> `testfloattointclamp` (GCC itself saturates via front-end constant folding —
> verified `gcc -O0` prints 2147483647, the .expect IS canon; c2mir converts at
> runtime → INT_MIN), `testfinstrumentfunctions` (no inline asm in the test;
> instrumentation IS implemented — `no_instrument_function` on a prototype
> doesn't merge into the later definition), `testbuiltinframeaddress` /
> `testbuiltinsetjmp` (both lower to runtime helpers in va_helpers.cpp that
> execute in the helper's frame — structurally unable to satisfy
> frame-address/returns-twice semantics), `testdlcall` (madc's `dlcall()`
> builtin has no MIR-lane runtime; the test has no `#load`), `testdlopen`
> (#load parses and lowers; the MIR import resolver just doesn't consult the
> #load'd handles), `testbuiltinvalisttypedef` (the test is INVALID on x86-64 —
> gcc and clang both reject `ap = 0` on the array-typed va_list; madc's
> `__builtin_va_list` divergently accepts it), `testnestedpackedmember` (c2mir
> packed nested-struct member offset not packed while sizeof is), and
> `testwideconcat` (madc-side mixed-width concat lowering emits undeclared
> `__wliteral__a`). Follow-on near-miss tasks filed: fold-spine float→int
> overflow folding (lifts testfloattointclamp), prototype-attr merge for
> `no_instrument_function`, #load/dlcall MIR import resolution, mapping
> `__builtin_va_list` to the target's real type (then convert the test to
> `.expect_err` and lift), and the fork-side `_Complex int` gen fatal.
> Reducers banked: `tmp/s61_cx{A..E}.mad`, `tmp/s61_ftoi.c`,
> `tmp/s61_valist.c`, `tmp/s61_packed.c`.
>
> **Local branch update (2026-06-28, `feature/front-end-performance-claude` @
> Kind 3 dependent-member body tsubst slice):** fulltest
> **670 passed, 0 failed, 0 timed out, 18 skipped** (exit 0, both check gates
> GREEN). The env-gated path is also green:
> `MADC_XTEST_DEP_PARSE=1 bash scripts/run_tests.sh` reports **670/0/0/18**.
> `bin/test_cir` reports **92 test cases, 1137 assertions, 4 skipped** after
> adding nested dependent member-template body coverage and dependent explicit
> destructor rematerialization coverage. `MADC_XTEST_DEP_PARSE=1 bin/madc
> --show-stats tests/testvector.mad` now reports **14 hit / 1 fallback**;
> `std::allocator_traits::destroy<_Up>` is gone from the fallback profile and
> the remaining vector fallback is the out-of-scope
> `std::__cxx11::basic_string::_M_construct<_InIterator>` template-id body
> shape. The C11 spot-check confirms allocator-traits destroy passes the
> allocator reference receiver as `__a`, not a rewritten callee symbol, and
> `__new_allocator<basic_string>::destroy` calls the real `basic_string` D1
> destructor. gcc.c-torture remains byte-identical to
> `docs/parity/torture-failset-current.txt`: **1571 passed, 33 compile-failed,
> 18 runtime-failed, 0 timed out, 63 skipped**, 51-name failset.
>
> **Local branch update (2026-06-26, `feature/front-end-performance-claude` @
> converted system-header reference-forwarded placement-new pack slice,
> including the earlier reserved scalar/pointer helper call widening,
> with two-tree direct type-arg binding and direct
> value/ref/expression/forwarding-call/constructor pack fan-out plus covered
> system-header placement-new pack fan-out and simple class `_Up` placement-new
> tsubst plus direct `__destroy(T*)` helper tsubst and local non-pack nested
> namespace-call tsubst plus nested function-template instantiation, plus
> dependent-parse-error scope balancing and pointer-parameter-pack call
> expansion plus guarded direct `_Destroy_aux`/member-template `__destroy`
> tsubst plus direct `std::move<Args>(args)...` forwarding-pack coverage plus
> the `--show-stats` tsubst engagement counter and ranked fallback profile):**
> fulltest
> **670 passed, 0 failed, 0 timed out, 18 skipped** (exit 0, both check gates
> GREEN). The env-gated tsubst path is also green:
> `MADC_XTEST_DEP_PARSE=1 make -C src fulltest` reports **670/0/0/18**.
> `bin/test_cir` reports **86 test cases, 1067 assertions, 4 skipped** after
> adding coverage for direct `tsubst_type_args` binding of a body-only template
> parameter and direct `tsubst_type_arg_packs` capture for a variadic member
> template, plus direct CIR fan-out for value-pack call arguments like
> `sink(args...)` and reference-pack call arguments like
> `Args&... args` / `sink(args...)`, pointer-pack call arguments like
> `Args*... ps` / `sink(ps...)`, and expression-pattern packs like
> `sink((args + 1)...)`, plus the first forwarding-call pack pattern
> `sink(std::forward<Args>(args)...)` and the same direct structural path for
> `sink(std::move<Args>(args)...)`, plus covered local member-template
> constructors like `Holder(Args... args) { member = sink(args...); }`, plus
> system-header placement-new pack bodies, scalar `_Up` lowering, and simple
> class `_Up` lowering with scalar/pointer constructor pack elements for
> allocator-style `new ((void*)p) _Up(std::forward<Args>(args)...)`, plus
> direct `__destroy(T*)` helpers that defer pointee inspection until after
> substitution and lower class pointees to the concrete destructor, plus guarded
> member-template bodies named `__destroy` whose retained body itself contains a
> direct `__destroy(T*)` marker, plus local
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
> materialized before the outer constructor call, plus `--show-stats` tsubst
> body engagement counters proving real workload visibility of hit/fallback
> split (`testsubscript` currently reports 6 hit / 29 fallback under
> `MADC_XTEST_DEP_PARSE=1`) and a ranked fallback profile. Clean `-O2`
> `testsubscript` profiling ranks the top real fallback shapes as
> `std::allocator_traits::construct<_Up,_Args...>` (4),
> `std::allocator_traits::destroy<_Up>` (4), and
> `std::__new_allocator::construct<_Up,_Args...>` (3), with instantiate time
> 0.327 s and total in-process time 0.804 s on this host. The latest robustness
> slice also adds
> `tests/testdependentparseerror.mad`, proving an env-gated dependent parse
> error balances the temporary parameter compound scope and exits nonzero
> without SIGSEGV. Phase 4 is now tracked at
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
