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
#   madc-<ver>-macos-<arch>/bin/madc          stripped, forest-packed hosted binary
#   madc-<ver>-macos-<arch>/lib/libmadc_rt.a  emitted-C runtime (W3: try/catch + VLA)
#   madc-<ver>-macos-<arch>/share/man/man1/madc.1.gz
#   madc-<ver>-macos-<arch>/share/doc/madc/examples/madc.ini
#   madc-<ver>-macos-<arch>/LICENSE
#   madc-<ver>-macos-<arch>/THIRD_PARTY_NOTICES/libc++-copyright.txt
#   madc-<ver>-macos-<arch>/THIRD_PARTY_NOTICES/darwin-libc-NOTICE.txt
#   madc-<ver>-macos-<arch>/THIRD_PARTY_NOTICES/APSL-2.0.txt
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
    if ! MADC_READER="$reader" bash scripts/verify_macho_release.sh "$bin" "obj/hosted-${bin_arch}-macos/forest.bin"; then
        echo "package_release_macos: $bin failed verify_macho_release — refusing to package" >&2
        exit 1
    fi

    local rtlib="lib/libmadc_rt-hosted-${bin_arch}-macos.a"
    if [ ! -f "$rtlib" ]; then
        echo "package_release_macos: $rtlib missing — run 'make -C src release-macos' first" >&2
        exit 1
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
    cat > "$stage/$root/README-macos.txt" <<EOF
madc ${VER} for macOS (${pkg_arch})
====================================

Install: copy bin/madc anywhere on your PATH.

This binary is ad-hoc signed (no Apple Developer ID). Because it was
downloaded, macOS quarantines it; the first run will be blocked by
Gatekeeper. Either:

    xattr -d com.apple.quarantine bin/madc

or right-click the binary in Finder and choose Open once.

The binary is self-contained: the C standard headers and the frozen C++
standard-library groves (<string>, <vector>, <iostream>, ...) are embedded,
so no Xcode or Command Line Tools installation is required. C++ headers
outside the packed set are not available on a machine without headers and
fail with a clear error.

Emitted C (madc --emit=c11): if the emitted program enters a try/catch
(any std::cout insertion does, via libc++'s stream machinery) or frees a
VLA, it references madc's small C runtime. Link the shipped archive:

    cc -std=c11 program.c -L<this-dir>/lib -lmadc_rt -lc++

Programs that used <ns_madc> (the madc dialect surface) additionally need
the full madc runtime, which this tarball does not ship: madc -o refuses
such programs on macOS with a clear error. (This is also why madcide,
the IDE the Linux and Windows packages ship as a binary, is not in this
tarball yet — it arrives with the macOS madc runtime library.)

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
