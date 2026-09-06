#!/usr/bin/env bash
# check-object-mode-scope.sh — object-capture mode is entered ONLY through
# the scoped guard.
#
# Rule (DupFamily object_mode_emit_scoping, consolidated with IDE-10c):
# every native-emit lane enters madc_object_mode through ObjectModeScope
# (save in ctor, restore in dtor) — never a bare one-shot assignment. A
# leaked flag poisons an in-process caller's later JIT sessions
# (madc::build_native runs the emit inside a live host). The two CLI
# lanes (madc_cir_emit_native, madc_project_emit_native) were the
# original sites; a new emit lane is exactly where a third copy would be
# born.
#
# Marker: the ASSIGNMENT `madc_object_mode = true;` in src/*.cpp
# (semicolon-anchored so prose in comments never counts — the
# count-with-the-right-pattern rule). Exactly one is legal: the
# ObjectModeScope constructor.
set -u

SRCDIR="$(dirname "$0")/../src"

count_sets()
{
	grep -h "madc_object_mode = true;" "$@" | wc -l | tr -d ' '
}

n=$(count_sets "$SRCDIR"/*.cpp)
if [ "$n" -ne 1 ]; then
	echo "check-object-mode-scope: FAIL — $n 'madc_object_mode = true'" \
	     "sites in src/*.cpp (expected 1: the ObjectModeScope ctor)." \
	     "A new emit lane must declare an ObjectModeScope — see the" \
	     "DupFamily object_mode_emit_scoping." >&2
	exit 1
fi

# Negative control: the check must fail on a synthetic one-shot set.
tmp=$(mktemp --suffix=.cpp)
echo "	madc_object_mode = true;	// synthetic one-shot" > "$tmp"
if [ "$(count_sets "$SRCDIR"/*.cpp "$tmp")" -ne 2 ]; then
	rm -f "$tmp"
	echo "check-object-mode-scope: FAIL — negative control did not" \
	     "detect a synthetic one-shot set (the marker went blind)." >&2
	exit 1
fi
rm -f "$tmp"

echo "check-object-mode-scope: OK (one entry owner: ObjectModeScope)"
exit 0
