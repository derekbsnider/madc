#!/bin/bash
# DRIFT-PREVENTION GATE -- "which bytes hold this constant's value" has ONE
# owner, Variable::slot_kind() (include/datatokens.h).
#
# Variable's parse-time value slot had five accessors (set, cmp, dec, inc, get),
# each a ladder of `slot_type() == &ddX` identity tests, and the lists drifted:
# get() had no bool row (`constexpr bool B = false;` read true), none had
# wchar_t / char16_t / char32_t or the LLP64 platform long (a zero-length
# `int a[N]`), set() had no __int128 row. slot_kind() picks the slot once --
# the special rows, then every other integer scalar by its storage -- and every
# accessor switches on it. An identity test on slot_type() is a new ladder.
# The one exception is the `const char *` text compare (cmp(std::string&)),
# which is no scalar slot.
#
# Two-sided: the negative control proves the pattern still matches a ladder row.
set -u
cd "$(dirname "$0")/.."

PAT='slot_type\(\) *(==|!=) *&dd'

ctl=$(printf '\telse if (slot_type() == &ddINT32)  *((int32_t *)data) = c;\n\tif (slot_type() != &ddBOOL) return 0;\n' | grep -cE "$PAT")
if [ "$ctl" -ne 2 ]; then
	echo "check-one-slot-dispatch: NEGATIVE CONTROL FAILED -- the pattern no longer"
	echo "  matches a slot identity ladder row (matched $ctl of 2)"
	exit 1
fi

bypass=$(grep -rnE "$PAT" src include tests/unit --include='*.cpp' --include='*.h' \
	| grep -v '&ddCHARptr')
n=$(printf '%s' "$bypass" | grep -c . || true)
echo "slot identity tests outside Variable::slot_kind(): $n (target 0)"
if [ "$n" -ne 0 ]; then
	printf '%s\n' "$bypass"
	echo "  -> switch on Variable::slot_kind(): an identity list misses every type it does not name."
	exit 1
fi
echo "GREEN -- the value slot has one dispatch."
