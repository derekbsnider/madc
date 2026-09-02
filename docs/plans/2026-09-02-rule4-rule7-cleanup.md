# Rule #4 / Rule #7 cleanup — one owner per rule, no names in general machinery

**Status: PLAN (owner-requested 2026-09-02; opened during the darwin-host D4 wave).**
Owner: *"I am deeply concerned at how much of the code is filled with serious
violations of Rule #4 and Rule #7 — we likely need a specific cleanup plan."*

Rule #4 (AGENTS.md): understand what exists before assuming it doesn't — a
helper that already owns a concept gets ADOPTED, never re-implemented.
Rule #7: no hard-coding of specifics (user names, library names, target
facts) into general machinery — data lookups, type predicates, registries.

This plan extends the duplication campaign opened 2026-07-27
(`docs/plans/2026-07-27-angle-bracket-consolidation.md`, `/dupaudit`,
`DupFamily` nodes in `madc-knowledge`) from per-feature recon to a
repo-wide, ranked, gated burndown, and adds the Rule #7 sweep the campaign
never had.

## Why now — the evidence of one afternoon (2026-09-02)

Both families below were found while fixing the darwin suite. Both have the
shape round 1 of the campaign found: **the good owner already existed, was
documented as the owner, and was not adopted.**

| Family | Sites | Owner that existed | Divergence when hit |
|---|---|---|---|
| "does this directory supply header X" (include precedence) | 3 predicates, 2 filesystem-only | `Program::resolved_include_provider_exists()` — "the one existence predicate" per its own comment | header-less Mac: embedded `<stddef.h>` shadowed libc++'s wrapper unit, `#include <iostream>` #errored (fixed 4fbcae4d by adoption) |
| "what is this scalar type's canonical spelling" | 6 spelling formers + 3 DataType→C-spelling switch tables (`DataDef::mangle_scalar_spelling`, parser.cpp `canonical_builtin_simple_type_name`, cir_dump.cpp scalar word) | none adopted; the mangler's table is the only model-aware one (LLP64 `long long`), the other two hardcode `long` | darwin: the pinned int64 dd's display name "int64_t" re-resolved through the SOURCE-spelling table to `long long`; `isL<long>` keyed two ways, deduced `size_t` bound `std::max<unsigned long long>` — the whole D4 identity class (16 tests both arches) |
| "does this typedef name a distinct type identity" (class-scope typedef arm) | 1 arm, 2 identities riding one string test | the identity spelling rule every former uses (`canonical spelling, else display name`); the arm compared the source SPELLING to the display NAME instead — a proxy that also happened to carry the wchar_t/char16_t/char32_t identity | any pinned dd with a canonical spelling minted `typedef _Tp type` as a NEW type; `__is_same(long, remove_const<long>::type)` false; `common_type<long,long>` derived from itself (stack overflow in the freezer) |

Rule #7 density, measured with `git grep` (2026-09-02, HEAD 4fbcae4d):

| File | `name == "…"` / `spelling_is("…")` style compares | of which `"__…"` (intrinsics / ABI names) |
|---|---:|---:|
| src/parser.cpp | 287 | 131 |
| src/cir_builder.cpp | 47 | — |
| src/lexer.cpp | 29 | — |
| src/madc_mangle.cpp | 11 | — |

Raw counts overstate the problem: a keyword spelling (`decltype`, `alignof`,
`noexcept`) IS the grammar. The sample classifies into three kinds, and only
the third is a Rule #7 violation proper:

- **(K) keyword / grammar spellings** — `name == "decltype"`, `spelling_is("typename")`.
  Legitimate as grammar, but they belong in the `--std=`-gated keyword
  registry (`docs/plans/2026-06-15-cpp-keyword-registry-plan.md`), not in
  ad-hoc ladders.
- **(I) compiler-intrinsic ladders** — the type-trait set (`__is_class` …)
  appears in THREE ladders (parser.cpp 13551, 14559, 33215); the
  `__madc_eval_*_runtime` symbol set in three (579, 622, 25155); the complex
  builtins `__builtin_conjf`/`conjf` in three files. One intrinsic table per
  set is a Rule #4 fix; the ladders are also where Rule #7 hides
  (`__builtin_alloca || alloca || malloc` at parser.cpp:15104 decides
  allocation semantics by NAME).
