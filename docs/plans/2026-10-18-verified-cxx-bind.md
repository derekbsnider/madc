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

## Thread safety

The bind state is per-`Program` (`ForestSide`). The overlay in K2 is a local
of the check. Nothing is shared and there are no new globals.

## Order

K0 (done) → K1a → K2 → K1b → K3. Each slice gets its own commits with Tier 1 +
Tier 2, plus the knob-on packed suite on the container as K1a's and K2's
measure. The seam battery runs at 0.106.0.
