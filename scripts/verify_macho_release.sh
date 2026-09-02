#!/bin/bash
# verify_macho_release.sh — container-side gate for a stripped darwin release
# binary (macos-release-lane plan W1): the strip must not have cost the
# forest carrier or the code signature.
#
#   bash scripts/verify_macho_release.sh <macho-binary> <expected-forest.bin>
#
# Four authorities, runnable on the Linux container AND natively on a darwin
# build host (darwin-host port D2 — the host's brew coreutils/llvm serve the
# GNU spellings; MADC_READER names the reader, OTOOL the load-command dumper):
#   1. the __MADC,__forest section exists and its BYTES equal the freeze's
#      standalone container (extracted by load-command offset/size);
#   2. the extracted bytes read back as a forest (the same --dump-forest
#      checks the pack gate applies) through the READER: the Linux bin/madc
#      on the container, the unstripped hosted binary on a darwin host — a
#      madc of the same tree either way (freeze-context hash);
#   3. arm64 slices carry LC_CODE_SIGNATURE (llvm-strip re-signs ad-hoc when
#      it rewrites the file; a missing signature means AMFI kills the binary
#      on sight). x86-64 darwin does not require one.
#   4. the embedded C prelude is OPEN-provenance (W0.5): the umbrella's
#      marker line rides in .rodata, so the binary names its own prelude
#      input. An "sdk-private" stamp (the never-shipped oracle diff input)
#      or a missing marker refuses the release here, mechanically.
# In-vivo AMFI acceptance stays the Mac battery's job (W2).
set -e

BIN="$1"
FOREST="$2"
# Tool spellings: the container's versioned apt names first, then the
# unversioned ones a brew llvm prefix (or PATH) supplies.
OTOOL="${OTOOL:-$(command -v llvm-otool-18 || command -v llvm-otool || echo llvm-otool-18)}"
READER="${MADC_READER:-bin/madc}"
TIMEOUT="$(command -v timeout || command -v gtimeout || true)"
if [ -z "$TIMEOUT" ]; then
    echo "verify_macho_release: needs GNU timeout (brew install coreutils on a Mac)" >&2
    exit 1
fi

if [ -z "$BIN" ] || [ -z "$FOREST" ]; then
    echo "usage: $0 <macho-binary> <expected-forest.bin>" >&2
    exit 2
fi
if [ ! -f "$BIN" ] || [ ! -f "$FOREST" ]; then
    echo "verify_macho_release: missing input ($BIN / $FOREST)" >&2
    exit 1
fi

LOADCMDS=$("$OTOOL" -l "$BIN")

# 1. The forest section: offset + size from the load commands.
SECT=$(printf '%s\n' "$LOADCMDS" | awk '
    /sectname __forest/ { in_sect=1 }
    in_sect && /^ *size /   { size=$2 }
    in_sect && /^ *offset / { print size, $2; exit }
')
if [ -z "$SECT" ]; then
    echo "verify_macho_release: FAILED — no __MADC,__forest section in $BIN" >&2
    exit 1
fi
SIZE=$((${SECT%% *}))
OFF=${SECT##* }
if [ "$SIZE" -le 0 ]; then
    echo "verify_macho_release: FAILED — empty forest section in $BIN" >&2
    exit 1
fi
# wc -c, not stat: GNU spells `stat -c %s`, BSD `stat -f %z` — wc is portable.
WANT=$(($(wc -c < "$FOREST")))
if [ "$SIZE" -ne "$WANT" ]; then
    echo "verify_macho_release: FAILED — section size $SIZE != container $WANT" >&2
    exit 1
fi

EXTRACT="$BIN.forest-extract"
tail -c +$((OFF + 1)) "$BIN" | head -c "$SIZE" > "$EXTRACT"
if ! cmp -s "$EXTRACT" "$FOREST"; then
    rm -f "$EXTRACT"
    echo "verify_macho_release: FAILED — embedded forest bytes differ from $FOREST" >&2
    exit 1
fi

# 2. Read-back through the production reader.
DUMP="$EXTRACT.dump"
if ! "$TIMEOUT" 120 "$READER" --dump-forest="$EXTRACT" > "$DUMP" 2>&1; then
    rm -f "$EXTRACT" "$DUMP"
    echo "verify_macho_release: FAILED — extracted forest does not read back" >&2
    exit 1
fi
grep -q '^forest	units=' "$DUMP"
grep -q '^ledger	modules=' "$DUMP"
UNITS=$(grep -c '^unit	' "$DUMP")
rm -f "$EXTRACT" "$DUMP"

# 3. arm64 must be signed after the strip rewrite.
ARCH=$("$OTOOL" -h "$BIN" | awk '/magic/ { getline; print $2 }')
if [ "$ARCH" = "0x0100000c" ] || [ "$ARCH" = "16777228" ]; then
    if ! printf '%s\n' "$LOADCMDS" | grep -q 'LC_CODE_SIGNATURE'; then
        echo "verify_macho_release: FAILED — arm64 slice has no LC_CODE_SIGNATURE" >&2
        exit 1
    fi
fi

# 4. Open-provenance prelude only. Positive match on the open stamp shape
#    (scripts/fetch_darwin_open_headers.sh is the stamp's one owner) — an
#    unknown or sdk-private stamp fails; so does a missing marker.
PROV=$(grep -a -o 'MADC-DARWIN-PRELUDE-PROVENANCE: [^*]*' "$BIN" | head -1 | sed 's/ *$//')
case "$PROV" in
    "MADC-DARWIN-PRELUDE-PROVENANCE: zig-"*" sha256="*)
        ;;
    "")
        echo "verify_macho_release: FAILED — no prelude provenance marker in $BIN" >&2
        exit 1
        ;;
    *)
        echo "verify_macho_release: FAILED — prelude is not open-provenance: $PROV" >&2
        exit 1
        ;;
esac

echo "verify_macho_release: OK ($BIN: $UNITS units, forest bytes intact, ${PROV#MADC-DARWIN-PRELUDE-PROVENANCE: })"
