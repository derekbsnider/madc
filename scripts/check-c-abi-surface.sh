#!/bin/bash
# check-c-abi-surface.sh — the C-ABI export gate (packaging arc PK1;
# contract: docs/adr/0003-c-abi-stance.md).
#
# Classifies the COMPLETE extern-C (unmangled) dynamic export surface of
# lib/libmadc.so:
#   madc_*  declared in include/madc_api.h          -> THE C ABI
#   madc_*  listed in scripts/c-abi-internal-exports.txt -> internal, not ABI
#   __madc* / polyglot / madarray_* / MIR family / GDB-JIT / dd*+tk*
#           -> known internal classes (prefix rules below)
#   anything else: allowlisted with a disposition, or FAIL loud.
# Also enforced: every madc_api.h-declared function IS exported, and the
# SONAME matches the pinned value (moving it is a deliberate edit here,
# in the same commit as the promise documentation).
#
# Self-testing: the negative controls run on EVERY invocation through
# the same violations() classifier as the real surface — a gate that
# cannot fail is not a gate. Linux/ELF lane only; the PE/dylib surfaces
# ride their own lanes (ADR 0003 Consequences).
#
# Note: the decl extractor reads `madc_xxx(` tokens from madc_api.h, so
# a comment naming an API function with parens counts as a declaration
# mention — fine for API prose, don't cite hypothetical madc_* names
# with parens in that header's comments.
set -u
cd "$(dirname "$0")/.."

SO=${1:-lib/libmadc.so}
HDR=include/madc_api.h
ALLOW=scripts/c-abi-internal-exports.txt
EXPECTED_SONAME=libmadc.so.0
CLASS_RE='^(__madc|__js_|__perl_|__php_|__py_|__rb_|__rust_|madarray_|MIR_|_MIR_|c2mir|__mir_|mir\.|__jit_debug_descriptor$|__jit_debug_register_code$|dd[A-Z]|tk[A-Z])'

say() { echo "check-c-abi-surface: $*"; }

# violations <exports> <decls> <allow> — all three sorted files; prints
# one line per offense, nothing when the surface is clean. THE one
# classifier: the self-test and the real run both go through it.
violations() {
	local exports=$1 decls=$2 allow=$3 union
	union=$(mktemp)
	sort -u "$decls" "$allow" > "$union"
	# A. declared but not exported — the contract is broken at build time
	comm -23 "$decls" "$exports" \
		| sed 's/^/MISSING-EXPORT (declared in madc_api.h, absent from the library): /'
	# B. exported madc_* neither declared nor listed — contract drift
	grep '^madc_' "$exports" | comm -23 - "$union" \
		| sed 's/^/UNDECLARED-madc_-EXPORT (declare in madc_api.h deliberately, or list in c-abi-internal-exports.txt with a disposition): /'
	# C. outside every known class and not listed — unclassified export
	grep -v '^madc_' "$exports" | grep -Ev "$CLASS_RE" | comm -23 - "$allow" \
		| sed 's/^/UNCLASSIFIED-EXPORT (no class, no allowlist disposition): /'
	rm -f "$union"
}

selftest() {
	local d out
	d=$(mktemp -d)
	printf 'madc_program_create\n' > "$d/decls"
	printf '__php_trim\nmadc_program_create\nmadc_verbose\n' | sort > "$d/exports"
	printf 'madc_verbose\n' > "$d/allow"
	out=$(violations "$d/exports" "$d/decls" "$d/allow")
	if [ -n "$out" ]; then
		say "SELFTEST FAILED — a clean surface reported violations:"
		printf '%s\n' "$out"
		rm -rf "$d"; return 1
	fi
	printf '__php_trim\nbogus_plain_export\nmadc_bogus_undeclared\nmadc_program_create\n' \
		| sort > "$d/exports"
	out=$(violations "$d/exports" "$d/decls" "$d/allow")
	case $out in
	*bogus_plain_export*) : ;;
	*) say "SELFTEST FAILED — an unclassified export slipped through (the gate cannot fail)"
	   rm -rf "$d"; return 1;;
	esac
	case $out in
	*madc_bogus_undeclared*) : ;;
	*) say "SELFTEST FAILED — an undeclared madc_ export slipped through"
	   rm -rf "$d"; return 1;;
	esac
	printf 'madc_program_create\nmadc_program_destroy\n' | sort > "$d/decls"
	printf 'madc_program_create\n' > "$d/exports"
	out=$(violations "$d/exports" "$d/decls" "$d/allow")
	case $out in
	*MISSING-EXPORT*madc_program_destroy*) : ;;
	*) say "SELFTEST FAILED — a missing declared export slipped through"
	   rm -rf "$d"; return 1;;
	esac
	rm -rf "$d"
	say "selftest OK (bogus export bites, missing decl bites, clean surface passes)"
}

selftest || exit 1

[ -f "$SO" ] || { say "no $SO — fulltest builds it (\$(LIBMADC_SHARED))"; exit 1; }
[ -f "$HDR" ] || { say "no $HDR"; exit 1; }
[ -f "$ALLOW" ] || { say "no $ALLOW"; exit 1; }

D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT

nm -D --defined-only "$SO" \
	| awk '$2 ~ /^[A-Za-z]$/ { print $3 }' \
	| grep -v '^_Z' | sort -u > "$D/exports"
grep -oE '\bmadc_[a-z0-9_]+[[:space:]]*\(' "$HDR" \
	| sed 's/[[:space:](]*$//' | sort -u > "$D/decls"
grep -v '^[[:space:]]*#' "$ALLOW" | awk 'NF { print $1 }' | sort -u > "$D/allow"

[ -s "$D/exports" ] || { say "FAIL: no extern-C exports read from $SO (nm problem?)"; exit 1; }
[ -s "$D/decls" ] || { say "FAIL: no madc_ declarations read from $HDR"; exit 1; }

BAD=$(violations "$D/exports" "$D/decls" "$D/allow")
if [ -n "$BAD" ]; then
	say "FAIL — the extern-C surface drifted (docs/adr/0003-c-abi-stance.md):"
	printf '%s\n' "$BAD"
	exit 1
fi

SONAME=$(readelf -d "$SO" | awk '/SONAME/ { gsub(/[][]/, "", $NF); print $NF; exit }')
if [ "$SONAME" != "$EXPECTED_SONAME" ]; then
	say "FAIL — SONAME is '$SONAME', pinned '$EXPECTED_SONAME' (moving it is a deliberate edit here + ADR 0003)"
	exit 1
fi

TOTAL=$(wc -l < "$D/exports")
API=$(comm -12 "$D/decls" "$D/exports" | wc -l)
LISTED=$(comm -12 "$D/allow" "$D/exports" | wc -l)
say "OK — $TOTAL extern-C exports classified ($API ABI via madc_api.h, $LISTED listed internal, rest by class); SONAME $SONAME"
