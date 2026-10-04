#!/bin/bash
# verify_pe_release.sh — container-side gate for the win64 release set
# (windows-release-lane plan W5; the set's shape: docs/plans/
# 2026-10-03-chthonia-windows-macos.md §7b): the strip+pack must have produced
# exactly the artifacts the lane decided to ship — a thin madc.exe and the
# libmadc-0.dll beside it that carries the forest.
#
#   bash scripts/verify_pe_release.sh [bin/release-windows/madc.exe]
#
# Eight authorities, all runnable on the Linux container:
#   1. the import table is the DECIDED set — UCRT api-sets + KERNEL32 +
#      WS2_32 + the staged runtime DLLs + libmadc-0.dll (the engine) — and
#      NEVER msvcrt.dll (a msvcrt import means the ucrt.specs swap was lost:
#      two CRTs in one process, %Lf prints 0.00);
#   2. the PE-trailer forest in libmadc-0.dll reads back through the EXACT
#      shipped bytes under wine: directory pin, the win64 ledger selection
#      present per module, and --run-frozen executes;
#   3. the exe and the DLL are stripped (no COFF symbol table);
#   4. the runtime DLLs the exe binds by name exist beside it (the
#      deployment set the zip stages);
#   5. the artifact carries EXACTLY ONE stdlib-flavor profile (owner policy:
#      Linux and Windows ship libstdc++, macOS ships libc++, never both);
#   6. madc.exe is a console program, and what it emits is console by
#      default and GUI under -mwindows, as the cross gcc stamps them;
#   7. the forest MOVED: the exe's own image carries none, and the exe binds
#      the DLL's by discovery (one container, in the library, serves the CLI
#      and every program built on the engine);
#   8. a project's "icon" becomes the image's icon resources exactly as
#      windres + the cross gcc make them of `32512 ICON "file.ico"`, and an
#      image without one has no .rsrc.
# In-vivo evidence on real Windows stays scripts/win_battery.sh's job.
set -e
cd "$(dirname "$0")/.."

BIN="${1:-bin/release-windows/madc.exe}"
LIB="$(dirname "$BIN")/libmadc-0.dll"
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
        libstdc++-6.dll|libwinpthread-1.dll|libmadc-0.dll) ;;
        *)
            echo "verify_pe_release: FAILED — unexpected import: $dll" >&2
            bad=1
            ;;
    esac
done <<<"$IMPORTS"
[ "$bad" -eq 0 ]

# 2. Forest read-back through the shipped bytes (wine; the carrier named
#    explicitly — discovery of it is check 7).
DUMP=tmp/verify_pe_dump.txt
mkdir -p tmp
timeout 300 "$WINE" "$BIN" --dump-forest="$LIB" > "$DUMP"
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
timeout 300 "$WINE" "$BIN" --run-frozen="$LIB" > /dev/null 2>&1

# 3. Stripped: no COFF symbol table survives, in the exe or the DLL.
for img in "$BIN" "$LIB"; do
    if [ "$("$OBJDUMP" -t "$img" | grep -c '^\[')" -gt 0 ]; then
        echo "verify_pe_release: FAILED — $img still carries a COFF symbol table (not stripped)" >&2
        exit 1
    fi
done

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
# 7. The forest MOVED to the library: an explicit read of the exe's own
#    bytes finds no container, and a program that includes a header binds
#    the DLL's through the library-image arm (the trace names the arm). On
#    genuine Windows the forest is the ONLY header source, so a missed bind
#    is "Failed to open include file" there; under wine the host's mingw
#    headers would mask it, which is why the arm is asserted, not the output
#    alone.
if timeout 120 "$WINE" "$BIN" --dump-forest="$BIN" 2>/dev/null | grep -q '^forest	units='; then
    echo "verify_pe_release: FAILED — $BIN carries a forest of its own (it belongs in $LIB only)" >&2
    exit 1
fi
BIND_DIR=tmp/verify_pe_bind
rm -rf "$BIND_DIR"
mkdir -p "$BIND_DIR"
printf '#include <stdio.h>\nint main(void) { puts("bound"); return 0; }\n' > "$BIND_DIR/bind.c"
timeout 300 "$WINE" "$BIN" -v "$BIND_DIR/bind.c" > "$BIND_DIR/bind.log" 2>&1 || true
if ! grep -aq 'forest-bind: \[library-image\] opened container' "$BIND_DIR/bind.log" \
    || ! grep -aq '^bound' "$BIND_DIR/bind.log"; then
    echo "verify_pe_release: FAILED — $BIN did not bind the forest in $LIB by discovery (see $BIND_DIR/bind.log)" >&2
    exit 1
