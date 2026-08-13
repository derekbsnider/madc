# CODEX HANDOFF #2 — close the burndown branch, then start POSIX P1/P2

**Author:** Claude (session #88, 2026-08-13). **Executor:** Codex.
**Read `AGENTS.md` first.** This is the second brief; the first
(`2026-08-13-win64-46b-burndown-CODEX-HANDOFF.md`) is **done and
accepted** — do not re-execute it.

Imperative directives, not suggestions. If a directive here contradicts
what you infer from the code, **stop and report**; do not choose.

---

## 0. Start state — verify before touching anything

```
branch : feature/win64-46b-burndown-codex   (yours again; Claude added
                                             review + plan commits on top)
HEAD   : bf723d60  "docs: W0.5 has no win64 analogue (owner correction, verified)"
tree   : clean EXCEPT owner-untracked test.mad + testsort.mad
```

`git -C /workspace/madc log --oneline -1` must show `bf723d60`. If not,
**stop and ask**.

**Your previous work is verified and accepted.** Claude independently
reproduced every lane at your `84dcb607`:

| lane | result |
|---|---|
| fulltest | rc=0 — 1029 / 0 / 9 skipped |
| libc++ JIT | 1025 / 0 / 13 skipped |
| EXE | 1000 / 0 (of 1029 JIT-passing) |
| OBJ | 1000 / 0 (of 1029 JIT-passing) |
| wine (yours) | 981 / 0 / 57 skipped |

The four compiler fixes were reviewed and stand: the MIR alloca/combine
fix, the nonvirtual-base tail-padding layout fix, the preprocessor
prescan/blue-paint fix, and the c2mir memory-shaped scalar conversion
fix. Trailers and oracles hold up. Your `wineserver` connection-reset
diagnosis supersedes the earlier "wine wobble" hand-wave — good find.

Full review with findings: `docs/plans/2026-08-13-codex-branch-review.md`.

---

## 1. SETTLED — do not re-open

Carried forward from handoff #1 (LLP64 platform model; `dd_platform_*`
as the one width owner; the container does all builds/tests; never
`git add -A`; never `git checkout` over uncommitted work), plus these,
all owner-decided or measured **this session**:

1. **The POSIX target surface is Win64-ONLY, by construction.** Linux is
   POSIX, macOS is a certified UNIX — Windows is the only non-POSIX
   target madc will ever have. Plan:
   `docs/plans/2026-08-13-posix-target-surface.md`.
2. **Take the POSIX library surface; refuse the POSIX personality.** No
   `fork` emulation, no mount/path translation, no signals-as-process-
   model, no `/proc`, no uid/gid. That mass is what `cygwin1.dll` is.
3. **PROVENANCE LAW: clean-room from POSIX.1/SUS + MSDN. Never read,
   quote or adapt Cygwin / newlib / msys2 sources** (GPL vs our MPL-2.0
   — a licensing incident, not a shortcut). mingw headers may be
   consulted for *interface facts* only.
4. **Two oracles.** Codegen / ABI / type model: `x86_64-w64-mingw32-gcc`,
   always. POSIX *semantics*: glibc on Linux (write a reducer, run it).
5. **W0.5 has no win64 analogue — measured.** mingw-w64's headers are
   *explicitly public domain* (`windows.h` and `stdio.h` both open "This
   file has no copyright assigned and is placed in the Public Domain").
   No Microsoft SDK is ever in our chain. No provenance pass needed.
6. **A forest contains the transitive CLOSURE of its entry points**, not
   the pack list. `--dump-forest` is the authority; `forest_pack_headers.txt`
   is only the entry-point list. (Claude got this wrong and was
   corrected — see §5 trap 1.)

---

## 2. PART ONE — close the branch (do this first, do not start Part Two until it merges)

### T1 — F2: make madc itself the `exec://` child *(supersedes F1)*

`testexecchannel`, `testvaluesort` and `testnsmadcautostring` all spawn
`exec://sort` and assert on its output. The subject under test is the
**channel machinery** — spawn a child, write its stdin, read its stdout.
`sort` is incidental and is the only part that does not port: Windows
`sort.exe` differs (case folding, CRLF), wine ships none, and POSIX
`sort` collation is locale-dependent (the macOS lane already carries an
`LC_ALL=C` warning).

**Directive:** replace the `sort` child with **madc itself** running a
small `.mad` child script that reads stdin and emits a deterministic
transformation. Always present, locale-free, identical on all three
platforms.

- Locate the child binary the same way the runner locates the binary
  under test (`MADC_BIN`), so the packed/release and hosted lanes each
  exercise **their own** artifact, never a stale `bin/madc`.
- The child script lives in `tests/` beside its consumers; if it is not
  a standalone test, follow the `include_helper.mad` precedent so the
  runner does not treat it as one.
- Afterwards these three should need **no** domain skip at all. Remove
  the ones that become unnecessary.

**Also revert F1's collateral:** `testnsmadcautostring.mad` was rewritten
to call `eval_expression_string`, which dropped **Linux** coverage of the
channel's gated `std::string` overloads (`write(string)`,
`readline(string&)`) — the thing its own header comment says it exists to
test. Restore that original body and let T1 make it portable. A win64
environment limit must never cost POSIX-host coverage.

