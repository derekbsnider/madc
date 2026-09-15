# #2 — the madcgit module on the Windows and macOS cross targets (runbook)

**Owner ordering (2026-09-15):** after #1 (the reactor's Windows backend, DONE),
#2 is *"madcgit on both cross targets (a fetched pinned libgit2)"*. This makes
the Windows and macOS release packages carry `libmadcgit`, so the three
`.win64_skip` fixtures the module's absence forced lift:
`testgit`, `testgraphpast`, `testnexus_records` (each skip reason: *"the mingw
cross lane on the build container has no libgit2 for the win64 target, so the
module is not built there and the nexus degrades to 'no repository'"*).

**⚠️ Owner framing (2026-09-15, the point of this whole slice — do not lose it):**
`libgit2` is **NOT in our distribution**. It is a **build-time requirement**.
Two different artifacts share the word "git": `libgit2` (third-party, a
dependency) and `libmadcgit` (OUR module, `src/modules/madcgit/madcgit.cpp`,
which links libgit2 — this is what ships as part of madcide). Because we ship
**pre-compiled binaries** (the Windows/macOS bundles), the build process must
obtain libgit2 (build or install) for each target ABI so it can produce
`libmadcgit`. The end user installs nothing; whatever libgit2 code the module
needs must already be inside the shipped binary. See "How libgit2 reaches the
bundle" below — it changes the packaging from the Linux weak-dep model.

**Status:** planned, not started. Branch it off develop as
`feature/madcgit-cross-claude`.

## ⚠️ The precedent does NOT transfer as banked — read this first

The one-line banking said *"a fetched prebuilt libgit2, the WebView2-SDK-fetch
precedent."* That is misleading and cost is real if taken at face value:
`scripts/fetch_webview2_sdk.sh` fetches **header-only** build files from a
NuGet zip — webview supplies the loader implementation itself, so nothing is
compiled or linked. **libgit2 is a compiled library**: the module
(`lib/libmadcgit.{dll,dylib}`) must LINK against a libgit2 built for each
target ABI (mingw-w64 UCRT x86-64; darwin arm64 and x86-64). There is no
header-only shortcut. So the WebView2 script is the precedent only for the
*fetch-and-pin-by-sha* shape, not for the substance.

## The decision this runbook exists to surface (make it first)

**Prebuilt-binary fetch vs. build-from-source (minimal).** Recommendation:
**build libgit2 from source, minimal, per target.** Reasoning:
- The module is `madc`'s **READ-ONLY view of a LOCAL repository**
  (`check-one-git-owner.sh` forbids any remote/clone/fetch/push/write API in
  `src/modules/madcgit/madcgit.cpp`). So it needs NONE of libgit2's network
  stack — no https (OpenSSL/mbedTLS), no ssh (libssh2), no http-parser. Its
  ONLY mandatory dependency is **zlib**, which is ALREADY available for both
  cross targets (the Makefile links `-lz` on the win64 and darwin lanes).
- A minimal libgit2 CMake build (`-DBUILD_SHARED_LIBS`, `-DUSE_HTTPS=OFF
  -DUSE_SSH=OFF -DBUILD_CLI=OFF -DBUILD_TESTS=OFF -DREGEX_BACKEND=builtin
  -DUSE_BUNDLED_ZLIB=OFF`) is small and reproducible, and cross-builds with
  the SAME mingw / darwin toolchains the container already drives.
- Reliable *prebuilt* mingw-UCRT and darwin-cross libgit2 binaries are NOT
  available as a single pinned artifact the way NuGet WebView2 is; hunting
  them per target is more fragile than a pinned source tarball + sha.
- Precedent for a cross-built static dep already exists in-tree: **zstd** is
  staged as `libzstd-<arch>-macos.a` (`DARWIN_ZSTD_DIR`) and the win lane
  carries its own. Follow that shape.
- **Pin to the container's version, 1.7.2** (`provision_container.sh`
  `PKGS_git=libgit2-dev`), so the Linux system lib and the cross builds agree.

The owner may veto (a vetted prebuilt source would be simpler if one exists).
State the ruling in the plan before writing the fetch/build script.

### RULING (2026-09-15, resolved while starting the slice)

**Source-build minimal + static-link, structured the ZSTD way — a stage script, NOT
a Makefile-inline build.** The owner's framing (libgit2 = build requirement,
static-linked into `libmadcgit`) settles source-vs-prebuilt. The one open impl
detail — WHERE the per-target build lives — resolves to the zstd precedent, not
the webview one:

