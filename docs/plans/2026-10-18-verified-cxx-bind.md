# The C++ bind is verified, and a decline is always safe — plan (0.106.0 arc, section K)

Owner 2026-10-18: "you can't leave this broken like this … recon it, plan it,
think it through and fix it properly"; "There are two C packs — the C only
pack, and the C/C++ mixed pack so C++ should NEVER dip into the C-only pack".

## The problem

A C compile's bind is verified: `forest_bind_env_ok` declines a root whose
frozen macro environment differs from the consumer's (J4). A C++ / madc bind
is not. The check sits behind the rollout knob `MADC_FOREST_ENV_CHECK` (off
since v40), so a C++ program that sets a feature macro before its system
includes (`NDEBUG`, `_GNU_SOURCE`) is served the freeze's view. This is the
silent wrong answer that J4 fixed for C (sqlite3).

With the knob on, the packed suite fails 159 tests (145 madc, 14 gnu++17).
All 159 pass with it off. Artifacts are in `tmp/envknob/` on the build
container.

## Recon (2026-10-18, packed release at b23e4613a)

- **The division holds for C++.** A C++ compile probes the library image's
  stack and opens the 2283-unit madc profile, the mixed C/C++ pack. It never
  opens the C layer:
  - The bind matches the exact config word.
  - The source arm and the ledger are carried only by the madc profile.
  - `forest_adopt_declared_function` reads the bound profile first.

  It holds by stack ORDER, though, not by rule. `forestMatchStdlibFlavor`
  masks the language bits away (0xfffe0000). A C compile reads header TEXT
  and the AOT ledger from the mixed pack, by ruling J3. The owner's model
  allows that: the mixed pack is C/C++.
- **Why a knob-on compile fails.** The decline fallback itself is unsound.
  The trace for `#include <iostream>`, gnu++17, knob on:
  1. Root `<iostream>` declines on `bits/setjmp.h` `_SETJMP_H`, so it is
     tokenized live. Its includes bind or decline one at a time.
  2. `bits/ios_base.h` declines (`stdio.h` `_WCHAR_H`), so it is tokenized
     live.
  3. `<streambuf>` BINDS. Its closure holds `bits/ios_base.h`, which the
     check PRUNES as "already present ('_IOS_BASE_H' guard live)".
  4. The bound decls are restored once, at the end of tokenize
     (`flush_forest_pending_globals` → `forest_restore_decls`). That is
     before the parse reaches ANY live token. `forest_apply_pending`
     re-parses streambuf's default arguments
     (`ios_base::openmode __mode = ios_base::in | ios_base::out`), but
     `ios_base` exists only in live tokens not yet parsed. Hence
     `streambuf:259: Unknown namespace or class 'ios_base'`.
  5. `<string>` fails the same way (`basic_string.h:1085 'max_size'`).
     `<string_view>` binds over a pruned live `bits/char_traits.h`.
- **The prune's premise is false at restore time.** The prune assumes the
  live unit's content "is already present". It is present in the token
  stream, but the restore runs ahead of the whole parse. The restore model is
  a closed PREFIX: every bound decl is installed before every live decl. A
  bound unit may therefore never depend on a live unit. In C the restored
  records are structural, so the violation is mostly absorbed (live-wins,
  `denotes_same_type`). C++ re-parses by name (default arguments, template
  hydration), so it breaks. The husk path (a live unit under a bound root)
  violates the same invariant.
- **Why everything declines: the deps record the corpus's include order.**
  The madc profile is ONE TU, frozen in corpus order. A consult is recorded
  per MACRO, with the state the corpus happened to have there. The top
  definers across the 159:

  | unit / macro | lines | what differs |
  |---|---:|---|
  | `stdio.h` `_WCHAR_H` | 1616 | freeze skipped `#ifdef _WCHAR_H` (`open_wmemstream` redecl); the consumer takes it |
  | `wchar.h` `_VA_LIST_DEFINED` | 1076 | typedef guard: freeze skipped, consumer takes |
  | `bits/setjmp.h` `_SETJMP_H` | 914 | `#if !defined _SETJMP_H && !defined _PTHREAD_H` — the SAME outcome via `pthread.h` |
  | `time.h` `__pid_t_defined` | 864 | typedef guard: freeze skipped, consumer takes |
  | `sys/types.h` `__off_t_defined` | 592 | typedef guard: freeze skipped, consumer takes |
  | `wchar.h` `WCHAR_MIN` | 278 | directive-only arm writing two names (`WCHAR_MIN`, `WCHAR_MAX`) |
  | `c++/13/stdlib.h` `_GLIBCXX_INCLUDE_NEXT_C_HEADERS` | 40 | wrapper whose whole content flips |

  Every consult is checked against the consumer's state at ROOT time. But the
  macros that decide a consult can be defined by the closure ahead of it
  (`_PTHREAD_H`, by the includer `pthread.h`). The frozen PP events carry no
  include positions (J4 note), so the consult's real state cannot be computed
  today.
