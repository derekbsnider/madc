# RETIRE-STD-HARDCODING CAMPAIGN — HANDOFF (2026-06-02) — READ FIRST

> Self-contained resume doc for the "retire ALL std:: hardcoding" campaign. Read this +
> the spec + the plans, then continue. This is a SEPARATE track from the gcc-torture parity
> campaign (that lives on `develop`; see `2026-06-01-HANDOFF.md`). This campaign lives on its
> own feature branch and has NOT been merged to develop.

## ⏩ STEP 0 — orient
```
bash scripts/resume.sh                     # live git/reflog/fork/build state
git -C /workspace/madc branch --show-current   # expect: feature/retire-std-hardcoding-claude
git -C /workspace/madc log --oneline develop..HEAD
```
The mangler unit tests are the campaign's safety net — run them anytime:
```
make -C src obj/madc_mangle.o 2>/dev/null; make -C src   # (rebuilds madc_mangle.o)
( ulimit -t 60; g++ -std=c++11 -Iinclude tests/unit/test_mangle.cpp obj/madc_mangle.o -o tmp/test_mangle ) && ./tmp/test_mangle
```

## 0. THE PRINCIPLE (never drift from this — the user enforced it HARD)

**madc hardcodes ONLY the C/C++ primitive basis; every other type is COMPOSED from it.** This is
the whole point of C/C++: a tiny set of primitives (`void`/`char`/`int`/`long`/`float`/`double`/
`bool` + the composition mechanisms: pointer/array/struct/union/enum/function), and **everything
else — `std::string`, every stream, every container, every user type — is an ordinary composed
`DataDefCLASS`/`DataDefSTRUCT` built by parsing a declaration**, NOT a privileged builtin.

Consequences (the end state):
- **No per-type code** — no "string handling", no "stream handling". A use site (`cout << x`,
  `s.length()`, `outf.open(f)`, `a + b`) is resolved by ONE generic path: overload resolution
  (incl. non-member operators) → **mangle the chosen declaration** → ABI from the declaration
  (size, base offsets, sret) → emit the call → linker resolves against libstdc++ (madc is a C++
  front-end against real libstdc++, exactly like g++/clang++ + the linker).
- **No per-type DataType tag or builtin DataDef** for any non-primitive (kill `dt*STREAM`,
  `dd*STREAM`, the `sizeof(std::…)` builtins). std:: types live in `#include <…>` headers (data).
- **No hardcoded mangled-symbol literals** — `madc_mangle` is the single source of every symbol.
- **No wrappers/shims** (`string_concat`/`streamout_*`/`ifstream_open`/`sstream_*`/`__std_*`).
- **The ONLY hardcoded std:: data is the auto-include symbol→header trigger map** (config, not logic).
- **madc itself contains NO reference to a real std:: type** (no `#include <string>`, no
  `sizeof(std::string)`); layout is derived from the parsed header and cross-checked **in a
  DOCTEST only** (the test `#include`s the real headers; madc does not).

Authoritative spec: **`docs/superpowers/specs/2026-06-02-retire-std-hardcoding-design.md`** (read it).

## 1. RULES OF ENGAGEMENT (the user re-enforced these repeatedly; memory `feedback_correct_over_shortcuts`)

- **SHORTCUTS ARE CATEGORICALLY UNACCEPTABLE.** The hardcoded `_ZSt` stream literals were a
  shortcut (dodging a 1-line `madc_mangle` bug) that caused DAYS of drift. RED-FLAG TELLS = about
  to hardcode a literal / add a wrapper-shim / special-case higher up / think "good enough for now"
  → STOP and fix the deepest layer.
- **RULE #1: GCC/G++/CLANG IS CANON.** Every mangled symbol/layout is verified against the real
  toolchain (`c++filt`, `nm -D libstdc++`, `g++ -S`, `sizeof`/offset probes) BEFORE it is asserted.
- **"WAIT" MEANS PAUSE AND TALK — NEVER revert.** No `git checkout` over uncommitted work.
- **KISS** — no invented jargon; use struct/class/object. Fix at the deepest layer; no shims.
- **Gate every change:** `make -C src test` (capped `( ulimit -t N; timeout M … )`) + integration
  `bash scripts/run_tests.sh` + (for codegen changes) coordinator re-runs SMAUG soak. Commit, push.

## ⭐ 2026-06-02 (cont.) — LAYER-3 TRUTH · THE GATE · THE KEYSTONE MAP (READ BEFORE §2)

