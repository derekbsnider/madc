#!/usr/bin/env bash
# check-madcide-single-owners.sh — madcide's consolidated single owners
# stay single (DupFamily madcide_buffer_row_shape, consolidated IDE-10b).
#
# The buffer-table row shape {doc, path, caret, mark, bend} has ONE fresh-
# row owner: push_buffer_row. The one deliberate exception is edit_file's
# seed row (it carries LIVE interaction state, not fresh zeroes). Marker:
# a "bend"-field write — BOTH spellings of the shape: the keyed assign
# ('"bend"] = -1') and the keyed literal element ('"bend": -1', the
# rows[]-literal sweep 2026-08-31) — in tools/madcide/madcide_core.inc
# == 2 (the owner + the seed). A new open path must call push_buffer_row,
# not restate the shape.
set -u

FILE="$(dirname "$0")/../tools/madcide/madcide_core.inc"
CLIENT="$(dirname "$0")/../tools/madcide/madcide_client.inc"

count_rows()
{
	grep -cE '"bend"\] = -1|"bend": -1' "$1"
}

n=$(count_rows "$FILE")
if [ "$n" -ne 3 ]; then
	echo "check-madcide-single-owners: FAIL — $n container-navigation row" \
	     "shape sites in tools/madcide/madcide_core.inc (expected 3:" \
	     "push_buffer_row + edit_file's live-state seed + layout_tab, the" \
	     "layout tab's owner (V2)). Route new rows through those owners." >&2
	exit 1
fi

# Negative control: a synthetic third shape site must fail the count.
tmp=$(mktemp)
cat "$FILE" > "$tmp"
echo '    b["bend"] = -1;	// synthetic' >> "$tmp"
if [ "$(count_rows "$tmp")" -ne 4 ]; then
	rm -f "$tmp"
	echo "check-madcide-single-owners: FAIL — negative control did not" \
	     "detect a synthetic row site (the marker went blind)." >&2
	exit 1
fi
rm -f "$tmp"

# File IDENTITY has ONE owner: same_file (DupFamily
# madcide_path_identity_compare, consolidated at the V6 seam). "Is this
# buffer / TU / diagnostic about THAT file" is a canonical comparison
# (madc::canonical_path on both sides); a raw `["path"] ==` / `["file"] ==`
# text compare is the copy that let a TU added by one spelling while its
# buffer was open under another be double-added and dirty-marked on the
# wrong row. Marker: no raw compare of a path/file field in the core.
count_raw_path_compares()
{
	grep -cE '\["(path|file)"\] ==' "$1"
}

n=$(count_raw_path_compares "$FILE")
if [ "$n" -ne 0 ]; then
	echo "check-madcide-single-owners: FAIL — $n raw path/file text" \
	     "compare(s) in tools/madcide/madcide_core.inc (expected 0: file" \
	     "identity is same_file, canonical on both sides):" >&2
	grep -nE '\["(path|file)"\] ==' "$FILE" >&2
	exit 1
fi

# Negative control for the identity marker.
tmp=$(mktemp)
cat "$FILE" > "$tmp"
echo '    if ( r["path"] == p ) return true;	// synthetic' >> "$tmp"
if [ "$(count_raw_path_compares "$tmp")" -ne 1 ]; then
	rm -f "$tmp"
	echo "check-madcide-single-owners: FAIL — negative control did not" \
	     "detect a synthetic raw path compare (the marker went blind)." >&2
	exit 1
fi
rm -f "$tmp"

# The LAST PATH COMPONENT has ONE owner: php::basename (PHP parity, landed
# in this very arc) — the V6 seam audit found the core still hand-rolling
# it three times with strrchr('/') (each copy answering "" for a path with a
# trailing separator, where the owner answers the last real component).
# Marker: no strrchr over '/' in the core; the one strrchr left is
# path_sans_ext's '.', a different rule.
count_hand_basenames()
{
	grep -c "strrchr(.*'/')" "$1"
}

