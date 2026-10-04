#!/bin/bash
# Build the Windows release zip (windows-release-lane plan W5).
#
#   bash scripts/package_release_windows.sh        (run from the repo root)
#
# Stages dist/madc-<ver>-windows-x86_64.zip (zip, not tar.gz — native
# extraction on Windows) containing
#   madc-<ver>-windows-x86_64/bin/madc.exe            the thin CLI (its engine and forest are libmadc-0.dll)
#   madc-<ver>-windows-x86_64/bin/madcide.exe         the IDE, AOT-compiled by that PE under wine
#   madc-<ver>-windows-x86_64/bin/chthonia.exe        the learning IDE built on madcide (a window by default)
#   madc-<ver>-windows-x86_64/bin/profiles/           madcide keybinding/theme profiles (data beside the exe)
#   madc-<ver>-windows-x86_64/bin/verbs/, checks/     the line editor's verb and check bodies
#   madc-<ver>-windows-x86_64/bin/libstdc++-6.dll     staged UCRT-flavor C++ runtime
#   madc-<ver>-windows-x86_64/bin/libwinpthread-1.dll staged UCRT winpthreads
#   madc-<ver>-windows-x86_64/bin/libmadc-0.dll       the full madc engine carrying the forest (win twin of libmadc.so.0; madc.exe, AOT output, madcide and chthonia bind it)
#   madc-<ver>-windows-x86_64/bin/madcwebview.dll     the platform webview library (WebView2 + native chrome; GUI programs: import madcwebview)
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
# Inputs are the `make -C src release-windows` artifacts: the release set,
# bin/release-windows/ (docs/plans/2026-10-03-chthonia-windows-macos.md §7b).
# This script
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
SET=bin/release-windows
BIN="$SET/madc.exe"
GCC_SRC="${WIN_UCRT_LIBSTDCXX_SRC:-/workspace/win-ucrt-libstdc++/gcc-13.2.0}"

mkdir -p dist

for f in "$BIN" "$SET/libstdc++-6.dll" "$SET/libwinpthread-1.dll" "$SET/libmadc-0.dll" \
         "$SET/madcwebview.dll" "$SET/madcgit.dll" lib/libmadc.dll.a lib/libmadc_rt-hosted-x86-64-windows.a; do
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
# The programs built below run from tmp/: they bind the release set's DLLs
# (the engine with the forest) through WINEPATH, as the zip's bin/ serves
# them by adjacency. Without it wine cannot load libmadc-0.dll (exit 53).
export WINEPATH="Z:$(pwd | sed 's,/,\\,g')\\bin\\release-windows"
wineserver -p || true
rm -f tmp/madcide-pkg.exe
( ulimit -t 600; timeout 600 wine "$BIN" -o tmp/madcide-pkg.exe tools/madcide/madcide.mad )
# chthonia.exe: the product built on madcide's base (tools/chthonia/
# chthonia.json, a GUI-subsystem image: no console window).
echo "== chthonia.exe (AOT via the release PE under wine) =="
rm -f tmp/chthonia-pkg.exe
( ulimit -t 600; timeout 600 wine "$BIN" --project tools/chthonia/chthonia.json -o tmp/chthonia-pkg.exe )
# The shipped plugins: a plugin with code carries its library (a .dll),
# built by this madcide.exe under wine (plan §41.11a step 6).
echo "== madcide plugins (each with code built by the packaged madcide.exe) =="
scripts/build_shipped_plugins.sh tmp/plugins-pkg-win wine tmp/madcide-pkg.exe

