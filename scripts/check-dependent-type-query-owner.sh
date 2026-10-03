#!/bin/bash
# DRIFT-PREVENTION GATE -- concrete and dependent type-query measurement.
#
# query_datadef_measure owns C/C++ sizeof/alignof semantics for a DataDef,
# including reference-to-referent normalization. Parse-once tsubst must fold a
# newly concrete dependent query through that owner; rebuilding a partial C type
# from the substituted DataDef previously lost reference/class structure.
#
# The delegate check reads the tsubst fold itself (CirBuilder::copy_cir_subtree):
# other cir_builder callers of query_datadef_measure USE the owner, and a count
# of every caller in the file reported each new use as a second owner.
set -u
cd "$(dirname "$0")/.."

# fold_delegates <file> -> query_datadef_measure calls inside copy_cir_subtree
fold_delegates() {
	awk '/^cir_node \*CirBuilder::copy_cir_subtree\(/ { on = 1 }
	     on && /query_datadef_measure\(/ { n++ }
	     on && /^}/ { on = 0 }
	     END { print n + 0 }' "$1"
}

# Negative control: the same count over a copy whose fold measures by hand
# must be 0, or the check cannot fail.
tmp=$(mktemp)
sed 's/query_datadef_measure(/query_by_hand(/' src/cir_builder.cpp > "$tmp"
ctl=$(fold_delegates "$tmp")
rm -f "$tmp"
if [ "$ctl" -ne 0 ]; then
	echo "negative control: fold delegate count $ctl over a hand-measuring copy (target 0) -- the check is blind"
	exit 1
fi

defs=$(grep -c '^size_t query_datadef_measure(' src/parser.cpp)
echo "type-query measurement owner: $defs definition(s) (target 1)"
if [ "$defs" -ne 1 ]; then
	echo "  -> keep exactly one query_datadef_measure implementation."
	exit 1
fi

delegates=$(fold_delegates src/cir_builder.cpp)
echo "tsubst type-query fold delegates: $delegates (target 1)"
if [ "$delegates" -ne 1 ]; then
	echo "  -> copy_cir_subtree's dependent sizeof/alignof fold must measure through query_datadef_measure."
	exit 1
fi

legacy=$(grep -c 'query_type->size' src/cir_builder.cpp)
echo "direct deferred query size reads: $legacy (target 0)"
if [ "$legacy" -ne 0 ]; then
	grep -n 'query_type->size' src/cir_builder.cpp
	echo "  -> do not reimplement type-query semantics from DataDef storage size."
	exit 1
fi

echo "GREEN -- eager and dependent type queries share one measurement owner."
