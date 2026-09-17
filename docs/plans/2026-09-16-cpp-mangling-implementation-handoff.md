# C++ symbol mangling per `--std=` — implementation handoff (2026-09-16)

**For a fresh compacted session whose job is to IMPLEMENT the feature.** Rehydrate:
`bash scripts/resume.sh`, then read **`docs/plans/2026-09-16-free-function-overloading-linkage.md`**
(the design — the spec you implement) and this handoff. The design was drafted and owner-approved
in the prior session (owner: "draft this session, implement in a fresh session"). Do NOT
re-litigate the design decisions in §2/§10.1 — they are SETTLED (below). Your task is execution.

## THE TASK (imperative)

Implement the design doc, **scope (c)**, following its §9 phases. Turn it into a bite-sized
implementation plan first (superpowers `writing-plans` → `subagent-driven-development` or
`executing-plans`), or follow §9 directly if you prefer — the phases are already ordered:

0. **Oracle harness FIRST — DONE (§PHASE 0 below).** A `g++ -c`→`nm` byte-equality corpus (free/member/operator/
   ctor/dtor; builtins, pointer/ref/const, namespaces, nested classes, templates). Make the
   existing Itanium `_sub` encoders reproduce every symbol byte-identical; close encoder gaps
   here BEFORE switching any emit symbol. **This de-risks every later phase — do not skip it.**
1. **Free functions** (this is the darwin-suite blocker unblock) behind a `FEATURE_*` guard.
2. **Members / operators / ctors / dtors** — switch user-bodied member emit off
   `Class__method__oN` onto Itanium; **retire `__oN` in c++/madc mode**.
3. **Namespace functions** — off `__ns__oN` onto Itanium.
4. **Forest serialization** parity for the generalized `c_linkage` + new symbols.
5. **Migration sweep + merge-wave battery**; darwin round-trip; add a madc↔g++ link-interop
   gate to `fulltest`; remove the feature guard.

