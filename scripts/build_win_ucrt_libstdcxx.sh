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
# The stage builds BOTH flavors: the static archive (link probes, AOT
# static consumers) AND libstdc++-6.dll + its import lib. The DLL is the
# win64 mangled-direct door (the darwin LC_LOAD_DYLIB analogue): PE ld
# auto-excludes runtime archives from --export-all-symbols, so a -static
# libstdc++ frozen into madc.exe exposes NO _Z* symbols to GetProcAddress
# and every script-side std:: import is undefined. madc.exe links the
# import lib instead and madcdl_sym_default walks the loaded DLL — one
# libstdc++ instance shared by host and scripts, the Linux posture. The
# DLL's LINK must ride the same ucrt.specs swap as the objects (a plain
# -shared link would pull -lmsvcrt = a second CRT in-process), and
# -static-libgcc keeps the distro's msvcrt-flavor libgcc DLL out of it.
#
# The stage also builds winpthreads (mingw-w64 source, same series as the
# distro toolchain) as a UCRT-flavored libwinpthread-1.dll: the shared
# libstdc++ imports winpthread by name, the distro's DLL imports
# msvcrt.dll (second CRT), and a static-in-exe + shared-in-DLL split
# would put TWO winpthread instances in one process — pthread objects
# cross the exe<->DLL boundary (std::thread's _M_start_thread/join are
# compiled in the DLL; madc_process pump threads are host-side), and
# winpthread handles are per-instance allocations. One shared UCRT
# winpthread serves exe, libstdc++-6.dll, and later AOT executables.
#
# Wired into scripts/provision_container.sh (winlane section) — the
# container is disposable; everything it needs is provisioned from the repo.
set -euo pipefail

GCC_VER=13.2.0
MINGW64_VER=11.0.1
MINGW=x86_64-w64-mingw32
ROOT=/workspace/win-ucrt-libstdc++
SRC=$ROOT/gcc-$GCC_VER
PTHREAD_SRC=$ROOT/mingw-w64-$MINGW64_VER
BUILD=$ROOT/build
PTHREAD_BUILD=$ROOT/build-winpthreads
STAGE=$ROOT/stage
UCRT_FLAGS="-D_UCRT -D__USE_MINGW_ANSI_STDIO=1"

if [ -f "$STAGE/lib/libstdc++.a" ] && [ -f "$STAGE/bin/libstdc++-6.dll" ] && [ -f "$STAGE/bin/libwinpthread-1.dll" ] && [ "${1:-}" != "--force" ]; then
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
if [ ! -d "$PTHREAD_SRC" ]; then
	echo "build_win_ucrt_libstdcxx: fetching mingw-w64-$MINGW64_VER source (winpthreads)"
	wget -q "https://github.com/mingw-w64/mingw-w64/archive/refs/tags/v$MINGW64_VER.tar.gz" -O "mingw-w64-$MINGW64_VER.tar.gz"
	tar xf "mingw-w64-$MINGW64_VER.tar.gz"
	rm -f "mingw-w64-$MINGW64_VER.tar.gz"
fi

rm -rf "$BUILD" "$PTHREAD_BUILD" "$STAGE"
mkdir -p "$BUILD" "$PTHREAD_BUILD"

# Standalone-build shim: libstdc++'s gthreads probe (and its own sources)
# include libgcc/gthr.h, which includes gthr-default.h — a file a full gcc
# build GENERATES into its libgcc build dir by copying gthr-<model>.h for
# the configured thread model. The probe looks in $BUILD/../libgcc; without
# the file, gthreads silently configures OFF and the staged lib has no
# std::mutex/std::thread (first loud symptom: tzdb.cc fails to compile).
# Synthesize it exactly as libgcc/Makefile.in does for --enable-threads=posix.
mkdir -p "$ROOT/libgcc"
cp "$SRC/libgcc/gthr-posix.h" "$ROOT/libgcc/gthr-default.h"

