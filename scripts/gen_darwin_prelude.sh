#!/bin/bash
# Generate the darwin embedded C prelude for hosted-*-macos madc builds.
#
# The run-only Macs have no guaranteed system headers, so a hosted madc
# binary carries the hosted C standard/POSIX headers itself:
#
#   - ONE flattened umbrella (__madc_darwin_prelude.h): clang -E against
#     the staged SDK pre-expands Apple's cdefs/availability macro
#     machinery; -dD keeps every #define as text so madc's OWN
#     preprocessor installs EOF, NULL, stdin=__stdinp, ... at include
#     time (a .madh token stream cannot carry macro state — that is why
#     this is embedded TEXT, not a baked PCH).
#   - a one-line stub per header name (#include <__madc_darwin_prelude.h>)
#     so any standard include pulls the umbrella exactly once (the
#     lexer's name-level once-only dedup makes the second include free).
#
# One umbrella instead of per-header flattening because independent
# flattening duplicates shared sub-headers: madc accepts repeated
# identical typedefs but (correctly) rejects repeated struct definitions
# (struct timespec appears in several closures).
#
# The output feeds scripts/gen_embedded_headers.sh as an EXTRA root for
# the per-mode embedded_headers.cpp (src/Makefile hosted MODEs). Content
# is SDK-derived: it must NEVER be committed or synced out of the build
# tree (the per-mode obj/ dir is the only legitimate home).
#
# Usage: gen_darwin_prelude.sh <target-triple> <sdk-path> <outdir>
#        CLANG overrides the compiler (default clang-18).

set -e

CLANG="${CLANG:-clang-18}"
TARGET="$1"
SDK="$2"
OUTDIR="$3"

if [ -z "$TARGET" ] || [ -z "$SDK" ] || [ -z "$OUTDIR" ]; then
    echo "usage: $0 <target-triple> <sdk-path> <outdir>" >&2
    exit 2
fi
if [ ! -d "$SDK/usr/include" ]; then
    echo "Error: $SDK/usr/include not found (staged macOS SDK required)" >&2
    exit 1
fi

# Hosted C standard/POSIX headers baked into hosted-darwin madc binaries.
# The freestanding six (stddef/stdint/float/limits/stdarg/stdbool) are
# already embedded from include/madc/ and stay OUT of this list.
HEADERS=(
    assert.h ctype.h errno.h fcntl.h inttypes.h locale.h math.h
    setjmp.h signal.h stdio.h stdlib.h string.h strings.h time.h
    unistd.h dirent.h getopt.h libgen.h
    sys/types.h sys/stat.h sys/time.h sys/wait.h sys/resource.h
    sys/mman.h sys/select.h sys/uio.h sys/utsname.h sys/ioctl.h
)

mkdir -p "$OUTDIR"

# Replace dst with src only on real content change (mtime hygiene: the
# Makefile runs this at parse time on every hosted build).
write_on_change() {
    local src="$1" dst="$2"
    if [ ! -f "$dst" ] || ! cmp -s "$src" "$dst"; then
        mv "$src" "$dst"
    else
        rm -f "$src"
    fi
}

UMB_TMP="$OUTDIR/.umbrella.$$"
{
    for h in "${HEADERS[@]}"; do
        echo "#include <$h>"
    done
} > "$UMB_TMP.c"

# _Nullable & friends are clang keywords that survive -E as bare tokens in
# declarations; predefining them empty scrubs the uses (the #define lines
# -dD keeps are inert — clang already expanded every macro use).
# -fno-blocks: darwin targets default -fblocks ON, which defines __BLOCKS__
# and pulls block-typed declarations (qsort_b & co, `^` syntax) that madc —
# like gcc — does not support; the headers guard them, so this compiles
# them out cleanly.
if ! $CLANG -target "$TARGET" --sysroot "$SDK" -x c -std=c11 -fno-blocks \
        -D_Nullable= -D_Nonnull= -D_Null_unspecified= \
        -E -dD -P "$UMB_TMP.c" -o "$UMB_TMP"; then
    rm -f "$UMB_TMP.c" "$UMB_TMP"
    echo "Error: $CLANG preprocess of the darwin prelude failed" >&2
    exit 1
fi
rm -f "$UMB_TMP.c"

# A prelude without printf is the exact silent failure this script exists
# to prevent — refuse to install it.
if ! grep -q "int printf" "$UMB_TMP"; then
    rm -f "$UMB_TMP"
    echo "Error: generated darwin prelude lacks printf — SDK/clang mismatch?" >&2
    exit 1
fi

write_on_change "$UMB_TMP" "$OUTDIR/__madc_darwin_prelude.h"

for h in "${HEADERS[@]}"; do
    mkdir -p "$OUTDIR/$(dirname "$h")"
    STUB_TMP="$OUTDIR/.stub.$$"
    printf '#include <__madc_darwin_prelude.h>\n' > "$STUB_TMP"
    write_on_change "$STUB_TMP" "$OUTDIR/$h"
done

# .MANIFEST: the covered header names in canonical order — the ONE owner of
# the list. scripts/forest_pack_darwin.sh reads it to generate the grove
# freeze TU (forest-carriers plan S1), so the frozen units always match the
# embedded set exactly. Dot-named so gen_embedded_headers.sh's dotfile skip
# keeps it out of the embedded table.
MAN_TMP="$OUTDIR/.manifest.$$"
printf '%s\n' "${HEADERS[@]}" > "$MAN_TMP"
write_on_change "$MAN_TMP" "$OUTDIR/.MANIFEST"

echo "darwin prelude: $OUTDIR (${#HEADERS[@]} header names)"
