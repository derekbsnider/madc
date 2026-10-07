#!/bin/bash
# Build distributable packages (.deb + .rpm) for the current release.
#
# Run on the build host (the container) from the repo root. The DISTRIBUTION
# configuration is madcdat=OFF (owner, 2026-07-23): the storage backends drag
# libdb/libgdbm/libqdbm/libsqlite3 in as hard package dependencies (qdbm is
# not even packaged on Fedora) and madcdat is the exploratory surface, not
# the language. This script therefore:
#   1. saves the tree's config.mk, reconfigures --enable-madcdat=no,
#      clean-rebuilds bin/madc-release + lib/release/libmadc.so
#   2. runs the FULL packed integration suite against that exact binary —
#      the packaged artifact is a tested artifact
#   3. stages and builds the .deb and .rpm
#   4. restores the saved config.mk and clean-rebuilds the tree back to its
#      normal (full) configuration
#
# Contents (deb/rpm under /usr; the tarball is the same layout, rootless
# and relocatable — extract anywhere, $ORIGIN runpaths bind lib/):
#   /usr/bin/madc                               bin/madc-release
#   /usr/bin/madcide                            AOT-compiled by that binary
#   /usr/lib/<multiarch|lib64>/libmadc.so.0     lib/release/libmadc.so (stripped pre-pack, forest inside)
#   /usr/lib/<multiarch|lib64>/libmadc.so       -> libmadc.so.0
#   /usr/lib/<multiarch|lib64>/libmadc_rt.a     emitted-C runtime (try/catch + VLA; a bare-box cc links it)
#   /usr/lib/<multiarch|lib64>/libmadcwebview.so the platform webview library (WebKitGTK 6.0 / GTK4 +
#                                               native chrome; GUI programs: import madcwebview) —
#                                               a WEAK dependency (Recommends): madc never loads it itself
#   /usr/lib/<multiarch|lib64>/libmadcgit.so    the madcgit module: madc's read-only git view, the
#                                               pinned libgit2 linked in statically (the nexus's PAST
#                                               verbs; git:: programs) — no libgit2 package needed
#   /usr/lib/<multiarch|lib64>/libmadcmark.so   the madcmark module: Markdown parsed by cmark-gfm,
#                                               linked in statically (markdown:: programs; the IDE's
#                                               help and previews) — no cmark-gfm package needed
#   /usr/share/madcide/profiles/                keybinding/theme profiles
#   /usr/share/madcide/plugins/                 the shipped plugins (bundles: default, …)
#   /usr/share/madcide/include/madcide/         the plugin API headers (<madcide/plugin>)
#   /usr/share/madcide/verbs/, checks/          the line editor's verb and check bodies
#   /usr/share/man/man1/madc.1.gz + madcide.1.gz
#   /usr/share/doc/madc/copyright               LICENSE (MPL-2.0)
#   /usr/share/doc/madc/webview-copyright       webview/webview (MIT) — the webview library's notice
#   /usr/share/doc/madc/libgit2-copyright       libgit2's COPYING — linked into libmadcgit.so
#   /usr/share/doc/madc/cmark-gfm-copyright     cmark-gfm's notices — linked into libmadcmark.so
#   /usr/share/doc/madc/changelog.gz            CHANGELOG.md
#   /usr/share/doc/madc/examples/madc.ini       documented example config
#
# Output artifacts: madc_<ver>-<debrel>_amd64.deb, madc-<ver>-<rel>.x86_64.rpm,
# madc-<ver>-linux-x86_64.tar.gz (+ README-linux.txt inside).
#
# One .deb per Ubuntu release, each built ON that release (its libc and
# libstdc++, and its headers in the forest): on Ubuntu <debrel> is
# <rel>~ubuntu<VERSION_ID> (0.102.0-1~ubuntu22.04), so an older release's
# package sorts below a newer one's and a distribution upgrade upgrades it.
# Its Depends is what the shipped binaries link, at this host's versions
# (dpkg-shlibdeps). `--deb` builds the .deb alone — another Ubuntu release's
# package, beside the full set built on the newest — and refreshes only its
# line in dist/SHA256SUMS.
#
# libmadc.so.0 ships because madc -o executables reference it at run time
# (DT_NEEDED); installing it to the system lib dir makes AOT output run
# anywhere the package is installed.
#
# All artifacts land in dist/ with their lines in dist/SHA256SUMS (this
# script rewrites that file wholesale; the win/mac packagers append).
# Version comes from the VERSION file; override the package revision
# with PKG_RELEASE=<n> (default 1).
set -e