# UCRT-flavor LINK environment for the DLL: same specs swap the hosted
# MODE uses for madc.exe itself (-lmsvcrt -> -lucrt), or the shared
# libstdc++ binds a second CRT into every process that loads it.
$MINGW-gcc-posix -dumpspecs | sed 's/-lmsvcrt/-lucrt/g' > "$ROOT/ucrt.specs"
UCRT_LINK="-specs=$ROOT/ucrt.specs -static-libgcc"

# winpthreads first, so the libstdc++ link below resolves -lpthread from
# the stage (UCRT flavor all the way down). Same source series as the
# distro toolchain (__MINGW64_VERSION_MAJOR); the DLL keeps the standard
# libwinpthread-1.dll name, so anything built against the distro import
# lib binds ours at runtime — PE imports resolve by DLL name.
cd "$PTHREAD_BUILD"
CC="$MINGW-gcc-posix $UCRT_FLAGS $UCRT_LINK" \
CXX="$MINGW-g++-posix $UCRT_FLAGS $UCRT_LINK" \
LDFLAGS="$UCRT_LINK" \
"$PTHREAD_SRC/mingw-w64-libraries/winpthreads/configure" \
	--host=$MINGW \
	--prefix="$STAGE" \
	--enable-shared \
	--enable-static
make -j"$(nproc)"
make install

cd "$BUILD"
CC="$MINGW-gcc-posix $UCRT_FLAGS $UCRT_LINK" \
CXX="$MINGW-g++-posix $UCRT_FLAGS $UCRT_LINK" \
LDFLAGS="$UCRT_LINK -L$STAGE/lib" \
"$SRC/libstdc++-v3/configure" \
	--host=$MINGW \
	--prefix="$STAGE" \
	--enable-shared \
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

# Shared-flavor gates: the DLL must exist, export the UCRT-mangled fpos
# surface (same flavor proof as the archive), and bind NO msvcrt.dll —
# a msvcrt import means the specs swap did not reach the DLL link and
# every madc.exe process would carry two CRTs.
DLL="$STAGE/bin/libstdc++-6.dll"
if [ ! -f "$DLL" ]; then
	echo "build_win_ucrt_libstdcxx: STAGE BROKEN — no libstdc++-6.dll (shared build missing)"
	exit 1
fi
$MINGW-objdump -p "$DLL" > "$BUILD/dll-objdump.txt"
$MINGW-nm -C "$STAGE/lib/libstdc++.dll.a" > "$BUILD/dll-nm.txt"
if ! grep -q "seekg(std::fpos<_Mbstatet>)" "$BUILD/dll-nm.txt"; then
	echo "build_win_ucrt_libstdcxx: STAGE BROKEN — DLL export surface is not the UCRT flavor"
	exit 1
fi
if grep -qi "DLL Name: msvcrt.dll" "$BUILD/dll-objdump.txt"; then
	echo "build_win_ucrt_libstdcxx: STAGE BROKEN — libstdc++-6.dll imports msvcrt.dll (second CRT)"
	exit 1
fi
PTHREAD_DLL="$STAGE/bin/libwinpthread-1.dll"
if [ ! -f "$PTHREAD_DLL" ]; then
	echo "build_win_ucrt_libstdcxx: STAGE BROKEN — no libwinpthread-1.dll (winpthreads build missing)"
	exit 1
fi
$MINGW-objdump -p "$PTHREAD_DLL" > "$BUILD/pthread-objdump.txt"
if grep -qi "DLL Name: msvcrt.dll" "$BUILD/pthread-objdump.txt"; then
	echo "build_win_ucrt_libstdcxx: STAGE BROKEN — libwinpthread-1.dll imports msvcrt.dll (second CRT; the distro DLL does exactly this — the stage build must not)"
	exit 1
fi
echo "build_win_ucrt_libstdcxx: libstdc++-6.dll imports:"
grep "DLL Name" "$BUILD/dll-objdump.txt" || true
echo "build_win_ucrt_libstdcxx: libwinpthread-1.dll imports:"
grep "DLL Name" "$BUILD/pthread-objdump.txt" || true
echo "build_win_ucrt_libstdcxx: staged at $STAGE"
