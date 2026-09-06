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
#   /usr/share/madcide/profiles/                keybinding/theme profiles
#   /usr/share/man/man1/madc.1.gz + madcide.1.gz
#   /usr/share/doc/madc/copyright               LICENSE (MPL-2.0)
#   /usr/share/doc/madc/changelog.gz            CHANGELOG.md
#   /usr/share/doc/madc/examples/madc.ini       documented example config
#
# Output artifacts: madc_<ver>-<rel>_amd64.deb, madc-<ver>-<rel>.x86_64.rpm,
# madc-<ver>-linux-x86_64.tar.gz (+ README-linux.txt inside).
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
    # $3 (prefix) is "usr" for the deb/rpm filesystem layout and "" for
    # the relocatable tarball root — the SAME staging lines serve all
    # three packages (one implementation; only the layout parameterizes).
    local root="$1" libdir="$2" prefix="$3"
    local p="$root${prefix:+/$prefix}"
    mkdir -p "$p/bin" "$root/$libdir" \
             "$p/share/man/man1" "$p/share/doc/madc/examples"
    install -m 755 bin/madc-release "$p/bin/madc"
    # NO strip here: since the PK2 shared default, `make release` strips
    # the release library BEFORE packing the forest into it (strip-before-
    # pack ordering, src/Makefile) — stripping again rewrites the ELF and
    # silently drops the appended forest container. lib/release/ is the
    # release mode's OWN product dir (per-mode-names law): a dev rebuild
    # can never swap this file.
    install -m 644 lib/release/libmadc.so "$root/$libdir/libmadc.so.0"
    ln -s libmadc.so.0 "$root/$libdir/libmadc.so"
    # The emitted-C runtime (a few KB, static): what `madc --emit=c11`
    # output links on a box with no madc at all (cc prog.c -lmadc_rt) —
    # try/catch context stack + VLA scope-exit helpers, nothing else.
    # Platform parity: the mac and win archives already ship it.
    install -m 644 lib/release/libmadc_rt.a "$root/$libdir/libmadc_rt.a"
    install -m 755 tmp/madcide-pkg "$p/bin/madcide"
    mkdir -p "$p/share/madcide/profiles"
    install -m 644 tools/madcide/profiles/* "$p/share/madcide/profiles/"
    gzip -9n < docs/man/madc.1 > "$p/share/man/man1/madc.1.gz"
    gzip -9n < docs/man/madcide.1 > "$p/share/man/man1/madcide.1.gz"
    install -m 644 LICENSE "$p/share/doc/madc/copyright"
    gzip -9n < CHANGELOG.md > "$p/share/doc/madc/changelog.gz"
    # The example config keeps its real name: share/doc is not on
    # madc.ini's search path, so it can never shadow a user's config.
    install -m 644 docs/examples/madc.ini "$p/share/doc/madc/examples/madc.ini"
    # dpkg-deb requires plain 0755 directories. GNU chmod's NUMERIC modes
    # deliberately preserve a directory's setgid bit (inherited from the
    # checkout), so it must be cleared symbolically first.
    find "$root" -type d -exec chmod g-s {} +
    find "$root" -type d -exec chmod 0755 {} +
}

# ---------- deb ----------
DEBROOT=tmp/pkgroot/deb
stage "$DEBROOT" "usr/lib/x86_64-linux-gnu" usr
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
stage "$BUILDROOT" "usr/lib64" usr
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
/usr/lib64/libmadc_rt.a
/usr/share/madcide
%doc /usr/share/doc/madc/copyright
%doc /usr/share/doc/madc/changelog.gz
%doc /usr/share/doc/madc/examples/madc.ini
/usr/share/man/man1/madc.1.gz
/usr/share/man/man1/madcide.1.gz
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
( cd dist && sha256sum "madc_${VER}-${REL}_amd64.deb" "madc-${VER}-${REL}.x86_64.rpm" \
                       "$TROOT.tar.gz" > SHA256SUMS )
echo "== dist/ =="
ls -la dist/
