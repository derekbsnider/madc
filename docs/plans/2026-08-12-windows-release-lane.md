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
- **fork/exec** (`madc_process.cpp`, exec:// channels): CreateProcessW
  + pipe pair; the channel pump contracts (one pump loop owner) stay.
- **mmap** (`cir_freeze.cpp` — forest packing): reads can fall back to
  buffered IO; if mapping stays, CreateFileMapping/MapViewOfFile behind
  the same seam.
- **pthreads**: covered by the `-posix` toolchain flavor (winpthreads);
  DBG's thread_local discipline unchanged.
- **sockets** (tcp:// channels, madcdis): winsock2 — WSAStartup once,
  closesocket vs close, SOCKET vs int fd. The channel layer's
  close-on-exec atomicity note becomes HANDLE inheritance flags.
- **`madc_posix_io.cpp`**: the deliberate POSIX-IO surface — audit
  what of it is script-facing API (must work via Win32 equivalents)
  vs host plumbing.
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
