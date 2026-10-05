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
# tmp/madcide-pkg), chthonia compiled by it from tools/chthonia/chthonia.json
# ($MADC_STAGE_CHTHONIA, default tmp/chthonia-pkg), libmadcide (madcide's
# base as a library, lib/libmadcide.so, which chthonia links) and the
# shipped plugins
# built by that madcide
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
chthonia="${MADC_STAGE_CHTHONIA:-tmp/chthonia-pkg}"
plugins="${MADC_STAGE_PLUGINS:-tmp/plugins-pkg}"
p="$root${prefix:+/$prefix}"

for f in bin/madc-release lib/release/libmadc.so lib/release/libmadc_rt.a lib/libmadcide.so "$madcide" "$chthonia"; do
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
# The forest's carrier is the build's (configure --with-forest, recorded in
# config.mk; the Makefile's default is embedded). Embedded, it rides inside
# libmadc.so.0 and there is nothing more to stage. Sidecar, it is
# libmadc.so.0.forest beside the library the CLI and every host load: the
# discovery arm that probes <the loaded library>.forest, as `make install`
# stages it. The Homebrew formula builds the sidecar: pouring a bottle
# relocates each ELF file's RPATH, and the rewrite appends to the image, so
# a container appended to libmadc.so.0 no longer ends the file where its
# reader finds the footer.
shape=embedded
if [ -f config.mk ]; then
	shape=$(sed -n 's/^WITH_FOREST = //p' config.mk)
fi
if [ "$shape" = sidecar ]; then
	if [ ! -f lib/release/libmadc.so.forest ]; then
		echo "stage_install: missing lib/release/libmadc.so.forest (a --with-forest=sidecar release build)" >&2
		exit 1
	fi
	install -m 644 lib/release/libmadc.so.forest "$root/$libdir/libmadc.so.0.forest"
fi
# The emitted-C runtime (a few KB, static): what `madc --emit=c11` output
# links on a box with no madc at all (cc prog.c -lmadc_rt) — try/catch
# context stack + VLA scope-exit helpers, nothing else. Platform parity: the
# mac and win archives already ship it.
install -m 644 lib/release/libmadc_rt.a "$root/$libdir/libmadc_rt.a"
# The optional modules: the platform webview library (the loader tries
# <exedir>/../lib first, then the system search), the madcgit module and the
# madcmark module.
for m in libmadcwebview.so libmadcgit.so libmadcmark.so; do
	if [ -f "lib/$m" ]; then
		install -m 755 "lib/$m" "$root/$libdir/$m"
	elif [ -n "$MADC_STAGE_REQUIRE_MODULES" ]; then
		echo "stage_install: missing lib/$m (the packages carry it)" >&2
		exit 1
	fi
done
install -m 755 "$madcide" "$p/bin/madcide"
# chthonia, the learning IDE built on madcide's base: a window by default.
# Its desktop entry and the hicolor icons (the images of its .ico, the one
# source of its artwork — the Windows .rsrc reads the same file) make it a
# launchable application on a Linux desktop.
install -m 755 "$chthonia" "$p/bin/chthonia"
# libmadcide: madcide's base, the library a product built on it links.
install -m 644 lib/libmadcide.so "$root/$libdir/libmadcide.so"
mkdir -p "$p/share/applications"
install -m 644 tools/chthonia/chthonia.desktop "$p/share/applications/chthonia.desktop"
python3 scripts/ico_png_images.py tools/chthonia/chthonia.ico "$p/share/icons/hicolor" chthonia
# madcide's data (profiles, plugins, the plugin API headers, the line
# editor's verbs and checks): the one staging owner, into share/madcide —
# where resolve_data_dir and resolve_profile_dir look in an install.
scripts/stage_madcide_data.sh "$p/share/madcide" "$plugins"
gzip -9n < docs/man/madc.1 > "$p/share/man/man1/madc.1.gz"
gzip -9n < docs/man/madcide.1 > "$p/share/man/man1/madcide.1.gz"
install -m 644 LICENSE "$p/share/doc/madc/copyright"
install -m 644 third_party/webview/LICENSE "$p/share/doc/madc/webview-copyright"
# libmadcgit.so statically links the pinned libgit2 (GPLv2 WITH the linking
# exception, which permits linking into a differently-licensed program): its
# notice ships wherever the module does, from the staged source it was built
# from (src/madcgit.mk's LIBGIT2_STAGE). A module built against a system
# libgit2 (the Homebrew formula's; MADCGIT_LIBGIT2 empty) links no copy, and
# that library's own package carries its notice.
if [ -f "$root/$libdir/libmadcgit.so" ] && [ -n "$(make -C src -s print-MADCGIT_LIBGIT2)" ]; then
	install -m 644 "$(make -C src -s print-LIBGIT2_STAGE)/src/COPYING" "$p/share/doc/madc/libgit2-copyright"
fi
# A libmadcmark.so linked against the staged cmark-gfm carries its code
# (BSD-2 and MIT), so the stage's notice ships beside it; one linked against a
# system cmark-gfm (Homebrew) leaves the notice to that package.
if [ -f "$root/$libdir/libmadcmark.so" ]; then
	notice=$(make -C src -s print-MADCMARK_NOTICE)
	if [ -n "$notice" ]; then
		if [ ! -f "$notice" ]; then
			echo "stage_install: cmark-gfm's notice $notice is missing (scripts/stage_cmark_gfm.sh host)" >&2
			exit 1
		fi
		install -m 644 "$notice" "$p/share/doc/madc/cmark-gfm-copyright"
	fi
fi
gzip -9n < CHANGELOG.md > "$p/share/doc/madc/changelog.gz"
# The example config keeps its real name: share/doc is not on madc.ini's
# search path, so it can never shadow a user's config.
install -m 644 docs/examples/madc.ini "$p/share/doc/madc/examples/madc.ini"
# dpkg-deb requires plain 0755 directories. GNU chmod's NUMERIC modes
# deliberately preserve a directory's setgid bit (inherited from the
# checkout), so it must be cleared symbolically first.
find "$root" -type d -exec chmod g-s {} +
find "$root" -type d -exec chmod 0755 {} +
