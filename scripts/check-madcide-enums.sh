#!/usr/bin/env bash
# check-madcide-enums.sh — the IDE's discriminators are ENUMS (V0.5d,
# owner law 2026-09-09: enums, not strings). The bag's slots (pane,
# paneltab, pmode, vimode) and the kinds that ride a request are integers
# from tools/madcide/madcide_enums.inc; a NAME appears only where text
# leaves the program, through that file's *_name() converters. Three
# rules over tools/madcide/madcide_core.inc + madcide_client.inc +
# madcide_once.inc (the ui::NONE client):
#   1. no string literal is written to a discriminator slot
#      (`"pane", "x"` / `"paneltab", "x"` / `"pmode", "x"` / `"vimode", "x"`
#      / `"dlgkind", "x"`);
#   2. no compare against a discriminator's NAME word — the words are the
#      `return "…";` literals of the converters in madcide_enums.inc
#      (pane_name, tab_name, prompt_name, vimode_name, req_name): a
#      `== "outline"` or `!= "insert"` in the core is a string discriminator
#      that came back;
#   3. no request literal names its kind as text (`rq = { "kind": "…"`);
#   4. (V1 Views) no View row carries its representation as text — a
#      `"lang": "…"` or `"generator": "…"` literal field (the lang is a
#      madc::fk* enumerator from <bits/file_kinds>, the generator an
#      ide_gen); the focused-View slot `fview` takes no string either;
#   5. (V2 layouts) no layout node carries its discriminator as text — a
#      `"slot": "…"`, `"side": "…"`, `"mode": "…"` or `"dir": "…"` literal
#      field (ide_slot / ui::side / ide_pmode / ui::split belong there).
# Each rule carries a negative control.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CORE="$ROOT/tools/madcide/madcide_core.inc"
CLIENT="$ROOT/tools/madcide/madcide_client.inc"
ONCE="$ROOT/tools/madcide/madcide_once.inc"
ENUMS="$ROOT/tools/madcide/madcide_enums.inc"

# The name words: every `return "word";` inside the five name converters.
name_words()
{
	awk '/^const char \*(pane|tab|prompt|vimode|req|view|gen|slot|container|pmode)_name\(long/ { on = 1 }
	     on { print }
	     on && /^}/ { on = 0 }' "$1" |
	grep -o 'return "[a-z]*";' | sed 's/return "//; s/";$//' | grep -v '^$' | sort -u
}

check()
{
	local core="$1" client="$2" enums="$3" label="$4" once="$5"
	local words alt bad rc=0
	words=$(name_words "$enums")
	if [ -z "$words" ]; then
		echo "check-madcide-enums: FAIL ($label) — no name words found in" \
		     "madcide_enums.inc (the converter anchors moved)." >&2
		return 1
	fi
	alt=$(echo "$words" | paste -sd'|' -)
	bad=$(grep -n -E 'ui::set\([a-z0-9]+, [a-z0-9]+, "(pane|paneltab|pmode|vimode|dlgkind|fview)", "' "$core" "$client" "$once")
	if [ -n "$bad" ]; then
		echo "check-madcide-enums: FAIL ($label) — a string literal is written" \
		     "to a discriminator slot:" >&2
		echo "$bad" >&2
		rc=1
	fi
	bad=$(grep -n -E "[!=]= \"($alt)\"" "$core" "$client" "$once")
	if [ -n "$bad" ]; then
		echo "check-madcide-enums: FAIL ($label) — a compare against a" \
		     "discriminator's name word (an enumerator belongs there):" >&2
		echo "$bad" >&2
		rc=1
	fi
	bad=$(grep -n -E 'rq = \{ "kind": "' "$core" "$client" "$once")
	if [ -n "$bad" ]; then
		echo "check-madcide-enums: FAIL ($label) — a request literal names its" \
		     "kind as text:" >&2
		echo "$bad" >&2
		rc=1
	fi
	bad=$(grep -n -E '"(lang|generator)": "' "$core" "$client" "$once")
	if [ -n "$bad" ]; then
		echo "check-madcide-enums: FAIL ($label) — a View row carries its" \
		     "representation as text (madc::fk* / ide_gen belong there):" >&2
		echo "$bad" >&2
		rc=1
	fi
	bad=$(grep -n -E '"(slot|side|mode|dir)": "' "$core" "$client" "$once")
	if [ -n "$bad" ]; then
		echo "check-madcide-enums: FAIL ($label) — a layout node carries a" \
		     "discriminator as text (ide_slot / ui::side / ide_pmode / ui::split belong there):" >&2
		echo "$bad" >&2
		rc=1
	fi
	return $rc
}

if ! check "$CORE" "$CLIENT" "$ENUMS" "live" "$ONCE"; then
	exit 1
fi

# Negative controls: each rule must catch a synthetic drift.
tmpcore=$(mktemp)
awk '{ print } /^bool IdeSession::apply_ide_event/ { print "\tui::set(w, es, \"pane\", \"zz\");" }' "$CORE" > "$tmpcore"
if check "$tmpcore" "$CLIENT" "$ENUMS" "control" "$ONCE" 2>/dev/null; then
	rm -f "$tmpcore"
	echo "check-madcide-enums: FAIL — negative control: a string written to a" \
	     "slot went undetected (rule 1 went blind)." >&2
	exit 1
fi
awk '{ print } /^bool IdeSession::apply_ide_event/ { print "\tif ( pane == \"outline\" ) return true;" }' "$CORE" > "$tmpcore"
if check "$tmpcore" "$CLIENT" "$ENUMS" "control" "$ONCE" 2>/dev/null; then
	rm -f "$tmpcore"
	echo "check-madcide-enums: FAIL — negative control: a name-word compare" \
	     "went undetected (rule 2 went blind)." >&2
	exit 1
fi
awk '{ print } /^bool IdeSession::apply_ide_event/ { print "\tvar rq = { \"kind\": \"shell\", \"pause\": 0 };" }' "$CORE" > "$tmpcore"
if check "$tmpcore" "$CLIENT" "$ENUMS" "control" "$ONCE" 2>/dev/null; then
	rm -f "$tmpcore"
	echo "check-madcide-enums: FAIL — negative control: a text request kind" \
	     "went undetected (rule 3 went blind)." >&2
	exit 1
fi
awk '{ print } /^bool IdeSession::apply_ide_event/ { print "\tvs[] = { \"id\": 1, \"lang\": \"mc11\", \"generator\": \"madc\" };" }' "$CORE" > "$tmpcore"
if check "$tmpcore" "$CLIENT" "$ENUMS" "control" "$ONCE" 2>/dev/null; then
	rm -f "$tmpcore"
	echo "check-madcide-enums: FAIL — negative control: a View row's text" \
	     "representation went undetected (rule 4 went blind)." >&2
	exit 1
fi
awk '{ print } /^bool IdeSession::apply_ide_event/ { print "\tvar ln = { \"kind\": ctPANE, \"slot\": \"sidebar\", \"side\": \"left\" };" }' "$CORE" > "$tmpcore"
if check "$tmpcore" "$CLIENT" "$ENUMS" "control" "$ONCE" 2>/dev/null; then
	rm -f "$tmpcore"
	echo "check-madcide-enums: FAIL — negative control: a layout node's text" \
	     "discriminator went undetected (rule 5 went blind)." >&2
	exit 1
fi
rm -f "$tmpcore"
n=$(name_words "$ENUMS" | wc -l)
echo "check-madcide-enums: OK ($n discriminator names stay behind the converters; no slot takes text; controls bite)"
