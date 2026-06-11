# HANDOFF 2026-06-11 — eval leftovers B+A0+A DONE; the next-work queue (user-prioritized)

**This is the CURRENT cold-restart contract.** It supersedes
`docs/plans/2026-06-10-all-reds-green-HANDOFF.md` (whose §4 queue is now
partially stale — items 1/2/5 done earlier, item 3b's "increment 3 leftovers"
done THIS session). Read that doc only for its §5 reducer inventory and §3
deferred-v2 template gaps, both still accurate. On resume: run
`bash scripts/resume.sh`, read this file, then TRUST THE LIVE REPO.

---

## 1. State snapshot (verified, not aspirational)

- **Branch:** `develop` @ `8dfa827`, clean tree, **18 commits ahead of
  origin/develop and DELIBERATELY UNPUSHED** — see §2 rule 1.
  `feature/eval-leftovers-claude` (same head minus nothing — develop was
  fast-forwarded to it) is pushed to origin as a WIP backup.
- **Gates, all verified ON THE MERGED DEVELOP TREE:**
  - fulltest **557 passed / 0 failed / 0 timed out / 18 skipped**, make
    exit 0, BOTH check gates GREEN (retire-std-hardcoding 0 lines;
    call-emit-symbol drift 0).
  - `make -C src test` green: `test_libmadc_program` **97/38**,
    `test_mangle` 49 cases (incl. the new madc::value + text-carrier pins).
  - gcc.c-torture **1567 / 31 / 56 / 1** (passed/compile-fail/runtime-fail/
    timeout), failset **byte-identical** to `tmp/failset_lsq.txt` (sorted-set
    compare; remember TIMEOUT lines don't match `^FAIL` — grep
    `^FAIL|^TIMEOUT`).
  - SMAUG boots: soak exit 124 + `Realms of Despair ready`.
- **MIR fork:** `/workspace/mir` develop @ `2ffebff` (= `MIR_COMMIT`),
  unchanged this session.
- **Mirrors are synced** (claude_status.json, CHANGELOG [Unreleased],
  docs/test-status.md, ROADMAP, KG, ~/.claude memory). claude_status.json
  is the canonical snapshot; its `head` describes this state.

## 2. User directions captured this session (RULES — do not violate)

