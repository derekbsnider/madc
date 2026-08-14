#!/bin/bash
# DRIFT-PREVENTION GATE -- one target-aware aggregate layout answer.
#
# DataDefSTRUCT owns MadC semantic layout.  Every DataDef-backed aggregate
# definition carries that settled answer in MC11-IR; c2mir consumes it for JIT
# execution, and the C renderer preserves the source pack state.  Contract-free
# source trees still use c2mir's native C layout algorithm.  This is the gate
# for dupaudit family aggregate_layout_engines (2026-08-14).
set -eu
cd "$(dirname "$0")/.."

builder=${MADC_AGGREGATE_GATE_BUILDER:-src/cir_builder.cpp}
consumer=${MADC_AGGREGATE_GATE_CONSUMER:-third_party/mir/c2mir/c2mir.c}
emitter=${MADC_AGGREGATE_GATE_EMITTER:-src/cir_emit_c.cpp}

count_lines()
{
	awk -v pattern="$1" '$0 ~ pattern { n++ } END { print n + 0 }' "$2"
}

check_sources()
{
	local b=$1
	local c=$2
	local e=$3
	local failed=0
	local owner_defs owner_refs owner_link aggregate_contract_defs
	local member_contract_defs consumer_defs consumer_refs
	local member_consumer_defs member_consumer_refs emitter_defs emitter_refs

	owner_defs=$(count_lines '^node_t CirBuilder::aggregate_def_node' "$b")
	owner_refs=$(count_lines 'aggregate_def_node[(]' "$b")
	owner_link=$(count_lines 'tag, members, aggregate_layout_contract[(]owner[)][)][;]' "$b")
	aggregate_contract_defs=$(count_lines '^node_t CirBuilder::aggregate_layout_contract' "$b")
	member_contract_defs=$(count_lines '^node_t CirBuilder::member_layout_contract' "$b")
	consumer_defs=$(count_lines '^static int settled_aggregate_layout [(]' "$c")
	consumer_refs=$(count_lines 'settled_aggregate_layout [(]' "$c")
	member_consumer_defs=$(count_lines '^static int settled_member_layout [(]' "$c")
	member_consumer_refs=$(count_lines 'settled_member_layout [(]' "$c")
	emitter_defs=$(count_lines '^int declaration_pack[(]' "$e")
	emitter_refs=$(count_lines 'declaration_pack[(]' "$e")

	echo "aggregate definition owner: $owner_defs definition, $owner_refs references"
	echo "owner-to-contract links: $owner_link (target 1)"
	echo "aggregate/member contract builders: $aggregate_contract_defs/$member_contract_defs (target 1/1)"
	echo "c2mir aggregate consumer: $consumer_defs definition, $consumer_refs references"
	echo "c2mir member consumer: $member_consumer_defs definition, $member_consumer_refs references"
	echo "emit-C pack reader: $emitter_defs definition, $emitter_refs references"

	if [ "$owner_defs" -ne 1 ] || [ "$owner_refs" -lt 7 ] || [ "$owner_link" -ne 1 ]; then
		echo "REGRESSION -- DataDef-backed definitions must use the settled-layout funnel."
		failed=1
	fi
	if [ "$aggregate_contract_defs" -ne 1 ] || [ "$member_contract_defs" -ne 1 ]; then
		echo "REGRESSION -- MC11 aggregate/member layout contracts must each have one builder."
		failed=1
	fi
	if [ "$consumer_defs" -ne 1 ] || [ "$consumer_refs" -ne 2 ]; then
		echo "REGRESSION -- c2mir must have one settled aggregate consumer and one layout call site."
		failed=1
	fi
	if [ "$member_consumer_defs" -ne 1 ] || [ "$member_consumer_refs" -ne 2 ]; then
		echo "REGRESSION -- c2mir must have one settled member consumer and one layout call site."
		failed=1
	fi
	if [ "$emitter_defs" -ne 1 ] || [ "$emitter_refs" -lt 5 ]; then
		echo "REGRESSION -- emitted C must recover pack state through one declaration reader."
		failed=1
	fi
	return "$failed"
}

if ! check_sources "$builder" "$consumer" "$emitter"; then
	exit 1
fi

tmpdir=$(mktemp -d tmp/aggregate-layout-gate.XXXXXX)
trap 'rm -rf "$tmpdir"' EXIT

# Negative control: sever the one definition-funnel-to-contract edge in a
# scratch copy.  The structural half of this gate must reject it before any
# runtime oracle is consulted.
mutated=$tmpdir/cir_builder.no_contract.cpp
sed 's/tag, members, aggregate_layout_contract(owner));/tag, members, list());/' \
	"$builder" > "$mutated"
if check_sources "$mutated" "$consumer" "$emitter" >/dev/null 2>&1; then
	echo "REGRESSION -- aggregate-layout static gate accepted its negative control."
	exit 1
fi

if [ "${MADC_AGGREGATE_GATE_STATIC_ONLY:-0}" = 1 ]; then
	echo "GREEN -- aggregate layout has one semantic owner and two contract consumers."
	exit 0
fi

madc=${MADC_BIN:-bin/madc}
cc=${CC:-gcc}
source_file=tests/testaggregatelayoutcontract.mad
expect_file=${MADC_AGGREGATE_EXPECT:-tests/testaggregatelayoutcontract.expect}
jit_out=$tmpdir/jit.out
emit_c=$tmpdir/emitted.c
emit_exe=$tmpdir/emitted
emit_out=$tmpdir/emitted.out

if ! "$madc" --no-config "$source_file" > "$jit_out" 2> "$tmpdir/jit.err"; then
	echo "REGRESSION -- aggregate-layout JIT specimen failed."
	sed -n '1,20p' "$tmpdir/jit.err"
	exit 1
fi
if ! cmp -s "$expect_file" "$jit_out"; then
	echo "REGRESSION -- JIT aggregate layout differs from the compiler oracle."
	diff -u "$expect_file" "$jit_out"
	exit 1
fi

if ! "$madc" --no-config --emit=c11 "$source_file" > "$emit_c" 2> "$tmpdir/emit.err"; then
	echo "REGRESSION -- aggregate-layout emit-C specimen failed."
	sed -n '1,20p' "$tmpdir/emit.err"
	exit 1
fi
if ! "$cc" -std=gnu11 -O0 "$emit_c" -o "$emit_exe" > "$tmpdir/cc.out" 2> "$tmpdir/cc.err"; then
	echo "REGRESSION -- emitted aggregate-layout C did not compile."
	sed -n '1,20p' "$tmpdir/cc.err"
	exit 1
fi
if ! "$emit_exe" > "$emit_out" 2> "$tmpdir/emitted.err"; then
	echo "REGRESSION -- emitted aggregate-layout executable failed."
	sed -n '1,20p' "$tmpdir/emitted.err"
	exit 1
fi
if ! cmp -s "$expect_file" "$emit_out"; then
	echo "REGRESSION -- emitted-C aggregate layout differs from the compiler oracle."
	diff -u "$expect_file" "$emit_out"
	exit 1
fi

echo "GREEN -- one settled aggregate layout reaches semantics, JIT, and emitted C."
