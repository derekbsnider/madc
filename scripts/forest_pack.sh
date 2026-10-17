#!/bin/bash
# forest_pack.sh — build the standard-header forest container and ship it
# with a madc binary (the release pack step; design doc
# 2026-07-04-forest-default-mode-design.md §7).
#
#   bash scripts/forest_pack.sh [--dialect-image] [--sidecar] [--image <file>] [bin/madc]
#
# Default (embedded carrier): append the container to the binary itself.
# MUST run AFTER strip: strip rewrites the ELF image and discards appended
# bytes, so strip-after-pack destroys the blob (mandatory ordering).
# Re-packing an already-packed binary appends a new footer; the reader finds
# the LAST one, so correctness holds (the stale blob's bytes are dead weight
# until the next relink).
#
# --sidecar (forest-carriers S3, --with-forest=sidecar): write the container
# as a standalone <image>.forest file beside the image instead — a
# --freeze-append onto an EMPTY file IS a valid standalone container
# (placement 2, container at offset 0; the S1 darwin cross-freeze precedent).
# The runtime discovery chain finds it as a sidecar arm.
#
# --image <file> (forest-carriers S4, --enable-shared): pack a carrier image
# OTHER than the binary — the shared libmadc, whose image the thin CLI and
# every embedding host load, so one container serves them all (discovery arm
# 2). The named binary still does the freezing and the verification; only the
# carrier changes. Same strip-before-pack ordering applies to that image.
#
# --dialect-image (pack-slices design §7; dialect per-name plan B1): the madc
# dialect surface as its own container, the DIALECT IMAGE — the compiler's
# dialect roots (--print-dialect-roots: every dialect fragment and the
# headers their auto-include pulls, in its order) frozen under --std=madc,
# and nothing else: no source arm, no AOT ledger, no shared standards, none
# of the pack list's C headers. A missing prerequisite fails the freeze (a
# parse error refuses the compile; the gate's pack-error count is zero). It
# carries ROLE dialect (--freeze-role=dialect), so every
# segment is stored raw: no zstd, no blocks, read in place (and no layout
# pass — it has no block to lay out). EVERY product mode ships it: it is the
# FIRST container of the carrier the system-header pack rides (the image, or
# the sidecar file it starts), so a discovery walk, newest first, meets the
# system pack before it (B2 filters the walk by role). A run without
# --dialect-image packs the image first, then the system-header pack after
# it; --with-forest=none packs the image alone — that build reads system
# headers from disk (it bakes no MADC_FOREST_EXPECT_*), and a strict compile
# never binds a madc-only profile.
#
# Pack failure = build failure (set -e).
set -e
cd "$(dirname "$0")/.."
. scripts/forest_pack_share.sh	# the standards the one profile serves
. scripts/forest_pack_layout.sh	# the layout pass (format 60)
. scripts/forest_pack_roots.sh	# the list's roots by population

SIDECAR=0
DIALECT_IMAGE=0
IMAGE=
while [ $# -gt 0 ]; do
    case "$1" in
	--sidecar) SIDECAR=1; shift ;;
	--dialect-image) DIALECT_IMAGE=1; shift ;;
	--image) IMAGE="$2"; shift 2 ;;
	--image=*) IMAGE="${1#--image=}"; shift ;;
	*) break ;;
    esac
done
BIN="${1:-bin/madc}"
IMAGE="${IMAGE:-$BIN}"
LIST=scripts/forest_pack_headers.txt
GUARDED_LIST=scripts/forest_pack_guarded.txt
LEDGER_LIST=scripts/ledger_sources.txt
# Each stage keeps its own work files: the image's stage runs inside the
# system pack's run, and both stay for inspection.
if [ "$DIALECT_IMAGE" = 1 ]; then
    STAGE=tmp/forest_pack_dialect
else
    STAGE=tmp/forest_pack
fi

# The dialect image goes into the carrier first, in every product mode.
if [ "$DIALECT_IMAGE" = 0 ]; then
    if [ "$SIDECAR" = 1 ]; then
	bash scripts/forest_pack.sh --dialect-image --sidecar --image "$IMAGE" "$BIN"
    else
	bash scripts/forest_pack.sh --dialect-image --image "$IMAGE" "$BIN"
    fi
