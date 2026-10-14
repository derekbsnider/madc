#!/bin/bash
# forest_pack_layout.sh — the pack step's LAYOUT pass (format 60; design doc
# docs/plans/2026-10-13-pack-slices-design.md §3). Sourced by every pack
# script (forest_pack.sh, forest_pack_windows.sh, forest_pack_darwin.sh):
#
#   forest_pack_layout <container-image> <header-list> <madc> [madc args...]
#
# Binds each root of <header-list> (the pack's canonical root order) alone
# against the container just written, recording the frozen-table reads the
# bind makes (--forest-touch-log), then lays the container out by those reads
# (--forest-relayout): string-pool records and arena records are ordered by
# which roots read them, so a compile decodes about the blocks its own
# includes need. The binds run --emit=c11 — they parse and lower but never
# execute — so they run wherever the freezer runs, a cross freezer included.
# <madc> [args...] is the freezer's own command (wine + exe, the cross madc
# plus its flags). Work files sit beside the container and are removed on
# success. Returns nonzero — failing the pack — when a root does not compile
# against the container, records no read, or the relayout fails.
forest_pack_layout() {
    local image="$1" list="$2"
    shift 2
    local dir="$image.layout"
    rm -rf "$dir"
    mkdir -p "$dir"
    local logs=() i=0 h n
    while IFS= read -r h; do
        n=$(printf '%03d' "$i")
        printf '#include <%s>\nint main() { return 0; }\n' "$h" > "$dir/r_$n.mad"
        if ! "$@" --no-config --std=madc --forest-bind="$image" \
                --forest-touch-log="$dir/r_$n.log" --emit=c11 "$dir/r_$n.mad" \
                > /dev/null 2> "$dir/r_$n.err"; then
            echo "forest_pack_layout: FAILED - <$h> does not compile against the container:" >&2
            head -5 "$dir/r_$n.err" >&2
            return 1
        fi
        logs+=("$dir/r_$n.log")
        i=$((i + 1))
    done < <(grep -vE '^[[:space:]]*(#|$)' "$list")
    if [ "$i" -eq 0 ]; then
        echo "forest_pack_layout: FAILED - $list names no root" >&2
        return 1
    fi
    if ! "$@" --no-config --forest-relayout="$image" "${logs[@]}"; then
        echo "forest_pack_layout: FAILED - the relayout of $image" >&2
        return 1
    fi
    rm -rf "$dir"
}
