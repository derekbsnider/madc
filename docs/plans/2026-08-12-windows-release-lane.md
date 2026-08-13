# Windows release lane — full-suite win64 (Track 6.4)

**Status: PLAN (drafted 2026-08-12, session #83). Owner directives:
mingw-w64 + libstdc++; UCRT (decided 2026-08-12); FULL suite support —
JIT, AOT `-o`, `--obj`, emitted C, packed groves — "no cutting
corners". Sequenced by the owner: this lane ships BEFORE the
GitHub-Actions release automation (which will then cover all three
OSes).**

The macOS lane (docs/plans/ macos-release-lane, shipped v0.76.0) is the
template: same artifact discipline (self-contained binary, packed
forest, provenance-audited prelude, verify gate on the exact shipped
bytes, in-vivo battery), different substrate. Windows is CLOSER to the
Linux lane than darwin was in one big way — same stdlib flavor
(libstdc++, GPL+runtime-exception, already our default lane) — and
farther in another: the host OS API surface (no fork, no dlopen, no
POSIX mmap semantics, winsock) and a third executable format (PE/COFF).

## Settled decisions

| Decision | Choice | Why |
|----------|--------|-----|
| Toolchain | **mingw-w64 cross from the build container** (`x86_64-w64-mingw32-g++`, the `-posix` thread flavor) | gcc canon end-to-end; winpthreads gives the pthread/std::thread surface for free; cross keeps QNAP/container discipline unchanged |
| C++ stdlib | **libstdc++** | owner directive; same flavor as the Linux lane — the groves/forest machinery reuses the default-lane path, not a new flavor family like darwin's libc++ |
| CRT | **UCRT** (owner, 2026-08-12) | C99/C11-conformant, ABI-stable, the supported modern CRT; an OS component on Windows 10+ (our floor); MSYS2's default since 2022. msvcrt is the VC6-era compat layer — wrong side of "no cutting corners" |
| long double | **x87 80-bit (mingw-gcc model), NOT MSVC's double** | gcc parity is canon; mingw-gcc keeps 80-bit long double on win64. Neither msvcrt nor UCRT can printf it — mingw's own ANSI stdio (`__USE_MINGW_ANSI_STDIO`) formats it; the embedded prelude must route the printf family accordingly. (Session #83 just fixed 16-byte long double alignment — the Windows target keeps it.) |
| Arch | **x86-64 only** (owner, 2026-08-12: "we only need to support Win64 — MIR is 64-bit only at the moment") | matches MIR's win64 gen support; 32-bit and AArch64-windows have no MIR floor |
| Linking | **static libstdc++/libgcc/winpthreads in shipped binaries** | self-contained ethos of the release lanes; no DLL-hell, no redistributable step. UCRT itself stays a dynamic OS import (that is the supported model) |
| Validation host | **The REAL Windows 11 box** — the build container runs inside WSL2 on it (topology confirmed 2026-08-12: container = a container inside the WSL distro; interop does NOT reach the container namespace, so execution goes over a channel to the host). Recommended channel: Windows' built-in OpenSSH server — the battery becomes an exact clone of the proven mac_battery ssh pattern | native-Windows evidence for EVERY batch, not just session end; no emulation layer in the loop. `wine64` stays available as an optional isolation fallback, nothing more |

## Workstreams

### W0 — Toolchain + provisioning
- Add mingw-w64 cross packages (`g++-mingw-w64-x86-64-posix`,
  `binutils-mingw-w64-x86-64`) and `wine64` to
  `scripts/provision_container.sh` (container is disposable — the apt
  layer dies with rebuilds).
- **W0.1 UCRT flavor check — RESOLVED (2026-08-12, probed).** Ubuntu
  noble's packages (gcc 13.2, mingw-w64 11.0.1) default to msvcrt and
  expose no `-mcrtdll`, but ship the full UCRT import libs
  (`libucrt.a`/`libucrtbase.a`) INCLUDING `__getmainargs`-family compat
  shims, so option (a) works with no CRT rebuild. The recipe:
  compile `-D_UCRT -D__USE_MINGW_ANSI_STDIO=1`, link
  `-specs=<dumpspecs with -lmsvcrt→-lucrt>` plus
  `src/win_ucrt_compat.S` (`_setjmp`/`_setjmpex` → `__intrinsic_setjmpex`
  thunks — msvcrt-compiled static libs like Ubuntu's winpthreads
  reference the msvcrt-only names). Two traps verified: `_UCRT` alone
  turns mingw's ANSI stdio OFF and `%Lf` of 80-bit long double silently
  prints 0.00 (UCRT treats ld as 64-bit); and `crt2u.o` is the
  *unicode*-entry object, not a UCRT variant. Gate codified as
  `scripts/win_ucrt_gate.sh`: UCRT-only import tables (apisets, no
  msvcrt.dll), `%lld` + `%Lf` correctness, static-libstdc++ C++, and
  the W2.1 probe with its negative control. Imports come out as
  `api-ms-win-crt-*` apisets (they forward to ucrtbase — that is the
  UCRT surface).
- **W0.2 Windows-execution channel — RESOLVED (2026-08-12, session #84),
  design ≠ plan: NO Windows sshd, NO reboot, NO tunnel.** True topology:
  the container runs under **Docker Desktop's own VM** (host.docker.internal
  resolves, 192.168.65.254), not inside the owner's Ubuntu distro. Channel:
  **container → `ssh derek@host.docker.internal` → the Ubuntu WSL distro's
  sshd** (WSL2 localhost-forwarding puts the distro's port 22 on the Windows
  loopback; Docker Desktop's host gateway reaches it) **→ WSL interop runs
  `.exe`s as GENUINE Windows processes** (real PE loader/ntdll/ucrtbase —
  real-Windows evidence, unlike wine). Container key authorized for derek.
  PROVEN: gate_hello_c.exe on Windows build 26200 prints the exact oracle
  (incl. 80-bit `ld=3.25` on real ucrtbase); W2.1 on real ntdll: NON_SEH
  probe PROBE_OK, SEH negative control dies (silently — WER swallows the
  banner, so battery legs must classify by output MARKER, not exit code).
  Gotchas: interop warns "UNC paths are not supported" when cwd is the WSL
  fs (cosmetic; cd to a /mnt/c path in the battery); exes run fine from the
  WSL fs. DONE: `scripts/win_run.sh` is that runner (scp+ssh over the
  channel, per-invocation dir under `/mnt/c/Users/Public/madcwin`, output
  relayed, marker-classification documented in its header) — the full
  4-leg `win_ucrt_gate.sh` passes on REAL Windows via
  `MADC_WIN_RUNNER="bash /workspace/madc/scripts/win_run.sh"` (absolute
  path — the gate cd's into its workdir). Original option
  analysis — the container cannot reach WSL
  interop (probed 2026-08-12: no /init, no WSLInterop binfmt, no
  /mnt/c inside the container namespace). Options, owner-side setup:
  (a) **enable OpenSSH Server on the Windows 11 host** (Settings →
  Optional features; the container then drives `ssh <user>@<host>` to
  stage + run, mac_battery-style) — RECOMMENDED; (b) re-plumb the
  container with interop (/init bind + binfmt + /mnt/c) — more moving
  parts, couples the container to WSL internals; (c) a shared-volume
  + watcher runner on the WSL distro — a bespoke moving part, last
  resort.
- Probe: cross-build a trivial C and C++ hello; run them over the
  W0.2 channel on real Windows; capture the import tables
  (`objdump -p`). This is the lane's hello-world gate.

### W1 — madc host port (the POSIX surface)
Inventory from recon (session #83 greps):
- **dlopen/dlsym — the big one.** ~10 files, including the
  mangled-direct architecture's `dlsym(RTLD_DEFAULT, ...)` resolution
  of libstdc++/libc symbols at MIR link time. Windows has no
  RTLD_DEFAULT: the equivalent is GetProcAddress over an explicit
  module set (EnumProcessModules, or the known set: the exe itself +
  ucrtbase + libstdc++/winpthreads statics resolve INTO the exe).
  Design a small `madc_dl` seam (one owner: `madc_dlopen/madc_dlsym/
  madc_dlsym_default`) implemented POSIX and Win32; NO scattered
  `#ifdef _WIN32` at call sites. With static libstdc++ most
  "RTLD_DEFAULT" hits resolve inside our own image —
  `GetProcAddress(GetModuleHandle(NULL), ...)` needs the exe built
  `-Wl,--export-all-symbols` (or a curated .def) so its symbols are
  visible. That linker decision is part of this workstream.
  **Slice 1 DONE (session #85, 2026-08-12): the seam EXISTS.**
  `include/madc_dl.h` + `src/madc_dl.cpp` — `madcdl_open_global` /
  `open_local` / `open_self` / `probe_loaded` / `sym` / `sym_default` /
  `close` / `error`; every host call site migrated (including the
  script-facing `dlopen()`/`dlsym()` builtin thunks in parser.cpp — the
  plan's proposed `madc_dlopen` names were TAKEN by those thunks, hence
  the `madcdl_` prefix — and the unit-test import resolvers;
  `test_native_shared.cpp` stays raw deliberately: it simulates a
  third-party C host consuming a madc-emitted .so via RTLD_DEEPBIND).
  POSIX passthrough, fulltest 1023/0. Gate:
  `scripts/check-one-dl-owner.sh` in fulltest (negative-controlled).
  **Slice 2 DONE (same session): dladdr rides the seam.**
  `madcdl_addr(addr, MadcDlInfo&)` — `MadcDlInfo{fname,fbase,sname,saddr}`
  with the Win32 contract stated (module path/base from the loader;
  sname/saddr may be NULL there — all consumers already guard). All 8
  sites migrated (madc.cpp crash-handler backtrace, madc_globals
  self-path, madc_cir cover analysis ×2, cir_builder host-object anchor,
  parser external-symbol recovery ×3); `<dlfcn.h>` is now included ONLY
  by src/madc_dl.cpp. Gate tightened to dladdr/Dl_info/`<dlfcn.h>` and
  negative-controlled again — it caught a third-indent parser.cpp site a
  replace-all had missed. fulltest 1023/0. NEXT in W1: the fork/exec
  surface (madc_process, exec:// channels → CreateProcess) or mmap
  (cir_freeze) — then the Win32 backend of madc_dl itself alongside the
  W1 host-port build wiring.
- **fork/exec** (`madc_process.cpp`, exec:// channels): CreateProcessW
  + pipe pair; the channel pump contracts (one pump loop owner) stay.
  **Slice 3 DONE (session #85): spawn owner consolidated.** The
  `--freeze-run` re-exec (madc.cpp's own fork/execv/waitpid) now rides
  `Process::run_and_wait(exe, argv)` — the owner's inherited-stdio arm
  beside the piped channel arm, so the Win32 backend (CreateProcess +
  handle-inheritance lists) lands in src/madc_process.cpp alone. Gate:
  `scripts/check-one-spawn-owner.sh` in fulltest (fork/vfork/exec-family/
  posix_spawn outside the owner = RED; negative-controlled both ways).
  system()/popen() deliberately out of scope — CRT-portable on mingw.
  **✅ WIN64 ISOLATION — OWNER-DISCUSSED AND REFRAMED (2026-08-12,
  session #85): the user-facing axis is IN-PROCESS vs SUBPROCESS**, not
  fork vs no-fork — fork vs CreateProcess-respawn is a platform backend
  detail behind the seam, exactly like every other W1 owner. Settled in
  the discussion: (1) isolation is already OPTIONAL (execution_mode
  defaults to in_process; fork_per_invocation is host opt-in; only
  system_locked clamps it on); (2) the interim win64 contract is a LOUD
  invocation-time error naming the knob ("subprocess isolation ... not
  available on this platform; use execution_mode::in_process") — never
  a silent in-process downgrade of a sandbox the host asked for;
  (3) the full contract (enum respelled subprocess_per_invocation with
  the C-API fork spelling kept as alias, NO host-image re-entry on any
  platform — a spawned child cannot resolve host symbols the way a
  forked one can, so the portable rule must hold everywhere — spawn
  backend = self-respawn + frozen-forest transport, also attractive on
  POSIX where fork-in-threaded-host is hazardous, and the system_locked
  wording for platforms without a backend) is a tracked design-doc arc
  (task #44).
  **Sibling posture, also settled 2026-08-12 — fork() in the LIBC
  OFFERING to compiled programs:** madc binds the host CRT's real
  symbols under real names; UCRT has no fork, and the platform's
  reference compiler (x86_64-w64-mingw32-gcc) has none either, so in
  strict C/C++ dialects on win64 `fork()` fails to resolve exactly as
  under mingw-gcc (gcc parity; anything else would break the emit-C
  oracle). The exec*/_spawn family binds normally (it exists in the
  CRT, Windows semantics). The PORTABLE process story is madc's own
  surface (Process, exec:// channels, madc::sys), which already has
  its CreateProcess backend. No Cygwin-style fork emulation, ever.
  **Slice 9 DONE (session #85): the Process owner's Win32 arm.**
  `start()` = CreatePipe pairs (child ends alone inheritable) + a
  `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` restricting inheritance to exactly
  the three std handles (the O_CLOEXEC analogue); parent ends bridge to
  CRT fds via `_open_osfhandle`, so the pipe channels and the pump loop
  are untouched. `run_and_wait()` = explicit application path +
  inheritable std-handle duplicates (the execv contract; duplicates, not
  inherit-flag mutation — flag flips would race concurrent spawns in an
  embedding host). One command-line string under the MS CRT quoting
  rules (`append_windows_argument`; CRT argv contract only — no cmd.exe
  ^ rules, no madc surface spawns batch files); environment block
  ci-sorted for the loader; NTSTATUS exit codes flow through the
  existing `int` status (no WIFEXITED split); `terminate()` =
  `TerminateProcess(128+SIGTERM)`. Shared owners that rode along:
  `detail::read_fd` (both raw `::read` channel sites + the exec-errno
  reader migrated; Win arm = one `_read` capped at INT_MAX) and
  `detail::win_error_text` (the one GetLastError formatter — madc_dl's
  private FormatMessage copy adopts it). madc_process.cpp mingw-GREEN
  (8th of 14 probed TUs); fulltest 1023/0. Run validation = the
  hosted-MODE battery, like every Win32 arm.
  **Slice 10 DONE (session #85): datachannel Win32 arm — mingw-GREEN +
  64-bit-correct.** Open flags arm in place (`_O_NOINHERIT` atomic
  close-on-exec + `_O_BINARY` — the Win CRT defaults to TEXT mode, CRLF
  would corrupt a byte channel). Fix-what-you-find: the seek/size/
  regular-file sites COMPILED under mingw but were 32-bit-wrong
  (mingw off_t/struct stat truncate >2GB; 32-bit _fstat FAILS on >4GB,
  demoting big files to non-seekable) — now behind `detail::seek_fd`
  (_lseeki64), `detail::fd_size` (_fstat64), `detail::fd_is_regular_file`.
  9 of 14 TUs green; fulltest 1023/0.
  **Slice 11 DONE (session #85): diagnostics arms — madc.cpp + parser.cpp
  (the 20k-line TU) go mingw-GREEN.** ⚠ TRAP: winnt.h declares a
  `TokenType` ENUMERATOR (TOKEN_INFORMATION_CLASS) — windows.h can NEVER
  meet madc's tokens.h in one TU. The crash surface therefore moved to
  its own TU `src/madc_crash.{h,cpp}` (shared JIT-aware backtrace
  printer; POSIX sigaction/sigaltstack arm unchanged; Win32 arm =
  SetUnhandledExceptionFilter + CaptureStackBackTrace +
  SetThreadStackGuarantee, returns CONTINUE_SEARCH so WER/NTSTATUS still
  happen). Resource guards = documented no-ops on Win (darwin-RLIMIT_AS
  posture; knob-naming JobObject guard = residual). Syslog sink: policy
  in the engine, transport = `detail::debug_log_line`
  (OutputDebugStringA). `localtime_r` ×2 → `detail::local_time` (MS
  localtime_s swaps the argument order). Script env/errno builtins bind
  the host CRT's REAL names per the in-comment canon: `_putenv_s` +
  `_errno` on Windows. Probe note: TUs including cir headers need
  `-Ithird_party/mir` in the probe recipe. 11 of 14 TUs green — the
  remaining 3: madc_program (fork-isolation OWNER DECISION), pch (zlib),
  madc_socket_channel (winsock), ns_perl (glob). fulltest 1023/0.
  **Slice 12 DONE (session #85): winsock + glob arms — 13 of 14 probed
  TUs mingw-GREEN.** Socket channel Win32 arm: ONE WSAStartup owner
  (`socket_stack_ready`, never WSACleanup), winsock errors harvested via
  `socket_last_error()`/`set_socket_error_code()` (WSA codes are system
  codes — win_error_text formats them), SOCKET kept in the int fd model
  (kernel handles fit 32 bits, the documented WOW64 interop rule;
  INVALID_SOCKET truncates to -1), stream write = `::send` (⚠ a SOCKET
  is NOT a CRT fd — write_fd_without_sigpipe would be wrong), close =
  closesocket, shutdown = SD_RECEIVE/SD_SEND, close-on-exec =
  SetHandleInformation, datagram truncation = WSAEMSGSIZE (winsock has
  no recvmsg/MSG_TRUNC), AF_UNIX via `<afunix.h>` (Win10 1803+).
  perl::glob rides a new `detail::glob_paths` owner (POSIX ::glob /
  Win component-wise FindFirstFile walk) — MANDATORY routing: ns_perl
  includes tokens.h, windows.h can never enter it. pch.cpp cleared by
  installing libz-mingw-w64-dev (+ recorded in
  scripts/provision_container.sh PKGS_winlane). The ONE red TU left:
  madc_program.cpp = the fork-isolation OWNER DECISION. fulltest 1023/0.
  **Slice 14 DONE (session #85, 4c7d94fb..e6bbfd2d): the
  hosted-x86-64-windows MODE — madc.exe LINKS, UCRT-clean, and runs
  under wine.** All 14 probed TUs mingw-GREEN (madc_program via the
  loud isolation interim above; madc_globals self-path arm was probe-
  flagged but never ported — GetModuleFileNameA arm added). The MODE:
  mingw-w64 -posix + the W0 UCRT recipe baked into CC/CXX (host tables
  see the binary's own posture), NON-SEH setjmp lane-wide, per-mode
  MIRLIB variant (host detection = native _WIN32 arms), -static,
  -Wl,--export-all-symbols, -lws2_32 -lz, .exe suffix. Link-burndown
  facts: (1) ⚠ the C++ ABI leaks the CRT flavor via std::mbstate_t
  (msvcrt: int / UCRT: _Mbstatet) — every fpos<mbstate_t> symbol
  mangles differently, so the distro msvcrt-flavor libstdc++ CANNOT
  serve -D_UCRT objects; scripts/build_win_ucrt_libstdcxx.sh stages a
  UCRT rebuild of the same 13.2.0 libstdc++ once per container (in
  provision_container.sh; gthr-default.h must be synthesized or
  gthreads silently configures OFF = no std::mutex; ⚠ nm | grep -q
  under pipefail SIGPIPEs nm on a MATCH — materialize then grep).
  (2) PE weak externals don't compose with emutls thread_locals
  (madc_globals plain-defines on win). (3) __madc_builtin_stpncpy_chk
  composed from memcpy/memset chks (mingw lacks the glibc libcall).
  Result: 17.7MB madc.exe, imports = KERNEL32 + api-ms-win-crt-* +
  WS2_32 only; wine runs it — front end fully alive (colored caret
  diagnostics). **Run-burndown defect #1 diagnosed to root:** plain
  `printf` resolves NOWHERE at runtime — UCRT doesn't export it, and
  PE ld auto-excludes the mingw runtime archives from
  --export-all-symbols so the __mingw_printf interposer never reaches
  the export table (puts etc. resolve fine from ucrtbase). NEXT: the
  compiled-in runtime name→address table (darwin runtime-table
  precedent) in the resolver — parse-time probe and JIT import
  resolution must share it — then the battery legs under wine +
  win_run.sh.
  **Slice 15 DONE (session #85, 1b38c948): defect #1 FIXED — the ONE
  mingw ANSI-stdio interposer map.** The W2 driver map promoted to
  fork-owned `third_party/mir/mir-mingw-stdio.h` (referencing __mingw_*
  makes the host's own static link bind the addresses — no export table
  involved); BOTH default-scope resolvers consult it first: c2m's
  import_resolver and madcdl_sym_default's Win arm (which the parser's
  existence probe AND madc_import_resolver already share — the slice-1
  seam consolidation is what made this a one-site fix). Gate:
  scripts/check-one-mingw-stdio-map.sh in fulltest (__mingw_ outside
  the owner header = RED; negative-controlled). Validated under wine:
  hello14 prints; stdio15 reducer %s / sscanf %lf %d / printf %.1Lf all
  exact vs the mingw-gcc oracle. New fork-file convention (owner
  2026-08-12): files ADDED to the fork carry MadC-project attribution
  (Copyright 2019-2026 Derek Snider, same license as MIR) — applied to
  mir-mingw-stdio.h, mir-macho.c, mir-int128-helper.h (cb2325a5);
  mir-debug*.c/h are Cyan Ogilvie-added, attribution left to the owner.
  **Run-burndown defect #2 DIAGNOSED TO ROOT (the LLP64 arc — task
  #45, NEXT):** `printf("%lld", 1234567890123LL)` prints the low-32
  truncation; madc.exe reports sizeof(long)=8 where the mingw-gcc
  oracle says 4. Root: madc conflates "madc's 64-bit int" with the C
  spelling `long` — true on LP64, false on LLP64 (the fork's c2mir
  correctly models win64 `long` as 32-bit, cx86_64.h). THREE layers,
  fix in this order:
  (a) CIR spellings — cir_builder.cpp is the only speller (80 N_LONG
  sites; cir_emit_c.cpp just renders). Every site EXCEPT the long
  double pair {N_LONG, N_DOUBLE} means i64. Respell as `long long`
  ({N_LONG, N_LONG}) — 64-bit in BOTH models, so a no-op on LP64 and
  correct on LLP64: native_scalar_specs dtINT64/dtUINT64/default
  arms + the ~40 literal `{ {N_LONG}, ... }` extern shapes (vector
  consumers materialize verbatim, need_output_extern:7921/7951) + ~25
  direct simple(N_LONG) sites via a small append helper. Emit-C
  renders spec sequences token-by-token, so "long long" falls out.
  (b) HOST thunk signatures — extern-C runtime fns spelling `long`
  (madarray_size returns long; madarray_assign_int(void*, long);
  __madc_sys_init(long, ...); audit madc_mir_backend.cpp + ns_*.cpp +
  parser.cpp registrations) are 4-byte on the win64 HOST — the CIR
  extern and the C++ definition must agree, so sweep them to
  int64_t/uint64_t. Win64 callconv hides the mismatch (register args,
  callee reads 32 bits) — a SILENT truncation class.
  (c) The DIALECT question — what is script-`long` on win64? gcc
  parity (mingw-gcc = LLP64) says 4 bytes for strict C/C++ dialects;
  madc's DataDef model is LP64-hardcoded (front-end sizeof folds 8).
  Front-end target-parameterized type widths are a design decision —
  OWNER DISCUSSION before implementing; (a)+(b) are unambiguous and
  fix the truncation family regardless.
  Reducers: container tmp/win/lld16.mad (A literal / B variable /
  C sizeof triple) + stdio15.mad vs mingw-gcc oracles — wine: madc
  prints "A 1912276171 / C 8 8 8", oracle "A 1234567890123 / C 4 8 8";
  Linux is unaffected (both models 64-bit). ALSO fixes: malloc extern
  currently declared with a 4-byte size param on win64
  ({N_UNSIGNED, N_LONG} — cir_builder:8050).
  **Slices (a)+(b) SHIPPED (session #85, 77094857 + 0cb1f328): the
  truncation family is FIXED.** (a) One spelling owner —
  CirBuilder::append_i64 / i64_list emit the two-N_LONG `long long`
  spec; all ~80 sites respelled (native_scalar_specs /
  append_type_specs / emit_symbol_ret_specs arms, extern shapes,
  direct spec builders; long double keeps its {N_LONG, N_DOUBLE}
  pair, ld-pair-tagged). 64-bit constants ride N_LL/N_ULL + u.ll/u.ull
  — N_L's u.l storage ALSO narrowed on win64 because c2mir_node.h
  hardcoded 64-bit c2mir_long while claiming to mirror cx86_64.h; the
  mirror now carries the _WIN32 arm. integer() takes int64_t (host
  long is 32-bit on the win64 build); (long) call-site casts widened.
  N_LL added to the two constant-consumer checks. Gate:
  scripts/check-i64-spec-spelling.sh in fulltest (three markers,
  negative-controlled). (b) Host thunk sweep: madarray_size /
  assign_int / assign_bool, __madc_sys_init/_once,
  __madc_atomic_fetch_add_l → int64_t; _chk sizes + object_size →
  size_t; packed-varargs %d-family cast → long long; cir_builder's
  storage-word sizeof/alignof(long) → long long (alignof(long)=4 on
  win64 under-aligned 8-byte buffer slots). VERIFIED under wine:
  lld16 A/B and stdio15 x=%lld all print 1234567890123 in full,
  byte-identical to Linux madc and the mingw-gcc oracle's 64-bit legs.
  **Residual = (c), the dialect question (OWNER):** sizeof(long) still
  folds 8 in the front end (oracle: 4) — script-long width on win64,
  plus the platform-long-semantic helpers classified out of (b) (the
  *l bit-scan family, the generic overflow triple,
  __madc_try_context_size, and the consumer-less __madc_fd_/timeval
  dead thunks noted for cruft removal).
  **✅ (c) SETTLED BY OWNER (2026-08-13): the win64 target follows the
  PLATFORM type model (LLP64/MSVC/mingw), not Cygwin/POSIX.** Rationale:
  madc.exe is a Win64 C/C++ compiler — "we're not necessarily trying to
  make madc.exe act more like UN*X"; full POSIX flavor is served by
  madc under WSL. Consequences (the target-type-model arc, to plan):
  script `long` = 4 bytes on the win64 target (all dialects — follow
  mingw), `wchar_t` = 2 bytes there, the *l builtin family operates on
  32-bit long, sizeof folding + struct layout + the literal-typing
  ladder become target-parameterized (per the vision invariants: gated
  via the target enum, no hardcoded platform checks). The value ABI's
  int64_t payload is unaffected. Guaranteed widths stay the int64_t
  family names.
  integration tests PASS byte-exact** (testint, testfunc, testmath vs
  Linux madc, CRLF-normalized). Two findings: (1) ⚠ BATTERY HARNESS
  NOTE — win64 stdout is CRLF (Win CRT text mode; mingw-gcc programs
  do the same, so CRLF IS gcc-parity-correct platform behavior); the
  battery's oracle comparison must normalize `tr -d '\r'`, never
  "fix" madc.exe to emit LF. (2) **Run-burndown defect #3 (NEXT):**
  testif/testswitch/teststruct/testptr die at parse with
  "swprintf.inl:68: Incorrect number of parameters for 'vswprintf':
  expected 4 got 3" — mingw's swprintf.inl inline calls the 3-arg MS
  vswprintf while a 4-arg ISO prototype is in scope, i.e. the SCRIPT
  compile's preprocessor state selects INCONSISTENT
  __USE_MINGW_ANSI_STDIO branches across the served header chain.
  Hypothesis for next window: the script-side predefine set on the
  win64 host must carry the binary's own posture (_UCRT +
  __USE_MINGW_ANSI_STDIO=1 — the HOSTTAB principle); check
  sys_include_paths / the hosted-MODE predefines, and what mingw-g++
  itself predefines for the oracle. Repro: wine madc.exe
  tests/testif.mad (Linux rc=0).
  **Defect #3 burndown (session #86, 2026-08-13): the banked hypothesis
  was WRONG; five distinct roots fixed (four Linux-latent madc bugs),
  original error gone; testif still red at the NEXT frontier —
  "basic_string.h:87: use of undeclared identifier 'max_size'" in the
  staged UCRT libstdc++ 13.2.0 (13.3-on-Linux never showed it; fresh
  diagnosis next window).** The predefine posture was already correct
  (probe: script TU sees _UCRT, __USE_MINGW_ANSI_STDIO=1, NONSEH,
  _WIN64, SZL=4, SZW=2 — the HOSTTAB capture works; note the stale
  hardcoded __LP64__=1 seed survives for task #46, and __STRICT_ANSI__
  is seeded on BOTH lanes, established posture, suites green). The
  real chain, in burndown order:
  (1) **Global overload sets excluded the untracked FIRST declaration**
  — mingw swprintf.inl declares 4-arg ISO vswprintf then a 3-arg
  extern "C++" overload; the first (source-named, dlsym-import
  contract) never joined the "::name" set and carried no
  function_display_name, so the .inl's own 3-arg call could never
  re-rank ("expected 4 got 3"). Invisible to the libc++ global-abs
  precedent (all arity 1) — and on the libc++ LINUX lane abs(-2.5)
  silently truncated through int abs. Fix: parseDeclaration seeds the
  pre-existing source-named global into the set when tracking starts
  (sentinel spelling; Variable/import name untouched). Gate:
  tests/testglobaloverload_libcxx.mad (clang++ -stdlib=libc++ oracle
  2/25/7/3).
  (2) **#if/#elif capture lacked phase-3 comment replacement** —
  `#if !defined __NO_ISOCEXT /* in libmingwex.a */` (mingw wchar.h /
  stdlib.h guard for wcstold/strtold/llabs) reached the expression
  evaluator as `/ * garbage` and silently read FALSE, dropping the
  declarations ("'wcstold' is not a declaration in '::'" at cwchar).
  Fix: evaluateIfCondition strips comments at capture (block comments
  may span lines; literals shield the introducers). Gate:
  tests/testifcomment.mad.
  (3) **`(**name)(params)` declarator unsupported** — winpthreads'
  `extern void WINPTHREAD_API (**_pthread_key_dest)(void *);`
  (pthread.h:282). Fix: extra stars in the fn-ptr declarator arm wrap
  the DataDefFPTR in pointer levels. Gate: tests/testfnptrptr.mad.
  Residual = Gap{fnptr_ptr_value_semantics} (CIR renders the ptr-to-
  fnptr as long long*, &fnptr-var init drops the &, subscript-call
  fails c2mir check; reducers tmp/win/fpp31/32/33.mad; no live
  consumer — the shape was unparseable before).
  (4) **Unary minus typed as int over real/wide operands** — TokenNeg
  already propagated unsigned/complex but not real/wide-int, so
  abs(-2.5) RANKED as int even with the set fixed. Fix: TokenNeg
  datadef propagates real + wider-than-int integer operands.
  (5) **Lexer-aliased libm builtins lacked real signatures** —
  bits/std_abs.h calls __builtin_fabs with deliberately no <math.h>
  ("Use builtins to prevent needing math.h"); the aliased bare `fabs`
  fell to the i64 dlsym default and the xmm0 double read back as
  garbage (abs(2.5) -> 1.0). Fix: register the whole aliased family's
  real signatures beside the copysign precedent (fabs/sqrt/sin/cos/pow
  families + labs/llabs — labs is dtINT32 on _WIN32, the LLP64
  returns-long class).
  Plus the fork map extension: mir-mingw-stdio.h grew (a) direct
  __mingw_* spellings the served headers' inline bodies call (narrow
  twins + the wide printf/scanf family + strtox), (b) libmingwex plain
  names whose ucrtbase export is the wrong LD flavor or absent
  (strtold/wcstof/wcstold, fabs/fabsf/fabsl). Wide PLAIN names
  deliberately have no entries (__mingw_swprintf's 3-arg variadic
  shape is NOT ISO swprintf). NOTE for the battery: further libmingwex
  math names will surface the same way; if the list grows past a
  handful, generate the table from nm (HOSTTAB-style) instead of
  hand-extending.
  **Next-frontier probes (banked, reducers in container tmp/win/):**
  (a) bstr37.mad — `#include <iostream>` alone PARSES but dies at MIR
  link, "import of undefined item _ZNSt8ios_base4InitC1Ev":
  mangled-direct std:: symbols cannot resolve on win64 because the
  -static libstdc++ inside madc.exe is auto-excluded from
  --export-all-symbols. This blocks the ENTIRE C++ mangled-direct
  architecture on the lane — the real next arc. Candidate designs: a
  build-time-generated .def export list for the staged libstdc++'s
  defined symbols (nm-driven, HOSTTAB-style; ld requires listed
  symbols be linked in — may need --whole-archive), or shipping the
  staged libstdc++ as a DLL beside madc.exe (the darwin LC_LOAD_DYLIB
  analogue; the UCRT stage script currently builds static-only).
  (b) bstr38.mad — iostream + a GLOBAL `string test = "..."`
  reproduces the basic_string.h:87 "undeclared identifier 'max_size'"
  parse error (13.2.0-text instantiation issue; `#include <string>`
  alone is green). testif needs both fixed.
  **Mangled-direct arc SHIPPED (session #86 cont., commit 0eea1ae5):
  the DLL design (darwin flat-bind analogue), not the .def export
  list** — a .def would need --whole-archive libstdc++ (a >2x exe-size
  trade needing an owner YES) plus the PE 64k-ordinal risk, and mingw's
  own posture IS shared libstdc++. Three pieces: (1) the stage script
  builds libstdc++-6.dll through the same ucrt.specs swap (a plain
  -shared link would pull -lmsvcrt = second CRT) AND builds winpthreads
  from mingw-w64 11.0.1 source as a UCRT-flavor libwinpthread-1.dll —
  the distro's DLL imports msvcrt.dll, and a static-in-exe +
  shared-in-DLL winpthread split would put TWO instances in one process
  (pthread objects cross the exe<->DLL boundary: std::thread's
  _M_start_thread/join are compiled in the DLL; madc_process pump
  threads are host-side; winpthread handles are per-instance
  allocations). Gates in-script: DLL present, UCRT-mangled fpos export
  surface, no msvcrt.dll import in either DLL. (2) hosted MODE links
  libstdc++ + winpthread SHARED from the stage (zlib stays in the
  -Bstatic window, -static-libgcc), both DLLs copied beside the exe.
  (3) madcdl_sym_default walks self -> recorded -> libstdc++-6.dll ->
  libwinpthread-1.dll -> ucrtbase -> kernel32; the DLL export table
  plays libstdc++.so-dynsym's role under Linux dlsym(RTLD_DEFAULT);
  one-site fix (every resolver rides the slice-1 seam). RESULTS:
  bstr37 prints iostream-ok; bstr38's max_size parse error is GONE —
  same root (failed _Z existence probes had forced a
  pattern-instantiation path); wine sweep 3/7 -> 6/7 byte-exact
  (testswitch/teststruct/testptr green); madc.exe 17.9 -> 14.7MB.
  Remaining red: testif == bstr38 residual, undefined _Znwm — madc
  mangles size_t with LP64 'm' where win64 libstdc++ exports 'y'
  (win64 size_t = unsigned long long). That is task #46's type model;
  slice 46a (mangling letters + __LP64__ seed rider, no width changes)
  follows this entry.
  **46a SHIPPED (commit 27415fd8) + VMI typeinfo host-long fix
  (e91d2a70) + runner DLL-shipping (3e989795): the 7-test wine sweep is
  7/7 byte-exact.** 46a = ONE owner `TargetDataModel`
  (madc_target_data_model, datadef.h, host-derived default) consumed by
  DataDef::mangle_scalar_spelling (dtINT64/dtUINT64 desugar through
  `long long` on LLP64), builtin_code's width-carrying rows
  (size_t/uint64_t -> y, int64_t/ssize_t/ptrdiff_t -> x), the lexer's
  __LP64__ seed (LP64-only now — mingw never defines it so the baked
  capture could not overwrite the stale seed), and the labs
  registration (adopted the owner, replacing its private #ifdef).
  Unit-gated in test_mangle.cpp (LLP64 flip + LP64 negative control;
  note the desugar's typeid guard means only PLAIN DataDef instances —
  the parser-minted alias shape — desugar; the builtin dd subclasses
  never do). bstr37 iostream-ok, bstr38 globstr-ok, testif green.
  The VMI find (mingw -Wshift-count-overflow, analyzed not ignored):
  __vmi_class_type_info packed flags|(base_count<<32) in 32-bit host
  `long` through void_ptr_int(long) — silently wrong MI RTTI on win64
  only; all three widths now explicit 64-bit; concept sweep (`<< 32`
  on host-long types) found no other live site. Runner gained
  MADC_WRAPPER (generic prefix knob, negative-controlled 0/1026 with
  /bin/false; .expect matching is per-line SUBSTRING so CRLF needs no
  normalization layer). **46b still open** (script long = 4-byte width
  on win64, wchar_t internals, *l builtin widths — the lexer TS_LONG
  mapping and sizeof folding still say 8).
  **NEXT: the full-suite wine inventory** (MADC_BIN=madc.exe
  MADC_WRAPPER=wine run_tests.sh) — probe first, classify the honest
  fail list, mint .win-lane skip fixtures only for structurally-POSIX
  tests, then burn down the rest.
- **mmap** (`cir_freeze.cpp` — forest packing): reads can fall back to
  buffered IO; if mapping stays, CreateFileMapping/MapViewOfFile behind
  the same seam.
  **Slice 4 DONE (session #85): mapping owner consolidated.**
  `madc::detail::map_file_readonly(path, len)` in madc_posix_io owns the
  one mmap (cir_freeze forest image; madcdis arena/id_table frozen-
  segment plans will ride it too). Bonus: cir_freeze's `#undef
  MAP_FAILED` + `(void *)-1` wart (MIR's mir-code-alloc.h redefines the
  macro) is DELETED — the owner includes no MIR headers. Gate:
  `scripts/check-one-mmap-owner.sh` in fulltest (mmap family + MAP_/
  PROT_ tokens + `<sys/mman.h>` outside the owner = RED;
  negative-controlled).
- **pthreads**: covered by the `-posix` toolchain flavor (winpthreads);
  DBG's thread_local discipline unchanged.
- **sockets** (tcp:// channels, madcdis): winsock2 — WSAStartup once,
  closesocket vs close, SOCKET vs int fd. The channel layer's
  close-on-exec atomicity note becomes HANDLE inheritance flags.
- **`madc_posix_io.cpp`**: the deliberate POSIX-IO surface — audit
  what of it is script-facing API (must work via Win32 equivalents)
  vs host plumbing.
- **PROBED WORKLIST (session #85): 35/49 TUs already compile under
  x86_64-w64-mingw32-g++ 13-posix** (`-fsyntax-only -D_UCRT
  -D__USE_MINGW_ANSI_STDIO=1`; per-TU results in the container's
  `tmp/win/compile_probe2.txt`; counts are FIRST-error for
  fatal-include failures — e.g. parser.cpp's `<syslog.h>` hides behind
  its readline fatal, so each fix can reveal one more). The 14:
  - Seam owners (= the designed Win32 backends): `madc_dl.cpp`
    (dlfcn.h), `madc_posix_io.cpp` (sys/mman.h), `madc_process.cpp`
    (sys/wait.h).
  - Function-level gaps: `madc_globals.cpp` (readlink → 
    GetModuleFileName arm), `ns_madc.cpp` (gethostname → winsock),
    `madc_datachannel.cpp` (pread ×3 → seek+read owner),
    `cir_emit_c.cpp` (open_memstream → portable buffer writer),
    `madc.cpp` (execinfo backtrace → CaptureStackBackTrace or stub),
    `madc_program.cpp` (sys/resource rlimit → stub/JobObject).
  - Dependency gaps: `lexer.cpp` + `parser.cpp` (readline → the
    includes were DEAD, deleted in slice 5 — nothing in the tree calls
    GNU readline), `pch.cpp` (zlib → Ubuntu ships libz-mingw-w64-dev),
    `madc_socket_channel.cpp` (netdb.h → winsock2/ws2tcpip),
    `ns_perl.cpp` (glob.h → FindFirstFile-backed or vendored glob).
  - **Slice 5 DONE (session #85): POSIX-side portable sweep, round 1.**
    Dead readline includes deleted; `emit_to_string` rides a
    string-capture owner (`open_string_capture`/`finish_string_capture`
    in madc_posix_io — POSIX arm = open_memstream, Win arm = tmpfile
    read-back later) making `cir_emit_c.cpp` mingw-GREEN; pread/pwrite
    ride `pread_fd`/`pwrite_fd` (same owner, EINTR-retry inside; Win
    arm = OVERLAPPED-offset ReadFile/WriteFile). Peeling reveals the
    next classified layer per TU: lexer=realpath (→_fullpath),
    parser=syslog (→event-log/file arm), datachannel=O_CLOEXEC
    (→_O_NOINHERIT) — all Win32-sweep items.
- Rule discipline: every port goes through a named seam owner
  (helper-methods.md, no-parallel-implementations.md) — one
  implementation per concern, two platform backends.

### W2 — MIR/c2mir win64 floor (in-tree, `third_party/mir`)
- Upstream MIR already has: VirtualAlloc executable pages
  (mir-code-alloc-default.c), the Win64 calling convention throughout
  mir-gen-x86_64.c, `_WIN32` handling in c2mir. The JIT floor EXISTS.
- **PROBED + LARGELY FIXED (2026-08-12, session #84).** c2m.exe cross-builds
  with the W0.1 recipe and JIT-runs probes under wine. Six win64 floor
  defects found (several inherited from upstream, verified by building
  upstream c2m: identical failures) and fixed in-tree:
  1. c2mir's builtin `<stdarg.h>` mapped win32 `va_start` to the MSVC
     vcruntime intrinsic `__va_start` (unresolvable at JIT link) — now
     `__builtin_va_start` → MIR's native VA machinery on all targets.
  2. `classify_node` was missing ALL fork/madc node kinds (complex,
     defer, class, asm, attr) — assert at -O0, silent misclassification
     under NDEBUG. All 13 codes added, default made loud.
  3. TBAA `get_type_alias_name` lacked the complex basic types (win64's
     typed-memory complex lowering reaches it; SysV never did).
  4. Complex classification in `cx86_64-ABI-code.c` was SysV-only and
     OVERFLOWED types[] on win64 (MAX_QWORDS=1 — the NDEBUG page fault).
     win64 rules per gcc oracle: `float _Complex` = one GPR qword
     (RAX-returned, packed re|im); double/long-double complex = memory
     class. All three flavors now compute correctly under wine.
  5. VA_START/VA_ARG got the va_list VARIABLE pseudo where MIR's contract
     wants its ADDRESS (win64's scalar `char*` va_list register-allocates;
     SysV's array never did) → generated code dereferenced ap's
     uninitialized VALUE. Fixed win-only with `MIR_ADDR` (gen N_ADDR's own
     mechanism); a first, ungated attempt broke SysV va (array decay) and
     was caught by the native probe battery — the gate matters.
  6. **long double = mingw model (x87 80-bit, sizeof/align 16)**: 
     `mir_ldouble` now real long double except on MSVC hosts; MIR core's
     five `_WIN32 → LD-is-D` downgrade gates replaced by one `MIR_LD_IS_D`
     macro (mir.h) that keys on the MSVC host, not on Windows. Call
     boundary rides the memory-class machinery (`memory_value_type_p`):
     by-ref args, hidden-pointer return, by-ref varargs — exactly the
     mingw-gcc oracle. Register-resident LD values spill via the
     complex_temp ALLOCA idiom at arg/ret boundaries.
- **W2 residual — RESOLVED (2026-08-12, session #85).** The "one root"
  turned out to be FIVE distinct sites, all of one concept — "a memory-value
  SCALAR (win64-mingw long double) needs value materialization where the
  aggregate machinery only hands around block pointers":
  1. **c2mir callee param gather** (c2mir.c N_FUNC_DEF): a `reg_p` param
     skipped the gather entirely — the arg var (`I0_x`, the block ADDRESS)
     and the value reg var the body reads (`D0_x`, N_ID gen) were two
     unconnected registers, so the body computed on an uninitialized local.
     Fix: for `reg_p && memory_value_type_p` params, emit one typed load
     through the pointer into the N_ID-named value reg.  Complex/aggregates
     are never reg_p; SysV has no memory-value scalars — predicate-dead
     there, no #ifdef needed.
  2. **c2mir va_arg block path**: presented the fetched block as
     `MIR_T_UNDEF` mem (aggregate convention); scalar consumers can't read
     it.  Fix: typed mem for `scalar_type_p` (note: `t` is STALE there under
     va_arg_p — compute `get_mir_type(type)` directly).
  3. **c2mir call-result "by addr" branch**: same UNDEF presentation for the
     hidden-pointer LD return → `ldmov: wrong type memory` MIR error.
     Same fix shape (typed mem + `t`).
  4. **MIR machinize_call (mir-gen-x86_64.c)**: MIR-level `MIR_T_LD` call
     args/results (the mir.ld2i / mir.ui2ld conversion BUILTINS — native,
     mingw-compiled) went through the SysV-shaped value path: LD arg →
     `get_arg_reg`=NON_VAR → stack slot, while the mingw callee expects a
     POINTER in the arg reg (`fld (%rcx)` with stale rcx — the t_ld_d crash
     and t_ld_e's wrong-value v=3, one root, two symptoms).  Fix at the
     convention layer, `#if defined(_WIN32) && !MIR_LD_IS_D`: LD args spill
     into the call's block area and pass the slot ADDRESS as a pointer arg;
     an LD RESULT reserves a 16-byte slot whose address rides a hidden FIRST
     arg (RCX, shifting real args — the gcc oracle's convention), read back
     via a surviving temp pseudo inserted to execute BEFORE the SP-restore
     (no red zone on Windows).  The native helpers themselves needed ZERO
     changes — mingw already compiles them with exactly this convention.
  5. **c2m driver import binding** (c2mir-driver.c): win32 `std_libs` was
     msvcrt-FIRST — JIT `printf` bound to msvcrt (`%Lf` = by-value double,
     plus a SECOND CRT in a UCRT process: two heaps/FILE tables).  Fix: on
     the mingw-UCRT host, std_libs = ucrtbase+kernel32 only, and the
     printf/scanf family resolves FIRST to the host's `__mingw_*` ANSI-stdio
     implementations (gcc parity: that is what a mingw-gcc-compiled binary
     calls).  Guard `__MINGW32__ && _UCRT` / `__USE_MINGW_ANSI_STDIO`.
  VERIFIED: t_ld_b `ld=3.25`, t_ld_c `t=6.50`, t_ld_d `a=325 b=650`,
  t_ld_e `v=325`, t_ld_f exact — on wine AND real Windows (build 26200)
  via win_run.sh; w2_core/features/varargs/cplx_full all green both ways;
  native SysV battery byte-identical (regression control); fulltest green.
  `long double _Complex` stays green.  Reducers: tmp/win/t_ld_{b..f}.c.
  Known out-of-scope: the INTERP ffi path (`-ei`) does not implement the
  win64 LD convention (gen `-eg` is the madc path); MIR-level LD args in
  hand-written protos to JIT-to-JIT calls remain SysV-shaped on the callee
  side (c2mir never emits them on win64 — blk instead).
- Also found (platform-independent, task #43) — **FIXED (session #85)**:
  prefix-position `__attribute__((cleanup))` was silently dropped.  The
  parser keeps specs-position N_ATTRs (flattened into the decl-specs
  list); the CHECK layer's cleanup scan read only the declarator-suffix
  attrs slot.  Fix: `scan_cleanup_attrs` helper run over BOTH positions
  (the same per-position pairing vector attrs already use); specs-position
  attrs apply to every declarator, per gcc.  Gate:
  `third_party/mir/c-tests/new/cleanup-prefix-attr.c` (+.expect, gcc
  oracle; multi-declarator, reverse order).  w2_cleanup.c now prints
  cleanup=7 on native, wine, and real Windows; the cleanup-* family and
  the LD/w2 battery stay green.  madc's tree path was never affected.
- c2mir type model under our windows target: long double = 80-bit
  (mingw model) — DONE, see 6 above; `sizeof(long double)==16`,
  `_Alignof==16` verified in-JIT under wine.
- **W2.1 RISK — PROBED AND ANSWERED (2026-08-12).** Confirmed: with
  mingw's default win64 `setjmp` (captures
  `__builtin_frame_address(0)`), a `longjmp` across a VirtualAlloc'd
  JIT frame with no RUNTIME_FUNCTION entry faults inside ntdll
  (`MSVCRT_longjmp → RtlUnwind → RtlUnwindEx → page fault`, wine 9.0).
  Mitigation VERIFIED: `-D__USE_MINGW_SETJMP_NON_SEH` maps setjmp to
  `_setjmp(buf, NULL)` — NULL frame-ctx means longjmp restores
  registers without invoking the unwinder at all — and the same probe
  passes cleanly. NO fork work needed; madc's exception runtime and
  win64 prelude MUST compile setjmp as the NON-SEH variant
  (`__intrinsic_setjmpex(buf, NULL)` under UCRT). Both legs are gated
  in `scripts/win_ucrt_gate.sh` (the SEH leg as a negative control
  that must keep failing). Real-ntdll confirmation rides the first
  win_battery once W0.2 is up; the NULL-ctx semantics are CRT-level,
  so no divergence is expected.

### W3 — PE/COFF writers (the genuinely new compiler work)
- **mir-pe.c**: PE64 executable writer behind the SAME `MIR_object`
  seam as the ELF writer and mir-macho.c (the AOT plan reserved this
  slot — "Mach-O/PE assemblers later behind the same MIR_object
  seam"). Import model: IAT + import descriptors for ucrtbase.dll,
  kernel32.dll, ws2_32.dll (and nothing else if statics hold);
  relocations; entry glue (mainCRTStartup contract when linking our
  own image vs carrying mingw's crt objects — DECIDE: carrying
  crt2.o/crtbegin from the mingw sysroot, like darwin rides the
  platform contract, is the likely faithful path).
- **COFF `.o` writer** for the `--obj` lane (+ the `.o`-as-cache
  loader parity).
- `-static-libmadc -o` parity: `madc -o prog.exe` produces a runnable,
  self-contained PE from a .mad/.c/.cpp source, C AND C++, like darwin.
- Emitted-C lane: `x86_64-w64-mingw32-gcc emitted.c -lmadc_rt` works —
  ship `libmadc_rt.a` cross-built (W3.5, mirrors darwin W3).

### W4 — Embedded prelude + groves (provenance-clean, W0.5 style)
- Windows C prelude = mingw-w64 UCRT headers (+ the mingw ANSI stdio
  routing for the long-double printf family). Provenance audit before
  ANY public artifact: mingw-w64 headers are predominantly
  public-domain/permissive (ZPL-ish), winpthreads BSD — document
  per-file like docs/licenses/NOTICE-darwin-prelude.txt; carry the
  notices in the artifact.
- C++ groves: libstdc++ headers from the mingw sysroot, packed into
  the forest as the windows/libstdc++ flavor — the FLAVOR key work
  (target-OS × stdlib) already exists from the darwin arc; this reuses
  the libstdc++ half, not the libc++ lane. GPL+runtime-exception
  notices carried as on Linux.
- Freeze-context discipline: the windows forest is built BY the cross
  pipeline and read by madc.exe — same freeze-context hash rules; the
  verify gate must run the EXACT shipped bytes (packaging re-verifies
  its inputs — the 2026-08-11 lesson is codified in
  package_release_macos.sh; the windows packer inherits it).

### W5 — Lanes, battery, artifacts
- Suite lanes on real Windows: `run_tests.sh` gains a generic runner
  prefix (fixture-convention rule: a `MADC_RUNNER=...` env wrapping
  the madc invocation — the W0.2 ssh channel or wine64 — never
  per-test branches); expect a windows skip-fixture family
  (`.win_skip`) for genuinely-host-bound tests, each one line of why.
- `scripts/win_battery.sh` for the in-vivo pass on the Windows 11
  host over the W0.2 channel (mac_battery.sh is the template — same
  stage-dir + per-leg rc discipline).
- Artifacts: `madc-<ver>-windows-x86_64.zip` (zip, not tar.gz — native
  extraction) with bin/madc.exe, lib/libmadc_rt.a, README-windows.txt
  (SmartScreen/unsigned-binary note — the quarantine analog), license
  notices; SHA256SUMS refresh discipline shared with the other
  packagers; `verify_pe_release.sh` gate (imports table = the decided
  set, forest carrier intact, no msvcrt.dll).
- Release: the lane ships as a develop release with all four Linux
  lanes green PLUS the wine suite lane; promote/GH-release artifact
  set grows to 5 (deb, rpm, mac×2, windows zip). CI automation comes
  AFTER this lane works (owner sequencing).

## Sequencing

W0 → W1 and W2 in parallel (W2.1 setjmp probe FIRST-week) → W3 →
W4 → W5. The lane's midpoint milestone is "cross-built madc.exe JITs
the suite on the real Windows host over the W0.2 channel"; the
endpoint is "zip artifact passes verify + the packed suite on
Windows + win_battery".

## Open questions (owner)

1. ~~In-vivo host~~ **RESOLVED 2026-08-12**: the Windows 11 box IS the
   machine hosting the build container's WSL environment. Remaining
   owner action: pick the W0.2 channel (recommended: enable the
   built-in OpenSSH Server on the host).
2. Code signing: ship unsigned (SmartScreen warning documented in the
   README, like the Mac quarantine note) — assumed YES for v1 of the
   lane.

## Non-goals (this lane)

- 32-bit Windows, AArch64-windows, MSVC-ABI interop (madc.exe is a
  mingw-world binary; C++ interop with MSVC-built objects is
  explicitly out), MSI/installer packaging, CI automation (next arc).
