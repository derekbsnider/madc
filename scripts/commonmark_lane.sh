#!/bin/bash
# commonmark_lane.sh — the CommonMark conformance lane (plan
# docs/plans/2026-10-03-chthonia-windows-macos.md §7d). The madcmark module's
# Markdown TREE, rendered as HTML by scripts/commonmark_lane.mad the way
# cmark-gfm's own renderer writes it, against every example of the spec the
# pinned cmark-gfm ships (test/spec.txt in its staged source, read in place —
# the spec is CC-BY-SA 4.0 and is never copied into this repository).
#
# RATCHET shape (the c2mir-tests precedent): known-fails live in
# docs/parity/commonmark-baseline.txt (one example number per line, #
# comments allowed). The lane is RED when any example outside the baseline
# fails. A baseline example that PASSES is reported loudly, so the baseline
# only ever shrinks.
#
#   MADC_BIN              binary under test (default bin/madc)
#   MADC_COMMONMARK_SPEC  the spec (default: the staged cmark-gfm's
#                         test/spec.txt, from src/Makefile's CMARK_GFM_DIR
#                         and CMARK_GFM_TAG)
#   MADC_COMMONMARK_BASELINE  baseline file (default
#                         docs/parity/commonmark-baseline.txt)
set -u
cd "$(dirname "$0")/.."

BIN="${MADC_BIN:-bin/madc}"
BASE="${MADC_COMMONMARK_BASELINE:-docs/parity/commonmark-baseline.txt}"
if [ ! -x "$BIN" ]; then
	echo "commonmark_lane: $BIN missing — build first" >&2
	exit 1
fi
SPEC="${MADC_COMMONMARK_SPEC:-}"
if [ -z "$SPEC" ]; then
	dir=$(make -C src -s print-CMARK_GFM_DIR 2>/dev/null)
	tag=$(make -C src -s print-CMARK_GFM_TAG 2>/dev/null)
	SPEC="$dir/$tag/src/test/spec.txt"
fi
if [ ! -f "$SPEC" ]; then
	echo "commonmark_lane: no spec at $SPEC (stage cmark-gfm:" \
	     "scripts/stage_cmark_gfm.sh)" >&2
	exit 1
fi

declare -A baseline
if [ -f "$BASE" ]; then
	while IFS= read -r line; do
		case "$line" in ''|'#'*) continue;; esac
		baseline["${line%% *}"]=1
	done < "$BASE"
fi

mkdir -p tmp
out=tmp/commonmark_lane.out
timeout 600 "$BIN" scripts/commonmark_lane.mad "$SPEC" > "$out" 2>&1
rc=$?
summary=$(grep '^commonmark: ' "$out")
if [ "$rc" -ne 0 ] || [ -z "$summary" ]; then
	echo "commonmark_lane: the harness failed (rc=$rc)" >&2
	tail -20 "$out" >&2
	exit 1
fi

newfail=0; fixed=0
newfail_names=""; fixed_names=""
declare -A failed
while read -r tag num section; do
	[ "$tag" = FAIL ] || continue
	failed["$num"]=1
	if [ -z "${baseline[$num]:-}" ]; then
		newfail=$((newfail + 1))
		newfail_names="$newfail_names $num($section)"
	fi
done < "$out"
for num in "${!baseline[@]}"; do
	if [ -z "${failed[$num]:-}" ]; then
		fixed=$((fixed + 1))
		fixed_names="$fixed_names $num"
	fi
done

echo "$summary (${newfail} outside baseline, ${fixed} baseline examples now passing)"
if [ "$fixed" -gt 0 ]; then
	echo "commonmark_lane: LOUD — baseline examples now pass; shrink $BASE:$fixed_names"
fi
if [ "$newfail" -gt 0 ]; then
	echo "commonmark_lane: RED — examples outside the baseline fail:$newfail_names" >&2
	exit 1
fi
exit 0
