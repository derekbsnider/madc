#!/bin/bash
# Stage the exact mingw-w64 headers that a packed, compiler-less Windows madc
# may have to tokenize live. The forest remains the primary header provider;
# this target-only embedded root covers individually declined conditional
# husks whose missing branch cannot be reconstructed from frozen state.
#
# Usage: gen_win_header_fallbacks.sh <mingw-include-root> <output-dir>
set -u

if [ $# -ne 2 ]; then
	echo "usage: $0 <mingw-include-root> <output-dir>" >&2
	exit 2
fi

source_root="$1"
output_dir="$2"

if [ ! -d "$source_root" ]; then
	echo "gen_win_header_fallbacks: mingw include root missing: $source_root" >&2
	exit 1
fi

mkdir -p "$output_dir"

# Keep this set minimal. Each entry is source text owned by mingw-w64 and is
# used only when the forest cannot reproduce the requested live/positional
# include. errno.h is task #57's missing-content husk; stdlib.h is the
# #include_next provider at the compiler-resource slot. Both are public-domain
# mingw-w64 text; their normal dependencies remain forest-served.
headers=(errno.h stdlib.h)
for header in "${headers[@]}"; do
	source_file="$source_root/$header"
	output_file="$output_dir/$header"
	tmp_file="$output_file.tmp.$$"
	if [ ! -f "$source_file" ]; then
		echo "gen_win_header_fallbacks: required header missing: $source_file" >&2
		exit 1
	fi
	cp "$source_file" "$tmp_file"
	if [ ! -f "$output_file" ] || ! cmp -s "$tmp_file" "$output_file"; then
		mv "$tmp_file" "$output_file"
	else
		rm -f "$tmp_file"
	fi
done

echo "win header fallbacks: $output_dir (${#headers[@]} exact mingw header(s))"