fi
rm -rf "$BIND_DIR"
# 8. The ICON: a project manifest's "icon" (an .ico) is laid out as .rsrc —
#    RT_ICON per image (ids 1..n) and RT_GROUP_ICON 32512 (IDI_APPLICATION:
#    Explorer's icon for the file and the one the webview window loads),
#    language 1033. The oracle is windres on `32512 ICON "app.ico"` linked by
#    the cross gcc; the reader prints each resource's (type, id, language,
#    size, code page, bytes — the group directory in hex, an image as a
#    digest) after checking every directory level ascends and the data
#    directory names the .rsrc section. chthonia's icon is the input: six DIB
#    images, 16 to 256 px.
pe_resources() {
    python3 - "$1" <<'PY'
import struct, sys, hashlib
b = open(sys.argv[1], 'rb').read()
lfanew = struct.unpack_from('<I', b, 0x3c)[0]
assert b[lfanew:lfanew+4] == b'PE\0\0', 'not a PE image'
nsec = struct.unpack_from('<H', b, lfanew + 6)[0]
opt = lfanew + 24
rva, size = struct.unpack_from('<II', b, opt + 112 + 2 * 8)
secs = []
for k in range(nsec):
    o = opt + 240 + 40 * k
    vsz, srva, rsz, fo = struct.unpack_from('<IIII', b, o + 8)
    secs.append((b[o:o+8].rstrip(b'\0').decode(), srva, max(vsz, rsz), fo))
if size == 0:
    print('rsrc=none sections=%s' % ','.join(s[0] for s in secs))
    sys.exit(0)
owner = [s for s in secs if s[1] == rva]
assert owner and owner[0][0] == '.rsrc', 'resource directory not at a .rsrc section'
def fo_of(r):
    for nm, srva, span, fo in secs:
        if srva <= r < srva + span: return fo + r - srva
    raise SystemExit('rva %x outside every section' % r)
base = fo_of(rva)
def walk(off, path):
    named, ids = struct.unpack_from('<HH', b, base + off + 12)
    assert named == 0, 'a named entry'
    prev = -1
    for i in range(ids):
        nid, to = struct.unpack_from('<II', b, base + off + 16 + 8 * i)
        assert nid > prev, 'directory entries not ascending'
        prev = nid
        if to & 0x80000000:
            walk(to & 0x7fffffff, path + [nid])
            continue
        drva, dsz, cp, _ = struct.unpack_from('<IIII', b, base + to)
        data = b[fo_of(drva):fo_of(drva) + dsz]
        t, n, l = path + [nid]
        body = data.hex() if t == 14 else hashlib.sha256(data).hexdigest()[:16]
        print('type=%d id=%d lang=%d size=%d cp=%d %s' % (t, n, l, dsz, cp, body))
walk(0, [])
PY
}
ICON_DIR=tmp/verify_pe_icon
rm -rf "$ICON_DIR"
mkdir -p "$ICON_DIR"
cp tools/chthonia/chthonia.ico "$ICON_DIR/app.ico"
printf 'int main(void) { return 0; }\n' > "$ICON_DIR/hello.c"
printf '{ "output": "icon", "icon": "app.ico", "tus": [ "hello.c" ] }\n' > "$ICON_DIR/icon.prj.json"
printf '{ "output": "plain", "tus": [ "hello.c" ] }\n' > "$ICON_DIR/plain.prj.json"
timeout 300 "$WINE" "$BIN" --project "$ICON_DIR/icon.prj.json" -o "$ICON_DIR/icon.exe" > "$ICON_DIR/icon.log" 2>&1 || {
    echo "verify_pe_release: FAILED — the project with an icon did not link (see $ICON_DIR/icon.log)" >&2; exit 1; }
timeout 300 "$WINE" "$BIN" --project "$ICON_DIR/plain.prj.json" -o "$ICON_DIR/plain.exe" > "$ICON_DIR/plain.log" 2>&1 || {
    echo "verify_pe_release: FAILED — the project without an icon did not link (see $ICON_DIR/plain.log)" >&2; exit 1; }
WINDRES_W64="${WINDRES_W64:-x86_64-w64-mingw32-windres}"
GCC_W64="${GCC_W64:-x86_64-w64-mingw32-gcc}"
printf '32512 ICON "app.ico"\n' > "$ICON_DIR/app.rc"
( cd "$ICON_DIR" && "$WINDRES_W64" app.rc -O coff -o app.res.o ) \
    && "$GCC_W64" -o "$ICON_DIR/oracle.exe" "$ICON_DIR/hello.c" "$ICON_DIR/app.res.o" || {
    echo "verify_pe_release: FAILED — the windres + $GCC_W64 icon oracle did not build" >&2; exit 1; }
pe_resources "$ICON_DIR/icon.exe" > "$ICON_DIR/icon.txt"
pe_resources "$ICON_DIR/oracle.exe" > "$ICON_DIR/oracle.txt"
if ! cmp -s "$ICON_DIR/icon.txt" "$ICON_DIR/oracle.txt"; then
    echo "verify_pe_release: FAILED — the icon resources differ from windres's:" >&2
    diff "$ICON_DIR/oracle.txt" "$ICON_DIR/icon.txt" >&2 || true
    exit 1
fi
icon_res=$(grep -c '^type=' "$ICON_DIR/icon.txt")
plain_res=$(pe_resources "$ICON_DIR/plain.exe")
if [ "${plain_res#rsrc=none }" = "$plain_res" ] || printf '%s\n' "$plain_res" | grep -q '\.rsrc'; then
    echo "verify_pe_release: FAILED — an image without an icon carries resources ($plain_res)" >&2
    exit 1
fi
rm -rf "$ICON_DIR"

echo "verify_pe_release: OK ($BIN: $UNITS units in $(basename "$LIB"), none in the exe, ONE stdlib profile, ledger complete, imports clean, stripped, DLL set adjacent, subsystem console=$con_sub -mwindows=$gui_sub as gcc, icon $icon_res resources as windres)"