n=$(count_hand_basenames "$FILE")
if [ "$n" -ne 0 ]; then
	echo "check-madcide-single-owners: FAIL — $n hand-rolled basename" \
	     "(strrchr '/') site(s) in tools/madcide/madcide_core.inc (expected" \
	     "0: the last path component is php::basename)." >&2
	grep -n "strrchr(.*'/')" "$FILE" >&2
	exit 1
fi

# Negative control for the basename marker.
tmp=$(mktemp)
cat "$FILE" > "$tmp"
echo "    const char *sl = strrchr(bp.c_str(), '/');	// synthetic" >> "$tmp"
if [ "$(count_hand_basenames "$tmp")" -ne 1 ]; then
	rm -f "$tmp"
	echo "check-madcide-single-owners: FAIL — negative control did not" \
	     "detect a synthetic hand-rolled basename (the marker went blind)." >&2
	exit 1
fi
rm -f "$tmp"

# A session's browser URL has ONE spelling: session_url (madcide_discover.inc)
# — the record's `url` and the $/madc/serve announcement both call it (the V6
# seam audit found the format restated in each). Marker: the http:// format
# appears once across tools/madcide.
TOOLS="$(dirname "$0")/../tools/madcide"
count_url_spellings()
{
	cat "$@" | grep -c 'format("http://{}/"'
}

