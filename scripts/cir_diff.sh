#!/bin/bash
# cir_diff.sh — structural diff of madc's CirBuilder tree vs c2m's node_t tree.
#
# Usage: scripts/cir_diff.sh <file.c>
#
# Runs `c2m -d <file>` (reference) and `bin/madc --std=c --dump-cir <file>`
# (our cir_node tree), normalizes both to <indent><NODE_TYPE> [payload] by
# stripping node-id prefixes, source positions, and checker type
# annotations, then diffs. Identical output means the trees match in shape.
#
# The reference c2m binary and our madc binary differ in two unavoidable
# ways the normalizer removes: (1) node uids come from different sequences,
# (2) c2m -d runs the checker so it prints type attrs; --dump-cir is
# pre-check. What remains is the structural skeleton + identifiers.

set -u
src="${1:?usage: cir_diff.sh <file.c>}"
C2M="${C2M:-/workspace/mir/c2m}"
MADC="${MADC:-bin/madc}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

normalize() {
    # 1. drop the "  NNN:" node-id prefix (keep the indentation that follows)
    # 2. delete the first (...) group (source position)
    # 3. delete a trailing ": <attrs>" tail (checker annotations / scope info)
    # 4. squeeze the double space left by step 2; drop blank lines
    sed -E 's/^[[:space:]]*[0-9]+:[[:space:]]?//' \
      | sed -E 's/\([^)]*\)//' \
      | sed -E 's/:[[:space:]].*$//' \
      | sed -E 's/[[:space:]]+$//' \
      | grep -v '^[[:space:]]*$'
}

# c2m -d writes the tree to stderr (timing lines too; the position filter
# below skips the <environment> preamble and timing noise).
"$C2M" -d "$src" 2>&1 \
  | sed -n "/$(basename "$src"):/,\$p" \
  | normalize > /tmp/cir_ref.txt

LD_LIBRARY_PATH="$ROOT/lib:/usr/local/lib" "$MADC" --std=c --dump-cir "$src" 2>&1 \
  | sed -n '/=== CIR TREE/,/=== END CIR TREE/p' \
  | grep -vE '=== (CIR TREE|END)' \
  | normalize > /tmp/cir_mine.txt

echo "=== diff: < c2m (reference)   > madc CirBuilder ==="
diff /tmp/cir_ref.txt /tmp/cir_mine.txt && echo "MATCH: trees are structurally identical"
