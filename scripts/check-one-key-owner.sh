#!/bin/bash
# check-one-key-owner.sh — the key-owner gate.
#
# Every ui model (the grid model in madcdis/tui_model.h, the DOM model in
# madcdis/web_model.h) hands its keys to ONE chord resolver and spells them
# through ONE spelling owner: include/madcdis/keys.h (tui_key, tui_keyev,
# tui_key_name / tui_key_from_name, tui_bindings, key_resolver). Until
# 2026-09-07 the chord state machine lived inline in tui_model::apply_keys
# with a private `_pending` string and direct `_bindings.prefix(...)` /
# `_bindings.action_of(...)` lookups — the shape a second model would have to
# COPY to resolve chords, and a copy of a rule is where the two drift apart
# (the web-provider engine plan, Task 1). Keys resolve natively, in this one
# owner — never in a page's JavaScript and never in a second C++ model.
#
# Rule: a pending-chord member or a direct bindings-table lookup appears in
# no src/ or include/ file outside the owner header. A model consumes the
# resolver (key_resolver::step) and reads key_resolver::pending().
#
# Negative control: a synthetic violation must FAIL the scan, else the gate
# itself is broken and we fail loudly.

set -u
cd "$(dirname "$0")/.." || exit 2

OWNER_FILE=include/madcdis/keys.h
PATTERN='std::string +_pending\b|_bindings\.prefix *\(|_bindings\.action_of *\(|_bindings\.bound *\('

scan() {
	# $@ = files; prints violations, returns 0 when clean.
	grep -nE "$PATTERN" "$@" /dev/null
	test $? -ne 0
}

# --- negative controls -------------------------------------------------------
tmp=$(mktemp)
printf 'std::string _pending;\t// chord so far\n' > "$tmp"
if scan "$tmp" >/dev/null 2>&1; then
	rm -f "$tmp"
	echo "check-one-key-owner: NEGATIVE CONTROL FAILED — the scan did not catch a pending-chord member" >&2
	exit 2
fi
printf 'if ( _bindings.prefix(candidate) )\n' > "$tmp"
if scan "$tmp" >/dev/null 2>&1; then
	rm -f "$tmp"
	echo "check-one-key-owner: NEGATIVE CONTROL FAILED — the scan did not catch a direct bindings lookup" >&2
	exit 2
fi
rm -f "$tmp"

# --- the owner must exist and must hold the resolver ------------------------
if ! grep -q '^class key_resolver' "$OWNER_FILE"; then
	echo "check-one-key-owner: owner key_resolver not found in $OWNER_FILE" >&2
	exit 1
fi

# --- the tree ---------------------------------------------------------------
files=$(git ls-files 'src/*.cpp' 'src/*.h' 'include/*.h' 'include/**/*.h' | grep -v "^$OWNER_FILE\$")
# shellcheck disable=SC2086
out=$(grep -nE "$PATTERN" $files /dev/null)
if [ -n "$out" ]; then
	echo "check-one-key-owner: chord state or a direct bindings lookup outside the one owner ($OWNER_FILE):" >&2
	echo "$out" >&2
	echo "  -> hand the key to key_resolver::step and read key_resolver::pending()" >&2
	exit 1
fi
echo "check-one-key-owner: OK — chords resolve only in $OWNER_FILE (negative controls bite)"
