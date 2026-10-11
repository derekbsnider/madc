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
TOOLS="$(dirname "$0")/../tools/madcide"
TEXTED="$(dirname "$0")/../tools/texteditor"

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

# A session's browser URL has ONE spelling: session_url (madcide_serve.inc)
# — the advertisement's `url` calls it (the V6 seam audit found the format
# restated beside the $/madc/serve announcement). Marker: the http:// format
# appears once across tools/madcide.
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

# A registry command is RUN by ONE core: api_run (madcide_api.inc — resolve
# the name, gate the tier, S.command, count the error rows, compose, typeset,
# fan out the new events). Every transport drives it — the api seat, the MCP
# tools (ide_command_tool) and, since the V6 seam, the ui::NONE `-c` client, which
# had restated the run/compose/render sequence. Marker: a `.command(` call,
# through ANY receiver, appears once across tools/madcide (the receiver's
# name is not the rule: a session spelled `s` runs a command the same way). A
# command run from inside the session or a client is its action event
# through apply_ide_event (action_event), never a second command call.
count_command_runs()
{
	cat "$@" | grep -c -E '\b[A-Za-z_][A-Za-z0-9_]*\.command\('
}

n=$(count_command_runs "$TOOLS"/*.inc)
if [ "$n" -ne 1 ]; then
	echo "check-madcide-single-owners: FAIL — $n S.command( run site(s)" \
	     "across tools/madcide (expected 1: api_run, the one command core" \
	     "every transport drives)." >&2
	grep -n -E '\b[A-Za-z_][A-Za-z0-9_]*\.command\(' "$TOOLS"/*.inc >&2
	exit 1
fi

# Negative control for the command-core marker.
tmp=$(mktemp)
cat "$TOOLS"/*.inc > "$tmp"
echo '    bool ok = S.command(adoc, code, arg, cont);	// synthetic' >> "$tmp"
echo '    s.command(doc, code, arg, cont);		// synthetic, another receiver' >> "$tmp"
if [ "$(count_command_runs "$tmp")" -ne 3 ]; then
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

# "Is the document DIRTY" has ONE owner: madc's document component
# (ui::doc_modified — DERIVED: the text's version differs from the one last
# saved; split arc 1D). The bag slot `modified` it replaced was unset until a
# first write, read ten ways (nine guarded, one not) and set by every edit
# site by hand. Marker: the slot is neither READ nor WRITTEN anywhere in the
# texteditor + madcide layers.
count_modified_slot()
{
	cat "$@" | grep -cE 'ui::(get\([a-z]*, w, [^,]*|set\(w, [^,]*), "modified"'
}

n=$(count_modified_slot "$TEXTED"/*.inc "$TOOLS"/*.inc)
if [ "$n" -ne 0 ]; then
	echo "check-madcide-single-owners: FAIL — $n uses of the modified bag slot" \
	     "across tools/texteditor + tools/madcide (expected 0: ui::doc_modified" \
	     "is the document's own, derived)." >&2
	grep -nE 'ui::(get\([a-z]*, w, [^,]*|set\(w, [^,]*), "modified"' "$TEXTED"/*.inc \
	     "$TOOLS"/*.inc >&2
	exit 1
fi

# Negative control: a synthetic read and a synthetic write both trip it.
tmp=$(mktemp)
echo '    ui::get(m, w, doc, "modified");	// synthetic' > "$tmp"
echo '    ui::set(w, doc, "modified", true);	// synthetic' >> "$tmp"
if [ "$(count_modified_slot "$tmp")" -ne 2 ]; then
	rm -f "$tmp"
	echo "check-madcide-single-owners: FAIL — negative control did not" \
	     "detect a synthetic modified-slot read and write (the marker went blind)." >&2
	exit 1
fi
rm -f "$tmp"

# An optional KEYED READ of a maybe-null map has ONE owner: keyed_get
# (editor_events.inc). madcide_past.inc carried a byte-identical twin,
# arg_of, until the V6 seam (32 call sites rode the second name). Marker:
# the guarded-read body appears once across the texteditor + madcide layers.
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
# (editor_events.inc) — they name the client the edit is for, so the edit
# follower (ed_text_changed) moves THAT client's byte-anchored highlight
# spans and selection as the editor's, every other viewer's as a peer's; a
# raw ui::text_insert/text_erase elsewhere is no client's edit (the "typing
# spaces re-shades everything" defect was the unshifted form). Allowed raw
# sites: the two owner bodies and madcide's [build]-buffer append (a
# different, non-viewed entity — naming the es would shift its spans by
# another document's bytes).
EVENTS="$(dirname "$0")/../tools/texteditor/editor_events.inc"

count_raw_mutations()
{
	cat "$@" | grep -c 'ui::text_insert(\|ui::text_erase('
}

n=$(count_raw_mutations "$FILE" "$EVENTS")
if [ "$n" -ne 3 ]; then
	echo "check-madcide-single-owners: FAIL — $n raw ui::text_insert/" \
	     "text_erase sites across madcide_core.inc + editor_events.inc" \
	     "(expected 3: the ed_text_insert/ed_text_erase owner bodies +" \
	     "append_build_line's cross-doc append). Route edits through the" \
	     "owners so the editing client's spans shift with the text." >&2
	exit 1
fi

# Negative control for the mutation marker.
tmp=$(mktemp)
cat "$FILE" > "$tmp"
echo '    ui::text_insert(w, doc, caret, "x");	// synthetic' >> "$tmp"
if [ "$(count_raw_mutations "$tmp" "$EVENTS")" -ne 4 ]; then
	rm -f "$tmp"
	echo "check-madcide-single-owners: FAIL — negative control did not" \
	     "detect a synthetic raw mutation (the marker went blind)." >&2
	exit 1
fi
rm -f "$tmp"

# The change-log record KIND has ONE reader (Nexus L4b, the enum law):
# clog_kind_of (editor_events.inc) converts the wire word of a record the
# tool host's memory hands back ONCE; every consumer switches on the code.
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

# "Is this path a FILE or a pseudo-buffer" has ONE owner: path_is_asset
# (madcide_core.inc; DupFamily madcide_pseudo_buffer_test, consolidated with
# F5, plan §41.10a slice 3 — the slice had begun a second copy,
# path_is_pseudo_buffer, beside the layer owner's). A pseudo-buffer
# ([build], [terminal], [repl]) is named in brackets. Marker: the bracket
# test on a name's first byte appears once across tools/madcide +
# tools/texteditor — inside the owner.
count_bracket_tests()
{
	cat "$@" | grep -cE "\[0\] *[!=]= *'\['"
}

n=$(count_bracket_tests "$TEXTED"/*.inc "$TOOLS"/*.inc)
if [ "$n" -ne 1 ]; then
	echo "check-madcide-single-owners: FAIL — $n pseudo-buffer bracket" \
	     "test(s) across tools/texteditor + tools/madcide (expected 1:" \
	     "path_is_asset)." >&2
	grep -nE "\[0\] *[!=]= *'\['" "$TEXTED"/*.inc "$TOOLS"/*.inc >&2
	exit 1
fi

# Negative control for the bracket marker.
tmp=$(mktemp)
echo "    if ( pc[0] == '[' ) return;	// synthetic" > "$tmp"
if [ "$(count_bracket_tests "$TEXTED"/*.inc "$TOOLS"/*.inc "$tmp")" -ne 2 ]; then
	rm -f "$tmp"
	echo "check-madcide-single-owners: FAIL — negative control did not" \
	     "detect a synthetic bracket test (the marker went blind)." >&2
	exit 1
fi
rm -f "$tmp"

# STOPPING a build has ONE owner: stop_build (both kinds: the exec pump's
# sent stop, the in-process build's scope_cancel). IdeSession::close() had
# its own copy of the first branch only, so an in-process build was never
# cancelled at the session's end (consolidated into IdeSession::stop_tasks
# with the quit-hang fix, 2026-09-28). Marker: the build's stop handle is
# READ once across tools/madcide — inside the owner.
count_buildstop_reads()
{
	cat "$@" | grep -c 'es_int(w, es, "buildstop"'
}

n=$(count_buildstop_reads "$TOOLS"/*.inc)
if [ "$n" -ne 1 ]; then
	echo "check-madcide-single-owners: FAIL — $n reads of the build stop" \
	     "handle across tools/madcide (expected 1: stop_build)." >&2
	grep -n 'es_int(w, es, "buildstop"' "$TOOLS"/*.inc >&2
	exit 1
fi

# Negative control for the build-stop marker.
tmp=$(mktemp)
echo '    long stopc = es_int(w, es, "buildstop", 0);	// synthetic' > "$tmp"
if [ "$(count_buildstop_reads "$TOOLS"/*.inc "$tmp")" -ne 2 ]; then
	rm -f "$tmp"
	echo "check-madcide-single-owners: FAIL — negative control did not" \
	     "detect a synthetic build-stop read (the marker went blind)." >&2
	exit 1
fi
rm -f "$tmp"

# Where madcide's DATA lives has ONE owner: resolve_data_dir
# (tools/texteditor/editor_events.inc; B85). The install layout's `share/`
# arm is spelled once, inside it, and no data file is read through a
# cwd-relative "tools/..." path: that path is what made save and quit (then
# the line editor's verbs) do nothing outside the repo root, and never in
# the shipped package.
count_share_arms()
{
	cat "$@" | grep -c '/share/{}'
}
count_cwd_data_reads()
{
	cat "$@" | grep -cE '"tools/(madcide|texteditor)/'
}

n=$(count_share_arms "$TOOLS"/*.inc "$TEXTED"/*.inc)
m=$(count_cwd_data_reads "$TOOLS"/*.inc "$TEXTED"/*.inc)
if [ "$n" -ne 1 ] || [ "$m" -ne 0 ]; then
	echo "check-madcide-single-owners: FAIL — $n install-layout share/ arms" \
	     "(expected 1: resolve_data_dir) and $m cwd-relative tools/ data" \
	     "reads (expected 0) across tools/madcide + tools/texteditor. Find" \
	     "data through resolve_data_dir." >&2
	grep -nE '/share/\{\}|"tools/(madcide|texteditor)/' \
	     "$TOOLS"/*.inc "$TEXTED"/*.inc >&2
	exit 1
fi

# Negative control: a synthetic cwd-relative read must trip the marker.
tmp=$(mktemp)
echo '    php::file_get_contents(t, "tools/madcide/profiles/joe.keys");	// synthetic' > "$tmp"
if [ "$(count_cwd_data_reads "$TOOLS"/*.inc "$TEXTED"/*.inc "$tmp")" -ne 1 ]; then
	rm -f "$tmp"
	echo "check-madcide-single-owners: FAIL — negative control did not" \
	     "detect a synthetic cwd-relative data read (the marker went blind)." >&2
	exit 1
fi
rm -f "$tmp"

# Every DOCUMENT is madc's document component, minted in ONE place per way
# in (tools/texteditor/editor_events.inc; B86, split arc 1D): new_document
# (ui::doc_new — a new file, the untitled buffer) and setup_document
# (ui::doc_open — a file read). A view buffer ([repl], [build], [terminal],
# Help, a git view) is no document: new_view_text. A bare
# `ui::create(w, "document")` is the copy that let an unreadable path become
# an empty buffer under its name and a view buffer read a same-named file.
count_document_mints()
{
	cat "$@" | grep -cE 'ui::create\([^,]*, *"document"\)|ui::doc_(new|open)\('
}

DOCSRC=$(ls "$TOOLS"/*.inc "$TOOLS"/*.mad "$TEXTED"/*.inc "$TEXTED"/*.mad 2>/dev/null)
n=$(count_document_mints $DOCSRC)
nnew=$(cat $DOCSRC | grep -c 'ui::doc_new(')
nopen=$(cat $DOCSRC | grep -c 'ui::doc_open(')
if [ "$n" -ne 2 ] || [ "$nnew" -ne 1 ] || [ "$nopen" -ne 1 ]; then
	echo "check-madcide-single-owners: FAIL — $n document mint sites across" \
	     "tools/madcide + tools/texteditor (expected 2: new_document's" \
	     "ui::doc_new, setup_document's ui::doc_open). Open a file through" \
	     "open_buffer_doc / setup_document, a view buffer through" \
	     "new_view_text." >&2
	grep -nE 'ui::create\([^,]*, *"document"\)|ui::doc_(new|open)\(' $DOCSRC >&2
	exit 1
fi

# Negative control: a synthetic third minter must trip the marker.
tmp=$(mktemp)
echo '    long nd = ui::create(w2, "document");	// synthetic' > "$tmp"
if [ "$(count_document_mints $DOCSRC "$tmp")" -ne 3 ]; then
	rm -f "$tmp"
	echo "check-madcide-single-owners: FAIL — negative control did not" \
	     "detect a synthetic document mint (the marker went blind)." >&2
	exit 1
fi
rm -f "$tmp"

# A data file's PATH has ONE owner: bundle_data_path (tools/madcide/
# madcide_plugins.inc; plugins Stage A) — the active bundle's own directory,
# then the shared profiles/. The keys, layout, menu, theme and status loaders
# each formed `<profile_dir>/<name>.<kind>` themselves; a sixth copy would
# read past a bundle's own files. Marker: no `{}/{}.<kind>` path spelled with
# a data kind's extension across tools/madcide + tools/texteditor.
count_data_paths()
{
	cat "$@" | grep -cE '"\{\}/\{\}\.(keys|layout|menu|theme|status)"'
}

n=$(count_data_paths "$TOOLS"/*.inc "$TEXTED"/*.inc)
if [ "$n" -ne 0 ]; then
	echo "check-madcide-single-owners: FAIL — $n data-file paths spelled by" \
	     "hand across tools/madcide + tools/texteditor (expected 0: every" \
	     "loader asks bundle_data_path)." >&2
	grep -nE '"\{\}/\{\}\.(keys|layout|menu|theme|status)"' "$TOOLS"/*.inc "$TEXTED"/*.inc >&2
	exit 1
fi

# Negative control: a synthetic hand-spelled path must trip the marker.
tmp=$(mktemp)
echo '    php::file_get_contents(txt, format("{}/{}.layout", dir, n));	// synthetic' > "$tmp"
if [ "$(count_data_paths "$TOOLS"/*.inc "$TEXTED"/*.inc "$tmp")" -ne 1 ]; then
	rm -f "$tmp"
	echo "check-madcide-single-owners: FAIL — negative control did not" \
	     "detect a synthetic hand-spelled data path (the marker went blind)." >&2
	exit 1
fi
rm -f "$tmp"

# A synthesized command event has ONE builder: action_event (post, command
# and a choice list's row each spelled the literal, and the client's paste
# answer ran a second command call, until plan §41.11a step 4). Marker: the
# event's "action_code" key appears once across tools/madcide.
count_action_events()
{
	cat "$@" | grep -c -E '"action_code"[[:space:]]*:'
}

n=$(count_action_events "$TOOLS"/*.inc)
if [ "$n" -ne 1 ]; then
	echo "check-madcide-single-owners: FAIL — $n action-event literal(s)" \
	     "across tools/madcide (expected 1: action_event, the one builder)." >&2
	grep -n -E '"action_code"[[:space:]]*:' "$TOOLS"/*.inc >&2
	exit 1
fi
tmp=$(mktemp)
cat "$TOOLS"/*.inc > "$tmp"
echo '    var e = { "event": "action", "action_code": code };	// synthetic' >> "$tmp"
if [ "$(count_action_events "$tmp")" -ne 2 ]; then
	rm -f "$tmp"
	echo "check-madcide-single-owners: FAIL — negative control did not" \
	     "detect a synthetic action-event literal (the marker went blind)." >&2
	exit 1
fi
rm -f "$tmp"

# The C / C++ / madc RANGES of <bits/file_kinds> have ONE reader in madcide:
# kind_family (madcide_lang.inc; DupFamily madcide_file_kind_family,
# consolidated 2026-10-03). kind_compiler_range, cxx_view_applies and
# kind_is_standard each spelled the ranges by hand; they ask kind_family now.
# Marker: a range bound (fkC_LAST / fkCPP_LAST / fkMADC_LAST) anywhere in
# tools/madcide or tools/texteditor outside madcide_lang.inc.
count_family_ranges()
{
	grep -lE 'fk(C|CPP|MADC)_LAST' "$@" | grep -vc '/madcide_lang\.inc$'
}

n=$(count_family_ranges "$TOOLS"/*.inc "$TOOLS"/*.mad "$TEXTED"/*.inc)
if [ "$n" -ne 0 ]; then
	echo "check-madcide-single-owners: FAIL — $n file(s) read the file-kind" \
	     "family ranges outside madcide_lang.inc (ask kind_family):" >&2
	grep -nE 'fk(C|CPP|MADC)_LAST' "$TOOLS"/*.inc "$TOOLS"/*.mad "$TEXTED"/*.inc \
		| grep -v '/madcide_lang\.inc:' >&2
	exit 1
fi
tmpd=$(mktemp -d)
echo '    return k >= madc::fkC && k <= madc::fkC_LAST;	// synthetic' > "$tmpd/synthetic.inc"
if [ "$(count_family_ranges "$TOOLS"/*.inc "$tmpd/synthetic.inc")" -ne 1 ]; then
	rm -rf "$tmpd"
	echo "check-madcide-single-owners: FAIL — negative control did not" \
	     "detect a synthetic family range read (the marker went blind)." >&2
	exit 1
fi
rm -rf "$tmpd"

# The plugin registry's "active" rows have ONE writer: plugin_record_active
# (madcide_plugin_code.inc). The library/source, builtin and host arms each
# appended the row by hand until the product's linked arm (D3) would have
# been a fourth copy. Marker: one `"active", ` write across tools/madcide.
count_active_writes()
{
	cat "$@" | grep -c 'ui::set(w, reg, "active", '
}

n=$(count_active_writes "$TOOLS"/*.inc)
if [ "$n" -ne 1 ]; then
	echo "check-madcide-single-owners: FAIL — $n writer(s) of the plugin" \
	     "registry's \"active\" rows across tools/madcide (expected 1:" \
	     "plugin_record_active):" >&2
	grep -n 'ui::set(w, reg, "active", ' "$TOOLS"/*.inc >&2
	exit 1
fi
tmpd=$(mktemp -d)
echo '    ui::set(w, reg, "active", active);	// synthetic' > "$tmpd/synthetic.inc"
if [ "$(count_active_writes "$TOOLS"/*.inc "$tmpd/synthetic.inc")" -ne 2 ]; then
	rm -rf "$tmpd"
	echo "check-madcide-single-owners: FAIL — negative control did not" \
	     "detect a synthetic active-row write (the marker went blind)." >&2
	exit 1
fi
rm -rf "$tmpd"

echo "check-madcide-single-owners: OK (one data-location owner: resolve_data_dir;" \
     "one data-file path: bundle_data_path; one action-event builder: action_event;" \
     "one document minter: new_document; one fresh-row owner: push_buffer_row;" \
     "one pause owner: terminal_return_pause; one text-mutation owner pair:" \
     "ed_text_insert/ed_text_erase; one record-kind reader per layer; one" \
     "validator seat: graph_edit_apply; one file-kind family reader: kind_family;" \
     "one active-plugin row writer: plugin_record_active)"
exit 0
