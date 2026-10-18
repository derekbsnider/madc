# C programs bind the pack's C layer — plan (0.106.0 arc, section J)

Owner 2026-10-17: "C is separate, uncompressed, and not on the same list";
"just make sure it doesn't get removed again". Agreed 2026-10-13
(`2026-10-13-pack-load-cost-experiments.md`, Test 3 and "The C question"),
dropped when the per-header slices shipped in 0.105.0. KG Decision
`c_pack_layer_never_dropped`, Gap `c_programs_parse_headers_from_text`.

## The problem

A `.c` compile (gnu17) and every `--std=c*` compile parse their system headers
from text on every run. The pack's one profile is frozen under madc (C++) and
labelled gnu++17 / c++17 only (`scripts/forest_pack_share.sh`), so a C config
misses it: `forest_probe_arm` finds no profile whose config serves the
compile, and `forest_missing_fallback` live-parses. `c_headers.c` costs 245M Ir
against gcc -O0 -c 144M and clang 127M (2026-10-17). The same headers compiled
as C++ cost madc 5x less than either. In the parse-cost gate, `c_headers.forest`
equals `c_headers.live`, and `sqlite3.forest` is in the same position.

## The agreed properties (each one must survive the design)

1. The C headers are frozen a SECOND time, under C (gnu17), as the C
   preprocessor sees them. Under C++, `<stdlib.h>` and `<math.h>` are
   libstdc++'s wrappers, so the C++ freeze's C units are not the C view.
2. It has its own label list, beside the C++ one, in `forest_pack_share.sh`:
   one owner, two lists.
3. Which C standards a gnu17 freeze may serve is MEASURED, not assumed.
   `__STRICT_ANSI__` hides glibc's POSIX names under c17, and
   `__STDC_VERSION__` gates C11 names under c99. A standard whose header view
   differs gets its own freeze, or none.
4. Its blocks are stored raw (uncompressed).
5. The C tables sit ahead of the C++ boundary: the C profile is stacked BELOW
   the C++ one in the same library image.
6. It is built on the per-header slices: a bind loads only the units the
   program's include closure reaches.