cd "$(dirname "$0")/.."
DEB_ONLY=0
case "${1:-}" in
"") ;;
--deb) DEB_ONLY=1 ;;
*) echo "usage: $0 [--deb]" >&2; exit 2 ;;
esac
VER=$(cat VERSION)
REL="${PKG_RELEASE:-1}"
DEBREL=$REL
os_id=$( (. /etc/os-release 2>/dev/null; echo "${ID:-}") )
if [ "$os_id" = ubuntu ]; then
    DEBREL="${REL}~ubuntu$( (. /etc/os-release; echo "$VERSION_ID") )"
fi
MAINT="Derek Snider <coding@psychedeliccanada.ca>"
HOMEPAGE="https://github.com/derekbsnider/madc"
SUMMARY="My Advanced Dialect of C - C/C++ JIT compiler and native toolchain"
DESC_BODY="madc runs C-family programs like scripts (JIT via MIR), compiles
them ahead-of-time to native ELF executables, objects, and shared
libraries with no external toolchain, and emits portable C11 source.
Standard headers are embedded; common symbols auto-include; top-level
statements run without main(). Helper namespaces bring PHP-, Perl-,
Python-, Ruby-, JS-, and Rust-style functions into native C code."

for f in docs/man/madc.1 docs/man/madcide.1 docs/examples/madc.ini \
         LICENSE CHANGELOG.md configure; do
    if [ ! -e "$f" ]; then
        echo "package_release: missing $f" >&2
        exit 1
    fi
done

# ---------- 1. distribution build: madcdat OFF ----------
SAVED_CONFIG=""
if [ -f config.mk ]; then
    SAVED_CONFIG=tmp/config.mk.pkgsaved
    mkdir -p tmp
    cp config.mk "$SAVED_CONFIG"
fi
restore_tree() {
    # MADC_PKG_NO_RESTORE=1 skips the restore rebuild for a THROWAWAY tree
    # (a CI runner discarded after the job) — on a development tree the
    # restore is mandatory: leaving the madcdat=no distribution config in
    # place silently changes what every later build and test validates.
    if [ -n "$MADC_PKG_NO_RESTORE" ]; then
        echo "== tree restore SKIPPED (MADC_PKG_NO_RESTORE) — throwaway build tree =="
        return 0
    fi
    echo "== restoring tree configuration =="
    if [ -n "$SAVED_CONFIG" ] && [ -f "$SAVED_CONFIG" ]; then
        cp "$SAVED_CONFIG" config.mk
        rm -f "$SAVED_CONFIG"
    fi
    make -C src clean > /dev/null
    make -C src -j"$(nproc)" > /dev/null
    make -C src -j"$(nproc)" release > /dev/null
}
trap restore_tree EXIT

./configure --enable-madcdat=no > /dev/null
make -C src clean > /dev/null
make -C src -j"$(nproc)" > /dev/null
make -C src -j"$(nproc)" release > /dev/null
# The platform webview library (GUI programs: import madcwebview) — built
# against the build host's WebKitGTK 6.0 / GTK4 (scripts/provision_container.sh;
# libwebkitgtk-6.0-dev in release.yml) and shipped as a WEAK dependency:
# madc itself never loads it, only a program that imports it does.
make -C src -j"$(nproc)" libmadcwebview > /dev/null
# The madcgit module (src/madcgit.mk): the read-only git view, statically
# linking the pinned libgit2 scripts/stage_libgit2.sh staged for this host
# (the floor: 1.8.7 / 1.9.7 or newer; Ubuntu's libgit2 is 1.7.2). The
# packages depend on no libgit2, so the stage is required: madcgit.mk's
# system-libgit2 arm (the Homebrew formula's) would leave a runtime
# dependency no package declares.
lg2=$(bash scripts/stage_libgit2.sh --path host)
if [ ! -f "$lg2" ]; then
    echo "package_release: $lg2 missing — bash scripts/stage_libgit2.sh host" >&2
    exit 1
