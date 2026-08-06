#!/bin/bash
# DRIFT-PREVENTION GATE -- function-template call deduction.
#
# Scalar parameters and direct parameter packs once parsed their pointer and
# reference layers separately. The pack copy stripped A& and a dark arm then
# reconstructed it with a broad TokenVar test that also claimed prvalue calls.
# Two member-template dispatch keys independently erased value category too.
# Keep one structural parser, one deduction implementation, and one call-shape
# suffix for every pre-deduction key.
set -u
cd "$(dirname "$0")/.."

defs=$(awk '
	/^static int fn_template_deduce_param\(/ { signature=1; next }
	signature && /^[[:space:]]*\{$/ { ++definitions; signature=0; next }
	signature && /;[[:space:]]*$/ { signature=0 }
	END { print definitions + 0 }
' src/parser.cpp)
echo "function-template deduction owner: $defs definition(s) (target 1)"
if [ "$defs" -ne 1 ]; then
	echo "  -> keep exactly one fn_template_deduce_param implementation."
	exit 1
fi

shapes=$(grep -c '^static bool fn_template_param_shape(' src/parser.cpp)
echo "function-template parameter-shape owner: $shapes definition(s) (target 1)"
if [ "$shapes" -ne 1 ]; then
	echo "  -> scalar and direct-pack shapes must share fn_template_param_shape."
	exit 1
fi

legacy=$(grep -c 'fn_template_pack_arg_element\|MADC_FWDREF_ARM' src/parser.cpp)
echo "legacy pack deduction paths: $legacy (target 0)"
if [ "$legacy" -ne 0 ]; then
	grep -n 'fn_template_pack_arg_element\|MADC_FWDREF_ARM' src/parser.cpp
	echo "  -> direct packs must delegate to fn_template_deduce_param."
	exit 1
fi

expr_calls=$(grep -c '&pgm, tc->parameters' src/parser.cpp)
echo "expression-aware deduction delegates: $expr_calls (target 2)"
if [ "$expr_calls" -ne 2 ]; then
	echo "  -> scalar and direct-pack call deduction must pass the argument expression."
	exit 1
fi

shape_owners=$(grep -c '^static std::string fn_template_call_shape_suffix(' src/parser.cpp)
echo "member-template call-shape owner: $shape_owners definition(s) (target 1)"
if [ "$shape_owners" -ne 1 ]; then
	echo "  -> keep exactly one fn_template_call_shape_suffix implementation."
	exit 1
fi

shape_delegates=$(grep -c 'fn_template_call_shape_suffix(tc)' src/parser.cpp)
echo "member-template call-shape computations: $shape_delegates (target 1)"
if [ "$shape_delegates" -ne 1 ]; then
	echo "  -> compute the shared member-template call shape exactly once."
	exit 1
fi

shape_consumers=$(grep -c '+ call_shape' src/parser.cpp)
echo "member-template pre-deduction shape consumers: $shape_consumers (target 2)"
if [ "$shape_consumers" -ne 2 ]; then
	echo "  -> the recursion guard and instance memo must consume the shared call shape."
	exit 1
fi

echo "GREEN -- call deduction and pre-deduction identity have one owner each."