- **History.** The check has been knob-off since v40. The only prior plan was
  "freeze each unit as its own first includer" (deferred, never approved). It
  does not fix `_SETJMP_H`: `bits/setjmp.h` `#error`s as a first includer.

## Design

### K0 — the division is a rule, not a stack order

- A C++ / madc compile reads only C++-family profiles in every pack reader:
  the bind, the source arm, the ledger and the declared-function adoption.
  The family is the config word's fk `language_std` (C 32-44; C++ 64-72 and
  madc 96).
- A C compile binds the C layer. It may read text and the ledger from the
  mixed pack (ruling J3).
- `a104eecaf`'s language view stays. C searches the C include path, gcc's
  `-x c` list, so a C read of the mixed pack's text never resolves a libstdc++
  wrapper.
- Gate: `forest_bind_gate.sh` `[division]`. On a stack with a C profile
  NEWEST, a C++ compile still binds and reads only the C++ profile. The
  control, a C compile, binds the C profile.

K0 DONE 2026-10-18 (e5477ae31): `cir_forest_profile_serves_family`, asked by
every probe mode in `probe_forest_chain`. Gate `forest_bind_gate.sh`
`[division]`: before the fix, the C++ compile printed `which=2`.

### Finding — K1 as first written made it worse (2026-10-18, measured)

The closed include set below was built and measured: the knob-on packed suite
went from 159 failures to **352**. It is stashed, not committed. The trace for
`#include <iostream>` on that build:

- `bits/stringfwd.h` binds before anything is live. It only FORWARD-declares
  `std::char_traits`.
- The restore registers the name with the freeze's COMPLETE entity. The arena
  holds one entity per name, as of the end of the freeze TU: the class pattern
  with its `__gnu_cxx::char_traits` base.
- `bits/char_traits.h` then parses live (its closure holds the live
  `wchar.h`). So `<iosfwd>`'s live `char_traits<_CharT>` meets a pattern whose
  base was never restored: "ClassPattern could not resolve type
  '__gnu_cxx::char_traits'".

The restore has no per-unit fidelity: a bound unit brings each name it
declares as the whole TU defined it. So no rule over include closures alone
can make a mixed C++ TU sound.

C mixes safely because its restored entities are structs, typedefs, enums and
functions, which merge with a live twin (typedef/class live-wins,
`denotes_same_type`). The evidence is the C suite and the C pack gates. C++
restores class patterns, templates and default arguments that are resolved BY
NAME.

### K1 (revised) — a C++ TU's system content is all-bound or all-live

- **K1a.** In a C++ / madc TU, a decline with nothing bound yet stops binding
  for the rest of the TU (`ForestSide::bind_stopped`). Every later include
  parses live. This is the common shape: a config macro before the first
  include. No mixing, no restart.
- **K1b.** A decline AFTER something bound mixes, and needs a decision at the
  decline. The root's live closure (the units not yet bound) is checked
  against the bound units' declared names, via the decl index, namespaces
  excepted:
  - Disjoint: the root parses live and the TU stays sound. The realistic
    cases look like this: `NDEBUG` before `<cassert>` after `<iostream>`; a
    feature macro after `<features.h>` changes nothing.
  - Overlapping: the TU's tokenize restarts with binding off. The mechanism
    is designed after K2 measures how often this happens. It is gated by a
    forced-restart differential over the whole suite (every test identical to
    a fresh live compile).
- C keeps per-root declines, the prune included.
- The stashed closed-set attempt is dropped. Its `[closedset]` reducer (a
  default argument naming a type from a live header; g++ and clang++
  `m=3 extra=20`) returns as K1a's gate leg.
- Measure after K1a: the knob-on suite. The remaining failures should be
  exactly the decline-after-bind TUs.

### K1 (first draft, superseded above) — the bound set is closed

- Rule: a root binds only if no unit in its closure is live-tokenized in this
  TU. Otherwise the root declines, and its live parse binds or declines its
  includes one by one under the same rule. Bound content is then a closed
  prefix, and the restore-once model is sound in C and C++ alike.
