#!/bin/bash
# check-one-module-member-owner.sh — the module-member materialization gate.
#
# The members of a namespace bound to a dynamic module (`import name as ns;`,
# `#load`) ARE the library's exports, materialized on first lookup: ONE owner,
# Program::resolve_module_member (src/parser.cpp), reached from
# find_namespace_member's miss path so every route into the namespace — a
# qualified `ns::m`, the statement-head `ns::m(...)` override, a postfix/cast
# head, `::ns::m`, a using-directive walk — sees the same registration
# (`__dl_<ns>_<m>`, stamped with its module identity, cached in the
# namespace map). On 2026-09-06 the rule had two copies at two qualified
# sites and NO copy on the other routes: a statement-position call drifted to
# the UNQUALIFIED dlsym fallback and registered a BARE symbol the JIT resolved
# by accident (RTLD_GLOBAL) and no native artifact could link; a cast operand
# reported "not a member" (KG Gap dynamic_module_member_lookup_paths_diverge,
# DupFamily dynamic_module_namespace_member_resolution). A second copy of
# either half — reading the module handle table, or minting the `__dl_`
# member symbol — is how that comes back.
#
# Rule: a READ of dlopen_map (find/count/at) or the `"__dl_" +` symbol mint
# appears nowhere in src/ or include/ outside the owner function. The binder
# (bind_module_namespace) WRITES the table; that is not matched.
#
# Negative control: a synthetic violation must FAIL the scan, else the gate
# itself is broken and we fail loudly.

set -u
cd "$(dirname "$0")/.." || exit 2

OWNER_FILE=src/parser.cpp
OWNER_FN='Program::resolve_module_member'
PATTERN='dlopen_map\.(find|count|at) *\(|"__dl_" *\+'

# Lines of OWNER_FILE inside the owner function's body: from its signature
# line to the first line that is a lone closing brace after it.
owner_range() {
	awk -v fn="$OWNER_FN" '
		$0 ~ fn && $0 ~ /^Variable \*/ { start = NR }
		start && NR > start && /^}/ { print start, NR; exit }
	' "$OWNER_FILE"
}

scan() {
	# $@ = files; prints violations, returns 0 when clean.
	grep -nE "$PATTERN" "$@" /dev/null
	test $? -ne 0
}

# --- negative control -------------------------------------------------------
tmp=$(mktemp)
printf 'void *h = dlopen_map.find(ns)->second;\n' > "$tmp"
if scan "$tmp" >/dev/null 2>&1; then
	rm -f "$tmp"
	echo "check-one-module-member-owner: NEGATIVE CONTROL FAILED — the scan did not catch a dlopen_map read" >&2
	exit 2
fi
printf 'std::string f = "__dl_" + ns + "_" + m;\n' > "$tmp"
if scan "$tmp" >/dev/null 2>&1; then
	rm -f "$tmp"
	echo "check-one-module-member-owner: NEGATIVE CONTROL FAILED — the scan did not catch a __dl_ member mint" >&2
	exit 2
fi
rm -f "$tmp"

# --- the owner must exist ---------------------------------------------------
range=$(owner_range)
if [ -z "$range" ]; then
	echo "check-one-module-member-owner: owner $OWNER_FN not found in $OWNER_FILE" >&2
	exit 1
fi
start=${range% *}
end=${range#* }

# --- the tree ---------------------------------------------------------------
files=$(git ls-files 'src/*.cpp' 'src/*.h' 'include/*.h' 'include/**/*.h' | grep -v "^$OWNER_FILE\$")
# shellcheck disable=SC2086
out=$(grep -nE "$PATTERN" $files /dev/null)
owner_out=$(grep -nE "$PATTERN" "$OWNER_FILE" | awk -F: -v s="$start" -v e="$end" '$1 < s || $1 > e' | sed "s|^|$OWNER_FILE:|")
if [ -n "$out" ] || [ -n "$owner_out" ]; then
	echo "check-one-module-member-owner: a module-member materialization outside the one owner ($OWNER_FILE:$OWNER_FN, lines $start-$end):" >&2
	[ -n "$out" ] && echo "$out" >&2
	[ -n "$owner_out" ] && echo "$owner_out" >&2
	echo "  -> look the member up through find_namespace_member (its miss path materializes module members)" >&2
	exit 1
fi
echo "check-one-module-member-owner: OK — module members are materialized only in $OWNER_FN (negative controls bite)"
