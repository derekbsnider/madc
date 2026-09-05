# The darwin-host build port — madc building (and testing) itself ON macOS

Owner direction 2026-09-01, stated three times during the PK5 wave:
GitHub's hosted macOS runners exist in BOTH arches (arm64 + Intel), and
"not only can they do the builds, they can also run all the test
suites." This arc makes madc a darwin-HOST toolchain — building itself
natively on a Mac — which is the single prerequisite behind four
prizes:

1. **CI mac builds** with the runner's legitimately licensed Xcode SDK
   (the owner-staged SDK never leaves owner hardware; that is why PK5's
   workflow ships linux+windows only).
2. **The full-suite macOS lane** — it has NEVER existed anywhere: the
   container cannot execute darwin, and the owner's Mac runs only the
   11-leg `mac_battery` (8/3). A native madc on a mac runner can run
   the entire `run_tests.sh` battery on real darwin.
3. **The first x86_64 execution proof** — nothing we own can run the
   x86-64 tarball today (container is linux, owner's Mac is arm64);
   an Intel mac runner is that venue.
4. **PK3-mac-runtime rides along** — the `libmadc-0.dylib` engine twin
   (whole-archive recipe, `@rpath` binding) is SIMPLER to build and
   verify natively than cross, and it unlocks madcide in the mac
   tarball (darwin `-o` currently refuses runtime-needing programs).

This plan supersedes the packaging-arc PK5 bullet's "macos-14 +
macos-13 build jobs" as written (that wording predates the measured
SDK/cross facts).

## Measured foundation (what is already proven — do not re-derive)

- **Source portability is a solved problem.** The cross lane compiles
  EVERY madc TU for darwin with clang-18 daily (`hosted-*-macos`
  MODEs), links with lld, and the products pass `mac_battery` on real
  hardware. The port is about BUILD MACHINERY, not source.
