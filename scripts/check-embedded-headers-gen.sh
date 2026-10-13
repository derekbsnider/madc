#!/bin/bash
# check-embedded-headers-gen.sh — the embedded-header generator never writes
# an empty table.
#
# scripts/gen_embedded_headers.sh bakes include/madc/* into the table the
# build compiles. A run that reads no header must FAIL and leave no table, so
# the build stops (the Makefile removes the table on a failed generation)
# instead of linking a madc with no embedded headers. The generator is run
# from a copy beside a scratch include/madc/, empty for the negative control
# and holding one header for the positive control.
#
# See .claude/rules/embedded-headers.md and KG Gap
# gen_embedded_headers_silent_empty_table.

set -u
cd "$(dirname "$0")/.." || exit 2

work="tmp/check-embedded-headers-gen.$$"
rm -rf "$work"
mkdir -p "$work/scripts" "$work/include/madc" || exit 2
trap 'rm -rf "$work"' EXIT
cp scripts/gen_embedded_headers.sh "$work/scripts/" || exit 2
gen="$work/scripts/gen_embedded_headers.sh"
out="$work/out.cpp"
fail=0

# Negative control: no header to read must be refused, with no table left.
if bash "$gen" "" "$out" > /dev/null 2>&1; then
	echo "check-embedded-headers-gen: FAIL — an empty header root produced a table" >&2
	fail=1
elif [ -e "$out" ]; then
	echo "check-embedded-headers-gen: FAIL — a refused generation left $out" >&2
	fail=1
fi

# Positive control: one header gives a table carrying it.
printf '#define MADC_GEN_MARK 1\n' > "$work/include/madc/mark.h"
if ! bash "$gen" "" "$out" > /dev/null 2>&1; then
	echo "check-embedded-headers-gen: FAIL — the generator refused a root holding mark.h" >&2
	fail=1
elif ! grep -q '{"mark.h", R"EMBED(#define MADC_GEN_MARK 1' "$out"; then
	echo "check-embedded-headers-gen: FAIL — the table does not carry mark.h" >&2
	fail=1
fi

# Neither run leaves its scratch beside the table.
left=$(find "$work" -name 'out.cpp.*' | wc -l)
if [ "$left" -ne 0 ]; then
	echo "check-embedded-headers-gen: FAIL — $left generator scratch file(s) left" >&2
	fail=1
fi

[ "$fail" -eq 0 ] || exit 1
echo "check-embedded-headers-gen: OK"