7. It lands with a GATE, never without one: `forest_pack_gate.sh` is red
   unless a `.c` compile (and each label's compile) binds the C layer. The
   `--std=c17` bind was dropped by the owner's 2026-10-18 ruling (below). The
   `c_headers.forest` parse-cost row is re-recorded at the new level, so losing
   the bind is a several-fold red.

## What exists (recon at 87080f042)

- A library already carries a STACK of containers, newest first (footer at
  EOF). Every discovery arm walks it and skips a profile whose config does not
  serve the compile (`forest_probe_arm`, `serves_config`). A gnu17 profile
  stacked under the C++ one is reachable with no reader change; it needs no
  new role, since it is another SYSTEM profile with another config word.
- The freeze path is language-agnostic. A `.c` TU freezes under gnu17, and
  `forest_config_gate.sh` already freezes `consumer.c` and binds it from a `.c`
  compile.
- Compression: `madc_cir.cpp` sets the codec to zstd (level 15) when
  appending. `add_seg` with codec None writes a segment the bind reads in
  place. R4d removed the only whole-container raw switch, which was
  role-keyed (`cir_forest_storage_codec`, the dialect image's).
- Labels: a label equal to the profile's own config is skipped, so a list
  naming the freeze's own standard yields one fewer `shared` line. Labels are
  asserted, not proven.
- Measured view equality: the config-blind `content` digest line of
  `--dump-forest` (`forest_bind_gate.sh` `[reprogate]`).
- `forest_bind_env_ok` does NOT protect a wrong label. Its checks are off by
  default, and a want-undefined / have-defined branch macro is skipped. A label
  must therefore be right by measurement.
- Pitfalls:
  - `ensure_ledger_forest` / `ensure_source_forest` reuse the BOUND forest;
    a C compile bound to the C profile would look for the AOT ledger and the
    source arm in the C profile.
  - The relayout rewrites only the LAST container and refuses one already laid
    out, so the order is: freeze C, lay out C, freeze C++, lay out C++.
  - `forest_pack.sh`'s dump and the `pack-bytes` gate cover only the newest
    container.
  - `forest_pack_layout.sh` hard-codes `--std=madc` and `.mad` roots.

## Rulings

- OWNER 2026-10-18: the pack precompiles ONLY the defaults, gnu17 (C) and
  gnu++17 (C++): "we can't precompile multiple variants as that will take up
  too much space, but we need to default to the precompiled version". gcc 13
  and clang 18 both default to gnu17. The bare `--std=c` / `--std=c++` name
  gnu17 / gnu++17 (they named c11 / c++11), and the tests use the gnu
  variants. The c17 profile (2.1 MB Linux, 4.1 MB win64) is gone; c17 and
  c11 parse the pack's text. A measured label (gnu11, c++17) costs no bytes
  and stays.

- The C roots list starts as the C spellings of the C++ list's C roots, plus
  madc's own C headers. This is the shape of Test 3 (24 headers, 214 units,
  4.3 MB raw), the shape the owner ruled on. Like the C++ list, it is a
  speed-up list, never the coverage boundary: the source arm serves every
  system header as text. It grows deliberately, and every root must freeze
  quietly under gnu17. Cost if wrong: a C program that includes a header off
  the list parses that header from text, as today.
- The raw switch is a storage flag on the freeze, `--freeze-raw`, never a
  role. A role names what a container is, and the C layer is a SYSTEM
  profile. A flag, not a valued option: there is nothing to misspell.
- The ledger and source-arm lookups find the profile that CARRIES them,
  rather than reusing the bound one. The C profile does not duplicate them.
- KG: Decision `shared_pack_profile_configs` (one C++ profile, 2026-10-11)
  ruled out per-config C++ profiles. The C layer is the owner's later, explicit
  ruling (10-17) and does not contradict it. Both nodes say so.

## Slices

- **J1 — raw containers.**
  - `--freeze-raw` writes the profile with no codec: every segment, the MIR
    cache and the ledger, with no level and no compressed intern spine.
  - `--dump-forest` reports `compressed=0` for it.
  - Gate: `forest_bind_gate.sh` `[rawgate]`. A `.c` consumer frozen raw shows
    0 codec segments, and a `.c` compile binds it with live-equal output. The
    control (the same freeze without the flag) stores codec segments.
- **J2 — the C freeze.**
  - `scripts/forest_pack_c_headers.txt` (v1, canonical order).
  - `FOREST_SHARED_C_STDS` in `forest_pack_share.sh`; `forest_share_check`
    per list.
  - `forest_pack.sh`: the C TU (`tmp/forest_pack_c_tu.c`) is frozen first,
    raw, and laid out (`forest_pack_layout.sh` takes the standard and the
    roots' extension). The C++ profile is then frozen on top, as today.
  - Measurement decides the labels and freezes: freeze the C TU under gnu17,
    gnu11, gnu99, c17, c11 and c99, and compare the `content` digests.
    - Standards equal to gnu17's are labels.
    - c17 must bind (the owner's gate). If its view differs, c17 gets its own
      raw freeze, with the standards equal to it as its labels.
    - Record the table in this plan.
  - Measured 2026-10-18 (the 30 v1 roots, `--freeze-raw`, dev binary at J3;
    every freeze exit 0, zero errors, zero warnings):

    | std   | content digest     | units | bytes     |
    |-------|--------------------|------:|----------:|
    | gnu17 | `e590413d340dccaf` |   163 | 3,402,928 |
    | gnu11 | `e590413d340dccaf` |   163 | 3,402,928 |
    | gnu99 | `ee5c1efaba9fe1a3` |   163 | 3,400,496 |
    | gnu89 | `afc84172a23f72fe` |   163 | 3,400,656 |
    | gnu23 | `10faeec2de1ee623` |   165 |         — |
    | c17   | `3e3addbec7c2bb69` |   114 | 2,122,288 |
    | c11   | `3e3addbec7c2bb69` |   114 | 2,122,288 |
    | c99   | `39e6538359c0aa7d` |   113 | 2,113,184 |
    | c89   | `2a7b606e9399bb3f` |   113 | 1,918,912 |
    | c23   | `1f382fbd50255115` |   116 |         — |

  - Ruling: the C layer is TWO raw profiles, `FOREST_C_LAYER` rows
    `c17 c11` (frozen first, the bottom) and `gnu17 gnu11` (directly under the
    madc profile, so a default `.c` compile reads it second). gnu99, gnu89,
    gnu23, c99, c89 and c23 each differ from both and parse the pack's text.
    Cost if wrong: those six configs keep today's text-parse cost; a seventh
    freeze each would add 2 to 3.4 MB raw.
- **J3 — one owner for the ledger and the source arm.**
  `ensure_ledger_forest` / `ensure_source_forest` walk the stack for the
  profile that carries the segment. Reducer: a `.c` compile with a header
  served from the source arm, and a `.c` AOT build.
  - Done: `forestMatchLedger` (a profile that carries the ledger segment,
    `CirFrozenForest::has_ledger`) for the ledger walk; the source walk takes
    the newest profile of the stdlib flavor, the bound profile only when the
    walk finds none. Gate: `forest_bind_gate.sh` `[clayer]` — a two-profile
    stack (a raw gnu17 C layer below a madc profile carrying `--freeze-sources`
    and `--freeze-ledger`).
  - Measured on 0.105.0 (the leg without `--freeze-raw`): the ledger half is
    red ("-static-libmadc needs this madc's AOT ledger, and no carrier
    provided one"). The source half was green there by ORDER: the first
    system include asks the source arm before the grove bind opens, so the
    bound-forest reuse never fired. J3 removes that order dependence.
  - Found: the bind gate's subject wraps `--std=madc`, so `[rawgate]`'s `.c`
    freeze and binds ran under madc. Both legs now name `--std=gnu17`.
- **J4 — the gate.**
  - A `forest_pack_gate.sh` leg, run by `forest_pack.sh`: a `.c` compile and a
    `--std=c17` compile on the packed binary, red unless each reports bound
    units ("bound to grove unit" / `--show-stats` units bound > 0).
  - A dump check: a C profile with `compressed=0`.
  - `--selftest` legs: bound is GREEN; mismatch, no log, or compressed is RED.
  - A `c-pack-bytes` baseline line.
  - The parse-cost `c_headers.forest` row (and `sqlite3.forest`) re-recorded
    at the new level.
  - Found (the first packed parse-cost run): `sqlite3.forest` went RED — the
    compile failed. sqlite3.c defines NDEBUG and _GNU_SOURCE ahead of its
    system includes; the C layer froze `<assert.h>` and `<features.h>`
    without them, and the bind served that view (a live `assert`, no
    `mremap`). The branch-macro environment check that declines such a root
    exists, but it is a default-OFF rollout knob (`MADC_FOREST_ENV_CHECK`,
    held off by the C++ packed-suite frontier).
  - Ruling: a C compile's bind is always verified (`forest_bind_env_ok`,
    `is_c_mode()`); the C++ default stays the knob's. C programs set feature
    macros as a matter of course, and the C layer has no knob-off frontier.
    Cost if wrong: a declined C root parses live (slower, never wrong).
    Reducer: `forest_bind_gate.sh` `[cenv]`, knob off (gcc, clang and live:
    `mremap=1`; bound before the fix: "use of undeclared identifier
    'only_in_debug_builds'").
  - Parse-cost on the packed release (2026-10-18, 91c63d95a + this slice):
    `c_headers.forest` 243.0M -> 52.1M Ir (-78.6%; gcc -O0 -c 144M, clang
    127M); before the verified bind it read 38.1M, but that bind served
    sqlite3 the wrong view. `sqlite3.forest` compiles again, flat (+0.04%):
    its NDEBUG / _GNU_SOURCE decline its roots to a live parse, as they must.
  - Found: under the verified check `<stdio.h>` DECLINES — unit
    `bits/libc-header-start.h` consults `__GLIBC_INTERNAL_STARTING_HEADER_IMPLEMENTATION`,
    which every glibc header defines just before including it; the dep's
    recorded definer is the FIRST such header of the freeze (not in a later
    root's closure), so the check calls it external. Ruling: not widened here
    — the frozen PP events carry no include positions, so "a closure unit
    defines it" cannot prove the define precedes the consult. Follow-up: record
    the include position (or the definer per consult) in the freeze, then rule
    the dep replay-internal. Cost now: `<stdio.h>` parses live under the C
    layer (52.1M, not ~38M).
    FIXED 2026-10-18 (owner: "you only did half the work"), forest v67, two
    causes:
    - glibc: a repeat consult whose definer is an OPEN ANCESTOR (still on
      the include stack, so its define precedes the consult) adds that
      definer to the recorded dep; the dep carries one tuple per definer and
      ANY definer inside the closure makes the name replay-internal.
    - mingw (win64): `stdio.h`'s `#ifndef WEOF` / `#ifndef NULL` arms were
      skipped at freeze (the earlier `ctype.h` / `stddef.h` roots defined
      them) and recorded as replayable arms, but the verified C bind
      declined on the same consult's want-defined dep first. A dep whose
      consult IS the captured arm (writing the name alone, over an
      object-like external definition) now carries
      `CIR_BRANCH_DEP_ARM_REPLAYS`; the bind runs the arm in the consumer's
      own state and, when it yields the frozen value, takes the replay. An
      arm whose value differs still declines. Gate: `forest_bind_gate.sh`
      `[armreplayc]`.
    The C gate (`forest_pack_gate.sh --bind-log`) now fails a probe that
    DECLINES any root, not only one that binds nothing.
  - Found: B199 — a C compile searched libstdc++'s include directories, so
    the C layer's `stdlib.h` / `math.h` units were the libstdc++ wrappers
    (libc++'s, for nearly every C root, on darwin). FIXED 2026-10-18 (owner:
    "fix it now"): `Program::search_view` cuts the flavor's table at its
    compiler-owned dir — a C compile searches from that dir on, gcc's and
    clang's `-x c` list — so property 1 holds outright: the C layer's units
    are the C library's own headers on every platform.
- **J5 — platform arms.** The win64 and darwin pack scripts freeze their own C
  lists (mingw/UCRT's C set; darwin's libc). They are built on the container
  and run at the seam.
  - The stage and its gate have ONE owner, `scripts/forest_pack_c_layer.sh`
    (`forest_pack_c_freeze`, `forest_pack_c_gate`); the Linux pack calls it
    and reproduces J2/J4 exactly (every gate OK).
  - win64 measured 2026-10-18 (`scripts/forest_pack_c_headers_windows.txt`,
    the hosted PE under wine, `--freeze-raw`; every freeze exit 0, zero
    errors, zero warnings, 59 units each): gnu17 == gnu11
    (`ec19d918fe34fde0`, 4,100,760 bytes), c17 == c11 (`2089c41be0ad343f`,
    4,079,592 bytes); gnu99, gnu89, gnu23, c99, c89 and c23 each differ. The
    same classes as Linux, so `FOREST_C_LAYER` holds for both.
  - DONE (win64, 2026-10-18): `release-windows` freezes the one gnu17 row
    below the madc profile and gates it on the shipped PE — `[win64-gnu17]`
    quiet freeze, 25 roots, raw, 4,112,528 bytes (recorded), gnu17 and gnu11
    bind; `[win64-c-default]` binds. The layout pass had failed on
    `<stdlib.h>` alone ("Identifier 'div_t' already defined") under gnu17 as
    under c17. The chain, traced: the C compile resolves `<stdlib.h>` to
    libstdc++'s wrapper (B199); the bind declines the wrapper root and
    mingw's `stdlib.h` (`_SECIMP` defined at freeze, opposite here), so the
    wrapper's `#include_next <stdlib.h>` live-tokenizes the embedded mingw
    copy — and `named_include_provider_exists` applied the from-the-top
    "outranked" test to that `#include_next`, so the `<stdlib.h>` once-only
    key was never recorded. mingw's tail `sec_api/stdlib_s.h` then bound a
    closure holding the `stdlib.h` unit; with no live-tokenized evidence the
    dep check took the MASKED-BIND arm instead of the prune and installed the
    frozen decls beside the live ones. The fix: the include_next arm skips
    the outranked test, as the tokenize site already did; the unit now
    prunes ("already present ('_INC_STDLIB' guard live)"). Reducer
    `tests/teststdlibreincludebind_c`. This was the frontier
    `forest_bind_env_ok`'s comment names for the C++ default ("`div_t`
    defined twice"); whether the C++ knob can now default on is a separate
    measurement.
  - DONE (darwin, 2026-10-18): `forest_pack_darwin.sh` freezes the gnu17 row
    onto the empty container first, raw, with the same freezer behind
    `--no-sysroot-includes`; the madc profile stacks on top (one standalone
    container, footer at EOF, embedded by `-sectcreate`). Roots:
    `scripts/forest_pack_c_headers_darwin.txt` (LIST VERSION 1, 45 — the
    prelude manifest's 36, then madc's own 9; prelude FIRST, or the pack
    check gate refuses "repeated declaration max_align_t" — the prelude's
    flattened clang stddef declares it unguarded). Measured with the cross
    freezers: gnu11 == gnu17 on both arches (arm64 `9a4d33854d2f0dea`,
    x86-64 `a295be14db94ca6b`). The gate's consumer is the freezer reaching
    the container explicitly (`--forest-bind`, `--emit=c11`), the split the
    parse leg already uses: OK `[darwin-c-default]`, OK `[darwin-gnu17]` on
    both arches (quiet, 45 roots, raw — arm64 4,572,304 bytes, x86-64
    5,361,392 recorded — gnu17 and gnu11 bind). The run on a Mac against the
    shipped binary rides `mac_battery.sh` at the seam. At this commit the C
    probe's `<stdio.h>` bound as libc++'s C wrapper (`.../c++/v1/stdio.h`) —
    B199 on darwin, where libc++ wraps nearly every C header; B199's fix
    (J4 "Found", above) takes the C++ directory off a C compile's search, so
    every darwin C root is the prelude's own.
  - `FOREST_PACK_TIMEOUT` (`forest_pack_share.sh`) is the one capped-run
    command for every pack step — GNU `timeout`, or `gtimeout` on a Mac build
    host — and the C stage calls the pack gate beside itself, so it runs from
    `src/` (darwin) as from the repo root (Linux, win64).
- **J6 — docs.** `docs/test-status.md`, ROADMAP row 8.12J, the KG nodes.

## Targets

- `c_headers.forest` from 243M Ir toward the C++ ratio, and below gcc's 144M.
- A `.c` `<stdio.h>` program binds; the 8 everyday C headers in about 24 ms
  against 159 ms live (Test 3's C-only raw arm).
- Pack size: the C layer adds about 4 to 5 MB raw (Test 3's 4.3 MB for 24
  headers). The owner ruled raw.
- C++ and dialect rows flat.

## Thread safety

The pack is immutable once built. A bind reads a profile through the
per-Program `ForestSide`, so no new shared mutable state is added. A raw
container is read in place, also immutable.

## Validation

Per slice: Tier 1 + Tier 2, the static gate loop, the touched unit binaries,
the packed release's `forest_pack_gate` and parse-cost. The batch lane per
batch. The seam is the 0.106.0 release boundary.
