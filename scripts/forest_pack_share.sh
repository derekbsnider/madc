#!/bin/bash
# forest_pack_share.sh — sourced by the pack scripts (forest_pack.sh,
# forest_pack_windows.sh, forest_pack_darwin.sh): the ONE owner of the
# standards the pack's one profile serves besides madc (owner 2026-10-11).
#
# The pack freezes its corpus once, under --std=madc. A source's extension
# picks its standard (split arc 1A.4: .cpp is gnu++17), and a profile binds
# only under a config it carries — so every .cpp compile fell through to the
# pack's source text (~315 ms startup against ~166 ms bound). The madc dialect
# is C++17 plus extras (auto-includes, auto-namespacing, auto-main, its
# keywords and tokens) that the --std= gate keeps out of a C++ compile, over
# the same macro table, so the same profile serves the C++ modes: the freeze
# labels it (--freeze-also-std=). One profile, no size growth.
#
#   FOREST_SHARE_ARGS         the --freeze-also-std= flags the freeze takes
#   forest_share_check DUMP   the packed profile carries one label per
#                             standard (its --dump-forest `shared` lines)

FOREST_SHARED_STDS=(gnu++17 c++17)
FOREST_SHARE_ARGS=()
for _std in "${FOREST_SHARED_STDS[@]}"; do
    FOREST_SHARE_ARGS+=("--freeze-also-std=$_std")
done
unset _std

forest_share_check() {
    local dump=$1 got
    got=$(grep -c '^shared	' "$dump" || true)
    if [ "$got" -ne "${#FOREST_SHARED_STDS[@]}" ]; then
        echo "forest_pack_share: FAILED - the profile carries $got label(s)," \
             "expected ${#FOREST_SHARED_STDS[@]} (${FOREST_SHARED_STDS[*]})" >&2
        return 1
    fi
    echo "forest_pack_share: the madc profile also serves ${FOREST_SHARED_STDS[*]}"
}