rm -rf "$STAGE"
mkdir -p "$STAGE/$ROOT/bin" "$STAGE/$ROOT/lib" "$STAGE/$ROOT/THIRD_PARTY_NOTICES"
install -m 755 "$BIN" "$STAGE/$ROOT/bin/madc.exe"
install -m 755 tmp/madcide-pkg.exe "$STAGE/$ROOT/bin/madcide.exe"
install -m 755 tmp/chthonia-pkg.exe "$STAGE/$ROOT/bin/chthonia.exe"
# madcide's data beside the exe — PE binding's adjacency rule extended to
# data: the search arms end at <exedir> (resolve_profile_dir,
# resolve_data_dir's last arm). The one staging owner.
scripts/stage_madcide_data.sh "$STAGE/$ROOT/bin" tmp/plugins-pkg-win
# Example config at the root under a NON-live name: ./madc.ini is a
# real search arm, so an extracted example must never shadow a config.
install -m 644 docs/examples/madc.ini "$STAGE/$ROOT/madc.ini.example"
install -m 755 "$SET/libstdc++-6.dll" "$STAGE/$ROOT/bin/libstdc++-6.dll"
install -m 755 "$SET/libwinpthread-1.dll" "$STAGE/$ROOT/bin/libwinpthread-1.dll"
install -m 755 "$SET/libmadc-0.dll" "$STAGE/$ROOT/bin/libmadc-0.dll"
# The platform webview library (GUI programs: import madcwebview): the
# loader searches the exe's directory on Windows — beside madc.exe, like
# the runtime DLLs. It needs the Evergreen WebView2 runtime on the machine.
install -m 755 "$SET/madcwebview.dll" "$STAGE/$ROOT/bin/madcwebview.dll"
# The madcgit module (a program that says `git::…`, e.g. madcide's nexus):
# the loader searches the exe's directory on Windows — beside madc.exe. The
# minimal read-only libgit2 is STATIC-linked inside it; nothing named libgit2
# ships.
install -m 755 "$SET/madcgit.dll" "$STAGE/$ROOT/bin/madcgit.dll"
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
# The webview library binds webview/webview (MIT): its notice ships with it.
install -m 644 third_party/webview/LICENSE "$STAGE/$ROOT/THIRD_PARTY_NOTICES/webview-LICENSE.txt"
# madcgit.dll statically links libgit2 (GPLv2 WITH the linking exception, which
# permits linking into a differently-licensed application): its notice ships.
install -m 644 "${LIBGIT2_DIR:-/workspace/libgit2}/src/COPYING" "$STAGE/$ROOT/THIRD_PARTY_NOTICES/libgit2-COPYING.txt"

cat > "$STAGE/$ROOT/README-windows.txt" <<EOF
madc ${VER} for Windows (x86_64)
================================

Install: extract this folder anywhere and run bin\\madc.exe. Keep the
DLLs next to madc.exe (libmadc-0.dll is the compiler itself) — Windows binds DLLs by adjacency (there is
no runpath), and executables madc emits with -o also bind them from the
directory they run in.

This binary is unsigned. SmartScreen will warn on first run of a
downloaded copy: choose "More info" -> "Run anyway", or unblock the zip
before extracting (right-click -> Properties -> Unblock).

The install is self-contained: the C standard headers (mingw-w64/UCRT)
and the frozen C++ standard-library groves (<string>, <vector>,
<iostream>, ...) are embedded in libmadc-0.dll, so no compiler
installation is required — for madc.exe and for every program built on
it (madcide.exe, chthonia.exe, your own).
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

GUI: bin\\madcwebview.dll is madc's binding of the platform webview
(WebView2) with the native menu bar and file dialogs. A program that
says \`import madcwebview;\` (madc's ui "web" target) loads it from the
directory madc.exe runs from — keep it beside the exe like the runtime
DLLs. madcide's window mode is one:

    bin\\madcide.exe file.c --gui

It needs the Microsoft Edge WebView2 Runtime, which Windows 11 (and any
machine with Microsoft Edge) already has.

chthonia (bin\\chthonia.exe): the easy GUI to learn C and C++, built on
madcide and laid out as Thonny is — the editor, the Shell (a C REPL) below
it, the Variables view beside it, Run (F5) and Stop on the toolbar. It
opens a window; give it a file to open:

    bin\\chthonia.exe file.c

madc.ini.example (this folder) is a documented example configuration
file; to use one, copy it to madc.ini next to where you run madc, or
into %XDG_CONFIG_HOME%\\madc\\madc.ini.
EOF

# Launch smoke on the STAGED exe: `madcide --help` prints its usage line
# and exits 0 — proving the shipped bytes load, bind libmadc-0.dll by
# adjacency from the staged bin/, and run main. (The interactive TUI
# needs a real console: a wine pty probe would prove wine's console
# layer, not Windows — the TUI proof stays with the genuine-win lane on
# owner hardware. The PK4 install gate below re-proves the ZIPPED bytes.)
smoke_out=$(cd "$STAGE/$ROOT/bin" && timeout 60 wine madcide.exe --help 2>/dev/null; true)
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