- libgit2 is a large CMake project (~hundreds of TUs), unlike webview's two `.cc`
  files that compile inline in `webview.mk`. A heavy external build must run ONCE
  and stage, never rerun on every `make release-windows`.
- `scripts/stage_darwin_zstd.sh` + the `provision_container.sh` hook is the exact
  in-tree shape for a cross-built static dep: clone a pinned tag, verify it on the
  tree about to build, read `CC`/`AR` from the hosted MODE's own `src/Makefile`
  (`make -s MODE=… print-CC/print-AR`), build, stage an idempotent per-target `.a`
  into `/workspace/<dep>`. `madcgit.mk` then merely REFERENCES the staged lib +
  include dir (the `DARWIN_ZSTD_LIB`/`DARWIN_ZSTD_INC` pattern).
- So Task 1's `fetch_libgit2.sh` becomes `scripts/stage_libgit2.sh <target>`
  (fetch subsumed into the stage, exactly as `stage_darwin_zstd.sh` clones+builds
  in one). `LIBGIT2_DIR` (default `/workspace/libgit2`) mirrors `DARWIN_ZSTD_DIR`.
- Cross toolchains (verified on the container 2026-09-15):
  win = `x86_64-w64-mingw32-gcc-posix …`, AR `x86_64-w64-mingw32-ar`;
  macos = `clang-18 -target <arch>-apple-macos12 --sysroot /workspace/sdk/MacOSX.sdk`,
  AR `llvm-ar-18`. CMake gets these via `-DCMAKE_C_COMPILER` + `-DCMAKE_C_FLAGS`
  + `-DCMAKE_AR`/`-DCMAKE_SYSTEM_NAME`. Since the build is static-only
  (`-DBUILD_SHARED_LIBS=OFF`), CMake never LINKS a darwin dylib — it only
  compiles `.o` and archives — which removes most of the Darwin-cross-from-Linux
  risk (no install_name/framework link step for libgit2 itself).

## How libgit2 reaches the bundle (the packaging model — differs by target)

The Linux and the bundled Windows/macOS targets are NOT the same:

- **Linux** keeps today's **system-dependency** model (`package_release.sh`):
  ship `libmadcgit.so` as a weak dep and let the OS provide `libgit2.so`
  (`libgit2-dev` / distro package). The rpm/deb can declare libgit2 a
  dependency; the tarball assumes it present. Nothing changes here.
- **Windows / macOS pre-compiled bundles** have NO package manager to supply
  libgit2 at runtime, so a `libmadcgit.dll` linked against a *system*
  `libgit2-2.dll` would fail to load and `git::available()` would be false —
  the module never loads, which is the very thing #2 exists to fix.
  **Ruling (recommended): STATICALLY link the minimal libgit2 into
  `libmadcgit` for these targets.** Then:
  - libgit2 is purely a build-time requirement (matches the owner framing:
    it is not in our distribution);
  - nothing named `libgit2` ships as its own file — its object code lives
    inside our `libmadcgit.{dll,dylib}`, which is self-contained;
  - `git::available()` is true on a fresh Windows/macOS machine with nothing
    installed. libgit2's licence (GPLv2 **with the linking exception**)
    permits static linking into a differently-licensed application; keep its
    `COPYING`/notice in the package's third-party licences.
  - Alternative the owner may prefer: bundle `libgit2-2.dll` / `libgit2.dylib`
    beside our binaries (dynamic). More files to ship and version; the static
    route is cleaner for a read-only minimal build. Decide with the
    source-vs-prebuilt ruling above — they compose (static link ⇒ build the
    minimal `libgit2.a` from source; do not chase a prebuilt shared lib).

## Tasks (each its own commit; `src/`/`include/` commits carry the 4 trailers — the Makefile and scripts do NOT)

1. **`scripts/fetch_libgit2.sh <target-dir>`** — pinned source tarball + sha
   (the `fetch_webview2_sdk.sh` shape: verify `.sha256`, curl, `sha256sum -c`,
   extract). One script; the per-target BUILD is the Makefile's job.
