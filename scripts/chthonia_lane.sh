#!/bin/bash
# chthonia_lane.sh — Chthonia built and tested as its own repository builds
# it (docs/plans/2026-10-04-chthonia-own-repository.md §6 step 2): its build
# script and its tests (tools/chthonia/scripts/), against libmadcide and
# madcide's public headers, never the base's sources.
#
#   scripts/chthonia_lane.sh
#       the madc under test (MADC_BIN, default bin/madc): libmadcide built by
#       it into its own lib directory, where `-lmadcide` and chthonia's
#       "libs" find it, and the tree's public headers (tools/madcide/include)
#   scripts/chthonia_lane.sh --installed TARBALL
#       an INSTALLED madc: the relocatable tarball package_release.sh writes,
#       unpacked, with its own libmadcide and share/madcide/include — what
#       Chthonia's CI installs
#   --exported (with either form)
#       the scripts run from Chthonia's repository as scripts/chthonia_export.sh
#       makes it (§6 step 3), not from tools/chthonia: with --installed, what
#       Chthonia's CI runs on Linux
#
# Steps: build chthonia (scripts/build.sh), run it (--help prints its usage),
# run the console tests, then the window tests under xvfb-run when the host
# has it (the tree form builds libmadcwebview first), then the Linux packages
# (scripts/package_linux.sh: .deb, .rpm, tarball, each holding exactly the
# staged files) when the host has dpkg-deb and rpmbuild. The tree has no
# madcide program, so there the built chthonia builds its bundle's library
# (it reads the same --build-plugin); an installed madc's madcide does it in
# the installed form. The installed form then installs Chthonia's tarball
# into the unpacked madc folder — first removing every path the tarball
# carries, so what is checked is the tarball's — and runs
# scripts/check_install.sh there. One summary line:
#   chthonia: build ok | tests: N passed, 0 failed | gui: M passed, 0 failed | packages: ok [| install: ok]
# Record it with scripts/lane_ledger.sh record chthonia "<line>".
set -u
cd "$(dirname "$0")/.." || exit 2
mode=tree
exported=0
while [ $# -gt 0 ]; do
	case "$1" in
	--installed)
		mode=installed
		tarball=${2:-}
		shift 2 || shift
		;;
	--exported) exported=1; shift ;;
	*) tarball= ; mode=usage; break ;;
	esac
done
if [ $mode = usage ] || { [ $mode = installed ] && [ ! -f "$tarball" ]; }; then
	echo "usage: $0 [--installed TARBALL] [--exported]" >&2
	exit 2
fi
work=$PWD/tmp/chthonia-lane
rm -rf "$work"
mkdir -p "$work" tmp/logs
log=$PWD/tmp/logs/chthonia-lane-$(date +%Y%m%d-%H%M%S).log
cdir=$PWD/tools/chthonia
if [ $exported = 1 ]; then
	cdir=$work/repo
	bash scripts/chthonia_export.sh "$cdir" >> "$log" 2>&1 || { echo "chthonia: FAILED — the export (log $log)"; tail -20 "$log" >&2; exit 1; }
fi
case "$(uname -s)" in
Darwin) so=dylib; libpath=DYLD_LIBRARY_PATH ;;
*) so=so; libpath=LD_LIBRARY_PATH ;;
esac
fail() {
	echo "chthonia: FAILED — $1 (log $log)"
	tail -20 "$log" >&2
	exit 1
}

if [ $mode = tree ]; then
	bin=${MADC_BIN:-bin/madc}
	[ -x "$bin" ] || fail "$bin missing — build first"
	madc=$(cd "$(dirname "$bin")" && pwd)/$(basename "$bin")
	inc=$PWD/tools/madcide/include
	libdir=$(cd "$(dirname "$madc")/../lib" && pwd)
	echo "== libmadcide (by $bin) ==" >> "$log"
	# An absolute source path: the base finds its data through __FILE__,
	# which spells the path as given (gcc's rule), and the tests run from
	# tools/chthonia.
	( ulimit -t 900; timeout 900 "$madc" -shared -o "$libdir/libmadcide.$so" \
		"$PWD/tools/madcide/madcide_base.mad" ) >> "$log" 2>&1 || fail "libmadcide did not build"
else
	tar -xf "$tarball" -C "$work" || fail "cannot unpack $tarball"
	madc=$(ls -d "$work"/*/bin/madc 2>/dev/null | head -1)
	[ -x "$madc" ] || fail "no bin/madc in $tarball"
	folder=$(cd "$(dirname "$madc")/.." && pwd)
	inc=$(cd "$(dirname "$madc")/.." && pwd)/share/madcide/include
	libdir=$(cd "$(dirname "$madc")/../lib" && pwd)
fi

echo "== build ($madc) ==" >> "$log"
MADC="$madc" MADCIDE_INCLUDE="$inc" bash "$cdir"/scripts/build.sh -o "$work/chthonia" \
	>> "$log" 2>&1 || fail "chthonia did not build"
help=$(env "$libpath=$libdir" timeout 60 "$work/chthonia" --help 2>&1)
case "$help" in
*"usage: chthonia"*) ;;
*) echo "$help" >> "$log"; fail "chthonia --help printed no usage line" ;;
esac

echo "== tests ==" >> "$log"
MADC="$madc" MADCIDE_INCLUDE="$inc" bash "$cdir"/scripts/run_tests.sh >> "$log" 2>&1
trc=$?
tests=$(grep -E '^[0-9]+ passed, [0-9]+ failed$' "$log" | tail -1)

gui="not run (no xvfb-run)"
grc=0
if command -v xvfb-run > /dev/null 2>&1; then
	if [ $mode = tree ]; then
		make -C src libmadcwebview >> "$log" 2>&1 || fail "libmadcwebview did not build"
	fi
	echo "== gui ==" >> "$log"
	MADC="$madc" MADCIDE_INCLUDE="$inc" timeout -k 3 600 xvfb-run -a \
		bash "$cdir"/scripts/run_tests.sh --gui >> "$log" 2>&1
	grc=$?
	gui=$(grep -E '^[0-9]+ passed, [0-9]+ failed$' "$log" | tail -1)
fi

pkgs="not run (no dpkg-deb or rpmbuild)"
installed=
if command -v dpkg-deb > /dev/null 2>&1 && command -v rpmbuild > /dev/null 2>&1; then
	echo "== packages ==" >> "$log"
	builder=()
	[ $mode = tree ] && builder=("MADCIDE=$work/chthonia")
	env MADC="$madc" MADCIDE_INCLUDE="$inc" ${builder[@]+"${builder[@]}"} \
		bash "$cdir"/scripts/package_linux.sh -o "$work/dist" >> "$log" 2>&1 \
		|| fail "the Linux packages did not build"
	pkgs=ok
	if [ $mode = installed ]; then
		echo "== install ($folder) ==" >> "$log"
		tgz=$(ls "$work"/dist/chthonia-*-linux-*.tar.gz)
		tar -tzf "$tgz" | grep -v '/$' | while IFS= read -r f; do rm -f "$folder/$f"; done
		tar -xzf "$tgz" -C "$folder" || fail "cannot unpack $tgz into $folder"
		bash "$cdir"/scripts/check_install.sh "$folder" >> "$log" 2>&1 \
			|| fail "the installed Chthonia did not pass scripts/check_install.sh"
		installed=" | install: ok"
	fi
fi
echo "chthonia: build ok | tests: ${tests:-(no summary)} | gui: ${gui:-(no summary)} | packages: $pkgs$installed"
[ $trc -eq 0 ] && [ $grc -eq 0 ] || { tail -40 "$log" >&2; exit 1; }
