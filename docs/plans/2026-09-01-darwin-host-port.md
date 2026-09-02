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
- **D3 — CI mac jobs in release.yml.** macos-14 (arm64) + macos-13
  (x86_64) build/package/attach with a PK4-style extract-and-run
  install gate for the tarballs (the x86_64 job = the first-ever
  execution proof); `/promote` sheds its mac-attach step; asset
  ownership becomes CI for all six. OWNER DECISION at this exit:
  whether the container cross lane remains canonical for local
  ceremonies or becomes verification-only.
- **D4 — the full-suite macOS lane.** `run_tests.sh` under the gnubin
  posture on the runner; a `darwin` skip/expect fixture domain via the
  existing `MADC_SKIP_EXT` machinery for genuinely-divergent tests;
  triage the first run's failures the root-cause way (no bulk skips).
  New push-gated lane row (`macos-suite`) in `docs/lane-status.tsv`
  once green — this is the never-existed lane, the arc's biggest
  prize.
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
