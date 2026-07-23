#!/bin/bash
# forest_index_oracle.sh — decl-index parity gate (B4a; design doc
# 2026-07-04-forest-default-mode-design.md §8 risk 2, §9).
#
# Contract: every name live parse REGISTERS for the packed closure must
# appear in the container's decl index, so a B4b bind-time miss can always
# materialize it. Mechanically:
#   side A = declindex names from --dump-forest of a freshly frozen pack TU
#   side B = --dump-registered on the pack TU, minus the empty-TU baseline
#            (builtins/predefines), minus madc's SYNTHETIC EMIT SCHEMES
#            (Class__Class ctor doubling, ___dtor, __oN overload clones,
#            __operator mangles, __i<spelling>_<fnv1a32> instantiation
#            products incl. their local-class scions — see
#            overload_spelling_symbol_suffix() in src/parser.cpp — derived
#            entities, never source lookups: bind-time resolves the PATTERN
#            from the index and MINTS the product symbol, same as live)
#   gate   = (B - A) - scripts/forest_index_allowlist.txt  must be EMPTY
set -e
cd "$(dirname "$0")/.."

BIN="${1:-bin/madc}"
LIST=scripts/forest_pack_headers.txt
mkdir -p tmp
TU=tmp/forest_oracle_tu.cpp

{
    grep -vE '^[[:space:]]*(#|$)' "$LIST" | while read -r h; do
        echo "#include <$h>"
    done
    echo "int main() { return 0; }"
} > "$TU"
echo 'int main() { return 0; }' > tmp/forest_oracle_empty.cpp

ulimit -t 900
timeout 600 "$BIN" --freeze=tmp/forest_oracle.msnap "$TU" > /dev/null 2>&1
timeout 120 "$BIN" --dump-forest=tmp/forest_oracle.msnap 2>/dev/null \
    | awk -F'\t' '$1=="declindex"{print $4}' | sort -u > tmp/forest_oracle_a.txt
timeout 600 "$BIN" --dump-registered "$TU" 2>/dev/null | sort -u \
    > tmp/forest_oracle_full.txt
timeout 120 "$BIN" --dump-registered tmp/forest_oracle_empty.cpp 2>/dev/null \
    | sort -u > tmp/forest_oracle_base.txt

comm -23 tmp/forest_oracle_full.txt tmp/forest_oracle_base.txt \
    | awk -F'\t' '{print $2}' | sort -u > tmp/forest_oracle_b.txt

# Synthetic-scheme filter: drop madc's own emitted-name shapes (see header).
comm -23 tmp/forest_oracle_b.txt tmp/forest_oracle_a.txt \
    | grep -vE '___dtor$|__o[0-9]+$|__operator|__i[[:alnum:]_]{0,61}_[0-9a-f]{8}(___|$)' \
    | awk '{n=$0; sub(/^.*::/,"",n); L=length(n);
            for (i=3;i<L-1;i++)
                if (substr(n,i,2)=="__") {
                    a=substr(n,1,i-1); b=substr(n,i+2);
                    if (a==b) next;
                }
            print}' > tmp/forest_oracle_missing.txt

grep -vE '^[[:space:]]*(#|$)' scripts/forest_index_allowlist.txt | sort -u \
    > tmp/forest_oracle_allow.txt
comm -23 <(sort -u tmp/forest_oracle_missing.txt) tmp/forest_oracle_allow.txt \
    > tmp/forest_oracle_fail.txt

if [ -s tmp/forest_oracle_fail.txt ]; then
    echo "forest_index_oracle: FAIL — registered names missing from the decl index:"
    cat tmp/forest_oracle_fail.txt
    exit 1
fi
echo "forest_index_oracle: OK ($(wc -l < tmp/forest_oracle_a.txt) indexed names cover $(wc -l < tmp/forest_oracle_b.txt) registered lookups; $(wc -l < tmp/forest_oracle_allow.txt) allowlisted)"
