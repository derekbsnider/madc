#!/bin/bash
# Build the Windows release zip (windows-release-lane plan W5).
#
#   bash scripts/package_release_windows.sh        (run from the repo root)
#
# Stages dist/madc-<ver>-windows-x86_64.zip (zip, not tar.gz — native
# extraction on Windows) containing
#   madc-<ver>-windows-x86_64/bin/madc.exe            stripped, forest-packed
#   madc-<ver>-windows-x86_64/bin/madcide.exe         the IDE, AOT-compiled by that PE under wine
#   madc-<ver>-windows-x86_64/bin/profiles/           madcide keybinding/theme profiles (data beside the exe)
#   madc-<ver>-windows-x86_64/bin/libstdc++-6.dll     staged UCRT-flavor C++ runtime
#   madc-<ver>-windows-x86_64/bin/libwinpthread-1.dll staged UCRT winpthreads
#   madc-<ver>-windows-x86_64/bin/libmadc-0.dll       the full madc engine (win twin of libmadc.so.0; AOT output + madcide bind it)
#   madc-<ver>-windows-x86_64/lib/libmadc.dll.a       import lib for it (link .o output)
#   madc-<ver>-windows-x86_64/lib/libmadc_rt.a        emitted-C runtime (try/catch + VLA)
#   madc-<ver>-windows-x86_64/madc.ini.example        documented example config (non-live name)
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

for f in "$BIN" bin/libstdc++-6.dll bin/libwinpthread-1.dll bin/libmadc-0.dll \
         lib/libmadc.dll.a lib/libmadc_rt-hosted-x86-64-windows.a; do
    if [ ! -f "$f" ]; then
        echo "package_release_windows: $f missing — run 'make -C src release-windows' first" >&2
        exit 1
    fi
done

# Defense in depth: the gate runs on the exact input being packaged.
bash scripts/verify_pe_release.sh "$BIN"

# ---------- madcide.exe (owner ruling 2026-09-01: packages ship the IDE) ----------
# Compiled BY the release PE under wine — the same dogfood proof the
# Linux packages carry (packaging arc PK7/PK3). The PE finds its own
# DLLs by adjacency in bin/. NOT stripped: the exe comes out of MIR's
# PE writer already lean, and an external strip rewriting a writer-
# produced image is the same class of risk as re-stripping a forest-
# packed ELF.
echo "== madcide.exe (AOT via the release PE under wine) =="
export WINEDEBUG=-all
wineserver -p || true
rm -f tmp/madcide-pkg.exe
( ulimit -t 600; timeout 600 wine "$BIN" -o tmp/madcide-pkg.exe tools/madcide/madcide.mad )

rm -rf "$STAGE"
mkdir -p "$STAGE/$ROOT/bin" "$STAGE/$ROOT/lib" "$STAGE/$ROOT/THIRD_PARTY_NOTICES"
install -m 755 "$BIN" "$STAGE/$ROOT/bin/madc.exe"
install -m 755 tmp/madcide-pkg.exe "$STAGE/$ROOT/bin/madcide.exe"
# Data beside the exe — PE binding's adjacency rule extended to data:
# madcide's profile search ends at <exedir>/profiles (resolve_profile_dir).
mkdir -p "$STAGE/$ROOT/bin/profiles"
install -m 644 tools/madcide/profiles/* "$STAGE/$ROOT/bin/profiles/"
# Example config at the root under a NON-live name: ./madc.ini is a
# real search arm, so an extracted example must never shadow a config.
install -m 644 docs/examples/madc.ini "$STAGE/$ROOT/madc.ini.example"
install -m 755 bin/libstdc++-6.dll "$STAGE/$ROOT/bin/libstdc++-6.dll"
install -m 755 bin/libwinpthread-1.dll "$STAGE/$ROOT/bin/libwinpthread-1.dll"
install -m 755 bin/libmadc-0.dll "$STAGE/$ROOT/bin/libmadc-0.dll"
install -m 644 lib/libmadc.dll.a "$STAGE/$ROOT/lib/libmadc.dll.a"
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
import libmadc-0.dll (the full madc engine) — keep it (and the two
runtime DLLs) next to the emitted exe or on PATH. Runtime-free
programs, and programs built with
-static-libmadc whose runtime needs are covered by the embedded AOT
ledger, import only the Windows CRT.

Emitted C (madc --emit=c11): link the shipped archive with a mingw-w64
toolchain:

    x86_64-w64-mingw32-gcc -std=c11 program.c -L<this-dir>\\lib -lmadc_rt

Object output (madc --obj) links against lib\\libmadc.dll.a.

madcide (bin\\madcide.exe): the madc IDE — a terminal editor whose core
is the live compiler (diagnostics, outline, and syntax colour are
projections of real parse data). Run it in a real console window:

    bin\\madcide.exe file.c

Keybinding profiles and colour schemes live in bin\\profiles (JOE-style
chords by default; emacs, pico, and vi-modal neovim personalities
included — all plain text, copy and edit them to make your own).

madc.ini.example (this folder) is a documented example configuration
file; to use one, copy it to madc.ini next to where you run madc, or
into %XDG_CONFIG_HOME%\\madc\\madc.ini.
EOF

# Launch smoke on the STAGED exe: no-args madcide prints its usage line
# and exits 1 — proving the shipped bytes load, bind libmadc-0.dll by
# adjacency from the staged bin/, and run main. (The interactive TUI
# needs a real console: a wine pty probe would prove wine's console
# layer, not Windows — the TUI proof stays with the genuine-win lane on
# owner hardware. The PK4 install gate below re-proves the ZIPPED bytes.)
smoke_out=$(cd "$STAGE/$ROOT/bin" && timeout 60 wine madcide.exe 2>/dev/null; true)
case "$smoke_out" in
    *"usage: madcide"*) echo "madcide.exe staged smoke: OK" ;;
    *) echo "package_release_windows: staged madcide.exe smoke failed (got: $smoke_out)" >&2
       exit 1 ;;
esac

( cd "$STAGE" && rm -f "../$ROOT.zip" && zip -q -r -X "../$ROOT.zip" "$ROOT" )
rm -rf "$STAGE"
echo "packaged dist/$ROOT.zip"

# PK4 install gate: unzip the ARTIFACT bytes to scratch and run them under
# wine — the staged smoke above proves the stage, this proves the zip
# (compile+run -o beside the DLLs, madcide usage, hidden-DLL negative
# control). See scripts/package_install_gate.sh.
echo "== install gate (zipped-artifact smoke) =="
bash scripts/package_install_gate.sh winzip "dist/$ROOT.zip"

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
