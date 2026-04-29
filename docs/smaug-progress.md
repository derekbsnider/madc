# SMAUG 1.8 Port Progress

Running estimate of how close madc is to executing the SMAUG 1.8 source
umbrella end-to-end. Updated each time a front edge advances or a
language gap closes.

The port itself lives in the external [MadSMAUG](https://github.com/derekbsnider/MadSMAUG)
repo; madc lands the language gaps surfaced by the port.

Upstream total: **158,537 lines** across `MadSMAUG/upstream/smaug1.8/src/*.{c,h}`
(including IMC sources, which will be skipped on the first-pass
bootstrap).

## Current state — 2026-04-29 (session 12)

| Phase            | % | Notes |
|------------------|--:|-------|
| **Parse**        | ~86% | 136,166 / 158,537 lines ingested. 49 upstream TUs + `_bootstrap_comm_shim.c`. (IMC headers guarded under `#ifdef IMC` and not counted.) |
| **Compile**      | ~86% | Every ingested TU compiles cleanly post-session-10 lexer fixes (`##` token paste, `__attribute__` skip, IRBuilder::coerce dst=void fast path). |
| **Link**         | ~95% | Post-session-11 funcnode dedupe — all 1878 user-defined functions now bind labels. Previously only 168 / 1878 (9%) survived finalize. |
| **Runtime**      | ~95%+ | **`boot_db()` runs end-to-end**: all 25 area files load, area_update fires, then board / vault / clan / member-list / council / deity / watch / ban / corpse / immortal-host / hint / project / morph / login-message / color loading all complete; the bootstrap shim reaches `[probe] after boot_db`. Five session-12 fixes unstuck the `load_vaults` SIGSEGV cap: (1) mixed string-literal / char-pointer ternary type unification, (2) TokenTerQ merge_slot rewrite around emit_branch + IRBuilder::coerce, (3) dtSTRING ↔ pointer-to-char and 8-byte int ↔ dtSTRING IR coerce extensions (covers `ctime` / dlsym-fallback returns), (4) local C fixed-size array LEA re-emit on every reuse (mirrors the existing global-fixed-array re-emit; SMAUG `bug()`'s buf[MAX_STRING_LENGTH] crashed in `strcpy(buf, "[*****] BUG: ")` when the prior `if (fpArea != NULL) sprintf(buf, ...)` branch wasn't taken), (5) crash-handler `backtrace()` walk for non-JIT faulting RIPs. Only remaining surface is the game loop (sockets), which the bootstrap shim doesn't invoke. |

## Reproducing the runtime

```sh
# Set up a writable data directory tree
mkdir -p /tmp/smaug_run
cd /tmp/smaug_run
for d in area gods player system boards classes clans races system; do
  ln -sfn /workspace/MadSMAUG/upstream/smaug1.8/$d $d
done
# `area` and `system` need to be real (writable) for SMAUG to write
# back; symlinks let upstream `../system/sysdata.dat` resolve through
# the symlink target.
rm area system
cp -rL /workspace/MadSMAUG/upstream/smaug1.8/area area
cp -rL /workspace/MadSMAUG/upstream/smaug1.8/system system
# Optional: remove sysdata.dat / news.dat / stances.dat to bypass
# their parse-crash and reach later init phases.
rm -f system/sysdata.dat system/news.dat
cd area
/workspace/madc/bin/madc /workspace/madc/MadSMAUG/src/SMAUG.mad
```

The session-11 funcnode-dedupe fix unstuck the runtime blocker. Root
cause was duplicate function definitions (~125 of them, mostly shim
stubs of upstream functions) sharing one FuncDef + FuncNode. When
`cc.addFunc(funcnode)` was called twice for the same node, asmjit's
Compiler v1.14 silently dropped the labels of every funcnode added
between the duplicate addFunc calls. The fix walks `pending_funcs` in
reverse, marks earlier duplicates as `is_overridden`, and short-circuits
both `prepareFuncNode` and `TokenFunc::compile` for them. The LAST
source definition wins, which matches the user's expectation that shim
stubs override upstream defs.

Active TUs (36): act_move, db, hashstr, handler, fight, skills, news,
magic, mud_prog, stances, requests, act_comm, act_obj, boards,
act_info, act_wiz, ban, comments, const, clans, colorize, deity,
hint, grub, comm, tables, save, misc, reset, mapout, special,
makeobjs, imm_host, polymorph, planes, house — plus
_bootstrap_comm_shim, ibuild, ident, interp.

Deferred TUs:
- `variables.c` — IRBuilder::coerce surface in a different code path.
- `update.c` — same (a different surface in `damage`-adjacent code).
- `build.c` — C99 VLA (`char temp_buf[N + max_buf_lines]`); functions
  used elsewhere are stubbed in the bootstrap shim.

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
| `fight.c`       | 4,521 | ✅    | ✅      | Compiles after the return-mis-detection fix lands |
| `skills.c`      | 6,239 | ✅    | ✅      | Compiles after `class`-as-identifier + compound-subscript-assign land |
| `act_comm.c`    | 4,310 | ✅    | ✅      | |
| **Subtotal**    | 42,891 | | | |

## Not yet ingested

| File            | Lines | Blocker |
|-----------------|------:|---------|
| `mud_prog.c`    | 4,019 | Uses `goto label` — labels + goto not yet supported. Stubbed in `_bootstrap_comm_shim.c` for now (rprog/mprog/oprog leave/entry/greet triggers) |
| `comm.c`        | 4,091 | Shimmed; real `comm.c` has descriptor / socket / color code |
| `update.c`      | 3,144 | |
| `magic.c`       | 7,394 | |
| `act_comm.c`    |       | |
| `act_info.c`    |       | |
| `act_obj.c`     |       | |
| `act_wiz.c`     |       | |
| …many more…    |       | See `MadSMAUG/upstream/smaug1.8/src/Makefile` C_FILES for the full list |

IMC sources (`imc*.c`) are deferred — the first-pass goal is the core
SMAUG game loop without MUD-network federation.

## Pinned language gaps (from `TODO.md`)

- `goto label` / forward labels — needed for mud_prog.c, magic.c, etc.
- `IRBuilder::coerce() invalid src` inside a fight.c/skills.c function — current compile-time front edge. Likely a typed-pointer-return mis-wiring or a call-return path that drops `regdp.second`.
- Real `<` / `<=` comparison with Mem-backed literal RHS (surfaced this session)
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