fi
make -C src -j"$(nproc)" libmadcgit > /dev/null
# The madcmark module (src/madcmark.mk): Markdown, statically linking the
# pinned cmark-gfm scripts/stage_cmark_gfm.sh staged for this host. Required
# for the same reason as libgit2's stage: the system arm (the Homebrew
# formula's) links a shared cmark-gfm no package declares.
cm=$(bash scripts/stage_cmark_gfm.sh --path host)
if [ ! -f "$cm/libcmark-gfm.a" ]; then
    echo "package_release: $cm/libcmark-gfm.a missing — bash scripts/stage_cmark_gfm.sh host" >&2
    exit 1
fi
make -C src -j"$(nproc)" libmadcmark > /dev/null

if ldd bin/madc-release | grep -Eq "qdbm|gdbm|libdb|sqlite"; then
    echo "package_release: distribution binary still links storage libs" >&2
    exit 1
fi

# ---------- 1b. madcide (owner ruling 2026-09-01: the packages ship the IDE) ----------
# Compiled BY the just-built release compiler against the shared libmadc —
# the dogfood proof of the packaged shape (packaging arc PK7). Its data
# files ship under /usr/share/madcide; the binary finds them through
# resolve_profile_dir's install fallback (madcide_core.inc).
echo "== madcide (AOT via the release compiler) =="
( ulimit -t 240; timeout 300 bin/madc-release -o tmp/madcide-pkg tools/madcide/madcide.mad )
strip --strip-unneeded tmp/madcide-pkg
# libmadcide: madcide's base as the library a product links (a product's
# manifest names it in its "libs"), built by the same compiler into that
# compiler's own lib directory (bin/../lib), where a manifest's library is
# found first.
echo "== libmadcide (via the release compiler) =="
( ulimit -t 240; timeout 300 bin/madc-release -shared -o lib/libmadcide.so tools/madcide/madcide_base.mad )
# The shipped plugins: a plugin with code carries its library, built by
# this madcide (plan §41.11a step 6); every stage() copies the one set.
echo "== madcide plugins (each with code built by the packaged madcide) =="
scripts/build_shipped_plugins.sh tmp/plugins-pkg tmp/madcide-pkg

# ---------- 2. packed suite against the distribution binary ----------
# MADC_PKG_SKIP_SUITE=1 skips the suite for a package run on content a
# recorded green battery already validated (owner rule 2026-08-09/-11:
# never re-run suites on already-proven content; the caller cites the
# battery). Default: the suite runs.
if [ -n "$MADC_PKG_SKIP_SUITE" ]; then
    echo "== packed suite SKIPPED (MADC_PKG_SKIP_SUITE) — caller cites a recorded green battery on this content =="
else
    echo "== packed suite (distribution binary) =="
    MADC_BIN=bin/madc-release bash scripts/run_tests.sh
fi

# ---------- 3. package ----------
rm -rf tmp/pkgroot tmp/rpmtop
mkdir -p dist

stage() {
    # $3 (prefix) is "usr" for the deb/rpm filesystem layout and "" for the
    # relocatable tarball root — the SAME staging (scripts/stage_install.sh,
    # which the Homebrew formula calls too) serves all three packages; only
    # the layout parameterizes. The packages carry the optional modules.
    MADC_STAGE_REQUIRE_MODULES=1 scripts/stage_install.sh "$1" "$2" "$3"
}

# ---------- deb ----------
DEBROOT=tmp/pkgroot/deb
stage "$DEBROOT" "usr/lib/x86_64-linux-gnu" usr
mkdir -p "$DEBROOT/DEBIAN"
chmod 0755 "$DEBROOT/DEBIAN"
# Depends: the libraries the shipped binaries link, at the versions this
# host's packages carry (dpkg-shlibdeps over a minimal debian/control). The
# webview module is left out: its WebKitGTK and GTK are the Recommends below
# (madc never loads it itself). The modules' references to libmadc's own
# symbols (the host process supplies them) are warnings, kept in the log.
SHLIB=tmp/pkgroot/shlibdeps
mkdir -p "$SHLIB/debian"
printf 'Source: madc\n\nPackage: madc\nArchitecture: any\n' > "$SHLIB/debian/control"
DEBLIB="$PWD/$DEBROOT/usr/lib/x86_64-linux-gnu"
DEBBIN="$PWD/$DEBROOT/usr/bin"
DEPENDS=$(cd "$SHLIB" && dpkg-shlibdeps -O --ignore-missing-info -l"$DEBLIB" \
              "$DEBBIN/madc" "$DEBBIN/madcide" \
              "$DEBLIB/libmadc.so.0" "$DEBLIB/libmadcide.so" \
              "$DEBLIB/libmadcgit.so" "$DEBLIB/libmadcmark.so" 2> shlibdeps.log |
          sed -n 's/^shlibs:Depends=//p')
