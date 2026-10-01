#!/bin/bash
# stage_install.sh — the installed layout of madc, staged into a root: ONE
# implementation for every package (the deb, the rpm, the relocatable
# tarball: scripts/package_release.sh) and the Homebrew formula
# (packaging/homebrew/madc.rb, into its keg).
#
#   scripts/stage_install.sh <root> <libdir> <prefix>
#
#   <root>    the staging root (a package's tree, a keg)
#   <libdir>  the library directory under <root> ("lib", "usr/lib/<multiarch>")
#   <prefix>  "usr" for a filesystem layout, "" for a root that IS the prefix
#
# Inputs, built before it runs (from the repo root): bin/madc-release,
# lib/release/libmadc.so (the forest inside), lib/release/libmadc_rt.a,
# madcide compiled by that release compiler ($MADC_STAGE_MADCIDE, default
# tmp/madcide-pkg) and the shipped plugins built by that madcide
# ($MADC_STAGE_PLUGINS, default tmp/plugins-pkg). The optional modules
# (lib/libmadcwebview.so, lib/libmadcgit.so: weak dependencies, a program
# that imports them loads them) are staged when they were built;
# MADC_STAGE_REQUIRE_MODULES=1 (the packages) makes a missing one an error.
set -e
cd "$(dirname "$0")/.."

if [ $# -ne 3 ]; then
	echo "usage: $0 <root> <libdir> <prefix>" >&2
	exit 2
fi
root="$1"
libdir="$2"
prefix="$3"
madcide="${MADC_STAGE_MADCIDE:-tmp/madcide-pkg}"
plugins="${MADC_STAGE_PLUGINS:-tmp/plugins-pkg}"
p="$root${prefix:+/$prefix}"

for f in bin/madc-release lib/release/libmadc.so lib/release/libmadc_rt.a "$madcide"; do
	if [ ! -f "$f" ]; then
		echo "stage_install: missing $f (build it first)" >&2
		exit 1
	fi
done
if [ ! -d "$plugins" ]; then
	echo "stage_install: missing $plugins (scripts/build_shipped_plugins.sh)" >&2
	exit 1
fi

mkdir -p "$p/bin" "$root/$libdir" \
	 "$p/share/man/man1" "$p/share/doc/madc/examples"
install -m 755 bin/madc-release "$p/bin/madc"
# NO strip here: since the PK2 shared default, `make release` strips the
# release library BEFORE packing the forest into it (strip-before-pack
# ordering, src/Makefile) — stripping again rewrites the ELF and silently
# drops the appended forest container. lib/release/ is the release mode's
# OWN product dir (per-mode-names law): a dev rebuild can never swap this
# file.
install -m 644 lib/release/libmadc.so "$root/$libdir/libmadc.so.0"
ln -sf libmadc.so.0 "$root/$libdir/libmadc.so"
# The emitted-C runtime (a few KB, static): what `madc --emit=c11` output
# links on a box with no madc at all (cc prog.c -lmadc_rt) — try/catch
# context stack + VLA scope-exit helpers, nothing else. Platform parity: the
# mac and win archives already ship it.
install -m 644 lib/release/libmadc_rt.a "$root/$libdir/libmadc_rt.a"
# The optional modules: the platform webview library (the loader tries
# <exedir>/../lib first, then the system search) and the madcgit module.
for m in libmadcwebview.so libmadcgit.so; do
	if [ -f "lib/$m" ]; then
		install -m 755 "lib/$m" "$root/$libdir/$m"
	elif [ -n "$MADC_STAGE_REQUIRE_MODULES" ]; then
		echo "stage_install: missing lib/$m (the packages carry it)" >&2
		exit 1
	fi
done
install -m 755 "$madcide" "$p/bin/madcide"
mkdir -p "$p/share/madcide/profiles"
install -m 644 tools/madcide/profiles/* "$p/share/madcide/profiles/"
# The shipped plugins (bundles: <name>/<name>.plugin, the data files it
# carries, and its code as source plus its built library), the plugin
# search path's second arm (resolve_data_dir).
mkdir -p "$p/share/madcide/plugins"
cp -R "$plugins"/. "$p/share/madcide/plugins/"
# The plugin API headers a plugin's code includes (<madcide/plugin>):
# --build-plugin puts this directory on the include path (resolve_data_dir),
# and `madc -shared -I` names it by hand.
mkdir -p "$p/share/madcide/include/madcide"
install -m 644 tools/madcide/include/madcide/* "$p/share/madcide/include/madcide/"
# The line editor's verb and check bodies (save, quit and the rest are
# verbs): resolve_data_dir finds them here, and madcide refuses to start
# without them rather than run an editor that cannot save or quit.
mkdir -p "$p/share/madcide/verbs" "$p/share/madcide/checks"
install -m 644 tools/texteditor/verbs/*.madv "$p/share/madcide/verbs/"
install -m 644 tools/texteditor/checks/*.madv "$p/share/madcide/checks/"
gzip -9n < docs/man/madc.1 > "$p/share/man/man1/madc.1.gz"
gzip -9n < docs/man/madcide.1 > "$p/share/man/man1/madcide.1.gz"
install -m 644 LICENSE "$p/share/doc/madc/copyright"
install -m 644 third_party/webview/LICENSE "$p/share/doc/madc/webview-copyright"
gzip -9n < CHANGELOG.md > "$p/share/doc/madc/changelog.gz"
# The example config keeps its real name: share/doc is not on madc.ini's
# search path, so it can never shadow a user's config.
install -m 644 docs/examples/madc.ini "$p/share/doc/madc/examples/madc.ini"
# dpkg-deb requires plain 0755 directories. GNU chmod's NUMERIC modes
# deliberately preserve a directory's setgid bit (inherited from the
# checkout), so it must be cleared symbolically first.
find "$root" -type d -exec chmod g-s {} +
find "$root" -type d -exec chmod 0755 {} +