fi

# The libstdc++ include root, ASKED not assumed: the guarded roots below are
# included by absolute path, and a hard-coded /usr/include/c++/<ver> would rot
# on the next gcc bump — silently, since a missing guarded root only shows up
# on the headerless lane under --std=c++20. Same fact the win64 pack gets from
# MADC_WIN_CXX_INCLUDE, discovered here because the host toolchain owns it.
CXX_INCLUDE="${MADC_CXX_INCLUDE:-}"
if [ -z "$CXX_INCLUDE" ]; then
    CXX_INCLUDE=$(echo | ${CXX:-g++} -E -Wp,-v -x c++ - 2>&1 >/dev/null \
        | sed -n 's/^ \(\/.*\/c++\/[^/]*\)$/\1/p' | head -1)
fi
if [ -z "$CXX_INCLUDE" ] || [ ! -d "$CXX_INCLUDE" ]; then
    echo "forest_pack: could not locate the libstdc++ include root" >&2
    echo "  (set MADC_CXX_INCLUDE; the guarded roots need an absolute path)" >&2
    exit 1
fi
mkdir -p tmp
TU=${STAGE}_tu.cpp

# --dialect-image: the roots are the COMPILER's (--print-dialect-roots, under
# --std=madc): every dialect fragment the auto-include's provider table names
# and the headers their own auto-include pulls (today <stdio.h>, for
# <ns_ui_web>'s stderr), in the auto-include's order — the two owners a
# dialect compile includes them through (Program::queue_fragment_prerequisites,
# ordered_auto_include_headers). Never the pack list's C portion: the image
# holds what a dialect compile reaches, nothing more (an explicitly included
# fragment gathers no prerequisites, so they are roots ahead of it). Nothing
# drops silently: the printed fragments are exactly include/madc/'s ns_*
# fragments, and, while the pack list names fragments (list v8; B5 drops
# them), exactly the list's.
if [ "$DIALECT_IMAGE" = 1 ]; then
    DIALECT_LIST=${STAGE}_headers.txt
    timeout 60 "$BIN" --no-config --std=madc --print-dialect-roots > "$DIALECT_LIST"
    printed=$(grep '^ns_' "$DIALECT_LIST" | sort)
    layout=$(ls include/madc | grep '^ns_' | grep -v '\.' | sort)
    listed=$(forest_pack_roots "$LIST" dialect | grep '^ns_' | sort)
    if [ -z "$printed" ] || [ "$printed" != "$layout" ]; then
	echo "forest_pack: --dialect-image: the printed dialect roots are not include/madc's fragments" >&2
	diff <(echo "$layout") <(echo "$printed") >&2 || true
	exit 1
    fi
    if [ -n "$listed" ] && [ "$printed" != "$listed" ]; then
	echo "forest_pack: --dialect-image: the printed dialect roots are not $LIST's fragments" >&2
	diff <(echo "$listed") <(echo "$printed") >&2 || true
	exit 1
    fi
    LIST=$DIALECT_LIST
fi

# The SOURCE ARM (owner 2026-10-13: the pack holds ALL system headers, never a
# test-driven subset): every header the C library's, the kernel headers' and
# the C++ standard library's packages install, GENERATED here from the
# package manager's own lists, packed as source-only units beside the frozen
# grove (--freeze-sources=) and checked unit by unit by the gate
# (--units-from). docs/plans/2026-10-13-pack-all-system-headers.md.
# (None in the dialect image: the system pack carries them.)
SOURCES_LIST=${STAGE}_sources.txt
if [ "$DIALECT_IMAGE" = 1 ]; then
    : > "$SOURCES_LIST"
elif ! bash scripts/forest_pack_sources.sh linux "$CXX_INCLUDE" > "$SOURCES_LIST"; then
    echo "forest_pack: could not list the system headers" >&2
    exit 1
fi