**THE RECURRENCE EXPLAINED (why it "kept sneaking back" for a week).** There are THREE
layers of std:: hardcoding; sessions kept declaring "done" at Layers 1–2 while Layer 3 —
the *builtins* — was NEVER removed. Git-verified: `DataDefSTRING`/`add_string_methods`
have NO removal commit, are ancient, and are present on `master` too.
- **Layer 1 — dtSTRING/tk*/ns_stl TAGS → RETIRED** (P2.14; grep 0). Real.
- **Layer 2 — `_ZSt` literals → SWEPT** (mangler complete + cir_builder sweep). Real.
- **Layer 3 — builtin `ddSTRING`/`DataDefSTRING(sizeof(std::string))`, `add_string_methods`,
  stream `dt*STREAM`/`dd*STREAM`/`DataDef*STREAM`, `add_fstream_methods`,
  `ifstream_*`/`ofstream_*` wrappers, every `sizeof(std::…)` → NOT DONE.** THIS is the work.

**THE CONTRACT (user-enforced, hard).** "DONE" = `scripts/check-no-std-hardcoding.sh` prints
GREEN **and** `make -C src fulltest` passes **and** SMAUG soaks clean. NEVER a behavioral
"it works." The gate is committed + **wired into `make fulltest` (suite is RED until 0)** +
fails on tombstone comments too ("gone without a trace — git is the record"). Baseline 473;
**currently 469** (tombstones removed). Report the live count every step. No faked
intermediate drops — the next drop is the streams migration (no safe drop exists before the
keystone lands).

**THE KEYSTONE (what unlocks deleting the builtins — does NOT exist yet).** The generic
mechanism: *a bodyless method in a `std::` header class → a `DataDefCLASS` (layout from
declared members — already works for vector) whose method auto-binds `emit_symbol` to the
mangled libstdc++ symbol.* Precise map (subagent-verified, all src/parser.cpp unless noted):
- Classes parse via **`TokenCLASS::parse`** (NOT TokenSTRUCT::parse). Member-fn recognized by
  peek-`(` (~11682); `FuncDef` built by `parseFunction(...)` (~11737), pushed to
  `ddc->methods`/`method_map` (~11740-11752). ctor ~11581-11611, dtor ~11554-11578,
  operator ~11642-11676 (falls through the method branch).
- **Bodyless methods ALREADY parse** (parseFunction bodyless branches 14586-14625 & 14639-14646
  → create `Method`, no body, `return`). ✅ DONE increment 1: added `FuncDef::declaration_only`
  (include/madc.h, ctor-initialized false), set true in both branches → exposes "bodyless".
- **HOOK (next):** in `TokenCLASS::parse` ~11745 (after `mfd->returns_ref=…`), if
  `pgm.current_namespace=="std"` AND `mfd->declaration_only`, set
  `mfd->emit_symbol = <mangler call from the parsed signature>`. Mangler entry points:
  `itanium_mangle_{member,ctor,dtor,operator}_sub` (src/madc_mangle.cpp); `emit_symbol`
  consumed by `CirBuilder::emit_symbol_method_call` (src/cir_builder.cpp:2911/2977).
  `current_namespace` set in `TokenNAMESPACE::parse` ~10043.
- **THE HARD CORE (no shortcut):** the symbol needs the FULL canonical template-id
  (`std::__cxx11::basic_string<char,std::char_traits<char>>`,
  `std::basic_ofstream<char,std::char_traits<char>>`). RECONSTRUCT it from
  `current_namespace` + template name + concrete args — the headers MUST mirror libstdc++
  structure (template params + the inline `__cxx11` namespace). NEVER hardcode a symbol. The
  template-instantiation path (`instantiate_template_use` ~1541) has never produced a
  libstdc++ mangled symbol (vector has bodies), so this reconstruction is new code.
- **THE NEW CLASS-MODEL PIECE:** `DataDefCLASS` has NO base-offset field — single base assumed
  at offset 0 (datadef.h:699/717; base-member copy 11478-11510). `basic_ios` vbase needs a
  real non-zero base-subobject offset (+248) field + offset-aware dispatch, header-derived +
  doctest-checked, **NEVER hardcode 248** (STOP+escalate if not cleanly derivable).

**THE FOUNDATIONAL PIECE (the crux of "no hardcoded symbols").** The mangler needs the FULL
canonical Itanium spelling (`std::basic_ofstream<char,std::char_traits<char>>`), but
`instantiate_template_use` (~1541) names an instantiated class with a SANITIZED tag
(`vector_int`, `basic_ofstream_char_char_traits_char`; arg names joined with `_`, `*`→`p`,
lines 1577-1579) — the canonical structure is LOST. Reconstructing it later is lossy
(args are themselves template-ids). SOLUTION (clean, general, recursive): give each type a
**`canonical_cpp_spelling`** (on TokenDataType or the DataDefCLASS) SET AT INSTANTIATION:
builtins carry the obvious spelling ("char","const char*"); an instantiation sets it to
`"<ns>::<tname><" + join(args' canonical spellings, ",") + ">"`. Since args are instantiated
first, the parent is just string concatenation. Then the hook reads the class spelling +
maps each `mfd->parameters[i]` (DataDef* + ref_params/const_params) to its canonical spelling
(streams need only: void, `const char*`, the openmode enum `std::_Ios_Openmode` → a non-template
std type "std::_Ios_Openmode"); feed `itanium_mangle_{member,ctor,dtor,operator}_sub`. NEVER
hardcode a symbol. (add_string_methods sidesteps this by passing literal spellings — that is the
Layer-3 shortcut being replaced.) Need a `DataDef -> canonical C++ spelling` helper too.