- The PRUNE disposition is deleted: a live-tokenized unit in the closure now
  declines the root.
- The HUSK disposition (a live unit under a bound root) goes the same way, and
  K2's live arm replay replaces it.
- The masked bind stays. It is a PP-level twin, not a dependency on live
  decls.
- Gates:
  - Reducers `tests/testforestdeclineclosed{,2}` (`.env`:
    `MADC_FOREST_ENV_CHECK=1`), `<iostream>` and `<string>`. g++ and clang++
    output; the packed binary before the fix fails with `ios_base` and
    `max_size`.
  - The knob-on packed suite: 0 knob-only failures.
  - The C suite and the C pack gates stay green.
  - The parse-cost gate: knob off is unchanged.
- Cost: with the knob on, a decline now cascades into more live parsing. K2
  removes the false declines.

K1a DONE 2026-10-18 (5bd77c67e). The knob-on packed suite went from 159 to 32
failures. Gates:
- `forest_bind_gate.sh` `[allornone]` (new).
- `[redecl]`, now C plus a C++ check of the stop.
- `[statmem]`, consumer `<locale>` first: `<cstdio>`-first declines on
  `stdio.h`'s `_VA_LIST_DEFINED` arm until K2.

### K2 — detailed design (2026-10-18, after K1a)

Measured after K1a: the knob-on failures are 32, and every one is a decline
AFTER a bind. Those declines are the corpus-order classes in the table above.

What the freeze records today:
- Per unit, its PP events (defines and undefs, in order).
- Per unit, its include edges (in order), as a separate list.
- Per-MACRO branch deps (name, state, value, definers).

The two lists carry no interleaving, so the bind cannot know the macro state at
a consult. It installs children before parents (`forest_bind_include`'s own
comment calls that an approximation), and it compares each dep against the
consumer's state at ROOT time.

v68:
- **Positions.**
  - Every edge records the number of the unit's PP events before it.
  - Every condition record records the events and the edges before it.
  - Together they give one total order per unit: events, includes and
    conditionals, as the freeze met them.
- **Condition records.** For each `#if` / `#elif` / `#ifdef` / `#ifndef` the
  freeze evaluates that consults an EXTERNAL macro (the existing test in
  `pack_record_branch_macro`), it records:
  - the kind, the raw condition text and the outcome;
  - for a SINGLE-ARM conditional (no `#elif` / `#else`) that was skipped, the
    arm's text. This generalizes the v55 / v67 `#ifndef` capture.
- **The bind walk.** The check and the PP install become ONE pre-order walk
  in that total order over the root's closure (bound units excluded). It
  applies events to the live tables under an undo log (`set_macro`, the one
  writer).
