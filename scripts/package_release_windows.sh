#!/bin/bash
# Build the Windows release zip (windows-release-lane plan W5).
#
#   bash scripts/package_release_windows.sh        (run from the repo root)
#
# Stages dist/madc-<ver>-windows-x86_64.zip (zip, not tar.gz — native
# extraction on Windows) containing
#   madc-<ver>-windows-x86_64/bin/madc.exe            stripped, forest-packed
#   madc-<ver>-windows-x86_64/bin/libstdc++-6.dll     staged UCRT-flavor C++ runtime
#   madc-<ver>-windows-x86_64/bin/libwinpthread-1.dll staged UCRT winpthreads
#   madc-<ver>-windows-x86_64/bin/libmadc_rt.dll      madc runtime for AOT output
#   madc-<ver>-windows-x86_64/lib/libmadc_rt.dll.a    import lib (link .o output)
#   madc-<ver>-windows-x86_64/lib/libmadc_rt.a        emitted-C runtime (try/catch + VLA)
#   madc-<ver>-windows-x86_64/LICENSE
#   madc-<ver>-windows-x86_64/THIRD_PARTY_NOTICES/... (see below)
#   madc-<ver>-windows-x86_64/README-windows.txt      SmartScreen / deployment notes
# and refreshes its line in dist/SHA256SUMS (other lines preserved — run
# scripts/package_release.sh FIRST; it rewrites that file wholesale).
#
# The DLLs live in bin/ BESIDE madc.exe deliberately: PE has no runpath —
# adjacency is the binding rule, for the exe itself and for every
# runtime-needing exe madc emits next to it.
#
# Inputs are the `make -C src release-windows` artifacts. This script
# re-runs scripts/verify_pe_release.sh on the exact binary it packages
# (the 2026-08-11 lesson: gates hold for the EXACT bytes shipped, never
# by construction). In-vivo evidence is scripts/win_battery.sh on the
# owner's Windows box.
#
# Third-party notices:
#   - libstdc++-6.dll / libmadc_rt.a's unwinder: GPL-3 with the GCC
#     Runtime Library Exception — COPYING3 + COPYING.RUNTIME from the
#     EXACT gcc source tree the stage was built from
#     (scripts/build_win_ucrt_libstdcxx.sh).
#   - libwinpthread-1.dll + the mingw-w64 UCRT headers the forest
#     freezes: the mingw-w64 copyright document (headers are
#     predominantly public domain — "no copyright assigned"; winpthreads
#     is MIT/BSD — the document carries the exact texts).
#   - the forest/pch codec: zstd's BSD license (statically linked).
set -e

VER=$(cat VERSION)
ROOT="madc-${VER}-windows-x86_64"
STAGE="dist/.stage-windows"
BIN=bin/madc-release-x86-64-windows.exe
GCC_SRC="${WIN_UCRT_LIBSTDCXX_SRC:-/workspace/win-ucrt-libstdc++/gcc-13.2.0}"

mkdir -p dist

for f in "$BIN" bin/libstdc++-6.dll bin/libwinpthread-1.dll bin/libmadc_rt.dll \
         lib/libmadc_rt.dll.a lib/libmadc_rt-hosted-x86-64-windows.a; do
    if [ ! -f "$f" ]; then
        echo "package_release_windows: $f missing — run 'make -C src release-windows' first" >&2
        exit 1
    fi
done

# Defense in depth: the gate runs on the exact input being packaged.
bash scripts/verify_pe_release.sh "$BIN"