**Validate locally without a Mac** via the cross-darwin madc (memory `reference_cross_darwin_repro`):
`make -C src cross-x86-64-macos` → `bin/madc-x86-64-macos -c -o out.o test.mad` → `llvm-nm-18`.
The darwin failing tests are `testmadcide_discover`, `_attach`, `_lsp_serve`, `_lsp_stdio`
(the `send`/`write` collision) + `testimportiface` (that one is blocker #3, separate — §"darwin
side-fixes" below). Acceptance for phase 1: those 4 madcide tests compile clean under the cross
madc; full (c): the madc↔g++ ABI harness green.

## PHASE 0 — DONE (2026-09-16, branch `feature/cpp-symbol-mangling-claude`, d1895beaf..82afaa3fe)

The oracle harness exists and is GREEN — SET EQUALITY over 154 user-shape symbols:
- `tests/abi/mangle_corpus.cpp` (stdlib-agnostic, so one oracle serves the Linux AND Mac lanes) →
  `scripts/gen_mangle_oracle.sh` compiles it with g++ 13.3 AND clang++ 18.1, requires identical
  defined-symbol sets (154/154), emits `tests/unit/mangle_oracle.inc` `{mangled, demangled}`.
  ⚠️ Regenerate ON THE CONTAINER and scp the `.inc` back at once — the next sync overwrites it.
- `tests/unit/test_mangle.cpp` `ORACLE_CHECK(got, want)`: `want` must BE an oracle row, the encoder
  must reproduce it, the row is consumed; the final case requires every row consumed. `bin/test_mangle`
  82/82 cases, 524/524 assertions.
- `scripts/mangle_abi_gate.sh --selftest` (fulltest): drift lane (regenerate → row-diff against the
  checked-in `.inc`) + negative control (a corpus copy with one extra shape → 154→155 detected).
  **Phase 5 adds the end-to-end lane here**: `madc --std=c++20 -c tests/abi/mangle_corpus.cpp` → nm,
  set-equal to the oracle (normalize the C2/D2/C5 aliases madc need not emit).

Encoder gaps the oracle found — all FIXED (own commits, trailers): unary operators mangle by ARITY
(`ps/ng/de/ad` vs `pl/mi/ml/an`; arity threaded from the callers); 12 missing operator codes
(`-> ->* %= &= |= ^= <<= >>= && || , <=>`); conversion functions `cv<type>`
(`itanium_mangle_conversion_sub` — an explicit API, never a heuristic); ctor flavors C1/C2/C5
(`itanium_mangle_ctor_sub(cls, params, flavor)`, mirrors the dtor's D0/D1/D2); internal linkage
`_ZL4s_fni` (`itanium_mangle_nested_sub(..., internal_linkage)`; `FuncDef::internal_linkage` is the
fact); `_ZTS` of a namespaced class (`itanium_typeinfo_name_sym_cpp` / `_string_cpp`); USER function
templates at any scope (`itanium_mangle_function_template_sub`: `_Z5identIiET_S0_`,
`_ZN2ns6nidentIiEET_S1_`) + zero-parameter templates end in `v` (both template minters had it wrong);
`itanium_encode_type_sub`'s memo keyed on the target data model too (it served a stale `m` for
`size_t` under LLP64). The naive encoder family (`itanium_encode_type`/`_params`/`itanium_mangle`/
`_method`/`_ctor`/`_dtor`/`_operator`/`_nested`) is RETIRED — zero production callers, wrong for
namespaced types and substitutions; there is ONE encoder now.

KEPT for phase 2: the bare-name RTTI trio (`itanium_typeinfo_sym`/`_name_sym`/`_name_string`, live at
`cir_builder.cpp:10333-10644`) spells `_ZTI2VB` for `ns::VB` where g++ emits `_ZTIN2ns2VBE` — switch
those calls to the `_cpp` forms with `canonical_cpp_spelling()` (bare-`name` fallback for a global
class; the `_cpp` form is byte-identical there).

KEY FACTS FOR PHASE 1 (verified):
- **The free-function minter already exists**: `itanium_mangle_nested_sub({}, name, params)` — the
  global branch is `_Z<name><params>` through the full `_sub` type encoder;
  `send(madc::channel&, madc::value&)` → `_Z4sendRN4madc7channelERNS_5valueE`, oracle-proven. No new
  encoder work; phase 1 is parser ROUTING.
- The spelling pipeline exists: `param_cpp_spellings` built from source tokens
  (`parser.cpp:65913-65930`), `FuncDef::mangle_param_spelling(i)` (`madc.h:493`, desugars scalar +
  fn-ptr typedefs), `spell_varargs_tail` (`madc.h:482`), `namespace_cpp_function_symbol`
  (`parser.cpp:2904` → `itanium_mangle_nested_sub`), `cpp_spelling_for_mangle` (`2845`, DataDef
  fallback). A global-scope user class has NO canonical spelling (`parser.cpp:47732` gates on a
  non-empty namespace) → callers fall back to the bare `name` — correct, no leading `::`.
- **TRAP**: `param_cpp_spellings` are pushed ONLY on the `!func_already_declared` branch (`65909`);
  the reconciliation branch (`65895`) pushes none. The §4.4 fresh FuncDef for a different-signature
  definition must capture its own spellings, or the bind guard (`45322`, spelling-misalign) bails.
- `check-rule-trailers.sh` matches only `^(src|include)/.*\.(c|cc|cpp|h|hpp)$` — Makefile, scripts
  and tests need no trailers.

## PHASE 1 — CORE LANDED (2026-09-16, `9ba213130` + darwin #3 `0a07c75bc`; full JIT suite pending)

What landed (see the commit message for the full map): `Program::cpp_free_fn_mangling_enabled()`
(the `FEATURE_CPP_MANGLE` bring-up guard — RETIRED in phase 5, the predicate IS `presents_as_cpp()`); the global tracking gate
admits every FILE-SCOPE C++-linkage function (`compounds.empty()`, main excluded); file-scope
`extern "C"` records `c_linkage`; a tracked bodied function mints `emit_symbol` via
`namespace_cpp_function_symbol` (asm label wins; `internal_linkage` → `_ZL…`);
`fold_same_signature_overload` = the identity TRUTH (post-parse Itanium signature: fold names/
defaults/typedef variants, inherit C linkage, reject redefinitions); `parseFunction` errors on a
C-linkage signature clash ("conflicting types for 'f'" / "conflicting declaration of C function
'f'"); `CirBuilder::func_def_symbol` = the ONE body-symbol rule (definition, lock-step prototype,
profiler self-address, reachability mark — were four `var_emit_name` spellings); shim NAME = source
key; `user_func_names` carries source + emitted names. Oracle-exact vs g++ AND clang++ (output and
`nm` symbol sets) — see the commit's Oracle trailer. Targeted 11/11.

Tests: `testfreeoverload`, `testfreeoverload_cmode` (C error), `testexterncoverload` (the `send`
shape), `testexterncnooverload` (extern "C" can't overload), `testexterncredecl` (linkage
inheritance). Pre-existing `testfreeop` now emits `_Zlt1Ai`/`_Zeq1AS_`/`_Zeq1APKi`.

FIRST FULL JIT RUN: 1261/123/1TO. Every failure root-caused and fixed in three commits
(`f5bb59f7f` mangle, `e37676cac` cir, `7c622550d` parser + fixtures); the 124 → 124/124 green.
The classes (read the commit messages for the layer argument):
- **Hand-written libc prototypes** (50): `extern int printf(const char *, ...);` with no header
  (c-testsuite convention). RULE: a DECLARATION-ONLY file-scope prototype whose name is a
  `libc_signatures` entry and whose return matches the table's class carries C linkage (bare) —
  `libc_prototype_return_matches`, the same `dynamic_symbol_fallback_return_type` the undeclared-
  call path uses. Never for a DEFINITION (`send`/`write` ARE table names — the darwin shape must
  stay C++). A `builtin_registration` replaced by a prototype passes C linkage on.
- **Twins** (35 "redefinition"): `abs(long)`/`abs(long long)` = one `ddINT64` on glibc; glibc
  `iscanonical(_Float128)` = long double → one Itanium signature, two bodies. The fold keeps the
  newcomer on its INTERNAL name, out of the set (pre-mangling shape). KG Gap
  `long_long_distinct_datadef_lp64` = the type-model fix that would let both mint (darwin's
  `dd_platform_longlong` split generalized).
- **Function-template products** (`_Z4makei` collisions): excluded from the phase-1 mint/fold
  (`fn_template_instantiation_depth == 0`) — phase 3 mints the template form.
- **Latent bugs exposed** (own commits): `call_emit_symbol(v, fd)` for a call THROUGH a fn-ptr
  VARIABLE preferred the pointee's `emit_symbol` → a direct call to the wrong function
  (testforeach2, silent); `peek_param_list_spelling` threw at EOF (`nextToken`) — a lookahead
  must `peekToken`; `declaration_only` flipped back by a later block-scope prototype
  (testmixedfuncvardecl) — now monotonic, and the return-type refresh carries
  `declaration_only`/`c_linkage`/`param_cpp_spellings`; fn-ptr parameter spelling didn't peel
  pointer/array layers (`fptr_structural_spelling`, datadef.h — one owner for
  `mangle_param_spelling` and `cpp_spelling_for_mangle`).
- **Shim key**: `program::call` (madc_program.cpp:640) computes `"__madc_shim_" +
  call_emit_symbol(func, name)` — the CIR side must mirror it (my "source name" change was wrong
  and reverted).
- **Fixtures moved with the lowering**: `testemitindent.expect` (`int _Z4pickiiPi(...)`),
  `testmadcide`'s shows-lowering probe (`long long _Z3addll`); `testfinstrumentfunctions` runs
  as C (`--std=c11`: it defines the `__cyg_profile_*` hooks bare — g++ would not link that either).

SECOND FULL JIT RUN: **1385/0/0TO/9skip — GREEN.** Static gates green (rule-trailers 902/0,
call-emit-symbol, var-emit-name ratchet 17≤19, one-delim-tracker, c-abi-surface, clone-origin).
**DARWIN ACCEPTANCE GREEN** (cross madc, no Mac): `testmadcide_{discover,attach,lsp_serve,lsp_stdio}`
compile clean (rc=0) — blocker #2 CLOSED; `testimportiface` imports `U _sqrt` (was `__Z4sqrtd`) —
blocker #3 CLOSED. UNIT TESTS: four snippets in the unit harnesses declared HOST/NATIVE C symbols bare and looked
them up by bare name — under g++-ABI parity those are C++-linkage functions, so the snippets now
say `extern "C"` (the C-export contract a C++ program spells; not a compiler change):
`test_cir` "external bool returns" (host helpers resolved by dlsym), `test_native_shared` (`madd`/
`mmul` dlsym'd from the `-shared` ET_DYN), `test_object_load` (`madd`/`mmul` via
`MIR_object_loaded_sym`; the R6 externals + `combine`); `test_object_load`'s inline `sumv`
binding check moves to `_Z4sumvii` (STB_WEAK is exactly the vague-linkage contract);
`test_cir_freeze` B3's interning probe moves to `_Z6helperi`. `tests/abi/vec_ffcall.c` (cc as C,
madc as C++) wraps its libvecnative prototypes in `#ifdef __cplusplus extern "C"` guards —
vector_abi_gate green again. Other fulltest gates run green: sret/unprototyped/emitc_sret/
emitcxx_roundtrip, tsubst_flagon, the six static gates. `Adder__add` (a class method, internal
scheme) is still asserted by name in test_object_load — phase 2 moves it to `_ZN5Adder3addEii`.

