# SMAUG 1.8 Port Progress

Running estimate of how close madc is to executing the SMAUG 1.8 source
umbrella end-to-end. Updated each time a front edge advances or a
language gap closes.

The port itself lives in the external [MadSMAUG](https://github.com/derekbsnider/MadSMAUG)
repo; madc lands the language gaps surfaced by the port.

Upstream total: **158,537 lines** across `MadSMAUG/upstream/smaug1.8/src/*.{c,h}`
(including IMC sources, which will be skipped on the first-pass
bootstrap).

## Current state — 2026-04-24

| Phase            | % | Notes |
|------------------|--:|-------|
| **Parse**        | ~18% | 27,821 / 158,537 lines ingested via the umbrella + fully parsed |
| **Compile**      | ~18% | Same scope — JIT finalize succeeds (pre-existing `finalize error=26` is non-fatal) |
| **Link**         | 0%  | Umbrella stops on the first undefined symbol per C file added |
| **Runtime**      | 0%  | `main()` has never been executed against the full umbrella |

These numbers are rough. Headers are ingested whole; each C file that
parses cleanly counts as fully parsed. The link column will be
meaningful once we start stubbing and connecting more TUs.

## Files ingested by `MadSMAUG/src/SMAUG.mad`

| File            | Lines | Parse | Compile | Notes |
|-----------------|------:|:-----:|:-------:|-------|
| `mud.h`         | 5,971 | ✅    | ✅      | Forward typedefs, struct layouts, macros |
| `bet.h`         |   140 | ✅    | ✅      | |
| `hint.h`        |    17 | ✅    | ✅      | |
| `house.h`       |   156 | ✅    | ✅      | |
| `news.h`        |    91 | ✅    | ✅      | |
| `act_move.c`    | 2,835 | ✅    | ✅      | `grab_word`'s `*p++ = rhs` closed this session |
| `db.c`          | 7,936 | ✅    | ✅      | `#undef bug` wrap in the umbrella avoids the macro-over-definition collision at db.c:4184 |
| `hashstr.c`     |   236 | ✅    | ✅      | Runs standalone as `hashstr.mad` |
| `handler.c`     | 4,887 | ✅    | ✅      | `EXT_BV` struct-copy, `(*p).member`, `.4` floats, `expr[i].member`, struct-array subscripts — all closed this session |
| `ibuild.c`      | 3,891 | ✅    | ✅      | |
| `ident.c`       |   354 | ✅    | ✅      | |
| `interp.c`      | 1,307 | ✅    | ✅      | |
| **Subtotal**    | 27,821 | | | |

## Not yet ingested

| File            | Lines | Blocker |
|-----------------|------:|---------|
| `fight.c`       | 4,521 | Current front edge — `learn_from_failure` referenced by ibuild.c, defined here |
| `comm.c`        | 4,091 | Shimmed; real `comm.c` has descriptor / socket / color code |
| `update.c`      | 3,144 | |
| `magic.c`       | 7,394 | |
| `mud_prog.c`    | 4,019 | |
| `act_comm.c`    |       | |
| `act_info.c`    |       | |
| `act_obj.c`     |       | |
| `act_wiz.c`     |       | |
| …many more…    |       | See `MadSMAUG/upstream/smaug1.8/src/Makefile` C_FILES for the full list |

IMC sources (`imc*.c`) are deferred — the first-pass goal is the core
SMAUG game loop without MUD-network federation.

## Pinned language gaps (from `TODO.md`)

- `a + b` in some declaration-init contexts produces wrong result (pre-existing)
- `p[i].member` via raw pointer subscript segfaults at runtime (pre-existing)
- Function-like macros shadowing later definitions (workaround via umbrella `#undef`)
- `int a = -2;` declaration-init drops the unary `-` (pre-existing)

## How the estimate is maintained

- Parse/compile %s: bump when a new upstream file is added to the
  umbrella and parses cleanly (or when the front edge advances within
  a file that wasn't fully covered). LoC counts come from
  `wc -l upstream/smaug1.8/src/*.{c,h}`.
- Link %: bump when undefined-symbol errors fall below some threshold
  (today, a single missing symbol still stops the umbrella, so this
  stays 0%).
- Runtime %: bump when `bin/madc SMAUG.mad` actually runs without
  SIGSEGV; then staged upward as basic features work (login, movement,
  combat, persistence).

Precision doesn't matter much — the goal is direction and a shared
picture of how close we are.
