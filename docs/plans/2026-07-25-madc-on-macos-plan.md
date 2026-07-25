# madc on macOS — Route 1 track plan (2026-07-25, owner-adopted)

## Goal

**A madc binary that runs natively on the owner's Macs** — madc-the-
compiler on macOS, not only madc targeting it.  The Mach-O/ARM64 track
(`2026-07-25-macho-arm64-plan.md`, complete: v0.43.0 axis A + v0.44.0
axis B) gave madc the ability to *emit* signed Apple binaries from
Linux; this track ports and cross-builds madc *itself* as one of those
binaries.

**ROUTE 1 ADOPTED (owner, 2026-07-25: "I prefer route 1, as
recommended"):** an osxcross-style cross-BUILD toolchain on Linux —
clang-18 + the owner's macOS SDK + ld64.lld — builds madc's C++
codebase for the Mac from the container.  Routes considered and the
adoption record live in the macho-arm64 plan's "larger goal" section.

## Laws and constraints (unchanged, restated for this track)

- **The no-external-toolchain law governs madc's PRODUCT path only** —
  what madc emits.  madc itself has always been built with g++/clang;
  Route 1 extends that build to a cross toolchain.  Nothing external
  enters what the shipped madc *does*: on the Mac, MIR remains the
  compiler/assembler/linker/signer for everything madc emits.
- **The macOS SDK is NEVER committed, synced, or redistributed.**
  Owner-extracted from their own Xcode (macOS 15.5 SDK).  Locations:
  container `/workspace/sdk/MacOSX.sdk` (extracted; tarball copy at
  container `/workspace/MacOSX.sdk.tar.xz`), NAS
  `/workspace/madc/tmp/MacOSX.sdk.tar.xz` (tmp/ is gitignored AND
  excluded from remote_build sync).
- The owner's Macs are **run-only** devices: no Xcode CLT on the test
  path, no system compilers, **no `/usr/include`** — the shipped madc
  must be self-sufficient (embedded headers).
- QNAP never builds/tests; the container does everything; binaries
  come back via the NAS.

## Inputs already in place (proven 2026-07-25, pre-track)

- **Toolchain proven end-to-end**: `clang++-18 -target
  {arm64,x86_64}-apple-macos12 --sysroot /workspace/sdk/MacOSX.sdk
  -fuse-ld=lld` built a real C++ program (iostream/string/vector
  against genuine libc++) into Mach-O PIEs for BOTH targets, first
  try (`tmp/r1gate-{a64,x64}` on the NAS; print + exit 27).  lld
  linker-signs ad-hoc on arm64 automatically.
- **Fallback toolchain**: real Apple ld64-956.6 + cctools-1030.6.3
  built for both `*-apple-darwin24` in container `/workspace/xtools`
  (use only if lld misbehaves; primary = pure LLVM).
- **MIR on Apple hosts is upstream-supported**: `mir-code-alloc-
  default.c` carries the Apple Silicon JIT arms (`MAP_JIT` mmap, W^X
  `mprotect` handling under `__APPLE__ && __aarch64__`); the fork
  adds nothing host-side.
- **`mir-target.h` needs ZERO knobs for this track**: with no
  override every `MIR_TARGET_*` macro follows host detection — a
  libmir compiled BY the darwin cross compiler detects
  `__APPLE__`/arch natively, `MIR_TARGET_APPLE_P` turns on by itself,
  and the v0.44.0 Mach-O writer becomes the *native* writer.  A
  madc-on-mac `-o app` emits signed Mach-O with no cross MODE at all.
- The v0.44.0 axis-B machinery (per-mode obj trees, per-mode lib
  archives, MIR variant-lib build rules) is the wiring template.

## Phasing — three phases, each independently shippable

The port splits on one line: **madc's own compiled code vs the script
lane's C++ runtime types.**  madc's C++ source compiles against libc++
transparently (r1gate proved real libc++ works).  But .mad scripts
using `std::string`/containers/streams resolve **mangled-direct
against the host C++ runtime** — on macOS that is libc++
(`std::__1::` inline namespace: different manglings, different object
layouts, e.g. 24-byte vs 32-byte `std::string`).  That is a
self-contained ABI-flavor project and phase-gating it keeps Phase 1
small and honest.

### Phase 1 — madc runs on the Macs, C lane complete  ← THIS TRACK'S GATE

Deliverable: `bin/madc-hosted-arm64-macos` + `bin/madc-hosted-x86-64-macos`
— full madc binaries (JIT **and** AOT; native target = the Mac itself),
cross-built from the container, ad-hoc signed, runnable on the run-only
Macs.  In scope: the whole C lane (.mad and pure C programs, embedded
headers, JIT execution, `-o` → signed Mach-O PIE, `-c`/merge, script
mode, exit-status parity).  Out of scope (→ Phase 2): .mad programs
using `std::string`/STL containers/iostreams.

### Phase 2 — libc++ STD-ABI flavor (script-lane C++ types)

`madc_mangle.cpp` is the single mangling authority (plus ~8 call sites
carrying `__cxx11` knowledge) — give it an ABI flavor switch:
`std::__cxx11::`/libstdc++ layouts on GNU hosts, `std::__1::`/libc++
layouts on Apple hosts.  Includes the object-size/layout table
(string/stream sizes used for stack reservation), `cout`/`cerr`/`cin`
global symbol names, and the real-header parsing story on a
header-less Mac (SDK-derived or embedded libc++ subset — owner
decision when we get there).  Own plan doc when the phase starts.

### Phase 3 — product shape on macOS

> **SUPERSEDED (2026-07-25):** Phase 3 is subsumed by the owner-directed
> forest-carriers track —
> [`2026-07-25-forest-carriers-plan.md`](2026-07-25-forest-carriers-plan.md)
> (Mach-O `__MADC,__forest` section via `-sectcreate` — no re-signer on
> the build path — in-house re-signer for post-link packing, configurable
> carriers/install shapes, `-static-libmadc`). The bullets below are the
> original sketch, kept for history.

- **Forest pack**: the appended-blob model is ILLEGAL on signed Mach-O
  (the file must end exactly at the signature — trailing bytes = AMFI
  kill on arm64).  Design: carry the forest as a real section
  (`__MADC,__forest` via `-sectcreate` at link or a post-link
  injector), read it back with the self-image section API instead of
  the trailer probe, and re-sign after any rewrite.
- **Re-signer**: strip and pack both invalidate signatures.  Build a
  tiny standalone ad-hoc signer from the fork's own CodeDirectory
  code (mir-macho.c already knows how) — keeps the flow in-house; no
  Apple codesign needed.  (Interim: lld's linker signature stands as
  long as we don't rewrite the binary after link.)
- libmadc.{a,dylib} for Mac hosts (`-dynamiclib -install_name`),
  madcdat re-enabled, packed-release parity with Linux.

## Phase 1 port-audit worklist (recon complete 2026-07-25)

Everything below is a **host-feature** switch (`#ifdef __APPLE__` at
compile time of madc itself) — the same category as gdb-JIT
registration in the fork.  Zero Linux behavior change; Linux fulltest
green is the refactor gate.

### A. Self-executable discovery — 6 sites, one helper

`/proc/self/exe` does not exist on macOS; the replacement is
`_NSGetExecutablePath()` (+ `realpath`).  Extract a named helper
(`madc_self_exe_path()`, helper-methods rule) and converge all sites:

| Site | Use |
|------|-----|
| `src/madc_cir.cpp:1355` | `cir_selfexe_libdir`: readlink → lib dir |
| `src/madc_cir.cpp:4313,4362` | forest bind container-path fallback |
| `src/madc.cpp:1024` | `--run-frozen` self path |
| `src/madc.cpp:1491` | `execv("/proc/self/exe")` for `--freeze-run` (resolved path works on both) |
| `src/cir_freeze.cpp:716` | appended-blob probe (Phase 1: probe reads the binary, finds no trailer magic in a signed Mach-O, falls through to live parse — the default-on fallback already handles this gracefully) |

### B. dlopen sonames (host side)

- `src/madc.cpp:824-826` (`"lib" + name + ".so"`) and `:1532`
  (`dflt_suffix = ".so"`): darwin arm uses `.dylib`.
- Target-side cover analysis (`madc_cir.cpp:1178,1382`) is already
  target-keyed (Apple targets = cover-analysis-only since v0.44.0) —
  untouched.

### C. /proc probes

- `src/madc_program.cpp:2008` `/proc/self/statm` → mach
  `task_info(MACH_TASK_BASIC_INFO)` arm.
- `src/madc_program.cpp:2000` `getrusage` — **`ru_maxrss` is BYTES on
  macOS vs KB on Linux**; normalize in the consumer.
- `src/madc_program.cpp:2944,3844` `/proc/self/fd/N` → `/dev/fd/N`
  (same semantics on darwin).

### D. Front-end host-header story

- `src/sys_include_paths.cpp` is generated from the BUILD host's g++ —
  wrong for a darwin binary, and the run-only Macs have no system
  headers anyway.  Darwin builds get a table that prefers the
  embedded headers and (if present) the standard CLT SDK path as an
  opportunistic tail — must degrade gracefully to embedded-only.
- `src/predefined_macros.cpp:451` predefines `_GNU_SOURCE` (glibc
  flavor) for target compiles: darwin-host flavor block predefines
  `__APPLE__`/`__MACH__` and drops the glibc-isms.

### E. Build wiring (src/Makefile)

- `LIBS = -rdynamic -ldl -lz -lm -lpthread`: darwin conditionals —
  no `-ldl` needed (libSystem; SDK carries a libdl.tbd reexport but
  don't rely on it), `-rdynamic` → ld64 exports executable globals by
  default (verify with the first build; explicit
  `-Wl,-export_dynamic` if not — the dlsym import resolver REQUIRES
  madc's own extern-C runtime + mangled publics visible via
  `dlsym(RTLD_DEFAULT)`).
- `-Wl,--whole-archive` → `-Wl,-force_load,<archive>` (per-archive
  syntax, not bracketing).
- `-shared -Wl,-soname` → `-dynamiclib -install_name` (Phase 3;
  Phase 1 builds no shared lib).
- New MODEs `hosted-arm64-macos` / `hosted-x86-64-macos` →
  `bin/madc-hosted-{arm64,x86-64}-macos`: full binaries (NOT the
  emit-only cross model — on these, host == target, JIT included).
  `CROSS_HOST_CXX = clang++-18 -target {arm64,x86_64}-apple-macos12
  --sysroot /workspace/sdk/MacOSX.sdk -fuse-ld=lld`.  Per-mode obj
  trees and per-mode lib archive names (axis-B precedent — never
  clobber host libmadc.a).
- libmir variant per host: `make -C /workspace/mir
  BUILD_DIR=build-hosted-<target> CC='clang-18 -target … --sysroot …'`
  — **no MIR_TARGET defines** (host detection through the darwin
  compiler is the correct configuration).
- `./configure --enable-madcdat=no` for the port (storage surfaces
  untouched); madcdat on macOS = Phase 3.

### F. Verified non-issues (checked, no action)

- No Linux-only syscall APIs anywhere in `src/` (no seccomp / epoll /
  eventfd / inotify / prctl / memfd).
- `<execinfo.h>` (backtrace, `madc.cpp:9`) exists on macOS.
- `open_memstream` (`cir_emit_c.cpp:148`) exists since macOS 10.13
  (minos 12).
- `getopt_long`, `sigaction`, `clock_gettime`, pthreads: all darwin.
- The `-c` object flavor stays the ELF-container dev vehicle on the
  Mac too (madc's own reader links it; MH_OBJECT is fork task #4,
  orthogonal).

## Gates

- **G1 (Linux, automated):** both hosted binaries build; Linux
  fulltest stays green after the port pass (zero host behavior
  change); `llvm-otool`/`llvm-readobj` validate the hosted binaries
  as well-formed signed Mach-O PIEs; `llvm-nm` shows the dlsym-needed
  exports (extern-C runtime + mangled namespace publics) in the
  export set.
- **G2 (owner, run-only Macs):** a curated self-test bundle — hello
  `.mad`, C-lane compute with exit code, `-o` AOT emit **and run the
  emitted binary**, JIT + script-mode parity — with one-line
  instructions; binaries + bundle on the NAS with exact paths.
  Verify item rides along: `MAP_JIT` under a plain ad-hoc
  (non-hardened) signature — expected fine (the JIT entitlement only
  gates hardened-runtime processes).

## Decided defaults (change only on owner directive)

- MODE/binary naming: `hosted-<target>` prefix distinguishes
  macOS-HOSTED madcs from the v0.44.0 emit-only crosses
  (`bin/madc-arm64-macos` = Linux-hosted cross compiler;
  `bin/madc-hosted-arm64-macos` = madc FOR the Mac).  On the Mac it
  installs as plain `madc`.
- Phase-1 deliverables ship **unstripped `-O2`** (release optimizer,
  no strip): stripping invalidates the lld signature and the
  re-signer is Phase 3.  Size delta vs a stripped binary is noted as
  a pending owner size-call for the product shape, not a Phase-1
  blocker.
- Phase-1 deliverables ship **unpacked** (no forest blob — live parse
  fallback is default behavior); packed forest on Mach-O is Phase 3.
- Primary toolchain = pure LLVM-18 (clang + ld64.lld); cctools/ld64
  in `/workspace/xtools` is the fallback only.
- minos 12.0, plain arm64 (no arm64e) — inherited from the Mach-O
  track.

## Phase 1 landing notes (2026-07-25 — what the build surfaced beyond the audit)

- **Fork: the object layer was host-gated on `<elf.h>`** — on a darwin
  host the ENTIRE MIR_object layer (including the Mach-O writer) would
  have compiled to "no <elf.h>" stubs.  Fixed at the root: new
  `mir-elf-defs.h` (local SysV gABI + psABI definitions, values
  verified against glibc; the mir-macho.c pattern), the
  `MIR_DEBUG_HAVE_ELF` gate and its stub block deleted.  The Windows
  track needs exactly this too.
- **Fork: `__muloti4` has no home on arm64-macos** — clang lowers
  `__builtin_mul_overflow(__int128)` to that libcall; darwin libSystem
  exports it for x86_64 only and Ubuntu's clang ships no darwin
  compiler-rt.  `MIR_int128_[u]muloti` gained an Apple arm with a
  manual checked multiply (inline mul + `__udivti3` wrap check — both
  exported on arm64); bit-exact with the builtin, overflow included.
- **Fork: the `__mir_*oti` asm-alias exports** are excluded on Apple
  (Mach-O has no aliases; the lane they serve has no Apple twin yet).
- **`mempcpy` is a GNU extension** — `__madc_builtin_mempcpy_chk`'s
  darwin arm composes `memcpy_chk` + `dst + n`.
- **errno accessor is per-libc**: glibc `__errno_location` vs darwin
  `__error` — registered under the host's own name (parser.cpp).
- **Cover analysis is HOST-keyed** (dlsym/dladdr run in-process): the
  base cover spellings on darwin are `libc++` / `libsystem_` /
  `libSystem` stems, not ELF sonames — without this every AOT emit on
  a Mac would be refused as "runtime-needing" (printf dladdr-reports
  libsystem_c.dylib, which never matches libc.so.6).
- **Real latent defect found by clang**: `TokenChar::is_constant()`
  lacked `const`, so it HID (not overrode) the base virtual — char
  literals reported non-constant through base pointers.  Fixed; plus
  85 mechanical `override` markers (headers now clang-warning-clean).
- **remote_build.sh deleted `*.d` on every mir sync** — each sync
  blinded make to fork header changes (stale objects). Excluded now.
- **dlsym export surface confirmed on darwin executables**: 2337
  globals exported with no `-rdynamic` (ld64 default) — the script
  lane's resolver requirement holds.  The namespace publics mangle
  with `std::__1` (libc++), confirming the Phase-2 delta.
- Deliverable sizes: ~6.0 MB unstripped `-O2` per binary.

## G2 round-2 fix slice — the darwin embedded C prelude (2026-07-25/26)

G2 root cause (hardware-proven on the A64 laptop): hosted binaries
shipped with NO darwin header story — `<stdio.h>` resolved into the
libc++ `c++/v1` wrapper maze (or nothing), printf stayed undeclared,
and the dlsym variadic-fallback convention collides with the Apple
arm64 stack-varargs ABI (x86-64 immune by register-ABI coincidence).

**Design — flattened umbrella, embedded TEXT (not .madh):**

- `scripts/gen_darwin_prelude.sh`: clang-18 `-target … --sysroot SDK
  -x c -std=c11 -E -dD -P` flattens ONE umbrella TU over the hosted
  C standard/POSIX header set. `-dD` keeps every `#define` as text so
  madc's OWN preprocessor installs `EOF`, `NULL`, `stdin=__stdinp`, …
  at include time — a `.madh` token stream structurally CANNOT carry
  macro state (`#define` is consumed at lex time), which is also why
  the old baked-PCH layer was emptied. `-D_Nullable=`-family scrubs
  the clang nullability keywords. The script refuses to install a
  prelude that lacks `printf` (fail-loud beats silent degradation).
- One umbrella + one-line stubs (`stdio.h` → `#include
  <__madc_darwin_prelude.h>`) instead of per-header flattening:
  independent flattening duplicates shared sub-headers, and madc
  accepts duplicate identical typedefs but correctly rejects duplicate
  struct definitions (`struct timespec` is in several closures). The
  name-level once-only dedup makes the second standard include free.
- `gen_embedded_headers.sh` gained an extra-root + outfile mode; the
  hosted MODEs generate a PER-MODE `embedded_headers.cpp` in the obj
  tree (SDK-derived content never reaches committed files) and compile
  it via an explicit rule — a failed generation stops the build.
- Embedded resolution already precedes the filesystem walk in both the
  .mad and .c lanes, so the `c++/v1` maze is unreachable for covered
  names. The freestanding six stay on the committed `include/madc/`
  set; the umbrella covers the hosted set (stdio/stdlib/…/sys/*).

**Deep-layer fixes the prelude shook out (all target-independent):**

- Function-pointer params: `*` and cv-qualifiers now interleave
  (`char const * *`, `char * restrict` — Apple `_RuneLocale`).
- `#pragma pack`: full GCC semantics (`pack(N)` set, `pack()` reset,
  `push` saves current, `pop` restores) AND a real architecture fix —
  madc tokenizes the whole file before parsing, so the old lex-time
  pack state was STALE at parse time (every balanced push/pop region
  silently lost its packing; proven by probe). Pack events now ride a
  side channel pinned to the next real token and are applied one-shot
  in `nextToken()` — invisible to peekToken/scanners, hot path guarded
  by `empty()`. Known gap: pack events do not survive `.madh`/forest
  serialization (pre-existing class; the prelude tokenizes live).
- Apple-target `__asm("_open")` labels: leading underscore stripped at
  `consume_gnu_asm_label` — madc's canonical symbol space is the
  C/dlsym name (darwin dlsym takes no underscore; the Mach-O writer
  re-prepends it). Fixes hosted JIT dlsym AND cross/hosted AOT binds
  for the labeled POSIX family (`open`, `fopen`, `kill`, …).
  `MADC_TARGET_APPLE_P` moved to `datadef.h` (single owner).
- `_Float16` registered via the `_FloatN` nearest-supported precedent
  (→ float): the macOS 15 SDK declares `__fabsf16` & co UNGUARDED.
- Explicit `#include` defers to the auto-include prelude ONLY when a
  named provider (PCH/embedded) can serve it (shared
  `named_include_provider_exists()`); otherwise the direct filesystem
  path with its loud open-failure — gcc canon: explicit includes
  resolve or error. An uncovered include on a header-less Mac now
  fails loudly instead of silently producing garble.

## Risks

- **dlsym export visibility on darwin executables** — the entire
  script-lane import resolver hangs on it; G1 checks it structurally
  (`llvm-nm`), G2 proves it live.  Mitigation: `-Wl,-export_dynamic`.
- **MAP_JIT + ad-hoc signature interaction** on Apple Silicon — the
  documented rule says non-hardened processes don't need the JIT
  entitlement, but only G2 proves it on real hardware.
- **libc++ leakage into Phase 1**: any .mad test in the bundle that
  quietly touches `std::` types belongs to Phase 2 — the bundle must
  be curated C-lane-only, or the gate mixes phases.
- Cross-built binaries cannot run in the container (no darwin
  userland emulation) — G1 is structural only; behavioral truth
  arrives at G2.  Keep the bundle small and decisive.