2. **`src/madcgit.mk` — cross arms, libgit2 STATIC-linked.** Today line 30
   excludes cross/hosted modes outright
   (`ifeq (,$(filter cross-% hosted-%,$(MODE)))`). Add per-mode arms mirroring
   `webview.mk`: `ifeq ($(MODE),hosted-x86-64-windows)` builds
   `bin/libmadcgit.dll` (the module image is a `.dll` there —
   `madc_module_library_spelling` already names it), and the two
   `hosted-*-macos` build `lib/…/libmadcgit.dylib`. Each arm: cross-build the
   **minimal static** `libgit2.a` into `../obj/madcgit/<mode>/libgit2`
   (CMake flags in the decision above; the fetched source from task 1),
   set `MADCGIT_CFLAGS` to its include dir and `MADCGIT_LIBS` to
   `…/libgit2.a -lz` (the STATIC archive, NOT `pkg-config`, which answers for
   the host), and link the module with the cross `$(CXX)` so libgit2's object
   code is IN `libmadcgit.{dll,dylib}` — nothing named libgit2 ships. Keep the
   module UNDEFINED-at-link against libmadc's OWN symbols exactly as the Linux
   arm does (bound at dlopen from the importing image); only libgit2 is
   resolved statically at the module's own link.
3. **The release recipes.** `release-windows` / `release-macos` (src/Makefile)
   build the module beside the webview library (`webview-windows` /
   `webview-macos` is the sibling to copy); `package_release_windows.sh` and
   `package_release_macos.sh` ship the SELF-CONTAINED `libmadcgit.dll` /
   `libmadcgit.dylib` beside `madc.exe` / the app (weak dep, as
   `package_release.sh` does on Linux — lines 107-168, 245, 296 — EXCEPT the
   cross bundles need no separate libgit2, since it is static-linked). Carry
   libgit2's `COPYING`/notice into each package's third-party licences.
   Packaging order **Linux → Windows → macOS**
   ([[feedback_packaging_order_linux_first]]: a restore-rebuild deletes the
   macOS forest.bin). NOTE: Linux keeps its system-dependency model (ship
   `libmadcgit.so`, expect system `libgit2.so`) — the static-link is the
   Windows/macOS-bundle answer, per "How libgit2 reaches the bundle" above.
4. **Lift the three `.win64_skip`** (`testgit`, `testgraphpast`,
   `testnexus_records`) once the module builds for win64; the module also
   reaches the macOS package (no macOS domain skip existed — verify).
5. **The seam** (this is #2's own merge wave): pre-build all toolchains
   ([[feedback_seam_prebuild_all_toolchains]]) → battery + the four
   develop-gated lanes → record + `check --promote` → merge to develop. The
   genuine-win lane needs the `feature/win-run-env-forward-claude` harness fix
   MERGED first (or cherry-picked) so its suite runs, and `win_suite` needs
   `bin/libmadcgit.dll` staged beside `madc.exe`.

## Reading list

- `src/madcgit.mk` (the whole file — the Linux arm is the shape to fork).
- `src/webview.mk` (lines 19-60: the exact per-mode cross pattern —
  `hosted-x86-64-windows`, `hosted-*-macos`, the `.sha256` order dep, the
  cross `$(CXX)` selection `WEBVIEW_CXX`).
- `src/Makefile` around `release-windows` (958-984), `release-macos` (304),
  `DARWIN_ZSTD_DIR` (256-374), the win LIBS line (457) — how a per-target dep
  is staged and linked today.
- `scripts/fetch_webview2_sdk.sh` (the fetch-and-pin shape) and
  `scripts/package_release{,_windows,_macos}.sh` (where libmadcgit ships).
- `src/madc_modules.cpp` — the madcgit row + `madc_module_library_spelling`
  (the `.dll`/`.dylib` names are DATA there; do not hardcode elsewhere).
- `docs/plans/2026-09-15-madcgit-module-plan.md` — the module's "As landed",
  whose Windows/macOS row already names this as the follow-up.
- libgit2 1.7.2 `CMakeLists.txt` options (confirm the minimal-build flags
  above against that release before writing the build arm).

## "Done" definition

`make -C src release-windows` and `release-macos` produce
`libmadcgit.{dll,dylib}`; the packages ship them; `git::available()` is true
under wine and on genuine-win and macOS (not just Linux); `testgit`,
`testgraphpast`, `testnexus_records` pass on the wine and genuine-win lanes;
all four develop-gated lanes green on the merged content; merged to develop.
NOT master (that is #3 + the owner's `/promote` decision).

## Out of scope

- Any write/remote libgit2 API (the module is read-only by gate).
- #3 (the release tier + gcc-torture + `/promote`).
