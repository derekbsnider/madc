#!/usr/bin/env bash
# The shipped plugins as a package carries them (plan §41.11a step 6: a
# plugin with code ships as its source plus a prebuilt library per
# platform): tools/madcide/plugins copied into <out-dir>, then every plugin
# there whose manifest names "code" built into its library beside it by the
# PACKAGED madcide — its engine and its plugin API are the ones the package
# ships, so the library is the one that madcide accepts at activation.
#
# Usage: scripts/build_shipped_plugins.sh <out-dir> <madcide command...>
#   e.g. scripts/build_shipped_plugins.sh tmp/plugins-pkg tmp/madcide-pkg
#        scripts/build_shipped_plugins.sh tmp/plugins-pkg-win wine tmp/madcide-pkg.exe
# Used by package_release.sh and package_release_windows.sh, so the plugin
# set and the build are one implementation. A plugin with no "code" is data
# only and is copied as it is. A build that fails fails the package.
set -u
if [ $# -lt 2 ]; then
	echo "usage: $0 <out-dir> <madcide command...>" >&2
	exit 2
fi
out="$1"
shift
rm -rf "$out"
mkdir -p "$out"
cp -R tools/madcide/plugins/. "$out/"
built=0
for d in "$out"/*/; do
	d="${d%/}"
	n=$(basename "$d")
	m="$d/$n.plugin"
	[ -f "$m" ] || continue
	grep -q '"code"' "$m" || continue
	if ! ( ulimit -t 300; timeout 300 "$@" --build-plugin "$d" ); then
		echo "build_shipped_plugins: plugin '$n' did not build" >&2
		exit 1
	fi
	built=$((built + 1))
done
echo "build_shipped_plugins: $built plugin(s) built into $out"
