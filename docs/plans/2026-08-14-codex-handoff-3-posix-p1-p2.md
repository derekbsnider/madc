# Codex handoff #3 — POSIX target surface, P1 + P2

Author: Claude, session #88 close (2026-08-14).
Plan of record: `docs/plans/2026-08-13-posix-target-surface.md`.
Task: **#48**. Predecessor: handoff #2, executed and released as **v0.79.0**.

Branch: **create `feature/posix-p1p2-codex` off `develop`** (which is at
`c5c79248`, pushed, v0.79.0 released). Do not continue on the win64
burndown branch; it is merged and closed.

---

## 0. SETTLED — do not re-open these

1. **Win64 is the only target this plan touches.** Linux and macOS are
   already POSIX; `O_NONBLOCK`, `fcntl`, `dlfcn`, sockets all work there
   through the platform's own headers. A change that alters behaviour on
   a POSIX host is out of scope and is a bug in your change.
2. **PROVENANCE LAW.** Clean-room from POSIX.1/SUS and MSDN only. **Never
   read, quote, or adapt Cygwin, newlib, or msys2 source.** They are GPL;
   madc is MPL-2.0. mingw-w64's *headers* are public domain and are fine
   to read as an interface fact — that is how you learn what the platform
   already declares, and §8's "take the interface fact from the platform
   header" depends on it.
3. **No runtime DLL.** A madc-compiled Windows program must never
   *require* a madc `.dll`. The vehicle is `libmadc_rt.a`, one object per
   feature area, membership owned by `scripts/ledger_sources.txt`
   (strict C11, no builtins, no C++ runtime).
4. **Oracles.** Codegen / ABI / layout / widths → `x86_64-w64-mingw32-gcc`,
   always. POSIX *semantics* → glibc on Linux. MSVC is evidence only for
   Windows API/UCRT semantics with no ABI question in view; it never
   overrides mingw. This was narrowed in `1f39caad` — keep it narrow.
5. **The layer is ON by default**, with `--no-posix-compat` to opt out.

---

## 1. What I verified, and how it changes P1

The plan's §9 calls P1 "leaves ... no state, no registry". **That is
right for three items and wrong for two.** I checked what mingw actually
ships rather than trusting the skip reasons, on the container (the NAS
has no mingw toolchain — a null result there proves nothing):

```
R=/usr/share/mingw-w64/include
grep -c strndup           $R/string.h      -> 0
grep -cE '\bsetenv\b'     $R/stdlib.h      -> 0
grep -cE 'timeradd|timersub'   $R/sys/time.h -> 0   # and not in its closure either
    # ^ CORRECTED: my original marker here also named timerisset, and read 0.
    #   That was WRONG — timerisset/timercmp/timerclear ARE provided, from
    #   _timeval.h:16-20, which sys/time.h INCLUDES. Grepping one file cannot
    #   answer a question about a header's include closure. The delta is two
    #   macros (timeradd, timersub), not five.
grep -c d_type            $R/dirent.h      -> 0
grep -c O_NONBLOCK        $R/fcntl.h       -> 0
grep -nE '\bsleep\b'      $R/unistd.h      -> line 46: unsigned int __cdecl sleep(unsigned int);
ls $R/dlfcn.h $R/sys/socket.h              -> ABSENT, ABSENT
```

So:

| item | mingw state | what it actually needs |
|---|---|---|
| `strndup` | `string.h` **ships**, symbol absent | declaration **delta** + shim |
| `setenv`/`unsetenv` | `stdlib.h` **ships**, absent | declaration **delta** + shim over `_putenv_s` |
| `timeradd`/`timersub` | `sys/time.h` **ships**, both absent | macro **delta**, header-only |
| `dirent.d_type` | `dirent.h` **ships**, no such member | **NOT a leaf** — see D2 |
| `sleep`/`usleep` | `unistd.h` **ships and declares `sleep`** | **not a header problem at all** — see D3 |
| `dlfcn.h` | **ABSENT entirely** | whole header + shim (P2) |

**Every P1 header exists on mingw.** Not one of them can be solved by
serving a whole madc header, because §8 forbids shadowing a header the
platform ships — and rightly: shadow `string.h` and you lose every
mingw declaration in it. The dominant P1 mechanism is therefore
**augmenting a real header**, which does not exist yet. That is D1.

---

## 2. Decisions I made so you don't have to stop and ask

### D1 — the supplement mechanism (the core of P1)

