#!/bin/bash
# RATCHET GATE — one pointer-peel walker in the CIR emitter.
#
# The rule: "peel pointer levels off a DataDef (counting stars, landing on
# the base)" belongs to dd_peel_pointers (src/cir_builder.cpp, exported in
# cir_builder.h). NOTHING else in the emitter may hand-roll the
# while(is_pointer())/base_type walk.
#
# Why: the walk has a subtlety a copy always gets wrong eventually — a
# const-qualified level (`char * const *` = PTR(CONST(PTR(char)))) passes
# is_pointer() but fails dynamic_cast<DataDefPTR*>, so a cast-keyed copy
# breaks mid-chain and LOSES a star. Eight copies existed on 2026-08-29;
# the divergence emitted `char *flagarray` for SMAUG's `char * const
# *flagarray` (smaug_gate RED, c2mir pointer-assignment warnings). The
# owner steps via as_pointer_dd() — which DataDefCONST forwards — and
# unqualifies the final base.
#
# Marker: the CONCEPT — a while loop stepping on is_pointer() — not one
# spelling of the body. The owner itself is the single allowed match.
#
# Ratchet: the count must never rise above BASELINE (= the owner). Lower
# it never; migrate the copy instead.
set -u
cd "$(dirname "$0")/.."

BASELINE=1   # dd_peel_pointers itself

count=$(grep -c "while (.*is_pointer())" src/cir_builder.cpp)

if [ "$count" -gt "$BASELINE" ]; then
	echo "REGRESSION — $count is_pointer() peel loop(s) in cir_builder.cpp (baseline $BASELINE: dd_peel_pointers only)."
	echo "Migrate the new copy to dd_peel_pointers — do not add an exemption:"
	grep -n "while (.*is_pointer())" src/cir_builder.cpp
	exit 1
fi
if [ "$count" -lt "$BASELINE" ]; then
	echo "REGRESSION — expected the dd_peel_pointers owner loop; found $count matches."
	echo "If the owner was renamed/refactored, update this gate in the same commit."
	exit 1
fi

# Negative control: the marker must actually match the owner's loop shape.
if ! grep -q "while (dd && dd->is_pointer())" src/cir_builder.cpp; then
	echo "REGRESSION — negative control failed: the marker no longer matches dd_peel_pointers' own loop."
	exit 1
fi

echo "one-pointer-peel gate: GREEN — dd_peel_pointers is the emitter's only pointer-peel walker."
exit 0