if [ -z "$DEPENDS" ]; then
    echo "package_release: dpkg-shlibdeps named no dependencies (see $SHLIB/shlibdeps.log)" >&2
    exit 1
fi
cat > "$DEBROOT/DEBIAN/control" << EOF
Package: madc
Version: ${VER}-${DEBREL}
Section: devel
Priority: optional
Architecture: amd64
Maintainer: ${MAINT}
Depends: ${DEPENDS}
Recommends: libwebkitgtk-6.0-4, libgtk-4-1
Homepage: ${HOMEPAGE}
Description: ${SUMMARY}
$(printf '%s\n' "$DESC_BODY" | sed 's/^/ /')
EOF
printf 'activate-noawait ldconfig\n' > "$DEBROOT/DEBIAN/triggers"
DEB="dist/madc_${VER}-${DEBREL}_amd64.deb"
dpkg-deb --build --root-owner-group "$DEBROOT" "$DEB"
if [ "$DEB_ONLY" = 1 ]; then
    echo "== install gate (the .deb) =="
    bash scripts/package_install_gate.sh deb "$DEB"
    ( cd dist && { grep -v "  $(basename "$DEB")\$" SHA256SUMS 2>/dev/null || true
                   sha256sum "$(basename "$DEB")"; } > SHA256SUMS.new && mv SHA256SUMS.new SHA256SUMS )
    echo "packaged $DEB (Depends: $DEPENDS)"
    exit 0
fi

# ---------- rpm ----------
RPMTOP=$(pwd)/tmp/rpmtop
mkdir -p "$RPMTOP"/{BUILD,RPMS,SPECS,SOURCES,BUILDROOT}
BUILDROOT="$RPMTOP/BUILDROOT/madc-${VER}-${REL}.x86_64"
stage "$BUILDROOT" "usr/lib64" usr
# %files names the staged layout's directories, never its files:
# stage_install.sh is the one owner of what ships (the deb and the tarball
# take its tree whole), so a newly staged file needs no edit here.
cat > "$RPMTOP/SPECS/madc.spec" << EOF
Name: madc
Version: ${VER}
Release: ${REL}
Summary: ${SUMMARY}
License: MPL-2.0
URL: ${HOMEPAGE}
AutoReqProv: yes
Recommends: webkitgtk6.0
Recommends: gtk4
%define debug_package %{nil}
# The webview library's own DT_NEEDED (webkitgtk, gtk4 and their world) must
# not become hard Requires of the whole package: the GUI is optional, the
# weak dependencies above name it.
%global __requires_exclude_from ^/usr/lib64/(libmadcwebview|libmadcgit|libmadcmark)\\.so\$
%define __strip /bin/true
%define _build_id_links none

%description
${DESC_BODY}

%post -p /sbin/ldconfig
%postun -p /sbin/ldconfig

