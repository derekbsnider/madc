#!/bin/bash
# Build the macOS release tarballs (macos-release-lane plan W4).
#
#   bash scripts/package_release_macos.sh          (run from the repo root)
#
# Stages one tar.gz per arch into dist/, named
#   madc-<ver>-macos-arm64.tar.gz / madc-<ver>-macos-x86_64.tar.gz
# each containing
#   madc-<ver>-macos-<arch>/bin/madc          stripped, forest-packed hosted binary
#   madc-<ver>-macos-<arch>/lib/libmadc_rt.a  emitted-C runtime (W3: try/catch + VLA)
#   madc-<ver>-macos-<arch>/share/man/man1/madc.1.gz
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
# fresh binaries on disk (bitten 2026-08-11). In-vivo evidence (AMFI,
# behavior) is scripts/mac_battery.sh on the owner's Macs.
set -e

VER=$(cat VERSION)

mkdir -p dist

package_arch() {
    local bin_arch="$1" pkg_arch="$2"
    local bin="bin/madc-release-${bin_arch}-macos"
    local root="madc-${VER}-macos-${pkg_arch}"
    local stage="dist/.stage-${pkg_arch}"

    if [ ! -f "$bin" ]; then
        echo "package_release_macos: $bin missing — run 'make -C src release-macos' first" >&2
        exit 1
    fi

    # Defense in depth: re-run the release gate on the exact input.
    # 2026-08-11 proved the "inputs passed the gate by construction"
    # assumption wrong — the recipe strips the binaries BEFORE its
    # verify step, so a failed verify still leaves fresh binaries on
    # disk for a later packaging pass to pick up.
    if ! bash scripts/verify_macho_release.sh "$bin" "obj/hosted-${bin_arch}-macos/forest.bin"; then
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
             "$stage/$root/THIRD_PARTY_NOTICES"
    install -m 755 "$bin" "$stage/$root/bin/madc"
    # The emitted-C runtime (W3): programs madc emits as C11 reference the
    # try/catch + VLA runtime when those features are used; on a Mac with no
    # madc library installed this archive is what `cc emitted.c` links.
    install -m 644 "$rtlib" "$stage/$root/lib/libmadc_rt.a"
    gzip -9n < docs/man/madc.1 > "$stage/$root/share/man/man1/madc.1.gz"
    install -m 644 LICENSE "$stage/$root/LICENSE"
    # The frozen C++ groves derive from LLVM's libc++ headers
    # (Apache-2.0-with-LLVM-exception): carry the license text.
    if [ -f /usr/share/doc/libc++-18-dev/copyright ]; then
        install -m 644 /usr/share/doc/libc++-18-dev/copyright \
            "$stage/$root/THIRD_PARTY_NOTICES/libc++-copyright.txt"
    else
        echo "package_release_macos: libc++ copyright text not found" >&2
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
the full madc runtime, which this tarball does not ship.
EOF
    tar -C "$stage" -czf "dist/$root.tar.gz" "$root"
    rm -rf "$stage"
    echo "packaged dist/$root.tar.gz"
}

package_arch arm64 arm64
package_arch x86-64 x86_64

# Refresh our lines in SHA256SUMS without touching the .deb/.rpm ones.
( cd dist
  if [ -f SHA256SUMS ]; then
      grep -v -e "madc-${VER}-macos-arm64.tar.gz" \
              -e "madc-${VER}-macos-x86_64.tar.gz" SHA256SUMS > SHA256SUMS.tmp || true
  else
      : > SHA256SUMS.tmp
  fi
  sha256sum "madc-${VER}-macos-arm64.tar.gz" "madc-${VER}-macos-x86_64.tar.gz" >> SHA256SUMS.tmp
  mv SHA256SUMS.tmp SHA256SUMS )

echo "== dist/ =="
ls -la dist/