**SEQUENCING:** streams FIRST (isolated, 28 gate-lines) to build+prove the keystone where
blast radius is smallest; THEN std::string (239 `&ddSTRING` sites) rides the SAME mechanism;
delete builtins/callbacks/wrappers/tags as each generic path covers it; gate→0.
Stream mangler doctests already added + green (Task 1, commit 9582beb): test_mangle 44/143.
INCREMENTS: (1✅ 9ad1c35) FuncDef::declaration_only. (2 next) canonical_cpp_spelling on
types + set in instantiate_template_use + DataDef->spelling helper. (3) the hook at
parser.cpp:11744 (+ ctor/dtor/operator blocks). (4) author headers (string, ios/ostream/
istream bases, fstream) mirroring libstdc++ template/namespace structure incl. inline __cxx11.
(5) DataDefCLASS base-subobject offset (basic_ios +248). (6) switch registration + DELETE
Layer-3 builtins/callbacks/wrappers/tags + rewire &ddSTRING sites. (7) gate→0 + fulltest + SMAUG.

## 2. LIVE STATE (verify on resume)

- Branch **`feature/retire-std-hardcoding-claude`**, **pushed** (== origin), **8 commits ahead of
  develop**, tracked tree CLEAN. **develop is UNTOUCHED at `110e026`** (no drift; this campaign is
  isolated; `claude_status.json`/`2026-06-01-HANDOFF.md` reflect develop and remain accurate for it).
- MIR fork pin unchanged (`MIR_COMMIT=8864a73`) — this campaign needed NO fork change.
- Gate: integration **457 pass / 7 fail (6 known feature-gaps + flaky testfortypedcomma) / 55 skip**
  — UNCHANGED throughout (all campaign work so far is mangler-only + a behavior-preserving symbol
  sweep). Unit **7/7 binaries**; `test_mangle` **40 cases / 134 assertions** green. gcc-torture
  UNAFFECTED (pure C, never invokes the mangler). SMAUG boots clean (pure C, never touches streams).

### Commit trail (develop..HEAD)
```
d1e6ace spec: retire ALL std:: hardcoding — primitive-basis design
97e7eb4 plan: mangler part 1 (So/Si/Sd/Ss complete-spec abbreviations)
d03e38d fix(mangle): So/Si/Sd/Ss are complete-specialization abbreviations (no template args)
4d57001 plan: mangler part 2 (non-member std template operators)
71a2f48 feat(mangle): non-member std template operators (getline/endl/operator<</>>)
d9ad564 feat(mangle): std namespace vars + function-pointer types (complete stream symbol coverage)
5ec3072 refactor(cir): generate ALL stream symbols via the mangler — delete every _ZSt literal
6996cb5 plan: file streams as header-defined classes (vbase investigation + offsets)
```

## 3. WHAT'S DONE — W1 (mangler completeness) + the cout/ostream literal sweep

The mangler `src/madc_mangle.cpp` is now COMPLETE — it generates EVERY std:: symbol, each pinned to
the real libstdc++ symbol by `tests/unit/test_mangle.cpp` (verified vs `c++filt`):
- **W1a** (`d03e38d`): `So`/`Si`/`Sd`/`Ss` are **complete-specialization** abbreviations — emit
  standalone, NO appended template args. Root-cause of the hardcoded stream literals: the mangler
  was treating them like `Sa`/`Sb` (template prefixes) → `_ZNSoIc…lsEd` instead of `_ZNSolsEd`.
  Fix: `std_complete_abbrev()` + removed Si/So/Sd from the prefix `std_abbrev()`.
