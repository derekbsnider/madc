#!/bin/bash
# build_win_ucrt_libstdcxx.sh — one-time container stage for the windows
# release lane (Track 6.4 W1): gcc's libstdc++-v3 built AGAINST UCRT.
#
# Why this exists: Ubuntu's mingw-w64 toolchain ships ONE libstdc++, built
# against its msvcrt CRT default. The C++ ABI leaks the CRT flavor through
# std::mbstate_t (msvcrt: int; UCRT: _Mbstatet), so every fpos<mbstate_t>
# symbol — istream::seekg(pos), the streambuf seekpos/seekoff vtable slots,
# stringbuf::seekpos — mangles differently under -D_UCRT, and the prebuilt
# archive cannot satisfy UCRT-compiled objects (the W1 slice-14 link
# failure). MSYS2 and Fedora solve this by shipping a UCRT-rebuilt C++
# runtime; noble has no such flavor, so we build libstdc++-v3 from the SAME
# 13.2.0 source series as the distro cross gcc, once per container, staged
# like the per-target zstd builds. The hosted-x86-64-windows MODE consumes
# the stage via -nostdinc++ -isystem + -L: headers AND archive must both be
# the UCRT flavor (bits/c++config.h is build-generated).
#
# Wired into scripts/provision_container.sh (winlane section) — the
# container is disposable; everything it needs is provisioned from the repo.
set -euo pipefail

GCC_VER=13.2.0
MINGW=x86_64-w64-mingw32
ROOT=/workspace/win-ucrt-libstdc++
SRC=$ROOT/gcc-$GCC_VER
BUILD=$ROOT/build
STAGE=$ROOT/stage
UCRT_FLAGS="-D_UCRT -D__USE_MINGW_ANSI_STDIO=1"

if [ -f "$STAGE/lib/libstdc++.a" ] && [ "${1:-}" != "--force" ]; then
	echo "build_win_ucrt_libstdcxx: already staged at $STAGE (--force to rebuild)"
	exit 0
fi

for tool in $MINGW-gcc-posix $MINGW-g++-posix $MINGW-nm; do
	if ! command -v "$tool" > /dev/null 2>&1; then
		echo "build_win_ucrt_libstdcxx: MISSING $tool — run scripts/provision_container.sh"
		exit 2
	fi
done

mkdir -p "$ROOT"
cd "$ROOT"
if [ ! -d "$SRC" ]; then
	echo "build_win_ucrt_libstdcxx: fetching gcc-$GCC_VER source"
	wget -q "https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VER/gcc-$GCC_VER.tar.xz"
	tar xf "gcc-$GCC_VER.tar.xz"
	rm -f "gcc-$GCC_VER.tar.xz"
fi

rm -rf "$BUILD" "$STAGE"
mkdir -p "$BUILD"

# Standalone-build shim: libstdc++'s gthreads probe (and its own sources)
# include libgcc/gthr.h, which includes gthr-default.h — a file a full gcc
# build GENERATES into its libgcc build dir by copying gthr-<model>.h for
# the configured thread model. The probe looks in $BUILD/../libgcc; without
# the file, gthreads silently configures OFF and the staged lib has no
# std::mutex/std::thread (first loud symptom: tzdb.cc fails to compile).
# Synthesize it exactly as libgcc/Makefile.in does for --enable-threads=posix.
mkdir -p "$ROOT/libgcc"
cp "$SRC/libgcc/gthr-posix.h" "$ROOT/libgcc/gthr-default.h"

cd "$BUILD"
CC="$MINGW-gcc-posix $UCRT_FLAGS" \
CXX="$MINGW-g++-posix $UCRT_FLAGS" \
"$SRC/libstdc++-v3/configure" \
	--host=$MINGW \
	--prefix="$STAGE" \
	--disable-shared \
	--enable-static \
	--disable-libstdcxx-pch \
	--enable-threads=posix \
	--disable-nls
make -j"$(nproc)"
make install

# Proof the stage is the UCRT flavor: the _Mbstatet-mangled fpos symbol
# must exist in the staged archive (the msvcrt flavor mangles fpos<int>).
# NOTE: not `nm | grep -q` — under pipefail, grep -q's early exit SIGPIPEs
# nm and fails the pipeline on a MATCH. Materialize, then grep.
$MINGW-nm -C "$STAGE/lib/libstdc++.a" > "$BUILD/stage-nm.txt"
if ! grep -q "seekg(std::fpos<_Mbstatet>)" "$BUILD/stage-nm.txt"; then
	echo "build_win_ucrt_libstdcxx: STAGE BROKEN — no fpos<_Mbstatet> seekg in staged libstdc++"
	exit 1
fi
echo "build_win_ucrt_libstdcxx: staged at $STAGE"