- **(L) library / user names in generic machinery** — `name == "stdin" || "stdout" || "stderr"`
  (21613), `name == "perl"` selecting a policy flag (24873),
  `incfile == "string"` (lexer 1431), `name == "println" → "std_println"`
  (53395). These are Rule #7 proper: replace with flags, registries, type
  predicates, or the embedded-header table.

## The tool — aislop (owner-selected 2026-09-02)

<https://github.com/scanaislop/aislop> — MIT, deterministic (no LLM at run
time; same code, same score), C/C++ supported. Six engines; the ones that
serve this plan:

- **AI-slop detectors**: duplicated helpers (Rule #4, the TEXTUAL class),
  hidden fallbacks and swallowed exceptions (the shim class Rule #2 forbids),
  dead code, todo stubs, oversized functions, narrative comments.
- **Code quality**: function/file size, deep nesting, dead code.
- **Linting**: cppcheck / clang-tidy for C++.

Output: score 0–100 per file and per finding; `--json` / `--sarif`;
`.aislop/history.jsonl` + `aislop trend`. Gate: `aislop ci` exits 1 below
`ci.failBelow` in `.aislop/config.yml`; `--changes --base origin/develop`
gates only the files a branch touched. Available locally
(`npx aislop@latest scan ./src`, `brew install scanaislop/tap/aislop`,
`pipx install aislop`) and as a GitHub Action.

What it does NOT see, so the plan keeps its own instruments for it:

- **Semantic duplication that shares no text** (six angle-bracket scanners,
  three DataType tables with different spellings) — that is `/dupaudit`'s
  marker discipline and the `DupFamily` nodes.
- **Rule #7 name ladders** (`name == "stdin"`, `== "malloc"`) — no detector
  reads intent; these stay the grep markers below, filed as `Gap` nodes.

How it is used here:

1. **Baseline first** (measure before designing): `aislop scan ./src --json`
   on the build container; the compact record is
   `docs/parity/aislop-baseline.txt` (per-rule and per-file counts; the
   4.5 MB JSON stays in `tmp/`). The fixing follows the score order within
   a tier; the scanner does not fix anything (`aislop fix` is mechanical
   formatting only and is NOT run — tabs/style are the repo's).

   **Measured 2026-09-02 (aislop 0.16.0, 80 files): score 32 "Critical",
   5754 diagnostics.** Read with the repo's conventions in hand:
   - **5392 (94%) are four style rules that contradict house law**, not
     defects: `cpp-null-literal` 3107 (the code uses `NULL`),
     `cpp-c-style-cast` 1338, `cpp-iostream-leftover` 547 and
     `cpp-endl-in-stream` 400 (the mandated `DBG(cout << ... << endl)`
     diagnostics). They go in `.aislop/config.yml` as disabled BEFORE any
     ratchet, or the score measures the style guide, not the code.
   - **362 signal findings**: `function-too-long` 181 (max 80 lines —
     parser.cpp 134 signal findings, cir_builder.cpp 75), `cpp-manual-delete`
     79, `too-many-params` 46, `file-too-large` 16 (every core file),
     `todo-stub` 14, `cpp-define-constant` 12, `deep-nesting` 7,
     `unreachable-code` 6 (real: code after return/throw at
     cir_builder.cpp:19486, lexer.cpp:9755, parser.cpp:36978/39619/54298/
     68386 — check each), `security/shell-injection` 1 = `madc_system()`,
     the runtime shim behind the LANGUAGE's `system()` call (by design;
     suppress with a scoped `aislop-ignore-line`).
   - **Zero `duplicated helpers` findings for C++** — the Rule #4 detector
     does not reach this language, exactly the gap the plan assumed.
     Duplication stays `/dupaudit`'s job; aislop's contribution here is the
     size/nesting pressure that makes duplicates easier to see, plus the
     dead-code and todo lists.
2. **Ratchet**: `ci.failBelow` starts AT the measured baseline of the worst
   file and only ever moves UP; `fulltest` runs `aislop ci --changes --base
   origin/develop` so a change cannot lower a touched file's score.
3. **GitHub Action**: an advisory scan job on pull requests (SARIF to code
   scanning). CI stays build-and-smoke plus this report; the battery stays
   the local gate (owner rule 2026-09-01).

## Phases

### P0 — Finish the darwin identity family (in flight, D4 wave 2)

The scalar-spelling family is the first entry and is half done:

- ONE DataType→target-C-spelling table: `DataDef::target_scalar_spelling()`
  (lifted from the mangler's switch; the mangler keeps its alias guard in
  front of it). The two model-blind copies (parser.cpp
  `canonical_builtin_simple_type_name`, cir_dump.cpp scalar word) adopt it in
  their own commit — cir_dump's `long` on LLP64 for a `long long` value is
  the divergence to state.
- The pinned 64-bit dds carry their target spelling as canonical ONLY where
  their display name is not this target's spelling for the type (stamped by
  `madc_stamp_primitive_type_ids`, computed from the tables — no target test).
- `canonical_template_binding_dd` honors that canonical spelling; the
  class-scope typedef arm compares the source spelling to the dd's IDENTITY
  spelling (canonical, else display name) — never restrict it to alias
  bases or add `scalar_alias_of` to it: its alias IS the identity for
  `typedef wchar_t char_type` (canonical "wchar_t"), which is what keeps
  `char_traits<char_type>` keyed wchar_t.
- Gate: `tests/testtplargidentity.mad` (g++/clang++ oracle
  `1 1 1 | 1 1 | 1 | 9 7 9`, identical on every target — the reducer
  deliberately avoids `long` vs `long long` distinctness, which madc still
  conflates on glibc); the `collect_vbases` self-base guard is the loud stop
  for the class.
- Measured (2026-09-02): the darwin pack froze at 66 errors vs 64 before.
  The same corpus frozen with the PRE-fix freezer (a HEAD worktree on the
  container) and the histograms diffed: one line differs,
  `__atomic_is_lock_free` 10→12 — the known cross-freezer dlsym leak, now
  reached by the `long` and `unsigned long` `__atomic_base` instantiations
  that no longer alias the `long long` ones. Baseline set to 66 with that
  reason (`docs/parity/pack-degradation-baseline.txt`). The linux pack is
  byte-identical (93, dk-none 55). A first version of the typedef-arm fix
  (alias bases only) had pushed both packs UP in the wchar_t class — the
  arm's string test was the accidental carrier of the wchar_t identity —
  which is how the next entry below was found.
- **Reproduction without a Mac**: the Linux-hosted cross freezer
  (`bin/madc-arm64-macos --std=c++17 --no-config --no-sysroot-includes
  --emit=c11 <reducer>`) carries the darwin type model (MADC_CROSS_APPLE),
  so every darwin identity reducer runs on the build container in seconds.
  Use it before any Mac round trip.
- **wchar_t (KG Gap `wchar_t_template_arg_identity`): DONE, D4 wave 3.**
  Distinct dds (DataDefPlatformWCHAR / CHAR16 / CHAR32, slots 39–41); the
  three spelling carve-outs deleted. Two more families surfaced underneath
  and are recorded here so the inventory does not rediscover them:
  - **rawtype-as-identity** (representation standing in for type identity).
    Owner: `Program::proven_scalar_identity()` — the fundamental type a
    scalar dd denotes, proven through the one builtin table (cv peeled,
    alias chain walked, identity spelling resolved). Adopted by both scalar
    lanes of `score_arg_to_param`. OPEN copies: the fn-ptr signature
    compare in `score_arg_to_param`, the typedef-redecl dedup's pointer arm
    in `TokenTYPEDEF::parse` (`existing->rawtype() == alias_dd->rawtype()`).
    Divergence to state: a `wchar_t*`/`int*` pair reads "same" in both.
  - **keyword-redeclaration acceptance** (a typedef naming a C++ keyword
    type). Owner: `typedef_alias_spelling` (the one alias acceptor; the enum
    arm's private 3-way switch was a fifth copy and adopted it). The flag
    (`TokenDataType::keyword`) is registration-owned and map-read, the
    `builtin` shape — a first version read it from the token and never
    fired, because the lexer mints a fresh token per occurrence. OPEN: only
    wchar_t/char8_t/char16_t/char32_t are flagged; C89's `typedef int bool`
    must stay legal, so the set is the [lex.key] types C spells as
    identifiers, not every keyword.
  - **one key rule, two resolution orders** (CONSOLIDATED): the bare arm of
    `canonical_arg_key_fragment` resolved a builtin spelling through the
    table while the suffix arm resolved its core through
    `resolve_named_datadef` (datatype_map first → the lexer's literal-typing
    ddINT "int"); `template<> struct N<int*>` keyed intP against int32_tP
    uses. Both arms now resolve table-first (`tests/testptrbuiltinspec.mad`).
  - **stale rows in the one builtin-spelling table** (CONSOLIDATED): the
    owner `Program::resolve_builtin_type_spelling` still mapped
    `long double` to ddDOUBLE and `__int128` to ddINT64 from before those
    dds existed — the SPELLED side of the type model disagreed with the
    DECLARED side (the lexer emits ddLDOUBLE/ddINT128), so K<long double>
    == K<double> and W<__int128> answered for W<long>; and pch.cpp's
    `builtin_datadef_from_spelling` was a full private COPY of the table
    that had drifted further (wchar_t -> ddINT32). Rows fixed, the copy
    adopts the owner (`tests/testbuiltintplkey.mad`). Lesson for P1: a
    table that names types is itself a copy of the type model — every new
    dd must visit it, or better, the table should be derived from the dds.
  - **flattening loses C++ guards** (header text): the darwin prelude is
    flattened under `-x c`, so any `#ifndef __cplusplus` in Apple's C
    headers is resolved away. wchar_t was the one instance in the served
    surface; `gen_darwin_prelude.sh` restores that guard. A second such
    divergence would be a second awk line — the generator owns the rule.
- Banked, not in this slice: `TraitTypeArg::same_as` compares display NAMES —
  two class-scope aliases of one type (`size_type` vs `difference_type`)
  read as different types. C++ says a typedef never mints a type. Fix =
  compare alias roots; needs its own reducer and battery.

### P1 — Inventory (recon only, no fixes)

Run `/dupaudit` per subsystem — NOT per feature branch — and record every
family as a `DupFamily` node (rule, marker, sites, divergence, status):

1. type identity and spelling (the six formers; `cs.empty() ? name : cs`
   fallbacks; by-name table lookups)
2. include resolution and header precedence (three predicates now one owner;
   `is_embedded_header_allowed` / shim classifier still filesystem-only)
3. template instantiation keys (`canonical_arg_key_fragment` call sites;
   the explicit-arg filter compares names)
4. mangling and symbol formation (`overload_spelling_symbol_suffix`, inst_key)
5. overload ranking (CIR rank vs parse-time rank — two rankers?)
6. emitters (C11 / MC11 / dump spell scalars from separate tables)

Rule #7 sweep: the scanner's findings, else the markers above. Each (L)
hit becomes a `Gap` node with the site and the data-driven replacement.

### P2 — Rank

Divergent families first (each is a live bug — say what it does when hit),
then pure-cost duplicates, then cosmetic repetition. The scanner's score
orders within a tier.

### P3 — Burn down, one family per merge wave

Per family, in this order, its own commit:

1. **Adopt** the existing owner. Extract a new one only when none exists
   (state the search — the `Searched:` trailer).
2. Migrate every site; a site carrying a local guard gets its RULE diffed
   against the owner first (round-4 lesson: the owner may be incomplete).
3. **Gate** it in `fulltest` with a negative control
   (`scripts/check-one-delim-tracker.sh` is the template).
4. Close the `DupFamily` / `Gap` node; note the gate.

Cadence: one family per wave alongside feature work. A big-bang refactor
is unsafe under the battery cadence and unreviewable.

### P4 — Prevention

- Rule #4 is already gated by the `Searched:` trailer
  (`scripts/check-rule-trailers.sh`). Keep it.
- Rule #7 gets a ratchet: the scanner's total score per file recorded in
  `docs/parity/` like the pack-degradation baseline; a commit that raises it
  fails `fulltest` unless the baseline is moved with a stated reason.
- `/dupaudit` step 2 (re-check known families) runs at every merge wave,
  not only at feature merge.

## Non-goals

- No renaming of the pinned dds' display names (`int64_t` is in every
  emitted-C typedef, test expectation and frozen record).
- No consolidation without a gate. A fix plus a comment regrows.