- **W1b** (`71a2f48`): non-member std function templates (`std::operator<<`/`>>`/`getline`/`endl`)
  via `itanium_mangle_std_free_template(name,targs,ret,params)` + `$Tn` template-param placeholders
  (`$T0`→`T_`…). THREE discoveries (all in the commit msg): `$Tn` are substitution candidates (so
  repeat uses become back-refs S4_/S5_); the function-template NAME is substitution candidate #0
  (the +1 slot shift — Itanium `<template-prefix>`; cf. the spec's `first<Duo>` example); function
  templates encode the return type.
- **W1c/d** (`d9ad564`): `itanium_mangle_std_var("cout")`→`_ZSt4cout`; function-pointer types
  (`"R (*)(P)"`→`PF…E`) so the endl manipulator op `_ZNSolsEPFRSoS_E` is generatable.
- **THE SWEEP** (`5ec3072`): `cir_builder.cpp` `stream_object_symbol`/`ostream_insert_symbol`/
  `translate_stream_chain` now generate every symbol via the mangler (function-local `static
  std::string` caches). **`grep '"_Z' src/cir_builder.cpp` is EMPTY.** Byte-identical symbols
  (doctest-proven) → zero behavior change.

## 4. WHAT'S NEXT — the codegen migration (each its own gated step)

**IMMEDIATE NEXT: file streams.** Full plan: **`docs/superpowers/plans/2026-06-02-file-streams-header-defined.md`**.
Key facts already established (g++ probe `tmp/voff.cpp`):
- madc's class model = single-inheritance, base subobject at **offset 0** (`datadef.h:699-722`).
- `ofstream→ostream` offset **0** (so `<<`/`open`/`close`/`is_open` work with the offset-0 model);
  `ofstream→basic_ios` offset **248** (so `good()`/`eof()` need a non-zero base offset — the one new
  class-model piece, derived from the header layout + doctest-checked, **NEVER hardcode 248 in madc**;
  if it can't be cleanly header-derived, STOP and escalate).
- Plan tasks: (1) mangler doctests for ofstream/basic_ios symbols [SAFE first step]; (2) author
  `include/madc/fstream` + ios/ostream/istream bases (layout-faithful); (3) route offset-0 ops
  through generic resolution + mangler + generalize `translate_stream_chain` to any ostream-derived
  object; (4) the `basic_ios` +248 base-offset; (5) DELETE `add_fstream_methods` +
  `madc_stream_runtime.cpp` + `dt*STREAM`/`dd*STREAM` + the `sizeof(std::…)` builtins. Fixes
  `testfstream`/`testloop` → integration 457→459.

**THEN (later sub-projects, spec §):** cin `>>` (`testcin`); string residual wrappers
(`string_concat`/`equals`/`assign` in `madc_mir_backend.cpp`); stringstream + `stoi`/`to_string`
conversions; final grep-gate (`grep -rn "_ZSt\|dt.*STREAM\|dd.*STREAM\|streamout_\|streamin_\|string_concat\|__std_\|sizeof(std::" src/ include/` → 0 outside the mangler + auto-include map);
**codify a `.claude/rules/` rule:** std:: symbols are mangler-generated, never hardcoded literals.

## 5. MANGLER MECHANICS (for whoever extends it next — `src/madc_mangle.cpp`)

- `ItaniumMangler` keeps an ordered candidate table (`keys_`); `subref(n)` → `S_`,`S0_`,…;
  `tparam_ref(n)` → `T_`,`T0_`,…. `encode_type` registers candidates per the Itanium rules; decos
  ("P"/"R"/"K"/"O") wrap the core, each a candidate. `std_abbrev` = St/Sa/Sb prefix abbreviations;
  `std_complete_abbrev` = Ss/Si/So/Sd complete specializations (no args). `canon_type*` produce the
  substitution-independent keys.
- Public API (`include/madc_mangle.h`): `itanium_mangle_{member,ctor,dtor,operator}_sub` (members of
  a template-id class), `itanium_mangle_std_free_template` (non-member std template fns),
  `itanium_mangle_std_var`, the `std_{string,vector,map,set,stringstream}_type()` spellings.
- To add a symbol form: write the failing doctest (the exact g++ symbol via `c++filt`), implement,
  iterate against the oracle (the `MANGLE_DEBUG` candidate-dump technique was used to find the +1
  slot bug — re-add a temporary getenv-gated dump in `mangle_*` if needed). NEVER hardcode a literal.

## 6. NO-DRIFT CHECKLIST (state was left consistent)

- All work committed + pushed on the feature branch; tracked tree clean.
- develop untouched (110e026); the mirrors (`claude_status.json`, `2026-06-01-HANDOFF.md`,
  README/CHANGELOG) reflect develop and are correct for it — do NOT edit them for this campaign until
  it merges to develop.
- Memory `project_cpp_mangled_direct` UPDATED 2026-06-02 with the campaign state (the old "mangler
  can't do substitutions → hardcode the symbols" guidance is marked OBSOLETE — it was the drift
  source). `feedback_correct_over_shortcuts` strengthened (shortcuts categorically unacceptable).
- No half-done code: every commit builds + passes the gate; the sweep is behavior-preserving;
  file-streams is PLANNED but NOT started.
