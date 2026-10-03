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
# The seat files (madcide_mcp / _past / _propose / _seat / _nexus / _tests /
# _mcpclient / _layers.inc) are checked by the same rules (Nexus L4c–L4e):
# the tier, proposal-status, intent-vocabulary, layer, test-result and
# node-serve words joined the converter list.
#   7. every unscoped enumerator of madcide's sources (tools/madcide/*.inc,
#      the plugin API <madcide/plugin_api>, the shared editor core
#      tools/texteditor/*.inc — one translation unit) is declared ONCE: C11
#      6.7.2.2 and [dcl.enum] make a second declaration of the name in one
#      scope an error, which gcc, g++ and clang report and madc does not yet
#      (BUGS.md B104: the later declaration silently wins, as plugin_verb's
#      pvEVENT once renumbered provenance's).
# Each rule carries a negative control.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CORE="$ROOT/tools/madcide/madcide_core.inc"
CLIENT="$ROOT/tools/madcide/madcide_client.inc"
ONCE="$ROOT/tools/madcide/madcide_once.inc"
ENUMS="$ROOT/tools/madcide/madcide_enums.inc"
# The seat layer (Nexus L4c): checked with the same rules.
SEAT_FILES=$(ls "$ROOT"/tools/madcide/madcide_mcp.inc "$ROOT"/tools/madcide/madcide_past.inc \
	"$ROOT"/tools/madcide/madcide_propose.inc "$ROOT"/tools/madcide/madcide_seat.inc \
	"$ROOT"/tools/madcide/madcide_nexus.inc "$ROOT"/tools/madcide/madcide_tests.inc \
	"$ROOT"/tools/madcide/madcide_mcpclient.inc "$ROOT"/tools/madcide/madcide_layers.inc \
	"$ROOT"/tools/madcide/madcide_lsp.inc 2>/dev/null)

# The name words: every `return "word";` inside the five name converters.
name_words()
{
	awk '/^const char \*(pane|tab|prompt|vimode|req|view|gen|slot|container|pmode|tier|proposal_status|record_kind|record_state|record_op|link_op|ref_kind|rel|provenance|test_family|nexus_op|mcp_transport|explain_detail|layer|test_result|node_serve|pos_encoding|lsp_token|lsp_method|mcp_method)_name\(long/ { on = 1 }
	     on { print }
	     on && /^}/ { on = 0 }' "$1" |
	grep -o 'return "[a-z/]*";' | sed 's/return "//; s/";$//' | grep -v '^$' | sort -u
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
	bad=$(grep -n -E 'ui::set\([a-z0-9]+, [a-z0-9]+, "(pane|paneltab|pmode|vimode|dlgkind|fview)", "' "$core" "$client" "$once" $SEAT_FILES)
	if [ -n "$bad" ]; then
		echo "check-madcide-enums: FAIL ($label) — a string literal is written" \
		     "to a discriminator slot:" >&2
		echo "$bad" >&2
		rc=1
	fi
	bad=$(grep -n -E "[!=]= \"($alt)\"" "$core" "$client" "$once" $SEAT_FILES)
	if [ -n "$bad" ]; then
		echo "check-madcide-enums: FAIL ($label) — a compare against a" \
		     "discriminator's name word (an enumerator belongs there):" >&2
		echo "$bad" >&2
		rc=1
	fi
	bad=$(grep -n -E 'rq = \{ "kind": "' "$core" "$client" "$once" $SEAT_FILES)
	if [ -n "$bad" ]; then
		echo "check-madcide-enums: FAIL ($label) — a request literal names its" \
		     "kind as text:" >&2
		echo "$bad" >&2
		rc=1
	fi
	bad=$(grep -n -E '"(lang|generator)": "' "$core" "$client" "$once" $SEAT_FILES)
	if [ -n "$bad" ]; then
		echo "check-madcide-enums: FAIL ($label) — a View row carries its" \
		     "representation as text (madc::fk* / ide_gen belong there):" >&2
		echo "$bad" >&2
		rc=1
	fi
	bad=$(grep -n -E '"(slot|side|mode|dir)": "' "$core" "$client" "$once" $SEAT_FILES)
	if [ -n "$bad" ]; then
		echo "check-madcide-enums: FAIL ($label) — a layout node carries a" \
		     "discriminator as text (ide_slot / ui::side / ide_pmode / ui::split belong there):" >&2
		echo "$bad" >&2
		rc=1
	fi
	# 6. (V6 seam) no strcmp/strncmp ladder against a discriminator's name
	#    word — the MCP seat's top-level dispatch did this while its own tool
	#    families and the LSP face converted once; a wire method converts
	#    through its *_of() and the seat switches on the code.
	bad=$(grep -n -E "strn?cmp\([^,]+, \"($alt)\"" "$core" "$client" "$once" $SEAT_FILES)
	if [ -n "$bad" ]; then
		echo "check-madcide-enums: FAIL ($label) — a strcmp against a" \
		     "discriminator's name word (convert once through its *_of(), switch on the code):" >&2
		echo "$bad" >&2
		rc=1
	fi
	return $rc
}

