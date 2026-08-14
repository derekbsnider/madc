# The madc POSIX target surface (Win64 ONLY — the one non-POSIX target)

**Owner decision, 2026-08-13:** madc should be **as POSIX compliant as
possible** on every target it compiles for; we do **not** take a Cygwin
runtime dependency; and the relevant parts of the runtime must be
**statically embeddable** rather than always requiring a `.dll` / `.so`.

This document is the architecture for that. It supersedes nothing: the
LLP64 type-model decision (`long` = 4 bytes on win64, plan
`2026-08-12-windows-release-lane.md`) stands unchanged — **type model and
library surface are orthogonal.** madc.exe stays a Win64 LLP64 compiler
that speaks the platform's ABI; what grows is the set of *library* names
it can serve.

## 0. Scope law: this is Win64-only, by construction

**Every other target madc compiles for is already POSIX.** Linux is
POSIX; macOS/darwin is POSIX (certified UNIX, in fact). On those hosts
madc supplies *nothing* — it serves the platform's real headers and binds
the platform's real libc, which is exactly why `tests/testfcntl.mad`
passes there today in all three lanes with zero madc code in the path
(§7). **Windows is the odd one out, and it is the only one.**

Three consequences, all load-bearing:

1. **"Win64 first" would be the wrong framing** — there is no second.
   This layer is not a portable abstraction being rolled out per-target;
   it is a compatibility shim for the single non-POSIX platform in the
   support matrix.
2. **The arc cannot regress Linux or macOS.** L2 headers are served only
   where the native toolchain lacks them (§8), and L3 objects compile
   only into the win64 archive. A POSIX host never activates a line of
   it. Reviewers should hold every slice to that standard: if a change
   in this arc can be observed on a POSIX host, it is in the wrong layer.
3. **A new POSIX target costs nothing.** Any future *nix port inherits
   the platform's own libc surface, as Linux and darwin do now.