# AOT ledger (forest-carriers S5): the C-lane madc runtime, compiled by THIS
# madc into MIR modules and packed as one more container segment, so
# -static-libmadc programs carry the machinery inside their own image.
# scripts/ledger_sources.txt is the single owner of the tagged list
# (src/Makefile selects from the same file to build these into libmadc).
# (None in the dialect image.)
if [ "$DIALECT_IMAGE" = 1 ]; then
    LEDGER_SOURCES=
elif ! LEDGER_SOURCES=$(bash scripts/select_ledger_sources.sh linux "$LEDGER_LIST"); then
    echo "forest_pack: could not select Linux ledger sources" >&2
    exit 1
fi
LEDGER_ARGS=()
while IFS= read -r src; do
    [ -n "$src" ] || continue
    LEDGER_ARGS+=("--freeze-ledger=$src")
done <<<"$LEDGER_SOURCES"
LEDGER_MODULE_COUNT=${#LEDGER_ARGS[@]}

{
    echo "// generated by scripts/forest_pack.sh from $LIST (canonical order)"
    grep -vE '^[[:space:]]*(#|$)' "$LIST" | while read -r h; do
        echo "#include <$h>"
    done
    # A default-standard preprocess cannot discover these edges — they sit
    # behind __cplusplus guards. Absolute roots keep their canonical sysroot
    # names while their guarded bodies stay semantically empty in this profile;
    # the freeze still records the exact raw text for live C++20
    # re-tokenization. Same mechanism as the win64 pack. (Not in the
    # dialect image: no C++ standard header is its root.)
    [ "$DIALECT_IMAGE" = 1 ] ||
    grep -vE '^[[:space:]]*(#|$)' "$GUARDED_LIST" | while read -r h; do
        echo "#include \"$CXX_INCLUDE/$h\""
    done
    echo "int main() { return 0; }"
} > "$TU"

# The pack runs a COPY of the binary: Linux forbids writing to a running
# executable (ETXTBSY), and (in embedded mode) the append target IS the
# compiler doing the work.
cp "$BIN" tmp/forest_packer_madc

ulimit -t 900
if [ "$SIDECAR" = 1 ]; then
    OUT="$IMAGE.forest"
    # The dialect image is the sidecar's first container: its stage starts
    # the file, the system pack's appends to it.
    if [ "$DIALECT_IMAGE" = 1 ]; then
	rm -f "$OUT"
    fi
    APPEND_TO="$OUT"
elif [ "$IMAGE" = "$BIN" ]; then
    OUT="$IMAGE"
    APPEND_TO="$OUT"
else
    # A carrier image that is not the binary is (in the shared shape) the
    # libmadc the PACKER ITSELF has loaded. Append to a copy and rename it
    # over the image: the running mapping is never written through, and the
    # replacement is atomic.
    OUT="$IMAGE"
    APPEND_TO=tmp/forest_pack_image.bin
    cp -p "$IMAGE" "$APPEND_TO"
fi
# --no-config: the pack must be reproducible. An ambient madc.ini (repo root,
# developer config dir, system config dir) carrying a `std` or `include` key
# would silently change the frozen corpus's PRODUCER CONFIG, after which every
# ordinary compile would fall through the v27 config gate. --std=madc for the
# same reason: the TU is a .cpp, and a source's extension now picks its
# standard (split arc 1A.4: gnu++17) — the corpus is the madc dialect's.
#
# The output is CAPTURED for the degradation gate (task #63): the pack's
# tolerated parse errors live in it, and a pack that exits 0 while a header
# class stopped parsing is exactly the silent loss the ratchet catches.
# Redirect-then-cat, never `| tee` — without pipefail a pipeline's status is
# tee's, so a pack FAILURE would read as success.
PACK_LOG=${STAGE}_freeze.log
FREEZE_ARGS=(--freeze-mir-cache)
if [ "$DIALECT_IMAGE" = 0 ]; then
    FREEZE_ARGS+=("--freeze-sources=$SOURCES_LIST" "${LEDGER_ARGS[@]}" "${FOREST_SHARE_ARGS[@]}")
else
    FREEZE_ARGS+=(--freeze-role=dialect)
fi
if ! timeout 900 tmp/forest_packer_madc --no-config --std=madc "${FREEZE_ARGS[@]}" --freeze-append="$APPEND_TO" "$TU" > "$PACK_LOG" 2>&1; then
    cat "$PACK_LOG"
    echo "forest_pack: FAILED - freeze-append exited nonzero" >&2
    exit 1
fi
cat "$PACK_LOG"
if [ "$DIALECT_IMAGE" = 0 ]; then
    timeout 120 tmp/forest_packer_madc --dump-forest="$APPEND_TO" > ${STAGE}_share_dump.txt
    forest_share_check ${STAGE}_share_dump.txt
    # Lay the container out by the reads the pack's root binds make (format
    # 60). Not the raw dialect image: it has no block to decode or lay out.
    if ! forest_pack_layout "$APPEND_TO" "$LIST" timeout 120 tmp/forest_packer_madc; then
	echo "forest_pack: FAILED - layout pass" >&2
	exit 1
    fi
fi
if [ "$APPEND_TO" != "$OUT" ]; then
    mv -f "$APPEND_TO" "$OUT"
fi

# Verify: the container reads back (context-hash pin + directory) and the
# binary still runs the frozen module — the dlsym-heavy smoke that proves
# plain strip kept .dynsym intact. Packing the binary's OWN image verifies
# through the self-image arm (no path: the running executable's blob); every
# other carrier names the container file explicitly.
DUMP=${STAGE}_dump.txt
if [ "$OUT" = "$BIN" ]; then
    timeout 120 "$BIN" --dump-forest > "$DUMP"
    timeout 120 "$BIN" --run-frozen
else
    timeout 120 "$BIN" --dump-forest="$OUT" > "$DUMP"
    timeout 120 "$BIN" --run-frozen="$OUT"
fi
if ! grep -q '^forest	units=' "$DUMP"; then
    echo "forest_pack: FAILED - packed image has no forest directory" >&2
    exit 1
fi
if [ "$DIALECT_IMAGE" = 0 ] && ! grep -q '^layout	reads' "$DUMP"; then
    echo "forest_pack: FAILED - the packed container was not laid out" >&2
    exit 1
fi
if [ "$DIALECT_IMAGE" = 0 ] &&
   ! grep -q "^ledger	modules=$LEDGER_MODULE_COUNT	symbols=" "$DUMP"; then
    echo "forest_pack: FAILED - expected $LEDGER_MODULE_COUNT selected ledger modules" >&2
    if ! grep -a '^ledger' "$DUMP" >&2; then
        echo "forest_pack: dump has no ledger record" >&2
    fi
    exit 1
fi
while IFS= read -r src; do
    [ -n "$src" ] || continue
    LEDGER_MODULE_PREFIX=$'ledgermod\t'"$src"$'\t'
    if ! grep -Fq "$LEDGER_MODULE_PREFIX" "$DUMP"; then
        echo "forest_pack: FAILED - selected ledger module is missing: $src" >&2
        exit 1
    fi
done <<<"$LEDGER_SOURCES"

# MIR-cache bind-lane equivalence (rung 3, opt-in lane): the packed binary
# compiling a real test with MADC_MIR_CACHE_BIND=1 must (a) actually engage
# the cache lane — a silent fallback would pass every equality check while
# testing nothing — and (b) produce byte-identical output vs the default
# (lane off) run: the blob is DERIVED state, never semantic. The bind reaches
# its container through the DISCOVERY chain, so for a sidecar or a library
# image this doubles as that carrier's arm smoke. (The system pack's lane: the
# dialect image carries no <iostream> for the test to bind.)
if [ "$DIALECT_IMAGE" = 0 ]; then
    v=$(MADC_MIR_CACHE_BIND=1 timeout 120 "$BIN" -v tests/testfreezerun.mad 2>&1 | tr -d '\0')
    grep -aq 'mir cache: bind module staged' <<<"$v"
    out_cache=$(MADC_MIR_CACHE_BIND=1 timeout 120 "$BIN" tests/testfreezerun.mad 2>&1)
    out_nocache=$(timeout 120 "$BIN" tests/testfreezerun.mad 2>&1)
    [ "$out_cache" = "$out_nocache" ]
fi

# Degradation gate (task #63) — the consumer half. A -v run of the PACKED
# product over a real C++ program (iostream/string/vector: the exact closure
# task #64 destroyed) so the bind MATERIALIZES the container; the gate then
# hard-zeroes the losses that have no legitimate instance. It refuses to render
# a verdict on a log with no materialization, so this leg cannot pass by
# binding nothing. Separate from the cache-equivalence run above on purpose:
# that one tests the opt-in cache lane, this one tests what the bind LOST.
#
# --dump/--units-from close a /dupaudit finding that came with the gate: the
# win64 and darwin packs each carried their own "every listed entry point must
# be a unit" loop, and THIS pack — the primary lane — had none, so a dropped
# unit here was invisible until the headerless lane. One owner, three callers.
# The dialect image is its own profile, its load log the dialect workload's
# (the fragments it binds) — bound through the carrier's discovery arm, the
# image being the carrier's only container at this stage — and its stats run
# proves the bind reads it in place (no zstd frame; the gate's image leg). Its
# TU roots nothing the compiler's dialect roots do not name (--image-roots).
# The system pack's dump lists the image below it (the gate's stack leg).
LOAD_LOG=${STAGE}_load.log
if [ "$DIALECT_IMAGE" = 1 ]; then
    STATS_LOG=${STAGE}_stats.log
    timeout 300 "$BIN" -v scripts/parse_cost/dialect.mad > "$LOAD_LOG" 2>&1
    timeout 300 "$BIN" --show-stats scripts/parse_cost/dialect.mad > "$STATS_LOG" 2>&1
    bash scripts/forest_pack_gate.sh --profile linux-dialect \
	--pack-log "$PACK_LOG" --load-log "$LOAD_LOG" \
	--dump "$DUMP" --units-from "$LIST" --image-stats "$STATS_LOG" \
	--image-roots "$LIST"
else
    timeout 300 "$BIN" -v tests/testfreezerun.mad > "$LOAD_LOG" 2>&1
    # The split (dialect per-name plan B2): with the system pack over the
    # image, a strict compile (C and C++) opens the system profile and never
    # the image; a dialect compile binds the image — under --no-forest-bind
    # too — and decodes no zstd frame. (The image-only carrier's stats run is
    # stage 1's.)
    printf '#include <stdio.h>\nint main(void) { printf("ok\\n"); return 0; }\n' > ${STAGE}_strict.c
    printf '#include <cstdio>\nint main() { std::printf("ok\\n"); return 0; }\n' > ${STAGE}_strict.cpp
    timeout 120 "$BIN" --show-stats --std=c17 ${STAGE}_strict.c > ${STAGE}_strict_c17.log 2>&1
    timeout 120 "$BIN" --show-stats --std=c++17 ${STAGE}_strict.cpp > ${STAGE}_strict_cxx17.log 2>&1
    timeout 300 "$BIN" --show-stats scripts/parse_cost/dialect.mad > ${STAGE}_dialect_stats.log 2>&1
    timeout 300 "$BIN" --show-stats --no-forest-bind scripts/parse_cost/dialect.mad > ${STAGE}_nobind_stats.log 2>&1
    SPLIT_STATS=(--strict-stats ${STAGE}_strict_c17.log --strict-stats ${STAGE}_strict_cxx17.log
        --image-stats ${STAGE}_dialect_stats.log --image-stats ${STAGE}_nobind_stats.log)
    bash scripts/forest_pack_gate.sh --profile linux \
	--pack-log "$PACK_LOG" --load-log "$LOAD_LOG" \
	--dump "$DUMP" \
	--units-from "$LIST" --units-from "$SOURCES_LIST" --sources-from "$GUARDED_LIST" \
	"${SPLIT_STATS[@]}"
fi

units=$(grep -c '^unit	' "$DUMP")
if [ "$SIDECAR" = 1 ]; then
    echo "forest_pack: OK ($units units in sidecar $OUT; bind cache == no-cache)"
else
    echo "forest_pack: OK ($units units appended to $OUT; bind cache == no-cache)"
fi
