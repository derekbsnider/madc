# CODEX HANDOFF — win64 46b burndown (task #42 / #46b)

**Author:** Claude (session #88, 2026-08-13). **Executor:** Codex.
**Read `AGENTS.md` first** — it is your canonical briefing and it applies in
full. This document does not repeat it; it tells you exactly what to do.

These are **imperative directives, not suggestions**. Where a decision is
already made it is in **SETTLED** below — do not re-litigate it, do not
"improve" it, do not widen scope. If a directive here contradicts something
you infer from the code, **stop and report**, do not choose.

---

## 0. Start state (verify before you touch anything)

```
branch : feature/windows-release-lane-claude
HEAD   : 1c540b4a  "embedded headers: size_t/ptrdiff_t/intmax spell through __*_TYPE__ (46b)"
remote : origin = git@github.com:derekbsnider/madc.git  (pushed, in sync)
tree   : clean EXCEPT owner-untracked test.mad + testsort.mad
```

Verify with `git -C /workspace/madc log --oneline -1` and `git status --short`.
If HEAD differs, **stop and ask** — someone moved the branch.

**Your branch:** cut `feature/win64-46b-burndown-codex` from that commit and
work there. One agent per branch (`.claude/rules/branching.md`); Claude owns
`feature/windows-release-lane-claude` and will merge your branch back.

```
git -C /workspace/madc checkout -b feature/win64-46b-burndown-codex 1c540b4a
```

---

## 1. SETTLED — do not re-open

1. **win64 is the PLATFORM type model (LLP64), not a POSIX emulation.** Owner
   decision, 2026-08-13: script `long` = **4 bytes** on win64 in *all*
   dialects, `wchar_t` = **2 bytes**, `*l` builtins are 32-bit-long. madc.exe
   is a Win64 C/C++ compiler; "madc under WSL already exists for full UN*X
   flavour". **A test whose output changed because `sizeof(long)` is now 4 is
   not a regression — it is the decision landing.**
2. **The one width owner is `madc_target_data_model`** (`include/datadef.h`)
   plus the accessors `dd_platform_long()` / `dd_platform_ulong()` /
   `dd_platform_wchar()` (defined `src/parser.cpp`, beside the `dd*` globals).
   **Never test `_WIN32` at a consumer site.** Ask the owner.
3. **gcc/clang are canon; for the win64 target the canon is
   `x86_64-w64-mingw32-gcc-posix` / `-g++-posix`.** Every expected-output
   decision in Work Package B comes from running that compiler. Never from
   reasoning, and never from what madc happens to print.
4. **The QNAP NAS never builds or tests.** All compiles and suites run on the
   container: `ssh -p 2299 dev@localhost`, **absolute paths in the remote
   command**. Stage the tree first with `bash scripts/remote_build.sh sync`
   (run from `/workspace/madc`). This is not negotiable.
5. **Never `git add -A`.** `test.mad` and `testsort.mad` in the repo root are
   the owner's untracked scratch files. Stage explicit paths only.
6. **Never `git checkout` over uncommitted work.** Commit first, always.
7. The `.win64_expect` / `.win64_skip` fixture conventions already exist and
   are documented (`.claude/rules/test-fixtures.md`,
   `docs/rules/test-fixtures.md`). Use them. Do not add runner logic.

---

## 2. Where the lane stands

Full wine suite at `1c540b4a` (this is your baseline — reproduce it before
changing anything):

```
cd /workspace/madc   # ON THE CONTAINER
WINEDEBUG=-all MADC_BIN=bin/madc-hosted-x86-64-windows.exe MADC_WRAPPER=wine \
  MADC_SKIP_EXT="win64 wine64" bash scripts/run_tests.sh
# => 947 passed, 30 failed, 0 timed out, 59 skipped
```

Linux gates at the same commit, all green — **your changes must keep them
green**:

| gate | command | result at 1c540b4a |
|---|---|---|
| fulltest | `bash scripts/remote_build.sh fulltest` | rc=0, 1027 passed / 0 failed |
| libc++ JIT lane | `bash scripts/remote_build.sh libcxxjit` | 1023 passed / 0 failed / 13 skipped |

The 30 wine failures decompose as follows. **This classification is evidence,
not guesswork** — every "actual" below was captured by running the test
standalone under wine at `1c540b4a`.

### 2a. Wine flakes — 3 (do nothing)

`test3eq`, `testctorargs`, `teststrarrinit` — expected and actual output are
**byte-identical**; each passes standalone. Wine environment wobble; a full
wine run shows 2–4 different ones each time. **Verify any "new" wine failure
standalone before believing it.** Real Windows does not have this property.

### 2b. Work Package B candidates — 14 (LP64-encoded `.expect` files)

| test | `.expect` (LP64) | madc on win64 |
|---|---|---|
| test_mi_layout | `11 22 33 24 9 4 24` | `11 22 33 12 9 4 16` |
| testanonunionalias | `8` | `4` |
| testbitfieldunit | `A 4 B 4 C 8 D 8` | `A 4 B 4 C 8 D 4` |
| testclasspatternvector | `45 11 80` / `... 48 8 0 8 16` | `45 11 64` / `... 32 8 0 4 8` |
| testdeclonlyspec | `4 1 8` | `4 1 4` |
| testdecltyperet | `5 6 7 8 8` | `5 6 7 4 4` |
| testemptybaseoffset | `24 0 0` | `12 0 0` |
| testenumsize | `1 8 4 2 2 16 ok` | `1 4 4 2 2 16 ok` |
| testmemberaliasshadow | `8 1` | `4 1` |
| testmembertmplrefret | `4 4 8 4 4 1` | `4 4 4 4 4 1` |
| testpragmapack | `p4=12 natural=16` | `p4=8 natural=8` |
| teststructvolatilemember | `1 1` | `0 0` |
| testuint32realcoerce | first line `4294967296` | first line `0` |
| testunsignedfold | `64 0` | `32 0` |

Every one is a `sizeof(long) 8 → 4` consequence, and in every case madc's new
answer *looks* like the correct win64 answer. **"Looks correct" is not a
verdict.** Work Package B is: get the verdict from the oracle.

### 2c. Real defects — 4 (Work Packages A and C)

| test | symptom | package |
|---|---|---|
| testphp | MIR: `import of undefined item _ZN3php13number_formatE...El...` | **A** |
| testmadcevalscope | new stderr warning `incompatible pointer types of argument and parameter` at `tests/testmadcevalscope.mad:29:23` | **C1** |
| testspaceship_realhdr | parse error `use of undeclared identifier 'strong_ordering'` | **C2** |
| testgcclimitmacros | prints `0 0`, expects `1 1` | **C3** |

### 2d. Pre-existing round-2 singles — 9 (NOT yours unless time remains)

`testexcept`, `testfdsetfromsystime`, `testgccconversionprefix`, `testlang`,
`testnsmadcautostring`, `teststrcmpret`, `testsysobject`, `testtemplate`,
`testtranssecondary`. These predate this window (see the ROUND 2 entry in
`docs/plans/2026-08-12-windows-release-lane.md`). Leave them alone until A, B
and C are done and pushed.

---

## 3. WORK PACKAGE A — intrinsic namespace headers must not spell `long`

**Do this one first. It is a real, silent, cross-platform-class defect.**

### The defect

madc's script-facing intrinsic namespace headers live in `include/madc/` as
**extension-less** files (`ns_php`, `ns_perl`, `ns_ruby`, `ns_rust`,
`ns_python`, `ns_js`, `ns_madc`). They declare the namespace publics that
scripts call, and those declarations are resolved **mangled-direct** against
the real `namespace php { … }` implementations compiled into the host
(`.claude/rules/cpp-first-api.md`).

They spell 64-bit parameters as **`long`**. The host implementations
(`src/ns_php.cpp` and the C++-side twins `include/madc/ns_php.h` …) spell them
as **`int64_t`**. On LP64 both mangle to Itanium `l`, so the mismatch is
invisible. On win64 `long` is now 4 bytes and mangles `l` while `int64_t`
mangles `x` — the script imports a symbol the host does not export:

```
import of undefined item _ZN3php13number_formatERNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE l S6_
host actually exports:    _ZN3php13number_formatERNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE x S6_
                                                                                                      ^
```

This is **the same root** that round 1 fixed for the eval/context/channel API
(`<ns_madc>` + `ns_madc.cpp` + `channel.h`, commit `6bc2d79d`) — that fix
covered `ns_madc` only, and `ns_madc` is already clean (0 hits). The other six
were missed.

### Counts (`grep -c '\blong\b'`, at `1c540b4a`)

```
include/madc/ns_php     30
include/madc/ns_python  22
include/madc/ns_perl    14
include/madc/ns_rust    14
include/madc/ns_ruby     6
include/madc/ns_js       2
include/madc/ns_madc     0   <- already correct, use it as the model
```

### Directives

1. In each of the six files, replace every **standalone `long`** with
   `int64_t` — both the `extern "C"` block declarations and the
   `namespace X { }` publics. `int64_t` is a native madc type (no include
   needed); `include/madc/ns_php.h` (the host-side twin) already uses exactly
   this spelling — **make the script header agree with its twin, per file**.
2. **Do not touch `long long`, `unsigned long long`, or `long double`** if any
   appear. Only the bare `long` / `unsigned long` spellings.
3. **Verify against the host, do not assume.** For each changed function, the
   host's exported symbol is the truth:
   ```
   ssh -p 2299 dev@localhost 'cd /workspace/madc; x86_64-w64-mingw32-nm \
     bin/madc-hosted-x86-64-windows.exe | grep _ZN3php'
   ```
   The script header's spelling must produce that symbol. If a function's host
   side genuinely takes a 4-byte type, change the *header to match the host*,
   not the other way round.
4. **Ship a gate.** Add `scripts/check-ns-header-widths.sh`, wired into
   `fulltest` (`src/Makefile` lines 828 and 832 show exactly how
   `check-one-dl-owner.sh` and `check-i64-spec-spelling.sh` are invoked —
   copy that shape). It must fail when a bare `long`
   appears in any `include/madc/ns_*` script header, with a message saying
   why (mangling differs from the host's `int64_t` on LLP64). **Negative-control
   it**: plant a `long`, confirm the gate goes red, remove it, confirm green.
   A rule without a gate decays — this is repo law.
5. Verify: `testphp` green under wine, and re-run every namespace test
   (`testperl`, `testpython`, `testruby`, `testrust`, `testjs`, `testphp`,
   `testmadc_ns`) under both the wine lane and Linux `fulltest`.

**One commit** for Package A. Rule trailers are **mandatory** (it touches
`include/`); `scripts/check-rule-trailers.sh` in `fulltest` will fail the
build without them. See `.claude/rules/rule-trailers.md` — `Layer:` must state
the chain and why yours is the deepest.

---

## 4. WORK PACKAGE B — oracle the 14, then mint fixtures

### The rule, in one line

> **Run the mingw oracle. If madc's output equals the oracle's, mint
> `tests/<base>.win64_expect` with the ORACLE's text. If it differs, you have
> found a real bug — stop, diagnose, and fix it at the deepest layer.**

You are never allowed to write a `.win64_expect` from madc's own output. The
fixture's content comes from the oracle compiler; that is the entire point of
the convention (`docs/rules/test-fixtures.md`).

### Oracle recipe (verified working; workspace `/workspace/madc/tmp/win/`)

On the container. Copy the `.mad` to a `.c` / `.cpp`, strip any `#!` shebang
line, and compile with the madc win posture:

```bash
ssh -p 2299 dev@localhost 'cd /workspace/madc/tmp/win
 sed "s|#!/usr/bin/env madc||;s|#!../bin/madc||" /workspace/madc/tests/testenumsize.mad > o.cpp
 x86_64-w64-mingw32-g++-posix -O0 -static -D_UCRT -D__USE_MINGW_ANSI_STDIO=1 \
   -specs=/workspace/madc/obj/hosted-x86-64-windows/ucrt.specs o.cpp -o o.exe
 WINEDEBUG=-all wine o.exe'
```

Notes that cost time if you rediscover them:
- Use `-g++-posix` for anything with C++ syntax, `-gcc-posix` for plain C.
- `-static` avoids libstdc++/winpthread DLL resolution noise for the oracle.
- Some tests use madc-only surface (`php::`, `array`, `printstr`, `putu`).
  **Those cannot be oracle'd by g++ directly.** For them, reduce to the C/C++
  core of what the test measures (usually a `sizeof`/layout `printf`) and
  oracle *that*; state the reduction in the commit message.
- **Classify by exit status and output marker, never by rc alone** — WER
  swallows crashes on Windows, and wine writes diagnostics to stderr.

### Per-test directives

- For all 14 in §2b, produce the oracle output, then:
  - **match** → `tests/<base>.win64_expect` containing the oracle's lines.
  - **mismatch** → real bug. Report it to the owner with the reducer and both
    outputs before fixing; fix in its own commit with a reducer in `tests/`.
- `teststructvolatilemember` deserves a sanity read: the union is
  `{ double d; unsigned long u[2]; }` — with 4-byte longs `u[]` covers only
  the low half of the double, so `0 0` is very likely correct. **Confirm it
  with the oracle anyway.**
- `testuint32realcoerce` casts `(float)~0U` to `unsigned long`; with a 4-byte
  target that is an out-of-range conversion. Get the oracle's answer; if the
  oracle and madc disagree here, that is a genuine conversion bug and it
  outranks the fixture work.

Commit the fixtures as **one commit** (fixtures only — no `src/` changes, so
`Oracle:` trailer text should name the compiler and the reduction). If any
real bug falls out, that gets its **own separate commit** with a reducer —
never folded in (`.claude/rules/fix-what-you-find.md`).

---

## 5. WORK PACKAGE C — diagnose 3

Do these **after** A and B, and **do not guess**: form a hypothesis by reading,
then confirm with a reducer in `tmp/win/`. Report before large changes.

**C1 — `testmadcevalscope` stderr warning.** New this window: `incompatible
pointer types of argument and parameter` at `tests/testmadcevalscope.mad:29:23`.
The program's stdout is entirely correct — **the warning itself is the
failure**: `tests/testmadcevalscope.expect_quiet` exists, so an EMPTY stderr
is part of this test's contract (diagnostic hygiene). Do not silence the
warning; find why the types now mismatch. Look at line 29's call against the
eval-API signatures in `include/madc/ns_madc` vs `src/ns_madc.cpp` — round 1
respelled that API to `int64_t` (commit `6bc2d79d`), so a pointer-typed
parameter may have been missed, or a `char*` / `const char*` mismatch is now
surfacing through a width-changed neighbour. A genuine incompatibility that
happens to work is still a bug: fix the declaration, do not downgrade the
diagnostic.

**C2 — `testspaceship_realhdr`: `use of undeclared identifier 'strong_ordering'`.**
The real `<compare>` header is being parsed but `std::strong_ordering` never
gets declared, so something earlier in that header's chain is being dropped
silently on the win64 lane. This is a header-chain parse casualty like the
`#if !defined __NO_ISOCEXT /* comment */` root fixed in session #86 (commit
`effd654d`) — **look for a similar silent-false or silent-skip**, not for a
missing feature. Reduce to the smallest `#include <compare>` program that
loses the declaration.

**C3 — `testgcclimitmacros` prints `0 0`, expects `1 1`.** The test is:
```c
long ptrdiff_max = __PTRDIFF_MAX__;
unsigned long size_max = __SIZE_MAX__;
printf("%d\n", ptrdiff_max > 0);
printf("%d\n", size_max > (unsigned long)ptrdiff_max);
```
Two candidate roots, and **you must determine which before editing**:
(a) madc's predefine **seeds** in `src/lexer.cpp` (~line 1325) hardcode LP64
values with LP64 suffixes — `__LONG_MAX__ = 9223372036854775807L`,
`__PTRDIFF_MAX__`, `__SIZE_MAX__`, `__INTMAX_MAX__`, `__UINTMAX_MAX__`. On
win64 the `L` suffix means 4 bytes. The per-MODE baked capture
(`src/predefined_macros.cpp`, generated from the target toolchain's `-dM`)
normally overrides these — **but it could not override `__LP64__` because
mingw never defines it**, and the same blind spot may apply to any macro
mingw spells differently. Dump madc's effective values and compare with
`x86_64-w64-mingw32-gcc-posix -dM -E - </dev/null | grep -E 'PTRDIFF_MAX|SIZE_MAX|LONG_MAX'`.
(b) The test itself is LP64-shaped (a 4-byte `long` cannot hold
`__PTRDIFF_MAX__` on any correct win64 toolchain), in which case mingw-gcc
prints `0 0` too and this is a Package-B fixture, not a bug.
**Run the oracle first.** It decides between (a) and (b) in one command.

---

## 6. Gates, commits, push

**Per batch of work (each package), in this order:**

```bash
# from /workspace/madc on the NAS
bash scripts/remote_build.sh sync build fulltest libcxxjit
```
Both must be rc=0 with no count regression (fulltest ≥1027 passed / 0 failed,
libcxxjit 1023/0/13 — the numbers move up only when you add tests).

Then the wine lane at the same content:
```bash
ssh -p 2299 dev@localhost 'make -C /workspace/madc/src hosted-x86-64-windows'
ssh -p 2299 dev@localhost 'cd /workspace/madc; WINEDEBUG=-all \
  MADC_BIN=bin/madc-hosted-x86-64-windows.exe MADC_WRAPPER=wine \
  MADC_SKIP_EXT="win64 wine64" bash scripts/run_tests.sh'
```

**A win64-model change requires the FULL wine suite, never a subset.** Claude
learned this the expensive way in this window: a 17-test subset was green
while 115 tests were broken, because no test in the subset reached the
embedded `<stddef.h>` `__need` arm.

**Commit discipline:**
- Every commit touching `src/` or `include/` carries the four trailers
  (`Hypothesis:` / `Layer:` / `Searched:` / `Oracle:`) — gated in `fulltest`.
- End every commit message with:
  ```
  Co-Authored-By: Codex <noreply@openai.com>
  ```
  (match whatever attribution trailer your harness is configured to use;
  keep it consistent across the batch).
- Small self-contained commits, one concern each.
- Push only to `origin` (`git@github.com:derekbsnider/madc.git`) — **verify the
  URL before every push**; this workspace has multiple remotes and `origin`
  is not always first in the list.

---

## 7. Traps that have already cost time in this lane

1. **Wine writes its own chatter to stderr.** `.expect_quiet` tests fail on
   wine's noise unless `WINEDEBUG=-all` is set. It is part of the canonical
   invocation for a reason.
2. **`tail | echo $?` reports tail's rc.** Use `${PIPESTATUS[0]}`, or read the
   printed summary lines — they are the verdict.
3. **A background local suite run gets killed at the tool timeout and orphans
   the remote run.** Use `remote_build.sh` (it logs to `tmp/logs/`), or
   `nohup` the remote command writing to a log plus a `.done` marker, and wait
   on the marker. Never poll with `while pgrep -f <pattern>` — the pattern
   matches the waiter's own command line.
4. **`undefined MIR import` lines printed beside a MIR error can be false
   positives.** `cir_dump_undefined_imports` (`src/madc_cir.cpp:217`) consults
   only madc's resolver, not MIR's `MIR_load_external` registry. Read the
   first error, not the trailing list. (In `testphp`'s case the imports listed
   *are* real — confirmed against `nm` — but do not assume it in general.)
5. **win64 stdout is CRLF** and that is gcc-parity-correct. `.expect` matching
   is per-line substring, so it tolerates the `\r`. Never add a normalization
   layer, and never make madc.exe emit LF.
6. **Scratch files go in `tmp/`** (gitignored), never in `tests/` or the repo
   root. Promoted reducers get renamed and moved with proper fixtures.

---

## 8. Definition of done

- [ ] Package A: six ns headers respelled, gate added + negative-controlled and
      wired into `fulltest`, `testphp` and all namespace tests green on both
      lanes.
- [ ] Package B: all 14 oracled; each is either a `.win64_expect` carrying the
      oracle's text, or a reported/fixed real bug with its own reducer.
- [ ] Package C: three diagnoses reported; fixes landed where the root is in
      madc, fixtures where the oracle agrees with madc.
- [ ] `fulltest` rc=0 and `libcxxjit` 0 failed at final content.
- [ ] Full wine suite re-run at final content; the remaining failures are the
      9 round-2 singles (§2d) plus at most a handful of verified flakes.
- [ ] Branch pushed to `origin`, and a hand-off note stating: branch, final
      commit, the four numbers (fulltest / libcxxjit / wine passed / wine
      failed), and the exact remaining failure list.

Report the numbers. Do not summarize them as "green".
