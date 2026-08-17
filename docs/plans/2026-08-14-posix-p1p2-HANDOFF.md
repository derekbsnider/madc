# HANDOFF — POSIX P1/P2 + the zero-warnings law, session #88 (2026-08-14)

**Read this fully before acting. Assume a cold start.** Run
`bash scripts/resume.sh` first — it prints live git state and any orphaned
background jobs, which a compaction summary cannot.

Branch: **`feature/posix-p1p2-codex`**, based on `develop` @ `c1abb482`
(v0.79.0). Tasks **#48** (POSIX surface) and **#50** (zero-warnings law).

---

## 1. What is DONE and validated on this branch

### POSIX target surface (task #48)

| commit | what |
|---|---|
| `6bea22f1` | (Codex) T1 — target-gated POSIX header **supplement** mechanism |
| `157aa8ef` | (Codex) target-aware ledger membership (`select_ledger_sources.sh`) |
| `3b5295b2` | (Codex) T2 `strndup`, T4 `timeradd`/`timersub`, D3 lowercase `sleep` |
| `6b43a9d4` | (Codex) T7 `scripts/win_posix_archive_gate.sh`, wired into `fulltest` |
| `08a76f43` | (Claude) fix: class-nested enum TAG leaked into the global `datatype_map` |
| `2e85903a` | (Claude) T3 `setenv`/`unsetenv` + T5 `<dlfcn.h>` + `TargetOS` gating |
| `eb3cf5dd` | (Claude) fix: class-nested enum ENUMERATORS scoped to their owner |

### The pre-merge `/dupaudit` and what it found (task #48, this session)

| commit | what |
|---|---|
| `71a2a8af` | **fix(lexer): `__has_include` must see the POSIX whole-provider arm** |
| `656bf873` | **fix(cir): the cross arm's missing include + a dlopen that cannot succeed** |

`/dupaudit` scoped to the include/header-serving paths (required by
`branching.md` before a feature branch merges — this branch ADDED a serve
path). It reported three families; the top one was a **live silent wrong
answer** and is fixed:

- **`named_include_resolution_plan` grew 3 → 4 sites.** The whole-provider arm
  decides "no native provider ⇒ `posix/<name>` serves it" AFTER the filesystem
  walk, at a position `__has_include` did not mirror. Confirmed on the hosted
  PE under wine: `has_dlfcn 0` while `served 1`. The canonical
  `#if __has_include(<dlfcn.h>)` idiom took the no-dlfcn branch on a target
  that serves dlfcn. Fixed by splitting the decision into
  `Program::posix_whole_provider_serves()` — one owner, two consumers.
  Reducer `tests/testhasincludeagrees.mad` (gcc + clang oracles agree).

Two families remain **open and unfixed** — structural, no live symptom:
- `posix_supplement_owed_predicate` (5 call sites, 5 different guards; `:4600`
  is UNGATED where its four siblings apply `is_system_header_path`, `:4669`
  omits the `!protocol_visit` term `:4738` carries).
- `posix_internal_key_serve_tail` (2 character-identical serve tails).

### The zero-warnings law (task #50, owner directive this session)

**OWNER LAW: no warnings anywhere, on either surface.** Both are now at zero
and BOTH are enforced.

| commit | what |
|---|---|
| `041d1dcf` | build: three flag-hygiene defects (see below) |
| `903e88d0` | fix(cir_builder): undersized `%zu` buffers + side-effecting `typeid` |
| `87efb272` | fix(parser): INTSPLIT `typeid` operands had side effects |
| `24b23eed` | fix: two definitions whose target condition ≠ their use |
| `e047747b` | fix(madcdis): 35 driver `override` markers |
| `894533e6` | fix(va_helpers): snprintf-backed formatting (`sprintf` deprecated on Apple) |
| `52b1297f` | docs: warning baseline back to all-zero |
| `a5e0a5a5` | gate: `check-cross-mode-compiles.sh` |
| *(see §2)* | build: **`-Werror`** |

---

## 2. Facts that cost real time — do not re-derive

- **`?=` SILENTLY NO-OPS on variables make predefines.** `$(origin X)` is
  `default`, not `undefined`. Verified with one-line makefiles:
  `ARFLAGS ?= rcs` never applied, so **every archive was built `rv`** (no `c`
  ⇒ ar warns "creating <archive>"; `v` ⇒ narrates members). Fixed with an
  `ifeq ($(origin ARFLAGS),default)` guard. **Still standing:** `CC ?= clang`
  and `CXX ?= clang++` (src/Makefile:11-12) also no-op, so **the host builds
  with g++ though the Makefile declares clang** — which is exactly why
  clang-only diagnostics (the 35 `override`, the `typeid` operands) never
  appeared on the host lane. Flipping that is an OWNER decision, not a
  cleanup: KG Gap `conditional_assign_noop_on_make_builtins`.
  `CXXFLAGS`/`CPPFLAGS`/`LDFLAGS`/`INSTALL` are `undefined`, so THEIR `?=`
  works and `-Wall` really is applied.
- **INCREMENTAL BUILDS HIDE WARNING CLASSES.** Each only compiles the objects
  it touches, so each run shows a different subset. Two rounds of *clean*
  darwin builds were needed before the count actually reached zero. Always
  `rm -rf obj/<mode>` before claiming a lane is clean.
- **A ratchet with a stale-high entry reports GREEN over a reached goal.**
  `warn_census.sh` printed `tests improved : 1` on every fulltest run since the
  nullptr-as-integer family was fixed, and nobody lowered the entry. Verified
  zero directly (`testcoutstr_libcxx.warnings` is 0 bytes; no `.warnings` file
  in the census is non-empty) before removing it.
