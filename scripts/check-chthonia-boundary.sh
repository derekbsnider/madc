#!/bin/bash
# check-chthonia-boundary.sh — Chthonia stays on madcide's public surface.
#
# Chthonia moves to its own repository at its next release
# (docs/plans/2026-10-04-chthonia-own-repository.md); until the cut its
# files must depend on madcide only the way a separate product can: through
# the public headers madc ships (tools/madcide/include/madcide/, installed as
# share/madcide/include/madcide). A reach into madcide's internals here would
# not build in the other repository, and would be found only at the cut.
#
# Scope: the files that move — tools/chthonia/ and the bundle,
# tools/madcide/plugins/chthonia/. Its tests are not in scope yet: they
# include madcide's internals until madc-devel ships the product test
# harness (plan §3.2).
#
# Rule:
#   - an #include is a public header (<madcide/NAME>, NAME a file in
#     tools/madcide/include/madcide/) or a quoted file in the includer's own
#     directory;
#   - the build manifest (tools/chthonia/chthonia.json) names only paths
#     inside the moving set or the public include directory: madcide's base
#     is the library libmadcide ("libs": ["madcide"], plan §3a), never a
#     source the build compiles.
#
# Negative control: a synthetic file including a madcide internal, and a
# manifest naming one (the base's unit, madcide_base.mad, included), must
# FAIL; a public include, a sibling include and a manifest linking
# libmadcide must PASS — else the gate itself is broken and we fail loudly.

set -u
cd "$(dirname "$0")/.." || exit 2

PUBLIC_DIR=tools/madcide/include/madcide

# check_includes FILE... — prints each include outside the rule; 0 = clean.
check_includes() {
	local f line target bad=0
	for f in "$@"; do
		while IFS= read -r line; do
			case "$line" in
			*'#include <madcide/'*'>'*)
				target=${line#*<madcide/}
				target=${target%%>*}
				[ -f "$PUBLIC_DIR/$target" ] && continue
				;;
			*'#include "'*'"'*)
				target=${line#*\"}
				target=${target%%\"*}
				case "$target" in */*) ;; *) [ -f "$(dirname "$f")/$target" ] && continue ;; esac
				;;
			esac
			echo "$f: $line"
			bad=1
		done < <(grep -E '^[[:space:]]*#[[:space:]]*include' "$f")
	done
	return $bad
}

# check_manifest FILE — prints each path outside the rule; 0 = clean.
check_manifest() {
	local f="$1" p bad=0
	for p in $(grep -oE '"\.\./[^"]*"' "$f" | tr -d '"'); do
		case "$p" in
		../madcide/include|../madcide/plugins/chthonia/*) ;;
		*) echo "$f: $p"; bad=1 ;;
		esac
	done
	return $bad
}

# --- negative controls -------------------------------------------------------
tmpd=$(mktemp -d)
printf '#include "../madcide/madcide_core.inc"\n' > "$tmpd/bad.mad"
printf '#include <madcide/plugin>\n#include "sibling.h"\n' > "$tmpd/good.mad"
: > "$tmpd/sibling.h"
printf '{ "tus": [ "../madcide/madcide_core.inc" ] }\n' > "$tmpd/bad.json"
printf '{ "tus": [ "../madcide/madcide_base.mad" ] }\n' > "$tmpd/base.json"
printf '{ "libs": [ "madcide" ], "tus": [ { "include_dirs": [ "../madcide/include" ] } ] }\n' > "$tmpd/good.json"
fail_control() {
	rm -rf "$tmpd"
	echo "check-chthonia-boundary: CONTROL FAILED — $1" >&2
	exit 2
}
check_includes "$tmpd/bad.mad" >/dev/null && fail_control "an include of a madcide internal passed"
check_includes "$tmpd/good.mad" >/dev/null || fail_control "a public include or a sibling include failed"
check_manifest "$tmpd/bad.json" >/dev/null && fail_control "a manifest naming a madcide internal passed"
check_manifest "$tmpd/base.json" >/dev/null && fail_control "a manifest compiling madcide's base from source passed"
check_manifest "$tmpd/good.json" >/dev/null || fail_control "a manifest linking libmadcide with the public include dir failed"
rm -rf "$tmpd"

# --- the tree ---------------------------------------------------------------
src=$(git ls-files 'tools/chthonia/*.mad' 'tools/chthonia/*.h' 'tools/chthonia/*.inc' \
	'tools/madcide/plugins/chthonia/*.mad' 'tools/madcide/plugins/chthonia/*.inc' \
	'tools/madcide/plugins/chthonia/*.h')
status=0
# shellcheck disable=SC2086
if ! out=$(check_includes $src); then
	echo "check-chthonia-boundary: Chthonia includes a madcide internal:" >&2
	echo "$out" >&2
	status=1
fi
if ! out=$(check_manifest tools/chthonia/chthonia.json); then
	echo "check-chthonia-boundary: Chthonia's build manifest names a madcide internal:" >&2
	echo "$out" >&2
	status=1
fi
if [ $status -ne 0 ]; then
	echo "  -> Chthonia depends on madcide through $PUBLIC_DIR only (plan docs/plans/2026-10-04-chthonia-own-repository.md §2)" >&2
	exit 1
fi
echo "check-chthonia-boundary: OK (Chthonia uses madcide's public headers and links libmadcide)"
