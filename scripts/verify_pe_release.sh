#!/bin/bash
# verify_pe_release.sh — container-side gate for the packed win64 release exe
# (windows-release-lane plan W5): the strip+pack must have produced exactly
# the artifact the lane decided to ship.
#
#   bash scripts/verify_pe_release.sh [bin/madc-release-x86-64-windows.exe]
#
# Five authorities, all runnable on the Linux container:
#   1. the import table is the DECIDED set — UCRT api-sets + KERNEL32 +
#      WS2_32 + the two staged runtime DLLs — and NEVER msvcrt.dll (a
#      msvcrt import means the ucrt.specs swap was lost: two CRTs in one
#      process, %Lf prints 0.00);
#   2. the PE-trailer forest reads back through the EXACT shipped bytes
#      under wine (self-image arm): directory pin, the win64 ledger
#      selection present per module, and --run-frozen executes;
#   3. the binary is stripped (no COFF symbol table);
#   4. the runtime DLLs the exe binds by name exist beside it (the
#      deployment set the zip stages);
#   5. the artifact carries EXACTLY ONE stdlib-flavor profile (owner policy:
#      Linux and Windows ship libstdc++, macOS ships libc++, never both).
# In-vivo evidence on real Windows stays scripts/win_battery.sh's job.
set -e
cd "$(dirname "$0")/.."

BIN="${1:-bin/madc-release-x86-64-windows.exe}"
OBJDUMP="${OBJDUMP:-x86_64-w64-mingw32-objdump}"
WINE="${MADC_WINE:-wine}"
export WINEDEBUG=-all

if [ ! -f "$BIN" ]; then
    echo "verify_pe_release: $BIN missing — run 'make -C src release-windows' first" >&2
    exit 1
fi

# 1. Import-table policy.
IMPORTS=$("$OBJDUMP" -p "$BIN" | awk '/DLL Name:/ { print $3 }' | sort -u)
if printf '%s\n' "$IMPORTS" | grep -qi '^msvcrt\.dll$'; then
    echo "verify_pe_release: FAILED — $BIN imports msvcrt.dll (UCRT specs swap lost)" >&2
    exit 1
fi
bad=0
while IFS= read -r dll; do
    case "$dll" in
        KERNEL32.dll|WS2_32.dll|ucrtbase.dll) ;;
        api-ms-win-crt-*.dll) ;;
        libstdc++-6.dll|libwinpthread-1.dll) ;;
        *)
            echo "verify_pe_release: FAILED — unexpected import: $dll" >&2
            bad=1
            ;;
    esac
done <<<"$IMPORTS"
[ "$bad" -eq 0 ]

# 2. Forest read-back through the shipped bytes (wine, self-image arm).
DUMP=tmp/verify_pe_dump.txt
mkdir -p tmp
timeout 300 "$WINE" "$BIN" --dump-forest > "$DUMP"
grep -q '^forest	units=' "$DUMP"
grep -q '^ledger	modules=' "$DUMP"
if ! LEDGER_SOURCES=$(bash scripts/select_ledger_sources.sh win64 scripts/ledger_sources.txt); then
    echo "verify_pe_release: could not select win64 ledger sources" >&2
    exit 1
fi
while IFS= read -r src; do
    [ -n "$src" ] || continue
    if ! grep -Fq "$(printf 'ledgermod\t%s\t' "$src")" "$DUMP"; then
        echo "verify_pe_release: FAILED — ledger module missing: $src" >&2
        exit 1
    fi
done <<<"$LEDGER_SOURCES"
UNITS=$(grep -c '^unit	' "$DUMP")
timeout 300 "$WINE" "$BIN" --run-frozen > /dev/null 2>&1

# 3. Stripped: no COFF symbol table survives.
if [ "$("$OBJDUMP" -t "$BIN" | grep -c '^\[')" -gt 0 ]; then
    echo "verify_pe_release: FAILED — $BIN still carries a COFF symbol table (not stripped)" >&2
    exit 1
fi

# 4. The deployment set beside the exe (PE has no runpath; adjacency is
#    the binding rule; the zip stages exactly these).
BINDIR=$(dirname "$BIN")
for dll in libstdc++-6.dll libwinpthread-1.dll libmadc-0.dll; do
    if [ ! -f "$BINDIR/$dll" ]; then
        echo "verify_pe_release: FAILED — $dll missing beside $BIN" >&2
        exit 1
    fi
done

# 5. EXACTLY ONE stdlib-flavor profile (owner policy 2026-08-16: Linux and
#    Windows ship libstdc++, macOS ships libc++, no product ships both). A
#    host-capability probe once appended the build container's own libc++
#    corpus — 845 units of /usr/include/c++/v1 — into this PE, where no
#    libc++ runtime exists and only wine's Z:\ mapping made it look
#    serviceable. Asking the shipped bytes for the OTHER flavor must find no
#    second container to open.
#    Negative control for this negative assertion: the same run MUST report
#    the producer-config mismatch. That proves the request actually reached
#    the profile-stack walk, so a silently-failed probe cannot false-green
#    the "no second profile" claim.
#    The probe must actually INCLUDE something: a TU with no #include never
#    reaches a forest bind, so the walk never runs and both greps are
#    meaningless (that vacuum is what the control below caught in review).
#    tests/teststdunversioned.mad is the exerciser the removed pack leg used.
PROFILE_LOG=tmp/verify_pe_profile.log
timeout 300 "$WINE" "$BIN" -v -stdlib=libc++ \
    tests/teststdunversioned.mad > "$PROFILE_LOG" 2>&1 || true