The corollary for judging any proposed addition: *"which platform's
absence forces this?"* If the answer is not "Windows", it belongs in L1
(madc's own portable surface), not here.

---

## 1. The line: what we take, what we refuse

Cygwin is two things bolted together, and only one of them is desirable.

**What we take — the library surface.** POSIX function and header
spellings backed by the platform's own API: `fcntl` over
`SetHandleInformation` / `ioctlsocket`, `flock` over `LockFileEx`,
`<sys/socket.h>` over winsock, `<dlfcn.h>` over `LoadLibrary` /
`GetProcAddress`, `strndup`, `setenv`, `timeradd`. Every one of these is
a thin, stateless-or-nearly-stateless translation. **The capability
already exists on Windows in every case; only the spelling is missing.**

**What we refuse — the POSIX personality.** Mount tables and `/`-to-drive
path translation, `fork()` emulation, POSIX signals as a process model,
process groups and job control, `/proc`, uid/gid emulation, and a
`select()` that unifies files with sockets. This is the mass of Cygwin,
it is where the `cygwin1.dll` runtime dependency comes from, and it is
what makes Cygwin programs *Cygwin* programs rather than Windows
programs. **madc emits Windows programs.**

`fork()` is already settled and does not reopen: win64 offers UCRT truth,
`fork` is absent exactly as under mingw-gcc, and the portable process API
is madc's own (`Process`, channels). Nothing here changes that.

### The dividing test

> **Does the name need madc to maintain a parallel OS model to answer it?**
> No → we serve it. Yes → it stays absent, and the capability is reached
> through madc's own portable surface.

The single exception, deliberately taken, is §4.

---

## 2. PROVENANCE LAW (read before writing one line)

Every implementation in this layer is **clean-room, written from the
published specification**: POSIX.1-2024 / SUS, the C standard, and MSDN
for the Win32 side.

- **Never read, consult, quote, or adapt Cygwin, newlib, msys2 runtime,
  or any GPL/LGPL libc source** while working on this layer. Cygwin is
  GPL-3.0-with-exception; madc is MPL-2.0. A "look at how Cygwin does it"
  is a licensing incident, not a shortcut.
- mingw-w64 headers may be **consulted for interface facts only** (what a
  type is, what a macro's value must be for ABI compatibility) — they are
  the platform's own public interface. Do not copy implementation bodies.
- Every file in the layer carries the standard madc MPL header.
- The W0.5 darwin prelude established this discipline for the macOS lane;
  this is the same rule for a different reason.

If a semantic question cannot be answered from the spec, answer it from
**observed glibc behaviour** (write a reducer, run it on Linux) — that is
a behavioural oracle, not a source derivation.

---

## 3. Layers

| layer | what it is | where it lives | exists today |
|---|---|---|---|
| **L0 host seams** | madc's own code calling the OS | `madc_dl.cpp`, `madc_posix_io.cpp`, `madc_process.cpp`, `madc_socket_channel.cpp` | ✅ done (W1) |
| **L1 portable surface** | what portable *scripts* should call | channels, `Process`, `madc::sys`, `madcdl_*` | ✅ mostly; gains non-blocking (§7) |
| **L2 POSIX headers** | POSIX spellings served only where the native toolchain lacks them — **win64 only** (§0) | `include/madc/posix/**` (new) | ⬜ this plan |
| **L3 POSIX shims** | the implementations L2 declares — **win64 only**, compiled into the win64 archive alone | new members of `libmadc_rt` | ⬜ this plan |
| **L4 refused** | fork/signals/mounts/`/proc`/uid | — | never |

**L0 is not L3.** `src/madc_posix_io.cpp` is the host-side owner —
madc's own C++ calling the OS. L3 is a strict-C11 shim that *compiled
user programs* call. They may share technique; they never share code
paths, because L3 must link into an emitted-C program that contains no
madc C++ at all.

---

## 4. The fd registry — the one piece of state, and why it is not Cygwin

POSIX `fcntl(fd, F_SETFL, O_NONBLOCK)` must work whether `fd` names a
file, a pipe, or a socket. On Windows those are three different calls
(`SetNamedPipeHandleState`, `ioctlsocket`, and for files: not a thing —
overlapped I/O is set at open time). A `SOCKET` is not a CRT fd; madc
already knows this from W1 slice 12, where the int-fd model holds only
because kernel handle values fit in 32 bits.

So the layer needs to answer "what kind of object is this int?".

**Decision: a narrow fd registry, not an fhandler hierarchy.**
`madc_fd` maps an int to `{ kind, HANDLE }` where kind ∈ `{ file, pipe,
char, socket }`, plus the per-fd POSIX flag bits POSIX requires us to
remember (`O_NONBLOCK`, `FD_CLOEXEC`) because Windows has no way to read
them back. That is the *whole* structure: one enum, one handle, one flag
word. No path translation, no mount table, no per-object virtual method
table, no inheritance-across-fork bookkeeping.

Cygwin's weight is not its fd table — it is the mount/path layer, the
`fork` copy machinery, and the signal/process personality. A three-field
registry is the minimum honest cost of a correct `fcntl`, and we take it
knowingly.

**Where the registry is NOT needed, do not consult it.** madc's own
channel layer already knows an object's kind statically; L1's
non-blocking support calls `ioctlsocket` directly with no lookup. The
registry exists for L2/L3 only — user programs speaking POSIX.

**Default-fd rule:** fds 0/1/2 and any fd madc did not open resolve
lazily via `GetFileType(_get_osfhandle(fd))`, so a program that never
touches the layer pays nothing and inherited descriptors work.

---

## 5. Packaging: static-first, no runtime DLL

**Requirement: a madc-compiled Windows program must never *require* a
madc `.dll`.** Shared linkage stays available; it is never mandatory.

`libmadc_rt.a` is already the right vehicle and already has the right
property:

- It is strict C11 with **no C++ runtime dependency** (`ledger_sources.txt`
  is the membership owner).
- It is per-mode (`libmadc_rt-hosted-x86-64-windows.a`).
- Its completeness is already gated: `emitc_sret_gate` links a specimen
  against **this archive alone**.
- **A static archive natively links only the members whose symbols are
  referenced.** "Statically embed the relevant parts" is what `ar` +
  `ld` already do — a program that never calls `socket()` gets no
  winsock shim in its binary.

Directives:

1. **One object per feature area** (`rt_posix_fd.o`, `rt_posix_sock.o`,
   `rt_posix_dl.o`, `rt_posix_str.o`, `rt_posix_time.o`, …), so
   archive-member selection is real granularity, not a 200KB blob.
2. Compile the archive `-ffunction-sections -fdata-sections`; native
   links pass `--gc-sections`. That gives sub-member granularity too.
3. **The JIT lane needs nothing**: the runtime is already compiled into
   `madc.exe`, so a JIT'd script resolves these symbols in-process.
4. AOT (`-o`, `--obj`) links the archive statically by default.
5. `--emit=c11` consumers get `include/madc/posix/**` + the archive; the
   documented invocation is `gcc prog.c -I<madc>/include/madc/posix
   -lmadc_rt`.
6. **Gate:** extend the `emitc_sret_gate` pattern — a specimen using
   sockets + `fcntl` + `dlopen` must link against `libmadc_rt.a` **alone**
   and run, and `objdump -p` must show no madc DLL in its imports.

---

## 6. Two oracles, and the parity rule that keeps this honest

Adding names mingw-gcc lacks changes what "gcc parity" means on this
lane. It is redefined explicitly, not quietly:

- **Codegen / ABI / type-model oracle: the target toolchain**
  (`x86_64-w64-mingw32-gcc`). Unchanged. Widths, layout, mangling,
  calling convention, `sizeof(long)` — mingw-gcc is canon, always.
- **POSIX semantics oracle: glibc on Linux.** What `fcntl(F_SETFL)` or
  `recv` on a non-blocking socket must *do* is defined by POSIX and
  witnessed by glibc. A shim is correct when a reducer prints the same
  thing on Linux/glibc and on win64/madc.

The emitted-C rule becomes: **emitted C compiles with the target's own
gcc against madc's POSIX headers and links against `libmadc_rt.a`.** The
gate in §5.6 is what proves it. A program that uses no POSIX-extension
name keeps the strict old property (compiles with bare mingw-gcc) — and
that must stay true, so the headers are additive only.

---

## 7. Non-blocking: two different surfaces, do not conflate them

**POSIX `O_NONBLOCK` already works on Linux, by default, with zero madc
code.** madc serves the real glibc `<fcntl.h>` and binds real glibc
`fcntl`, so a script doing `fcntl(fd, F_SETFL, flags | O_NONBLOCK)` gets
the platform's own implementation. Evidence: `tests/testfcntl.mad` does
exactly that and asserts `newflags & O_NONBLOCK`; its **only** skip
fixture is `.win64_skip` (no `.exe_skip`, no `.mir_skip`), so it passes
in the Linux JIT, EXE and OBJ lanes today.

That is the whole architecture in miniature, and it is the model this
plan follows: **madc supplies nothing the platform already supplies.**
Non-blocking is therefore a *pure win64 gap* — mingw has no `fcntl` and
no `O_NONBLOCK` (both verified) — served by P3 exactly like the other
nineteen. It is not a portable gap, and it does not need shipping
"first".

Separately and independently: **madc's own channel API has no
non-blocking mode on either platform.** Marker, recorded so a later
sweep re-checks instead of re-deriving — the CONCEPT, not one spelling:
`grep -rniE "nonblock|non_block|non-block|FIONBIO|MSG_DONTWAIT|SOCK_NONBLOCK|WSAEventSelect|EWOULDBLOCK|EAGAIN|set_blocking|blocking"`
over `madc_datachannel.cpp`, `madc_socket_channel.cpp`,
`madc_channel_stream.cpp`, `madc_process.cpp`, `madc_posix_io.cpp`,
`madc_datachannel.h`, `madc_process.h` — zero hits (2026-08-13). That is a question about madc's own abstraction
(should `tcp://` / `exec://` channels expose a non-blocking mode at
all?), not a POSIX-compliance question. It is genuinely optional, it
gates nothing in this plan, and it is deliberately demoted to P6.

The distinction generalises: **L2/L3 exist only where the target
toolchain is missing something. L1 grows only when madc's own portable
API is missing something.** A gap in one is not evidence of a gap in the
other.

---

## 8. Header-serving discipline

- Serve a POSIX header **only where the native toolchain lacks it**. The
  existing outranking machinery (`embedded_header_outranked()`,
  `is_embedded_header_allowed()`, `lexer.cpp`) already implements
  "a real header ahead of madc's resource slot wins" — reuse it, add no
  parallel path.
- Where mingw ships a header but omits a member or macro (`struct dirent`
  has no `d_type`; `fcntl.h` has no `O_NONBLOCK` — both verified), madc
  must **not** shadow the whole header. Serve the delta.
- Types that cross the ABI (`struct sockaddr`, `fd_set`, `struct
  timeval`) must be **byte-identical to the platform's own** — winsock2's
  definitions are the truth on win64. Take the interface fact from the
  platform header; never re-invent a layout.
- **Default: the layer is ON.** `--no-posix-compat` opts out for strict
  mingw-parity builds. Rationale: the owner's directive is maximal POSIX
  compliance, and a program that does not name these symbols is
  unaffected either way.

---

## 9. Slices (execution order; each lands with a gate)

**P1 — leaves.** `strndup`, `setenv`/`unsetenv`, the `timer*` macros,
`sleep`, `dirent.d_type`. No state, no registry. Clears 5 skips.
*Gate:* those tests drop their `.win64_skip` and pass.

> **Corrected 2026-08-14 (verified against the container's mingw root,
> `/usr/share/mingw-w64/include`).** "Leaves" was right for three of the
> five and wrong for two, and the mechanism assumption was wrong for all
> of them:
>
> | item | mingw state | real cost |
> |---|---|---|
> | `strndup` | `string.h` **ships**, symbol absent | declaration delta + shim |
> | `setenv`/`unsetenv` | `stdlib.h` **ships**, absent | delta + shim over `_putenv_s` |
> | `timer*` | `sys/time.h` **ships**; see the re-correction below | macro delta, header-only |
> | `dirent.d_type` | `dirent.h` **ships**, no such member | **not a leaf** — see below |
> | `sleep`/`usleep` | `unistd.h` **ships and declares `sleep`** | not a header problem at all |
>
> 1. **Every P1 header already exists on mingw**, so none can be served
>    whole (§8 forbids shadowing). The dominant P1 mechanism is
>    *augmenting* a real header — a supplement served immediately after
>    it, keyed by filename convention under `include/madc/posix/`, gated
>    on target + `--no-posix-compat`, additive-only. That mechanism did
>    not exist and is P1's first task.
> 2. **`d_type` cannot be a supplement.** It is a struct member, and
>    mingw's `readdir` fills mingw's `struct dirent`. Serving it means
>    madc owns `dirent.h` *and* `opendir`/`readdir`/`closedir` over
>    `FindFirstFileW` — its own slice, not a leaf. Half-serving it (a
>    shadowed struct the CRT never fills) would be a silent wrong answer.
> 3. **`sleep` is a symbol-resolution question**, not a header one:
>    `testlibc` includes nothing and tests the bare-name dlsym fallback
>    through `madcdl_sym_default`. Note that an archive member is linked
>    into `madc.exe` only if referenced — verify presence with `nm` on the
>    built binary, not from the Makefile.
>
> **Re-corrected 2026-08-14, later the same day — the `timer*` row above was
> WRONG, and the error was mine (Claude).** I ran
> `grep -cE 'timeradd|timerisset' $R/sys/time.h`, got 0, and wrote "absent".
> **The macros are not written in `sys/time.h`; they are in
> `_timeval.h`, which `sys/time.h` includes** (`_timeval.h:16-20`, alongside
> `winsock2.h`, `winsock.h`, `time.h` and `psdk_inc/_ip_types.h` as the other
> includers). Grepping one file cannot answer a question about a header's
> include closure — the same "the marker could not have found it" failure the
> forest pack-list error was, one day earlier.
>
> The verified truth: mingw provides **`timerisset`, `timercmp`,
> `timerclear`** natively; only **`timeradd` and `timersub`** are absent. The
> delta is two macros, not five, and Codex's narrower supplement
> (`include/madc/posix/sys/time.h`, `timeradd` + `timersub` only) is correct.
> Everything else in the correction above stands — including the point it
> exists to make, since `sys/time.h` shipping is exactly why a supplement is
> needed rather than a whole header.
>
> The per-task form is `docs/plans/2026-08-14-codex-handoff-3-posix-p1-p2.md`.

**P2 — `<dlfcn.h>`.** The backend already exists (`madc_dl.cpp`,
`LoadLibrary`/`GetProcAddress`, the `sym_default` walk). This is a header
plus a strict-C11 shim forwarding to it. *Gate:* `testclassstaticitanium`
passes; `testdlcall`/`testdlopen` need their `libc.so.6` asset made
portable first (they are test-asset problems, §10).

> **Corrected 2026-08-14 on implementation — two things above are wrong.**
>
> 1. **It must NOT forward to `madcdl_*`.** That seam is L0 (madc's own C++
>    calling the OS); this is L3, and §3 already settled it: *"they never
>    share code paths, because L3 must link into an emitted-C program that
>    contains no madc C++ at all."* An emitted-C program links
>    `libmadc_rt.a` ALONE, so a forward would be an unresolved symbol
>    there. `src/rt/rt_posix_dl.c` therefore implements
>    `dlopen`/`dlsym`/`dlclose`/`dlerror` over `LoadLibraryA` /
>    `GetProcAddress` / `FreeLibrary` directly, with `RTLD_DEFAULT` over
>    `EnumProcessModules` (which resolves to `K32EnumProcessModules` in
>    kernel32 — no psapi link). Same technique, separate owner, by design.
> 2. **`testclassstaticitanium` is NOT the gate and does NOT unskip.** It
>    asserts `dlsym(RTLD_DEFAULT, …)` can see libstdc++'s own
>    `numpunct<char>::id`. Measured on the actual artifact: the hosted PE
>    exports **5873** names and **0** matching `numpunctIcE2id`, because
>    libstdc++ is linked statically and PE `ld` does not export `_Z*` from
>    those archives (`sleep` IS in that table, which is the positive
>    control for the marker). So the test cannot print `matches_itanium=1`
>    on win64 no matter what `<dlfcn.h>` does. Its skip stays, with the
>    reason corrected from "mingw ships no dlfcn.h" to the real one.
>
> The gate is instead `tests/testdlfcnportable.mad` — the API contract with
> no library filename anywhere, so it runs on all three targets (that
> filename dependency is exactly what makes `testdlopen`/`testdlcall`
> structurally POSIX-only). **Net skip arithmetic: P2 clears 0 of the
> pre-existing skips and adds one new cross-platform test.** `<dlfcn.h>`
> is still worth having on its own merits — a user program can now
> `dlopen` a DLL — but the handoff's "clears 1 skip" was wrong.
>
> `include/madc/posix/dlfcn.h` is also the first WHOLE provider: mingw
> ships no such header, so there is nothing to augment. That needed a new
> lexer arm (`Program::tokenize_posix_whole_provider`), gated on the
> include walk's actual outcome — the resolved path names no readable file.

**P3 — the fd registry + `fcntl` + `flock`.** §4. `F_GETFL`/`F_SETFL`
(`O_NONBLOCK`), `F_GETFD`/`F_SETFD` (`FD_CLOEXEC`), `F_DUPFD`; `flock`
over `LockFileEx`/`UnlockFileEx`. *Gate:* `testfcntl`, `testflock`, and a
new reducer proving a non-blocking socket returns `EWOULDBLOCK`
identically on Linux and win64.

**P4 — sockets.** `<sys/socket.h>`, `<netinet/in.h>`, `<arpa/inet.h>`,
`<netdb.h>`, `<sys/select.h>`, `fd_set` reachable from `<sys/time.h>`.
Thin POSIX spellings over winsock, which madc already links and
initializes (the `socket_stack_ready` WSAStartup owner). Errno mapping is
the real work: `WSAGetLastError` → POSIX `errno` values, one table, one
owner. *Gate:* `testhttpget`, `testsockaddr`, `testtcpchannel`,
`testservent`, `teststructinterop`, `testfdsetfromsystime` unskip.

**P5 — SMAUG-driven residue.** `smaug_requests_source` /
`testsmaug_requests` fall out of P3+P4. Whether MadSMAUG *targets*
Windows is a separate product call — but after P4 the compiler no longer
blocks it.

**P6 — a non-blocking mode on madc's own channel API** (§7) — OPTIONAL
and independent. This is not POSIX compliance work (POSIX non-blocking
is free on Linux and lands with P3 on win64); it is the separate
question of whether madc's `tcp://` / `exec://` channel abstraction
should expose one. Needs an owner call on the API shape before it is
worth starting; nothing else in this plan waits on it.

---

## 10. Explicitly not in scope

- `fork()`, POSIX signals as a process model, process groups, `/proc`,
  uid/gid, mount/path translation (§1, L4).
- A `select()` that mixes file fds with socket fds. Winsock's `select`
  takes sockets only; POSIX programs that select on both are the one
  place we accept a documented divergence rather than build a poll
  emulator. Revisit only with evidence of a real consumer.
- The four **test-asset** skips, which no amount of POSIX layer fixes:
  `testbuiltinvalisttypedef` (asserts SysV va_list semantics that are
  *correctly* different on win64 — permanent), `testdlcall` /
  `testdlopen` (hardcode `libc.so.6`, an ELF soname), `testexecchannel`
  (expects POSIX `sort` collation). These need portable test assets, and
  that is a test-hygiene task, not a platform task.
- The 25 `*_libcxx` skips: the win64 lane has no libc++ stage by owner
  decision (2026-08-12). Unrelated to this plan.

---

## 11. Why this is not "madc becomes Cygwin"

Stated once, for the reviewer who will worry:

1. **No runtime dependency.** Cygwin programs require `cygwin1.dll`.
   madc programs link a static archive, and only the members they use.
2. **No parallel OS model.** No mount table, no path translation, no
   `fork`, no signals-as-process-model. A madc program's fds *are*
   Windows handles; its processes *are* Windows processes.
3. **No source lineage.** Clean-room from the spec, MPL-2.0 (§2).
4. **Additive only.** A program using no POSIX-extension name compiles
   exactly as mingw-gcc compiles it, with the same ABI and the same
   widths. The type model is untouched.

The honest summary is: **madc gains a POSIX compatibility library, the
way every serious Windows toolchain eventually does — not a POSIX
personality.**