rm -rf "$STAGE"
mkdir -p "$STAGE/$ROOT/bin" "$STAGE/$ROOT/lib" "$STAGE/$ROOT/THIRD_PARTY_NOTICES"
install -m 755 "$BIN" "$STAGE/$ROOT/bin/madc.exe"
install -m 755 bin/libstdc++-6.dll "$STAGE/$ROOT/bin/libstdc++-6.dll"
install -m 755 bin/libwinpthread-1.dll "$STAGE/$ROOT/bin/libwinpthread-1.dll"
install -m 755 bin/libmadc_rt.dll "$STAGE/$ROOT/bin/libmadc_rt.dll"
install -m 644 lib/libmadc_rt.dll.a "$STAGE/$ROOT/lib/libmadc_rt.dll.a"
install -m 644 lib/libmadc_rt-hosted-x86-64-windows.a "$STAGE/$ROOT/lib/libmadc_rt.a"
install -m 644 LICENSE "$STAGE/$ROOT/LICENSE"

if [ ! -f "$GCC_SRC/COPYING3" ] || [ ! -f "$GCC_SRC/COPYING.RUNTIME" ]; then
    echo "package_release_windows: GCC license texts not found under $GCC_SRC" >&2
    exit 1
fi
install -m 644 "$GCC_SRC/COPYING3" "$STAGE/$ROOT/THIRD_PARTY_NOTICES/GCC-COPYING3.txt"
install -m 644 "$GCC_SRC/COPYING.RUNTIME" \
    "$STAGE/$ROOT/THIRD_PARTY_NOTICES/GCC-RUNTIME-LIBRARY-EXCEPTION.txt"
if [ ! -f /usr/share/doc/mingw-w64-common/copyright ]; then
    echo "package_release_windows: mingw-w64 copyright text not found" >&2
    exit 1
fi
install -m 644 /usr/share/doc/mingw-w64-common/copyright \
    "$STAGE/$ROOT/THIRD_PARTY_NOTICES/mingw-w64-copyright.txt"
if [ ! -f /workspace/zstd/LICENSE ]; then
    echo "package_release_windows: zstd license text not found" >&2
    exit 1
fi
install -m 644 /workspace/zstd/LICENSE "$STAGE/$ROOT/THIRD_PARTY_NOTICES/zstd-LICENSE.txt"

cat > "$STAGE/$ROOT/README-windows.txt" <<EOF
madc ${VER} for Windows (x86_64)
================================

Install: extract this folder anywhere and run bin\\madc.exe. Keep the
three DLLs next to madc.exe — Windows binds DLLs by adjacency (there is
no runpath), and executables madc emits with -o also bind them from the
directory they run in.

This binary is unsigned. SmartScreen will warn on first run of a
downloaded copy: choose "More info" -> "Run anyway", or unblock the zip
before extracting (right-click -> Properties -> Unblock).

The binary is self-contained: the C standard headers (mingw-w64/UCRT)
and the frozen C++ standard-library groves (<string>, <vector>,
<iostream>, ...) are embedded, so no compiler installation is required.
Headers outside the packed set are not available on a machine without
them and fail with a clear error.

Native output (madc -o prog.exe): programs that use madc's runtime
import libmadc_rt.dll — keep it (and the two runtime DLLs) next to the
emitted exe or on PATH. Runtime-free programs, and programs built with
-static-libmadc whose runtime needs are covered by the embedded AOT
ledger, import only the Windows CRT.

Emitted C (madc --emit=c11): link the shipped archive with a mingw-w64
toolchain:

    x86_64-w64-mingw32-gcc -std=c11 program.c -L<this-dir>\\lib -lmadc_rt

Object output (madc --obj) links against lib\\libmadc_rt.dll.a.
EOF

( cd "$STAGE" && rm -f "../$ROOT.zip" && zip -q -r -X "../$ROOT.zip" "$ROOT" )
rm -rf "$STAGE"
echo "packaged dist/$ROOT.zip"

# Refresh our line in SHA256SUMS without touching the other packagers' lines.
( cd dist
  if [ -f SHA256SUMS ]; then
      grep -v -e "$ROOT.zip" SHA256SUMS > SHA256SUMS.tmp || true
  else
      : > SHA256SUMS.tmp
  fi
  sha256sum "$ROOT.zip" >> SHA256SUMS.tmp
  mv SHA256SUMS.tmp SHA256SUMS )

echo "== dist/ =="
ls -l dist/ | sed -n '2,12p'
