#!/bin/bash
# check-neutral-tables.sh — a generated table (the dialect registry, a module
# table) is TARGET-NEUTRAL: a type spelled int64_t / uint64_t is written by its
# accessor (dd_platform_longlong() / dd_platform_ulonglong()) at every layer —
# a parameter, a reference, a function pointer's return and parameters — and
# so is its parameter SPELLING, which the mangler reads; never the generating
# madc's own `long` (&ddINT64) or a literal "long long". One table serves every
# target (src/madc_builtin_registry.cpp NeutralTypeIdentities): a glibc-written
# &ddINT64 compiled into a darwin madc mangles `l` where darwin's exports say
# `x`, and every int64_t-taking dialect or module function fails to resolve.
#
#   scripts/check-neutral-tables.sh [madc]     (default bin/madc)
#
# Negative control: a face spelling plain `long` must come back as &ddINT64
# (the check tells the two apart, and `long` keeps the shared identity).
set -u
cd "$(dirname "$0")/.." || exit 2
MADC=${1:-bin/madc}
[ -x "$MADC" ] || { echo "check-neutral-tables: no $MADC (build it first)" >&2; exit 1; }
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

# gen <face text> <out>: the module table of a one-namespace face
gen() {
	printf '%s\n' "$1" > "$T/face.madi"
	"$MADC" --std=madc --emit-module-table="$2" "$T/face.madi" > "$T/gen.log" 2>&1 || {
		cat "$T/gen.log" >&2
		echo "check-neutral-tables: FAIL — the generator refused the face" >&2
		exit 1
	}
}

gen 'namespace neutral {
    int64_t bare(int64_t a, uint64_t b);
    void ref(int64_t &a);
    void callback(int64_t (*f)(uint64_t));
}' "$T/neutral.inc"
host=$(grep -cE '&ddU?INT64\b' "$T/neutral.inc")
lit=$(grep -cE '"(unsigned )?long long[ &*]*"|"u?int64_t[ &*]*"' "$T/neutral.inc")
acc=$(grep -oE 'dd_platform_u?longlong\(\)' "$T/neutral.inc" | wc -l)
# types: bare's return + 2 parameters, ref's 1, callback's pointer return + 1;
# spellings: bare's 2, ref's 1, callback's parameter 1
if [ "$host" -ne 0 ] || [ "$lit" -ne 0 ] || [ "$acc" -lt 10 ]; then
	grep -nE 'ddU?INT64|dd_platform|long long|int64_t' "$T/neutral.inc" >&2
	echo "check-neutral-tables: FAIL — $host type(s) as the host's long, $lit literal platform spelling(s), $acc accessor(s) of 10" >&2
	exit 1
fi

gen 'namespace neutral {
    long plain(long a);
}' "$T/control.inc"
if ! grep -qE '&ddINT64\b' "$T/control.inc" || grep -qE 'dd_platform_longlong\(\)' "$T/control.inc"; then
	grep -nE 'ddINT64|dd_platform' "$T/control.inc" >&2
	echo "check-neutral-tables: NEGATIVE CONTROL FAILED — plain long was not written as &ddINT64" >&2
	exit 1
fi
echo "check-neutral-tables: OK (int64_t/uint64_t types and spellings by accessor at $acc sites; plain long stays &ddINT64)"
