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
#   madc-<ver>-macos-<arch>/lib/libmadcwebview.dylib  the platform webview library
#                                             (WKWebView + native chrome; GUI programs:
#                                             import madcwebview) — macOS 13.3+
#   madc-<ver>-macos-<arch>/lib/libmadcgit.dylib   the madcgit module (git::, libgit2 linked in)
#   madc-<ver>-macos-<arch>/lib/libmadcmark.dylib  the madcmark module (markdown::, cmark-gfm linked in)
#   madc-<ver>-macos-<arch>/bin/madcide       the IDE, AOT-compiled by bin/madc  } darwin host of
#   madc-<ver>-macos-<arch>/bin/chthonia      the learning IDE (a window)       } this arch only
#   madc-<ver>-macos-<arch>/share/madcide/    madcide's data (profiles, plugins, plugin headers,
#                                             verbs, checks — scripts/stage_madcide_data.sh)
#   madc-<ver>-macos-<arch>/share/man/man1/madc.1.gz (+ madcide.1.gz with the IDE)
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
    # The platform webview library (GUI programs: import madcwebview) —
    # release-<arch>-macos builds it beside the binary (webview-<arch>-macos);
    # the loader finds it as lib/ next to bin/ (madc_self_lib_dir).
    local webview="lib/webview/${bin_arch}-macos/libmadcwebview.dylib"
    if [ ! -f "$webview" ]; then
        echo "package_release_macos: $webview missing — run 'make -C src release-macos' first (it builds webview-${bin_arch}-macos)" >&2
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
    # The madcgit module (a program that says `git::…`, e.g. madcide's nexus) —
    # release-<arch>-macos builds it beside the webview library (madcgit-<arch>-macos);
    # the minimal read-only libgit2 is STATIC-linked inside it. The loader finds
    # it as lib/ next to bin/ (madc_self_lib_dir).
    local madcgit="lib/madcgit/${bin_arch}-macos/libmadcgit.dylib"
    if [ ! -f "$madcgit" ]; then
        echo "package_release_macos: $madcgit missing — run 'make -C src release-macos' first (it builds madcgit-${bin_arch}-macos)" >&2
        exit 1
    fi
    # The madcmark module (a program that says `markdown::…`, e.g. the IDEs'
    # help), built the same way with cmark-gfm STATIC-linked inside it.
    local madcmark="lib/madcmark/${bin_arch}-macos/libmadcmark.dylib"
    if [ ! -f "$madcmark" ]; then
        echo "package_release_macos: $madcmark missing — run 'make -C src release-macos' first (it builds madcmark-${bin_arch}-macos)" >&2
        exit 1
    fi

    # madcide and chthonia (owner ruling 2026-09-01: the packages ship the
    # IDE; chthonia plan §7 D4), AOT-compiled by THIS arch's release madc and
    # the plugins built by that madcide — the package_release.sh shape. Only
    # a darwin host of this arch can run them (building a plugin runs the
    # built madcide), so a cross host packages the compiler alone and says so.
    local ide=0
    if [ "$HOST_OS" = Darwin ] && [ "$(uname -m | sed 's/x86_64/x86-64/')" = "$bin_arch" ]; then
        ide=1
        echo "== madcide + chthonia + plugins ($bin_arch, AOT via $bin) =="
        rm -f "tmp/madcide-pkg-$bin_arch" "tmp/chthonia-pkg-$bin_arch"
        mkdir -p tmp
        "$bin" -o "tmp/madcide-pkg-$bin_arch" tools/madcide/madcide.mad
        "$bin" --project tools/chthonia/chthonia.json -o "tmp/chthonia-pkg-$bin_arch"
        scripts/build_shipped_plugins.sh "tmp/plugins-pkg-$bin_arch" "tmp/madcide-pkg-$bin_arch"
    else
        echo "package_release_macos: SKIP madcide/chthonia for $bin_arch — they are built by the $bin_arch release madc on a darwin host of that arch (the release.yml mac job); this tarball carries the compiler alone"
    fi

    rm -rf "$stage"
    mkdir -p "$stage/$root/bin" "$stage/$root/lib" "$stage/$root/share/man/man1" \
             "$stage/$root/share/doc/madc/examples" "$stage/$root/THIRD_PARTY_NOTICES"
    install -m 755 "$bin" "$stage/$root/bin/madc"
    install -m 644 docs/examples/madc.ini "$stage/$root/share/doc/madc/examples/madc.ini"
    # The emitted-C runtime (W3): programs madc emits as C11 reference the
    # try/catch + VLA runtime when those features are used; on a Mac with no
    # madc library installed this archive is what `cc emitted.c` links.
    install -m 644 "$rtlib" "$stage/$root/lib/libmadc_rt.a"
    install -m 755 "$rtdylib" "$stage/$root/lib/libmadc-0.dylib"
    install -m 755 "$webview" "$stage/$root/lib/libmadcwebview.dylib"
    install -m 755 "$madcgit" "$stage/$root/lib/libmadcgit.dylib"
    install -m 755 "$madcmark" "$stage/$root/lib/libmadcmark.dylib"
    gzip -9n < docs/man/madc.1 > "$stage/$root/share/man/man1/madc.1.gz"
    if [ "$ide" = 1 ]; then
        install -m 755 "tmp/madcide-pkg-$bin_arch" "$stage/$root/bin/madcide"
        install -m 755 "tmp/chthonia-pkg-$bin_arch" "$stage/$root/bin/chthonia"
        # madcide's data under share/madcide, where an installed madcide
        # looks (<exedir>/../share/madcide): the one staging owner.
        scripts/stage_madcide_data.sh "$stage/$root/share/madcide" "tmp/plugins-pkg-$bin_arch"
        gzip -9n < docs/man/madcide.1 > "$stage/$root/share/man/man1/madcide.1.gz"
    fi
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
    # The webview library binds webview/webview (MIT): its notice ships with it.
    install -m 644 third_party/webview/LICENSE \
        "$stage/$root/THIRD_PARTY_NOTICES/webview-LICENSE.txt"
    # libmadcgit.dylib statically links libgit2 (GPLv2 WITH the linking
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
    # libmadcmark.dylib statically links cmark-gfm (BSD-2 and MIT): its notice
    # ships.
    install -m 644 "$(make -C src -s print-CMARK_GFM_STAGE)/src/COPYING" \
        "$stage/$root/THIRD_PARTY_NOTICES/cmark-gfm-COPYING.txt"
    local ide_text=""
    if [ "$ide" = 1 ]; then
        ide_text="
madcide (bin/madcide): the madc IDE — a terminal editor (bin/madcide
file.c), or a window with --gui. Its keybinding profiles, plugins and
the line editor's verbs live in share/madcide next to this README.

chthonia (bin/chthonia): the easy GUI to learn C and C++, built on
madcide and laid out for learning — the editor, the Shell (a C REPL) below
it, the Variables view beside it, Run (F5) and Stop on the toolbar. It
opens a window by default (--tui asks for the terminal):

    bin/chthonia file.c
"
    fi
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

GUI programs: lib/libmadcwebview.dylib is madc's binding of the platform
webview (WKWebView) with the native menu bar and file dialogs. A program
that says \`import madcwebview;\` (madc's ui "web" target) loads it from
this lib/ next to bin/madc. It requires macOS 13.3 or later.
${ide_text}
share/doc/madc/examples/madc.ini is a documented example configuration
file; to use one, copy it to ~/.config/madc/madc.ini.
EOF
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
