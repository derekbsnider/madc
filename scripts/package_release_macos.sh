#!/bin/bash
# Build the macOS release tarballs (macos-release-lane plan W4; per-arch +
# darwin-host posture: darwin-host port D3).
#
#   bash scripts/package_release_macos.sh [arm64|x86-64 ...]   (from the repo root)
#
# Arches default to what `make -C src release-macos` built on THIS host: both
# on the container (the cross lane), the host arch on a darwin host (the
# release.yml mac jobs — one runner per arch). Stages one tar.gz per arch
# into dist/, named
#   madc-<ver>-macos-arm64.tar.gz / madc-<ver>-macos-x86_64.tar.gz
# each containing
#   madc-<ver>-macos-<arch>/bin/madc          the stripped thin CLI: it loads
#                                             lib/libmadc-0.dylib (@loader_path/../lib)
#   madc-<ver>-macos-<arch>/lib/libmadc_rt.a  emitted-C runtime (W3: try/catch + VLA)
#   madc-<ver>-macos-<arch>/lib/libmadc-0.dylib  the madc engine and runtime (D5),
#                                             carrying the frozen header forest
#                                             (__MADC,__forest): what the CLI and a
#                                             runtime-needing program madc -o builds
#                                             load (@rpath/libmadc-0.dylib)
#   madc-<ver>-macos-<arch>/lib/madc/modules/ui_web.dylib  the ui_web module: the platform
#                                             webview (WKWebView + native chrome; GUI programs:
#                                             import ui_web) — macOS 13.3+
#   madc-<ver>-macos-<arch>/lib/madc/modules/git.dylib  the git module (import git;,
#                                             libgit2 linked in) — madc's module directory
#   madc-<ver>-macos-<arch>/lib/madc/modules/markdown.dylib  the markdown module (import markdown;,
#                                             cmark-gfm linked in) — madc's module directory
#   madc-<ver>-macos-<arch>/share/man/man1/madc.1.gz
#   madc-<ver>-macos-<arch>/share/doc/madc/examples/madc.ini
#   madc-<ver>-macos-<arch>/LICENSE
#   madc-<ver>-macos-<arch>/THIRD_PARTY_NOTICES/libc++-copyright.txt
#   madc-<ver>-macos-<arch>/THIRD_PARTY_NOTICES/darwin-libc-NOTICE.txt
#   madc-<ver>-macos-<arch>/THIRD_PARTY_NOTICES/APSL-2.0.txt
#   madc-<ver>-macos-<arch>/THIRD_PARTY_NOTICES/webview-LICENSE.txt  (webview/webview, MIT)
#   madc-<ver>-macos-<arch>/THIRD_PARTY_NOTICES/libgit2-COPYING.txt, cmark-gfm-COPYING.txt
#   madc-<ver>-macos-<arch>/THIRD_PARTY_NOTICES/zstd-LICENSE.txt  (zstd, BSD — linked into libmadc-0.dylib)
#   madc-<ver>-macos-<arch>/README-macos.txt  ad-hoc signing / quarantine notes
# and refreshes their lines in dist/SHA256SUMS (other lines preserved — run
# scripts/package_release.sh FIRST; it rewrites that file wholesale).
#
# Inputs are the `make -C src release-macos` artifacts. This script
# re-runs scripts/verify_macho_release.sh on each binary it packages —
# forest-carrier, signature, AND prelude-provenance gates (W0.5) hold
# for the EXACT bytes tarred, not by construction: the recipe strips
# binaries before its own verify step, so a failed verify still leaves
# fresh binaries on disk (bitten 2026-08-11). The verify reader/otool
# follow the Makefile's posture: bin/madc + llvm-otool-18 on the
# container; on a darwin host the hosted binary of the arch being
# packaged reads its own forest and brew llvm@18's llvm-otool dumps the
# load commands (env MADC_READER / OTOOL override both).
#
# In-vivo evidence: on a darwin host each tarball then runs the PK4
# install gate (scripts/package_install_gate.sh mactar — extract, run,
# the Mac battery with its PASS floor, the hidden-archive negative
# control); on the container that leg prints a stated SKIP and the
# owner's Macs / the release.yml mac jobs are the execution proof.
#
# The libc++ copyright shipped is the PINNED package's own file, from the
# stage scripts/fetch_libcxx_headers.sh writes (LIBCXX_HEADERS_HOME) —
# one source on both build hosts, never the build host's /usr/share/doc.
set -e

