#!/bin/bash
# The ONE translation of a stage target into the CMake toolchain arguments
# that build a third-party static archive with madc's own compiler line for
# that target. The scripts that stage a CMake-built dependency read it
# (stage_libgit2.sh, stage_cmark_gfm.sh); a library's own options — its
# feature switches, its zlib policy — stay in its script.
#
#   bash scripts/stage_cmake_args.sh <host|x86-64-windows|arm64-macos|x86-64-macos>
#       prints the arguments, ONE PER LINE (a value may contain spaces):
#       (read them into an array line by line: stage_libgit2.sh)
#   bash scripts/stage_cmake_args.sh --target <target>
#       prints the canonical target (host -> x86-64-linux, aarch64-linux)
#
# The compilers + archiver are the target's hosted MODE's OWN (make print-CC /
# print-CXX / print-AR: mingw-w64 UCRT over specs for windows, cross clang-18 + llvm-ar-18
# for macOS, the host's default build for Linux) — never a copy kept here.
# Every archive links into a shared module, so the host build is
# position-independent; a cross target names its system and keeps CMake's
# searches inside it; a macOS target builds against the hosted MODE's SDK at
# the MODE's deployment floor with the MODE's linker flags (CMake's compiler
# checks link an executable: without -fuse-ld=lld clang falls back to the host
# GNU ld, which cannot link Mach-O).
set -e

usage="usage: stage_cmake_args.sh [--target] <host|x86-64-windows|arm64-macos|x86-64-macos>"
print_target=
if [ "${1:-}" = "--target" ]; then
    print_target=1
    shift
fi
target="${1:?$usage}"
host_target="$(uname -m | tr _ -)-linux"
case "$target" in
    x86-64-windows|arm64-macos|x86-64-macos) ;;
    x86_64-windows) target=x86-64-windows ;;
    x86_64-macos)   target=x86-64-macos ;;
    host|"$host_target") target=$host_target ;;
    *) echo "stage_cmake_args: unknown target '$target' ($usage)" >&2; exit 2 ;;
esac
if [ -n "$print_target" ]; then
    echo "$target"
    exit 0
fi
cd "$(dirname "$0")/.."

case "$target" in
    *-linux) MODE= ; MODEARG=() ;;
    *)       MODE="hosted-$target" ; MODEARG=( MODE="$MODE" ) ;;
esac
# print-CC yields "<program> <flags...>"; CMake wants the program and its
# flags apart.
CCLINE=$(make -C src -s "${MODEARG[@]}" print-CC)
CXXLINE=$(make -C src -s "${MODEARG[@]}" print-CXX)
AR=$(make -C src -s "${MODEARG[@]}" print-AR)
[ -n "$CCLINE" ] && [ -n "$CXXLINE" ] && [ -n "$AR" ] || { echo "stage_cmake_args ($target): could not read CC/CXX/AR of MODE=${MODE:-default} from src/Makefile" >&2; exit 1; }
CC_PROG=${CCLINE%% *}
CC_FLAGS=${CCLINE#* }
[ "$CC_FLAGS" = "$CCLINE" ] && CC_FLAGS=
CXX_PROG=${CXXLINE%% *}
CXX_FLAGS=${CXXLINE#* }
[ "$CXX_FLAGS" = "$CXXLINE" ] && CXX_FLAGS=
# ranlib beside the archiver (llvm-ar-18 -> llvm-ranlib-18; *-ar -> *-ranlib).
RANLIB=$(printf '%s' "$AR" | sed 's/\(.*\)ar/\1ranlib/')

# A project that enables C++ (cmark-gfm's bare project() does) checks the
# C++ compiler too; one that does not leaves it unused, hence
# --no-warn-unused-cli. CMP0056 NEW: the compiler checks link with the
# linker flags below — a project declaring an old cmake_minimum_required
# (cmark-gfm: 3.0) would otherwise link them with the host's GNU ld.
echo "--no-warn-unused-cli"
echo "-DCMAKE_POLICY_DEFAULT_CMP0056=NEW"
echo "-DCMAKE_BUILD_TYPE=Release"
echo "-DCMAKE_C_COMPILER=$CC_PROG"
echo "-DCMAKE_C_FLAGS=$CC_FLAGS"
echo "-DCMAKE_CXX_COMPILER=$CXX_PROG"
echo "-DCMAKE_CXX_FLAGS=$CXX_FLAGS"
echo "-DCMAKE_AR=$(command -v "$AR" || echo "$AR")"
echo "-DCMAKE_RANLIB=$(command -v "$RANLIB" || echo "$RANLIB")"
case "$target" in
    *-linux)
        echo "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
        exit 0
        ;;
    *-windows) SYSNAME=Windows ;;
    *-macos)   SYSNAME=Darwin ;;
esac
echo "-DCMAKE_SYSTEM_NAME=$SYSNAME"
echo "-DCMAKE_SYSTEM_PROCESSOR=x86_64"
echo "-DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER"
echo "-DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY"
echo "-DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY"
case "$target" in
    *-macos)
        arch=${target%-macos}
        LDF=$(make -C src -s MODE="$MODE" print-DARWIN_LD_FLAGS)
        # The SDK sysroot: the container's staged SDK on a cross build, Xcode's
        # (xcrun --show-sdk-path) on a native darwin host — whichever the
        # hosted MODE uses.
        SDK=$(make -C src -s MODE="$MODE" print-MACOS_SDK)
        MINOS=$(make -C src -s MODE="$MODE" print-MACOS_MINOS)
        [ -n "$SDK" ] && [ -n "$MINOS" ] || { echo "stage_cmake_args ($target): could not read MACOS_SDK/MACOS_MINOS of MODE=$MODE from src/Makefile" >&2; exit 1; }
        echo "-DCMAKE_OSX_SYSROOT=$SDK"
        echo "-DCMAKE_OSX_ARCHITECTURES=$(printf %s "$arch" | sed 's/x86-64/x86_64/')"
        echo "-DCMAKE_OSX_DEPLOYMENT_TARGET=$MINOS"
        echo "-DCMAKE_EXE_LINKER_FLAGS=$LDF"
        echo "-DCMAKE_SHARED_LINKER_FLAGS=$LDF"
        echo "-DCMAKE_MODULE_LINKER_FLAGS=$LDF"
        ;;
esac