### T2 — F3: settle the `testvaluesort` reclassification with evidence

Its fixture moved `win64_skip` → `wine64_skip` claiming "PASSES with
Windows sort.exe on the real Windows runner (verified 2026-08-13)". The
two domains mean different things: `win64_skip` = out of scope on
Windows; `wine64_skip` = **passes on real Windows, fails only under
wine**. The second is a claim about hardware we own.

```
MADC_WIN_RUNNER="bash /workspace/madc/scripts/win_run.sh"   # ABSOLUTE path
```
`win_run.sh` runs the binary as a genuine Windows process on the owner's
box (real PE loader / ntdll / ucrtbase — not wine). Classify by **output
marker, not rc** (WER swallows crashes).

If T1 lands first this becomes moot for that test — but **the rule
stands**: a `wine64_skip` requires real-Windows evidence. Audit the other
`.wine64_skip` fixtures against it.

### T3 — F5: narrow the MSVC oracle precedence

You recorded "platform-aware GCC/Clang/MSVC oracle precedence" in the
plan and KG. MSVC as a reference for **what Windows C semantics are** is
fine. MSVC as an **ABI** oracle contradicts a settled non-goal of this
lane — "MSVC-ABI interop (madc.exe is a mingw-world binary; C++ interop
with MSVC-built objects is explicitly out)". Read what was recorded; if
it reaches past semantics, narrow it and say so in both surfaces.

### T4 — finish the review surface Claude did not read

`include/datadef.h` (+106), `src/lexer.cpp` (+146),
`src/cir_builder.cpp` (+72), and `src/embedded_headers.cpp` (+421 —
**confirm it is regenerated output** from the ns-header change, not
hand-edited; `scripts/gen_embedded_headers.sh` is the generator). Report
anything that is a shim rather than a fix at the deepest layer.

### T5 — `/dupaudit`, scoped

`branching.md` requires it before a feature branch merges. Scope it to
the subsystems this branch touched: the **layout/packing** paths and the
**macro-expansion** paths. Findings that report a divergent family are
live bugs, not debt.

### Part One gates

```bash
bash scripts/remote_build.sh sync build fulltest libcxxjit exe obj
ssh -p 2299 dev@localhost 'make -C /workspace/madc/src hosted-x86-64-windows'
ssh -p 2299 dev@localhost 'cd /workspace/madc; WINEDEBUG=-all \
  MADC_BIN=bin/madc-hosted-x86-64-windows.exe MADC_WRAPPER=wine \
  MADC_SKIP_EXT="win64 wine64" bash scripts/run_tests.sh'
```
No count may regress. **A win64 model or header change requires the FULL
wine suite, never a subset** (§5 trap 2).

---

## 3. PART TWO — POSIX P1 + P2 (only after Part One merges)

Full architecture: `docs/plans/2026-08-13-posix-target-surface.md`. Read
§0 (scope law), §2 (provenance law), §4 (the fd registry), §5
(packaging) before starting. Do **not** start P3/P4 (fd registry,
sockets) on this pass.

### P1 — the leaves (no state, no registry)

