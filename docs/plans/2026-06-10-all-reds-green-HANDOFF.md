# HANDOFF — ALL integration reds green; merged to develop (2026-06-10)

> **Cold-restart contract:** read this top-to-bottom, run `bash
> scripts/resume.sh`, then TRUST THE LIVE REPO over any stale claim here
> (see `feedback_verify_over_stale_handoff`). Every "fails/works" statement
> below was verified at `42831d7` on 2026-06-10.

## 1. State

- **develop @ `42831d7`** (pushed) — fast-forwarded from
  `feature/cpp-detection-idiom-claude` (branch kept, also pushed, same tip).
- **fulltest: 547 passed / 0 failed / 0 timed out / 26 skipped — ZERO
  integration reds** for the first time on the CIR backend.
- **gcc.c-torture: 1567 / 31 compile-failed / 56 runtime-failed / 1 timeout /
  30 skipped** — failset byte-identical across the whole session
  (reference: `tmp/failset_lsq.txt`; regenerate per the gates below if gone).
- **SMAUG boots** via `--project` (gate command in §6).
- MIR fork pin unchanged: `/workspace/mir` develop @ `2ffebff`
  (`MIR_COMMIT`), no fork changes this session.
- The campaign plan `docs/plans/2026-06-10-testfstream-alias-reference-plan.md`
  is **COMPLETED** (banner at its top has the commit map).

## 2. What landed (session commit map, develop history)

| commit | what |
|---|---|
| `5482124` | docs: canon research — g++/clang alias-as-canonical-type model (`docs/plans/refs/2026-06-10-alias-ref-canon-notes.md`) |
| `8b16fd8` | fix(cpp): alias-spelled reference returns keep their reference-ness |
| `c4a39aa` | feat(cpp): namespace function-template BODY instantiation — to_string/stoi run |
| `883c26e` | test(cpp): testfstream GREEN through real libstdc++ headers — fulltest 547/0 |
| `42831d7` | docs: sync mirrors |

### 2.1 Alias-spelled reference returns (`8b16fd8`)

The model (from the canon notes): **an alias is a type, not a spelling** —
resolution must produce (base, ptr depth, IS-REFERENCE) as one unit.

- `DataDefREF : DataDefPTR` (`include/datadef.h`) — a reference-marked type,
  IDENTICAL to the pointer it lowers to (same name/size/DataType) except for
  `is_reference()`. `Program::getReferenceType` (parser.cpp, next to
  `getPointerType`) caches them + collapses ref-of-ref.
- Both trailing-`&` alias paths (`typedef T&` in `TokenTYPEDEF::parse`,
  `using = T&` in `TokenUSING::parse`) store the reference type; alias-CHAIN
  hops propagate it for free because the ref-ness is in the stored DataDef.
- The class-METHOD parse (TokenCLASS::parse, method branch) unwraps:
  `is_reference()` → `ret_is_ref = true` + referee as return type — exactly
  literal-`&` semantics → `FuncDef::returns_ref`. Data members untouched.
- `parsePostfixChainFrom` builds the main parser's `TokenSubscript` for a
  CHAIN-HEAD `var[idx]` when var's class has `operator[]`
  (`TokenSubscript::subscript_operator_element_type` gate) — `&s[1]` now
  dispatches `operator[]`. **Head-only + class-only**: mid-chain TokenVars
  can be member proxies (bare-name emission), and fixed arrays/pointers KEEP
  the old TokenSubscriptExpr path (its element typing — row types for sizeof
  — is load-bearing: `testmemchr2darray` / `teststructchararraysubaddr`
  regressed when the gate was broader and are the canaries).
- Parameters spelled by ref-aliases (`ref_params`) were NOT needed and are
  not wired — an explicit follow-up if a wall needs it.

### 2.2 Namespace function-template BODY instantiation (`c4a39aa`)

**Why:** libstdc++ does NOT export its function templates (`nm -D` finds no
`__gnu_cxx::__stoa`, `std::__detail::__to_chars_len/__to_chars_10_impl`) —
mangled-direct (Pattern A) can never bind them; madc must compile the bodies.
Calls used to collapse onto the body-less `__ns_<ns>_<name>` placeholder and
die at MIR-link ("import of undefined item").

Mechanism (Borland monomorphize, on demand at the call):

