#!/bin/bash
# check-one-key-owner.sh — the ui INPUT owners gate: one key owner, one
# focus owner.
#
# Every ui model (the grid model in madcdis/tui_model.h, the DOM model in
# madcdis/web_model.h) hands its keys to ONE chord resolver and spells them
# through ONE spelling owner — include/madcdis/keys.h (tui_key, tui_keyev,
# tui_key_name / tui_key_from_name, tui_bindings, key_resolver) — and keeps
# its focus slot, per-choice selection and tab/arrow/enter rules in ONE
# focus owner — include/madcdis/ui_focus.h (focusable, focus_state). Until
# 2026-09-07 both lived inline in tui_model: a private `_pending` string with
# direct `_bindings.prefix(...)` / `_bindings.action_of(...)` lookups, and a
# `_focusables` vector indexed beside a `_selection` map — the shapes a
# second model would have to COPY, and a copy of a rule is where the two
# drift apart (the web-provider engine plan, Tasks 1 and 2). Keys resolve
# and focus moves natively, in these owners — never in a page's JavaScript
# and never in a second C++ model.
#
# Rule: a pending-chord member or a direct bindings-table lookup appears in
# no src/ or include/ file outside keys.h (a model reads
# key_resolver::pending()); a focusable-list index or a selection-map
# subscript appears in none outside ui_focus.h (a model reads focus_state's
# accessors); and the keys → events LOOP — the step()/navigate() calls with
# the §7.5 printable coalescing between them — appears in none outside
# ui_input.h (a model calls ui_apply_keys over its two owners; the DOM
# model and the grid model run the one loop).
#
# Negative controls: a synthetic violation of each rule must FAIL the scan,
# else the gate itself is broken and we fail loudly.

set -u
cd "$(dirname "$0")/.." || exit 2

KEY_OWNER=include/madcdis/keys.h
KEY_PATTERN='std::string +_pending\b|_bindings\.prefix *\(|_bindings\.action_of *\(|_bindings\.bound *\('
FOCUS_OWNER=include/madcdis/ui_focus.h
FOCUS_PATTERN='_focusables *\[|_selection *\[|std::vector<focusable> +_focusables\b'
# The keys → events LOOP (chord step, printable coalescing, navigate) runs
# once, in madcdis/ui_input.h ui_apply_keys; a model calls the adapter,
# never the owners' step()/navigate() directly.
INPUT_OWNER=include/madcdis/ui_input.h
INPUT_PATTERN='key_step +[A-Za-z_]+ *= *[A-Za-z_.>-]+\.step *\(|\.navigate *\('
PATTERN="$KEY_PATTERN|$FOCUS_PATTERN|$INPUT_PATTERN"

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
printf 'size_t n = _focusables[_focus].option_count;\n' > "$tmp"
if scan "$tmp" >/dev/null 2>&1; then
	rm -f "$tmp"
	echo "check-one-key-owner: NEGATIVE CONTROL FAILED — the scan did not catch a focusable-list index" >&2
	exit 2
fi
printf '_selection[_focus] = sel;\n' > "$tmp"
if scan "$tmp" >/dev/null 2>&1; then
	rm -f "$tmp"
	echo "check-one-key-owner: NEGATIVE CONTROL FAILED — the scan did not catch a selection-map subscript" >&2
	exit 2
fi
printf 'key_step step = _keys.step(k);\n' > "$tmp"
if scan "$tmp" >/dev/null 2>&1; then
	rm -f "$tmp"
	echo "check-one-key-owner: NEGATIVE CONTROL FAILED — the scan did not catch a second keys → events loop" >&2
	exit 2
fi
rm -f "$tmp"

# --- the owners must exist and must hold their state machines ---------------
if ! grep -q '^class key_resolver' "$KEY_OWNER"; then
	echo "check-one-key-owner: owner key_resolver not found in $KEY_OWNER" >&2
	exit 1
fi
if ! grep -q '^class focus_state' "$FOCUS_OWNER"; then
	echo "check-one-key-owner: owner focus_state not found in $FOCUS_OWNER" >&2
	exit 1
fi
if ! grep -q '^inline std::vector<tui_event> ui_apply_keys' "$INPUT_OWNER"; then
	echo "check-one-key-owner: adapter ui_apply_keys not found in $INPUT_OWNER" >&2
	exit 1
fi

# --- the tree: each rule outside its own owner --------------------------------
files=$(git ls-files 'src/*.cpp' 'src/*.h' 'include/*.h' 'include/**/*.h')
# shellcheck disable=SC2086
key_out=$(grep -nE "$KEY_PATTERN" $(echo "$files" | grep -v "^$KEY_OWNER\$") /dev/null)
# shellcheck disable=SC2086
focus_out=$(grep -nE "$FOCUS_PATTERN" $(echo "$files" | grep -v "^$FOCUS_OWNER\$") /dev/null)
# shellcheck disable=SC2086
input_out=$(grep -nE "$INPUT_PATTERN" $(echo "$files" | grep -v "^$INPUT_OWNER\$") /dev/null)
if [ -n "$key_out" ]; then
	echo "check-one-key-owner: chord state or a direct bindings lookup outside the one owner ($KEY_OWNER):" >&2
	echo "$key_out" >&2
	echo "  -> hand the key to key_resolver::step and read key_resolver::pending()" >&2
fi
if [ -n "$focus_out" ]; then
	echo "check-one-key-owner: focus-list or selection state outside the one owner ($FOCUS_OWNER):" >&2
	echo "$focus_out" >&2
	echo "  -> hand the key to focus_state::navigate and read focus_state's accessors" >&2
fi
if [ -n "$input_out" ]; then
	echo "check-one-key-owner: a second keys → events loop outside the one adapter ($INPUT_OWNER):" >&2
	echo "$input_out" >&2
	echo "  -> run ui_apply_keys(keys_owner, focus_owner, keys)" >&2
fi
if [ -n "$key_out" ] || [ -n "$focus_out" ] || [ -n "$input_out" ]; then
	exit 1
fi
echo "check-one-key-owner: OK — chords resolve only in $KEY_OWNER, focus moves only in $FOCUS_OWNER, the keys → events loop runs only in $INPUT_OWNER (negative controls bite)"