%files
/usr/bin/*
/usr/lib64/*
/usr/share/madcide
%doc /usr/share/doc/madc
/usr/share/man/man1/*
EOF
rpmbuild --define "_topdir $RPMTOP" --buildroot "$BUILDROOT" -bb "$RPMTOP/SPECS/madc.spec"
cp "$RPMTOP/RPMS/x86_64/madc-${VER}-${REL}.x86_64.rpm" dist/

# ---------- tarball (relocatable: extract anywhere, no root) ----------
TROOT="madc-${VER}-linux-x86_64"
TARSTAGE=tmp/pkgroot/tar
rm -rf "$TARSTAGE"
stage "$TARSTAGE/$TROOT" "lib" ""
cat > "$TARSTAGE/$TROOT/README-linux.txt" << EOF
madc ${VER} for Linux (x86_64)
==============================

Install: extract this folder anywhere and add bin/ to your PATH — no
root needed. The layout is relocatable: bin/madc and bin/madcide find
lib/libmadc.so.0 through their own \$ORIGIN-relative runpath, and the
frozen system-header forest lives INSIDE that library, so the toolchain
is self-contained (no compiler or headers installation required).

Prefer a system install? The .deb and .rpm packages install the same
contents under /usr and register the library with ldconfig.

Native output (madc -o prog): the produced binary references
libmadc.so.0 only when it uses the madc runtime; it looks in its own
../lib first, then this toolchain's lib/, then /usr/local/lib and the
system search path. A plain C program's executable is runtime-free.

Emitted C (madc --emit=c11): on a machine with no madc at all, link
the shipped archive: cc -std=c11 program.c -L<this-dir>/lib -lmadc_rt
(only needed when the program enters try/catch or frees a VLA).

madcide: keybinding profiles and colour schemes load from
share/madcide/profiles next to this README. See share/man/man1 for the
manual pages, and share/doc/madc/examples/madc.ini for a documented
example configuration file.

GUI: lib/libmadcwebview.so is madc's binding of the platform webview
(WebKitGTK 6.0 / GTK4) with the native menu bar and file dialogs. A
program that says \`import madcwebview;\` (madc's ui "web" target) loads
it from this lib/; madcide's window mode is one:

    bin/madcide file.c --gui

Git: lib/libmadcgit.so is madc's read-only view of a git repository, with
libgit2 linked into it (the \`git::\` namespace; madcide's MCP seat reads
history, blame and revisions through it). It is loaded on first use and
needs no libgit2 installed; libgit2's notice is
share/doc/madc/libgit2-copyright.

Markdown: lib/libmadcmark.so parses Markdown with cmark-gfm linked into
it (the \`markdown::\` namespace; the IDEs' help and previews read
through it). It is loaded on first use and needs no cmark-gfm installed;
cmark-gfm's notice is share/doc/madc/cmark-gfm-copyright.

It needs the WebKitGTK 6.0 and GTK 4 runtime libraries installed
(Debian/Ubuntu: libwebkitgtk-6.0-4 libgtk-4-1; Fedora: webkitgtk6.0
gtk4). Nothing else in this folder does.
EOF
# The $ORIGIN proof: the staged binaries must bind the STAGED library
# (relocatable runpath), not the build tree's. ldd resolves runpaths
# from the binary's real location but does NOT canonicalize (the
# $ORIGIN arm resolves as .../bin/../lib/libmadc.so.0), so compare
# canonicalized paths — readlink -f on both sides, the shell analogue
# of canonical_path_for_compare(). An old-policy binary (absolute
# build-tree runpath first) still fails: its canonical path is the
# build tree's lib, never the stage's.
want=$(readlink -f "$TARSTAGE/$TROOT/lib/libmadc.so.0")
for b in madc madcide; do
    bound=$(ldd "$TARSTAGE/$TROOT/bin/$b" | grep 'libmadc\.so\.0' | awk '{print $3}')
    real=$(readlink -f "$bound" 2>/dev/null)
    if [ -z "$real" ] || [ "$real" != "$want" ]; then
        echo "package_release: $b binds '$bound' (canonical '$real'), not the staged lib — relocatable runpath broken" >&2
        exit 1
    fi
done
tar -C "$TARSTAGE" -czf "dist/$TROOT.tar.gz" "$TROOT"
echo "packaged dist/$TROOT.tar.gz"

# ---------- PK4 install gates: the ARTIFACT bytes, extracted and run ----------
# The staging asserts above prove the stage; these re-prove what a user
# actually downloads (each probe carries its own negative control).
echo "== install gates (extracted-artifact smokes) =="
bash scripts/package_install_gate.sh deb "$DEB"
bash scripts/package_install_gate.sh rpm "dist/madc-${VER}-${REL}.x86_64.rpm"
bash scripts/package_install_gate.sh tar "dist/$TROOT.tar.gz"

# ---------- checksums ----------
( cd dist && sha256sum "$(basename "$DEB")" "madc-${VER}-${REL}.x86_64.rpm" \
                       "$TROOT.tar.gz" > SHA256SUMS )
echo "== dist/ =="
ls -la dist/
