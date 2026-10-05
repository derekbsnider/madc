#!/bin/bash
# stage_madcide_data.sh — THE staging of madcide's data into an install:
# one implementation for every platform's package (chthonia plan
# docs/plans/2026-10-03-chthonia-windows-macos.md §7 step D4: "one staging
# script serves all three platforms"). Callers: scripts/stage_install.sh
# (Linux packages, the Homebrew keg), scripts/package_release_windows.sh,
# scripts/package_release_macos.sh.
#
#   scripts/stage_madcide_data.sh <data-dir> <plugins-dir>
#
#   <data-dir>     where madcide's data searches end (resolve_data_dir,
#                  resolve_profile_dir): <prefix>/share/madcide on Linux and
#                  macOS; the exe's own directory on Windows, where PE
#                  binding's adjacency rule extends to data
#   <plugins-dir>  the shipped plugin bundles scripts/build_shipped_plugins.sh
#                  built for this platform
#
# Stages, under <data-dir>:
#   profiles/          keybinding and colour profiles
#   plugins/           the shipped plugins (bundles: <name>/<name>.plugin, the
#                      data files it carries, and its code as source plus its
#                      built library) — the plugin search path's second arm
#   include/madcide/   the public headers: the plugin API a plugin's code
#                      includes (<madcide/plugin>; --build-plugin puts this
#                      directory on the include path, and `madc -shared -I`
#                      names it), and the harness a product's tests include
#                      (<madcide/harness>, with <madcide/session> and
#                      <madcide/vocabulary>; their manifest's include_dirs
#                      names it and its "libs" names libmadcide)
#   verbs/, checks/    the line editor's verb and check bodies (save, quit and
#                      the rest are verbs): madcide refuses to start without
#                      them rather than run an editor that cannot save or quit
set -e
cd "$(dirname "$0")/.."

if [ $# -ne 2 ]; then
	echo "usage: $0 <data-dir> <plugins-dir>" >&2
	exit 2
fi
dest="$1"
plugins="$2"
if [ ! -d "$plugins" ]; then
	echo "stage_madcide_data: no plugin bundles at $plugins (scripts/build_shipped_plugins.sh builds them)" >&2
	exit 1
fi

mkdir -p "$dest/profiles" "$dest/plugins" "$dest/include/madcide" \
	 "$dest/verbs" "$dest/checks"
install -m 644 tools/madcide/profiles/* "$dest/profiles/"
cp -R "$plugins"/. "$dest/plugins/"
install -m 644 tools/madcide/include/madcide/* "$dest/include/madcide/"
install -m 644 tools/texteditor/verbs/*.madv "$dest/verbs/"
install -m 644 tools/texteditor/checks/*.madv "$dest/checks/"