1. **NEVER push `develop` to GitHub without a `/release` that bumps the
   version** (verbatim: "don't push to github without a /release to bump
   the version"). Feature branches may merge to develop LOCALLY; develop
   publishes only as part of a versioned release. Recorded in
   `~/.claude/.../memory/feedback_workflow.md`.
2. **100% gcc-torture parity is NOT the goal.** Much of the remaining
   failset is gcc-only builtin/extension material, not standard C23. The
   task is to find the actual dividing line (§3 item 1) and re-document the
   promote gate accordingly — not to grind the tail blindly.
3. **Priority order set by the user:** failset classification audit FIRST,
   then the template-instantiation batch (pack elision + a+"literal" + the
   two small adjacent fixes) — "a little more useful" than torture-tail
   grinding. `<=>` joins the C++ compliance track; `===` is a madc-dialect
   feature for the value/eval track.

## 3. The next-work queue (in the user's priority order)

### Item 1 — Failset classification audit (NO CODE; the dividing line)

**Goal:** classify every entry of the 88-line failset and produce the
defensible promote-gate number. Today the gate (branching.md / ADR 0001)
says "match or exceed master's 1645" — master's asmjit backend proves 1645
is *achievable*, but the user judges part of the 78-test gap (and certainly
the 40 even asmjit never passed) to be gcc-only material that should be
formally SKIPPED, not chased.

**Inputs:**
- `tmp/failset_lsq.txt` — the 88 current failures (31 compile + 56 runtime
  + 1 timeout `pr60003.c`). Regenerate any time:
  `make -C src gcctest` then `grep -E '^FAIL|^TIMEOUT' <log> | sort`.
- `docs/parity/root-cause-worklist.md` — the existing ~16 root-cause
  clusters (some entries pre-date recent fixes; verify against the live
  failset).
- `/workspace/madc-asmjit` — the asmjit-backend worktree (the 1645 oracle):
  for each failing test, does asmjit/master pass it? That splits the 88
  into "asmjit passed it" (achievable-78) vs "nobody passed it" (~40 →
  wait: 1685−1645=40 is asmjit's own failset; the overlap needs computing,
  do not assume).
- The test sources themselves: `gcc_testsuite/gcc.c-torture/execute/…`.

**Method per test:** read the source; tag one of
  (a) **standard C89–C23** — a real compliance bug madc must fix;
  (b) **GNU extension used by real-world code** — worth supporting (the
      computed-goto / case-ranges precedent);
  (c) **gcc-internal / torture-only construct** — formally skip (e.g.
      `__builtin_apply`-class, gcc-specific nested-function tricks).
Note the failing diagnostic for (a)/(b) — it usually names the cluster.

**Deliverables:** a classification table (suggest
`docs/parity/failset-classification.md`); the revised gate number =
1685 − |class (c)| (and any (b) the user de-scopes); edits to
`docs/adr/0001-cir-c2mir-backend.md` + `.claude/rules/branching.md` +
`docs/rules/branching.md` re-defining "parity" as that number; ROADMAP
Track 1.3 note. **Get the user's sign-off on the (b)/(c) split before
editing the gate docs** — it's their call which extensions matter.

### Item 2 — Template-instantiation batch (ONE campaign, shared machinery)

All four sub-items live in/around the namespace fn-template body
instantiation machinery that landed 2026-06-10
(`Program::instantiate_namespace_fn_template_for_call`, parser.cpp:23536,
called from the parseCallFunc tail at :9638). The 2026-06-10 handoff §3
lists the deliberate v2 gaps this batch closes part of.

**2a. Pack elision — `std::stof`/`std::stod`/`std::stold` (+ all wstring
`sto*` without a base param).** libstdc++ implements the sto* family over
`__gnu_cxx::__stoa`:
```cpp
template<typename _TRet, typename _Ret = _TRet, typename _CharT,
         typename... _Base>
_Ret __stoa(_TRet (*__convf)(const _CharT*, _CharT**, _Base...),
            const char *__name, const _CharT *__str,
            std::size_t *__idx, _Base... __base);
```
stoi/stol deduce `_Base... = {int}` (works today); stof/stod's `strtod(const
char*, char**)` deduces an EMPTY pack — the v1 deducer can't, so no
instantiation happens and a call dies at MIR link with
`import of undefined item __ns___gnu_cxx___stoa` (invisible until something
CALLS stof — uncalled bodies aren't emitted; no current test does).
**Fix:** pack ELISION in substitution — when a pack deduces empty, drop the
pack parameter from the instantiated signature AND every `__base...`
expansion in the body/calls. Write the failing test first
(`string s = "3.14"; double d = std::stod(s);` under BOTH default mode and
`--std=c++17 --no-embedded-headers`).

**2b. `a + "literal"` — free-OPERATOR body instantiation.** libstdc++
exports `operator+(const string&, const string&)`, `(const char*, string&&)`
shapes etc., but NOT `operator+(const basic_string&, const char*)` — it's
an inline template, never explicitly instantiated. The W2 Pattern-A binder
(`std_free_operator_instantiation`) binds EXPORTED symbols mangled-direct;
for the non-exported shapes it needs what 2a builds: instantiate the free
operator TEMPLATE's body on demand. Same machinery, operator spelling.
**CAUTION:** reducers reproduce ONLY under `--std=c++17
--no-embedded-headers` (the embedded path masks it — this caused a wrong
"already fixed" call on 2026-06-10; see memory `reducers-need-flags`).

**2c. Decl-path SILENT ctor-drop → loud error.** When a class declaration's
init finds no matching ctor, translate_block's `if (cc)
m_pending_stmts.push_back(cc);` (cir_builder.cpp:1030/1062/1164 — the
pattern guards a possibly-NULL ctor call) silently drops the construction —
the a+b SIGSEGV hid behind this for days. Make the no-ctor-match case a
loud `error_node` (match the "unhandled expression" precedent at the
translate_expr tail). Expect to surface latent cases: run fulltest + the
torture failset-diff; any new red is a REAL pre-existing silent drop.

**2d. `cout << s` where `s` is a const-string&-PARAMETER.** Reducer
`tmp/rK.mad` (tmp/ is gitignored — if gone, reconstruct: a function taking
`const std::string &s` doing `cout << s`, run with `--std=c++17
--no-embedded-headers`). Pre-existing c2mir check error, unrelated to a+b;
attribute with the 3-way method (gcc + clang + stock `/workspace/mir/c2m`)
before touching anything.

### Item 3 — `<=>` (C++20 three-way comparison) — compliance track

**Current state (verified):** the lexer tokenizes it (`src/lexer.cpp:2878`)
into `Token3Way` (`include/tokens.h:769`, `tk3Way`), and that is ALL —
**zero references in parser.cpp or cir_builder.cpp**, no KG node, and NOT
listed in `docs/plans/cpp-support.md` (an unlisted compliance gap — add it
there first).

**Staged plan:**
1. Builtin scalars: parse `a <=> b` as a binary operator; CIR-lower to the
   `(a>b)-(a<b)` int shape... BUT C++ semantics return
   `std::strong_ordering`/`partial_ordering` (from `<compare>`, a REAL
   header — the values are constexpr class objects). For madc-mode a
   pragmatic int result may be acceptable; for `--std=c++20` faithfulness
   needs the category types. Decide with the user; gate the token at the
   C++20 std floor via the LanguageStd enum either way (it currently lexes
   UNGATED — check `--std=c++17` rejects it once parsing exists).
2. Class `operator<=>` overloads — ride the existing member/free operator
   machinery + mangled-direct/instantiation for std types.
3. Rewritten candidates (`a < b` → `(a <=> b) < 0`, reversed `==`) — parser
   overload-resolution work; `= default` generation for `operator<=>`.

### Item 4 — `===` (madc-dialect strict equality) — value/eval track

**Current state: does not exist** (no token, no docs, no KG node; `===`
today lexes as `==` then `=`). It is NOT C/C++ — it's the JS/PHP
strict-equality operator, and belongs to madc's polyglot dialect.
**Design sketch:** meaningful over `madc::value` (the A0 unification just
created the substrate): `a === b` ⇔ same kind AND same value (no
coercion), vs `==` which may coerce. Gate STRICTLY to STD_MADC via the
LanguageStd enum (in `--std=c`/`c++` modes `===` must keep its current
parse). First consumer: the eval expression DSL (whose string-compare
semantics package B just defined — the rewrite pass in madc_program.cpp is
where DSL compare semantics live). Needs a lexer token (Token3Eq?), DSL
semantics, and a decision on whether the REAL language (not just the DSL)
supports it for `madc::value`/`array` operands.

### Item 5 — eval package C (background, the 38 unit skips)

`register_function` host callbacks (the callback pointer must register as a
MIR import), `get/set_global`, string call marshalling
(`native_type_from_datadef` should reuse `DataDef::marshals_value_text`),
fork-child execution, AOT save/load, limit-rejection shapes. Categorized in
the `tests/unit/test_libmadc_program.cpp` suite-head comment; plan:
`docs/plans/2026-06-10-libmadc-eval-on-cir-plan.md`. Plus the A0.2
builtin-retirement queue item (ddARRAY itself earns retirement once the
header-defined class model can express it).

## 4. What landed this session (context for the code you'll touch)

Eval leftovers plan
(`docs/superpowers/plans/2026-06-10-eval-leftovers-B-A0-A.md`, spec
`docs/superpowers/specs/2026-06-10-eval-leftovers-design.md`) — all 13
tasks done. 14 commits `b144571..8dfa827`; the informative ones:

- `e60c466` + `66eeaaf` — **A0**: MadValue/MadArray deleted; script `array`
  IS public `madc::value` (ddARRAY `canonical_cpp_spelling = "madc::value"`);
  kind-safe extern-C boundary (`ns_common::value_array_for_write` /
  `value_object_for_write` — stderr + per-thread dummy, never a C++
  exception into MIR-JIT frames).
- `fe06468` — **mangler**: a substitution back-ref used as a name PREFIX
  keeps the `N..E` wrap (`RNS_5valueE`, was `RS_5value`; only `St` stands
  unwrapped). Pinned by 4 g++-verified literals in test_mangle.
- `daf04ed` — **`<ns_madc>` declaration-only mangled-direct**;
  `src/ns_madc.cpp` = real `namespace madc` impls + extern-C C-host API.
  GENERAL fix: declaration-only namespace OVERLOADS bind their Itanium
  symbol on `emit_symbol` (the old tracking set a bodiless `__ns_*__oN`
  local_emit_name — first decl-only overload set ever).
- `86e0a48` — **string capture**: `DataDef::marshals_value_text()` is
  DEFINED IN `src/madc_mangle.cpp` — the no-std-hardcoding gate's ONE
  permitted home; the plan's literal `is_string_class` is **gate-banned by
  name**. Spellings compare via Itanium encoding (pre-C++11-ABI
  `std::basic_string` ≠ `__cxx11` — correctly distinct types).
- `1627e62` — **call-site scope capture**:
  `Program::runtime_eval_scope_public_target` rebinds madc:: publics to
  `_ctx` sibling overloads under the per-family engine gates (both
  TokenCallFunc construction sites; qualified `madc::X()` arrives via the
  `ns_resolved` label). THREE root fixes: TokenScopeContext lowers to the
  ctx array LVALUE (legacy N_ADDR double-wrapped against `array&` reference
  params — host read a pointer slot as kind::null); `Program::decl_init_self`
  stops `int x = madc::eval_*(…)` capturing x inside its own initializer;
  `set_variable_from_value` installs captured STRING locals.
- `b99d38e` — typed-out `eval_expression(long&/double&,…)` `_ctx` siblings
  (every public the rebind targets MUST have one).

## 5. Gates & commands (run after every change; cap everything)

- Cap: `( ulimit -t 120; timeout 180 <cmd> )` — scale up for build/test;
  ONE heavy job at a time.
- Build: `touch` edited sources first (NAS mtime trap), `make -C src`,
  verify the `obj/*.o` actually recompiled and `grep -c "warning:"` = 0.
- `make -C src fulltest` — expect **557/0/0/18** + exit 0 + both gates
  GREEN. (`make -C src` does NOT relink `bin/test_*` — only `make -C src
  test` does.)
- Torture (after parser/cir/shared-lowering changes):
  `( ulimit -t 3600; timeout 5400 make -C src gcctest ) > tmp/g.txt 2>&1`
  then `grep -E '^FAIL|^TIMEOUT' tmp/g.txt | sort | diff - <(sort
  tmp/failset_lsq.txt)` → empty.
- SMAUG soak: `cd /workspace/MadSMAUG/runtime/area` then `( ulimit -t 120;
  timeout 50 /workspace/madc/bin/madc --project
  /workspace/MadSMAUG/compile_commands.json -lcrypt 4000 )` → exit 124 +
  grep `Realms of Despair ready`.
- C++-only changes don't need the torture/SMAUG soak (both are C89) — but
  say so explicitly in the commit.

## 6. Gotchas (every one of these bit a session)

- **Reducers need their flags**: real-header bugs reproduce ONLY under
  `--std=c++17 --no-embedded-headers`; flagless = embedded path = masked.
- **ns_* embedded headers (incl. `<ns_madc>`) need `std::string` declared
  first** — scripts/`write_file` test sources must `#include <string>` (or
  `<iostream>`) before them, or you get a misattributed "undeclared
  identifier 'string'".
- **madc::program captures std::cout** — doctest output swallowed; use
  `bin/test_libmadc_program --out=FILE` (and `-tc="pattern*"`).
- **The no-std-hardcoding gate bans type-identity predicates BY REGEX**
  (`is_std_string|is_string_class|\bstring_[a-z]|…`) everywhere except
  `src/madc_mangle.{cpp,h}` — read `scripts/check-no-std-hardcoding.sh`
  BEFORE adding any string/stream-identity check; the knowledge belongs in
  the mangler, callers ask marshalling questions.
- Verbose-gated DBG diagnostics exist at the capture boundaries
  (`__madc_scope_set_int_runtime`, `runtime_eval_context` — ctx address,
  kind, size): run with `-v` when scope capture misbehaves.
- DBG is thread-dead on parser workers; never gate errors behind it.
- Failed speculative fn-template instantiations PRINT an error then fall
  back fine (don't chase those ghosts).
- `tmp/` is gitignored — reducers named in handoffs may need recreating.

## 7. Where everything lives

- Canonical snapshot: `claude_status.json` (head names this handoff).
- This queue's user rationale: §2 above; KG `Decision
  {torture_parity_dividing_line_audit}` + the 2026-06-11 eval Feature/
  Decision nodes.
- Eval architecture + remaining package C: memory `libmadc-eval-track`,
  plan `docs/plans/2026-06-10-libmadc-eval-on-cir-plan.md`.
- Parity worklist: `docs/parity/root-cause-worklist.md`; failset baseline
  `tmp/failset_lsq.txt` (gitignored — regenerate via gcctest if absent).
- Prior handoff (reducer inventory §5, deferred template-v2 gaps §3):
  `docs/plans/2026-06-10-all-reds-green-HANDOFF.md`.
