#!/bin/bash
# forest_pack_share.sh — sourced by the pack scripts (forest_pack.sh,
# forest_pack_windows.sh, forest_pack_darwin.sh): the ONE owner of the
# standards each of the pack's profiles serves besides its own, and of the
# capped-run command their steps share.
#
# THE MADC PROFILE (owner 2026-10-11). The pack freezes its corpus once, under
# --std=madc. A source's extension picks its standard (split arc 1A.4: .cpp is
# gnu++17), and a profile binds only under a config it carries — so every .cpp
# compile fell through to the pack's source text (~315 ms startup against
# ~166 ms bound). The madc dialect is C++17 plus extras (auto-includes,
# auto-namespacing, auto-main, its keywords and tokens) that the --std= gate
# keeps out of a C++ compile, over the same macro table, so the same profile
# serves the C++ modes: the freeze labels it (--freeze-also-std=). One
# profile, no size growth.
#
#   FOREST_SHARE_ARGS         the --freeze-also-std= flags the freeze takes
#   forest_share_check DUMP   the packed profile carries one label per
#                             standard (its --dump-forest `shared` lines)
#
# THE C LAYER (docs/plans/2026-10-18-c-pack-layer.md; owner 2026-10-17: "C is
# separate, uncompressed, and not on the same list"): the C roots frozen again
# under C, as the C preprocessor sees them, in raw profiles stacked BELOW the
# madc profile. One row per profile, "<std> [label...]", frozen in row order —
# the last row sits directly under the madc profile. OWNER 2026-10-18: the
# pack precompiles ONLY the defaults — gnu17 here, gnu++17 above (the madc
# profile) — "we can't precompile multiple variants as that will take up too
# much space"; every other standard parses the pack's text. A label costs no
# bytes, so a standard MEASURED identical to a row's rides it.
#
#   FOREST_C_LAYER                  the rows
#   forest_c_share_args STD         the --freeze-also-std= flags of STD's row
#   forest_share_check_c DUMP STD   STD's profile carries its row's labels
#   FOREST_PACK_TIMEOUT             the capped-run command (below)
#
# A C label is MEASURED, never assumed: a standard is a label only when its
# freeze of the C roots has the same `content` digest as the row's own
# (__STRICT_ANSI__ hides glibc's POSIX names under c17; __STDC_VERSION__ gates
# C11 names under c99). A wrong label binds silently — the bind's environment
# check does not catch it. The 2026-10-18 measurement (the plan, J2): gnu11 ==
# gnu17 and c11 == c17; c17 != gnu17 (a strict profile was dropped by the
# owner ruling above); gnu99, gnu89, gnu23, c99, c89 and c23 each differ.

FOREST_SHARED_STDS=(gnu++17 c++17)
FOREST_SHARE_ARGS=()
for _std in "${FOREST_SHARED_STDS[@]}"; do
    FOREST_SHARE_ARGS+=("--freeze-also-std=$_std")
done
unset _std

FOREST_C_LAYER=("gnu17 gnu11")

# THE CAPPED RUN: every pack step runs under GNU timeout — `timeout` on the
# build container, brew coreutils' `timeout` (gnubin) or `gtimeout` on a Mac
# build host (the darwin self-freeze). Empty when the host has neither; a
# pack script refuses to run without it.
FOREST_PACK_TIMEOUT="$(command -v timeout || command -v gtimeout || true)"

# forest_c_row_labels STD — the labels of STD's row, one per line.
forest_c_row_labels() {
    local row std
    for row in "${FOREST_C_LAYER[@]}"; do
        read -r std _ <<<"$row"
        if [ "$std" = "$1" ]; then
            printf '%s\n' $row | tail -n +2
            return 0
        fi
    done
    echo "forest_pack_share: no C layer row for '$1'" >&2
    return 1
}

forest_c_share_args() {
    local labels l
    labels=$(forest_c_row_labels "$1") || return 1
    for l in $labels; do
        printf -- '--freeze-also-std=%s\n' "$l"
    done
}

# forest_share_labels DUMP NAME LABEL... — the dumped profile carries exactly
# one `shared` line per LABEL.
forest_share_labels() {
    local dump=$1 name=$2 got
    shift 2
    got=$(grep -c '^shared	' "$dump" || true)
    if [ "$got" -ne "$#" ]; then
        echo "forest_pack_share: FAILED - $name carries $got label(s)," \
             "expected $# ($*)" >&2
        return 1
    fi
    echo "forest_pack_share: $name also serves $*"
}

forest_share_check() {
    forest_share_labels "$1" "the madc profile" "${FOREST_SHARED_STDS[@]}"
}

forest_share_check_c() {
    local labels
    labels=$(forest_c_row_labels "$2") || return 1
    # shellcheck disable=SC2086
    forest_share_labels "$1" "the C layer's $2 profile" $labels
}