**Serve `include/madc/posix/<name>` as a SUPPLEMENT that is tokenized
immediately after the real `<name>` is served, not instead of it.**

- Keyed by filename convention, so adding `posix/sys/time.h` is the
  entire registration step — no table, no per-header branch (design
  rule #7).
- `scripts/gen_embedded_headers.sh` already keys nested paths by path
  relative to `include/madc/` (its own comment cites
  `include/madc/sys/wait.h` → `"sys/wait.h"`), so `posix/string.h` is a
  valid key today with no generator change. **Verify that before
  relying on it.**
- Gate it on target + policy: supplements are consulted only when the
  target is win64 and `--no-posix-compat` was not passed.
- The hook belongs beside the existing include machinery in
  `src/lexer.cpp` (`find_embedded_header` /
  `is_embedded_header_allowed` at 841, 3221, 3593, 4409; the ranking
  twin `Program::embedded_header_outranked` is `src/parser.cpp:23120`).
  **Extend that path. Do not add a second include path** — that is
  precisely the duplication family the last branch spent its dupaudit
  eliminating.
- A supplement must be **additive only**: declarations and macros the
  platform header omits. It must never redefine a type the platform
  header already defines.

### D2 — `dirent.d_type` is not a leaf; scope it honestly

`d_type` is a **struct member**, and mingw's `readdir` fills mingw's
`struct dirent`. You cannot add a member from a supplement, and a
supplement that redefines the struct violates D1's additive rule.

Serving `d_type` therefore means madc **owns `dirent.h` and its
implementation** — `opendir`/`readdir`/`closedir`/`rewinddir` over
`FindFirstFileW`/`FindNextFileW`, where `dwFileAttributes` is the
natural source of `DT_DIR`/`DT_REG`/`DT_LNK` anyway. That is a real
subsystem, not a leaf.

**Split it out as its own slice and do it last in this handoff (T6).**
If it does not fit the session, ship P1's other four plus P2 and leave
`testdirent` skipped with its reason updated to say *why* it is bigger
than it looks. Do **not** half-serve it by shadowing the header with a
struct that mingw's CRT does not fill — that is a silent-wrong-answer
bug, the class that outranks everything else.

### D3 — `testlibc` is a symbol-resolution problem, not a header one

mingw's `unistd.h` declares `sleep` (line 46). `testlibc` includes
nothing at all — it tests the **bare-POSIX-name dlsym fallback**. The
fix is that `madcdl_sym_default` (`src/madc_dl.cpp:37`, which walks
self-exe → recorded modules and consults `mir_mingw_ansi_stdio_lookup`
first at line 173) must find a lowercase `sleep`.

**Trap:** `-Wl,--export-all-symbols` is on (`src/Makefile:315`), but an
**archive member is only linked in if something references it**. A
`sleep` shim that lives only in `libmadc_rt.a` will not be present in
`madc.exe` at all, so it cannot be exported, so the walk cannot find it.
Verify with `nm`/`objdump -p` on the built `.exe` — **inspect the
artifact, not the Makefile.** If it is absent, the fix is in how the rt
objects reach the binary, not in the walk.

### D4 — the archive-only gate lands in P1, not later

The plan defers the "links against `libmadc_rt.a` alone, no madc DLL in
`objdump -p`" gate to P4-ish. **Move it forward.** It is the gate that
proves the owner's static-embedding requirement, and it is far cheaper
to keep true from the first shim than to retrofit after four slices.

Model it on `scripts/emitc_sret_gate.sh` (wired at `src/Makefile:859`),
which already links a specimen against the archive alone. The win64
archive already exists and currently holds exactly `rt_except.o` and
`rt_vla.o` (`ar t lib/libmadc_rt-hosted-x86-64-windows.a`).

---

## 3. Tasks

**T1 — the supplement mechanism (D1).** Target- and policy-gated,
extending the existing include path. Ship it with a negative control:
a supplement that would fire on Linux must **not** fire, and the gate
must fail if it does.

**T2 — `strndup`** (`teststrextra`). `posix/string.h` supplement +
`rt_posix_str.o`. Oracle: glibc semantics; mingw-gcc must reject the
same source today (it does — that is why the skip exists).

**T3 — `setenv`/`unsetenv`** (`testgetenv_realhdr`). Supplement +
`rt_posix_env.o` over `_putenv_s`. Mind the semantics: `setenv(name,
val, overwrite=0)` must not overwrite, and `unsetenv` removes rather
than setting empty — `_putenv_s(name, "")` is the removal spelling on
Windows and the two are easy to conflate.

**T4 — `timeradd`/`timersub`** (`testtimermacros`) — those two only;
`timerisset`/`timercmp`/`timerclear` come from mingw's `_timeval.h`.
Header-only supplement, no archive member. Note `timercmp(&a, &b, <)` takes an
**operator token** as its third argument; write it the way POSIX
specifies or the test's `cmp_lt/cmp_gt/cmp_eq` line will not compile.

**T5 — `<dlfcn.h>`** (P2, `testclassstaticitanium`). Whole header —
mingw ships none — plus `rt_posix_dl.o` forwarding to the existing
backend: `madcdl_open_global`/`_local`/`_self`, `madcdl_sym`,
`madcdl_sym_default`, `madcdl_close`, `madcdl_error` (`src/madc_dl.cpp`
lines 12–68). **The backend is done; this is a header and a forward.**
Map `RTLD_LAZY|RTLD_NOW|RTLD_GLOBAL|RTLD_LOCAL|RTLD_DEFAULT` onto it
honestly, and where a flag has no Windows meaning, say so in a comment
rather than pretending.
`testdlcall`/`testdlopen` stay skipped — they hardcode `libc.so.6`, an
ELF soname. That is test-asset hygiene, not platform work.

**T6 — `dirent` (D2), only if T1–T5 are complete and green.**

**T7 — the archive-only gate (D4)**, wired into `fulltest`.

Each task drops its `.win64_skip` **and** passes. A task that leaves the
skip in place is not done.

---

## 4. Traps

1. **The NAS never builds or tests.** `bash scripts/remote_build.sh sync`
   to stage, then `ssh -p 2299 dev@localhost` with **absolute paths** in
   the remote command. A toolchain query on the NAS returns "absent" for
   things that exist — I hit exactly that writing this document.
2. **Wine lane invocation** (canonical):
   `WINEDEBUG=-all MADC_BIN=bin/madc-hosted-x86-64-windows.exe MADC_WRAPPER=wine MADC_SKIP_EXT="win64 wine64" bash scripts/run_tests.sh`.
   Start a persistent `wineserver -p` first — the rotating failures
   diagnosed last branch were a wineserver connection reset, not madc.
3. **A model or header change needs the FULL wine suite, not a subset.**
   The 46b arc had a 17-test subset green while 115 tests were broken by
   the same change.
4. **Append to KG narrative properties, never replace.** `SET f.x = f.x + '…'`,
   and read `size(f.x)` back before and after. Handoff #2's F6 was ~2900
   characters of history lost to a replace.
5. **Never `git add -A`.** `test.mad` and `testsort.mad` in the repo root
   are the owner's and stay untracked.
6. **Rule trailers** (`Hypothesis:`/`Layer:`/`Searched:`/`Oracle:`) on
   every `src/`/`include/` commit; `scripts/check-rule-trailers.sh` gates
   it. `Searched:` names the *concept*, not the identifier you already
   knew.
7. **Negative-control every gate you add**, and every absence claim you
   make. If a search could not have found the thing, its null result is
   not evidence.
8. **Do not touch the type model.** LLP64 widths are settled and released
   (`dd_platform_long`/`_ulong`/`_wchar`, typeid pins 35/36). A POSIX
   shim that needs a width has a bug in the shim.

---

## 5. Definition of done

- `feature/posix-p1p2-codex` off `develop`, pushed.
- Four Linux lanes: `fulltest`, libc++ JIT, `--exe`, `--obj`.
- Full wine suite under a persistent wineserver, with the skip count
  **down by the number of tests you unskipped** and the reason for every
  remaining skip still accurate.
- The archive-only gate green and in `fulltest`.
- `/dupaudit` scoped to the include/header-serving paths — you are adding
  a serve path, which is exactly where a duplicate is born.
- Mirrors synced: `claude_status.json`, `docs/plans/ROADMAP.md`,
  `CHANGELOG.md`, the POSIX plan doc, and the KG.
- **Report the numbers. Do not summarize them as "green."**
- A feature merge to `develop` ships **with a release** (standing owner
  rule).

## 6. Out of scope

P3 (fd registry, `fcntl`, `flock`), P4 (sockets, the `WSAGetLastError` →
`errno` table), P5, P6. W3 PE/COFF and everything downstream of it. The
25 `*_libcxx` skips (owner decision: the win64 lane has no libc++ stage).
`testbuiltinvalisttypedef` — win64's scalar `va_list` is a *correct*
divergence and that skip is permanent.
