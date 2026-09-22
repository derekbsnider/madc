#!/bin/bash
# DRIFT-PREVENTION GATE -- "is this type void" has ONE owner, DataDef::is_void().
#
# DataDefPTR::rawtype() forwards to the pointee, and a DataDefPTR is constructed
# with its pointee's type(), so a bare `rawtype() == DataType::dtVOID` -- or
# `type()` -- is ALSO true for void*, void**, ... Sixteen sites asked the
# question by hand and nine forgot the pointer guard: void** arithmetic scaled by
# one byte, the overload ranker accepted int** -> void** and S* -> void** (a
# SILENT wrong overload, exit 0), a range-for refused a void** iterator, a
# multi-return refused a void* slot, std::format accepted void** while refusing
# int*. The ONE bare test allowed is the owner's own, in include/datadef.h.
#
# Two-sided: the negative control proves the pattern still matches a bare test,
# and the owner count (target 1) proves it still matches the real one.
set -u
cd "$(dirname "$0")/.."

PAT='(rawtype|type)\(\) *(==|!=) *DataType::dtVOID|DataType::dtVOID *(==|!=)'

ctl=$(printf '\tif (p->base_type->rawtype() == DataType::dtVOID)\n\tif (b->type() != DataType::dtVOID)\n' | grep -cE "$PAT")
if [ "$ctl" -ne 2 ]; then
	echo "check-one-void-predicate: NEGATIVE CONTROL FAILED -- the pattern no longer"
	echo "  matches a bare rawtype()/type() void test (matched $ctl of 2)"
	exit 1
fi

owner=$(grep -cE "$PAT" include/datadef.h)
echo "void-predicate owner (include/datadef.h): $owner bare test(s) (target 1: DataDef::is_void)"
if [ "$owner" -ne 1 ]; then
	grep -nE "$PAT" include/datadef.h
	echo "  -> the one bare dtVOID test is DataDef::is_void()'s; route the rest through it."
	exit 1
fi

bypass=$(grep -rnE "$PAT" src include --include='*.cpp' --include='*.h' --include='*.c' \
	| grep -v '^include/datadef.h:')
n=$(printf '%s' "$bypass" | grep -c . || true)
echo "bare void tests outside the owner: $n (target 0)"
if [ "$n" -ne 0 ]; then
	printf '%s\n' "$bypass"
	echo "  -> use DataDef::is_void(): a bare rawtype()/type() == dtVOID is also true for void*, void**."
	exit 1
fi
echo "GREEN -- \"is this type void\" has one owner."
