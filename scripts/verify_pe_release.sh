#!/bin/bash
# verify_pe_release.sh — container-side gate for the packed win64 release exe
# (windows-release-lane plan W5): the strip+pack must have produced exactly
# the artifact the lane decided to ship.
#
#   bash scripts/verify_pe_release.sh [bin/madc-release-x86-64-windows.exe]
#
# Four authorities, all runnable on the Linux container:
#   1. the import table is the DECIDED set — UCRT api-sets + KERNEL32 +
#      WS2_32 + the two staged runtime DLLs — and NEVER msvcrt.dll (a
#      msvcrt import means the ucrt.specs swap was lost: two CRTs in one
#      process, %Lf prints 0.00);
#   2. the PE-trailer forest reads back through the EXACT shipped bytes
#      under wine (self-image arm): directory pin, the win64 ledger
#      selection present per module, and --run-frozen executes;
#   3. the binary is stripped (no COFF symbol table);
#   4. the runtime DLLs the exe binds by name exist beside it (the
#      deployment set the zip stages).
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
for dll in libstdc++-6.dll libwinpthread-1.dll libmadc_rt.dll; do
    if [ ! -f "$BINDIR/$dll" ]; then
        echo "verify_pe_release: FAILED — $dll missing beside $BIN" >&2
        exit 1
    fi
done

echo "verify_pe_release: OK ($BIN: $UNITS units, ledger complete, imports clean, stripped, DLL set adjacent)"