# Rule 7's files: madcide's translation unit.
ENUM_FILES=$(ls "$ROOT"/tools/madcide/*.inc "$ROOT"/tools/madcide/include/madcide/* \
	"$ROOT"/tools/texteditor/*.inc 2>/dev/null)

# Every unscoped enumerator the given files declare, one name per line: the
# body of each `enum NAME [: TYPE] {` that starts a line (its brace on that
# line or the next), `enum class` / `enum struct` skipped (their names are
# the enum's own), comments stripped, each item's name before any `=`.
enumerators()
{
	awk '
	{ sub(/\/\/.*/, "") }
	function take(body,   n, i, items, it) {
		n = split(body, items, ",")
		for ( i = 1; i <= n; i++ ) {
			it = items[i]
			sub(/=.*/, "", it)
			gsub(/[ \t\r]/, "", it)
			if ( it ~ /^[A-Za-z_][A-Za-z0-9_]*$/ )
				print it
		}
	}
	on {
		if ( index($0, "}") ) { body = body " " substr($0, 1, index($0, "}") - 1); take(body); on = 0 }
		else body = body " " $0
		next
	}
	pend {
		pend = 0
		if ( $0 ~ /^[ \t]*\{/ ) { line = substr($0, index($0, "{") + 1) }
		else next
		if ( index(line, "}") ) { take(substr(line, 1, index(line, "}") - 1)); next }
		on = 1; body = line; next
	}
	/^[ \t]*enum[ \t]+[A-Za-z_]/ && !/^[ \t]*enum[ \t]+(class|struct)[ \t]/ {
		if ( !index($0, "{") ) { if ( !index($0, ";") ) pend = 1; next }
		line = substr($0, index($0, "{") + 1)
		if ( index(line, "}") ) { take(substr(line, 1, index(line, "}") - 1)); next }
		on = 1; body = line
	}' "$@"
}

# Rule 7: the names declared more than once (empty = none).
twice_declared()
{
	enumerators "$@" | sort | uniq -d
}

if ! check "$CORE" "$CLIENT" "$ENUMS" "live" "$ONCE"; then
	exit 1
fi
nenum=$(enumerators $ENUM_FILES | wc -l)
if [ "$nenum" -lt 200 ]; then
	echo "check-madcide-enums: FAIL — rule 7 read only $nenum enumerators" \
	     "(the enum reader went blind)." >&2
	exit 1
fi
twice=$(twice_declared $ENUM_FILES)
if [ -n "$twice" ]; then
	echo "check-madcide-enums: FAIL — an enumerator is declared twice (gcc," \
	     "g++ and clang refuse it; madc lets the later one win, B104):" >&2
	echo "$twice" >&2
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
awk '{ print } /^bool IdeSession::apply_ide_event/ { print "\tif ( strcmp(m, \"initialize\") == 0 ) return true;" }' "$CORE" > "$tmpcore"
if check "$tmpcore" "$CLIENT" "$ENUMS" "control" "$ONCE" 2>/dev/null; then
	rm -f "$tmpcore"
	echo "check-madcide-enums: FAIL — negative control: a strcmp ladder" \
	     "against a method word went undetected (rule 6 went blind)." >&2
	exit 1
fi
tmpenum=$(mktemp)
printf 'enum zz_control : unsigned char\n{\n    zzNONE = 0, cmdNONE\n};\n' > "$tmpenum"
if [ -z "$(twice_declared $ENUM_FILES "$tmpenum")" ]; then
	rm -f "$tmpcore" "$tmpenum"
	echo "check-madcide-enums: FAIL — negative control: an enumerator declared" \
	     "twice went undetected (rule 7 went blind)." >&2
	exit 1
fi
rm -f "$tmpcore" "$tmpenum"
n=$(name_words "$ENUMS" | wc -l)
echo "check-madcide-enums: OK ($n discriminator names stay behind the converters; no slot takes text; $nenum enumerators each declared once; controls bite)"
