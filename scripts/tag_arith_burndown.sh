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
# id-addressable derived-type API (Program::derived_type_id).
#
# STATUS: the encoding has been REMOVED (atomic core flip, 2026-06-30) — the
# enum dt*ptr/dt*ref ranges, the rt{Ptr,Ref,DePtr,DeRef} macros, and the offset
# math in the accessors are gone. Both baselines are 0, so this is now a
# FINISH-LINE CHECK: any new raw tag-arithmetic or dt*ptr/dt*ref/reftype()
# consumer is a regression. Keep it at 0/0 — derive types via the object graph
# and derived_type_id, never a +10000/20000 tag.
#
# CORE (datadef.h: the enum dt*ptr/dt*ref ranges, the rt{Ptr,Ref,DePtr,DeRef}
# macros, and the accessor bodies that implement them) is the ENCODING itself —
# removed LAST, so it is NOT counted here. Everything else that names a raw
# +10000/+20000 tag is.
set -u
cd "$(dirname "$0")/.."

BASELINE_FILE="docs/parity/tag-arith-baseline.txt"

# Metric 1 — RAW-TAG worklist = rt{Ptr,Ref,DePtr,DeRef} construction sites + raw
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

# Metric 2 — CONSUMER surface = sites that READ the offset indirectly and so
# break the moment the +10000/+20000 tag is dropped from the accessors, but are
# INVISIBLE to the rt*-only count above: dt*ptr/dt*ref NAMED-constant uses
# (type()==dtCHARptr, switch cases, signature literals) + reftype() band-reads.
# These migrate to representation-agnostic predicates (is_pointer()/is_reference()
# /rawtype()) ahead of the core flip — behavior-preserving for BOTH the structural
# DataDefPTR objects and the plain tagged DataDefs (the predicates already answer
# correctly for both today). Same exclusions; rawtype()-stripped values never
# equal a tag so they don't count.
chits=$( { grep -rnE 'dt[A-Za-z0-9]+(ptr|ref)\b' src/ include/ ;
           grep -rnE '\breftype[[:space:]]*\([[:space:]]*\)' src/ include/ ; } \
        | grep -vE '^include/datadef\.h:' \
        | grep -vE '^include/doctest\.h:' \
        | grep -vE '^include/json\.hpp:' \
        | grep -vE '^[^:]+:[0-9]+:[[:space:]]*//' \
        | grep -vE 'rawtype' \
        | sort -u )
cn=$( printf '%s' "$chits" | grep -c . )

echo "tag-arithmetic burndown: $n raw-tag site(s) + $cn consumer site(s) remain (target 0/0)"

if [ "${1:-}" = "--check" ]; then
  # Baseline file: the first two number-only lines are metric 1 then metric 2.
  mapfile -t nums < <( [ -f "$BASELINE_FILE" ] && grep -E '^[0-9]+$' "$BASELINE_FILE" )
  base=${nums[0]:-0}
  cbase=${nums[1]:-0}
  echo "baseline: raw-tag=$base consumer=$cbase"
  rc=0
  if [ "$n" -gt "$base" ]; then
    echo "RED — raw-tag site count rose above baseline ($n > $base)."
    echo "Migrate the new site onto structural queries / derived_type_id, or it is a regression."
    printf '%s\n' "$hits" | sed 's/^/  /' | head -40
    rc=1
  elif [ "$n" -lt "$base" ]; then
    echo "PROGRESS — raw-tag below baseline ($n < $base); lower line 1 of $BASELINE_FILE to $n in this commit."
  else
    echo "GREEN — raw-tag at baseline ($n)."
  fi
  if [ "$cn" -gt "$cbase" ]; then
    echo "RED — consumer site count rose above baseline ($cn > $cbase)."
    echo "Rewrite the new site with is_pointer()/is_reference()/rawtype(), not a dt*ptr/dt*ref constant or reftype() band-read."
    printf '%s\n' "$chits" | sed 's/^/  /' | head -40
    rc=1
  elif [ "$cn" -lt "$cbase" ]; then
    echo "PROGRESS — consumer below baseline ($cn < $cbase); lower line 2 of $BASELINE_FILE to $cn in this commit."
  else
    echo "GREEN — consumer at baseline ($cn)."
  fi
  exit $rc
fi

# Default: report both worklists.
echo "--- raw-tag sites ---"
printf '%s\n' "$hits" | sed 's/^/  /'
echo "--- consumer sites ---"
printf '%s\n' "$chits" | sed 's/^/  /'