VER=$(cat VERSION)
HOST_OS=$(uname -s)
LIBCXX_HOME="${LIBCXX_HEADERS_HOME:-/workspace/libcxx-headers}"

if [ "$HOST_OS" = Darwin ]; then
    # The Makefile's DARWIN_OTOOL default (brew llvm@18); keg-only, not on PATH.
    OTOOL="${OTOOL:-$(brew --prefix llvm@18 2>/dev/null)/bin/llvm-otool}"
    export OTOOL
fi
SHA256SUM=$(command -v sha256sum || echo "shasum -a 256")

# Arch selection: explicit args (bin spellings; x86_64 accepted), else the
# host's release-macos set — both arches on a cross host, the host arch on
# a darwin host (RELEASE_MACOS_ARCHES in src/Makefile says the same).
ARCHES=()
for a in "$@"; do
    case "$a" in
        arm64|x86-64) ARCHES+=("$a") ;;
        x86_64) ARCHES+=("x86-64") ;;
        *) echo "package_release_macos: unknown arch '$a' (arm64 | x86-64)" >&2; exit 2 ;;
    esac
done
if [ ${#ARCHES[@]} -eq 0 ]; then
    if [ "$HOST_OS" = Darwin ]; then
        ARCHES=("$(uname -m | sed 's/x86_64/x86-64/')")
    else
        ARCHES=(arm64 x86-64)
    fi
fi

mkdir -p dist

package_arch() {
    local bin_arch="$1" pkg_arch="${1/x86-64/x86_64}"
    local bin="bin/madc-release-${bin_arch}-macos"
    local root="madc-${VER}-macos-${pkg_arch}"
    local stage="dist/.stage-${pkg_arch}"
    local reader="${MADC_READER:-bin/madc}"

    if [ ! -f "$bin" ]; then
        echo "package_release_macos: $bin missing — run 'make -C src release-macos' first" >&2
        exit 1
    fi

    # Defense in depth: re-run the release gate on the exact input.
    # 2026-08-11 proved the "inputs passed the gate by construction"
    # assumption wrong — the recipe strips the binaries BEFORE its
    # verify step, so a failed verify still leaves fresh binaries on
    # disk for a later packaging pass to pick up.
    if [ "$HOST_OS" = Darwin ] && [ -z "${MADC_READER:-}" ]; then
        reader="bin/madc-hosted-${bin_arch}-macos"
    fi
    if ! MADC_READER="$reader" bash scripts/verify_macho_release.sh "$bin" "obj/hosted-${bin_arch}-macos/libmadc-0.dylib" "obj/hosted-${bin_arch}-macos/forest.bin"; then
        echo "package_release_macos: $bin failed verify_macho_release — refusing to package" >&2
        exit 1
    fi

    local rtlib="lib/libmadc_rt-hosted-${bin_arch}-macos.a"
    if [ ! -f "$rtlib" ]; then
        echo "package_release_macos: $rtlib missing — run 'make -C src release-macos' first" >&2
        exit 1
    fi
    # The ui_web module (GUI programs: import ui_web) — release-<arch>-macos
    # builds it beside the binary (ui_web-<arch>-macos); found by name in
    # madc's module directory (the binary's ../lib/madc/modules).
    local webview="lib/madc/modules/${bin_arch}-macos/ui_web.dylib"
    if [ ! -f "$webview" ]; then
        echo "package_release_macos: $webview missing — run 'make -C src release-macos' first (it builds ui_web-${bin_arch}-macos)" >&2
        exit 1
    fi
    # The madc engine and runtime (D5): libmadc-0.dylib, built per arch beside
    # forest.bin and carrying it. The thin CLI and a runtime-needing image load
    # it as @rpath/libmadc-0.dylib, their LC_RPATHs reaching this lib/ next to
    # bin/ (the verify above checked the CLI's).
    local rtdylib="obj/hosted-${bin_arch}-macos/libmadc-0.dylib"
    if [ ! -f "$rtdylib" ]; then
        echo "package_release_macos: $rtdylib missing — run 'make -C src release-macos' first" >&2
        exit 1
    fi
    # The git module (a program that says `import git;`, or `%git` at the
    # prompt) — release-<arch>-macos builds it beside the ui_web module
    # (git-<arch>-macos); the minimal read-only libgit2 is STATIC-linked inside
    # it. Found by name in madc's module directory (the binary's
    # ../lib/madc/modules).
    local git="lib/madc/modules/${bin_arch}-macos/git.dylib"
    if [ ! -f "$git" ]; then
        echo "package_release_macos: $git missing — run 'make -C src release-macos' first (it builds git-${bin_arch}-macos)" >&2
        exit 1
    fi
    # The markdown module (a program that says `import markdown;`, e.g. the
    # IDEs' help), built the same way with cmark-gfm STATIC-linked inside it.
    local markdown="lib/madc/modules/${bin_arch}-macos/markdown.dylib"
    if [ ! -f "$markdown" ]; then
        echo "package_release_macos: $markdown missing — run 'make -C src release-macos' first (it builds markdown-${bin_arch}-macos)" >&2
        exit 1
    fi

    rm -rf "$stage"
    mkdir -p "$stage/$root/bin" "$stage/$root/lib/madc/modules" "$stage/$root/share/man/man1" \
             "$stage/$root/share/doc/madc/examples" "$stage/$root/THIRD_PARTY_NOTICES"
    install -m 755 "$bin" "$stage/$root/bin/madc"
    install -m 644 docs/examples/madc.ini "$stage/$root/share/doc/madc/examples/madc.ini"
    # The emitted-C runtime (W3): programs madc emits as C11 reference the
    # try/catch + VLA runtime when those features are used; on a Mac with no
    # madc library installed this archive is what `cc emitted.c` links.
    install -m 644 "$rtlib" "$stage/$root/lib/libmadc_rt.a"
    install -m 755 "$rtdylib" "$stage/$root/lib/libmadc-0.dylib"
    install -m 755 "$webview" "$stage/$root/lib/madc/modules/ui_web.dylib"
    install -m 755 "$git" "$stage/$root/lib/madc/modules/git.dylib"
    install -m 755 "$markdown" "$stage/$root/lib/madc/modules/markdown.dylib"
    gzip -9n < docs/man/madc.1 > "$stage/$root/share/man/man1/madc.1.gz"
    install -m 644 LICENSE "$stage/$root/LICENSE"
    # The frozen C++ groves derive from LLVM's libc++ headers
    # (Apache-2.0-with-LLVM-exception): carry the license text — the pinned
    # package's own, from the stage that also serves its header text.
    if [ -s "$LIBCXX_HOME/copyright" ]; then
        install -m 644 "$LIBCXX_HOME/copyright" \
            "$stage/$root/THIRD_PARTY_NOTICES/libc++-copyright.txt"
    else
        echo "package_release_macos: $LIBCXX_HOME/copyright missing — bash scripts/fetch_libcxx_headers.sh stages the pinned libc++ package (headers + copyright)" >&2
        exit 1
    fi
    # The embedded C prelude derives from Apple's APSL/BSD libc headers
    # (W0.5, open-provenance by the verify gate): carry the notice + the
    # APSL text (docs/licenses/NOTICE-darwin-prelude.txt records the
    # per-file audit these ship under).
    install -m 644 docs/licenses/NOTICE-darwin-prelude.txt \
        "$stage/$root/THIRD_PARTY_NOTICES/darwin-libc-NOTICE.txt"
    install -m 644 docs/licenses/APSL-2.0.txt \
        "$stage/$root/THIRD_PARTY_NOTICES/APSL-2.0.txt"
    # The ui_web module binds webview/webview (MIT): its notice ships with it.
    install -m 644 third_party/webview/LICENSE \
        "$stage/$root/THIRD_PARTY_NOTICES/webview-LICENSE.txt"
    # The git module statically links libgit2 (GPLv2 WITH the linking
    # exception, which permits linking into a differently-licensed application):
    # its notice ships.
    install -m 644 "$(make -C src -s print-LIBGIT2_STAGE)/src/COPYING" \
        "$stage/$root/THIRD_PARTY_NOTICES/libgit2-COPYING.txt"
    # libmadc-0.dylib statically links the pinned zstd (BSD, the
    # scripts/stage_darwin_zstd.sh stage the hosted MODE links): its notice
    # ships, as the Windows zip's does.
    local zstd_license
    zstd_license="$(make -C src -s MODE="hosted-${bin_arch}-macos" print-DARWIN_ZSTD_DIR)/LICENSE"
    if [ ! -f "$zstd_license" ]; then
        echo "package_release_macos: $zstd_license missing — the zstd stage libmadc-0.dylib links (scripts/stage_darwin_zstd.sh)" >&2
        exit 1
    fi
    install -m 644 "$zstd_license" "$stage/$root/THIRD_PARTY_NOTICES/zstd-LICENSE.txt"
    # libmadc-0.dylib statically links the pinned PCRE2 (BSD;
    # scripts/stage_pcre2.sh, split arc 1A.3): its LICENCE ships.
    local pcre2_licence
    pcre2_licence="$(make -C src -s print-PCRE2_STAGE)/src/LICENCE"
    if [ ! -f "$pcre2_licence" ]; then
        echo "package_release_macos: $pcre2_licence missing — the PCRE2 stage libmadc-0.dylib links (scripts/stage_pcre2.sh ${bin_arch}-macos)" >&2
        exit 1
    fi
    install -m 644 "$pcre2_licence" "$stage/$root/THIRD_PARTY_NOTICES/pcre2-LICENCE.txt"
    # markdown.dylib statically links cmark-gfm (BSD-2 and MIT): its notice
    # ships.
    install -m 644 "$(make -C src -s print-CMARK_GFM_STAGE)/src/COPYING" \
        "$stage/$root/THIRD_PARTY_NOTICES/cmark-gfm-COPYING.txt"
    cat > "$stage/$root/README-macos.txt" <<EOF
madc ${VER} for macOS (${pkg_arch})
====================================

Install: keep bin/ and lib/ together (bin/madc loads lib/libmadc-0.dylib,
which holds the compiler and its headers) and put bin/ on your PATH.

This binary is ad-hoc signed (no Apple Developer ID). Because it was
downloaded, macOS quarantines it; the first run will be blocked by
Gatekeeper. Either:

    xattr -dr com.apple.quarantine .

(from this directory: it clears bin/ and lib/ together), or right-click
the binary in Finder and choose Open once.

The installation is self-contained: the C standard headers and the frozen
C++ standard-library groves (<string>, <vector>, <iostream>, ...) are
embedded in lib/libmadc-0.dylib, so no Xcode or Command Line Tools
installation is required. C++ headers
outside the packed set are not available on a machine without headers and
fail with a clear error.

Emitted C (madc --emit=c11): if the emitted program enters a try/catch
(any std::cout insertion does, via libc++'s stream machinery) or frees a
VLA, it references madc's small C runtime. Link the shipped archive:

    cc -std=c11 program.c -L<this-dir>/lib -lmadc_rt -lc++

Programs that use the madc dialect surface (print, format, var, the
php:: and other namespaces) need the full madc runtime: lib/libmadc-0.dylib.
A program built with madc -o (or a library built with madc -shared) loads
it as @rpath/libmadc-0.dylib, and finds it in the lib/ next to its own
bin/ (or next to the madc that built it), so keep bin/ and lib/ together.

GUI programs: the ui_web module (lib/madc/modules/ui_web.dylib) is madc's
binding of the platform webview (WKWebView) with the native menu bar and
file dialogs. A program that says \`import ui_web;\` gets madc's ui "web"
target. It requires macOS 13.3 or later.

The IDE, madcide, is its own download
(https://github.com/derekbsnider/madcide), carrying a madc of its own.

share/doc/madc/examples/madc.ini is a documented example configuration
file; to use one, copy it to ~/.config/madc/madc.ini.
EOF
    # Every entry gets the modes an install gives every user (the one owner
    # stage_install.sh shares): the copied plugin bundles otherwise keep the
    # checkout's modes and the gzipped pages the umask's.
    scripts/install_modes.sh "$stage/$root"
    tar -C "$stage" -czf "dist/$root.tar.gz" "$root"
    rm -rf "$stage"
    echo "packaged dist/$root.tar.gz"
    # PK4: re-prove the ARTIFACT bytes (runs on a darwin host; stated SKIP
    # elsewhere — see scripts/package_install_gate.sh).
    bash scripts/package_install_gate.sh mactar "dist/$root.tar.gz"
    PACKAGED+=("$root.tar.gz")
}

PACKAGED=()
for a in "${ARCHES[@]}"; do
    package_arch "$a"
done

# Refresh OUR lines in SHA256SUMS — only the tarballs packaged this run;
# the .deb/.rpm/.zip lines (and the other arch's, on a one-arch host) stay.
( cd dist
  if [ -f SHA256SUMS ]; then cp SHA256SUMS SHA256SUMS.tmp; else : > SHA256SUMS.tmp; fi
  for f in "${PACKAGED[@]}"; do
      grep -v "  $f\$" SHA256SUMS.tmp > SHA256SUMS.tmp2 || true
      mv SHA256SUMS.tmp2 SHA256SUMS.tmp
  done
  $SHA256SUM "${PACKAGED[@]}" >> SHA256SUMS.tmp
  mv SHA256SUMS.tmp SHA256SUMS )

echo "== dist/ =="
ls -la dist/
