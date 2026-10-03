#!/bin/bash
# DRIFT-PREVENTION GATE -- non-class reference-formal argument lowering.
#
# ref_param_arg_addr_from_value is the single owner of the C ABI rule: a
# reference formal receives the referent lvalue's address, an address-backed
# reference forwards its stored address, and a prvalue uses one correctly typed
# temporary. Copied/tsubst and deferred-construction calls used to reproduce
# partial versions of this policy; the operator paths took raw addresses.
set -u
cd "$(dirname "$0")/.."

defs=$(grep -c '^node_t CirBuilder::ref_param_arg_addr_from_value(' src/cir_builder.cpp)
echo "reference-argument lowering owner: $defs definition(s) (target 1)"
if [ "$defs" -ne 1 ]; then
	echo "  -> keep exactly one ref_param_arg_addr_from_value implementation."
	exit 1
fi

copied=$(awk '
	/^node_t CirBuilder::copied_call_arg_for_formal\(/ { infn=1 }
	infn && /ref_param_arg_addr_from_value\(/ { found=1 }
	infn && /^}/ { print found + 0; exit }
' src/cir_builder.cpp)
echo "copied-call delegate: $copied (target 1)"
if [ "$copied" -ne 1 ]; then
	echo "  -> copied_call_arg_for_formal must delegate reference binding to the owner."
	exit 1
fi

deferred=$(awk '
	/^[[:space:]]*explicit_arg_node =$/ { inlambda=1 }
	inlambda && /ref_param_arg_addr_from_value\(/ { found=1 }
	inlambda && /^[[:space:]]*};$/ { print found + 0; exit }
' src/cir_builder.cpp)
echo "deferred-construction delegate: $deferred (target 1)"
if [ "$deferred" -ne 1 ]; then
	echo "  -> deferred construction must delegate reference binding to the owner."
	exit 1
fi

raw=$(grep -c 'node1(N_ADDR, translate_expr(top->right), top->right)' src/cir_builder.cpp)
echo "raw operator reference address paths: $raw (target 0)"
if [ "$raw" -ne 0 ]; then
	grep -n 'node1(N_ADDR, translate_expr(top->right), top->right)' src/cir_builder.cpp
	echo "  -> route operator reference arguments through ref_param_arg_addr."
	exit 1
fi

# The legacy copy took a copied value's address for a REFERENCE formal. The one
# named exception binds no reference: copied_class_value_arg (d0e860b73) passes
# a class prvalue of the parameter's own class as the by-value parameter object
# of an invisible-reference class ([class.temporary]) -- the class by-value
# ABI, object_arg_value's concern, not this owner's.
legacy_sites() {
	awk '
		/^node_t CirBuilder::copied_class_value_arg\(/ { skip = 1 }
		!skip && /return node1\(N_ADDR, value, arg\);/ { print FNR ": " $0 }
		skip && /^}/ { skip = 0 }
	' src/cir_builder.cpp
}
legacy=$(legacy_sites | grep -c . || true)
echo "legacy copied-value address implementations: $legacy (target 0)"
if [ "$legacy" -ne 0 ]; then
	legacy_sites
	echo "  -> route copied reference values through ref_param_arg_addr_from_value."
	exit 1
fi

echo "GREEN -- non-class reference arguments share one lowering owner."
