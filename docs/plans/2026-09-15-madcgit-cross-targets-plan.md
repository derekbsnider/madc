# #2 — the madcgit module on the Windows and macOS cross targets (runbook)

**Owner ordering (2026-09-15):** after #1 (the reactor's Windows backend, DONE),
#2 is *"madcgit on both cross targets (a fetched pinned libgit2)"*. This makes
the Windows and macOS release packages carry `libmadcgit`, so the three
`.win64_skip` fixtures the module's absence forced lift:
`testgit`, `testgraphpast`, `testnexus_records` (each skip reason: *"the mingw
cross lane on the build container has no libgit2 for the win64 target, so the
module is not built there and the nexus degrades to 'no repository'"*).

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

## Tasks (each its own commit; `src/`/`include/` commits carry the 4 trailers — the Makefile and scripts do NOT)

1. **`scripts/fetch_libgit2.sh <target-dir>`** — pinned source tarball + sha
   (the `fetch_webview2_sdk.sh` shape: verify `.sha256`, curl, `sha256sum -c`,
   extract). One script; the per-target BUILD is the Makefile's job.
2. **`src/madcgit.mk` — cross arms.** Today line 30 excludes cross/hosted
   modes outright (`ifeq (,$(filter cross-% hosted-%,$(MODE)))`). Add
   per-mode arms mirroring `webview.mk`: `ifeq ($(MODE),hosted-x86-64-windows)`
   builds `bin/libmadcgit.dll` (the module image is a `.dll` there —
   `madc_module_library_spelling` already names it), `ifeq` the two
   `hosted-*-macos` build `lib/…/libmadcgit.dylib`. Each arm: cross-build (or
   fetch-prebuilt) libgit2 into `../obj/madcgit/<mode>/libgit2`, set
   `MADCGIT_CFLAGS`/`MADCGIT_LIBS` to that staged tree (NOT `pkg-config`,
   which answers for the host), and link the module with the cross `$(CXX)`.
   Keep the module UNDEFINED-at-link against libmadc's symbols exactly as the
   Linux arm does (bound at dlopen from the importing image).
3. **The release recipes.** `release-windows` / `release-macos` (src/Makefile)
   build the module beside the webview library (`webview-windows` /
   `webview-macos` is the sibling to copy); `package_release_windows.sh` and
   `package_release_macos.sh` ship `libmadcgit.dll` / `libmadcgit.dylib`
   beside `madc.exe` / the app (weak dep, as `package_release.sh` does on
   Linux — lines 107-168, 245, 296). Packaging order **Linux → Windows →
   macOS** ([[feedback_packaging_order_linux_first]]: a restore-rebuild
   deletes the macOS forest.bin).
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
