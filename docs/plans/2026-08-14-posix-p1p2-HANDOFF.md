# HANDOFF — POSIX P1/P2 branch, session #88 (2026-08-14)

**Read this fully before acting. Assume a cold start.** Run
`bash scripts/resume.sh` first — it prints live git state and any orphaned
background jobs, which a compaction summary cannot.

Branch: **`feature/posix-p1p2-codex`**, based on `develop` @ `c1abb482`
(v0.79.0 released and pushed). Task **#48**.

---

## 1. What is DONE and validated on this branch

| commit | what |
|---|---|
| `6bea22f1` | (Codex) T1 — target-gated POSIX header **supplement** mechanism |
| `157aa8ef` | (Codex) target-aware ledger membership (`scripts/select_ledger_sources.sh`) |
| `3b5295b2` | (Codex) T2 `strndup`, T4 `timeradd`/`timersub`, D3 lowercase `sleep` |
| `6b43a9d4` | (Codex) T7 `scripts/win_posix_archive_gate.sh`, wired into `fulltest` |
| `31e69288` | (Claude) doc: re-correct the `timer*` delta — my grep missed the include closure |
| `08a76f43` | (Claude) **fix(parser)**: class-nested enum TAG leaked into the global `datatype_map` |
| `d5d5294d` | (Claude) status mirror |
| `2e85903a` | (Claude) **T3 `setenv`/`unsetenv` + T5 `<dlfcn.h>` + `TargetOS` gating** |
| *(pending)* | (Claude) **fix(parser)**: class-nested enum ENUMERATOR pseudo-namespace |

### The three defects fixed here (none were in the brief)

1. **Nested enum TAG leak** (`08a76f43`). `TokenENUM::parse()` called
   `set_class_type_alias` *and* wrote the bare tag into the global
   `datatype_map` + enclosing namespace. libc++'s `money_base::part` leaked
   as `part` / `std::part` / `std::__1::part`, so a user variable named
   `part` made `(part == 0)` parse as a **cast** ("Missing operand").
   **General, not libc++-specific** — the reducer fails on the archived
   pre-fix `tmp/release-bins/madc-release-v0.79.0` under the default flavor.
2. **Nested enum ENUMERATOR leak** (pending commit). Same family, one level
   over: the pseudo-namespace was keyed on the BARE tag in class scope, so
   `part::sign` resolved **from anywhere** — madc printed 3 where both
   oracles reject the source. Now keyed `Owner::tag`, with
   `Program::resolve_class_scoped_ns()` giving the in-class relative
   spelling its scope walk.
3. **`posix_compat_enabled()` gated on `#ifdef _WIN32` at the CONSUMER**,
   which `include/datadef.h` explicitly forbids ("never re-test `_WIN32` at
   a consumer"). Now `TargetOS` / `target_windows()`, the third member of
   the target-property family beside `target_llp64()` and
   `target_microsoft_bitfields()`.

---

## 2. The ONE thing still open on this branch

**`holder::part::sign` — the three-level qualified spelling of a
class-nested enum's enumerator — is rejected.** Both `g++ -std=c++11` and
`clang++ -std=c++11` accept it and print 3.

- **Error:** `'part' is not a class type in 'holder'`
- **Owner:** `src/parser.cpp:26786` (the qualified-class descent). When the
  next token is `::` it requires `member_dd` to be a `DataDefCLASS` to keep
  descending, and throws otherwise. A nested **enum** tag dead-ends there.
- **Fix shape:** add a `DataDefENUM` arm before that throw — consume the
  `::`, take the enumerator name, resolve it with **`find_namespace_member`**
  (the existing owner, already used by the `ns::Tag::Value` path at
  `src/parser.cpp:12083`) against the owner-keyed pseudo-namespace
  `<scope canonical spelling>::<tag>`, and push the constant onto `exStack`.
  Read that function's `QualifiedClassExprAction` / `exStack` contract
  before writing the arm — it is large and pushes results, it does not
  return them.
- **Reducer** (`tmp/` only — deliberately NOT in `tests/`, it would fail):
  add `cout << (int)holder::part::sign << endl;` to
  `tests/testnestedenumscope.mad`; oracle output is `3`.
- **Severity:** a LOUD error, not a wrong answer. The silent half is fixed.
- KG: `Gap{nested_enum_qualified_tag_spelling}`.

---

## 3. Then, in order

1. **`/dupaudit`** scoped to the include/header-serving paths (`branching.md`
   requires it before a feature branch merges). This branch ADDED a serve
   path — `Program::tokenize_posix_whole_provider` — which is exactly where
   a duplicate is born.
2. **Merge to `develop` WITH a release** (standing owner rule: a feature
   merge to develop ships with a release). Use `/release minor`.
3. **Task #49 — branch housekeeping** (owner directive, explicitly AFTER
   the merge): 10+ stale `feature/*` branches on origin. Do not blind-delete
   — `feature/forest-b4b-bind-claude` is DO-NOT-MERGE,
   `codex-header-parse-eval` is a preserved WIP snapshot,
   `feature/libcxx-typedef-identity-wip-claude` is WIP, and
   `feature/gecko-parser-tables-codex` is checked out in the
   `/workspace/madc-master` worktree.