n=$(count_url_spellings "$TOOLS"/*.inc)
if [ "$n" -ne 1 ]; then
	echo "check-madcide-single-owners: FAIL — $n session URL spelling(s)" \
	     "across tools/madcide (expected 1: session_url). Call the owner." >&2
	exit 1
fi

# Negative control for the URL marker.
tmp=$(mktemp)
cat "$TOOLS"/*.inc > "$tmp"
echo '    var u = format("http://{}/", ep);	// synthetic' >> "$tmp"
if [ "$(count_url_spellings "$tmp")" -ne 2 ]; then
	rm -f "$tmp"
	echo "check-madcide-single-owners: FAIL — negative control did not" \
	     "detect a synthetic URL spelling (the marker went blind)." >&2
	exit 1
fi
rm -f "$tmp"

# An MCP tool REFUSAL has ONE owner: tool_refuse (madcide_mcp.inc — the
# {content:[{type:text,text}], isError:true} envelope every tool family
# shares). The V6 seam audit found graph_call restating the envelope three
# times beside six adopters; a field added to the owner (a code, a hint)
# would have missed those three. Marker: the `"isError": true` literal
# appears exactly once in madcide_mcp.inc — inside the owner.
MCP="$(dirname "$0")/../tools/madcide/madcide_mcp.inc"
count_refusal_shapes()
{
	grep -c '"isError": true' "$1"
}

n=$(count_refusal_shapes "$MCP")
if [ "$n" -ne 1 ]; then
	echo "check-madcide-single-owners: FAIL — $n MCP refusal envelope" \
	     "site(s) in tools/madcide/madcide_mcp.inc (expected 1:" \
	     "tool_refuse). Call the owner, never restate the shape." >&2
	exit 1
fi

# Negative control for the refusal marker.
tmp=$(mktemp)
cat "$MCP" > "$tmp"
echo '    out = { "content": content, "isError": true };	// synthetic' >> "$tmp"
if [ "$(count_refusal_shapes "$tmp")" -ne 2 ]; then
	rm -f "$tmp"
	echo "check-madcide-single-owners: FAIL — negative control did not" \
	     "detect a synthetic refusal envelope (the marker went blind)." >&2
	exit 1
fi
rm -f "$tmp"

# A registry command is RUN by ONE core: api_run (madcide_api.inc — resolve
# the name, gate the tier, S.command, count the error rows, compose, typeset,
# fan out the new events). Every transport drives it — the api seat, the MCP
# seat, the LSP face and, since the V6 seam, the ui::NONE `-c` client, which
# had restated the run/compose/render sequence. Marker: `S.command(` appears
# once across tools/madcide.
count_command_runs()
{
	cat "$@" | grep -c '\bS\.command('
}

n=$(count_command_runs "$TOOLS"/*.inc)
if [ "$n" -ne 1 ]; then
	echo "check-madcide-single-owners: FAIL — $n S.command( run site(s)" \
	     "across tools/madcide (expected 1: api_run, the one command core" \
	     "every transport drives)." >&2
	grep -n '\bS\.command(' "$TOOLS"/*.inc >&2
	exit 1
fi

# Negative control for the command-core marker.
tmp=$(mktemp)
cat "$TOOLS"/*.inc > "$tmp"
echo '    bool ok = S.command(adoc, code, arg, cont);	// synthetic' >> "$tmp"
if [ "$(count_command_runs "$tmp")" -ne 2 ]; then
	rm -f "$tmp"
	echo "check-madcide-single-owners: FAIL — negative control did not" \
	     "detect a synthetic command run (the marker went blind)." >&2
	exit 1
fi
rm -f "$tmp"

# The return-key pause has ONE owner: terminal_return_pause (DupFamily
# terminal_return_pause, consolidated with the fork-Run slice; it lives
# in the TUI client since the gateway seam — the pause is terminal I/O).
# Marker: the prompt spelling appears exactly once across both layers —
# a second pause prompt means a run flow restated the pause instead of
# calling the owner.
count_pause()
{
	cat "$@" | grep -c 'press enter to return'
}

n=$(count_pause "$FILE" "$CLIENT")
if [ "$n" -ne 1 ]; then
	echo "check-madcide-single-owners: FAIL — $n return-pause prompts" \
	     "across madcide_core.inc + madcide_client.inc (expected 1:" \
	     "terminal_return_pause in the client). Call the owner, never" \
	     "restate the pause." >&2
	exit 1
fi

# Negative control for the pause marker.
tmp=$(mktemp)
cat "$FILE" > "$tmp"
echo '    println("[madcide] press enter to return");	// synthetic' >> "$tmp"
if [ "$(count_pause "$tmp" "$CLIENT")" -ne 2 ]; then
	rm -f "$tmp"
	echo "check-madcide-single-owners: FAIL — negative control did not" \
	     "detect a synthetic pause prompt (the marker went blind)." >&2
	exit 1
fi
rm -f "$tmp"

# "A project manifest is OPEN" has ONE owner: proj_has_manifest (the
# projfile slot non-null and non-empty). The V6 seam audit found the test
# inlined nine times beside the owner — once four lines after CALLING it.
# Marker: the slot's emptiness test (`strlen(pf`) appears once in the core.
count_manifest_tests()
{
	grep -c 'strlen(pf' "$1"
}

n=$(count_manifest_tests "$FILE")
if [ "$n" -ne 1 ]; then
	echo "check-madcide-single-owners: FAIL — $n manifest-open test(s) in" \
	     "tools/madcide/madcide_core.inc (expected 1: proj_has_manifest)." >&2
	grep -n 'strlen(pf' "$FILE" >&2
	exit 1
fi

# Negative control for the manifest marker.
tmp=$(mktemp)
cat "$FILE" > "$tmp"
echo '    bool has = !pf.is_null() && strlen(pf.c_str()) > 0;	// synthetic' >> "$tmp"
if [ "$(count_manifest_tests "$tmp")" -ne 2 ]; then
	rm -f "$tmp"
	echo "check-madcide-single-owners: FAIL — negative control did not" \
	     "detect a synthetic manifest test (the marker went blind)." >&2
	exit 1
fi
rm -f "$tmp"

# "Is the document DIRTY" has ONE owner: doc_modified (editor_events.inc).
# The `modified` slot is unset until a document's first write and reading an
# unset slot as a boolean throws; the V6 seam audit found ten reads, nine
# guarded by hand and one not. Marker: the slot is READ once across the
# texteditor + madcide layers — inside the owner.
TEXTED="$(dirname "$0")/../tools/texteditor"
count_modified_reads()
{
	cat "$@" | grep -c 'ui::get([a-z]*, w, [^,]*, "modified")'
}

n=$(count_modified_reads "$TEXTED"/*.inc "$TOOLS"/*.inc)
if [ "$n" -ne 1 ]; then
	echo "check-madcide-single-owners: FAIL — $n reads of the modified slot" \
	     "across tools/texteditor + tools/madcide (expected 1: doc_modified)." >&2
	grep -n 'ui::get([a-z]*, w, [^,]*, "modified")' "$TEXTED"/*.inc "$TOOLS"/*.inc >&2
	exit 1
fi

# Negative control for the modified-read marker.
tmp=$(mktemp)
cat "$TEXTED"/*.inc "$TOOLS"/*.inc > "$tmp"
echo '    ui::get(m, w, doc, "modified");	// synthetic' >> "$tmp"
if [ "$(count_modified_reads "$tmp")" -ne 2 ]; then
	rm -f "$tmp"
	echo "check-madcide-single-owners: FAIL — negative control did not" \
	     "detect a synthetic modified read (the marker went blind)." >&2
	exit 1
fi
rm -f "$tmp"

# An optional KEYED READ of a maybe-null map has ONE owner: keyed_get
# (editor_events.inc). madcide_past.inc carried a byte-identical twin,
# arg_of, until the V6 seam (32 call sites rode the second name). Marker:
# the guarded-read body appears once across the texteditor + madcide layers.
TEXTED="$(dirname "$0")/../tools/texteditor"
count_keyed_reads()
{
	cat "$@" | grep -c 'out = [a-z]*\[key\];'
}

n=$(count_keyed_reads "$TEXTED"/*.inc "$TOOLS"/*.inc)
if [ "$n" -ne 1 ]; then
	echo "check-madcide-single-owners: FAIL — $n guarded keyed-read" \
	     "bodies across tools/texteditor + tools/madcide (expected 1:" \
	     "keyed_get). Call the owner, never restate it under another name." >&2
	exit 1
fi

# Negative control for the keyed-read marker.
tmp=$(mktemp)
cat "$TEXTED"/*.inc "$TOOLS"/*.inc > "$tmp"
echo '	out = args[key];	// synthetic' >> "$tmp"
if [ "$(count_keyed_reads "$tmp")" -ne 2 ]; then
	rm -f "$tmp"
	echo "check-madcide-single-owners: FAIL — negative control did not" \
	     "detect a synthetic keyed-read body (the marker went blind)." >&2
	exit 1
fi
rm -f "$tmp"

# Text mutation has ONE owner pair: ed_text_insert / ed_text_erase
# (editor_events.inc) — they shift the es bag's byte-anchored highlight
# spans with the edit; a raw ui::text_insert/text_erase elsewhere leaves
# spans repainting one byte off per keystroke (the "typing spaces
# re-shades everything" defect). Allowed raw sites: the two owner bodies,
# madcide's [build]-buffer append, and clog_append's change-log buffer
# append (both a different, non-viewed entity — shifting the es's spans by
# its bytes would corrupt them).
EVENTS="$(dirname "$0")/../tools/texteditor/editor_events.inc"

count_raw_mutations()
{
	cat "$@" | grep -c 'ui::text_insert(\|ui::text_erase('
}

n=$(count_raw_mutations "$FILE" "$EVENTS")
if [ "$n" -ne 4 ]; then
	echo "check-madcide-single-owners: FAIL — $n raw ui::text_insert/" \
	     "text_erase sites across madcide_core.inc + editor_events.inc" \
	     "(expected 4: the ed_text_insert/ed_text_erase owner bodies +" \
	     "append_build_line's cross-doc append + clog_append's change-log" \
	     "buffer append). Route edits through the owners so highlight" \
	     "spans shift with the text." >&2
	exit 1
fi

# Negative control for the mutation marker.
tmp=$(mktemp)
cat "$FILE" > "$tmp"
echo '    ui::text_insert(w, doc, caret, "x");	// synthetic' >> "$tmp"
if [ "$(count_raw_mutations "$tmp" "$EVENTS")" -ne 5 ]; then
	rm -f "$tmp"
	echo "check-madcide-single-owners: FAIL — negative control did not" \
	     "detect a synthetic raw mutation (the marker went blind)." >&2
	exit 1
fi
rm -f "$tmp"

# The change-log record KIND has ONE reader per layer (Nexus L4b, the enum
# law): clog_kind_of (editor_events.inc) and nexus_kind_of (madcide_enums.inc)
# convert the persisted word ONCE; every consumer switches on the code.
# Marker: a literal `["kind"] == "` compare anywhere in the dialect tools
# must count 0 — a new one is a second reader of the wire word.
count_kind_compares()
{
	cat "$@" | grep -c '\["kind"\] == "'
}

KIND_FILES="$(dirname "$0")/../tools/texteditor/*.inc $(dirname "$0")/../tools/madcide/*.inc"
n=$(count_kind_compares $KIND_FILES)
if [ "$n" -ne 0 ]; then
	echo "check-madcide-single-owners: FAIL — $n literal record-kind" \
	     "compare(s) in tools/texteditor + tools/madcide (expected 0)." \
	     "Read the kind through clog_kind_of / nexus_kind_of and switch" \
	     "on the code." >&2
	grep -n '\["kind"\] == "' $KIND_FILES >&2
	exit 1
fi

# Negative control for the kind-compare marker.
tmp=$(mktemp)
echo '    if ( rec["kind"] == "splice" )	// synthetic' > "$tmp"
if [ "$(count_kind_compares "$tmp")" -ne 1 ]; then
	rm -f "$tmp"
	echo "check-madcide-single-owners: FAIL — negative control did not" \
	     "detect a synthetic kind compare (the marker went blind)." >&2
	exit 1
fi
rm -f "$tmp"

# The candidate VALIDATOR has ONE seat (Nexus L4c, design §3.4): the engine's
# one body with a commit flag is called as parse_refresh_checked (swap) and
# parse_would_accept (verdict only) — each EXACTLY ONCE across the dialect
# tools, both inside graph_edit_apply. A second call site is a second edit
# path that will drift from the all-or-nothing transaction.
count_validator_calls()
{
	cat "$@" | grep -c 'madc::parse_refresh_checked(\|madc::parse_would_accept('
}

VALIDATOR_FILES="$(dirname "$0")/../tools/madcide/*.inc $(dirname "$0")/../tools/texteditor/*.inc"
n=$(count_validator_calls $VALIDATOR_FILES)
if [ "$n" -ne 2 ]; then
	echo "check-madcide-single-owners: FAIL — $n validator call sites in" \
	     "tools/madcide + tools/texteditor (expected 2: parse_refresh_checked" \
	     "+ parse_would_accept, both in graph_edit_apply). ONE validator with" \
	     "a commit flag — route every candidate through graph_edit_apply." >&2
	grep -n 'madc::parse_refresh_checked(\|madc::parse_would_accept(' $VALIDATOR_FILES >&2
	exit 1
fi

# Negative control for the validator marker.
tmp=$(mktemp)
echo '    bool ok = madc::parse_would_accept(d, h, s.c_str());	// synthetic' > "$tmp"
if [ "$(count_validator_calls $VALIDATOR_FILES "$tmp")" -ne 3 ]; then
	rm -f "$tmp"
	echo "check-madcide-single-owners: FAIL — negative control did not" \
	     "detect a synthetic validator call (the marker went blind)." >&2
	exit 1
fi
rm -f "$tmp"

echo "check-madcide-single-owners: OK (one fresh-row owner: push_buffer_row;" \
     "one pause owner: terminal_return_pause; one text-mutation owner pair:" \
     "ed_text_insert/ed_text_erase; one record-kind reader per layer; one" \
     "validator seat: graph_edit_apply)"
exit 0
