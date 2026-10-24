#!/bin/bash
# check-one-pattern-index.sh — a MIR target indexes its insn patterns by code
# through mir-gen.c's ONE indexer (GEN_INDEX_PATTERNS, a counting sort), never
# its own copy.
#
# Five targets (x86-64, aarch64, ppc64, riscv64, s390x) each carried the same
# qsort-by-code indexer and its own insn_pattern_info type. A copy is a target
# file that defines `struct insn_pattern_info`, a `pattern_index_cmp`, or
# builds `pattern_indexes` itself (VARR_CREATE of it, a qsort over it).
# Negative control: a synthetic target with the qsort copy must FAIL; one that
# calls GEN_INDEX_PATTERNS must PASS.
#
#   scripts/check-one-pattern-index.sh [DIR]   (DIR: the MIR sources to scan)
set -u
cd "$(dirname "$0")/.." || exit 2

copy='struct insn_pattern_info \{|pattern_index_cmp|VARR_CREATE \(int, pattern_indexes|qsort \(VARR_ADDR \(int, pattern_indexes\)'

# Prints the hand-built indexers in the target files under $1; 0 when none.
scan() {
	local dir=$1 f hits found=0
	for f in "$dir"/mir-gen-*.c; do
		[ -f "$f" ] || continue
		hits=$(grep -n -E "$copy" "$f")
		if [ -n "$hits" ]; then
			found=1
			printf '%s\n' "$hits" | sed "s|^|  $f:|"
		fi
	done
	return $found
}

mkdir -p tmp
ctl=$(mktemp -d tmp/pattern-index-ctl.XXXXXX) || exit 2
trap 'rm -rf "$ctl"' EXIT
mkdir -p "$ctl/bad" "$ctl/good"
printf 'static void patterns_init (gen_ctx_t gen_ctx) {\n  VARR_CREATE (int, pattern_indexes, alloc, 0);\n  qsort (VARR_ADDR (int, pattern_indexes), n, sizeof (int), pattern_index_cmp);\n}\n' \
	> "$ctl/bad/mir-gen-x.c"
printf 'static void patterns_init (gen_ctx_t gen_ctx) {\n  GEN_INDEX_PATTERNS (gen_ctx, patterns, MIR_INSN_BOUND, &pattern_indexes, &insn_pattern_info);\n}\n' \
	> "$ctl/good/mir-gen-x.c"
if scan "$ctl/bad" > /dev/null; then
	echo "check-one-pattern-index: CONTROL FAILED — a target's own qsort indexer went undetected" >&2
	exit 1
fi
if ! scan "$ctl/good" > /dev/null; then
	echo "check-one-pattern-index: CONTROL FAILED — GEN_INDEX_PATTERNS was flagged" >&2
	exit 1
fi

out=$(scan "${1:-third_party/mir}")
if [ -n "$out" ]; then
	echo "check-one-pattern-index: a MIR target indexes its patterns by hand —" >&2
	echo "call GEN_INDEX_PATTERNS (mir-gen.c) from its patterns_init:" >&2
	printf '%s\n' "$out" >&2
	exit 1
fi
echo "check-one-pattern-index: OK (one pattern indexer for every MIR target; control both ways)"