if ! grep -aq 'producer config mismatch' "$PROFILE_LOG"; then
    echo "verify_pe_release: FAILED — alternate-flavor probe never reached the profile stack;" >&2
    echo "  the 'no second profile' check below would be vacuous. Probe log tail:" >&2
    # The log must reach the failure report itself — on a CI runner the
    # file dies with the job (the PK5 burn-in read three of these blind).
    # A common cause: the alternate flavor's headers are not installed on
    # the build host, so -stdlib=libc++ bails before any forest bind
    # (the container carries libc++-18-dev; a bare host does not).
    tail -n 20 "$PROFILE_LOG" >&2
    exit 1
fi
if grep -aq 'opened container' "$PROFILE_LOG"; then
    echo "verify_pe_release: FAILED — $BIN carries a SECOND stdlib-flavor profile" >&2
    grep -a 'opened container' "$PROFILE_LOG" >&2
    echo "  owner policy: one flavor per artifact (win64 ships libstdc++ only)" >&2
    exit 1
fi
rm -f "$PROFILE_LOG"

# 6. The SUBSYSTEM (madcide polish P2c): madc.exe itself is a console
#    program; an executable it emits is console by default and WINDOWS_GUI
#    under -mwindows (mingw-gcc's spelling; a project manifest's
#    "kind": "gui" says the same for a --project build). The oracle is the
#    cross gcc on this host: -mwindows stamps 2, the default 3 — read from
#    the PE optional header (e_lfanew + 4 + 20 + 68, u16) the same way.
#    Never name an output `con.exe`: CON is the console DEVICE on Windows
#    (and under wine), so fopen(con.exe) opens the console — EBADF.
pe_subsystem() {
    python3 - "$1" <<'PY'
import struct, sys
b = open(sys.argv[1], 'rb').read()
lfanew = struct.unpack_from('<I', b, 0x3c)[0]
assert b[lfanew:lfanew+4] == b'PE\0\0', 'not a PE image'
print(struct.unpack_from('<H', b, lfanew + 4 + 20 + 68)[0])
PY
}
SUBSYS_DIR=tmp/verify_pe_subsystem
rm -rf "$SUBSYS_DIR"
mkdir -p "$SUBSYS_DIR"
printf 'int main(void) { return 0; }\n' > "$SUBSYS_DIR/hello.c"
self_sub=$(pe_subsystem "$BIN")
if [ "$self_sub" != 3 ]; then
    echo "verify_pe_release: FAILED — $BIN subsystem is $self_sub, expected 3 (console)" >&2
    exit 1
fi
timeout 300 "$WINE" "$BIN" -o "$SUBSYS_DIR/default.exe" "$SUBSYS_DIR/hello.c" > "$SUBSYS_DIR/default.log" 2>&1 || {
    echo "verify_pe_release: FAILED — the default emit of hello.c did not link (see $SUBSYS_DIR/default.log)" >&2; exit 1; }
timeout 300 "$WINE" "$BIN" -mwindows -o "$SUBSYS_DIR/gui.exe" "$SUBSYS_DIR/hello.c" > "$SUBSYS_DIR/gui.log" 2>&1 || {
    echo "verify_pe_release: FAILED — the -mwindows emit of hello.c did not link (see $SUBSYS_DIR/gui.log)" >&2; exit 1; }
con_sub=$(pe_subsystem "$SUBSYS_DIR/default.exe")
gui_sub=$(pe_subsystem "$SUBSYS_DIR/gui.exe")
GCC_W64="${GCC_W64:-x86_64-w64-mingw32-gcc}"
oracle_con=3
oracle_gui=2
if command -v "$GCC_W64" > /dev/null 2>&1; then
    "$GCC_W64" -o "$SUBSYS_DIR/oracle-default.exe" "$SUBSYS_DIR/hello.c" > /dev/null 2>&1 \
        && oracle_con=$(pe_subsystem "$SUBSYS_DIR/oracle-default.exe")
    "$GCC_W64" -mwindows -o "$SUBSYS_DIR/oracle-gui.exe" "$SUBSYS_DIR/hello.c" > /dev/null 2>&1 \
        && oracle_gui=$(pe_subsystem "$SUBSYS_DIR/oracle-gui.exe")
fi
if [ "$con_sub" != "$oracle_con" ] || [ "$gui_sub" != "$oracle_gui" ]; then
    echo "verify_pe_release: FAILED — emitted subsystems console=$con_sub gui=$gui_sub; gcc oracle console=$oracle_con gui=$oracle_gui" >&2
    exit 1
fi
rm -rf "$SUBSYS_DIR"

echo "verify_pe_release: OK ($BIN: $UNITS units, ONE stdlib profile, ledger complete, imports clean, stripped, DLL set adjacent, subsystem console=$con_sub -mwindows=$gui_sub as gcc)"