**`make -C src test`: every binary GREEN (unittest rc=0). PHASE 1 IS COMPLETE AND VALIDATED.**

PHASE 2 ENTRY MAP (as planned — see "PHASE 2 — LANDED" below for what was actually built):
user members / operators / ctors / dtors onto the `_sub` Itanium encoders,
`__oN` retired for them. Entry map (verified this session):
- Mint sites: the class parser's `unique_overload_symbol` callers — `parser.cpp:46891`, `48362`,
  `48427` (`operator_conv` — use `itanium_mangle_conversion_sub`), `48875`; ctors `61717`, `63653`;
  member-template instances `61344` (`__mti` — template form, later). Library members already bind
  through `bind_declared_cpp_symbol` (`45342`: `itanium_mangle_{ctor,dtor,member,operator}_sub`);
  user members must mint through the SAME calls (declaration_only false, body present).
- The readers that treat `!emit_symbol.empty()` as "external — no madc body" must read
  `declaration_only` (a helper `fd->externally_bound()` = `declaration_only &&
  !emit_symbol.empty()`): method dispatch `cir_builder.cpp:12242` (`emit_symbol_method_call` is the
  external-ABI path), ctor lowering `11827–11860`, copy-ctor `13337–13411`, dtor `13066/13077/
  13584–13615`, assign-op `8520`; the carrier rows `6002/6052/6092` are runtime machinery — keep.
- `func_def_symbol` widens from free functions to members (drop the `!owner_class` /
  `namespace_name.empty()` restriction once the readers are fixed) — the vtable-slot contract
  (library bodies keep the internal name) must survive: a LIBRARY class's materialized body is
  `is_system_header_path(tf->file)` / its class externally owned.
- RTTI trio `cir_builder.cpp:10333–10644` → the `_cpp` forms with `canonical_cpp_spelling()`
  (bare-`name` fallback for a global class).
- C2/D2: madc emits C1/D1 (+D0 for virtual); a g++-compiled derived class calls the base's C2 —
  emit C2 as an alias of C1 (no virtual bases) or define both. The oracle corpus has the rows.
- Fixtures that will move: `test_object_load` asserts `Adder__add` by name → `_ZN5Adder3addEii`.
- Acceptance: `testmadcide_*` still green on the cross madc; the phase-5 end-to-end corpus lane
  becomes runnable once members mint (add it to `mangle_abi_gate.sh` then).
Then phase 3 (namespace functions off `__ns__oN`; user function templates via
`itanium_mangle_function_template_sub`), phase 4 (freeze global-key sets, `madc_cir.cpp:4740`;
restore rebuilds global aliases, `parser.cpp:24961`), phase 5 (end-to-end lane, migration sweep,
seam battery). Darwin #1 (madcgit dylib `58f250811`) still needs one real darwin-probe round.
(members/operators/ctors/dtors — the `emit_symbol`-means-external readers in cir_builder's
ctor/dtor/method lowering are the work; RTTI onto the `_cpp` forms; ranking's
`activate_forest_function_family` side effect for tracked calls is worth a look).
KNOWN EDGES (cataloged, not blocking): a block-scope PROTOTYPE in C++ mode stays on the legacy
bare path (a body-follows peek would fix it); a C++ prior redeclared `extern "C"` is not yet
diagnosed as a linkage conflict; array parameters' captured spelling lacks the decay `*`
(phase 5's end-to-end corpus lane will catch `f_arr`).

## PHASE 2 — LANDED (2026-09-16, branch `feature/cpp-symbol-mangling-claude`)

**What a user class emits now** (probe `tmp/p2_cls.mad`, `nm` set-equal with g++ except storage
class B/D and harmless weak extras): `_ZN3Foo3setEPKc/Ei`, `_ZNK3Foo3getEv`, `_ZN3Foo5twiceEi`
(static), `_ZN3FooC1Ei`+`_ZN3FooC2Ei`, `_ZN3FooD1Ev`+`D2`+`D0`, `_ZN3FoopLEi`, `_ZNK3FooeqERKS_`,
`_ZNK3FoongEv`, `_ZNK3FoocvbEv` (conversion), `_ZN3Foo5countE` (static data member),
`_ZN2ns3Bar1fEd`, `_ZN5Outer5Inner1gEv`, `_ZTV/_ZTI/_ZTS` ×3.

**Mechanism — ONE owner (the owner's Rule-4 concern, taken):** `bind_declared_cpp_symbol` (the
pre-existing libstdc++ binder, `parser.cpp`) has two arms on `class_owns_its_cpp_symbols(ddc)`
(madc.h): USER arm → `local_emit_name` (own body); LIBRARY arm → the historical declaration-only
`emit_symbol` bind, unchanged. Recipe extracted as `member_itanium_symbol(ddc, mvar, kind, mname,
is_operator, conversion_type, flavor)`; kinds Ctor/Dtor/Method/Conversion (`CppSymKind`). The
registration funnel `register_class_method_signature` calls it for every kind (Conversion newly;
the library arm declines conversions — pre-phase-2 behaviour). Dtor of a vbase class = D2 (madc's
user dtor body is the base-subobject role; `class_synth_complete_dtor_symbol` = D1). Twin guard
(same Itanium signature on another member) keeps the internal name.
`parseFunction`'s two FuncDef rebuilds (the C `f()` unprototyped rebuild and the return-type
refresh) now carry `local_emit_name`/`emit_symbol`/`method_display_name`/`is_const_method`/
`is_member_template` — the zero-param static `S::f()` lost its name there. The definition site
migrates a user member's `emit_symbol` (asm label) to `local_emit_name` instead of dropping it.

