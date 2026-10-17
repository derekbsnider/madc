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
#   /usr/lib/<multiarch|lib64>/libmadc.so.0     lib/release/libmadc.so (stripped pre-pack, forest inside)
#   /usr/lib/<multiarch|lib64>/libmadc.so       -> libmadc.so.0
#   /usr/lib/<multiarch|lib64>/libmadc_rt.a     emitted-C runtime (try/catch + VLA; a bare-box cc links it)
#   /usr/lib/madc/modules/ui_web.so             the ui_web module: the platform webview (WebKitGTK 6.0 /
#                                               GTK4 + native chrome; GUI programs: import ui_web) —
#                                               a WEAK dependency (Recommends): madc never loads it itself
#   /usr/lib/madc/modules/git.so                the git module: madc's read-only git view, the
#                                               pinned libgit2 linked in statically (`import git;`;
#                                               the nexus's PAST verbs) — no libgit2 package needed
#   /usr/lib/madc/modules/markdown.so           the markdown module: Markdown parsed by cmark-gfm,
#                                               linked in statically (`import markdown;`; the IDE's
#                                               help and previews) — no cmark-gfm package needed;
#                                               madc's module directory (the binary's ../lib)
#   /usr/share/man/man1/madc.1.gz
#   /etc/apparmor.d/madc                        the .deb on Ubuntu 24.04 and later only: a profile
#                                               granting madc user namespaces (WebKit's sandbox, for
#                                               a ui:: program's window; loaded by postinst)
#   /usr/share/doc/madc/copyright               LICENSE (MPL-2.0)
#   /usr/share/doc/madc/webview-copyright       webview/webview (MIT) — the webview library's notice
#   /usr/share/doc/madc/libgit2-copyright       libgit2's COPYING — linked into the git module
#   /usr/share/doc/madc/cmark-gfm-copyright     cmark-gfm's notices — linked into the markdown module
#   /usr/share/doc/madc/changelog.gz            CHANGELOG.md
#   /usr/share/doc/madc/examples/madc.ini       documented example config
#
# The IDE, madcide, is its own packages (github.com/derekbsnider/madcide),
# each carrying the madc release it is built on.
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
APPARMOR=0
os_id=$( (. /etc/os-release 2>/dev/null; echo "${ID:-}") )
if [ "$os_id" = ubuntu ]; then
    os_ver=$( (. /etc/os-release; echo "$VERSION_ID") )
    DEBREL="${REL}~ubuntu${os_ver}"
    # Ubuntu 24.04 and later deny an unprofiled program the user namespaces
    # WebKit's sandbox (bwrap) needs: a window — a ui:: program's under madc —
    # dies at its first page with SIGTRAP (LP: #2046844). There the .deb
    # carries an AppArmor profile naming madc.
    if dpkg --compare-versions "$os_ver" ge 24.04; then
        APPARMOR=1
    fi
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

for f in docs/man/madc.1 docs/examples/madc.ini \
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
    make -C src -j"$(bash scripts/build_jobs.sh)" > /dev/null
    make -C src -j"$(bash scripts/build_jobs.sh)" release > /dev/null
}
trap restore_tree EXIT

./configure --enable-madcdat=no > /dev/null
make -C src clean > /dev/null
make -C src -j"$(bash scripts/build_jobs.sh)" > /dev/null
make -C src -j"$(bash scripts/build_jobs.sh)" release > /dev/null
# The ui_web module (src/ui_web.mk; GUI programs: import ui_web) — built
# against the build host's WebKitGTK 6.0 / GTK4 (scripts/provision_container.sh;
# libwebkitgtk-6.0-dev in release.yml) and shipped as a WEAK dependency:
# madc itself never loads it, only a program that imports it does.
make -C src -j"$(bash scripts/build_jobs.sh)" ui_web > /dev/null
# The git module (src/git.mk): the read-only git view, statically
# linking the pinned libgit2 scripts/stage_libgit2.sh staged for this host
# (the floor: 1.8.7 / 1.9.7 or newer; Ubuntu's libgit2 is 1.7.2). The
# packages depend on no libgit2, so the stage is required: git.mk's
# system-libgit2 arm (the Homebrew formula's) would leave a runtime
# dependency no package declares.
lg2=$(bash scripts/stage_libgit2.sh --path host)
if [ ! -f "$lg2" ]; then
    echo "package_release: $lg2 missing — bash scripts/stage_libgit2.sh host" >&2
    exit 1
fi
make -C src -j"$(bash scripts/build_jobs.sh)" git > /dev/null
# The markdown module (src/markdown.mk): Markdown, statically linking the
# pinned cmark-gfm scripts/stage_cmark_gfm.sh staged for this host. Required
# for the same reason as libgit2's stage: the system arm (the Homebrew
# formula's) links a shared cmark-gfm no package declares.
cm=$(bash scripts/stage_cmark_gfm.sh --path host)
if [ ! -f "$cm/libcmark-gfm.a" ]; then
    echo "package_release: $cm/libcmark-gfm.a missing — bash scripts/stage_cmark_gfm.sh host" >&2
    exit 1
