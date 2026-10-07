#!/bin/bash
# madc::console_attach on a REAL Windows console: every tests/cross/win64_*.cpp
# fixture (a filename convention, as aarch64_ldouble_lane.sh's) is built as a
# GUI image (-mwindows) against src/madc_console.cpp, staged on the Windows
# host and run from cmd on a console (ssh -tt: the host's WSL hands cmd a
# real console). The fixture's verdict is its LOG line; each must answer 1
# for every stream. [control: the same fixture against a copy of the source
# whose stdout reopens write-only ("w") answers out=0 — the lane sees the
# failure it guards.]
#
# Runs on the build container (mingw + the ssh channel to the host's WSL),
# like win_suite.sh. Knobs: MADC_WIN_SSH (default derek@host.docker.internal),
# MADC_WIN_DIR (default /mnt/c/Users/Public/madcwin), MADC_WIN_KEEP=1.
set -u
cd "$(dirname "$0")/.."

WIN_SSH="${MADC_WIN_SSH:-derek@host.docker.internal}"
WIN_BASE="${MADC_WIN_DIR:-/mnt/c/Users/Public/madcwin}"
STAGE="$WIN_BASE/console.$$.$RANDOM"
CXX=x86_64-w64-mingw32-g++-posix
OUT=tmp/win_console_lane
fail() { echo "win_console_lane: FAIL — $*" >&2; exit 1; }

cleanup()
{
	if [ "${MADC_WIN_KEEP:-0}" != 1 ]; then
		ssh -o BatchMode=yes "$WIN_SSH" "rm -rf '$STAGE'" >/dev/null 2>&1
	fi
}
trap cleanup EXIT

command -v "$CXX" > /dev/null || fail "no $CXX (run on the build container)"
rm -rf "$OUT"; mkdir -p "$OUT"
# The control's source: stdout reopened write-only, as before the fix.
sed 's/"CONOUT\$", "w+")/"CONOUT$", "w")/' src/madc_console.cpp > "$OUT/console_w.cpp"
cmp -s src/madc_console.cpp "$OUT/console_w.cpp" && fail "control: no read/write CONOUT\$ reopen to revert in src/madc_console.cpp"

# build NAME FIXTURE SOURCE: a GUI image, its runtime linked in (the host
# has no mingw DLLs on its PATH).
build() {
	"$CXX" -O0 -mwindows -static -o "$OUT/$1.exe" "$2" "$3" || fail "$1: build"
}
fixtures=$(ls tests/cross/win64_*.cpp 2>/dev/null)
[ -n "$fixtures" ] || fail "no tests/cross/win64_*.cpp fixture"
for fx in $fixtures; do
	build "$(basename "$fx" .cpp)" "$fx" src/madc_console.cpp
done
build control tests/cross/win64_console_attach.cpp "$OUT/console_w.cpp"

ssh -o BatchMode=yes "$WIN_SSH" "mkdir -p '$STAGE'" || fail "channel down ($WIN_SSH)"
scp -q -o BatchMode=yes "$OUT"/*.exe "$WIN_SSH:$STAGE/" || fail "staging copy"

# One cmd on a console runs every image twice — the parent's handles cleared
# (none) and kept (given) — and lingers while the GUI images write their logs.
runs=""
for exe in "$OUT"/*.exe; do
	b=$(basename "$exe" .exe)
	runs="$runs$b.exe none $b.log & $b.exe given $b.log & "
done
ssh -tt -o BatchMode=yes "$WIN_SSH" \
	"cd '$STAGE' && timeout 60 /mnt/c/Windows/System32/cmd.exe /c \"$runs ping -n 3 127.0.0.1 >nul\"" \
	> "$OUT/cmd.out" 2>&1 || fail "cmd on the host (see $OUT/cmd.out)"
logs=$(ssh -o BatchMode=yes "$WIN_SSH" "cd '$STAGE' && for f in *.log; do sed \"s/^/\$f: /\" \"\$f\"; done" | tr -d '\r')
echo "$logs"

for fx in $fixtures; do
	b=$(basename "$fx" .cpp)
	for mode in none given; do
		line=$(echo "$logs" | grep "^$b.log: $mode ")
		[ -n "$line" ] || fail "$b $mode: no verdict line"
		case "$line" in
			*" attach=1 in=1 out=1 err=1") echo "win_console_lane: ok   $b $mode" ;;
			*) fail "$b $mode: $line" ;;
		esac
	done
done
case "$(echo "$logs" | grep '^control.log: none ')" in
	*" out=0 "*) echo "win_console_lane: ok   control: stdout reopened write-only => out=0" ;;
	*) fail "control broken: the write-only reopen did not answer out=0 ($(echo "$logs" | grep '^control.log: none '))" ;;
esac
echo "win_console_lane: PASS"
