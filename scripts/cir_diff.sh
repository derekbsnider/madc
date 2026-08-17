#!/bin/bash
# cir_diff.sh — structural diff of madc's CirBuilder tree vs c2m's node_t tree.
#
# Usage:
#   scripts/cir_diff.sh <file.c>             # PRE-check  (our --dump-cir   vs c2m -d)
#   scripts/cir_diff.sh --checked <file.c>   # POST-check (our --dump-cir-checked vs c2m -d)
#
# Normalizes both to <indent><NODE_TYPE> [payload] by stripping node-id
# prefixes, source positions, and checker type annotations, then diffs.
# Identical output means the trees match in shape.
#
# Stage matters: `c2m -d` dumps the POST-check tree (it runs do_context first),
# so forward-declared structs are FOLDED into the typedef there. Our --dump-cir
# is PRE-check (unfolded). Use --checked (our --dump-cir-checked, which also runs
# do_context) for an apples-to-apples comparison; the plain mode will differ on
# forward-decl folding by design.
#
# The normalizer also drops: c2m's timing lines, c2m's injected standard prelude
# (char16_t/char32_t/alloca/builtins — they carry no source position, so the
# source-position range filter skips them), and the MODULE/LIST wrappers (ours
# carry a position, c2m's don't).

set -u
CHECKED=0
if [ "${1:-}" = "--checked" ]; then CHECKED=1; shift; fi
src="${1:?usage: cir_diff.sh [--checked] <file.c>}"
C2M="${C2M:-obj/mir/host/c2m}"
MADC="${MADC:-bin/madc}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

normalize() {
    # 1. drop the "  NNN:" node-id prefix (keep the indentation that follows)
    # 2. delete the first (...) group (source position)
    # 3. delete a trailing ": <attrs>" tail (checker annotations / scope info)
    # 4. squeeze trailing space; drop blank lines, c2m timing, MODULE/LIST wrappers
    sed -E 's/^[[:space:]]*[0-9]+:[[:space:]]?//' \
      | sed -E 's/\([^)]*\)//' \
      | sed -E 's/:[[:space:]].*$//' \
      | sed -E 's/[[:space:]]+$//' \
      | grep -vE '^[[:space:]]*$' \
      | grep -vE 'C2MIR|binary output|usec|msec' \
      | grep -vxE 'MODULE| *LIST'
}

# c2m -d writes the tree to stderr. The source-position range skips the
# <environment> preamble AND c2m's position-less injected prelude.
"$C2M" -d "$src" 2>&1 \
  | sed -n "/$(basename "$src"):/,\$p" \
  | normalize > /tmp/cir_ref.txt

if [ "$CHECKED" = 1 ]; then
    LD_LIBRARY_PATH="$ROOT/lib:/usr/local/lib" "$MADC" --std=c --dump-cir-checked "$src" 2>&1 \
      | sed -n "/$(basename "$src"):/,\$p" \
      | grep -v '=== END CIR' \
      | normalize > /tmp/cir_mine.txt
    label="POST-check (--dump-cir-checked)"
else
    LD_LIBRARY_PATH="$ROOT/lib:/usr/local/lib" "$MADC" --std=c --dump-cir "$src" 2>&1 \
      | sed -n '/=== CIR TREE/,/=== END CIR TREE/p' \
      | grep -vE '=== (CIR TREE|END)' \
      | normalize > /tmp/cir_mine.txt
    label="PRE-check (--dump-cir)"
fi

echo "=== diff [$label]: < c2m (reference)   > madc CirBuilder ==="
diff /tmp/cir_ref.txt /tmp/cir_mine.txt && echo "MATCH: trees are structurally identical"