fi
make -C src -j"$(bash scripts/build_jobs.sh)" markdown > /dev/null

if ldd bin/madc-release | grep -Eq "qdbm|gdbm|libdb|sqlite"; then
    echo "package_release: distribution binary still links storage libs" >&2
    exit 1
fi

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
# ui_web module is left out: its WebKitGTK and GTK are the Recommends below
# (madc never loads it itself). The modules' references to libmadc's own
# symbols (the host process supplies them) are warnings, kept in the log.
SHLIB=tmp/pkgroot/shlibdeps
mkdir -p "$SHLIB/debian"
printf 'Source: madc\n\nPackage: madc\nArchitecture: any\n' > "$SHLIB/debian/control"
DEBLIB="$PWD/$DEBROOT/usr/lib/x86_64-linux-gnu"
DEBBIN="$PWD/$DEBROOT/usr/bin"
DEPENDS=$(cd "$SHLIB" && dpkg-shlibdeps -O --ignore-missing-info -l"$DEBLIB" \
              "$DEBBIN/madc" \
              "$DEBLIB/libmadc.so.0" \
              "$PWD/$DEBROOT/usr/lib/madc/modules/git.so" \
              "$PWD/$DEBROOT/usr/lib/madc/modules/markdown.so" 2> shlibdeps.log |
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
if [ "$APPARMOR" = 1 ]; then
    # The shape of Ubuntu's own profiles for its WebKit programs (epiphany,
    # devhelp): unconfined, with user namespaces. postinst loads it as
    # dh_apparmor's snippet does. Proven on the 24.04 runner by
    # package_install_gate.sh apparmor (release.yml).
    mkdir -p "$DEBROOT/etc/apparmor.d"
    cat > "$DEBROOT/etc/apparmor.d/madc" << 'EOF'
# madc's windows are WebKitGTK, whose sandbox (bwrap) needs user namespaces.
# This profile allows everything else, as before; it only names the
# program so AppArmor grants them.

abi <abi/4.0>,
include <tunables/global>

profile madc /usr/bin/madc flags=(unconfined) {
  userns,

  # Site-specific additions and overrides. See local/README for details.
  include if exists <local/madc>
}
EOF
    chmod 0644 "$DEBROOT/etc/apparmor.d/madc"
    chmod 0755 "$DEBROOT/etc" "$DEBROOT/etc/apparmor.d"
    echo /etc/apparmor.d/madc > "$DEBROOT/DEBIAN/conffiles"
    cat > "$DEBROOT/DEBIAN/postinst" << 'EOF'
#!/bin/sh
set -e
if [ "$1" = configure ] && aa-enabled --quiet 2>/dev/null; then
    apparmor_parser -r -T -W /etc/apparmor.d/madc || true
fi
EOF
    chmod 0755 "$DEBROOT/DEBIAN/postinst"
fi
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
# The ui_web module's own DT_NEEDED (webkitgtk, gtk4 and their world) must
# not become hard Requires of the whole package: the GUI is optional, the
# weak dependencies above name it.
%global __requires_exclude_from ^/usr/lib/madc/modules/ui_web\\.so\$
%define __strip /bin/true
%define _build_id_links none

%description
${DESC_BODY}

%post -p /sbin/ldconfig
%postun -p /sbin/ldconfig

%files
/usr/bin/*
/usr/lib64/*
/usr/lib/madc
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
root needed. The layout is relocatable: bin/madc finds
lib/libmadc.so.0 through its own \$ORIGIN-relative runpath, and the
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

See share/man/man1 for the manual page, and
share/doc/madc/examples/madc.ini for a documented example configuration
file. The IDE, madcide, is its own download
(https://github.com/derekbsnider/madcide), carrying a madc of its own.

GUI: the ui_web module (lib/madc/modules/ui_web.so) is madc's binding of
the platform webview (WebKitGTK 6.0 / GTK4) with the native menu bar and
file dialogs. A program that says \`import ui_web;\` gets madc's ui
"web" target.

Git: the git module (lib/madc/modules/git.so) is madc's read-only view of a
git repository, with libgit2 linked into it (\`import git;\` and the
\`%git\` command; an IDE reads history, blame and revisions through it).
It needs no libgit2 installed; libgit2's notice is
share/doc/madc/libgit2-copyright.

Markdown: the markdown module (lib/madc/modules/markdown.so) parses
Markdown with cmark-gfm linked into it (\`import markdown;\`; the IDEs'
help and previews read through it). It needs no cmark-gfm installed;
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
for b in madc; do
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