4. **T6 `dirent`** — owner decision 2026-08-14: **its own release**, not this
   one. It is not a leaf: mingw ships `dirent.h` and its `readdir` fills
   mingw's `struct dirent`, so serving `d_type` means madc owns the header
   AND `opendir`/`readdir`/`closedir` over `FindFirstFileW`. Half-serving it
   with a shadowed struct the CRT never fills would be a silent wrong answer.
5. Then Windows **W3** (PE/COFF writers), W4 groves, W5 packaging.

---

## 4. Facts that cost real time to establish — do not re-derive

- **The NAS never builds or tests.** `bash scripts/remote_build.sh sync`,
  then `ssh -p 2299 dev@localhost` with **absolute paths**. The mingw
  toolchain exists ONLY on the container: a toolchain query on the NAS
  returns "absent" for things that exist. I lost time to exactly that.
- **mingw include root:** `/usr/share/mingw-w64/include` (on the container).
  It ships `string.h`, `stdlib.h`, `sys/time.h`, `dirent.h`, `unistd.h`,
  `fcntl.h`; it does **not** ship `dlfcn.h` or `sys/socket.h`.
- **`timerisset`/`timercmp`/`timerclear` ARE provided**, from `_timeval.h`
  which `sys/time.h` includes. Only `timeradd`/`timersub` are missing. My
  handoff #3 said otherwise because I grepped one file instead of the
  closure.
- **The hosted PE exports 5873 names** (`x86_64-w64-mingw32-objdump -p`),
  and **zero** matching `numpunctIcE2id` — static libstdc++ contributes no
  `_Z*` exports. `sleep` IS in that table (the positive control). This is
  why `testclassstaticitanium` cannot pass on win64 and why its skip stays.
  `nm -D` on a PE reports 0 total exports — it is the WRONG tool; use
  `objdump -p`.
- **L0 is not L3.** `src/rt/rt_posix_dl.c` must NOT forward to `madcdl_*`:
  an emitted-C program links `libmadc_rt.a` alone and the forward would be
  unresolved. Plan §3 settled this; handoff #3 got it wrong.
- **Wine lane**, canonical, with a persistent server started first:
  `WINEDEBUG=-all wineserver -p` then
  `WINEDEBUG=-all MADC_BIN=bin/madc-hosted-x86-64-windows.exe MADC_WRAPPER=wine MADC_SKIP_EXT="win64 wine64" bash scripts/run_tests.sh`
- **`src/embedded_headers.cpp` is generated but TRACKED.** It is regenerated
  on the container during a build and `remote_build.sh pull` does not bring
  it back, so it goes stale on the NAS. Run
  `bash scripts/gen_embedded_headers.sh` locally before committing when
  `include/madc/**` changed — no compiler needed.
- **Never `git add -A`.** `test.mad` and `testsort.mad` in the repo root are
  the owner's and stay untracked.

---

## 5. Process notes worth carrying

- **Negative-control every gate exclusion.** `check-one-dl-owner.sh` fired
  on the new target header. My first fix excluded one file and was still
  wrong — it missed `src/embedded_headers.cpp`, the generated mirror of the
  same content. The second states the boundary structurally (`include/madc/**`
  + its generated mirror are not host code) and the comment now carries the
  verification recipe. Both host surfaces were re-tested after.
- **Do not re-run a green suite to read a number.** I did once this session;
  the count was already in the EXE/OBJ lane output.
