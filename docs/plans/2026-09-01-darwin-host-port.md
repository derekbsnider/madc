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
    re-measure after item 14).
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
     type, arm64), testgccvectorsizeexprbitwise (aarch64 SIMD insn
     matching), teststringsize (libc++ `sizeof(std::string)=24` — a
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