- **Capture** (`register_skipped_namespace_template_function`): a
  body-bearing namespace fn template's declaration tokens are retained per
  `ns::name` in `Program::fn_template_map` (raw token pointers, the
  `last_skipped_template_decl` lifetime model; cloned per instantiation).
  The overload set `namespace_fn_overload_sets[ns::name]` is seeded with the
  placeholder (sentinel spelling `\x01fn-template-placeholder`) so
  instantiations always mint FRESH `__oN` symbols (re-entering parseFunction
  under the placeholder id would swallow their parameters).
- **Instantiate** (`Program::instantiate_namespace_fn_template_for_call`,
  hooked at the end of the live `parseCallFunc`): deduction supports bare /
  cv / `*` / `&` param shapes (`fn_template_deduce_param`), structural
  fn-ptr matching (`fn_template_deduce_fnptr_param` — `&std::strtol` return
  + param positions), ONE trailing parameter pack bound to EXACTLY ONE
  element, simple template-param defaults (`_Ret = _TRet`), and **explicit
  call template args** (`__stoa<long, int>`) captured by
  `Program::capture_call_template_args` into
  `TokenCallFunc::explicit_template_args` (the old behavior discarded them
  via `skip_template_id_suffix`). Bindings are FIRST-WINS (explicit args are
  authoritative; C++ excludes explicitly-bound params from deduction).
  Substituted tokens re-parse with the `instantiate_template_use` context
  recipe (compounds/class-scope/cur_func_name cleared, `current_namespace`
  = template ns, Cpp linkage). **Failures are non-fatal**: catch + drain the
  injected tokens back to the pre-injection deque depth → the call keeps
  the placeholder fallback.
- **Ranking integration**: instantiated overloads register in
  `namespace_fn_overload_sets`; `CirBuilder::call_target_funcdef` ranks them
  at CIR time. `parseCallFunc` re-ranks with the USER-WRITTEN args for
  default-filling/arity (a winner supplies both; NO winner ⇒
  `late_bound_no_winner` skips the parse-time arity throw — CIR decides).
- `static_assert`s inside instantiated bodies are consumed UNEVALUATED
  (`fn_template_instantiation_depth` joins the
  `parsing_template_instantiated_member_body()` gate) — variable templates
  (`__integer_to_chars_is_unsigned<_Tp>`) are not evaluable.

General bugs fixed because the instantiations flushed them out:

1. **`TokenCallFunc::user_argc`** — parse-time default-arg filling appended
   one overload's defaults (`size_t* __idx = 0` → literal `0`), and CIR
   re-ranking then scored `0` (int) against the `size_t*` param as -1,
   VETOING the very overload that supplied the defaults. Ranking now
   considers only user-written args.
2. **Function-to-pointer decay** in `score_arg_to_param` (cir_builder): a
   bare function argument binds a `DataDefFPTR` param (score 4; other arg
   positions discriminate between fn-ptr overloads).
3. **`current_namespace` restore** (parser, statement-level
   `ns::member(...)` handling): it SET the namespace then **cleared** it
   instead of restoring — harmless at top level, fatal inside any
   namespace-context parse (instantiated bodies registered their overloads
   under key `::__stoa`).