- **MIR needs nothing.** The darwin-HOST libmir variant already exists
  in the Makefile ("no MIR_TARGET knobs: host detection through the
  darwin compiler turns MIR_TARGET_APPLE_P on natively").
- **`madc -o` works on darwin** (mac_battery leg 6d; C++-world AOT
  leg green) — the suite's EXE lane has a foundation.
- **The darwin forest carrier is a Mach-O SECTION**
  (`-Wl,-sectcreate,__MADC,__forest`, standalone `forest.bin` built
  BEFORE link — appended blobs are illegal on signed Mach-O). Today
  the freezer is the Linux-hosted CROSS madc (`forest_pack_darwin.sh`).
- **The served C++ world is LLVM's libc++ headers**
  (`LLVM_LIBCXX_INC ?= /usr/lib/llvm-18/include/c++/v1`) — a
  provenance + parity PIN (Apache-2.0 text, identical to the
  `-stdlib=libc++` lane's zero-failure corpus). A darwin build host
  must serve the SAME 18.x header text (brew `llvm@18`), or groves
  diverge from the container baseline.
- **Makefile knob inventory** (recon 2026-09-01): hard `=` spellings
  needing parameterization: `CC/CXX_BASE = clang{,++}-18 -target …
  --sysroot $(MACOS_SDK)`, `AR = llvm-ar-18`,
  `HOSTTAB_CXX = clang++-18 …`, `-fuse-ld=lld` in the mode's `LIBS`.
  Already `?=`-overridable: `MACOS_SDK`, `DARWIN_PRELUDE_SYSROOT`,
  `DARWIN_ZSTD_DIR`, `LLVM_LIBCXX_INC`, `MACOS_MINOS`. GNU make
  command-line overrides beat even hard `=` assignments, so a probe
  can drive the existing MODE natively with zero repo edits (D0).
- **The linux default MODE is ELF-shaped**
  (`LIBS = -rdynamic -ldl …`) and stays that way: on a darwin host the
  build IS the `hosted-*-macos` MODE with a native toolchain posture —
  never a second ported "default" path (no-parallel-implementations).
- Script portability: build-critical gen scripts scanned clean of the
  obvious GNU-isms; `verify_macho_release.sh` carries one (`stat -c
  %s`; BSD spells `stat -f %z`). The posture decision below makes the
  class moot for build hosts.

## Architecture decisions

- **Darwin host ⇒ the `hosted-*-macos` MODE, native toolchain.** One
  MODE serves cross (container) and native (mac) — the toolchain
  spellings become variables with the current cross values as
  defaults. Product names stay identical
  (`bin/madc-release-{arch}-macos`, `obj/hosted-*/forest.bin`), so
  `verify_macho_release.sh` and `package_release_macos.sh` consume
  either origin unchanged.
- **Native freeze = SELF-freeze.** On a mac, the built hosted binary
  freezes its own standalone `forest.bin` (run natively), then the
  link embeds it — producer == consumer EXACTLY (stronger than the
  cross model's same-tree/same-keys argument). The cross-freezer
  remains the container's path; the freeze step gains a "runner =
  native" arm, selected by whether the build host can execute the
  target (uname-based, not hardcoded).
- **Native linker = Apple ld64** (ad-hoc signs arm64 automatically,
  handles `-sectcreate` natively); lld stays the cross linker. The
  probe measures whether brew `llvm@18`'s clang driver + native ld64
  compose cleanly.
- **Compiler = brew `llvm@18` clang, not Apple clang**, for exact
  version parity with the container's clang-18 (Apple clang is a
  differently-versioned fork; the groves and warning walls are
  baselined on LLVM 18). Apple's SDK + ld64 still come from Xcode via
  `xcrun`. D0 verifies availability/spellings.
- **Build-host userland = brew gnubin PATH-first** (bash, coreutils,
  gnu-sed, grep, gmake) on darwin BUILD hosts — collapses the BSD/
  bash-3.2 portability class for build-time scripts to ~zero. Runtime
  scripts that must run on ANY Mac (`mac_battery`) stay 3.2-clean as
  today.
- **zstd**: per-target static stage, same v1.5.5 recipe as the
  container, built natively on the mac host (brew zstd acceptable for
  probes; the shipped artifact's stage stays the pinned source build).

## Slices

- **D0 — measurement (probe workflow, no repo changes).**
  `.github/workflows/darwin-probe.yml` (dispatch-only, macos-14):
  inventory (SDK path, clang/ld versions, brew `llvm@18` layout),
  `fetch_darwin_open_headers.sh` portability, `autoreconf +
  configure` with brew prefixes, then `make hosted-arm64-macos` driven
  entirely by command-line overrides — the failure catalog IS the
  deliverable, uploaded as an artifact. Expected first walls: parse-
  time generator scripts under mac sh; the freeze step (no cross madc
  on the runner — D2's job).
  **Owner-Mac baseline (MEASURED 2026-09-01):** macOS 15.3.2 arm64,
  Command Line Tools ONLY (no Xcode.app) — SDK at
  `/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk`, which is
  EXACTLY the path `MADC_SYS_INCLUDE_PREFIX_MAP` already bakes as
  canonical (the cross-built include tables match a CLT-only consumer
  box by existing design); the SDK carries its own libc++ headers
  (`usr/include/c++/v1` present). Apple clang 17.0.0 (clang-1700),
  ld64 `ld-1167.5`. NO brew, NO autoconf, bash 3.2.57, and
  `/usr/bin/make` is **GNU Make 3.81 (2006)** — a hard wall: building
  needs brew's gmake (≥ 4) like the rest of the gnubin posture.
  `shasum`/`git`/`python3` present (the fetch script's needs).
  Conclusions: CLT alone suffices to RUN madc, never to BUILD it —
  brew (`llvm@18 zstd autoconf coreutils gnu-sed grep bash make`) is
  the documented build-host prerequisite on any Mac (runner images
  ship brew already); Apple clang 17 confirms the llvm@18-for-parity
  compiler decision.
  **✅ D0 COMPLETE (runner probe rounds 1–4, 2026-09-01) — D1's gate
  was met BY THE PROBE with zero repo changes.** Run 33571011884:
  `make exit 0`, `bin/madc-hosted-arm64-macos` (9.2 MB) linked by
  brew clang-18's driver + Apple ld64 (`-force_load` archive + darwin-host
  libmir) and RAN the hello natively (`darwin-host-probe-alive`); the
  whole hosted build took **~56 s** on macos-14 at `-j3`. Facts:
  - Runner: macOS 14.8.7, Xcode 15.4 SDK, Apple clang 15, ld-1053.12,
    bash 3.2, `/usr/bin/make` = GNU Make 3.81 (same walls as the owner
    Mac); brew `llvm@18` = clang 18.1.8 with libc++ headers.
  - `fetch_darwin_open_headers.sh` runs clean under the mac shell.
  - ALL 53 hosted TUs compile under `-Wall -Werror`; `HOSTTAB_GEN`
    (sys_include_paths/predefined_macros) generates on darwin.
  - Walls found, each already knob-shaped: brew autoreconf needs
    `automake`; `gen_darwin_prelude.sh` reads `$CLANG` (default
    clang-18); GNU Make 3.81 loses MIR's `c2mir/` obj dir (gmake 4.4.1
    via gnubin fixes it); the hosted link's freezer dependency is the
    CROSS madc (`CROSS_MADC_BIN`), sidestepped for the probe with
    `--with-forest=none` — that dependency IS D2.
  - The working override set (= D1's knob list): `MACOS_SDK=$(xcrun
    --show-sdk-path)`; `CC/CXX_BASE/AR` = `$LLVM/bin/{clang,clang++,
    llvm-ar}` with `-target arm64-apple-macos12 --sysroot $SDK`;
    `HOSTTAB_CXX` likewise + `-nostdinc++ -isystem $LLVM/include/c++/v1`;
    `LLVM_LIBCXX_INC=$LLVM/include/c++/v1`; `DARWIN_PRELUDE_SYSROOT` =
    the fetched open headers; `DARWIN_ZSTD_DIR` = a stage dir with
    `lib/zstd.h` + `libzstd-arm64-macos.a`; `LIBS=-lz -lm <zstd.a>`
    (drops `-fuse-ld=lld` → ld64); env `CLANG=$LLVM/bin/clang`; PATH =
    gnubin of `make coreutils gnu-sed grep`; configure
    `--with-forest=none --enable-madcdat=no` with brew CPPFLAGS/LDFLAGS.
  Consequence: D1 shrinks to KNOB PROMOTION (one `DARWIN_HOST` posture
  deriving all of the above, defaults = today's cross spellings so the
  container is byte-identical); D2 (native self-freeze) is the real
  remaining engineering.

- **D1 — the Makefile DARWIN_HOST posture.** Promote the D0 override
  set into named `?=` knobs (defaults = today's cross spellings, so the
  container path is byte-identical); host-table generation proven on
  darwin. Gate: `hosted-arm64-macos` compiles + links (pre-forest) on
  macos-14 and runs a hello natively.
  **✅ D1 EXECUTED (2026-09-01).** `src/Makefile` now carries ONE
  `DARWIN_HOST` posture (auto-on when `uname -s` = Darwin, or
  `DARWIN_HOST=1`) that derives the D0 override set:
  `DARWIN_LLVM_PREFIX` (`brew --prefix llvm@18`) → `DARWIN_CLANG` /
  `DARWIN_CLANGXX` / `DARWIN_AR` / `DARWIN_STRIP` / `LLVM_LIBCXX_INC`;
  `MACOS_SDK` (`xcrun --show-sdk-path`); `DARWIN_LD_FLAGS` empty (Apple
  ld64 — `-fuse-ld=lld` is the cross value); `DARWIN_ZSTD_PREFIX`
  (`brew --prefix zstd`) → `DARWIN_ZSTD_INC` / `DARWIN_ZSTD_LIB` (the
  container's `DARWIN_ZSTD_DIR` stage layout derives the same pair);
  `DARWIN_PRELUDE_SYSROOT` reads the fetch script's own
  `DARWIN_OPEN_HEADERS_HOME` knob. The hosted MODE's `CC`/`CXX_BASE`/
  `AR`/`LIBS`, `HOSTTAB_CXX`, the prelude generator's `CLANG`, and
  `release-macos`'s strip all spell through the knobs. Two loud walls:
  a darwin host under GNU make 3.x is refused at parse time (the
  measured MIR wall), and a hosted MODE whose compiler is not on PATH
  is refused with the install line (the generators' silent-failure
  class). Shell-derived defaults use the `$(origin)` guard (a `?=`
  would re-run brew/xcrun on every expansion).
  **Oracle (container = cross posture, byte-identical):** `make -n -B`
  recipe text IDENTICAL before/after for `hosted-arm64-macos` (317
  lines incl. the cross-arm64 recursion, MIR sub-make, forest pack,
  link), default (165), `hosted-x86-64-windows` (162), `release` (162),
  `debug` (162). Forced native posture on the container
  (`DARWIN_HOST=1 DARWIN_LLVM_PREFIX=/usr/lib/llvm-18
  DARWIN_ZSTD_PREFIX=/usr`) derives `/usr/lib/llvm-18/bin/{clang,
  clang++,llvm-ar}`, `-I/usr/include`, `/usr/lib/libzstd.a`, no
  `-fuse-ld` — the knob wiring, exercised without a Mac. Negative
  control: `DARWIN_CLANG=/nonexistent/clang` → `Makefile:321: ***
  hosted darwin MODE: compiler ... not found`. Then `release-macos`
  REBUILT from clean mode objdirs on the container: both arches 835
  units, pack parse errors 64 (baseline 64), `verify_macho_release`
  OK ×2, tarballs packaged (`dist/madc-0.97.0-macos-{arm64,x86_64}
  .tar.gz`).
  **Workflow:** `darwin-probe.yml` shrank to the D1 gate — brew set +
  gnubin on `GITHUB_PATH`, `DARWIN_OPEN_HEADERS_HOME` in `GITHUB_ENV`,
  `autoreconf; ./configure --with-forest=none --enable-madcdat=no`,
  a `make -pn` step echoing the derived posture as evidence, then
  plain `make -C src -j3 hosted-arm64-macos` and a hello that must
  print — no overrides; the job FAILS when the build or the run does.
  Owner dispatches it (`gh workflow run darwin-probe.yml -R
  derekbsnider/madc --ref develop`). **✅ Run 33573025122 GREEN on
  macos-14:** the posture echoed `DARWIN_HOST = 1`, `DARWIN_LLVM_PREFIX :=
  /opt/homebrew/opt/llvm@18`, `DARWIN_ZSTD_PREFIX := /opt/homebrew/opt/
  zstd`, `MACOS_SDK := /Applications/Xcode_15.4.app/.../MacOSX.sdk`,
  `DARWIN_LD_FLAGS =` (empty → ld64), gnubin `GNU Make 4.4.1`; plain
  `make -C src -j3 hosted-arm64-macos` built the 9.2 MB binary; the hello
  printed `darwin-host-gate-alive`; `madc 0.97.0`. D1's gate is met on
  the runner with the repo's own Makefile — no overrides anywhere.
  **Residual → D2:** `--with-forest=none` stays until the native
  self-freeze lands; the brew zstd default is probe-grade (the shipped
  stage stays the pinned v1.5.5 source build); on a darwin host the
  bare default goal (`make -C src`) is still the ELF-shaped linux
  MODE — whether it should redirect to `hosted-$(uname -m)-macos` is
  D2's call together with `release-macos`'s host-`bin/madc` read-back.
- **D2 — native self-freeze + verification.** The freeze arm that
  runs the built binary natively; `verify_macho_release.sh`
  portability (`stat -c`); native zstd stage. Gate: packed
  runner-built binary passes `verify_macho_release` AND its forest
  unit count matches the container cross-pack baseline; `mac_battery`
  green (leg-for-leg vs owner-hardware baseline) on the runner.
  **✅ D2 IMPLEMENTED (2026-09-02) — awaiting the runner gate.**
  `src/Makefile`: the FREEZER is chosen per origin — `DARWIN_FREEZER`
  = `$(CROSS_MADC_BIN)` on the container (unchanged recursive cross
  make), = `$(OBJDIR)/madc-freezer` when `DARWIN_SELF_FREEZE=1`
  (auto: `DARWIN_HOST=1` and `HOST_DARWIN_ARCH` == `DARWIN_ARCH`).
  Self-freeze = phase-1 link of the SAME objects with no forest section
  (never a product) → `forest_pack_darwin.sh` runs it to write the
  standalone `forest.bin` → the ordinary `-sectcreate` link. Producer
  == consumer by identity. A cross-arch hosted MODE on a darwin host
  refuses at parse time unless `DARWIN_FREEZER=` names a same-arch
  madc of this tree or `--with-forest=none`. `release-%-macos` is the
  ONE per-arch strip+verify rule for both origins (reader
  `MACHO_READER` = `bin/madc` on the container, the unstripped hosted
  binary on a Mac; `OTOOL=$(DARWIN_OTOOL)`); `release-macos` composes
  it — both arches sequentially on the container, the host arch on a
  Mac (no `$(MAKE)` of the ELF-shaped default there). Scripts:
  `verify_macho_release.sh` (`wc -c` for the size, `MADC_READER`,
  OTOOL fallback `llvm-otool-18` → `llvm-otool`, GNU `timeout`/
  `gtimeout` resolution) and `forest_pack_darwin.sh` (freezer-agnostic
  prose, the same timeout resolution). Workflow: `darwin-probe.yml` is
  the RELEASE gate on a matrix of `macos-14` (arm64) + `macos-15-intel`
  (x86_64 — the first x86_64 execution proof): `make release-macos`,
  freeze/verify evidence + unit count, the STRIPPED binary's hello,
  then `mac_battery.sh` against a tarball-shaped stage (`bin/madc` +
  `lib/libmadc_rt.a`) with the owner-hardware PASS floor of 8 and every
  FAIL line printed for the leg-for-leg compare.
  **Container oracle:** `make -n -B` text identical (modulo the MIR
  `-DGITCOMMIT=` define, which tracks HEAD) for hosted-arm64-macos
  incl. the cross recursion, default, hosted-x86-64-windows; forced
  self-freeze posture shows the freezer link → freezer-driven pack →
  `-sectcreate` link and NO cross recursion; cross-arch negative
  control refuses at `Makefile:634`, `WITH_FOREST=none` lifts it;
  `release-macos` REBUILT through the per-arch rule: both arches 835
  units, pack parse errors 64 (baseline 64), 55/55 listed headers
  present, `verify_macho_release` OK ×2 through `MADC_READER=bin/madc
  OTOOL=llvm-otool-18`, tarballs packaged + pulled.
  **Residuals:** brew zstd stays the darwin-host default (the shipped
  stage's pinned v1.5.5 build joins D3's release job, like the windows
  job's stage); `macho_exe_dylib_gate.sh` SKIPs on a Mac (it wants the
  cross madc + `llvm-*-18` names) — parametrize with D3; the darwin
  default goal (`make -C src` on a Mac = the ELF MODE) is still open —
  `release-macos`/`hosted-*` are the darwin entry points for now.
  **Runner gate round 1 (run 33574153379, 2026-09-02) — the self-freeze
  WORKS on both arches, and the header-text drift the plan predicted is
  now MEASURED.** Both legs (macos-14 arm64, macos-15-intel x86_64):
  `make release-macos` GREEN — phase-1 freezer linked, the hosted binary
  froze its own groves, `-sectcreate` link, strip, `verify_macho_release`
  OK; the STRIPPED binary ran the hello on x86_64 — **the first x86_64
  execution proof anywhere**. But the frozen corpus was brew llvm@18's
  libc++ **18.1.8** text, not the container's apt **18.1.3**: 837 units /
  54 pack parse errors vs the 835 / 64 baseline (a different error SET,
  not just fewer), and `mac_battery` fell to 5/6 passes: the groves
  reference `basic_string::__align_it` (18.1.8's static member function
  template — madc emitted it as an EXTERN call: undefined MIR import in
  `cout << "hi"`, `call to undeclared function` in emitted C, "not on the
  AOT ledger" in `-static-libmadc`), `ostringstream::str()` came back
  "Unidentified member", and the include-free `value` intrinsic failed
  c2mir's check (`invalid operand types of +`) with the exec:// channel
  leg printing nothing on arm64 — the last two not yet attributed (they
  are re-measured under the pin before any chase).
  **Decision: header text is a pinned INPUT.** `scripts/fetch_libcxx_headers.sh`
  stages the container's exact package (`libc++-18-dev 1:18.1.3-1ubuntu1`
  amd64 deb from the Ubuntu pool, sha256-pinned, 1017 files, `diff -r`
  EMPTY against the container's installed tree, idempotent, bad-sha
  refuses); the Makefile's darwin-host `LLVM_LIBCXX_INC` default reads
  its `LIBCXX_HEADERS_HOME` knob, and a hosted MODE whose served headers
  are missing refuses at parse time (clang is SILENT about a missing
  `-isystem`). The 18.1.8 corpus stays an explicit opt-in
  (`LLVM_LIBCXX_INC=$(brew --prefix llvm@18)/include/c++/v1`) — it is a
  CONFORMANCE burndown, two classes found: KG Gaps
  `libcxx_18_1_8_align_it_extern_call` and
  `libcxx_18_1_8_ostringstream_str_lookup` (fix-what-you-find: owned,
  scheduled right after the port's gate is green, not "eventually").
  Bumping the pin is a served-C++-world change: container apt + this
  script move together, then the pack-degradation baseline and the libc++
  lane re-baseline. Round 2 = the same gate under the pin; expected 835 /
  64 and the battery leg-for-leg at the owner-hardware 8/3.
  **✅ Round 2 (run 33575170136) GREEN on BOTH arches — D2's gate is
  met.** Under the pinned text the `__align_it` class vanished (the
  iostream, emitted-C-runtime and native-AOT legs pass), and the battery
  is **leg-for-leg parity with owner hardware**: arm64 8/3 with EXACTLY
  the three standing known-opens (`os.str()` husk, value intrinsic,
  exec:// channel — docs/test-status.md names them); x86_64 **9/2** (its
  exec:// leg passes) — the first x86_64 battery ever run. Correction:
  the `ostringstream::str()` finding is the standing known-open, NOT an
  18.1.8 artefact (KG gap re-pointed); `__align_it` IS 18.1.8-only.
  **Open: corpus parity is close, not exact** — 836 units / 54 pack
  parse errors on the runner vs the container's 835 / 64 on IDENTICAL
  libc++ text. The class breakdown differs in exactly one line:
  `__atomic/atomic_base.h:32 use of undeclared identifier
  '__atomic_is_lock_free'` ×10 on the container, 0 on the runner (a
  compiler builtin behind libc++'s `_LIBCPP_HAS_GCC_ATOMIC_IMP` /
  `_LIBCPP_HAS_C_ATOMIC_IMP` selection at `__config:1183`), plus one
  extra unit. So the native freezer took a different preprocessor path
  or opened one header the cross freezer cannot — candidates: the
  predefined-macro table (captured by brew clang 18.1.8 vs apt 18.1.3),
  the prelude flattening (same zig pin, different capture clang's
  resource headers), or a real SDK header via the CLT canonical path if
  it exists on the runner (a corpus-parity AND open-provenance risk).
  Round 3 uploads the freeze inputs/outputs (unit dump, pack log, TU,
  host tables, prelude manifest) for the diff against the container.
  **✅ Round 3 (run 33575773604) GREEN both arches; the gap is EXPLAINED
  and closed (D2c).** Diffing the runner's freeze evidence against the
  container's: all 835 shared units have IDENTICAL token counts — the
  frozen text was already the same corpus. The differences were three
  HOST LEAKS into the served world plus one cross-freezer artifact:
  1. **+1 unit** = the runner's real
     `/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/
     sys/_types/_mbstate_t.h` — libc++'s `__has_include` probe answered
     by the host SDK (the CLT path EXISTS on GH mac runners): a corpus
     AND open-provenance leak (one Apple header frozen into a shipped
     forest). Fix: `--no-sysroot-includes`
     (`RegistrationPolicy::enable_sysroot_includes`, cut at the ONE table
     view `Program::sys_include_paths()` returns, so `#include`,
     `__has_include`, `#include_next` and system-header classification
     agree by construction): toolchain roots only — the C++ stdlib dirs
     and the compiler-owned dir — never the host's C library / SDK.
     `forest_pack_darwin.sh` passes it (no-op on the container, where
     those roots do not exist). Reducer pair: `tests/testsysrootinc`
     (positive control) + `tests/testnosysrootinc` (`.flags`,
     `.expect_err`). On linux madc's embedded C headers are a SUPPLEMENT
     (glibc's header resolves), so the pair stays include-free apart from
     the sysroot-only `<sys/sysinfo.h>`.
  2. **`__VERSION__`** was the only predefined-macro difference
     ("Ubuntu Clang 18.1.3 (1ubuntu1)" vs "Homebrew Clang 18.1.8"): the
     capture compiler's identity leaking through the GCC posture into the
     table AND the flattened prelude. Fix in `gcc_posture_filter.sh` (the
     ONE posture filter): `__VERSION__` = the bare posture version, which
     is exactly GCC's spelling (`gcc -dM -E`: `"13.3.0"`).
  3. **Prelude fortify drift**: Apple's cdefs turns `_FORTIFY_SOURCE` on
     for clang, and the `secure/` wrapper SET flattened into the prelude
     depends on the capture compiler's builtin inventory
     (`__has_builtin(__builtin___strlcpy_chk)`: yes on 18.1.8, no on
     18.1.3 — six lines of drift). Fix: `gen_darwin_prelude.sh` flattens
     at `-D_FORTIFY_SOURCE=0`; the prelude is a function of the pinned
     headers alone, and the served C surface now matches linux/win64
     (neither fortified). Container prelude: −74 lines (the whole
     `__builtin___*_chk` block, 36 macros), +`__VERSION__ "13.3.0"`.
  4. **The container's ten `__atomic_is_lock_free` errors are the CROSS
     freezer's own**: its undeclared-identifier dlsym fallback probes the
     BUILD HOST's symbols (Linux: no libatomic loaded), where libSystem
     has it — ten `__atomic_base<T>::is_lock_free` bodies are error husks
     in every container-frozen darwin forest and resolve in a natively
     frozen one, exactly as a Mac consumer's live parse would. KG Gap
     `cross_freeze_host_dlsym_leak`; the pack-degradation baseline
     comment records native 54 vs cross 64 (the baseline stays the cross
     number while the container lane is canonical). Clang's resource
     headers (stdint/stddef/stdarg/limits/stdatomic) are byte-identical
     between 18.1.3 and 18.1.8 — not a drift source.
  **Container oracle for D2c:** `release-macos` rebuilt: both arches 835
  units, 64 (== 64) pack parse errors, `verify_macho_release` OK ×2;
  reducer pair + `testinclude*`/`testhasinclude*`/`testposixcompat*`
  8/8 JIT, exe 1/1, obj 1/1. Round 4 expectation: 835 units, 54 errors,
  prelude + predefined table byte-identical to the container's, battery
  8/3 (arm64) / 9/2 (x86_64).
  **✅ Round 4 (run 33586189982, D2c content) — D2 CLOSED with EXACT
  parity on both arches:** flattened prelude byte-IDENTICAL to the
  container's, predefined-macro table IDENTICAL (499 arm64 / 469 x86_64
  entries), unit list + per-unit token counts IDENTICAL (835 units, no
  SDK unit), 54 pack parse errors (the container's 64 minus the cross
  freezer's ten dlsym husks — the native number is the faithful one),
  `verify_macho_release` OK, stripped hello on both. Battery **9/2 on
  BOTH arches**: the exec:// channel leg that failed on arm64 in rounds
  1–3 (and is one of the three owner-hardware standing known-opens) now
  PASSES — the fortify wrappers the prelude used to carry were breaking
  the channel child's output on arm64; `-D_FORTIFY_SOURCE=0` removed
  them. Owner-hardware confirmation pending (Mac unreachable); the
  remaining known-opens are the `os.str()` husk and the value intrinsic.
  The runner-built binaries are now release-equivalent to the container's
  by construction: same inputs (pinned libc++ text, pinned open C headers,
  posture-filtered tables), same corpus, producer == consumer.
- **D3 — CI mac jobs in release.yml.** macos-14 (arm64) +
  macos-15-intel (x86_64; GitHub retired macos-13 — the label the D2
  gate proved) build/package/attach with a PK4-style extract-and-run
  install gate for the tarballs; `/promote` sheds its mac-attach step;
  asset ownership becomes CI for all six. OWNER DECISION at this exit:
  whether the container cross lane remains canonical for local
  ceremonies or becomes verification-only.
  **IMPLEMENTED 2026-09-02 (burn-in pending):**
  - `release.yml` gains `macos-package`, a two-leg matrix mirroring
    darwin-probe's provisioning (brew set + gnubin on GITHUB_PATH; the
    three staged INPUT trees at their scripts' knobs via GITHUB_ENV —
    `DARWIN_OPEN_HEADERS_HOME`, `LIBCXX_HEADERS_HOME`, and the new
    `DARWIN_ZSTD_DIR`), `make release-macos` (host arch), then
    `package_release_macos.sh`; `attach-release` needs it and lists the
    two tarballs; the header states the new ownership. The draft note
    no longer promises an owner-attached mac asset.
  - **zstd is a pinned input now on the darwin host too:**
    `scripts/stage_darwin_zstd.sh <arch>` is the ONE recipe for the
    per-target static v1.5.5 archive on both build hosts — it reads the
    hosted MODE's `CC`/`AR` from the Makefile through a new `print-%`
    introspection rule (never a copy of the compiler line), pin-checks
    the tree (`git describe --exact-match` = v1.5.5; a drifted tree
    refuses, exit 1), and never leaves a target-flavored `lib/libzstd.a`
    behind. Container oracle: the recipe with the cross CC reproduced
    the July arm64 stage byte-for-byte in size (796888). The Makefile's
    darwin-host block now skips the brew INC/LIB derivation when
    `DARWIN_ZSTD_DIR` is given (an `$(origin)` guard); brew zstd stays
    the probe default. `provision_container.sh` reports + stages the
    darwin twins (SDK present) and the pinned libc++ stage.
  - **The libc++ copyright shipped is the pinned package's own:**
    `fetch_libcxx_headers.sh` also extracts
    `usr/share/doc/libc++-18-dev/copyright` to
    `$LIBCXX_HEADERS_HOME/copyright` (idempotency requires it), and
    `package_release_macos.sh` reads ONLY that file — the container's
    `/usr/share/doc` path is gone (one source on both hosts; the
    container stage is now provisioned, tarball copyright `cmp`-equal to
    the stage).
  - **Per-arch packager:** `package_release_macos.sh [arm64|x86-64 ...]`;
    default = the host's `release-macos` set (both on the cross host,
    the host arch on a Mac = `RELEASE_MACOS_ARCHES`). SHA256SUMS
    refreshes only the lines packaged this run (container oracle: a
    single-arch call left the other arch's line intact). On a darwin
    host the re-verify reads through the arch's hosted binary and brew
    llvm@18's `llvm-otool` (`MADC_READER`/`OTOOL` env override).
  - **PK4 `mactar` install gate** (`package_install_gate.sh mactar`,
    wired into the packager): extract → the shipped `bin/madc` runs the
    marker probe → `mac_battery.sh` on the extracted layout with a PASS
    floor (`MAC_BATTERY_FLOOR`, default 8 = the owner-hardware baseline;
    every FAIL line printed) → NEGATIVE CONTROL: hide `lib/libmadc_rt.a`
    and leg 6c must FAIL. Host arch only (a foreign-arch tarball is
    refused — no Rosetta proof); a stated SKIP on a non-darwin host (the
    container's packager run prints it). Byte-identity oracle for all of
    the above: `make -n -B` of the four container recipes (hosted arm64,
    default ELF, hosted win64, release-arm64-macos) IDENTICAL to HEAD.
  - `macho_exe_dylib_gate.sh` tool names are knobs (`OTOOL`/`OBJDUMP`/
    `NM`/`MADC`/`FOREST`; defaults = the container's apt spellings; a
    SKIP names the knob). `/promote` and the packaging-arc plan state
    CI ownership of all six assets; the container cross `release-macos`
    is the local verification lane whose tarballs are never uploaded.
  **BURN-IN GREEN 2026-09-02 — D3 DONE (run 33589127706, dispatched
  against the existing v0.97.0 draft with build_ref=develop @18dc95d7):**
  all five jobs green. Both mac legs leg-for-leg identical: the pinned
  zstd stage cloned + built natively by the hosted MODE's compiler
  (`DARWIN_ZSTD_LIB = ~/zstd/libzstd-<arch>-macos.a`, the brew derivation
  bypassed), `DARWIN_FREEZER = obj/hosted-<arch>-macos/madc-freezer`,
  `RELEASE_MACOS_ARCHES = <host arch>`, forest_pack_darwin 835 units,
  verify_macho_release OK, tarball packaged, `mactar` gate PASS — marker
  probe ok, **mac_battery 9/2 on both arches** (the two FAILs = the two
  standing known-opens: the `os.str()` husk inside the chained C++ groves
  leg, the value intrinsic), negative control ok (hidden `libmadc_rt.a`
  ⇒ leg 6c FAILS). attach-release: the draft carries SIX assets + a
  six-line SHA256SUMS (deb, rpm, linux tarball, windows zip, macos arm64,
  macos x86_64). Wall: arm64 leg ≈ 4 min, Intel ≈ 8 min; the run is
  bounded by the linux job as before. master's `release.yml` synced
  IDENTICAL (@2056f871). **Exit decision (owner):** canonical mac lane —
  recommendation: runner-native canonical for shipped assets (faithful
  self-freeze, no owner-SDK dependence, execution-proven per arch);
  container cross `release-macos` = local verification lane.
- **D4 — the full-suite macOS lane.** `run_tests.sh` under the gnubin
  posture on the runner; a `darwin` skip/expect fixture domain via the
  existing `MADC_SKIP_EXT` machinery for genuinely-divergent tests;
  triage the first run's failures the root-cause way (no bulk skips).
  New push-gated lane row (`macos-suite`) in `docs/lane-status.tsv`
  once green — this is the never-existed lane, the arc's biggest
  prize.
  **WIRED 2026-09-02 (first run = MEASUREMENT):** darwin-probe.yml gains
  the suite step after the battery — `MADC_BIN=bin/madc-release-<arch>-macos
  MADC_SKIP_EXT=darwin MADC_EXE_FLAGS=-static-libmadc bash
  scripts/run_tests.sh --exe` (the release-shaped binary = the packed-suite
  analogue), wall time stamped, `suite.log` + a sorted `suite-failures.txt`
  uploaded; a new dispatch input `suite_gate` (default true) makes the
  step gate — `suite_gate=false` is the measurement mode for the triage
  run. Shape decisions, each with its stated reason: (1) the exe pass
  links `-static-libmadc` because an Apple-target program that needs the
  madc runtime fails at emit otherwise (`cir_target_runtime_refused`; no
  libmadc dylib until D5) — the darwin AOT lane IS the static one, and
  mac_battery leg 6d already proves that shape; this needed ONE generic
  runner knob, `MADC_EXE_FLAGS` (env, exe-pass link line only; container
  controls: unset = unchanged, a bogus flag = every exe link FAILS);
  (2) `--obj` is out of the darwin domain (no Mach-O relocatable writer;
  mac_battery leg 6 documents the loud decline) — stated in the workflow,
  no per-test fixtures; (3) the unit tests (`make -C src test`) are the
  ELF default MODE's and stay on the container — the darwin default goal
  is a separate open; (4) the headerless cell for macOS stays OPEN
  (headerless_suite.sh: needs a sandbox-profile masking primitive, not
  faked). Actions bumped to their Node 24 majors (checkout v7,
  upload-artifact v7, download-artifact v8, cache v6) — release.yml's
  bump is proven by its next dispatch. Job timeout 150 min until the
  suite's wall time is measured.
  **FIRST MEASUREMENT (2026-09-02, runs 33593948720 + 33594687050 with
  `MADC_FAIL_DETAIL`):** arm64 JIT 1049/202 (0 timeouts), EXE 820/176;
  Intel JIT 1160/91, EXE 894/212. 90 JIT failures are common, 112 are
  arm64-only, 1 Intel-only. The suite runs in ~4 min per leg. Root-cause
  classes (the diagnostics travel with the verdict now):
  1. **Apple arm64 variadic marshalling through the dlsym K&R guess —
     ~112 arm64-only.** Every zero-include `printf(...)` script: madc's
     dynamic-symbol fallback declared an undeclared libc name as
     `int printf()` (K&R, zero parameters); c2mir compiles a K&R call with
     every argument NAMED (registers), while a variadic callee on Apple
     arm64 reads its variadic arguments from the STACK (MIR's `__APPLE__`
     arm ends named args at the proto's count). Wrong values, addresses
     printed for ints, SIGSEGV/SIGBUS in the printf family. Linux and
     x86_64 were forgiving (named and variadic travel alike). Fix at the
     declaration layer, GCC canon (the builtin's real prototype for a known
     library function): `Program::forest_adopt_declared_function()` —
     the pack's declaration index says the name is a bare library
     function, the arena's FuncDef is materialized through the same
     `materialize_for` the bound-include restore uses (widened by that one
     name) and registered through `register_forest_func`, the one owner of
     restored function registration; adoption is limited to VARIADIC
     callees in a C-primitive shape (the K&R call is ABI-correct for every
     non-variadic callee; FILE*/unknown typedefs are exactly where gcc's
     builtins stop too). The dev lane (bin/madc, live headers, no
     container) keeps the guess; the forest-bound lanes adopt. Reducer
     tests/testimplicitlibcproto (`--emit=c11`; strong .headerless_expect
     + .darwin_expect, weak dev .expect). A first token-slice version
     (parse the pack's prototype tokens through the statement parser) was
     REJECTED by the container: inline libc definitions (`__bswap_16`) and
     the script-mode file-scope check made the sub-parse leave partial
     registrations behind — the arena route has no parser in it.
     **Cost, measured on the release binary:** a zero-include printf hello
     is 16 ms with println only; the first arena-route cut made it 50 ms
     (27 ms of it `materialize_pass` building the whole derived population
     for one admitted name — a closure-shaped filter keeps every unindexed
     record); `CirMaterializeFilter::exact` (only the asked-for names and
     their pulled references; a NEW name is growth; a later closure filter
     widens it) brings it to 21 ms, under the tcc bar; the `#include
     <stdio.h>` twin is 31 ms. The derived UNRESOLVED census is skipped
     for an exact pass and reported once per forest across widened
     re-passes — a re-pass had re-reported every id and forest_pack_gate
     read DK_NONE 110 vs baseline 55 (a doubled count, not a regression).
     Landed @5393bf34 (fix) + @eeba65de (posture/fixtures/advisory).
  **MEASUREMENT #2 (2026-09-02, run 33641540075 @1e27ae4f, after fix-1 +
  batch-1):** arm64 JIT 1147/100 (was 1049/202), Intel JIT 1183/64 (was
  1160/91); exe advisory 185/213 exe-build (Tier B, one cause). Triage on
  the owner's M-series Mac (ssh, the fresh cross tarball, then per-fix
  cross builds scp'd over) — every class below MEASURED there before the
  fix, VERIFIED there after:
  9. **Strict-mode adoption miss (arm64 ~20).** Under --std=c17/gnu11/c++17
     the container's producer config mismatches the compile, the exact
     bind is NULL (the "multi-dialect fall-through"), so fix-1 never ran:
     zero-include strict-mode printf still read garbage; and `__builtin_X`
     went straight to the dlsym twin guess. forest_adopt_declared_function
     falls back to the stdlib-flavor-matched source forest (a C-primitive
     variadic prototype does not vary with the freezing dialect; GCC's
     builtin prototype applies in every -std), and
     register_builtin_twin_alias adopts X for `__builtin_X` (a FuncDef copy
     emitting as X, registered through register_forest_func). Reducer
     tests/testimplicitlibcprotoc17. @3078d41e.
  10. **libc++ std::string LAYOUT on Apple arm64 (arm64 ~35: to_string,
     operator+, stod aborts, php::/rust:: string publics returning `[]`,
     the polyglot/eval segfaults).** `__config` selects
     `_LIBCPP_ABI_ALTERNATE_STRING_LAYOUT` on arm64-apple only under
     `_LIBCPP_COMPILER_CLANG_BASED`, the identity gcc_posture_filter.sh
     drops, so the frozen arm64 grove used the standard SSO layout while
     Apple's libc++.dylib and the clang-built ns_* host objects use the
     alternate one — byte dump: madc-built "abc" = `06 61 62 63 …` (size at
     byte 0), dylib-built to_string(42) = `34 32 … 02` (size at byte 23).
     A TARGET ABI fact: gen_predefined_macros.sh appends the define to a
     posture-filtered table that defines __APPLE__ and __aarch64__ (the one
     _LIBCPP_ABI_* define inside that clang-gated block; x86_64 keeps the
     old layout in the same branch — no line). Not the shared filter: a
     define arriving via the prelude umbrella lands after __config decided.
     Reducer tests/teststringabiinterop (one .expect, every lane). @b03d6e4b.
  11. **`sizeof(*(p))` grammar (both arches: teststructinterop).** Apple's
     FD_ZERO expands to `__builtin_bzero(p, sizeof(*(p)))`; the sizeof
     type-query arm claimed every `*`-led operand as `*ident` and threw.
     The arm now declines without consuming so the sizeof(expression)
     fallback measures it. Reducer tests/testsizeofderefparen. @c5a2dd79.
  12. **Bare `NULL` (both arches: testphpdumpptr, testprojectwiden).** The
     auto-include binds the pack's stddef.h unit alone (12 tokens: offsetof,
     ptrdiff_t) — the embedded header's full arm guarded NULL with #ifndef,
     false at freeze time under the prelude-first canonical order. Now
     unconditional (#undef + #define, gcc's shape). @36c27de7.
  13. **libc++ flavor domain (4, both arches).** The suite runs
     MADC_SKIP_EXT="darwin libcxx": testinvocable/testtraitassign/
     testcompare_realhdr/teststringsize test libstdc++ internals and carry
     .libcxx_skip already (the container's -stdlib=libc++ lane). testufcsorder
     .darwin_skip: UFCS fires only for an UNDECLARED name by design and the
     darwin umbrella declares strlen from any include. @ae3abdeb.
  14. **long double = double on Apple arm64 (arm64 4-5).** DataDefLDOUBLE
     baked 16/16 (x87); now sizeof/alignof(long double) of the compiler
     building madc for the target — what c2mir already does
     (sizeof (mir_ldouble)). Linux/Windows x86-64 unchanged (16/16).
  15. **Apple x86_64 `$INODE64` (Intel: teststat, testdirent).** The asm
     label landed on the Variable's storage_alias_name only, which the freeze
     does not carry; the x86_64 pack restored `stat` plain (verified: emit-C
     with that forest bound on Linux calls `stat`). parseFunction now also
     sets FuncDef::emit_symbol (the LIBRARY link symbol the arena record
     carries and call_emit_symbol reads first). Runner-verified only (no
     Intel Mac at hand).
  **WAVE 2 (2026-09-02) — the first two STILL-OPEN items below are CLOSED:**
  - **`long` vs `long long` identity: FIXED** (the type model, one data rule).
    Root cause traced with a probe: the use site spelled the resolved pinned
    dd by its DISPLAY name "int64_t" (`template_type_arg_spelling`'s
    `canonical spelling, else display name` fallback) and
    `canonical_arg_key_fragment` re-resolved that string through the
    SOURCE-spelling table, whose int64_t row is the distinct `long long` dd
    on Apple; the specialization side keyed the raw token `long` to int64_t.
    Same flip in `canonical_template_binding_dd` (deduced size_t bound
    `std::max<unsigned long long>`). Fix: `madc_stamp_primitive_type_ids`
    stamps a canonical spelling — `DataDef::target_scalar_spelling()`, the
    mangler's DataType→C table lifted out as the one owner — on exactly the
    pinned dds whose display name does not round-trip through the table
    (darwin: ddINT64 `long`, ddUINT64 `unsigned long`; glibc/LLP64 stamp
    nothing). The binding canonicalizer keeps a dd whose canonical spelling
    resolves to itself. Exposed and fixed on the way: the class-scope
    typedef arm compared the source spelling to the display NAME (now the
    identity spelling) — it had minted `typedef _Tp type` as a new type and
    `common_type<long,long>` derived from itself; `collect_vbases` now
    aborts naming a self-based class instead of overflowing the stack.
    Reducer: `tests/testtplargidentity.mad`. **Reproduce darwin identity on
    the container**: `bin/madc-arm64-macos --std=c++17 --no-config
    --no-sysroot-includes --emit=c11 <reducer>` (the Linux-hosted cross
    freezer carries the darwin type model). Darwin pack 64→66 with the
    reason stated in the baseline file: same corpus, pre/post freezers
    diffed, one line differs — `__atomic_is_lock_free` 10→12, the known
    cross-freezer dlsym leak reached by the `long`/`unsigned long`
    `__atomic_base` instantiations that no longer alias `long long`; the
    linux pack is byte-identical (93).
  - **Strict-mode `<stddef.h>` inside served libc++: FIXED @4fbcae4d.**
    `embedded_header_outranked` and `embedded_wins_include_next` adopt
    `resolved_include_provider_exists()` (disk OR pack) — the pack counts as
    a directory. Mac-verified on a header-less Mac.
  - **wchar_t template-argument identity (KG Gap
    `wchar_t_template_arg_identity`): FIXED, D4 wave 3 — five commits.**
    The symptom (a wchar_t-bound argument keyed "int32_t": `ctype<wchar_t>`'s
    explicit specialization invisible, the memberless primary instantiated,
    `__is_same(int, char_traits<wchar_t>::char_type)` TRUE) had THREE
    causes stacked, each found once the one above it was gone:
    1. **types** — `dd_platform_wchar()` returned the storage dd itself
       (ddINT32 / ddUINT16); char16_t/char32_t were ddUINT16/ddUINT32.
       Now DataDefPlatformWCHAR / DataDefCHAR16 / DataDefCHAR32, distinct
       dds named by their C++ spelling (typeid slots 39/40/41), the three
       spelling carve-outs deleted, Ds/Di mangling rows added.
    2. **headers + typedef** — the embedded `stddef.h` typedef'd
       `__WCHAR_TYPE__ wchar_t` unconditionally where gcc/clang guard with
       `#ifndef __cplusplus`, and the darwin prelude flattened Apple's
       `_wchar_t.h` under `-x c`, resolving the same guard away; madc
       accepted `typedef int wchar_t;` silently, so every DERIVED spelling
       (`wchar_t*`) keyed int32_t*. Both header texts carry the oracle's
       guard now (the prelude generator restores it), and
       `typedef_alias_spelling` — the one alias acceptor — refuses a
       language-keyword type (`TokenDataType::keyword`, read from the
       registration like `builtin`). This was the darwin-specific half:
       113 mixed wide-stream keys in the frozen groves.
    3. **overloads** — `score_arg_to_param` tied two scalar pointees /
       values on rawtype, so `const wchar_t*` bound a first-registered
       `kind<int>` instance. Both lanes compare through
       `Program::proven_scalar_identity()` (the one builtin table);
       unproven pointees keep the representation test.
    Found on the way: an explicit specialization on a pointer to a builtin
    (`template<> struct N<int*>`) keyed intP while its uses keyed int32_tP —
    `canonical_arg_key_fragment`'s suffix arm resolved its core through
    `resolve_named_datadef` (the lexer's literal-typing ddINT) instead of
    the builtin table the bare arm uses; one order for both arms now
    (`tests/testptrbuiltinspec.mad`). And two rows of that one table
    predated the types they name (`long double` -> the double dd,
    `__int128` -> the 64-bit dd): K<long double> and K<double> shared ONE
    instantiation, W<__int128> answered for W<long>, and the identity
    scorer tied f(long double) with f(double) — rows fixed, pch.cpp's
    private copy of the table adopts the owner
    (`tests/testbuiltintplkey.mad`; the battery caught it in
    teststaticoverload). Measured: linux pack 93 -> 68
    (dk-none 55 -> 45), darwin 66 -> 52 (49 + three more instances of
    the known `__scalar_hash<_Tp, 2>` body misparse, now that hash<long
    double>/hash<__int128> no longer alias the 8-byte hashes); mixed wide keys linux
    87 -> 0, darwin 113 -> 0. Reducers as tests: testwchartidentity,
    teststddefwchar, testtypedefkeywordredecl, testptrbuiltinspec.

  **MEASUREMENT #3 (2026-09-03, run 33759725335 @5511a3f9, after wave 3 — the
  wchar_t identity slice — both jobs green): arm64 JIT 1215/36/19skip (was
  100 at #2, 202 at #1), Intel JIT 1224/27/19skip (was 64, 91). EXE lane
  advisory: arm64 947/210, Intel 936/230 (-static-libmadc cannot cover the
  dialect runtime until D5).** Intel's 27 are a strict subset of arm64's 36;
  the nine arm64-only ones are the MIR aarch64 floor (gccvector x3 = SIMD,
  testint128 proto result type, testcomplexretconv `i2d` on ldouble,
  testbuiltincomplexparts / testbuiltinconjf abort, testprintfdouble SIGBUS =
  varargs double, teststdiobuiltinredirects garbage). The 27 common, by
  family — every one a served-grove libc++ gap, and (new this round) the
  istringstream and `.str()` families REPRODUCE ON THE CONTAINER: the cross
  freezer in DEFAULT (dialect) mode with the darwin pack bound
  (`bin/madc-arm64-macos --no-config --no-sysroot-includes
  --forest-bind=obj/hosted-arm64-macos/forest.bin --emit=c11 <test>`); the same
  TU passes under `--std=c++17`, and the linux libstdc++ pack passes in the
  dialect — a dialect-mode gate in the grove path is the layer:
  - istringstream/stringstream TYPE with a parenthesized ctor argument (5:
    testistream_libcxx, testopinherit, teststreambool, testvbasedyn;
    testcopiedrefptrparam = istream `traits_type`): `std::istringstream
    s("41")` is routed to parseFunction ("Failed to find type when parsing
    function parameters") — `std::istringstream s;`, `sizeof`, and a USER
    `typedef basic_istringstream<char> myiss; myiss s("41")` all work, so the
    pack-restored typedef's dd is not seen as btClass by parseDeclaration's
    ctor-call branch in dialect mode.
  - `.str()` husks (3: testmanip, testsstream, testvaluecoutsstream) —
    `Unidentified member 'str' in basic_stringstream_char_...`; reproduces
    the same way.
  - `unwrap_iter.h:48 invalid operand types of -` c2mir check error (4:
    testnestedenumvec, testvartplsigchain, testvartplsigchainns,
    testvecmembercopy) — the `__tree`/`unwrap_iter` class of measurement #1,
    now a hard error; emits clean C11 on the cross freezer (the error is
    c2mir's) — compile the emitted C for arm64 to reproduce.
  - list `__base_pointer` ClassPattern (3), path.h `format` (2: testmanipview,
    testofstreamwrite), initializer_list private `__begin_` (2), testiomanip
    (shift operands), testufcscall (`__ns_std____1_count` undefined),
    testclassproto (nested-std seed leak), teststreamdecl_libcxx (0 vs 1),
    teststructinterop (tm fields).
  - Domain, not defects (fixtures landed ebc105d5): testgnuattributemode and
    testmemclralignwide redeclare int16_t/int32_t/int64_t against Apple's
    <stdio.h> (clang rejects too); testdlcall dlopens libc.so.6.
  Triage artifacts: tmp/d4-m3/{arm64,x86-64}/ (suite.log, suite-failures.txt,
  classified/).

  **WAVE 4 LANDED (2026-09-03, six commits on 5511a3f9 — fb6f3537 9b2ee46c
  75388cb5 a49380c8 44da4f8e cf61e762): stream operands + wide literals in
  BOTH free-operator lanes.** `std::cout << ca` (char[]) printed garbage and
  `std::wcout << L"x"` / `<< q` printed a POINTER on every platform; fixed
  in the cir ranking lane (fb6f3537: the rhs decays through the ONE owner
  Program::array_decay_pointer; the lhs binding is substituted into param[1]
  before matching so the generic `const _CharT*` inserter deduces), then
  the same defect's second copy in the retained-template BODY route
  (cf61e762: libc++ exports no wchar_t inserter, so
  Program::instantiate_free_operator_template serves the body — it typed
  operands as the bare element and re-selected by registration order,
  serving the single-_CharT inserter for `wcout << wa`; it now types through
  the same decay owner AND instantiates only the signature the ranking lane
  chose — [over.match.best] decided once). sizeof(L"x") measures target
  units (9b2ee46c). Reducer tests/teststreamwideops.mad (g++ == clang++ ==
  madc on libstdc++ and libc++; Mac: all five lines, dialect and c++17).
  Darwin domain fixtures for three linux-domain tests (75388cb5). Packs
  unchanged (linux 68/45, darwin 52).

  **WAVE 5 SLICE 1 — root cause pinned (2026-09-03):** the istringstream
  ctor-argument family is PACK vs LIVE, not dialect vs strict (the pack is
  frozen for the dialect config word; a `--std=c++17` compile parses live —
  `--no-forest-bind` in default mode passes too). libc++'s __fwd/sstream.h
  typedefs instantiate basic_istringstream<char> / basic_stringstream<char>
  / basic_stringbuf<char> from the FORWARD declaration before <sstream>
  defines the template; nothing in the pack TU demands completion; the
  freeze-end sweep records memberless incomplete aggregates on purpose, so
  the pack serves 0-member husks (`forest_restore_decls: class
  basic_istringstream_char_... (0 members, 0 bases, 0 methods)`).
  parseDeclaration's demand-completion (complete_class_type_on_demand ->
  request_template_instantiation_completion) consults
  pending_template_instantiations — a parse-time side table minted only by
  the live forward-instantiation path and never frozen — finds nothing, the
  husk keeps has_user_ctor=false, the ctor-call branch is skipped and the
  declaration falls to parseFunction. basic_ios / basic_istream restore
  complete because libc++ has extern templates for them; libstdc++'s
  <sstream> has `extern template class basic_istringstream<char>`, which is
  why the linux pack never showed it. Fix (in flight): the pending record
  is derived from the husk's canonical template-id spelling at the ONE
  consult (pending_instantiation_from_canonical_identity).

  **WAVE 5 LANDED (2026-09-03, b14a6e2c + 8211a7b2): the istringstream /
  stringstream family = two frozen-forest defects, neither darwin-specific.**
  (1) b14a6e2c — a forest-restored INCOMPLETE husk completes on demand from
  its canonical identity: parseDeclaration's demand-completion consulted
  pending_template_instantiations, a parse-time table minted only by the
  live forward-instantiation arm and never frozen; the record is now derived
  from the husk's canonical template-id spelling at the one consult
  (pending_instantiation_from_canonical_identity). (2) 8211a7b2 — a restored
  POLYMORPHIC class is the parsed class: secondary_vptr_owners refilled from
  the restored vtable groups (the struct emitter named secondary vptr fields
  from the never-frozen layout list while ctors stamped from the frozen
  groups: `struct has no member __vptr_16` in basic_iostream<char>'s ctor);
  the flat vtable_slots list and virtual_methods set now FREEZE as defrec
  word runs (format 41 -> 42; every restored class had answered
  vtable_slot("~$deleting") < 0 — no deleting dtor, no `delete p` dispatch —
  and is_virtual_method() false); bound user-header method prototypes
  splice before the Pass-1.5 vtables that take their addresses. Reducers:
  tests/testistreamctorarg.mad (oracle `41 82 ok`) and forest_bind_gate
  case `secvptr` — the FIRST polymorphic bind case (the 26 others had no
  virtuals), which fails on the pre-fix binary exactly as the darwin family
  did. The linux libstdc++ pack carried the vptr-field hole silently (pad
  filler kept offsets; libstdc++ exports every stream ctor body). Measured
  on the darwin cross freezer with the pack bound: all eight family tests
  emit and pass clang -fsyntax-only; the owner's M-series Mac ran the
  build-29 darwin binary (tmp/logs/mac-red-29.log): testistreamctorarg
  `41 82 ok` in BOTH dialect (pack-bound) and --std=c++17 (live) mode,
  testsstream / testmanip / teststreambool at oracle output, teststreamwideops
  all five lines, every wave-3/4 reducer unchanged. Env probes kept:
  MADC_DECL_PROBE (ctor-call branch inputs), MADC_LAYOUT_PROBE (every
  compute_layout run of a class).
  Lesson for the runner measurement: the pack is frozen for the DIALECT
  config word, so a `--std=c++17` run with --forest-bind is a LIVE parse —
  "passes under c++17" meant "passes live", not a strict-mode difference.

  **WAVE 6 LANDED (2026-09-03, nineteen commits on 3f7c88ef: 9f144e11 4b032c94
  da57f5d5 4632b410 + 2ef73b51 bb1477ad ece8de40 ef7ed6b9 4b8dbb0c 327f4896 61153b26
  a8fbf20e + 624cb050 c85cefdb f413c761 c73f4496 6725ae40 77f54223 3eee5c43): measurement
  #4 (run 33784047473 @3f7c88ef: arm64 1227/23, Intel 1236/14) — every common
  darwin failure traced to a root and reproduced on the container, none
  darwin-specific in mechanism; the aarch64 MIR floor (9 arm64-only tests) is
  deferred LAST by owner ruling, batched with the three open vnmakarov/mir
  issues.** (1) 9f144e11 — teststructinterop printed FD_ISSET's raw macro
  result through %ld (Apple's __DARWIN_FD_ISSET returns the bit; test defect).
  (2) 4b032c94 — a member function named with explicit template arguments
  hides a namespace-scope function of the same name ([basic.lookup.unqual];
  `bind<U>(...)` in a member body bound <sys/socket.h>'s ::bind and the
  instantiation died as an undefined MIR import — testcopiedrefptrparam;
  reducer tests/testmemberhidesglobal.mad). (3) da57f5d5 — a template-head
  hidden-friend operator DEFINITION inside a class is a namespace-scope
  operator template ([temp.friend]; libc++ writes every <iomanip> inserter
  that way — testiomanip; reducer tests/testhiddenfriendtemplate.mad), and
  ef7ed6b9 freezes the friendship grants (friend_function_names /
  friend_class_names as defrec word runs, format 42 -> 43) so the pack-bound
  consumer honours them too (bind-gate case friendgrant). (4) 2ef73b51 —
  constructor overload ranking reads the argument's VALUE CATEGORY
  ([over.ics.rank]/3.2.3): std::move(x) / a by-value call / static_cast<T&&>
  (TokenCast::to_rvalue_ref, recorded at the named-cast parse) prefer a
  concrete T&& ctor and an lvalue cannot bind one; an INSTANTIATED
  member-template ctor (tsubst_source set) is deduction-decided and never
  judged — the first cut refused every lvalue to libc++'s
  __compressed_pair(_U1&&, _U2&&) / std::pair and broke std::map on both
  libraries in build-33/35 (silent wrong answer on linux before: the copy
  ctor won every move; reducer tests/testrvaluectorselect.mad `3 3 -1`).
  (5) bb1477ad — an out-of-class constructor DEFINITION attaches to the in-class
  declaration by SIGNATURE, not arity (libc++ vector declares five
  one-parameter ctors and defines copy/move out of class; testinitlist,
  testinitlistclass, testvecmembercopy; reducer
  tests/testoutoflinectoroverload.mad `3 3 -1 9`). (6) ece8de40 — the dialect
  UFCS trigger fires when the visible free-function set has NO arity-viable
  candidate, not only when the name is unbound (libc++ <map> reaches
  std::count by ADL — testufcscall; reducer tests/testufcsviable.mad `1 0`).
  (7) 4b8dbb0c — a same-tag aggregate from another scope never takes a bare key
  or emitted identity a complete aggregate holds, in BOTH orders: the class
  parser gains the global-reclaims-bare mirror arm (linux threw "already
  defined"; darwin live collided std::__1::__fs::filesystem::path with the
  user's `class path`), and the forest RESTORE applies the sibling-namespace
  rule itself (the user's global class parses first, libc++'s path is
  demand-restored later and clobbered struct_map + emitted a second `struct
  path` — testclassproto under the pack, `no member named 'n'`); one
  flat_scope_identifier replaces three inline `::`->`_` loops (reducer
  tests/testnamespaceclasscollide.mad `7 40 1`); a8fbf20e closes the pack
  ORDER neither could see — libc++'s path is pulled into the type graph by a
  restored record's reference and registered nowhere, so the EMITTER (the only
  layer that meets both objects) now tells a same-spelling twin from a
  different entity (m_emitted_struct_owner + struct_emission_deduped) and
  gives the latter a distinct emitted identity (`__madc_global__path` beside
  libc++'s `path`; testclassproto pack emission 9 clang errors -> 0). (8) 327f4896 — a class-pattern
  CAPTURE whose decltype(...) operand is unresolved (a call to a body-less
  fn-template placeholder still answering its ddINT64 default, or a
  dependent-member TokenInt stand-in) poisons the capture so the class stays
  on the parse lane: libc++'s __unwrap_iter_impl<_Iter,true>::_ToAddressT
  was baked as int64 in the frozen pattern and every vector copy failed with
  c2mir `invalid operand types of -` (testnestedenumvec, testvartplsigchain,
  testvartplsigchainns, testvecmembercopy; bind-gate case patternalias, tally
  27 -> 29; reducer tests/testpatternalias.mad; MADC_MTI_PROBE_CLASS now shows
  `_ToAddressT -> int32_t*` under the darwin pack). (9) 61153b26 — a
  using-directive's nested namespaces resolve as qualifiers
  ([namespace.udir]/2; `using namespace lib; fs::path` — found by the
  collision reducer's first spelling; reducer tests/testusingnsnested.mad
  `1 5 9`). (10) 624cb050 — testmemberhidesglobal declares its own global
  `::bind` instead of including <sys/socket.h> (mingw has none; the wave-6 wine
  lane failed at the include, 1219/1 -> 1220/0). (11) c85cefdb — THE
  REGRESSION the Mac A/B caught (b29 ok -> b38 `madc: caught SIGABRT` on
  teststdmapint / testufcscall / testufcsviable; the linux battery was green
  because libstdc++'s _Rb_tree never returns a node holder by value): 2ef73b51's
  value-category ranking read `return __h;` (libc++ __tree::__construct_node,
  a unique_ptr __node_holder) as the lvalue the name is, refused
  `unique_ptr(unique_ptr&&)`, the copy ctor is deleted, and
  class_copy_construct_into_retbuf fell to its bit-copy fallback — the local's
  cleanup destructor then freed the node under the tree (double free: SIGABRT on
  macOS malloc, SIGSEGV on linux under `-stdlib=libc++`). [class.copy.elision]/3:
  a `return` operand naming a non-volatile automatic object of the return class
  (a local or a parameter) is resolved AS IF an rvalue — the return-copy site,
  the only implicit-move context the builder has, now says so
  (returned_operand_is_implicitly_movable -> select_ctor_overload's
  implicit_move flag; one pass suffices since only concrete-`T&&` candidates
  read the category). Reducers tests/testimplicitmovereturn.mad (`c 5` — the
  move ctor, not the copy ctor's 105, which the PRE-wave binary printed too; a
  move-only class returned from a local) and tests/teststdmapint_libcxx (the
  libc++ std::map shape on the linux battery; the libstdc++ twin never saw it).
  METHOD that found it in minutes: `--emit=c11` with the pre-wave and post-wave
  cross binaries on the Mac (~/d4/madc-b29 vs madc-b38) and diff — the
  move-ctor call in __construct_node became `(*__retbuf) = __h`. The fix then
  exposed FOUR PRE-EXISTING defects (b29 fails each too), one commit each:
  (12) f413c761 — a class prvalue carried as a c2mir STRUCT VALUE (a
  by-value call of a class returned natively: no destructor, so no __retbuf) IS
  the result object: object_arg_addr spills it bitwise when its address is
  needed and the declaration lane initializes `C b = make(5)` directly
  (guaranteed elision) — before, a class WITH user copy/move ctors and NO
  destructor fell between class_trivially_copyable (the raw-call test) and
  class_needs_dtor (the retbuf decision): its move ctor was handed `&make(5)`
  (c2mir `lvalue required as unary & operand`); ONE predicate now,
  native_class_value_result (tests/testnativeretinit `5 7 1 9 0`).
  (13) c73f4496 — a ctor MEMBER INITIALIZER converting a derived pointer
  to a base pointer member applies upcast_class_ptr like the assignment and
  declaration lanes: `struct D : A, B {}; struct It { B *p; It(D *q) : p(q) {} }`
  read A's field through p (madc `1 2 0`, oracle `2 2 1` — silent), and on a
  primary base c2mir warned `incompatible types in assignment to a pointer` on
  every libc++ std::map (`__tree_iterator(__node_pointer) : __ptr_(__p)`), which
  the zero-warnings census would have refused for the new libc++ test
  (tests/testmemberinitupcast). (14) 6725ae40 — LAMBDAS: the closure's
  call operator is parsed by parseFunction (parameter declarators, trailing
  return with the parameters in scope, body, auto deduction, the nested-function
  hoist with `[&]`'s capture-by-reference) — the hand-rolled `type ident` loop
  accepted no declarator, and the implicit-move rule made `return s;` on a
  std::string select libc++'s MOVE constructor for the first time, whose __r_
  mem-init is `[](basic_string& __s) -> decltype(__s.__r_)&& { ... }(__str)`:
  every std::string move under -stdlib=libc++ died with "Expecting identifier
  in lambda parameter list" (teststrret_libcxx, teststrplus_libcxx — green in
  the wave-6 battery only because the copy ctor had always won;
  tests/testlambdaparamref `7 0 / 12 / 5 3 / 14`). (15) 77f54223 — ACCESS
  CONTEXT: a lambda (a hoisted nested function) written in a member has the
  member's access ([class.access]/2); the seven access checks read ONE
  derivation, access_context_class (the nearest enclosing member along the
  compound chain), and the friend grant reads the outermost function's name —
  the same libc++ lambda reads the private __r_ (tests/testlambdamemberaccess
  `10 25`). (16) 3eee5c43 — the close-out battery's first red
  (test_libmadc_program "call supports script string object returns", SIGSEGV):
  the implicit move applied to a returned by-value PARAMETER, and madc's
  parameter is a bitwise alias of the caller's object, so the move emptied the
  host's std::string in place; a returned parameter keeps the copy until the
  parameter is the callee's own object (the rule applies to a LOCAL only; the
  dependency is written at the site and on the Gap). STILL OPEN from the same
  drafts, filed as KG Gaps: by-value CLASS PARAMETERS are bitwise copies with
  no parameter destructor (`void f(D d)` from an lvalue prints `f 1` where g++
  prints `f 101 ~101` — silent, exit 0; string_pass_by_value_semantics
  generalized; the Itanium fix is pass-by-invisible-reference with caller-side
  temp + destruction, a parameter-representation change; it BLOCKS the
  parameter half of the implicit-move rule); a deleted copy ctor with no viable move
  leaves the retbuf copy on its silent bit-copy fallback where g++ rejects the
  program; lambda capture forms beyond `[]` / `[&]` (`[=]`, `[this]`, named and
  init-captures). Also 4632b410: MADC_MTI_PROBE_CLASS set-alias + MADC_DT_PROBE
  gate-input probes (they located the unwrap split: type=17 — the operand is
  the CALL, typed by the placeholder callee's default, not a TokenInt).
  Verification: build-38 (tmp/logs/d4-build-38.log): forest_bind_gate GREEN
  29/29, the 21 targeted linux tests 21/0, the 246-test class/template/map
  subset 246/0, packs at baseline (linux 68, darwin 52), darwin pack-bound
  emissions of testiomanip / testclassproto / testufcscall clang-clean, the
  alias probe `int32_t*`. Builds 39-43 (tmp/logs/d4-build-39..43.log, the six
  fixes): each fix verified on the container against the g++/clang++ oracle in both dialects before its commit (build-39 the std::map fix + testimplicitmovereturn; build-40 testnativeretinit; build-41 testmemberinitupcast + testlambdaparamref, libc++ std::map warning 1 -> 0; build-42 testlambdamemberaccess, teststrret/strplus_libcxx green, subset 372/0, darwin pack 52 -> 48; build-43 the returned-parameter exclusion after the close-out battery's unit-test SIGSEGV, subset 309/0); forest_bind_gate 29/29 throughout; the close-out battery at 3eee5c43 (tmp/logs/d4-wave6b.log): linux jit 1277/0/9skip exe 1218/0 obj 1218/0 packed 1277/0/9skip headerless 1248/0/38skip, warning ratchet GREEN, packs linux 68 / win64 68 / darwin 48; wine 1225/0/61skip + verify_pe_release OK; c-testsuite 220/220; macos cross both arches 835 units + verify_macho OK. Mac (tmp/logs/mac-red-39.log, mac-red-42.log): madc-b42 (77f54223) and madc-b43 (3eee5c43, the battery's release binary) on the owner's M-series Mac: teststdmapint / testufcscall / testufcsviable pass in the pack, live and under --std=c++17 where b38 aborted; every wave-6 reducer, all six new reducers, teststrret/strplus_libcxx, the existing lambda tests and the wave-3/4/5 regress line print their oracle output; the __tree:722 warning is gone on darwin too. TRAP: the first mac-red-42 run used a sed without g and ran madc-b39 — grep the driver's M= line before believing a red Mac run. Builds 33 and 35 were RED first: the T&& refusal hit
  forwarding references (the candidate is the ctor-template INSTANCE, spelled
  `tag*&&`, no template-parameter names of its own — tsubst_source is the
  handle), the capture poison keyed on a TokenInt while the operand is the
  CALL, and the collision reducer's `using namespace lib; fs::path` was a
  separate gap (G). Open after wave 6 — Gap
  libcxx_fn_template_placeholder_called_in_instantiated_body (wave-7 first
  item): the darwin emissions of testvecmembercopy / testinitlist /
  testinitlistclass / testnestedenumvec / testvartplsigchain(ns) still call
  the un-instantiated `std::forward` / `__uninitialized_allocator_copy_impl`
  placeholders (`extern long long f()`) inside instantiated bodies
  (make_pair<int*,int*>, pair's converting ctor, __uninitialized_allocator_copy),
  pack and live alike — surfaced once the unwrap alias stopped the c2mir error
  earlier in the pipeline; libc++ list __base_pointer rebind (3 arm64 tests);
  istream traits_type soft errors under the pack (test passes); filebuf D1Ev
  conflicting externs (emit-C portability); iomanip live memmove undeclared;
  `<filesystem>` `clock` on linux libstdc++; the 9 aarch64 MIR-floor tests
  LAST with the three vnmakarov/mir issues (owner ruling).

  **MEASUREMENT #5 (run 33816087788 @bb69b2ab, 2026-09-03 23:06 UTC): arm64
  1246/18/22skip (from 1227/23), Intel 1255/9/22skip (from 1236/14) — ZERO new
  failures on either arch; five fixed on both (testclassproto,
  testcopiedrefptrparam, testiomanip, teststructinterop, testufcscall).
  Intel's 9 == the common set: 6 placeholder-gap tests (testinitlist /
  testinitlistclass `import of undefined item __uninitialized_allocator_copy_impl`;
  testvecmembercopy / testnestedenumvec / testvartplsigchain(ns) vector:387:33
  `lvalue required as unary & operand`) + 3 libc++ list `__base_pointer`
  (testforeachiter, testphpdumpiter, testptrcmpupcast). arm64 adds the 9
  MIR-floor tests (testbuiltincomplexparts / testbuiltinconjf abort,
  testprintfdouble SIGBUS, testgccvectorbitwisenot / testgccvectorlit /
  testgccvectorsizeexprbitwise, testcomplexretconv `i2d` on ldouble, testint128
  proto result type, teststdiobuiltinredirects garbage). Runner mac battery 10/1
  both arches — the `__tree:722` warning is gone; the include-free value
  intrinsic (`invalid operand types of +`) is the standing known-open. Exe lane
  advisory (arm64 975/213, Intel 964/233, all `-static-libmadc` Tier B).**

  **WAVE 7, ITEM 1 LANDED (2026-09-04, four commits on bb69b2ab: 33beee8f
  0a353603 5357a30c 92e9cc53) — Gap libcxx_fn_template_placeholder_called_in_
  instantiated_body CLOSED: all six placeholder-gap tests pass under
  -stdlib=libc++ on linux with zero warnings; the darwin runner re-measures
  them in dispatch #6.** The chain, traced with include-free reducers
  (tmp/w7auto*.mad -> tests/testfntplsamename, tests/testfntpldefaultdecltype,
  tests/testfntplrefidentity; the libc++ twin tests/testvecmembercopy_libcxx is
  the linux gate for the whole chain):
  (1) 0a353603 — libc++ 18's pre-C++20 `__unwrap_range` spells its result type
  through a DEFAULTED parameter, `_Unwrapped = decltype(std::__unwrap_iter(
  std::declval<_Iter>()))`, and `__unwrap_iter`'s own return is
  `decltype(_Impl::__unwrap(std::declval<_Iter>()))` with `_Impl` defaulted.
  The unevaluated call's return lane (resolve_fn_template_return_by_key) filled
  no [temp.deduct]/8 defaults and resolved only template-id decltype operands,
  so the call kept the int64 placeholder, `_Unwrapped` = int64_t, `__unwrap_range`
  returned pair<int64,int64> (its body computed pair<int*,int*> and CONVERTED on
  return — visible in `--emit=c11`), and `__uninitialized_allocator_copy_impl`
  had no viable overload: the un-instantiated placeholder call. The lane now
  fills defaults after deduction (call argument types known; concrete bindings;
  definition namespace) and hands any other decltype operand to the ONE
  declared-type resolver's decltype arm.
  (2) 5357a30c — the last placeholder, `std::forward<_T1>(__t1)` inside
  make_pair<int*,int*>: madc names a reference dd as its pointer twin (DataDefREF
  IS-A DataDefPTR; `int&` and `int*` are both "int32_t*"), and the three identity
  formers of a fn-template instantiation — the inst_key memo, the overload set's
  template_arg_names, the ranker's explicit-argument match — keyed on that name:
  forward<int*> memo-hit the forward<int&> instance vector's construct path had
  made, ranked non-viable, fell to the placeholder. ONE identity spelling now
  (template_binding_identity_spelling, beside canonical_template_binding_dd): a
  reference spells `referent&`. Follow-on (first-class refs Phase 2): a
  DataDefREF NAME of its own.
  (3) 33beee8f + 92e9cc53 — found by the first reducer: six sites located a
  fn-template declarator by a first-occurrence NAME search; a same-named token
  inside the leading decltype return (`decltype(Impl<I>::unwrap(I())) unwrap(I i)`)
  became the declarator and `(I())` the parameter list. All six read the ONE
  DelimDepth-aware locator now.
  Verification (container build-2): reducers `3 4 1` / `5 7 5` == g++ == clang++;
  the six libc++ shapes rc=0, every .expect line met, zero warnings; the
  fn-template + libc++ neighbourhood subset 75/75.
  Lanes at 92e9cc53 (2026-09-04, tmp/logs/w7-battery.log): linux jit 1281/0/9skip,
  exe 1222/0, obj 1222/0, packed 1281/0/9skip, headerless 1251/0/39skip
  (forest_pack_gate linux 68 = baseline, forest_bind_gate 29/29, warning
  ratchet GREEN, tsubst flag-on GREEN); wine64 1228/0/62skip + verify_pe_release
  OK (win64 pack 68 = baseline); c-testsuite 220/220; macos cross release both
  arches 835 units, darwin pack 48 = baseline (mir-blob-skips 1/1), verify_macho
  OK. Dispatch #6 measures the item on the darwin runner (expected residue:
  arm64 ~12 = 9 MIR floor + 3 list, Intel ~3 list).

  **WAVE 7, ITEM 2 = libc++ `<list>` — ANALYSED, FILED (KG Gap
  libcxx_list_node_pointer_traits_completion_cycle), NOT FIXED: a design change.**
  Reproduces live on linux (`-stdlib=libc++`: list:353 `expression before '->'
  must be a pointer`, list:667 `member reference is not a structure or union`),
  not only on the darwin pack. `__list_node_pointer_traits<T,void*>`'s
  `typedef __rebind_pointer_t<void*, __list_node<T,void*>> __node_pointer` names
  `__list_node<T,void*>` only to form a POINTER to it; C++ forms that pointer to
  the incomplete specialization ([temp.inst]/1), madc COMPLETES the pointee, whose
  base `__list_node_base<T,void*>` then typedefs `_NodeTraits::__node_pointer /
  __base_pointer / __link_pointer` while the traits class is mid-instantiation —
  ephemeral opaque dependent-member placeholders (MADC_MTI_PROBE_CLASS:
  `opaque-member owner=__list_node_pointer_traits_int32_t_voidP member=
  __node_pointer in canon std::__1::__list_node_base<int32_t,void*>`) baked into
  the base's typedefs; the traits aliases heal afterwards (`set-alias
  __link_pointer -> __list_node_base_int32_t_voidP*`) but `__prev_`/`__next_`/
  `__ptr_` keep the placeholder and `->` refuses. The pack-bound lane reports the
  same cycle as "basic ClassPattern could not resolve __base_pointer". Fix shape:
  a class template-id used as a template ARGUMENT (or a pointee) is an opaque
  SHELL completed on a completeness demand — the machinery exists for
  declared-only templates (instantiate_opaque_template_use,
  complete_shell_class_type, has_pending_template_instantiation); making every
  template-argument use go through it is the two-phase arc, owner-visible in
  scope. Reducer (tmp/w7list.mad; clang++ -stdlib=libc++ `2 3`):
  `#include <list>` `std::list<int> l; l.push_back(1); l.push_back(2);` iterate
  with `std::list<int>::iterator`, print size and sum.

  **MEASUREMENT #6 (run 33828999038 @67401362, 2026-09-04 02:17 UTC): arm64
  1256/12/22skip (from 1246/18), Intel 1265/3/22skip (from 1255/9) — ZERO new
  failures on either arch; the six placeholder-gap tests fixed on both
  (testinitlist, testinitlistclass, testnestedenumvec, testvartplsigchain,
  testvartplsigchainns, testvecmembercopy) — exactly the predicted residue.
  Intel's 3 == the common set == the libc++ `<list>` cycle (testforeachiter,
  testphpdumpiter, testptrcmpupcast: list:315 `basic ClassPattern could not
  resolve __base_pointer in __list_node_base`, KG Gap
  libcxx_list_node_pointer_traits_completion_cycle). arm64 adds the 9
  MIR-floor tests (unchanged list). Runner mac battery 10/1 both arches (the
  include-free value intrinsic = the standing known-open); zero c2mir
  warnings in either suite log. Exe lane advisory (arm64 984/214, Intel
  969/238, all `-static-libmadc` Tier B). Artifacts in
  scratchpad/d4-33828999038.**

  **WAVE 7, ITEM 3 LANDED (2026-09-04, three commits on d1f4f0ea: 2466bbb9
  f974dec0 2455aafa) — KG Gaps class_param_by_value_bitwise_copy and
  string_pass_by_value_semantics CLOSED (both SILENT, exit 0): a by-value
  CLASS parameter is the callee's own object.** madc lowered `void f(D d)` as
  a C struct-by-value formal: a bitwise image of the caller's argument with no
  copy constructor and no destructor — `f(a)` printed `f 1` where g++ and
  clang print `f 101` then `~101`; a callee's `s += s` on a by-value
  std::string reallocated and freed the CALLER's buffer (double free,
  SIGABRT); and a by-value class argument to a library callee compiled by
  g++/clang (which expects a pointer) was handed a struct. 2466bbb9: the
  Itanium lowering — a class non-trivial for the purposes of calls is passed
  by INVISIBLE REFERENCE: the caller copy/move-constructs the parameter object
  (a prvalue argument IS it) into a cleanup-tagged temp and passes its
  address; the callee's formal is `struct T *` and every use reads through it.
  One owner per half beside its return-side twin: class_param_via_invisible_ref
  decides the ABI; param_decl declares the formal for every signature lane and
  native_param_shape (now a CirBuilder member) the extern shape;
  object_arg_value passes the parameter object's address (its value half is
  class_object_value, the temp class_object_temp — the functional cast keeps
  the value); param_is_invisible_ref marks the callee's parameter
  pointer-stored, read by var_is_pointer_stored, the TokenVar read, the member
  access (gated on !parent_expr: a chained `p.a.b` TokenMember's object is the
  ROOT variable, flags and all — the first build turned `__this->_M_impl.x`
  into `->` in every std::vector body), the address-of arm and the capture
  forwarding; the libmadc call shim passes a text-constructed std::string temp
  by address and COPIES an instance slot into its own temp. f974dec0: the
  return side keyed on class_needs_dtor alone, so a destructor-less class with
  a user copy/move constructor was returned as a native struct VALUE — a
  returned member (`H get() { return h; }`) never ran the copy constructor
  (`g 5` for g++'s `g 105`), an xvalue member never moved, a conditional of
  locals never copied; class_nontrivial_for_calls (class_needs_dtor ||
  !class_trivially_copyable) is now the ONE predicate behind
  class_return_via_retbuf and class_param_via_invisible_ref (the shim's
  instance-cell finalizer is NULL for a class with no destruction need). 2455aafa:
  the parameter half of the implicit-move rule returns
  (returned_operand_is_implicitly_movable admits vfPARAM again — 3eee5c43 had
  excluded it while the parameter aliased the caller's object): `return h;`
  from a parameter moves, and a move-only class is returnable from its
  parameter (madc had silently bit-copied it). Reducers (g++ == clang++):
  tests/testclassparambyvalue (copy / prvalue / xvalue / two params /
  dtor-only class / trivially copyable control), tests/teststringparambyvalue
  (+ the _libcxx twin), tests/testretuserctorclass, tests/testparamimplicitmove.
  Verification: container builds 1–6 (tmp/logs/w7bv-build-*.log, zero
  warnings), class/template/string/lambda/operator/container neighbourhood
  570/0 after the chained-member gate, test_libmadc_program 133/0,
  forest_bind_gate 29/29. NEW open KG Gap
  temporaries_scope_lived_not_full_expression: every temporary madc
  materializes (the parameter object now included) is destroyed at the end of
  the enclosing BLOCK, where C++ destroys it at the end of the full-expression
  — observable as destructor timing (reducer tmp/w7bv/templife.mad: g++
  `after f: live 1`, madc `live 2`); fix shape = the per-statement flush in
  translate_block wraps a temp-bearing expression/return statement in its own
  block, a declaration keeps its var_decl outside and wraps only the
  construction. Own slice.

  **WAVE 7, ITEMS 4–6 LANDED (2026-09-04, four commits on 2455aafa: G=84e51a1d
  D=529d3824 E=28606840 F=69427eb1; + H=d4b4a51c, the `/* identity-read */`
  markers check-template-thaw-choke demanded on D's two fn_template_map
  existence reads — the first wave battery's fulltest stopped at that gate with
  the integration suite itself 1289/0; the ten gates after it were pre-run
  green before H, which also hardens the constant-cast type-id walk for
  elaborated types and fixes leg 15's C-driver derivation).** Item 4a (G, found by item 4's reducer — KG Gap
  dependent_nontype_default_arg_read_as_cast_unfolded, SILENT exit 0, CLOSED):
  a dependent non-type default template argument `bool = (A::num == 0)` never
  folded after substitution, so `Z<I<0>>` instantiated the primary instead of
  the `<A, true>` partial specialization. parse_constant_primary decided "cast"
  from the first token after `(` alone — a substituted parameter is a TYPE
  token, so `( I_0 :: num == 0 )` was swallowed to the `)` as a cast target
  and the fold failed. [expr.cast]: a cast only when the parenthesized run IS
  a type-id (gcc parses the type-id tentatively and requires the `)`) —
  constant_cast_type_id_extent walks the run non-consumingly, reusing
  peek_class_member_type_chain for a `::name` member-TYPE chain (a value leaf
  is rejected). tests/testtpldefaultdepfold.mad (g++ == clang++ `2 1 2 / 20 10
  20 / 7 0 255`). Item 4 (D, `<filesystem>` clock on linux libstdc++ —
  KG Gap dependent_qualified_less_than_read_as_template_open CLOSED): whether
  a `<` begins a template-argument-list is a NAME question ([temp.names]/3),
  decided by lookup the way gcc's cp_parser_template_name and clang's
  Sema::isTemplateName decide it — never by the token before it. madc's
  DelimDepth::angle_open_context read any identifier before `<` as a
  template-id head, so <ratio>'s `struct __ratio_less_impl<_R1, _R2, true,
  false> : integral_constant<bool, _R1::num < _R2::num> { };` opened an angle
  that never closed: the class-prefix capture ran through every later header
  to a stray `>` in <bits/types.h>, std's namespace stayed open, time.h's
  `clock` registered as std::clock and <ctime>'s `using ::clock;` failed in
  every TU including <chrono> or <filesystem>. The same token rule sat in the
  template-ARGUMENT splitter (a bare DelimDepth driven by update(): the
  include-free reducer's base clause `BC<A::num < B::num>` failed "BC<> expects
  1 argument(s)" even with the capture right) and in the expression parser
  (`int x = K::num < 5;` skipped `<...>` as template arguments on sight). ONE
  owner now, on Program: class_member_lt_reading (a member template opens; a
  data / static member, enumerator, typedef is less-than; nothing or only
  non-template functions = Unknown, legacy reading kept) and
  unqualified_name_lt_reading (type / value template parameter less-than,
  template template parameter opens, templates in scope open, a variable
  less-than, functions and undeclared names keep opening per 3.3);
  DelimDepth::lt_reads_as_less_than reads them for every scan that carries the
  Program (walking the `q0::q1::name` chain: a dependent qualifier anywhere is
  less-than, a concrete chain resolves to a class and asks, `typename` / an
  elaborated keyword / a base-specifier head keep opening = gcc's `tag_type !=
  none_type`); resolve_class_qualified_expression reads the same predicate.
  Parameter KINDS now travel everywhere a parameter list does
  (ParsedTemplateParameterList.is_template_template, TemplateParamScope
  frames carry typeparam_is_type). Every stream scan constructs its tracker
  WITH the Program (`DelimDepth d(this)` / `d(&pgm)`) and
  check-one-delim-tracker.sh fails a bare tracker driven by update() in a
  stream-walking function — the shape the bug lived in (negative control: 8
  pre-conversion sites flagged). Reducers tests/testtplargless.mad (g++ ==
  clang++ `1 0 1 / 5 5`) and tests/testlessthanqualified.mad (`1 1 0 / 1 0 1 /
  1 0 1 / 1 4 7`). Residue, stated: a template-id qualifier spelled in source
  inside an argument list (`BC<I<7>::num < 5>`) is not resolvable from tokens
  and keeps the opening reading in a token scan. Item 5 (E, filebuf D1Ev
  conflicting externs — KG Gap member_dtor_extern_declared_void_ptr_conflicts_
  with_typed_proto CLOSED): class_member_destruct declares an aggregate's
  member destructor with ONE typed shape `void sym(struct mc *)` whether
  madc-emitted or bound to a library export (the Itanium signature either
  way) — libc++ basic_filebuf<char>::~basic_filebuf is exported AND parsed
  from <fstream>, so the old `extern void ..D1Ev(void *)` conflicted with the
  typed definition; c2mir tolerated it, clang rejected the emitted C11
  (testofstreamwrite / testmanipview under -stdlib=libc++). Item 6 (F, iomanip
  live memmove undeclared — KG Gap libcxx_iomanip_live_memmove_undeclared
  CLOSED): the Pass-0.75 referenced-function prototype sweep is one lambda
  (referenced_funcdef_protos) run again at Pass 1.95 into late_list for the
  header-declared functions a body the Pass-1.9 fixpoint materialized LATE
  referenced first (`memmove` inside libc++ __constexpr_memmove); c2mir
  implicit-int'd the call, clang rejected the emitted C. Gate: libcxx_gate.sh
  leg 15 — the emitted C11 of testofstreamwrite / testmanipview / testiomanip
  under libc++ is strict-C11 clean (clang -std=c11 -fsyntax-only), with a
  negative control (a conflicting redeclaration appended must be rejected).
  Verification: container build-11 rc=0 (zero warnings), the two D reducers
  and every tal*/clk* probe at oracle, the three emit-C legs 0 errors,
  neighbourhood subset 534/0/6skip (templates, classes, containers, streams, constants, casts, enums, C89), forest_bind_gate 29/29. `<filesystem>`
  NEXT FRONTIER (both g++-trivial, reducers tmp/w7bv/): `struct path::_Cmpt :
  path {` out-of-line NESTED class definition with base clause -> "Expecting
  identifier after type" (oolnested.mad); `namespace fs = std::filesystem;`
  NAMESPACE ALIAS -> "Expecting '{' after namespace name" (nsalias.mad).

  **MEASUREMENT #7 (run 33882552642 @dc9baf0d, 2026-09-04 14:14 UTC): arm64
  1262/14/22skip (from 1256/12), Intel 1271/5/22skip (from 1265/3); zero
  c2mir warnings; runner mac battery 10/1 both. The common set = the 3
  libc++ `<list>` cycle tests + 2 NEW: teststringparambyvalue and its _libcxx
  twin (wave-7 item 3's own reducers) — `std::string a(20, 'x')` died
  "no matching constructor for call to 'basic_string_view<char>(int32_t*)'"
  under the darwin PACK on both arches while the live parse passed.
  Reproduced on linux: `bin/madc-x86-64-macos --forest-bind=obj/hosted-
  x86-64-macos/forest.bin -stdlib=libc++ --emit=c11 tmp/w7bv/dp2.mad` fails,
  `--no-forest-bind` passes. arm64 adds the 9 MIR-floor tests (unchanged).**

  **WAVE 7b LANDED (2026-09-04, seven code commits on dc9baf0d: A=d624d03e
  B=061bb0ca C=a21e31cd E=eca465e9 F=ce331a34 D=636d30de N=29aca401, plus
  G=7beeb022, a comment-only reword — the battery's fulltest ran the
  integration suite 1296/0/9skip and STOPPED at its second gate,
  check-no-std-hardcoding: the commit-D comment spelled
  "string_view-constrained" and the gate reads comments (`\bstring_[a-z]`;
  "not even a comment"); the gates after it were pre-run on G's binary,
  tmp/logs/w7d-pregates.log, 85/85; W=e398fabd
  tests/testsizeofqualified.win64_expect — the wine lane's one failure was
  this test's linux expectation on an LLP64 target, madc == mingw-g++ there;
  L=2630aca5 the ledger RELEASE tier, see docs/rules/branching.md). LANES
  @e398fabd (tmp/logs/w7d-battery.log): linux jit 1296/0/9skip exe 1237/0 obj
  1237/0 packed 1296/0/9skip headerless 1265/0/40skip (forest_pack_gate linux
  68 = baseline; forest_bind_gate 29/29; warn ratchet GREEN; tsubst flag-on
  GREEN); wine64 1242/0/63skip (re-run with the fixture, rb-20260904-163837;
  first run 1241/1) + verify_pe_release OK + win64 pack 68 = baseline;
  c-testsuite 220/220 baseline EMPTY; macos cross release both arches 835
  units, darwin pack 48 = baseline both (mir-blob-skips 1/1), verify_macho OK;
  DARWIN-PACK REPRO on the rebuilt cross madc: tmp/w7bv/dp2.mad,
  teststringparambyvalue and its _libcxx twin all rc=0 under
  `--forest-bind=obj/hosted-x86-64-macos/forest.bin -stdlib=libc++ --emit=c11`
  — commit D closes dispatch #7's two new failures; dispatch #8 confirms on
  the runners.** The `<filesystem>`
  frontier peeled three layers and exposed a fourth underneath. A (aggregates
  ONE object): `struct B; B *g; struct B { int w; B(int); }` then `g->w` failed
  "no member named 'w'" at GLOBAL scope — the struct parser minted a
  DataDefSTRUCT placeholder and the class parser, unable to complete it in
  place, REPLACED it (every DataDefPTR::base_type typed against the
  declaration stayed on the empty object). gcc xref_tag keeps one node ("the
  forward-reference will be altered into a real type"); clang's
  redeclarations share one RecordType; both make the class-key a property of
  the one type (CLASSTYPE_DECLARED_CLASS / isClassCompatTagKind). Now:
  new_incomplete_aggregate decides the placeholder's KIND by mode (a
  DataDefCLASS wherever a struct-key body may take the class parser),
  struct_tag_or_implicit_forward is the one implicit-forward mint (5 raw
  copies folded), TokenSTRUCT::parse builds the body INTO the prior
  (incomplete_prior_aggregate; the field-by-field copy arm is gone) and runs
  compute_layout for every class it completes; the class parser's replacing
  arm is gone. tests/testfwdstructclassdef (g++ == clang++ `4 4 4 / 7 4 / 10
  5 / 9 / 12 3 y`). B (nested prior in the OWNER scope): the struct parser
  keys a nested placeholder `A::B`, the class parser `A__B` — it probed its
  own key only and minted a second object; it now asks the owner-scope lookup
  (gcc cp_parser_class_name in the nested-name-specifier's scope; clang
  LookupQualifiedName). tests/testnestedfwdclassdef (`1 4 6 / 5 1`). C
  (struct-key qualified head): `struct A::B : A {` never reached the class
  parser — cpp_struct_body_needs_class_parser only knew `{`/`:` right after
  the tag; qualified_class_head_starts_definition walks the
  nested-name-specifier (DelimDepth d(this)) and answers gcc's
  cp_parser_nth_token_starts_class_definition_p question. tests/
  testoolnestedstructhead (`3 4 4 / 7 8 1 / 5 2`); fs_path.h:859 passed. E
  (sizeof of a QUALIFIED type-id): `sizeof(O::N)` measured O and died on the
  `::` — the `!dd &&` guard on the declared-type route; tests/
  testsizeofqualified (`8 8 4 / 4 8 1 / 16 12`). F (out-of-line member of a
  NESTED class): resolve_qualified_class_owner knew `ns::Class` only; a
  class-rooted chain now descends by resolve_class_type_alias ([class.qual],
  the scan_resolve_qualifier_chain descent over names); tests/
  testnestedoutoflinemember (`13 2 42 8 4`); fs_path.h:1359 passed. D (the
  darwin failure — SFINAE fidelity, KG Gap sfinae_constrained_ctor_template_
  instantiated_past_its_guard CLOSED): libc++'s `basic_string(const _Tp&,
  const allocator&)`, constrained on __can_be_converted_to_string_view, was
  INSTANTIATED for _Tp = int in every lane (MADC_MTB_PROBE `inj
  BS__BS__o25<int32_t,0,>`); the live lane never lowered the losing instance,
  the pack lane did. Two roots in the trait, each a silent wrong answer: (a) the scalar
  rule let any arithmetic type convert to a pointer ([conv.ptr] admits only a
  null pointer constant) — is_convertible<const int&, const char*> read 1;
  (b) the trait refused every scalar<->class / unrelated class pair as
  "cannot faithfully evaluate" — is_convertible to a class is copy-init
  through a non-explicit converting ctor ([over.match.copy], [class.conv.ctor],
  no nested user conversion per [over.best.ics]/4): trait_class_constructible
  gained a copy_init form (FuncDef::is_explicit skipped; an unrelated class
  arg not viable; aggregate params of either object kind); also a trait
  operand's cv on the pointer itself (`const char* const&`) parses. With the
  trait folding to 0 the constraint arg is concrete, `enable_if_t<0, int>`
  has no `::type`, and instantiate_template_alias_use's concrete-arg branch
  returns the substitution failure. A third edit — a NON-dependent but
  UNFOLDABLE alias arg as failure — was tried and REVERTED: it flipped every
  libc++ guard on a trait madc cannot fold into rejection (teststrdefault_
  libcxx: `__assign_trivially_copyable` undefined import); those traits are
  KG Gap unfoldable_trait_in_sfinae_guard_accepted_by_placeholder
  (is_assignable<char&, const char&>, __has_input_iterator_category<char*>,
  __can_rewrap, __is_segmented_iterator …) — and the same disposition keeps
  the iterator-pair ctor `basic_string(_InputIterator, _InputIterator)`
  instantiated for int (`__has_input_iterator_category<int>` does not fold),
  a losing instance the PACK lane may still lower (D2). Dispatch #8 tells.
  tests/testsfinaeconvertible
  (`1 13 / 0 0 1 0 / 1 1 1 1 / 1 0 0 0`); libc++-free reducers tmp/w7bv/
  sf3 sf5 sf6 cv4 cv5. After D the constraint FAILS for int (MTB `FAIL
  BS__BS__o25 constraint[1]`); dispatch #8 re-measures the pack lane. NEW KG
  Gap pack_lane_lowers_unselected_ctor_template_instance (D2): the pack lane
  lowers an instantiated ctor body the live lane never emits — a LOADED !=
  parsed emission-set divergence, its own slice. N (namespace alias,
  [namespace.alias]): `namespace b = a;` — Program::namespace_aliases
  ("scope::alias" -> canonical target) read FIRST by canonical_nested_
  namespace, the one existence probe every qualifier / type / function /
  template / using-directive resolution passes; transported by the pack as
  DK_NSALIAS (CIR_FOREST_FORMAT_VERSION 44); gcc do_namespace_alias's
  DECL_NAMESPACE_ALIAS unwrapped by ORIGINAL_NAMESPACE, clang's
  NamespaceAliasDecl. tests/testnamespacealias (`1 6 5 7 9 / 1 6 1 5 / 1 5`).
  `<filesystem>` NEXT FRONTIER: fs_path.h:1433 `__path_iter_distance`
  "use of undeclared identifier" — a name used in an inline member body
  before its declaration later in the header (complete-class context /
  deferred body lookup). ALSO MEASURED, filed: 77 token-level hand-rolled
  delimiter counters in parser.cpp (LT 12, OpBrc 14, OpBrk 41, OpSqr 10 —
  template_class_head_is_qualified and cpp_struct_body_needs_class_parser
  among them); check-one-delim-tracker.sh keys on variable NAMES and misses
  them all — KG DupFamily token_level_delimiter_counters; the migration + a
  token-level ratchet marker is the next tracker slice. Verification per
  commit: build rc=0 zero warnings, reducer == g++ == clang++, neighbourhood
  subsets 0 failures, forest_bind_gate 29/29, smaug_gate OK (A), libcxx_gate
  OK (D).**

  **MEASUREMENT #8 (run 33902996610 @bbedcc4c, 2026-09-04 17:54 UTC): arm64
  1269/14/22skip, Intel 1278/5/22skip — passes +7 (the wave's tests), failures
  UNCHANGED in count: the two std::string(n, c) tests moved ONE LAYER DOWN. #7:
  `cir error: no matching constructor basic_string_view(int32_t*)` (closed by
  D). #8: `MIR error: import of undefined item basic_string_..._init_with_
  sentinel` — the losing iterator-pair constructor instance
  `basic_string(_InputIterator, _InputIterator)` for (20, 'x') was LOWERED in
  the pack lane (defined before `main`, as a user ROOT) together with its
  `__init<int,int>` helper, whose body calls the never-instantiated
  `__init_with_sentinel<int,int>`; live emitted neither. The linux repro had
  been emit-only (`--emit=c11 rc=0` cannot see a MIR link error) — the
  emitted C shows it: a call with no definition. Runner mac battery 10/1 both.**

  **WAVE 7c LANDED (2026-09-04, two fixes + a gate, on bbedcc4c: PR=360831da
  token provenance, DE=530187d7 deduction consistency).** The root was two
  layers below the pack. (1) PROVENANCE: TokenBase::clone() and its 118
  overrides construct a fresh token, and TokenBase's constructor stamps it
  with the CURRENT parse position — every parser-side copy of a template
  pattern, default argument, call argument or substitution run lost its
  origin. The injected member-template body was attributed to whatever token
  the parser had last consumed: live got enable_if.h BY LUCK (the constraint's
  `= 0` default) — a library function, unreferenced, never emitted; the pack
  lane got the USER file (the restored pattern's tokens had been frozen with
  the producer TU's file, for the same reason) — a user ROOT, emitted
  unconditionally. Two earlier fixes had met the same stamping and patched
  around it locally (`_parse_*` redirected across a clone loop: class-template
  instantiation, the lazy capture); the member-template injection had none.
  gcc/clang locate an instantiation at the template's definition; a
  macro-expanded token at its expansion site. Fix: TokenBase::clone_origin()
  (tokens.h) is the one owner of "copy this token" — all 146 parser.cpp copies
  and the builder's tsubst run copy read it; src/lexer.cpp's five
  macro-replacement clones stay bare by canon. Gate: scripts/check-clone-
  origin.sh in fulltest (negative control; the lexer the one exempt file).
  Rule bullet in mc11-ir.md. Probes kept: MADC_ROOTSPLIT_PROBE (the builder's
  root/library verdict + parseFunction's `{`-token attribution), the MTB
  `prov` line. Before/after on the pack lane: `file=tests/teststringparam
  byvalue.mad line=21 -> root` / `file=.../c++/v1/string line=1066 -> lib`;
  the refrozen x86-64 darwin pack's emitted C defines no o25, no
  `__init__o4__mti`, mentions no `__init_with_sentinel`; darwin pack gate 48 =
  baseline. KG Gap pack_lane_lowers_unselected_ctor_template_instance (D2)
  CLOSED by this root — the emitted definition sets of the two lanes now differ
  only by symbol NAMES (Gap pack_vs_live_instantiation_symbol_naming_divergence,
  low: `__mti__o3` vs `__mti`, `unsigned_long` vs `uint64_t` in
  __libcpp_numeric_limits, hash suffixes). (2) DEDUCTION: the call lane's
  deduction loop SKIPPED any argument whose parameter named only already-bound
  template parameters (a rule written for EXPLICIT template arguments), so
  `(20, 'x')` deduced _InputIterator = int from the first argument and never
  looked at the char — [temp.deduct.type]/2 makes that a deduction FAILURE
  (g++: "deduced conflicting types for parameter 'It' ('int' and 'char')"); the
  candidate must not exist. Now explicit bindings keep the skip (P is concrete
  after substitution), a parameter an earlier ARGUMENT deduced is deduced again
  through decayed_for_deduction (the one owner of the [temp.deduct.call]/2
  adjustments: function/array decay AND the top-level cv drop — `const int ci;
  pick(ci, 2)` deduces T = int, where the old block bound T = const int) and
  compared through deduced_bindings_conflict (the one owner; the free-operator
  walker's inline compare reads it too). tests/testdeductionconflict.mad (g++ ==
  clang++ `2 23 9 1`): the user-code twin `template<class It> W(It, It)` called
  as W(20, (char)3) had instantiated an ill-formed body (`*a` on an int) and
  failed to compile. With both fixes the iterator-pair candidate is never
  instantiated for the real test (MADC_ROOTSPLIT_PROBE prints nothing). NOT
  fixed here, recorded: tmp/w7bv/dp2.mad `std::string a(20, 120)` — a
  CONSISTENT (int, int) deduction keeps the candidate, its constraint
  `__has_input_iterator_category<int>` cannot fold (KG Gap unfoldable_trait_
  in_sfinae_guard_accepted_by_placeholder: the `decltype(__test<_Tp>(nullptr))`
  detection idiom), the template WINS as the exact match, its body references
  the silently-dropped ill-formed `__init_with_sentinel<int,int>`: undefined
  import in EVERY lane (live linux libc++ too) — the trait-fold slice.
  Verification: reducer == g++ == clang++ (runner 1/0); a 415-test
  template/string/container subset 0 failures; forest_bind_gate 29/29;
  libcxx_gate OK; tsubst flag-on GREEN; class_pattern_equivalence OK;
  smaug_gate OK; refrozen x86-64 darwin pack: no o25 / __init__o4__mti /
  __init_with_sentinel in the emitted C, pack gate 48 = baseline. (3) THE GATE
  CAUGHT THE FIX: the first wave-7c battery's fulltest ran integration
  1297/0/9skip on PR+DE and STOPPED at scripts/check-fn-template-deduction-
  owner.sh (`expression-aware deduction delegates: 3 (target 2)`) — DE's
  already-bound arm re-deduced with its OWN deducer call, a second scalar
  deduction site beside the general block's, each with its own copy of the
  agreement check: the drift that gate exists to prevent. DG=02a76f49:
  deduce_scalar_arg (a lambda beside decayed_for_deduction) is the ONE step —
  the deducer with the argument expression, then the [temp.deduct.type]/2
  agreement check through deduced_bindings_conflict, else the fresh binding —
  and both callers read it, differing only in what an undecomposable shape
  means (the arm keeps the skip, the block fails the candidate). Behaviour-
  preserving; the battery re-ran on it.**

  Lanes at 02a76f49 (2026-09-05, tmp/logs/w7f-battery.log): linux jit 1297/0/9skip exe 1238/0 obj 1238/0 packed 1297/0/9skip headerless 1266/0/40skip, fulltest rc=0 with every gate GREEN (clone-origin and deduction-owner included), forest_pack_gate linux 68 = baseline, forest_bind_gate 29/29, warn ratchet GREEN (0 warnings), tsubst flag-on GREEN; wine64 1243/0/63skip + verify_pe_release OK + win64 pack 68 = baseline; c-testsuite 220/220 baseline EMPTY; macos cross release both arches 835 units, darwin pack 48 = baseline both (mir-blob-skips 1/1), verify_macho OK; darwin pack check on the rebuilt cross madc + refrozen x86-64 pack: teststringparambyvalue(+_libcxx) emitted C has 0 o25 definitions and 0 __init_with_sentinel mentions; dp2 keeps its sentinel (the trait-fold gap, expected). Battery 1 on 360831da stopped at the deduction-owner gate; 02a76f49 is that fix.

  **MEASUREMENT #9 (run 33920437418 @801ae480 — code 02a76f49 — 2026-09-04
  21:18 UTC; the owner dispatched it as `suite_gate=fals`, which the workflow's
  `= "true"` test reads as measurement mode): arm64 1272/12/22skip, Intel
  1281/3/22skip — EXACTLY THE PREDICTION. Passes +3 on each arch versus #8 (the
  two std::string(n, c) tests closed by wave 7c — provenance + deduction — and
  the new testdeductionconflict); zero new failures. The Intel 3 are the libc++
  <list> tests (testforeachiter, testphpdumpiter, testptrcmpupcast); the arm64 12
  are those 3 plus the 9 MIR aarch64-floor tests, list unchanged
  (testbuiltincomplexparts / testbuiltinconjf, testcomplexretconv,
  testgccvectorbitwisenot / testgccvectorlit / testgccvectorsizeexprbitwise,
  testint128, testprintfdouble, teststdiobuiltinredirects). Zero c2mir
  warnings in the suite logs. Runner mac battery 10/1 both (the include-free
  value intrinsic, the standing known-open). Exe lane advisory (arm64 1000/214,
  Intel 990/233; -static-libmadc Tier B until D5). THE DARWIN RESIDUE IS NOW
  EXACTLY THE TWO ARCS: libc++ <list> (the two-phase completion arc, owner
  scope; it never worked — the 2026-08-16 libc++ lane run predates the tests)
  and the MIR aarch64 floor — both PROMOTION BLOCKERS under the 2026-09-04 law
  (darwin-suite green on both arches, or every failure a filed .darwin_skip
  with its reason).**

  **MEASUREMENT #10 (run 33934880333 @df6c620e — code b152299f — 2026-09-05
  01:02 UTC, `suite_gate=false`): arm64 1279/6/23skip, Intel 1283/3/22skip —
  EXACTLY THE PREDICTION, and exactly the arm64 Mac's own full-suite tally on
  the same content (tmp/logs/w8-mac-fullsuite.log). Passes +7 on arm64 and +2
  on Intel versus #9; zero new failures. The arm64 6 = the 3 gccvector tests
  (testgccvectorbitwisenot, testgccvectorlit, testgccvectorsizeexprbitwise —
  the aarch64 V128 SIMD arc, docs/plans/2026-09-05-aarch64-simd-v128.md) + the
  3 libc++ <list> tests (testforeachiter, testphpdumpiter, testptrcmpupcast);
  the Intel 3 = the <list> tests. Wave 8's six MIR-floor fixes hold on the
  runner (testbuiltincomplexparts, testbuiltinconjf, testcomplexretconv,
  testint128, testprintfdouble, teststdiobuiltinredirects all pass). Exe lane
  advisory (arm64 1012/210, Intel 992/233). Triage:
  tmp/d4-triage/d4-33934880333.**

  **WAVE 9 LANDED — THE aarch64 SIMD (MIR_T_V128) ARC, S1-S4 (2026-09-05,
  s155; plan docs/plans/2026-09-05-aarch64-simd-v128.md). The three gccvector
  tests were the last MIR-floor residue after wave 8: the fork's <=16-byte
  vector support is target-independent above the backend and was x86-64 only —
  mir-gen-aarch64.c, mir-aarch64.h, mir-aarch64.c had ZERO V128 sites, so a
  vector argument rode the integer lanes (8 of 16 bytes; the two SILENT tests)
  and a V128 memory operand met a table with no vmov (the loud one). One rule:
  a V128 is an FP-class 128-bit register value, the LD-in-Q shape the backend
  already moves (the LDMOV rows), saves (V8-V15 at a 16-byte stride) and passes
  (v[NSRN], a 16-byte stack slot). S1 d123c3b0 register class + moves + spills +
  the AAPCS64 short-vector convention (fp_class_type_p / stack_arg_16_p, one
  predicate per ABI fact; mir-gen.c's get_move_code forward-declared as the one
  move-code owner; the mv/Mv letters; va_arg_builtin's vector arms; the NEON
  encoding oracle promoted into the subtree, aarch64-neon-encodings.md);
  S2 715dc569 and/orr/eor, add/sub .16b/.8h/.4s/.2d, mul .8h/.4s; S3 43b4f307
  fadd/fsub/fmul/fdiv .4s/.2d; S4 797d88b8 cmeq, signed cmgt, fcmeq, fcmeq+mvn,
  fcmgt/fcmge with swapped operands, ushl/sshl per-lane shifts, neg+shift for
  the per-lane right shifts, umov/[neg]/dup/shift for the scalar-count shifts on
  the fixed temps X9 + V16. Gate: ALL 8 qemu reducers == aarch64 gcc at c2m's
  default level and -O2 (tmp/logs/simd-qemu-3.log). D5 IN ACTION — the S4
  reducers, plain GNU C green under gcc and clang, exposed THREE FRONT-END gaps
  (each fixed on develop first, its own commit + gcc==clang reducer):
  ef156cac parser (a SIMD-typed EXPRESSION is subscriptable — `(a + b)[2]` fell
  to the lambda introducer; TokenBand/Bor/Xor type by the usual arithmetic
  conversions, C11 6.5.10-12, where they were bare `int`; a vector comparison
  types at reduce time as gcc's opaque signed-lane vector;
  Program::simd_type owns the anonymous vector types), 15126633 c2mir (a vector
  comparison is gcc's OPAQUE vector of SIGNED lanes — `(ua == ub)[0]` widened to
  4294967295, a SILENT wrong answer on every target; a double compare into
  `long long` lanes was rejected), 90990307 c2mir (a vector shifted by a scalar
  count of any integer type — the count went through the lane-fit test).
  Mac (arm64, the cross release build staged at ~/madc-s154): 1285 pass / 3 fail / 23 skip (tools/ and examples/ staged) — the 3 libc++ <list> tests only (testforeachiter, testphpdumpiter, testptrcmpupcast); the 17 vector tests, the 3 formerly-failing gccvector tests and the 3 new front-end reducers included, 17/0 (tmp/logs/simd-mac.log); release-arm64-macos 835 units, forest_pack_gate OK, verify_macho OK
  Lanes at 797d88b8 (tmp/logs/w9-battery.log): linux battery jit 1302/0/9skip exe 1243/0 obj 1243/0 packed 1302/0/9skip headerless 1271/0/40skip, fulltest rc=0 with every gate in the chain GREEN (check-c-abi-surface OK: 1090 extern-C exports classified, 43 listed internal; warning ratchet GREEN, 0 warnings over 1311 compiled; tag-arithmetic 0/0; tsubst flag-on GREEN; forest_pack_gate linux 68 = baseline; forest_bind_gate all OK); wine64 1248/0/63skip + verify_pe_release OK (234 units) + win64 pack 68 = baseline; c-testsuite 220/220, 0 outside baseline; macos cross release both arches 835 units, darwin pack 48 = baseline both (mir-blob-skips 1/1), verify_macho OK both (tmp/logs/w9-battery.log).
  OPEN in the arc: S5 — the ff_call / interp shims (c2m's surface, not madc's)
  with the two recorded shim defects (KG Gaps
  mir_ff_call_shim_val_stride_long_double,
  mir_aarch64_apple_interp_shim_stack_fp_arg_offset_unencoded) and the Apple
  va_arg arm's oracle through an arm64-macos c2m. THE DARWIN RESIDUE IS NOW ONE
  ARC: libc++ <list> (owner scope). Dispatch #11 measures wave 9 (expect arm64
  3 = the <list> tests, Intel 3).**

  **WAVE 8 LANDED — THE MIR aarch64 FLOOR (2026-09-05; the owner switched the
  queue to MIR with libc++ <list> after it, and pointed at upstream
  vnmakarov/mir #472-#475, #429, PR#466, PR#439). Seven root commits on
  35e581c6, one root each, plus the two gate fixes and the SIMD arc plan
  (0a776767). LOOPS: the arm64 Mac (`ssh madc-mac`,
  staged `~/madc-s154`: the container's `release-arm64-macos` binary, tests/,
  tools/, examples/, a perl `timeout` shim; the darwin runner's skip domains)
  and qemu on the container (an aarch64 c2m under `qemu-aarch64-static -L
  /usr/aarch64-linux-gnu`, oracle `aarch64-linux-gnu-gcc -static`; reducers
  tmp/mirq/*.c; toolchain + sysroot links pinned by PROV=dcd836b0). ROOTS of
  the nine (six fixed): (1) VA=fd487dc0 — madc modelled `__builtin_va_list` as
  the SysV x86-64 `__va_list_tag[1]` on every non-LLP64 target; Apple arm64's
  is a scalar `char *`, so every va_list a MIR variadic handed to libc was the
  record's ADDRESS (teststdiobuiltinredirects `hA> 0`, testprintfdouble
  `0.00` then SIGBUS; MIR's own va_arg, which takes the address, was right).
  The va_list SHAPE is now the fifth target property beside the data model,
  bit-field ABI, OS and int64 spelling: TargetVaList {SysVTagArray,
  AAPCS64Struct, Scalar} (datadef.h; default_target_va_list; builtin_va_list_
  type; use_builtin_va_list; the lexer's __builtin_va_copy body; stdarg.h's
  va_copy -> the builtin, the `#ifdef _WIN32` twin gone) and c2mir's
  SCALAR_VA_LIST_P (MIR_TARGET_WINDOWS_P || Apple && aarch64) replaces the two
  host-keyed `_WIN32` va_arg/va_start arms. testbuiltinvalisttypedef (a
  compile-error test for the ARRAY shape's `ap = 0` diagnostic) gets the
  darwin-arm64 skip its win64 twin has. (2) CX=93010447 — a `_Complex`
  argument is an AAPCS64 Homogeneous Floating-point Aggregate in consecutive
  FP registers; c2mir's aarch64 target passed a GPR block, so libm conjf /
  crealf / cimag (the lexer's __builtin_conj* map) read garbage and the tests'
  OWN abort() on the wrong value read as MIR SIGABRTs (testbuiltinconjf,
  testbuiltincomplexparts; madc's darwin signal handler prints no frames —
  KG Gap darwin_signal_handler_backtrace_empty). caarch64-ABI-code.c gains the
  complex arms (two FP vars, two complex_load ops, the callee gather), gated
  on a new gen_ctx flag call_arg_vararg_p: varargs keep the block shape MIR's
  va_block_arg reads. (3) LD=7d63083a — MIR_LD_IS_D canonicalized data, proto
  results, func vars and memory operands but NOT registers (new_func_reg) nor
  create_proto's copied result/argument types: `i2d ... Got 'ldouble'` then
  `call: operand #3 Got 'double', expected 'ldouble'` (testcomplexretconv,
  testcomplexfwddeclparams); canon_type reaches every typed slot MIR mints.
  (4) I128=63a6dd6f — caarch64-ABI-code.c had no __int128 arm (`wrong result
  type in proto`); reg_aggregate_size answers 16 (x0:x1; arguments already
  rode the BLK lane); verified under qemu against the gcc oracle line for
  line. (5) The three gccvector tests are NOT fixed: mir-gen-aarch64.c and
  mir-aarch64.c have ZERO V128 sites (the fork's SIMD is x86-64 only) — the
  aarch64 SIMD arc, planned in docs/plans/2026-09-05-aarch64-simd-v128.md
  (KG Gap aarch64_v128_simd_missing). UPSTREAM: #472 REPRODUCED + FIXED
  X472=0a2d3e8e (a file-scope `extern` WITH an initializer is a definition,
  C17 6.9.2p1; gcc-worded warning; tests/testexterninit); #473 REPRODUCED +
  FIXED X473=49741295 (anonymous-union members carry the union's alias class
  in N_FIELD / N_DEREF_FIELD; -O2 only — madc's default is -O1 —
  tests/testanonunionpun with a `-O2` .flags); #475 not in our tree (the
  reload-mem-ops guard exists); #474 present (win64 per-call spill space,
  perf, later); #429 both shapes pass on the Mac; PR#466 already adopted;
  PR#439 nothing needed (madc accepts `int f(...)`). Both fixes are candidate
  upstream PRs (owner review gates submission). VERIFICATION: pass 2
  (tmp/logs/w8b-verify.log) Mac nine 6 pass / 3 gccvector fail, broader
  varargs/printf/stdio/complex/int128/ldouble subsets 113/0, linux subsets
  127/0, unit rc=0, release-arm64-macos rc=0; the FULL arm64 Mac suite on
  this content (tmp/logs/w8-mac-fullsuite.log): 1275/10/23skip, of which 4
  were stage artifacts (tools/, examples/ and tmp/ not staged) that pass after
  staging — the REAL tally 1279/6: the 3 gccvector + the 3 libc++ <list>
  tests, exactly the dispatch-#10 prediction. THE GATE CAUGHT THE WAVE AGAIN:
  battery 1's fulltest ran integration 1299/0/9skip and stopped at
  scripts/check-c-abi-surface.sh — `UNDECLARED-madc_-EXPORT madc_target_
  va_list`: the fifth target property joined the code without joining
  scripts/c-abi-internal-exports.txt, where its four siblings are
  dispositioned "engine global: target ABI facts"; 0149306d lists it (scripts
  only; the ledger's code paths include scripts/, so battery 2 re-ran on it).
  AND A SECOND TIME: battery 2's chain passed that gate and stopped at the
  warning ratchet (scripts/warn_census.sh --check: `3 > 0 testexterninit.mad`)
  — the #472 reducer's `extern int c = 7;` lines warn BY DESIGN (gcc and clang
  warn on the construct too; 0a2d3e8e gave c2mir gcc's wording) and a test
  without a baseline entry allows zero. Neither a baseline allowance (the
  ratchet only goes down) nor a weaker reducer (every such declaration warns
  everywhere) was right; the missing piece was the compiler's own way to say
  "this program provokes diagnostics on purpose": gcc's `-w`. W=b152299f
  adds `-w` (thread_local madc_no_warnings -> c2mir's ignore_warnings_p at
  both option fills, --help, docs/usage.md, the C-ABI disposition in the same
  commit) and tests/testexterninit.flags carries it; battery 3 ran on that.
  Recorded refinements (KG): variadic complex HFA conformance, AAPCS64's
  even-pair rule for a 16-byte fundamental argument, struct HFAs still GPR
  blocks, the darwin backtrace.**

  Lanes at b152299f (2026-09-05, tmp/logs/w8-battery3.log): linux battery jit 1299/0/9skip exe 1240/0 obj 1240/0 packed 1299/0/9skip headerless 1268/0/40skip, fulltest rc=0 with every gate in the chain GREEN (check-c-abi-surface OK: 1090 extern-C exports classified, 43 listed internal; warning ratchet GREEN, 0 baseline warnings over 1308 compiled; tag-arithmetic 0/0; tsubst flag-on GREEN; forest_pack_gate linux 68 = baseline; forest_bind_gate all OK); wine64 1245/0/63skip + verify_pe_release OK (234 units) + win64 pack 68 = baseline; c-testsuite 220/220, baseline EMPTY; macos cross release both arches 835 units, darwin pack 48 = baseline both (mir-blob-skips 1/1), verify_macho OK both (tmp/logs/w8-battery3.log; battery 1 stopped at check-c-abi-surface, battery 2 at the warning ratchet).

  **STILL OPEN after batch 2 (the first two closed in wave 2 above; the rest in value order):**
  - **`long` vs `long long` identity on darwin (std::max undefined import 16
    both arches + testtypedefarg + likely basic_string __init_with_sentinel
    5).** Apple's headers alias int64_t/uint64_t to `long long`, a distinct
    type from `long` (Itanium x vs l), while the builtin ddINT64/ddUINT64
    are DISPLAY-named "int64_t"/"uint64_t" with an EMPTY canonical C++
    spelling; a spelling round-trip through that display name resolves to
    the darwin typedef and changes the type: `template<> struct isL<long>`
    registers as `isL_int64_t` (before the pack's typedef seeds load) while
    the use `isL<long>` instantiates `isL_long_long` (after); vector's
    `__recommend` calls `std::max<size_type>(2 * __cap, __new_size)` with
    size_type = unsigned long, the pack holds only `max<unsigned long long>`
    instantiations, the CIR-time rank finds no viable candidate
    (`a0=uint64_t` vs `unsigned long long`), std_free_function_instantiation
    fails to mint `max<unsigned long>` and the call binds the bare
    placeholder. A user `std::max<unsigned long>(a, b)` fails the same way;
    `<unsigned long long>` works. Fix at the type model: the builtin scalar
    dds carry their target C++ spelling from init (mangle_scalar_spelling's
    table), never a typedef name; no spelling round-trip may change a type.
  - **Strict-mode `<stddef.h>` inside a served libc++ header** (`--std=c++17`
    `#include <iostream>` on a Mac WITHOUT the baked libc++ path: `<cstddef>`
    #errors because `<stddef.h>` resolved to the embedded freestanding header
    instead of libc++'s wrapper unit). The runner has the staged tree at the
    baked path so its suite never sees it — the tarball user does. Own slice;
    consider running the runner suite with the staged input trees REMOVED
    (the darwin headerless cell, nearly free on the runner).
  - **Nested-std class name leak (testclassproto):** with the darwin pack
    bound, a user `class path` under `using namespace std` collides with
    `std::__fs::filesystem::path` (renaming the class passes) — a
    bare-name registration of a nested-namespace seed.
  - Grove fidelity families (both arches): list `__base_pointer` ClassPattern
    (3), initializer_list private `__begin_` (2), istream `traits_type` (1),
    `__filesystem/path.h` `format` (2), iomanip shifts (1), istringstream
    type (4), `.str()` husks (3), `__init_with_sentinel` (5, likely the
    identity class), `__ns_outer__detail_make_sig`/`count`/`make`/`pick`
    placeholders (5).
  - testvartpldepparams (parse error on darwin only, zero includes; the
    bisect variants ran silent), testint128 / SIMD insn matching (MIR
    aarch64), c2mir `i2d` on an `ldouble` operand (testcomplexretconv —
    re-measure after item 14). [wave 8: testint128 and testcomplexretconv
    CLOSED; SIMD = the aarch64 V128 arc plan.]
  2. **`__BLOCKS__` in the darwin predefined table — ~20, both arches.**
     A clang feature claim GCC never makes; on a Mac WITH the Command Line
     Tools installed, Apple's real SDK headers (`_stdlib.h` atexit_b /
     qsort_b, ...) take their block branches and madc fails at the first
     `(^)` declarator. Dropped in `gcc_posture_filter.sh`, the ONE posture
     filter (with `__clang__`/`__llvm__`/`__VERSION__`).
  3. **`-static-libmadc` cannot cover the dialect runtime (Tier B) — 169
     arm64 / 212 Intel exe-build failures, one message.** Every
     runtime-needing program: `madc_puti` & co. exist only as host
     objects. Owned by D5 (the runtime dylib). Until then the darwin exe
     lane is ADVISORY: `MADC_EXE_ADVISORY=<reason>` (run_tests.sh; the
     reason prints on the summary line every run) — not 169 per-test
     fixtures for one cause. The 7 FAIL(exe) that DID link (php dumps
     printing addresses for ints, testprefixincderef) are the varargs class
     inside an AOT image and re-measure after fix 1.
  4. **Frozen-grove libc++ template gaps — ~35, both arches:**
     `__ns_std____1_max` undefined import (16), `basic_string
     __init_with_sentinel` (5), `list::__base_pointer` ClassPattern (3),
     `initializer_list` private `__begin_` (2), istream `traits_type`,
     `__tree` / `unwrap_iter` c2mir warnings (8), `str()` husks — the
     served-grove fidelity family (pack-degradation baseline, KG gaps).
     These PASS on the container's live-header libc++ lane: the grove
     serving is the layer. Own slices.
  5. **Genuine platform divergences → darwin fixtures:** testsysobject
     (`platform: darwin`), testfcntl (`O_NDELAY=4`, SDK oracle), testdirent
     (`sizeof=1048`, Apple dirent) as `.darwin_expect`; testdlopen
     (libc.so.6), testsysrootinc (sys/sysinfo.h), testbuildnative /
     testparserun / testmadcide (native build of a runtime-needing program
     refused until D5) as `.darwin_skip` with the reason.
  6. **Apple x86_64 `$INODE64` — Intel-only (teststat, testdirent's
     regs_gt0):** `stat`/`readdir` bind the LEGACY 32-bit-inode symbols on
     x86_64 while the prelude's structs are the 64-bit-inode layout; Apple
     redirects through `__asm("_stat$INODE64")` labels on the declarations
     (arm64 has no legacy variant). madc must honor asm labels on function
     declarations for the hosted-x86-64-macos target. Own slice.
  7. **long double on Apple arm64 is double (8 bytes)** — madc reports
     `sizeof(long double)=16` and c2mir/MIR see `ldouble` where the target
     has `double` (testlongdouble, testldblalign, testcomplexretconv,
     testsignalingnan). Target type-model bug. Own slice.
  8. Remaining singles after the re-measure: testint128 (MIR proto result
     type, arm64 — CLOSED wave 8), testgccvectorsizeexprbitwise (aarch64 SIMD insn
     matching — the V128 arc plan), teststringsize (libc++ `sizeof(std::string)=24` — a
     `.darwin_expect` once the grove family is clean), testtypedefarg
     (`0 0 0 0`, both arches), the eval-family / exec-channel / ui
     segfaults (arm64; re-measure after fix 1), testhttpget/testtcpchannel
     (first byte lost, arm64).
- **D5 — PK3-mac-runtime.** `libmadc-0.dylib` (whole-archive
  `LIBMADC_STATIC`, the `libmadc-0.dll` recipe), the darwin emit lane
  naming `@rpath/@executable_path/../lib`, tarball staging, madcide
  in the mac tarball + its install-gate pty probe. Thread-safety
  contract: unchanged (the engine's existing contract; the dylib is a
  packaging of the same objects).

## Risks / opens

- **llvm@18 header-text parity**: the gate is grove-level (unit counts
  + context hashes vs the container baseline), not an assumption. If
  brew 18.x text drifts from apt 18.x, pin harder (exact 18.1.x) or
  vendor the include set.
- **Apple ld64 vs lld behavioral edges** on `-sectcreate` +
  `-force_load` + export surfaces (`dlsym` of executable globals
  without `-rdynamic` is a darwin FACT already relied on — native ld64
  must preserve it; D1's gate exercises it).
- **Runner wall-times**: unknown for a full mac build (linux job ≈ 17
  min; mac runners are slower per-core and `-j3/-j4`). D0 measures.
- **x86_64 divergences** surface for the first time ever in D3/D4 —
  budget triage time; they are discoveries, not regressions.
