#!/bin/bash
# check-one-const-shift-normalizer.sh — a MIR target whose shift patterns take a
# constant count only in 1..width normalizes its constant shifts through
# mir-gen.c's ONE owner (gen_normalize_const_shifts, from target_split_insns),
# never its own copy.
#
# ppc64 and s390x each carried the same rewrite (count 0 -> a move, a negative
# count -> the other direction, a count past the width -> clamped). s390x ran
# its copy inside target_translate's emission loop, where the move it inserted
# ahead of the shift was never emitted. A copy is a target file that flips a
# shift's direction (`insn->code = MIR_LSHS` / `MIR_RSHS`) or clamps a count
# (`ops[2].u.i > 64` / `> 32`).
# Negative control: a synthetic target with the copy must FAIL; one that calls
# gen_normalize_const_shifts must PASS.
#
#   scripts/check-one-const-shift-normalizer.sh [DIR]   (DIR: the MIR sources to scan)
set -u
cd "$(dirname "$0")/.." || exit 2

copy='insn->code = MIR_(LSHS|RSHS)|ops\[2\]\.u\.i > (64|32)'

# Prints the hand-written normalizers in the target files under $1; 0 when none.
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
ctl=$(mktemp -d tmp/const-shift-ctl.XXXXXX) || exit 2
trap 'rm -rf "$ctl"' EXIT
mkdir -p "$ctl/bad" "$ctl/good"
printf 'static void target_split_insns (gen_ctx_t gen_ctx) {\n  case MIR_URSHS: insn->code = MIR_LSHS; break;\n  if (insn->ops[2].u.i > 64) insn->ops[2].u.i = 64;\n}\n' \
	> "$ctl/bad/mir-gen-x.c"
printf 'static void target_split_insns (gen_ctx_t gen_ctx) { gen_normalize_const_shifts (gen_ctx); }\n' \
	> "$ctl/good/mir-gen-x.c"
if scan "$ctl/bad" > /dev/null; then
	echo "check-one-const-shift-normalizer: CONTROL FAILED — a target's own shift rewrite went undetected" >&2
	exit 1
fi
if ! scan "$ctl/good" > /dev/null; then
	echo "check-one-const-shift-normalizer: CONTROL FAILED — gen_normalize_const_shifts was flagged" >&2
	exit 1
fi

out=$(scan "${1:-third_party/mir}")
if [ -n "$out" ]; then
	echo "check-one-const-shift-normalizer: a MIR target rewrites its constant shifts by hand —" >&2
	echo "call gen_normalize_const_shifts (mir-gen.c) from its target_split_insns:" >&2
	printf '%s\n' "$out" >&2
	exit 1
fi
echo "check-one-const-shift-normalizer: OK (one constant-shift normalizer for every MIR target; control both ways)"
