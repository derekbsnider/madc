#!/bin/bash
# BURNDOWN GATE — tag-arithmetic retirement campaign.
# Design: docs/plans/2026-06-30-tag-arithmetic-retirement-plan.md
# Origin: docs/plans/2026-06-12-type-table-value-abi-design.md §1, §6.4.
#
# madc historically encodes pointer/reference DERIVATION as numeric ranges on
# the DataType tag: base at X, pointer at X+10000 (rtPtr), reference at X+20000
# (rtRef); rawtype()/reftype()/setRef() strip/add the offset. This "bit trick"
# cannot nest (no int**), collides across the fixed 255-slot bands, and is a
# PARALLEL type-identity scheme competing with the DataDef object graph
# (DataDefPTR/REF/CONST) + the typeid table. This gate counts the EXTERNAL
# consumers that still do raw tag arithmetic — the worklist to migrate onto
# structural queries (is_pointer/is_reference/base_type/getPointerType) and the
# id-addressable derived-type API (Program::derived_type_id). When this reaches
# 0, a final commit removes the core encoding from datadef.h (the enum ranges +
# rtPtr/rtRef macros + the offset math in the accessors) and this gate flips to
# a finish-line check.
#
# CORE (datadef.h: the enum dt*ptr/dt*ref ranges, the rt{Ptr,Ref,DePtr,DeRef}
# macros, and the accessor bodies that implement them) is the ENCODING itself —
# removed LAST, so it is NOT counted here. Everything else that names a raw
# +10000/+20000 tag is.
set -u
cd "$(dirname "$0")/.."

BASELINE_FILE="docs/parity/tag-arith-baseline.txt"

# External worklist = rt{Ptr,Ref,DePtr,DeRef} construction sites + raw
# literal-tag (10000/20000) comparisons, anywhere EXCEPT the core (datadef.h),
# the macro definitions, and vendored third-party headers.
hits=$( { grep -rnE 'rtPtr\(|rtRef\(|rtDePtr\(|rtDeRef\(' src/ include/ \
            | grep -vE '#[[:space:]]*define[[:space:]]+rt(Ptr|Ref|DePtr|DeRef)' ;
          grep -rnE '\b[12]0000\b' src/ include/ ; } \
        | grep -vE '^include/datadef\.h:' \
        | grep -vE '^include/doctest\.h:' \
        | grep -vE '^include/json\.hpp:' \
        | grep -vE '^[^:]+:[0-9]+:[[:space:]]*//' \
        | sort -u )
n=$( printf '%s' "$hits" | grep -c . )

echo "tag-arithmetic burndown: $n external raw-tag site(s) remain (target 0)"

if [ "${1:-}" = "--check" ]; then
  base=$( [ -f "$BASELINE_FILE" ] && head -n1 "$BASELINE_FILE" | tr -dc '0-9' || echo 0 )
  echo "baseline: $base"
  if [ "$n" -gt "$base" ]; then
    echo "RED — tag-arithmetic site count rose above baseline ($n > $base)."
    echo "Migrate the new site onto structural queries / derived_type_id, or it is a regression."
    printf '%s\n' "$hits" | sed 's/^/  /' | head -40
    exit 1
  fi
  if [ "$n" -lt "$base" ]; then
    echo "PROGRESS — count below baseline ($n < $base); lower $BASELINE_FILE to $n in this commit."
  else
    echo "GREEN — at baseline ($n)."
  fi
  exit 0
fi

# Default: report the sites (the working worklist).
printf '%s\n' "$hits" | sed 's/^/  /'
