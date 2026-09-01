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
# Contents:
#   /usr/bin/madc                               bin/madc-release
#   /usr/lib/<multiarch|lib64>/libmadc.so.0     lib/release/libmadc.so (stripped pre-pack, forest inside)
#   /usr/lib/<multiarch|lib64>/libmadc.so       -> libmadc.so.0
#   /usr/share/man/man1/madc.1.gz               docs/man/madc.1
#   /usr/share/doc/madc/copyright               LICENSE (MPL-2.0)
#   /usr/share/doc/madc/changelog.gz            CHANGELOG.md
#
# libmadc.so.0 ships because madc -o executables reference it at run time
# (DT_NEEDED); installing it to the system lib dir makes AOT output run
# anywhere the package is installed.
#
# Output: dist/madc_<ver>-<rel>_amd64.deb, dist/madc-<ver>-<rel>.x86_64.rpm,
# dist/SHA256SUMS. Version comes from the VERSION file; override the package
# revision with PKG_RELEASE=<n> (default 1).
set -e

cd "$(dirname "$0")/.."
VER=$(cat VERSION)
REL="${PKG_RELEASE:-1}"
MAINT="Derek Snider <coding@psychedeliccanada.ca>"
HOMEPAGE="https://github.com/derekbsnider/madc"
SUMMARY="My Advanced Dialect of C - C/C++ JIT compiler and native toolchain"
DESC_BODY="madc runs C-family programs like scripts (JIT via MIR), compiles
them ahead-of-time to native ELF executables, objects, and shared
libraries with no external toolchain, and emits portable C11 source.
Standard headers are embedded; common symbols auto-include; top-level
statements run without main(). Helper namespaces bring PHP-, Perl-,
Python-, Ruby-, JS-, and Rust-style functions into native C code."

for f in docs/man/madc.1 LICENSE CHANGELOG.md configure; do
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
    local root="$1" libdir="$2"
    mkdir -p "$root/usr/bin" "$root/$libdir" \
             "$root/usr/share/man/man1" "$root/usr/share/doc/madc"
    install -m 755 bin/madc-release "$root/usr/bin/madc"
    # NO strip here: since the PK2 shared default, `make release` strips
    # the release library BEFORE packing the forest into it (strip-before-
    # pack ordering, src/Makefile) — stripping again rewrites the ELF and
    # silently drops the appended forest container. lib/release/ is the
    # release mode's OWN product dir (per-mode-names law): a dev rebuild
    # can never swap this file.
    install -m 644 lib/release/libmadc.so "$root/$libdir/libmadc.so.0"
    ln -s libmadc.so.0 "$root/$libdir/libmadc.so"
    install -m 755 tmp/madcide-pkg "$root/usr/bin/madcide"
    mkdir -p "$root/usr/share/madcide/profiles"
    install -m 644 tools/madcide/profiles/* "$root/usr/share/madcide/profiles/"
    gzip -9n < docs/man/madc.1 > "$root/usr/share/man/man1/madc.1.gz"
    install -m 644 LICENSE "$root/usr/share/doc/madc/copyright"
    gzip -9n < CHANGELOG.md > "$root/usr/share/doc/madc/changelog.gz"
    # dpkg-deb requires plain 0755 directories. GNU chmod's NUMERIC modes
    # deliberately preserve a directory's setgid bit (inherited from the
    # checkout), so it must be cleared symbolically first.
    find "$root" -type d -exec chmod g-s {} +
    find "$root" -type d -exec chmod 0755 {} +
}

# ---------- deb ----------
DEBROOT=tmp/pkgroot/deb
stage "$DEBROOT" "usr/lib/x86_64-linux-gnu"
mkdir -p "$DEBROOT/DEBIAN"
chmod 0755 "$DEBROOT/DEBIAN"
cat > "$DEBROOT/DEBIAN/control" << EOF
Package: madc
Version: ${VER}-${REL}
Section: devel
Priority: optional
Architecture: amd64
Maintainer: ${MAINT}
Depends: libc6 (>= 2.38), libstdc++6, libgcc-s1, zlib1g, libzstd1
Homepage: ${HOMEPAGE}
Description: ${SUMMARY}
$(printf '%s\n' "$DESC_BODY" | sed 's/^/ /')
EOF
printf 'activate-noawait ldconfig\n' > "$DEBROOT/DEBIAN/triggers"
DEB="dist/madc_${VER}-${REL}_amd64.deb"
dpkg-deb --build --root-owner-group "$DEBROOT" "$DEB"

# ---------- rpm ----------
RPMTOP=$(pwd)/tmp/rpmtop
mkdir -p "$RPMTOP"/{BUILD,RPMS,SPECS,SOURCES,BUILDROOT}
BUILDROOT="$RPMTOP/BUILDROOT/madc-${VER}-${REL}.x86_64"
stage "$BUILDROOT" "usr/lib64"
cat > "$RPMTOP/SPECS/madc.spec" << EOF
Name: madc
Version: ${VER}
Release: ${REL}
Summary: ${SUMMARY}
License: MPL-2.0
URL: ${HOMEPAGE}
AutoReqProv: yes
%define debug_package %{nil}
%define __strip /bin/true
%define _build_id_links none

%description
${DESC_BODY}

%post -p /sbin/ldconfig
%postun -p /sbin/ldconfig

%files
/usr/bin/madc
/usr/bin/madcide
/usr/lib64/libmadc.so.0
/usr/lib64/libmadc.so
/usr/share/madcide
%doc /usr/share/doc/madc/copyright
%doc /usr/share/doc/madc/changelog.gz
/usr/share/man/man1/madc.1.gz
EOF
rpmbuild --define "_topdir $RPMTOP" --buildroot "$BUILDROOT" -bb "$RPMTOP/SPECS/madc.spec"
cp "$RPMTOP/RPMS/x86_64/madc-${VER}-${REL}.x86_64.rpm" dist/

# ---------- checksums ----------
( cd dist && sha256sum "madc_${VER}-${REL}_amd64.deb" "madc-${VER}-${REL}.x86_64.rpm" > SHA256SUMS )
echo "== dist/ =="
ls -la dist/