- **At each condition record**, the walk re-evaluates the text through the ONE
  `#if` evaluator: `evaluateIfCondition` split into reading the source text and
  evaluating it.
  - Same outcome → go on.
  - A skipped single arm is now taken (missing content):
    - its directives run in place, under the undo log (the `WCHAR_MIN` shape);
    - its declarations are spliced as live tokens after the root (the typedef
      guards, `stdio.h`'s `_WCHAR_H` redeclaration). They are C declarations
      that merge with any bound twin.
  - A taken arm is now skipped: a self-guard is masked (as today); otherwise
    the root declines.
  - Anything else → the undo log rolls back and the root declines (K1a / K1b
    decide what follows).
- **Deleted when the walk lands** (no parallel implementation):
  - the per-macro dep check;
  - the `PP_ARM` / `ARM_REPLAYS` tuples;
  - the post-order install;
  - the husk path (missing content IS the arm splice).
- **Gates:**
  - one `forest_bind_gate.sh` leg per class, each with a g++ / gcc oracle;
  - the 32 knob-on failures → 0;
  - the C-layer gates (`[cenv]`, `[armreplayc]`, the `<stdio.h>` C bind) stay
    green;
  - the parse-cost gate on the packed binary (one walk replaces the check
    plus the install, so it should be flat).

### K2 step 3 — the walk (2026-10-18, after v68 landed in daf6a6e10)

Measured first. The 32 knob-on failures have three causes:
- 28: `<string>` → `wchar.h` `#ifndef WCHAR_MIN`. The arm is directive-only
  and writes two names, so v67's one-name replay cannot take it.
- 3: `<memory>` → `time.h` `#ifndef __pid_t_defined`. The arm is a typedef
  plus its guard define.
- 1: `<iostream>` → `bits/setjmp.h` `#if !defined _SETJMP_H && !defined
  _PTHREAD_H`. At its real position (after `pthread.h` defines `_PTHREAD_H`)
  the outcome is the same.

Capture defect found: a comment line inside a skipped arm marks it impure
(`WCHAR_MIN`'s arm opens with one). The walk does not read the purity flag. It
runs a captured arm and keeps whatever tokens it yields.

Freeze additions (v69):
- Per condition, `CIR_COND_REC_END`: the unit's record count at the arm's
  end. The records between are nested in the arm.
- Per captured arm, `CIR_COND_ARM_STATE`: the freeze's state of every macro
  the arm writes, at the arm (`forest_macro_state_spelling`). The frozen
  content after the arm was expanded with those values. An arm the walk runs
  must leave the same state for every name the freeze held DEFINED and
  expanded into a token after the arm (`pack_expansion_clock`: the freeze
  stamps each macro's last expansion), or the bound content is wrong. Any
  other name carries no value into a frozen token, and a later conditional on
  it is recorded and re-evaluated, so the arm may set it (`pack_arm_state`
  writes such an entry as `?NAME`). Measured: madc's `stdint.h` spells
  `WCHAR_MIN` `INT32_MIN`, glibc `wchar.h`'s arm `__WCHAR_MIN`, and nothing
  after expands it. This is v67's value check, generalized to every name the
  arm writes.
- Per unit, its inclusions: `[n, (ev_end, ed_end, cond_end) × n]`, recorded
  where the unit's include returns. A guard-less unit included N times by the
  freeze TU (`bits/mathcalls.h`) has N inclusions. Each edge to it in the walk
  takes the next one; past the last, the freeze skipped the include, and the
  walk skips it too.

The walk (`forest_walk_unit`), one per root, used by every bind entry (the
two include sites and the two posix-provider sites go through
`forest_bind_include_root`):
- Skip a unit already bound, live-present, or on the walk's stack.
- Prune: when the inclusion opens with a recorded `#ifndef` whose name is
  defined now, the unit's file was live-tokenized here, and a live re-include
  would skip it, the unit is live-present and the walk skips it.
- Items in frozen order: a condition at `(ev, ed)` comes before edge `ed` and
  event `ev`; an edge at event position `p` comes before event `p`.
- An event applies through `set_macro`, the one writer. The `#define` and
  `#undef` handlers write through it too. The undo log keeps every write's
  prior state and restores them in reverse, so a name's earliest state comes
  back last. The log runs only when the check is on.
- A condition is re-evaluated: `#ifdef` / `#ifndef` with
  `macro_name_defined`, `#if` / `#elif` with `evaluate_if_text`. A text
  naming `__has_include_next` or a quoted `__has_include` is position-bound,
  so it counts as a flip.
  - Same outcome: go on.
  - Taken at freeze, skipped now, a single-arm `#ifndef N`, and the arm's own
    events define N (a sub-block guard): skip the arm's events, edges and
    nested records. Every other name the arm's events write must already hold
    the arm's final value (N itself only needs to be defined). Its declarations
    stay bound, as today's masked bind.
  - Skipped at freeze, taken now, arm captured: run it in place on its own
    Source. Its directives write through `set_macro` (logged), and the names
    it writes must then match `CIR_COND_ARM_STATE` (as above). Its tokens are kept and
    pushed after the walk commits, at the root's include site. An arm with a
    directive other than define, undef or a conditional is refused.
  - Anything else: verified → roll back and decline; knob off → follow the
    frozen outcome (today's knob-off behaviour).
- Commit: units enter `chain` in post-order (the decl restore's order), the
  embedded once-only keys and the cost stats as before, then the kept arm
  tokens.
- Decline: the undo log restores every name and the kept tokens are dropped.

K2 step 3 measured 2026-10-18 (packed `madc-release`, v69 pack):
- Knob on: 32 → 9 failures. The 9 are one class: `wchar.h`'s
  `#ifndef __attr_dealloc_fclose` arm. Frozen `wchar.h` expanded `stdio.h`'s
  `__attr_dealloc (fclose, 1)` into `open_wmemstream`'s declaration after the
  arm, and a live parse here leaves it empty. The difference is real, so the
  root declines. It declines after a bind, which is K1b's case.
- Knob off (the shipping default): 2098 / 2098.
- `forest_bind_gate.sh` `[k2walk]` (new): position, two-name directive arm
  with an inert spelling difference, typedef arm, a value expanded after the
  arm (must decline), and the undo on decline. `[redecl]`, `[husk]` and
  `[armreplayc]` now go through the walk.

Deleted with the walk: `forest_bind_env_ok`, `forest_bind_include`,
`forest_install_pp`, `forest_replay_pp_arms`, `skipped_arm_defines`,
`pp_install_mask`, `pp_arms`, `husk_live_pending` and the husk path. Deleted on
the freeze side (v70): the per-unit branch deps (slot +8) with their `PP_ARM`
and `ARM_REPLAYS` tuples, the container-global branch macro names (seg 6),
`skip_and_capture_arm`, the arm-purity flag and a condition record's token
positions. The consult recorder (`pack_note_branch_consult`) keeps one job:
count an external consult so the open conditional is recorded. The mixed
profile shrinks 58.7 KB (13.98 MB, 370 fewer segments), the C layer 32 KB.

K2 step 3 cost (parse-cost gate, callgrind Ir, release build). The first walk
cost +2.6% on `rung_stdio` and +5.1% on `c_headers.forest`. Measured causes
and fixes:
- Each unit's walk slots (events, edges, conditions) are read whole, and a
  small zstd frame costs ~14K Ir to decode before its bytes (the literal
  table). The slots are now stored raw (`forest_kind_walk_slot`). The mixed
  profile grows 0.23 MB, to 14.04 MB (cap 15 MB).
- Each `#if` / `#elif` records the macros it consulted, with a state hash
  (`CIR_COND_CONSULT_*`). When every hash still matches, the bind takes the
  frozen outcome without evaluating. A text with `__has_include` is always
  evaluated.
- Table writes go by spelling id (`set_macro_id`): a frozen event's name is
  never built as an `istring`. The undo log moves each prior value out of its
  table instead of copying it.
- Knob off, only a condition that can mask or run is evaluated.

Result (the gate run recorded in e5b0d55b5): `rung_stdio` 19.35M → 18.80M Ir
(−2.8%; bind 1.46M → 1.19M with raw slots alone), `rung_limits` −4.8%,
`rung_type_traits` −3.2%, `rung_c8` −2.5%, `rung_vector` −1.9%;
`c_headers.forest` 38.01M → 38.16M (+0.39%; bind 2.78M → 2.94M, the
per-condition consult checks the verified walk adds).

### K2b — the 9 are a false difference (2026-10-20, measured)

Re-measured on the packed release over 87cb582f6: the 9 knob-on failures are one
decline, `<wchar.h>`'s `__attr_dealloc_fclose`, in a TU that binds `<map>` / `<vector>`
first and never includes `<stdio.h>`. The decline is not a real difference:
- glibc's `<wchar.h>` reads `#ifndef __attr_dealloc_fclose` twice. The first arm
  defines it only `if __has_builtin (__builtin_fclose)` — 0 under gcc, clang and
  madc alike — so it leaves the name UNDEFINED. The second arm defaults it to
  empty. Only then does `open_wmemstream`'s declaration expand it.
- The walk checked the first arm's write at the first arm's end ("frozen
  `__attr_dealloc (fclose, 1)`, here undefined") and declined.
- And the two values the content can see lex alike: madc reads a GNU attribute
  it does not model (`__malloc__`) as no token, so `<stdio.h>`'s
  `__attr_dealloc (fclose, 1)` and the empty default both give nothing.

Two refinements of K2's arm-state rule, both exact:
- **Lex alike.** A written object-like macro holds the frozen state when its
  body and the frozen body lex to the same tokens here (`tokenize_text_run`,
  compared by token kind and `madc_token_spelling`) — not only when they are
  spelled alike (`forest_object_states_lex_alike`).
- **Deferred check.** The freeze marks an arm's entry `^` when the NEXT recorded
  conditional's captured arm writes the same name and no macro is expanded
  into the token stream in between (`expansions_at_begin` of that conditional ==
  `expansions_at_end` of this arm). The bind checks such a name after that
  conditional instead: an arm that ran there checked it itself; otherwise the
  name must hold the frozen state then, or the root declines
  (`forest_walk_resolve_deferred`; any check left at the unit's end runs there).
  No format bump: the mark lives in the arm state's pooled text, and a reader
  without K2b compares the marked entry, mismatches, and declines (safe).
- Gate: `forest_bind_gate.sh` `[lexalike]` — a corpus frozen `<stdio.h>` first;
  a consumer of `<wchar.h>` / `<cwchar>` alone binds and prints g++'s `n=3`
  (the C++ half RED before: `<cwchar>` declined on the arm).

With those two, 1 of the 9 binds; the other 8 decline at the next false
difference, one family with the first: glibc guards each `off64_t` typedef
with `#if defined __USE_LARGEFILE64 && !defined __off64_t_defined`, an `#if`
whose arm defines the guard it tests. The corpus froze `<stdio.h>` first; a
consumer that reaches `<unistd.h>` (or `<sys/types.h>`, `<fcntl.h>`) first
takes that header's arm and skips `<stdio.h>`'s.
- **The masked bind reads the condition, not the directive.** A taken single
  arm skipped here is masked when its guards are the arm's own: the names its
  events define that its condition spells (`source_spelled_names`), defined
  here, and the condition read with them undefined (`set_macro` both ways,
  under the undo log) is taken. `#ifndef G` is the one-name case. Every other
  name the arm writes still must hold the arm's final value
  (`forest_arm_guard_passed`).
- Gate: `forest_bind_gate.sh` `[typedefguard]` — a corpus frozen `<stdio.h>`
  then `<unistd.h>`; a consumer of `<unistd.h>` first binds `<stdio.h>` /
  `<cstdio>` and prints gcc's / g++'s `o=5` (C and C++ both RED before).

K2b measured 2026-10-20 (packed `madc-release`, knob on): the 9 → 0 declines;
the suite 2137 / 2139, and the 2 (`testnoreturn_c11`, `testthrow_through_frame`,
B205's gates not yet landed) fail with the knob off too. Knob-only failures: 0.

### K1b — measured (2026-10-20, after K2b)

Census, knob on, packed release: every suite test's verbose trace, counting
roots that declined and whether a bind came first.
- Over 87cb582f6 + K2b: 53 of 1996 tests decline, 48 after a bind, all pass.
  50 are one root: libstdc++'s `<stdlib.h>` wrapper, frozen only through
  `<bits/std_abs.h>`'s `#include_next`, which madc resolved from the top of the
  search list (gcc resumes after the includer's found dir). Fixed in
  fdbf8c551 (`tests/testincludenextsubdir_c`). `stdlib.h` became a pack root
  (list v11) so a C++ `<stdlib.h>` binds its real content.
- After it: 3 of 1997 decline, all after a bind, all pass:
  - two `assert.h` `#ifdef NDEBUG` (C++ `testsysheaderreinclude`, C
    `testsysheaderreinclude4_c`): real differences. The live closure is
    `assert.h` alone, disjoint from the bound units' names.
  - one C `bits/byteswap.h` (`testbswap_glibc_inline`): `<byteswap.h>`
    tail-matches the frozen `bits/byteswap.h` (KG
    `forest_tail_match_subdir_unit`); its `#error` arm declines, and the live
    parse reads the real `<byteswap.h>`. C keeps per-root declines.
- Overlapping declines after a bind in the suite: 0.

### K2 — the deps are exact (a decline means the content really differs)

- Freeze (format v68): record each conditional's OUTCOME.
  - The condition text and the taken arm, per directive that consults an
    external.
  - The include POSITION of every child include in the unit's PP event
    stream.
- Bind: the check walks the closure in include order. It keeps a scratch
  overlay: the consumer's state plus the PP events installed ahead. It
  re-evaluates each recorded condition in that POSITIONAL state:
  - Same outcome → satisfied (`_SETJMP_H`).
  - The freeze skipped an arm the consumer takes (missing content: the
    typedef guards, `WCHAR_MIN`, `_WCHAR_H`) → the unit binds, and the arm's
    text is spliced LIVE after it. This generalizes v67's `ARM_REPLAYS` from
    directive-only arms to any self-contained arm. The spliced text is live
    content AFTER the bound prefix, so K1's rule holds.
  - The freeze took an arm the consumer skips (extra content) → a self-guard
    is masked (as today). Otherwise the root declines (K1 keeps it correct).
- Gates:
  - Reducers for each class: `_SETJMP_H` via `pthread.h`; a typedef guard in
    both orders; `stdio.h` after `wchar.h`; `NDEBUG` / `_GNU_SOURCE` set
    before `<cassert>` / `<cstdio>` in C++ (a real decline, live-equal
    output).
  - A `--show-stats` decline count of 0 for the standard C++ headers in
    corpus order AND reversed order.
  - `forest_bind_gate.sh` legs per class.

### K3 — the check is on for every consumer

- `verified` becomes unconditional, and the `MADC_FOREST_ENV_CHECK` knob is
  deleted (no-parallel-implementations).
- The packed suite, Linux and win64, runs green with no knob.
- The parse-cost gate runs on the packed binary: any growth is a finding
  before the re-record, never a silent rebaseline.

Landed 2026-10-20 as 656f8f351, with K1b as the overlap detector (KG
`k1b_overlap_detector_defer_restart`: an overlapping decline after a bind
stops binding for the TU; the restart stays designed, not built).
- Reducer `tests/testndebugcxxbind`: `NDEBUG` defined after `<cstdio>`,
  before `<cassert>`; g++ and clang++ print `ok`. The packed madc aborted in
  `assert(0)` with the knob off.
- Packed Linux suite, no knob: 2139 passed, 2 failed — `testnoreturn_c11`
  and `testthrow_through_frame`, B205's gates, red without K3 too. win64
  runs at the 0.106.0 seam (`headerless-win`).
- Parse-cost: K3 alone raised five C++ rungs (+0.55% to +1.39%). B2 and B3
  of the front-end speed plan landed first and pay for it; every row ends
  at or below the old baseline, re-recorded downward.

### Finding — win64 declines `<iostream>` on `NULL` (2026-10-21, measured)

The first win64 release build since K3 (`make release-windows` at 1650fdacd)
fails `forest_pack_gate`: the load probe (`tests/testfreezerun.mad`,
`#include <iostream>` first) binds nothing.
- `forest bind: arm state differs — frozen '=NULL((void *)0)', here '=NULL0LL'`,
  then `DECLINE root .../iostream — unit .../wchar.h '#ifndef NULL' has an arm
  that leaves a macro unlike the freeze's`; binding stops for the TU.
- The freeze TU opens with `<cstddef>`: madc's `stddef.h` defines `NULL`
  `((void *)0)`, so mingw `wchar.h`'s `#ifndef NULL` arm is skipped. A consumer
  that opens with `<iostream>` reaches `wchar.h` with `NULL` undefined and runs
  the arm (`0LL`, mingw's C++ `_WIN64` spelling). `NULL` is expanded later in
  the freeze, so K2's rule declines.
- A false difference, by the oracle: mingw g++ 13 `-E -dD` over `#include
  <iostream>` defines `NULL 0LL` in `wchar.h` (line 3340), expands nothing to
  `0LL`, then reaches `stddef.h`'s first inclusion (`#undef NULL` / `#define
  NULL __null`, line 5150) before any use of `NULL`. madc's consumer walk binds
  `stddef.h`'s first inclusion at the same edge, so `NULL` holds the frozen
  `((void *)0)` again before any frozen token expands it.
- Linux never shows it: glibc headers spell no `NULL` of their own
  (`__need_NULL` defers to `stddef.h`). mingw headers spell their own; darwin's
  `_null.h` spells `__DARWIN_NULL`.
- Why K2/K2b cannot see it: the arm's check is due when the name's LAST
  freeze-time expansion follows the arm, and K2b defers only to the next
  conditional of the same unit. Here the restoring write is a different unit
  (`stddef.h`) bound at a later edge of an ancestor.

### K2c — check a name where the frozen content expands it

- Freeze: an EXPANSION POINT is a macro's first expansion into the token
  stream in a point epoch. Every include edge, recorded conditional, recorded
  arm's end and inclusion end starts a new epoch, so between two points of a
  name in one unit inclusion the walk replays the same events the freeze
  read. A point records the unit inclusion, the unit's events, edges and
  records ahead of it, and the freeze's state of the name there (a pooled
  `forest_macro_state_spelling`). The container keeps the points of the
  names a captured arm writes (the watched set): the conditions slot ends
  with them (`CIR_POINT_WORDS` each). Container v71.
- Walk: an arm the walk runs that leaves a written name unlike the freeze's
  state at the arm no longer declines; the name joins the walk's
  differs-set. At each point the walk reaches (its inclusion, events, edges
  and records all passed), a name in the walk's set or the TU's
  (`ForestSide::macro_differs`) must hold the point's state
  (`forest_state_holds`, lex-alike included), or the root declines.
- The walk's set joins the TU's when the walk ends, bound or declined: a
  declined root's live parse runs the same arms. A later root that froze an
  expansion of the old value declines instead of binding it (before K2c such
  a root bound the frozen value — a silent wrong value when the first root
  declined and parsed live).
- It subsumes K2b's `^` deferral and the v69 `?` mark (a name never expanded
  after the arm has no point to check): both are gone, and
  `CIR_COND_ARM_STATE` is the plain state.
- Gate: `forest_bind_gate.sh` `[k2c]` — `restore` (the mingw shape: an arm
  sets the name, an ancestor's later edge restores it before the expansion;
  binds), `carry` (a bound root's differing name declines a later root that
  froze an expansion) and `declined` (a declined root's differing name does
  the same). The win64 profile's `forest_pack_gate` load probe is the
  release check.
- An arm may carry a `#pragma push_macro` / `pop_macro` pair that it closes
  itself (mingw wchar.h's snwprintf); the pragma's restore is a macro write
  (`set_macro`, the freeze's event), so the walk's undo log covers it.
- Win64 result (2026-10-21): the load probe no longer declines on `NULL` /
  `WCHAR_MIN` / `WCHAR_MAX`; it declines next at mingw wchar.h's
  `#ifndef _WSTDIO_DEFINED`, whose arm (lines 462–1248 — the `#endif`
  comments at 883 / 885 are mislabelled, the count closes it at 1248, as
  gcc reads it) holds `#include <swprintf.inl>`. A run arm with an include
  is K2d.

### K2d — a run arm's `#include`, and the frozen arm it replaces

mingw `stdio.h` and `wchar.h` hold the same `#ifndef _WSTDIO_DEFINED` block
(prototypes, `__mingw_ovr` inlines, `#include <swprintf.inl>`). The freeze
reads `stdio.h` first: its block is taken and `wchar.h`'s is skipped. A
consumer that reaches `wchar.h` first takes the other one.

- (a) An `#include` read while the walk runs an arm is an edge of that walk.
  The header's frozen unit is walked there (`forest_arm_include` →
  `forest_walk_unit`), so its events replay at that point of the arm and its
  declarations restore with the root. If the header cannot be bound (not
  frozen, a quoted, `#include_next` or `__need` include, or a unit whose own
  walk declines), the run stops (`ForestArmAbort`), and the root declines and
  parses live. The arm purity test admits `#include` for the run (and the
  freeze capture), not for one-mutation replay.
- (b) A taken frozen arm that the walk skips (a masked bind) withdraws the
  declarations made inside it, because the arm the walk ran has already
  declared them live.
  - The freeze stamps each arm's first and last token. The writer stores them
    as the unit's own token range (`CIR_COND_TOK_BEGIN` / `_END`, the decl
    index's slice coordinate). Container v72.
  - The walk records each masked arm's range (`ForestSide::withdrawn_arms`).
  - At restore, a name whose every declaration in that unit lies inside a
    withdrawn arm is withdrawn: `CirMaterializeFilter::withdrawn` holds
    (unit, name), and `name_verdict` treats the unit as not declaring it.
- Gate: `forest_bind_gate.sh` `[k2d]`:
  - `withdraw`: the mingw shape. `top.h` binds, the run arm's include binds
    `inl.h`, and `stdio.h`'s skipped arm withdraws its declarations.
  - `unbound`: an arm includes a header the freeze never read, so the root
    declines.
  - Oracle: g++ prints `inline=41 twice=80` / `extra=3`.
  - `[redecl]` corpus 1 now asserts the withdrawal: `a.h`'s skipped arm no
    longer restores `fbg_T` / `fbgT` twins beside `b.h`'s live copies, which
    is gcc's reading. The twin tolerance (`denotes_same_type`, C11 6.7p3)
    moves to corpus 3, a twin with no arm: `d.h` declines and parses live,
    then `e.h` binds and restores its own `typedef fbgU`.
- Linux shows it too. glibc `stdio.h`'s typedef-guard arms
  (`__ssize_t_defined`, `__off_t_defined`, `_VA_LIST_DEFINED`,
  `__off64_t_defined`) are masked in `cxx_stl` / `subscript`, and their
  typedefs are withdrawn.
- Cost (parse-cost, callgrind A/B against a packed release of the K2c commit):
  - A TU with no withdrawal keeps K2c's path. `forest_run_bound`'s plain loop
    stays inline; the withdrawal path is `noinline`, and a qualified name is
    compared in place (`CirMaterializeFilter::withdraws`, as
    `ForestDeclNames::probe` does), never built.
  - Rows: rung_c8 −51K Ir, c_headers −6K, rung_vector +68K (+0.10%: the slot's
    2 words, the `name_verdict` arguments). The withdrawing TUs: cxx_stl
    +0.02%, subscript +0.04%.

## Thread safety

The bind state is per-`Program` (`ForestSide`). The overlay in K2 is a local
of the check. Nothing is shared and there are no new globals.

## Order

K0 (done) → K1a → K2 → K1b → K3. Each slice gets its own commits with Tier 1 +
Tier 2, plus the knob-on packed suite on the container as K1a's and K2's
measure. The seam battery runs at 0.106.0.