**Lowering:** `body_emit_symbol(v, fd)` (cir_builder) = `local_emit_name ?: var_emit_name`, never
`emit_symbol`; used by `func_def_symbol` (member arm), the vtable slots + thunks, the vbase-forward
demotion, `class_madc_dtor_body_symbol`. `class_vtable_symbol`/`class_typeinfo_symbol` → `_cpp`
forms over `cpp_linkage_spelling()`; `class_synth_dtor_symbol` (D1, or D2 for a vbase class),
`class_synth_complete_dtor_symbol` (D1), `class_deleting_dtor_symbol` (D0) replace the
`Cls___dtor*` literals (`itanium_class_symbols(cdd)` = `class_owns_its_cpp_symbols`; C modes keep
the old spellings). Pass 1.85 `base_object_alias_def` emits `C2`/`D2` linkonce forwarders for
vbase-less user classes. `method_slot_name` (display name, not a `Class__` strip) restored virtual
dispatch. Pass 0.748 `note_vtable_slot_references` pre-declares declared-only virtuals.
`synth_deleting_dtor_def` is linkonce now (was a strong duplicate across TUs).
`check-call-emit-symbol.sh` names `body_emit_symbol` as the second legitimate value-read home.
**The one invariant scope (c) breaks — "a madc body's symbol == its registration name" — and
every site that assumed it:** the deferred-body registry (`deferred_lazy_bodies`, keyed by the
Variable's name; the reachability fixpoint asks by emit symbol → `Program::deferred_lazy_body_key`
translates through `body_symbol_keys`, filled where the user arm assigns `local_emit_name`; every
symbol-keyed reader incl. the pack-time drain uses it), the vtable slots (`body_emit_symbol`), the
freeze body index (`forest_body_loc`), the subscript call (`class_subscript_addr_on` composed
`Class__operator[]` — now `class_method_call_symbol`), `parseFunction`'s two FuncDef rebuilds (the
identity carry), the virtual-call slot name (`method_slot_name`). A class-PATTERN capture does not
mint (`class_pattern_capture_in_progress`): the pattern's members are recipes over `T`; each
instance mints at its own registration.

**Tests/gates:** `tests/testmemberoverload{,.expect}` (g++==clang oracle); `tests/abi/interop_*`
+ `scripts/mangle_abi_gate.sh --interop` (lane A madc-defines/g++-uses incl. a g++ derived class
through madc's C2/D2/D0/`_ZTV`; lane B g++-defines/madc-uses under `--std=c++20`; alien-symbol
subset check with negative control) wired into fulltest; `test_object_load` asserts
`_ZN5Adder3addEii`; `test_class_pattern` asserts the user template member's name on
`local_emit_name`. KG gaps recorded: `virtual_overload_slots_name_keyed` (SILENT — arity-overloaded
virtuals share one name-keyed slot; own session), `copy_init_from_class_prvalue`,
`member_call_on_class_prvalue`, `explicit_cast_via_user_conversion`,
`qualified_outofline_member_def_in_namespace`. Phase-4 note: the forest RESTORE side
(`cir_freeze.cpp` ~3234) still reconstructs a member's `local_emit_name` from its registration key
(internal) — a restored USER class would emit internal names; the pack holds only library classes
so production is unaffected; LOADED==PARSED parity for user members = phase 4.