`strndup`; `setenv`/`unsetenv` over UCRT's `_putenv_s`; `sleep`/`usleep`
over `Sleep`; the `timeradd`/`timersub`/`timerclear` macros (pure macros
over a `struct timeval` mingw already ships); `dirent.d_type` (mingw's
`struct dirent` genuinely lacks it — verified — so map `FindFirstFile`'s
`dwFileAttributes` to `DT_DIR`/`DT_REG`/`DT_LNK`; **serve the delta, do
not shadow mingw's whole `dirent.h`**).

Clears 5 skips: `teststrextra`, `testgetenv_realhdr`, `testlibc`,
`testtimermacros`, `testdirent`.

### P2 — `<dlfcn.h>`

The backend already exists — `src/madc_dl.cpp` has the full Win32 arm
(`LoadLibrary`/`GetProcAddress`, the `sym_default` walk). P2 is a header
plus a strict-C11 shim forwarding to it. Clears
`testclassstaticitanium`. **`testdlcall`/`testdlopen` are NOT in scope**
— they hardcode `libc.so.6`, an ELF soname; those are test-asset
problems (same class as T1).

### Packaging directives (both slices)

- Shims are **new members of `libmadc_rt`**, strict C11, no C++ runtime
  (`scripts/ledger_sources.txt` is the membership owner).
- **One object per feature area** (`rt_posix_str.o`, `rt_posix_time.o`,
  `rt_posix_dir.o`, `rt_posix_dl.o`) so archive-member selection gives
  real granularity; compile `-ffunction-sections -fdata-sections`.
- **Headers under `include/madc/posix/`, served only where the native
  toolchain lacks them** — reuse `embedded_header_outranked()` /
  `is_embedded_header_allowed()` in `lexer.cpp`; add no parallel path.
- **Gate (required):** extend the `emitc_sret_gate` pattern — a specimen
  using the P1/P2 names must link against `libmadc_rt.a` **alone** and
  run, and `objdump -p` must show **no madc DLL** in its imports.
- Nothing here may be observable on a POSIX host. If it is, it is in the
  wrong layer.

---

## 4. PART THREE — optional, small: `windows.h` parses

Measured this session, so you do not need to re-derive it:

- **`windows.h` is NOT in the forest closure.** The 19 pack entry points
  pull 180 units on mingw (151 staged libstdc++, 41
  `/usr/share/mingw-w64/include`, 5 gcc) — zero hits for
  `windows.h`/`winnt.h`/`winsock2.h`/`windef.h`/`winbase.h`. The closure
  stops at the CRT layer (`_mingw.h`, `corecrt*.h`, `crtdefs.h`, …),
  exactly as Linux's stops at glibc.
- **madc cannot parse `windows.h` today.** It reaches
  `psdk_inc/intrin-impl.h:667` and dies: `use of undeclared identifier
  '__builtin_ia32_sfence'`. The mingw-gcc oracle compiles and runs the
  same source (`winh-ok 1`).
- **The gap is exactly two intrinsics**: `__builtin_ia32_sfence` and
  `__builtin_ia32_xgetbv` are the only `__builtin_ia32_*` names in that
  whole closure. madc currently knows **zero** of that family.

If you take this: add those two (sfence = a store fence; xgetbv reads an
extended control register), then re-probe. Reducer in
`tmp/win/probe/winh.mad`. **Parsing is not the same as the pack list's
bar**, which demands compiling *quietly* — exit 0, zero diagnostics.
Forest membership stays a separate, later, size-traded question and is
**not** part of this task.

---

## 5. Traps — each one has already cost time

1. **Negative-control your searches before believing a null.** A search
   that *cannot* find X is not evidence X is absent. Claude made three
   such claims this session, two of them committed: read
   `forest_pack_headers.txt` and declared the forest had no OS headers
   (it has 241 units of closure, full of glibc); used `git log -S"win_run"`
   to "prove" a script had not been run (`-S` finds commits that CHANGE a
   string — running leaves no diff trace). `dupaudit.md`: *"a bad marker
   is worse than no marker — it reports a smaller family with
   confidence."* Ask: **if it existed, would this command have shown it?**
   Prefer inspecting the artifact (`--dump-forest`, `ar t`, `nm`,
   `objdump -p`) over inferring from the recipe.
2. **A win64 model/header change needs the FULL wine suite.** A 17-test
   subset was green while 115 tests were broken — nothing in the subset
   reached the embedded `<stddef.h>` `__need` arm.
3. `WINEDEBUG=-all` is mandatory: wine writes chatter to stderr and
   `.expect_quiet` asserts an empty stderr.
4. `tail | echo $?` reports **tail's** rc. Use `${PIPESTATUS[0]}`, or read
   the printed summary lines.
5. `undefined MIR import` lines beside a MIR error can be false
   positives — `cir_dump_undefined_imports` (`src/madc_cir.cpp:217`)
   consults only madc's resolver, not MIR's load-external registry.
6. win64 stdout is CRLF and that is gcc-parity-correct; `.expect` matching
   is per-line substring so it tolerates `\r`. Never add a normalization
   layer.
7. Scratch files go in `tmp/` (gitignored), never `tests/` or the root.
8. **APPEND to a KG narrative property, never replace it.**
   `Feature{windows-release-lane}.progress` was overwritten on the last
   pass and ~2900 chars of accumulated W0→round-2 history were lost from
   the node (recovered only as a pointer to the plan-doc mirror — see
   review finding F6). Always
   `SET f.progress = f.progress + ' | <new>'`, and **read the size back
   before and after** to prove it grew. `status`/`next`-style properties
   are the exception: those are meant to be replaced.

---

## 6. Definition of done

- [ ] T1–T5 complete; the three `exec://` tests pass on all lanes with no
      domain skip.
- [ ] Four Linux lanes green with no count regression; full wine suite
      re-run at final content.
- [ ] `/dupaudit` run and its findings dispositioned.
- [ ] Part One merged (Claude or you, per owner direction) — note that a
      feature merge to `develop` ships **with** a release, by standing
      owner rule.
- [ ] If Part Two ran: P1/P2 shims in `libmadc_rt` with the
      links-against-the-archive-alone gate, negative-controlled, and the
      cleared skips removed.
- [ ] Hand-off note: branch, final commit, the four Linux numbers, the
      wine numbers, and the exact remaining failure/skip list.

Report the numbers. Do not summarize them as "green".
