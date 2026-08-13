# The madc POSIX target surface (Win64 first) — plan

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
| **L2 POSIX headers** | POSIX spellings served only where the native toolchain lacks them | `include/madc/posix/**` (new) | ⬜ this plan |
| **L3 POSIX shims** | the implementations L2 declares | new members of `libmadc_rt` | ⬜ this plan |
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

## 7. Non-blocking is a portable gap, not a Windows gap

madc's channel layer has **no non-blocking support on any platform** —
verified: zero `O_NONBLOCK` / `FIONBIO` references in
`madc_socket_channel.cpp` or `madc_datachannel.cpp`. So this is a new L1
capability everywhere, not a win64 port:

- L1 API: a non-blocking mode on the channel surface (one owner, both
  platforms), POSIX arm `fcntl(O_NONBLOCK)`, Win arm
  `ioctlsocket(FIONBIO)` for sockets and `SetNamedPipeHandleState`
  for pipes.
- L2/L3 then expose the POSIX spelling over the same primitives.
- Ship it L1-first so the portable surface is never behind the
  compatibility surface.

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

**P1 — leaves.** `strndup`, `setenv`/`unsetenv`, `sleep`/`usleep`,
`timeradd`/`timersub`/`timerclear`, `dirent.d_type` (map
`dwFileAttributes` → `DT_DIR`/`DT_REG`/`DT_LNK`). No state, no registry.
Clears 5 skips. *Gate:* those tests drop their `.win64_skip` and pass.

**P2 — `<dlfcn.h>`.** The backend already exists (`madc_dl.cpp`,
`LoadLibrary`/`GetProcAddress`, the `sym_default` walk). This is a header
plus a strict-C11 shim forwarding to it. *Gate:* `testclassstaticitanium`
passes; `testdlcall`/`testdlopen` need their `libc.so.6` asset made
portable first (they are test-asset problems, §10).

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

**P6 — portable non-blocking in L1** (§7) — may land before P3 if it is
convenient; it has no dependency on the registry.

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
