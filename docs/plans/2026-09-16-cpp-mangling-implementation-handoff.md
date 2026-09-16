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
(+ `FEATURE_CPP_MANGLE`, default ON, `-DFEATURE_CPP_MANGLE_OFF` = legacy); the global tracking gate
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

NEXT (in order): (1) the second full JIT run (`tmp/p1-jit-suite2.log`) must be 0 failures; (2)
`make -C src test` (unit tests); (3) cross-darwin acceptance: `make -C src cross-x86-64-macos`
then `bin/madc-x86-64-macos -c -o out.o tests/testmadcide_{discover,attach,lsp_serve,lsp_stdio}.mad`
compile clean and `testimportiface` shows `_sqrt` (U) not `__Z4sqrtd`; (4) bank, then phase 2
(members/operators/ctors/dtors — the `emit_symbol`-means-external readers in cir_builder's
ctor/dtor/method lowering are the work; RTTI onto the `_cpp` forms; ranking's
`activate_forest_function_family` side effect for tracked calls is worth a look).
KNOWN EDGES (cataloged, not blocking): a block-scope PROTOTYPE in C++ mode stays on the legacy
bare path (a body-follows peek would fix it); a C++ prior redeclared `extern "C"` is not yet
diagnosed as a linkage conflict; array parameters' captured spelling lacks the decay `*`
(phase 5's end-to-end corpus lane will catch `f_arr`).

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