VALIDATION (build 11 = 2c09cb912, container, 2026-09-16): full JIT suite **1386 passed / 0 failed / 0 timed out / 9 skipped** (= phase-1 baseline 1385 + testmemberoverload); doctest unit binaries rc=0; `mangle_abi_gate.sh --selftest --interop` OK (lane A madc-defines/g++-uses incl. a g++ derived class through madc's C2/D2/D0/_ZTV, lane B g++-defines/madc-uses under --std=c++20, negative control); check-call-emit-symbol, check-var-emit-name-bypass, check-c-abi-surface, check-rule-trailers GREEN; probe object nm set-equal with g++. CROSS-DARWIN ACCEPTANCE (container): `make -C src cross-x86-64-macos` rc=0 with 0 warnings; `bin/madc-x86-64-macos -c` on all 21 testmadcide_* tests + tmp/p2_cls.mad rc=0; the probe's Mach-O object defines exactly the Itanium set (`__ZN3Foo…`, `__ZTV/__ZTI/__ZTS`, C1/C2, D0/D1/D2, `_main`) and NO bare user symbol; across the madcide objects the only non-Itanium function definitions (912, e.g. `_basic_string_char_std____1__…_size`, `_char_traits_char__copy`, `_pair_…__o5`, `___madc_flvmar_N`) are libc++ header bodies madc materializes under its internal names + madc's own helpers — the recorded LIBRARY-class edge (bind_external_class_symbols re-binds their call symbols; whether library materializations move onto Itanium names is PHASE 5's migration-sweep decision), not a user-symbol gap. FOLLOW-UPS LANDED: 2c09cb912 fix(parser) — the K&R `f()` rebuild in parseFunction fired for a DEFINED `int g()` followed by a block-scope `int g();` (testdirectinit) and, once gated on a declaration-only prior, the undeclared-call dlsym fallback (libstdc++'s __builtin_vsnprintf before <stdio.h>) surfaced as never having carried builtin_registration — the five fallback addFunction sites now do, so the existing wholesale machine-registration replacement owns them; 515beb7b2 refactor — the dtor-body flavor rule (D2 with virtual bases, else D1) has ONE owner, Program::madc_dtor_body_flavor (dupaudit of every itanium_mangle_*_sub caller: this was the only phase-2 family; KG DupFamily dtor_body_itanium_flavor = consolidated). README/docs/test-status.md headline counts are the develop seam baseline and update at the (c) seam battery.

NEXT: **PHASE 3** — namespace functions off `__ns__oN` onto Itanium (`namespace_cpp_function_symbol`
already mints; route the bodied namespace function's own name like phase 1 did for the global
scope — `func_def_symbol`'s free-function arm drops `namespace_name.empty()`), user function
templates via `itanium_mangle_function_template_sub` (the `fn_template_instantiation_depth`
exclusions in phase 1/2 are the entry points), member templates (`is_member_template` exclusion in
`bind_user_member_symbol`).

## PHASE 3a — LANDED (2026-09-16, 106508b08): namespace functions

Functions madc DEFINES inside a namespace emit their Itanium symbols (`seam::f(int)` → `_ZN4seam1fEi`,
overloads distinct, nested namespaces `_ZN4seam5inner1gEv`); the internal `__ns_<ns>_<name>[__oN]`
survives only as the registration key. TWO owners, both pre-existing: parseDeclaration's post-parse
mint (the phase-1 arm at parser.cpp ~70290 minus its `!namespace_function` exclusion —
`namespace_cpp_function_symbol`, the same mint the declaration-only namespace bind uses) and
`CirBuilder::func_def_symbol` (minus its `namespace_name.empty()` clause). Same seam as phase 2 (body
symbol ≠ registration key): the mint records `body_symbol_keys[sym]` and the deferred-body readiness
check accepts a bodied function's `emit_symbol`, so a header's lazily materialized inline namespace body
(libstdc++ `std::__exception_ptr::swap`) is reached by the symbol its callers import. extern "C" inside a
namespace keeps the bare symbol (the c-linkage arm precedes the mint); template products stay excluded.
Tests/gates: `tests/testnamespacemangle` (g++==clang++ .expect); the interop corpus gained
`tally::checksum(int,int)/checksum(const char*)/tally::audit::stamp()` on both definers and users;
`mangle_abi_gate.sh --interop` compiles both madc objects with the EXISTING `-fno-eval-shims` (a .o keeps
its `__madc_shim_*` host-call adapters by documented design — madc_cir.cpp shim policy — and a g++ link
without libmadc cannot satisfy their madc_value_* imports; the flag exists for exactly that .o).
VALIDATION (build 13 = 106508b08): probe + test objects define exactly g++'s symbol sets; namespace/
using/overload family 35/35; interop A+B OK; full JIT suite **1387 passed / 0 failed / 0 timed out / 9 skipped** (1386 + testnamespacemangle); doctest unit binaries rc=0.
KG gap recorded: `qualified_namespace_function_definition` — `int seam::h(int x) { … }` outside its
namespace is refused ("Unknown C++ declarator scope"); g++/clang accept. Own parser session.

## PHASE 3b — LANDED (ef99d9daf): user function templates

A product of a USER free/namespace function template emits the Itanium
template-specialization symbol (_Z4makeIiET_S0_, _ZN2ns6nidentIiEET_S1_,
_Z5scaleIiET_S0_i — a non-template parameter stays `i`), weak. The
instantiation driver (instantiate_fn_template_binding — it alone holds the
retained pattern AND the binding) mints the symbol via the phase-0 encoder
itanium_mangle_function_template_sub into Program::pending_fn_instantiation_symbol;
parseDeclaration's symbol arm (the one mint owner) takes it for the product,
records body_symbol_keys, and func_def_symbol defines it. The $Tn placeholder
speller substitute_tparams MOVED from cir_builder.cpp into the mangle library
as itanium_substitute_tparams (six library-operator call sites renamed).
Not minted (internal name kept): a pattern from a system header, member
templates, packs, non-type parameters. Encoder-side fixes surfaced by
testfntplrefidentity: the type parser peels a `typename` disambiguator; a
REFERENCE template argument spells R (cpp_spelling_for_mangle's as_ref), so
forward<int&> and forward<int*> no longer fold to one item; the composer mints
only a well-formed _Z symbol. Oracle corpus + test_mangle gained the shapes
(g++==clang++, 158 rows). Tests: tests/testfntemplatemangle; interop corpus
gained tally::scale<int>/<double> across the TU boundary.

## PHASE 3c — LANDED (a84d82d22): user member function templates

A product of a USER class's member function template emits the Itanium
member-template specialization symbol (_ZN3Box4convIiEET_S1_, const member
_ZNK3Box3tagIcEEiv). Rule #4: the encoder itanium_mangle_member_template_sub
and a full library CALL path (CirBuilder::member_template_method_call — guarded
is_externally_defined, so it bails "not-external" for a user class) already
existed; user_member_template_product_symbol composes the SAME symbol at
instantiate_member_fn_template_for_call (the pattern's captured
template_param_spellings / template_return_spelling, itanium_substitute_tparams,
the deduced args via cpp_spelling_for_mangle) and sets it on the PRODUCT's
local_emit_name (the phase-2 own-body field), guarded to
class_owns_its_cpp_symbols and a well-formed symbol. THIRD placeholder-speller
copy (cir_builder's template_placeholder_spelling) consolidated onto
itanium_substitute_tparams (KG DupFamily template_param_placeholder_speller).
Not minted: library owner, pack / non-type parameter. Member templates on a
class TEMPLATE (owner spelling with args) and the cross-TU interop shape are a
later slice. Tests: tests/testmembertemplatemangle (member + const member
template on a user class).

VALIDATION (3b+3c, build 17 = a84d82d22): probe/test objects define exactly
g++'s symbol sets; template/member family green; interop A+B OK. Full JIT suite **1389 passed / 0 failed / 0 timed out / 9 skipped** (1387 + testfntemplatemangle + testmembertemplatemangle); doctest unit binaries rc=0; `mangle_abi_gate.sh --selftest --interop` OK (158 oracle rows, g++ == clang++); cross-darwin (`make -C src cross-x86-64-macos` rc=0, 0 warnings): the 21 testmadcide_* + the three phase-3 tests compile to Mach-O and define exactly the Itanium sets with darwin's leading underscore (`__ZN4seam1fEi`, `__Z4makeIiET_S0_`, `__ZN3Box4convIiEET_S1_`, extern "C" `_seam_c_export` bare).
KG: DupFamily template_param_placeholder_speller (3 byte-identical copies) =
consolidated; Gap qualified_namespace_function_definition (3a) still open.

## PHASE 4 — LANDED: the pack restore keeps the Itanium symbols

MEASUREMENT FIRST: the packed release binary (`make -C src release`, system-header
forest pack) compiles the phase tests to the SAME symbol sets as the dev binary
(testnamespacemangle 7, testfntemplatemangle 12, testmembertemplatemangle 7,
testmemberoverload 35), but the packed suite failed 9 libstdc++-heavy tests
(testiomanip, testmanip, testmanipview, testmathheader, teststdstringconv,
teststod, teststringabiinterop, testvaluestream, testvbasemanip) with an
undefined import of a header INLINE NAMESPACE function's Itanium symbol
(_ZSt5fixedRSt8ios_base, _ZSt5isinfd, _ZNSt7__cxx114stod…). ROOT: phase 3a's
arm mints emit_symbol AND records body_symbol_keys[sym] = registration name
(the translation pack_callee_homed / deferred_lazy_body_key use, because the
deferred-body registry is keyed by the registration name while callers import
the symbol). The freeze carries emit_symbol (madc_cir.cpp r.emit_symbol_id) and
register_forest_func restores it, and the forest fixpoint DID materialize the
frozen body — but keyed its forward proto, its conditional-emission mark and
the MIR-cache gate on the REGISTRATION name (kv.first) while the def node's
own declared id and every reference carry the symbol it defines; the
cond-emission harvest saw the key unreferenced and DROPPED the def (the packed
C11 held only the extern + the call). FIX: the fixpoint keys proto/mark/gate
on the def's own declared id (cir_declared_id, the existing accessor); the
proto record carries the registration key for the root exemption; the
"referenced?" test accepts a FREE/NAMESPACE body referenced under its
emit_symbol (never a library METHOD's — that is the external symbol its calls
bind to). The forest registrar + flush_forest_pending_globals record the
symbol→key translation (body_symbol_keys) like the parse-time registrar;
Program::body_registration_key(sym) is the one accessor the pack's name-keyed
lookups read. Two translation-only attempts did NOT move the nine — the
evidence that settled it was the packed binary's -v facts + emitted C11.
PACKED lane (bin/madc-release, build 21 = a2f9e5109): **1389 passed / 0 failed / 0 timed out / 9 skipped** — identical to the dev JIT lane; the 9 std-header tests + testusingfnoverload all pass; the packed C11 defines `_ZSt5fixedRSt8ios_base`; doctest unit binaries rc=0; the phase tests' symbol sets are identical packed vs dev (7/12/7/35 symbols).
ALSO CLOSED: testusingfnoverload's PACKED-only failure ("Unknown namespace or
class 'char_traits_char'" at iosfwd:114) was a downstream symptom of the same
dropped bodies, not a type-name restore defect — KG Gap
pack_restore_default_template_arg_instance_name resolved by this fix.

## PHASE 5 — LANDED (code): the migration sweep — commit `9100020ac`

THE SWEEP (the artifact, not the recipe): `tests/abi/mangle_corpus.cpp` minus
the 17 lines madc's parser refuses (below) compiled with `madc --std=c++20 -c`
and `g++ -c`, defined function symbols set-compared (tmp/p5 on the container).
118 of 124 byte-identical on the first run; the residues were real and are
closed:

- **Unary-first operator peers** (`Foo__operator*_un` / `&_un` / `++_un`
  emitted): the class member registrar's peer-retag branch (parser.cpp
  ~49157) wrote the internal `_un` KEY over the unary peer's already-bound
  Itanium `local_emit_name`. Now the symbol stays, `body_symbol_keys` follows
  the moved key; an unbound peer (library class) keeps the old behaviour.
- **Array parameter decay** (`_Z5f_arri`): parseFunction decayed the DataDef
  (getPointerType, ~66474) but the spelling never learned of it → `++param_ptr_depth`
  at the decay; a multi-dim parameter spells its decayed declarator
  `int (*)[3]`; the encoder gained the Itanium ARRAY production (`A<dim>_`,
  a substitution candidate like every decorated type): PA3_i / PA3_A4_i / PPi
  (3 new oracle rows → 161, g++ == clang++; ORACLE_CHECKs added).
- **Encoder refusal**: a spelling whose name component is not an identifier
  sets `TypeNode::invalid`; every entry point `settled()`s to the EMPTY
  symbol (the callers' existing bail path) — `int (*)[3]` once reached an
  object as `14int (*)[3]`. Unit checks: `int (*` and `{lambda(int)#1}` → "".
- **Instantiation products bind WEAK**: `_ZN3BoxIiEC1Ev` was T (g++: W) —
  a class template's members are minted from the pattern at instantiation
  WITHOUT parseFunction, so its vague-linkage grant never saw them. Stamped
  where every product registers: `addVariable`'s `vfINSTPRODUCT` arm sets
  `FuncDef::vague_linkage` (ONE owner for "product ⇒ vague"; parseFunction's
  own grant stays for the bodies it parses). Two madc objects using Box<int>
  no longer collide at link.
- **FEATURE_CPP_MANGLE retired**: `cpp_symbol_mangling_enabled()` is
  `presents_as_cpp()`.

GATE (`scripts/mangle_abi_gate.sh --interop`, fulltest): beside ALIEN
(madc ⊆ g++) the two definer objects of ONE program are checked for MISSING
(g++ ⊆ madc — set EQUALITY, the design §8 acceptance) and STRONG (nothing
madc binds T that g++ binds W); each with a negative control. The interop
corpus grew the sweep shapes (unary-first `operator*`/`operator++` pairs,
`add4(int[4])`, a `Cell<T>` class template with out-of-class members used by
both TUs). Lane A: 36 symbols set-equal, none bound stronger; output identical.

DECIDED (default, owner may flip): library class/template materializations
(the internal-name island, `bind_external_class_symbols` re-binds their calls)
STAY on internal names for (c) — they are madc-internal bodies never named by
a g++ TU; moving them costs every library materialization site for no link
that fails today.

LEFT, RECORDED (own sessions, not (c) blockers):
- KG Gap `corpus_end_to_end_lane`: the full corpus as a `--corpus` lane waits
  on 4 PARSER shapes (17 errors): explicit instantiation directives
  (`template struct Box<int>;`, 11×), out-of-class conversion-operator
  definitions (`Foo::operator bool() const {}`), `ns::Bar::Bar()` definitions
  (the `qualified_namespace_function_definition` family), `::operator new(n)`.
- KG Gap `long_long_distinct_datadef_lp64`: the ONLY symbol divergence left in
  the reduced corpus — `_Z7f_llongl`/`_Z8f_ullongm` vs g++ `x`/`y`
  (`dd_platform_longlong()==&ddINT64` on glibc LP64; the darwin split is the
  model to generalize; type-system session with the full battery).
- madc emits a class template's ctor/dtor bodies eagerly on mention (g++ only
  on ODR-use) — extra WEAK definitions of correct symbols, harmless at link.

VALIDATION (container build 2026-09-16 21:15): gate `--selftest --interop`
OK; array reducers f1..f4 == g++; doctest unit binaries rc=0; 17 targeted
tests (phase, operator, out-of-line template ctor, project) 17/17 JIT, 16/16
exe, 16/16 obj; 0 build warnings.

## THE SEAM — IN PROGRESS (2026-09-16/17): pre-build green; gate regressions fixed; gates GREEN at 58a2393e2; battery LAUNCHED

Pre-build (static gates, `make release`, `hosted-x86-64-windows`, `hosted-arm64-macos`): builds green,
0 warnings. The static gates found three defects no JIT suite covers, each fixed in its own commit:
`dd7a063bf` (smaug_gate: the C-linkage clash compared a desugared prior spelling against a raw fresh
one — ONE owner `FuncDef::mangle_spelling_for`), `7d0ed4e1d` (forest_bind_gate [method]: a
grove-restored USER member is bound through `bind_declared_cpp_symbol` at restore; the bound-method
walk seeds by own-body symbol and translates callees via `body_registration_key`). A first draft of
the latter also widened the PACK fixpoint's reference test to `local_emit_name` and regressed
[vecnewspec] (member-template placeholders alias their last product on `local_emit_name`) — bisected
against phase-4 and HEAD binaries, dropped. `5d1a7ca6f` (forest_bind_gate [silbody]: a BLOCK-SCOPE
C++ prototype is tracked and bound like a file-scope one — g++ declares it in the enclosing namespace;
the definition mint keeps file scope so a GNU nested definition stays legacy). At `5d1a7ca6f`:
forest_bind_gate 29/29, smaug_gate, interop, unit, targeted all green; the forest emitpack / sidecar /
crosstu / library gates pass once `bin/madc-thin` + `bin/madc-mono` are rebuilt (the launcher must build
them first — fixed locally). **`libcxx_gate` (RED at the 2026-09-16 pause) was TWO defects, both fixed 2026-09-17:** `f73ba1f55` — the extern-flush dedupe set `typed_proto_syms` was folded by REGISTRATION name at two sites while `func_proto` declares a bodied C++ function's prototype under `func_def_symbol` (its Itanium symbol), so the call site's opaque `extern void *_ZSt5fixedRSt8ios_base(void *)` survived beside the typed proto (clang-18: conflicting types; the libstdc++ emit had the identical pair, c2mir lenient). Both fold-ins key by `func_def_symbol`; gcc and clang `-fsyntax-only` now accept the libstdc++ emits of testiomanip/testmanipview/testofstreamwrite. `58a2393e2` — the deferred-body READINESS check's phase-4 arm ("a bodied function's body defines its emit_symbol") lacked `func_def_symbol`'s MEMBER exclusion: libc++'s `basic_filebuf<char>::basic_filebuf()` (C1Ev EXPORTED by libc++.so; out-of-line body deferred with `declaration_only` cleared) was DERIVED from `<fstream>` on the derived `basic_ofstream` ctor's import of C1Ev — and that derive parsed `<fstream>:337` `&std::use_facet<codecvt<…> >(this->getloc())`, a PRE-EXISTING parser gap (reproduces on the madc-astra 7b16830a and madc-base binaries; the reducer fails under libstdc++ too): KG Gap `addressof_qualified_template_id_call` (reducer + layer chain recorded) — its OWN focused session (no impromptu core-parser changes). ONE owner extracted: `FuncDef::body_defines_emit_symbol(const Method *)`, read by `func_def_symbol`, the readiness check and the forest-restore translation registrar (`flush_forest_pending_globals` carried the same divergence for the pack lane). `MADC_MTI_PROBE` gained a `ready key=… via=…` line naming the spelling that made a body ready. Evidence at `58a2393e2`: libcxx_gate OK (every leg), the three libc++ tests rc=0 with every .expect line, forest_bind_gate 29/29, check-rule-trailers GREEN. LESSON: the library's EXPORT LIST (`nm -D --defined-only libc++.so.1`) is the derive-vs-import oracle — `basic_ofstream<char>`'s ctors are NOT exported (`_LIBCPP_HIDE_FROM_ABI`; deriving them is right), `basic_filebuf<char>`'s ctor IS; the bind probe's `ext_def=1` does not mean exported. **THE BATTERY IS LAUNCHED** (2026-09-17, container pid 583020): `tmp/seam_stage2.sh` over the synced `58a2393e2` tree — prereq build (madc, madc-thin, madc-mono), EVERY fulltest gate, then `tmp/seam_battery.sh` if green; results `tmp/seam/*.rc`, `tmp/seam/battery.progress`, `tmp/seam/battery.done`, `tmp/seam/stage2.nohup`. Next: read them; `scripts/lane_ledger.sh record <lane> <tally>` per green lane; a red lane = its own fix commit + relaunch.

## THE SEAM — NEXT: the ONE battery for the whole (c)

Pre-build EVERY toolchain FIRST (feedback_seam_prebuild_all_toolchains):
static gates, `remote_build.sh sync build release win`, `make -k
hosted-arm64-macos`; THEN one fulltest + exe + obj + packed + headerless +
c-testsuite + wine + macOS cross; `scripts/lane_ledger.sh record` per lane;
`/dupaudit` scoped to the feature; develop merge (owner-visible). Master
promotion = OWNER after full (c) + darwin #1 probe round.

## SETTLED STATE (evidence — do NOT re-derive)

- **Scope = (c)** (owner ruling, Rule #1): EVERY user-defined C++ symbol mangles Itanium in
  c++/madc mode (free + member + operator + ctor + dtor + namespace) so a madc `.o`/`.so` is
  ABI-identical to g++/clang. Rejected: "mangle-on-collision" and "(b) free-functions-only,
  leave members on `__oN`" — both Rule #1 violations (the latter emits `Foo__bar` where g++
  emits `_ZN3Foo3barEv` → won't link against real C++). `extern "C"` + `main` stay bare; C mode
  stays bare and ERRORS on a signature clash. Default mode is `STD_MADC`.
- **`__oN` is RETIRED in c++/madc mode.** Itanium encodes param types → subsumes the
  order-based `__oN` disambiguation (Rule #7, one mangling path). `__oN`/internal names survive
  ONLY for madc-internal compiler-emitted machinery, never a symbol g++ could name.
- **Library (declaration-only) members/free functions are ALREADY real Itanium** —
  `itanium_mangle_*_sub` (`src/parser.cpp:45342-45351`) + `namespace_cpp_function_symbol`
  (`parser.cpp:2904`); this is how `std::string::c_str()` binds libstdc++. The gap is USER-BODIED
  symbols only. The encoders EXIST and are proven on the HARDEST shapes (libstdc++/libc++
  internals — `std::__cxx11::basic_string<…>`, nested templates, ABI tags, substitutions);
  arbitrary USER types are strictly simpler and largely already covered. Phase 0 is a
  VERIFICATION harness + small gap-fill, NOT building the mangler (owner, 2026-09-16).
- **Root cause of the darwin #2 symptom** (`src/parser.cpp:65083-65163` + per-param loop
  `65879-65891`): `parseFunction` finds the umbrella's `extern "C" send` prototype in the
  name-keyed `funcdef_map`, and its return-type-mismatch branch copies the OLD params (`fresh->
  parameters = func->parameters`, `65138`) while the body types locals from the NEW decl →
  param `c` typed `int` not `channel&`. That mistyping is what made `c.write` (2a) fall to UFCS
  → free `write`. Under (c) the user `send` becomes a distinct Itanium overload and never
  touches POSIX bare `send`. Make the reconciliation mode+signature aware (design §4.4/§4.5).
- **Key file:line map** is in the design doc §3 (emit precedence `cir_builder.cpp:357-364`;
  mangler `madc_mangle.cpp:163`/`123`/`17`; `c_linkage` set at only `parser.cpp:69826`; global
  overload gate `parser.cpp:69660-69666`; call ranking `call_target_variable` `cir_builder.cpp:4802`;
  mode predicates `include/madc.h:4981-5020`, default `STD_MADC` `parser.cpp:21793,21835`).
- **C-ABI gate** (`scripts/check-c-abi-surface.sh`) already excludes `_Z…` — no new allowlist.
- **git:** design doc committed `694330633` (chain: `80543234e` draft → `26614f2f3` correction →
  `694330633` scope-(c)). develop (local) is ahead of origin/develop `7862f951e` by these docs +
  the darwin commits below — do NOT push develop (promotion gate not green). Working tree clean
  but for owner's untracked `donut.c`/`test.mad`/`testsort.mad`.

## OPEN (owner, §10.2): gate sequencing

Phase 1 alone makes the darwin tests pass; phases 2–3 are what deliver ABI parity. Recommended:
land ALL of (c) as one merge wave (don't ship a half-migrated symbol scheme), validating the
darwin lane green after phase 1 to confirm direction early. Confirm with owner at the seam.

## DARWIN SIDE-FIXES (separate from the mangling feature — still open)

- **#1 madcgit dylib flat-path** — FIXED `58f250811` (`src/madcgit.mk`: `cp -f $@
  $(LIBDIR)/libmadcgit.dylib` under `ifdef HOSTED_DARWIN_TARGET`). Needs one darwin-probe
  validation round. Fixes testgit/testgraphpast/testnexus_layers.
- **#3 import drops darwin `extern "C"`** (testimportiface → `_Z4sqrtd`) — NOT YET DONE,
  script-only, LOW-RISK. `scripts/gen_darwin_prelude.sh` flattens the umbrella with `clang -x c`
  → under C, darwin's `__BEGIN_DECLS` (guarded `#if __cplusplus`) expands EMPTY → `extern "C"`
  stripped → `sqrt` mangles. FIX: wrap the flattened umbrella body (at the finalization step
  ~line 148, after the wchar_t guard) in a conditional `extern "C"` — `#ifdef __cplusplus\n
  extern "C" {\n#endif … #ifdef __cplusplus\n}\n#endif` — what the real headers do via
  `__BEGIN_DECLS`/`__END_DECLS`. Then rebuild the cross madc and confirm `llvm-nm-18` shows
  `_sqrt` not `__Z4sqrtd`. Ship with #1 in one darwin round-trip. (The darwin-suite lane is a
  RELEASE-tier gate for master; both must be green before promote.)

## STANDING CONSTRAINTS (unchanged)
Push only to owner remotes (origin = git@github.com:derekbsnider/madc.git); no `&&` chains
(use `;`); one Bash call = one simple command; container = `ssh -p 2299 dev@localhost` via
remote_build.sh (QNAP never builds); one heavy container job at a time; every commit touching
`src/`/`include/` carries the four rule trailers (Makefile+scripts+docs excluded); the seam is
the arc release boundary (ONE battery + lanes + develop merge there, never per slice); never
`MADC_PUSH_NOGATE=1`; master promote = OWNER decision (never run `/promote`). Core-parser change
→ this IS the focused session for it (owner-directed).
