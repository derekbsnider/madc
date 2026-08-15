#!/bin/bash
# gen_win_rt_exports.sh — generate libmadc_rt.dll's .def export list from the
# ONE mingw-stdio map owner (third_party/mir/mir-mingw-stdio.h).
#
# The map's symbols are statically-linked libmingwex/oldnames code that PE
# ld auto-excludes from --export-all-symbols, yet AOT images must import
# them from libmadc_rt.dll (they exist in no system DLL, or only in the
# wrong long-double flavor). Generated at build time from the map table, so
# the two can never drift: name = the first quoted string of each entry,
# exported symbol = the entry's final identifier (plain-name rows become
# `name = __mingw_name` aliases, identity rows plain exports).
#
# Usage: gen_win_rt_exports.sh <mir-mingw-stdio.h> <out.def>
set -u
map=$1
out=$2
tmp=$out.tmp
{
	echo "EXPORTS"
	grep -o '{"[^"]*",[^}]*}' "$map" \
	| sed -e 's/^{"\([^"]*\)",.*[^A-Za-z0-9_]\([A-Za-z0-9_]*\)}$/\1 \2/' \
	| while read -r name sym; do
		if [ "$name" = "$sym" ]; then
			echo "$name"
		else
			echo "$name = $sym"
		fi
	done
} > "$tmp"
n=$(grep -c . "$tmp")
if [ "$n" -lt 10 ]; then
	echo "gen_win_rt_exports: only $n lines extracted from $map — parse broke" >&2
	rm -f "$tmp"
	exit 1
fi
mv "$tmp" "$out"