- **`make -n` prints NOTHING for an up-to-date target.** My first `-Werror`
  plumbing probe returned zero for every case and looked like a wiring
  failure. Use `make -Bn`. A null needs a reason before you believe it.
- **The cross-* modes were in NO validation lane.** madc_cir.cpp's cross arm
  had been uncompilable since 2026-08-12 (`9afb4717`) while all five lanes
  were green, across the v0.79.0 release — and the macOS release artifacts
  build through exactly that mode. Now gated
  (`scripts/check-cross-mode-compiles.sh`, derived TU list, negative-controlled
  both ways). KG Gap `cross_modes_ungated_by_any_lane`.
- **The NAS never builds or tests.** `bash scripts/remote_build.sh sync`, then
  `ssh -p 2299 dev@localhost` with **absolute paths**. The mingw toolchain
  exists ONLY on the container; a toolchain query on the NAS returns "absent"
  for things that exist.
- **`remote_build.sh sync` excludes `tmp/`** (gitignored), so a scratch probe
  written there must be `scp`'d to the container separately.
- **mingw include root:** `/usr/share/mingw-w64/include`. Ships `string.h`,
  `stdlib.h`, `sys/time.h`, `dirent.h`, `unistd.h`, `fcntl.h`; does **not**
  ship `dlfcn.h` or `sys/socket.h`. `timerisset`/`timercmp`/`timerclear` ARE
  provided (from `_timeval.h`); only `timeradd`/`timersub` are missing.
- **The hosted PE exports 5873 names** (`x86_64-w64-mingw32-objdump -p`) and
  zero matching `numpunctIcE2id`. `nm -D` on a PE reports 0 — WRONG TOOL.
- **L0 is not L3.** `src/rt/rt_posix_dl.c` must NOT forward to `madcdl_*`: an
  emitted-C program links `libmadc_rt.a` alone.
- **Wine lane**, canonical, persistent server first:
  `WINEDEBUG=-all wineserver -p` then
  `WINEDEBUG=-all MADC_BIN=bin/madc-hosted-x86-64-windows.exe MADC_WRAPPER=wine MADC_SKIP_EXT="win64 wine64" bash scripts/run_tests.sh`
- **`src/embedded_headers.cpp` is generated but TRACKED**; run
  `bash scripts/gen_embedded_headers.sh` locally when `include/madc/**` changed.
- **Never `git add -A`.** `test.mad` / `testsort.mad` are the owner's.

---

### Validation at `63f008ad` (all six lanes, Claude-run)

| lane | result |
|---|---|
| `fulltest` (incl. the new cross-mode gate) | rc=0 — **1040** / 0 fail / 0 timeout / 9 skip |
| libc++ JIT (`--stdlib=libc++`) | **1036** / 0 / 0 / 13 |
| `--exe` | **1009** / 0 of 1040 |
| `--obj` | **1009** / 0 of 1040 |
| persistent-Wine hosted Win64 | **998** / 0 / 0 / 51 |
| `-Werror` clean rebuilds | host `-O0`, release `-O2`, `hosted-x86-64-windows`, `hosted-arm64-macos`, `hosted-x86-64-macos` — each rc=0, **0 warnings**, incl. 39 unit-test binaries |

---

## 3. Open items, in order

1. **Merge to `develop` WITH a release** (`/release minor`), per the standing
   owner rule that a feature merge ships with one.
3. **Task #49 — branch housekeeping**, explicitly AFTER the merge. Do not
   blind-delete: `feature/forest-b4b-bind-claude` is DO-NOT-MERGE,
   `codex-header-parse-eval` is a preserved WIP snapshot,
   `feature/libcxx-typedef-identity-wip-claude` is WIP, and
   `feature/gecko-parser-tables-codex` is checked out in the
   `/workspace/madc-master` worktree.
4. **T6 `dirent`** — owner decision: **its own release**. Not a leaf — mingw
   ships `dirent.h` and its `readdir` fills mingw's `struct dirent`, so serving
   `d_type` means owning the header AND `opendir`/`readdir`/`closedir` over
   `FindFirstFileW`. Half-serving it with a shadowed struct the CRT never
   fills would be a silent wrong answer.
5. **Windows W3** (PE/COFF writers), W4 groves, W5 packaging.

### Carried, not blocking

- `holder::part::sign` — the three-level qualified spelling of a class-nested
  enum's enumerator is rejected (a LOUD error, not a wrong answer). Owner
  `src/parser.cpp` qualified-class descent: it requires a `DataDefCLASS` to
  keep descending, so a nested enum tag dead-ends. Fix shape: add a
  `DataDefENUM` arm that consumes the `::` and resolves via
  `find_namespace_member` against the owner-keyed pseudo-namespace, pushing
  onto `exStack`. KG `Gap{nested_enum_qualified_tag_spelling}`.
- The two open `/dupaudit` families in §1.
- The darwin **grove husk**: `forest_pack_darwin.sh` emits ~58 madc parse
  errors on libc++ (`__atomic_is_lock_free`, a clang builtin madc lacks). The
  build succeeds and this is pre-existing macOS-lane residue, but the darwin
  grove is thin because of it.

---

## 4. Process notes worth carrying

- **Negative-control every gate before wiring it in.** `check-cross-mode-compiles.sh`
  was run both ways (include removed ⇒ exit 1 naming the TU; restored ⇒ exit 0).
  `-Werror` was verified with `make -Bn` to be on both the C++ and the `rt/*.c`
  compile lines, and absent under `WERROR=0`.
- **"Pre-existing" is not a disposition and there are no files that aren't
  yours.** Both defects that mattered most this session — the two-day cross
  build break and the `-nostdinc++` hazard — were found only because warnings
  in files "someone else" touched got analyzed instead of counted.
- **Do not re-run a green suite to read a number.** The count is already in
  the lane output.
