#!/bin/bash
# DRIFT-PREVENTION GATE -- concrete and dependent type-query measurement.
#
# query_datadef_measure owns C/C++ sizeof/alignof semantics for a DataDef,
# including reference-to-referent normalization. Parse-once tsubst must fold a
# newly concrete dependent query through that owner; rebuilding a partial C type
# from the substituted DataDef previously lost reference/class structure.
set -u
cd "$(dirname "$0")/.."

defs=$(grep -c '^size_t query_datadef_measure(' src/parser.cpp)
echo "type-query measurement owner: $defs definition(s) (target 1)"
if [ "$defs" -ne 1 ]; then
	echo "  -> keep exactly one query_datadef_measure implementation."
	exit 1
fi

delegates=$(grep -c 'query_datadef_measure(' src/cir_builder.cpp)
echo "tsubst type-query delegates: $delegates (target 1)"
if [ "$delegates" -ne 1 ]; then
	grep -n 'query_datadef_measure(' src/cir_builder.cpp
	echo "  -> dependent type queries must delegate measurement to the shared owner."
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
