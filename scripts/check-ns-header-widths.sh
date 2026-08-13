#!/bin/bash
# GATE — script-facing intrinsic namespace headers use fixed-width ABI types.
#
# The extension-less include/madc/ns_* files are compiled as script header
# text.  Their declarations must agree with the host-side namespace exports
# and extern-C shims, which use int64_t for 64-bit integer values.  A bare
# `long` happens to agree on LP64, but becomes a 32-bit type with a different
# Itanium mangling on LLP64 (win64).
#
# Host-side *.h twins are deliberately excluded.  `long long` and
# `long double` remain valid spellings; only a bare `long` is forbidden.
set -u
cd "$(dirname "$0")/.."

hits=$(grep -nH -E '\blong\b' include/madc/ns_* 2>/dev/null \
	| grep -vE '^include/madc/ns_[^:]*\.h:' \
	| sed -E 's_(//|/\*).*__' \
	| sed -E 's/\blong[[:space:]]+(long|double)\b//g' \
	| grep -E '\blong\b')
n=$(printf '%s' "$hits" | grep -c .)

if [ "$n" -ne 0 ]; then
	echo "ns-header-widths gate: $n bare-long spelling(s) in script-facing namespace headers."
	echo "Use the host declaration's fixed-width type (normally int64_t): bare long"
	echo "is 32-bit on LLP64 and produces a different ABI/mangled symbol than the host."
	printf '%s\n' "$hits" | sed 's/^/  /'
	exit 1
fi

echo "ns-header-widths gate: GREEN — script namespace headers contain no bare long."
exit 0