4. **`struct X {...} const var;`** — cv-qualifiers between the class `}` and
   the declarator now skip (libstdc++'s `_Save_errno` shape).
5. **Local-class reuse across instantiations** — a 2nd instantiation
   re-defining the body's LOCAL class reuses the first definition
   (gated on `fn_template_instantiation_depth > 0` + complete class; v2
   limitation: a param-DEPENDENT local class would silently reuse the first
   layout — none in the hot libstdc++ paths).
6. **Eager method bodies inside instantiations** — system-header lazy
   deferral is suppressed at depth>0 (a local class's ctor/dtor is
   referenced by cleanup, which never triggers the lazy-on-call path).
7. **`class_dtor_symbol` → `call_emit_symbol`** — a function-local class's
   dtor registers under a NESTED unique symbol on `local_emit_name`; reading
   only `emit_symbol` emitted a dangling `Cls___dtor` cleanup reference.

### 2.3 testfstream rewrite + remaining walls (`883c26e`)

- `tests/testfstream.mad` is standard C++ (g++ AND clang++ validated,
  identical output), with `tests/testfstream.flags`
  (`--std=c++17 --no-embedded-headers`) and `.expect` — the
  testloop/testdefer pattern. Runs in ~1.3s (no `.timeout` needed).
- **Fortify builtins**: `__builtin___memcpy/memmove/mempcpy/memset_chk` +
  `__strncpy/strncat_chk` map to `__madc_builtin_*` wrappers
  (lexer.cpp define_map + va_helpers.cpp, the existing strcpy_chk pattern).
- **C++ [namespace.udir] unqualified-call fallback**: `using namespace std`
  imports SKIP names a global already claims, so with `<cstdlib>`/`<cstring>`
  in the TU, `getline(inf, line)` bound POSIX
  `::getline(char**, size_t*, FILE*)` and died on arity. Directives are now
  recorded (`Program::active_using_namespaces`) and
  `using_namespace_call_fallback` (hooked at the `ns_resolved:` function
  block) rebinds to a directive namespace's member when the global's arity
  rejects the queued arg count. The std::getline placeholder accepts
  anything; Pattern A binds it mangled-direct as before.

## 3. Deferred v2 gaps (deliberate, documented)

- **Zero-element parameter packs**: `__stoa(&strtod, "stod", str, idx)`
  (stof/stod/stold and ALL wstring sto* without `__base`) deduces no pack
  element → no instantiation → placeholder fallback. Harmless TODAY because
  un-called bodies aren't emitted — but a program CALLING `std::stof` will
  hit `import of undefined item __ns___gnu_cxx___stoa`. Fix = pack ELISION
  in substitution (drop `_Base... __base` param + `__base...` uses).
- **Template-id-shaped param deduction** (`basic_istream<_CharT,_Traits>&`):
  rejected by the v1 deducer — intentionally, it doubles as the safety gate
  keeping `std::getline` on Pattern A (mangled-direct, correct since getline
  IS exported). Body-instantiating those is only needed if a non-exported
  template with template-id params shows up.
- **First-wins deduce conflicts**: `f(T,T)` called with (int,long)
  instantiates `f<int>` instead of erroring — ranking still scores the call;
  acceptable looseness, noted here for the day it surprises someone.
- The capture/seed only fires for templates **with bodies** parsed outside a
  `deferred_function_body_sink`; signature-only templates keep exactly the
  old placeholder behavior.

## 4. Next work (in priority order, from the prior queue)

1. ~~**std::string `a+b` real-header SIGSEGV**~~ **DONE 2026-06-10**
   (`23027f7`, fulltest 548/0/0/26, torture failset byte-identical, SMAUG
   boots): free namespace operators returning a class BY VALUE now bind
   mangled-direct via the Itanium sret/__retbuf shape
   (`resolve_free_operator_byvalue` + `emit_free_operator_byvalue`); decl
   inits construct straight into the variable (copy elision, g++ canon);
   parser types `a+b` via `free_binary_operator_return_class`. Root cause
   was twofold: the W2 binder declined by-value returns AND the decl path
   silently dropped the construction (uninit string → dtor freed garbage).
   Pinned by `tests/teststringplus_realhdr.{mad,flags,expect}`.
   IMPORTANT when verifying: the reducers ONLY reproduce under
   `--std=c++17 --no-embedded-headers` — flagless runs take the embedded
   path and mask the bug.
   Spawned follow-up gaps (real, distinct, NOT regressions):
   - `a + "literal"` — `operator+(const string&, const char*)` is NOT
     exported by libstdc++ (only `(PKc,str)`, `(char,str)`, `(str,str)`
     are) → needs fn-template BODY instantiation for free operators.
   - `cout << s` where `s` is a const-string&-PARAMETER (reducer
     `tmp/rK.mad`) — pre-existing c2mir check error, unrelated to a+b.
   - The decl path still SILENTLY drops a class init when no ctor matches
     (`if (cc)` in translate_block) — should become a loud error.
2. **W2 OPERATOR-path re-mangle (step D)** — see
   `docs/plans/2026-06-09-emit-symbol-unification-HANDOFF.md`.
3. **Torture parity gap** (Track 1.3, the promote gate): 1567 vs asmjit's
   1645 — worklist `docs/parity/root-cause-worklist.md`.
4. (If prioritized) the §3 pack-elision gap — `std::stof`/`std::stod`.
5. ~~**`#include <cstdio>` DEFAULT-mode wall**~~ **DONE 2026-06-10**
   (`8a897f8` + `e0e8…` regen, fulltest **549/0/0/26** exit 0, torture
   failset byte-identical, SMAUG soaks with the new prototypes): the
   embedded stdio.h shim now declares the full C89 stdio surface (the
   ctype.h/<cctype> precedent — a using-declaration needs a global decl
   to bind to) incl. a real-layout `fpos_t` and REAL VARIADIC
   printf/scanf-family prototypes (the old "stay on dlsym" limitation
   predates variadic prototype support). Pinned by
   `tests/testcstdio.{mad,expect}` (default-mode `<cstdio>` +
   `std::printf` + a+b/c_str/printf interop = the cstr4 shape).
   STILL OPEN from this wall: the diagnostic misattributes a header's
   line number to the MAIN file's name (`tmp/x.mad:99` for cstdio:99) —
   separate lexer file-tracking bug.

Note: `scripts/check-no-std-hardcoding.sh` had been RED (250 lines) since
the 2026-06-08 nlohmann/json vendoring — all third-party false positives +
one comment; fixed in `b0519de` (json.hpp excluded like doctest.h). It
gates fulltest's exit code, so flagless-green fulltest claims between
06-08 and 06-10 were test-green but gate-red.

## 5. Reducers on disk (tmp/, gitignored)

| file | proves |
|---|---|
| `tmp/ts5.mad` | `char c = s[1]` → `b` (alias-ref deref) |
| `tmp/ts4.mad` | `char *p = &s[1]` → `b` (addr-of subscript) |
| `tmp/ts1.mad` / `ts1a` / `ts1b` | to_string + stoi / each alone |
| `tmp/ft1.mad` | deduced fn-template instantiation (`foo::len10(u)` → n=4) |
| `tmp/ft2.mad` | explicit template args (`foo::conv<long,int>(42)` → r=42) |
| `tmp/gl1.mad` | unqualified getline w/ `<cstdlib>`+`<cstring>` (udir fallback) |
| `tmp/cstr3.mad` / `cstr4.mad` | string a+b (GREEN since 23027f7; run WITH the real-header flags) |
| `tmp/rG.mad` / `rI.mad` / `rL.mad` / `rA.mad` | a+b decl-init / assignment / call-arg / combined (all GREEN) |
| `tmp/rK.mad` | OPEN: `cout << s` for a const-string&-PARAMETER (pre-existing) |
| `tmp/cstdio_probe.mad` | OPEN: default-mode `#include <cstdio>` fpos_t wall (run FLAGLESS) |
| `tmp/canon1-3.cpp` | the g++/clang canon evidence (notes in docs/plans/refs/) |

## 6. Gates (after EVERY behavior change; a task is not done without them)

1. `( timeout 1500 make -C src fulltest )` → **must stay 547/0/0/26**.
2. `( timeout 3500 make -C src gcctest ) > tmp/gcctest_X.log` then
   `diff <(grep -E "^FAIL|^TIMEOUT" tmp/gcctest_X.log | sort) tmp/failset_lsq.txt`
   → empty (or strictly fewer; update the reference).
3. SMAUG soak: `cd /workspace/MadSMAUG/runtime/area; timeout 50
   /workspace/madc/bin/madc --project /workspace/MadSMAUG/compile_commands.json
   -lcrypt 4000` → exit **124** AND log contains `Realms of Despair ready at`.
4. `scripts/check-no-std-hardcoding.sh`, `scripts/check-call-emit-symbol.sh`
   (both wired into fulltest). Cap every run `( ulimit -t 120; timeout 180 … )`,
   ONE heavy job at a time.

## 7. Gotchas for the next agent

- **DBG() is thread_local-dead on parser/cir_builder worker threads** —
  real-header parses run on workers; use the gated `std::cerr` probes:
  `-DMADC_DEBUG_FNTPL=1` (fn-template seams), `-DMADC_DEBUG_ALIASREF=1`
  (alias-ref store/consume). `make` does NOT track flag changes — `touch`
  the file before rebuilding with a `-D` flag, and rebuild normal (touch
  again) before gating.
- The reverted-approach warning stands: a blanket `TokenSubscriptExpr`
  operator[] dispatch in cir_builder regressed 9 embedded container tests;
  the landed head-only/class-only parser-side gate is the proven shape.
- `git log` one-liners for this session intentionally tell the story —
  read the commit bodies (`git log 530ba41..42831d7`) before re-deriving
  any of §2.
- The fn-template instantiation hook makes failures non-fatal but still
  PRINTS the parse error to stderr (throwbuf::sync prints before throwing)
  — a scary-looking error followed by a working program can be a failed
  speculative instantiation that fell back correctly.
