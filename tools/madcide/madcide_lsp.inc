// madcide_lsp.inc — THE LSP FACE (client-server V6c-2; plan
// docs/plans/2026-09-14-v6c2-lsp-adapter-plan.md, design
// 2026-09-09-nexus-client-server-design.md §2.7/§2.8).
//
// `madcide <file> --lsp` is a Language Server over stdio: the standard
// deployment, where the editor SPAWNS the server and speaks JSON-RPC 2.0
// framed by Content-Length. It is the second face of the api/query layer the
// MCP seat already serves — developers keep the editor they have, and the
// arc's claim that the api is client-general gets its acceptance test.
//
// What this file does NOT contain is the point: no analysis. Every answer is
// an EXISTING projection (madc::parse_check / parse_outline / parse_spans /
// parse_enclosing) or an existing graph verb (graph_definition /
// graph_references) — the same reads graph_call serves the MCP seat, run
// after the same layer check (L4e). Diagnostics refresh through the ONE
// compiler entry (refresh_diags) and every text change rides the ONE
// text-mutation owner (ed_text_insert / ed_text_erase), so an edit arriving
// over LSP appears in an open TUI window and lands in the project stream like
// any other.
//
// Rulings carried here:
//   - the position ENCODING is negotiated at initialize and stored on the bag
//     (`penc`); LSP's default is UTF-16 code units, and reading one as a byte
//     offset silently mis-places every position on a line with a non-ASCII
//     character before it;
//   - a URI resolves to a document through edit_file — the ^K E open-or-
//     switch owner — with canonical path comparison, so one file is one
//     buffer however the client spells it;
//   - a NOTIFICATION never gets a reply, even when it is refused (a reply to
//     one can abort a client's session); its refusal goes to stderr, which is
//     not the protocol stream;
//   - the reparse on open/change/save is SYNCHRONOUS — the cost the editor
//     pays on ^K ;. A "reparse only when it looks cheap" rule would be a
//     hard-coded specific; the background-parse path is the named follow-up.
//
// Depends on madcide_core.inc (the session), madcide_api.inc (session_state)
// and madcide_mcp.inc (the tier gate's eff_tier, the layer owner). Included
// after all three.

// ---- JSON-RPC envelopes ------------------------------------------------

void lsp_reply(var &out, var &id, var &result)
{
    out = { "jsonrpc": "2.0", "id": id, "result": result };
}

void lsp_error(var &out, var &id, long code, const char *message)
{
    var err = { "code": code, "message": message };
    out = { "jsonrpc": "2.0", "id": id, "error": err };
}

// A server -> client NOTIFICATION (publishDiagnostics today). It carries no
// id: the client must not answer it.
void lsp_notify(var &out, const char *method, var &params)
{
    out = { "jsonrpc": "2.0", "method": method, "params": params };
}

// ---- file:// URIs ------------------------------------------------------
// A URI is text at an input boundary: decoded once here, canonicalised, and
// carried as a path afterwards. Only the `file` scheme is served — a client
// that opens an untitled: or vscode-notebook-cell: document gets a refusal
// naming the scheme, never a silent miss.

bool lsp_uri_path(var &out, var &uri)
{
    if ( uri.is_null() )
	return false;
    var u = uri;
    const char *s = u.c_str();
    if ( strncmp(s, "file://", 7) != 0 )
	return false;
    var raw = js::decodeURIComponent(s + 7);	// %20 and friends; '/' passes through
    madc::canonical_path(out, raw.c_str());
    return true;
}

// The inverse, for every Location the server reports. encodeURIComponent
// escapes '/' too, so the path is encoded per SEGMENT — one join, never a
// pass that would turn the separators into %2F.
void lsp_path_uri(var &out, var &path)
{
    var abs;
    madc::canonical_path(abs, path.c_str());
    array parts;
    php::explode(parts, "/", abs.c_str());
    array enc;
    long n = php::count(parts);
    for ( long i = 0; i < n; i = i + 1 )
    {
	var seg = php::array_get_cstr(parts, i);
	var e = js::encodeURIComponent(seg.c_str());
	enc[] = e;
    }
    var joined = php::implode("/", enc);
    out = format("file://{}", joined);
}

// ---- positions ---------------------------------------------------------
// LSP positions are 0-based lines and 0-based columns in the NEGOTIATED
// encoding; madcide's text is byte offsets and 1-based lines. Both directions
// go through ui::text_bytecol / ui::text_col16 (madcdis/text_utf16.h is the
// one owner) when the encoding is utf-16, and are identity when it is utf-8.

long lsp_offset_of(long w, long doc, long enc, var &pos)
{
    if ( pos.is_null() )
	return 0;
    long line = pos["line"].as_integer() + 1;		// 0-based on the wire
    long ch = pos["character"].as_integer();
    long start = ui::text_line_start(w, doc, line);
    if ( start < 0 )
	return ui::text_size(w, doc);			// past the last line
    long len = ui::text_line_len(w, doc, line);
    long b = ch;
    if ( enc == peUTF16 )
	b = ui::text_bytecol(w, doc, line, ch);
    if ( b < 0 )
	b = 0;
    if ( b > len )
	b = len;
    return start + b;
}

void lsp_position_of(var &out, long w, long doc, long enc, long off)
{
    long line = ui::text_line_of(w, doc, off);
    if ( line < 1 )
	line = 1;
    long start = ui::text_line_start(w, doc, line);
    if ( start < 0 )
	start = 0;
    long b = off - start;
    if ( b < 0 )
	b = 0;
    long ch = b;
    if ( enc == peUTF16 )
    {
	long c16 = ui::text_col16(w, doc, line, b);
	ch = c16 < 0 ? b : c16;
    }
    long l0 = line - 1;
    out = { "line": l0, "character": ch };
}

// The range two byte offsets name.
void lsp_range_of(var &out, long w, long doc, long enc, long from, long to)
{
    var s;
    lsp_position_of(s, w, doc, enc, from);
    var e;
    lsp_position_of(e, w, doc, enc, to);
    out = { "start": s, "end": e };
}

// The range a 1-based LINE spans, whole (a diagnostic with no usable column,
// a symbol's body).
void lsp_line_range(var &out, long w, long doc, long enc, long from_line, long to_line)
{
    long s = ui::text_line_start(w, doc, from_line);
    if ( s < 0 )
	s = 0;
    long e = ui::text_line_start(w, doc, to_line);
    long elen = ui::text_line_len(w, doc, to_line);
    if ( e < 0 )
    {
	e = s;
	elen = 0;
    }
    lsp_range_of(out, w, doc, enc, s, e + elen);
}

// ---- the document a request names --------------------------------------
// The buffers table IS the uri -> document map (edit_file compares paths
// canonically), so there is no second map to drift. 0 = no usable URI.

long lsp_doc_for(IdeSession &S, long doc, var &tdoc)
{
    if ( tdoc.is_null() )
	return 0;
    var uri = tdoc["uri"];
    var path;
    if ( !lsp_uri_path(path, uri) )
	return 0;
    long w = S.world();
    long es = S.es;
    long cur = S.active(doc);
    var curp;
    ui::get(curp, w, cur, "path");
    var curc;
    madc::canonical_path(curc, curp.is_null() ? "" : curp.c_str());
    if ( curc == path )
	return cur;					// already the active buffer
    edit_file(w, es, cur, path.c_str());
    return S.active(doc);
}

// The document's own URI, for every Location the server reports.
void lsp_doc_uri(var &out, long w, long doc)
{
    var p;
    ui::get(p, w, doc, "path");
    var path = "";
    if ( !p.is_null() )
	path = p;
    lsp_path_uri(out, path);
}

// ---- the identifier under a position ------------------------------------
// The ONE word owner answers it (ui::text_word_left / text_word_right — a
// word byte is [A-Za-z0-9_]). The composition is validated, not trusted: past
// the last word on a line word_right clamps to the end of the text and
// word_left then spans junk, so the candidate counts only when it is a real
// word containing the offset. A caret resting just PAST an identifier (where
// an editor leaves it) retries one byte back.

bool lsp_word_span(long &from, long &to, long w, long doc, long off)
{
    long e = ui::text_word_right(w, doc, off);
    if ( e <= 0 )
	return false;
    long s = ui::text_word_left(w, doc, e);
    if ( s < 0 || s >= e )
	return false;
    if ( ui::text_word_right(w, doc, s) != e )
	return false;					// not one whole word
    if ( s > off || off > e )
	return false;					// the offset is not in it
    from = s;
    to = e;
    return true;
}

bool lsp_word_at(var &out, long &from, long &to, long w, long doc, long off)
{
    if ( !lsp_word_span(from, to, w, doc, off) )
    {
	if ( off <= 0 || !lsp_word_span(from, to, w, doc, off - 1) )
	    return false;				// the caret just past a word
    }
    var t;
    ui::text(t, w, doc);
    var word = perl::substr(t, from, to - from);
    out = word;
    return strlen(word.c_str()) > 0;
}

// ---- diagnostics --------------------------------------------------------
// parse_check rows carry a 1-based line and an END-anchored 1-based column
// (the span rows are start-anchored — the two conventions differ, and the
// engine's header says so). The range therefore ends AT the column and starts
// at the beginning of the word ending there; a column that names no word
// (a call's paren, say) falls back to the single character before it.

void lsp_diag_range(var &out, long w, long doc, long enc, var &row)
{
    long line = row["line"].as_integer();
    if ( line < 1 )
	line = 1;
    long lstart = ui::text_line_start(w, doc, line);
    if ( lstart < 0 )
    {
	lsp_line_range(out, w, doc, enc, 1, 1);
	return;
    }
    long llen = ui::text_line_len(w, doc, line);
    long col = row["column"].as_integer();		// 1-based, end-anchored
    if ( col < 1 || col > llen )
    {
	lsp_range_of(out, w, doc, enc, lstart, lstart + llen);
	return;						// no usable column: the line
    }
    long endoff = lstart + col;
    long s = ui::text_word_left(w, doc, endoff);
    if ( s < lstart || s >= endoff )
	s = endoff > lstart ? endoff - 1 : lstart;
    lsp_range_of(out, w, doc, enc, s, endoff);
}

// The severity as the protocol's number, from the row's ENUMERATOR (never its
// display word): error 1, warning 2, anything else 3 (Information).
long lsp_severity_of(var &row)
{
    var code = row["severity_code"];
    if ( code == madc::diag_severity::error )
	return 1;
    if ( code == madc::diag_severity::warning )
	return 2;
    return 3;
}

// The whole publishDiagnostics notification for `adoc`, appended to `notes`.
// Reparses through the ONE compiler entry first (refresh_diags), so what the
// client sees describes the text it just sent.
void lsp_publish(var &notes, IdeSession &S, long adoc)
{
    long w = S.world();
    long es = S.es;
    long enc = es_int(w, es, "penc", peUTF16);
    refresh_diags(w, es, adoc);
    var rows;
    ui::get(rows, w, es, "diags");
    var items = {};
    long n = rows.is_null() ? 0 : php::count(rows);
    for ( long i = 0; i < n; i = i + 1 )
    {
	var row = rows[i];
	var range;
	lsp_diag_range(range, w, adoc, enc, row);
	long sev = lsp_severity_of(row);
	var msg = row["message"];
	var item = { "range": range, "severity": sev, "source": "madc",
		     "message": msg };
	items[] = item;
    }
    var uri;
    lsp_doc_uri(uri, w, adoc);
    var params = { "uri": uri, "diagnostics": items };
    var note;
    lsp_notify(note, "textDocument/publishDiagnostics", params);
    notes[] = note;
}

// ---- the answering methods ----------------------------------------------

// documentSymbol <- parse_outline. A row's kind converts through
// lsp_symbol_of/lsp_symbol_number; `range` is the definition's whole extent,
// `selectionRange` the name's own line.
void lsp_document_symbol(var &out, IdeSession &S, long adoc, long h)
{
    long w = S.world();
    long enc = es_int(w, S.es, "penc", peUTF16);
    var rows;
    madc::parse_outline(rows, h);
    var syms = {};
    long n = php::count(rows);
    for ( long i = 0; i < n; i = i + 1 )
    {
	var row = rows[i];
	long line = row["line"].as_integer();
	long endl = row["end_line"].as_integer();
	if ( endl < line )
	    endl = line;
	var range;
	lsp_line_range(range, w, adoc, enc, line, endl);
	var sel;
	lsp_line_range(sel, w, adoc, enc, line, line);
	var kindw = row["kind"];
	long kind = lsp_symbol_number(lsp_symbol_of(kindw));
	var nm = row["name"];
	var sym = { "name": nm, "kind": kind, "range": range,
		    "selectionRange": sel };
	syms[] = sym;
    }
    out = syms;
}

// A definition node's Location: its span, in this document.
void lsp_node_location(var &out, long w, long adoc, long enc, var &node)
{
    long line = node["line"].as_integer();
    long endl = node["end_line"].as_integer();
    if ( endl < line )
	endl = line;
    var range;
    lsp_line_range(range, w, adoc, enc, line, endl);
    var uri;
    lsp_doc_uri(uri, w, adoc);
    out = { "uri": uri, "range": range };
}

// definition <- graph_definition on the identifier under the position. An
// unknown word answers an EMPTY list, never an error: "nothing is defined
// here" is an answer, and a client shows it as "no definition found".
void lsp_definition(var &out, IdeSession &S, long adoc, long h, var &params)
{
    long w = S.world();
    long enc = es_int(w, S.es, "penc", peUTF16);
    var pos = params["position"];
    long off = lsp_offset_of(w, adoc, enc, pos);
    var word;
    long from = 0;
    long to = 0;
    var locs = {};
    if ( !lsp_word_at(word, from, to, w, adoc, off) )
    {
	out = locs;
	return;
    }
    var snap;
    madc::graph_definition(snap, h, word.c_str());
    var nodes = snap["nodes"];
    long n = nodes.is_null() ? 0 : php::count(nodes);
    for ( long i = 0; i < n; i = i + 1 )
    {
	var node = nodes[i];
	var loc;
	lsp_node_location(loc, w, adoc, enc, node);
	locs[] = loc;
    }
    out = locs;
}

// references <- graph_references over the definition's id. The answer's EDGES
// carry the use sites (`from` names a Call node in `nodes`, the dialect's
// standing trap: the subject rides in `nodes` too). A Call node's column
// anchors at its argument list, so the reported range is the identifier the
// word owner finds immediately before it.
void lsp_node_by_id(var &out, var &nodes, long id)
{
    long n = nodes.is_null() ? 0 : php::count(nodes);
    for ( long i = 0; i < n; i = i + 1 )
    {
	var node = nodes[i];
	if ( node["id"].as_integer() == id )
	{
	    out = node;
	    return;
	}
    }
}

void lsp_references(var &out, IdeSession &S, long adoc, long h, var &params)
{
    long w = S.world();
    long enc = es_int(w, S.es, "penc", peUTF16);
    var pos = params["position"];
    long off = lsp_offset_of(w, adoc, enc, pos);
    var word;
    long from = 0;
    long to = 0;
    var locs = {};
    if ( !lsp_word_at(word, from, to, w, adoc, off) )
    {
	out = locs;
	return;
    }
    var def;
    madc::graph_definition(def, h, word.c_str());
    var dnodes = def["nodes"];
    if ( dnodes.is_null() || php::count(dnodes) == 0 )
    {
	out = locs;
	return;
    }
    var d0 = dnodes[0];
    long did = d0["id"].as_integer();
    var snap;
    madc::graph_references(snap, h, did);
    var nodes = snap["nodes"];
    var edges = snap["edges"];
    long n = edges.is_null() ? 0 : php::count(edges);
    var uri;
    lsp_doc_uri(uri, w, adoc);
    for ( long i = 0; i < n; i = i + 1 )
    {
	var edge = edges[i];
	var site;
	lsp_node_by_id(site, nodes, edge["from"].as_integer());
	if ( site.is_null() )
	    continue;
	long line = site["line"].as_integer();
	long lstart = ui::text_line_start(w, adoc, line);
	if ( lstart < 0 )
	    continue;
	long at = lstart + site["column"].as_integer();
	long s = 0;
	long e = 0;
	var got;
	var range;
	if ( lsp_word_at(got, s, e, w, adoc, at) )
	    lsp_range_of(range, w, adoc, enc, s, e);
	else
	    lsp_line_range(range, w, adoc, enc, line, line);
	var loc = { "uri": uri, "range": range };
	locs[] = loc;
    }
    // The client asked whether the DEFINITION counts as a reference.
    var ctx = params["context"];
    bool want_decl = false;
    if ( !ctx.is_null() )
	want_decl = ctx["includeDeclaration"].as_boolean();
    if ( want_decl )
    {
	var dloc;
	lsp_node_location(dloc, w, adoc, enc, d0);
	locs[] = dloc;
    }
    out = locs;
}

// hover <- the graph for what the word IS, with parse_enclosing as the
// fallback when it resolves to nothing (the status line's own query).
void lsp_hover(var &out, IdeSession &S, long adoc, long h, var &params)
{
    long w = S.world();
    long enc = es_int(w, S.es, "penc", peUTF16);
    var pos = params["position"];
    long off = lsp_offset_of(w, adoc, enc, pos);
    var word;
    long from = 0;
    long to = 0;
    var text;
    var range;
    bool has_range = false;
    if ( lsp_word_at(word, from, to, w, adoc, off) )
    {
	var snap;
	madc::graph_definition(snap, h, word.c_str());
	var nodes = snap["nodes"];
	if ( !nodes.is_null() && php::count(nodes) > 0 )
	{
	    var n0 = nodes[0];
	    var kind = n0["kind"];
	    var nm = n0["name"];
	    long line = n0["line"].as_integer();
	    text = format("{} {} — defined on line {}", kind, nm, line);
	    lsp_range_of(range, w, adoc, enc, from, to);
	    has_range = true;
	}
    }
    if ( text.is_null() )
    {
	long line = pos.is_null() ? 1 : pos["line"].as_integer() + 1;
	long column = 1;
	long lstart = ui::text_line_start(w, adoc, line);
	if ( lstart >= 0 )
	    column = off - lstart + 1;
	var enc_fn;
	madc::parse_enclosing(enc_fn, h, line, column);
	var nm = enc_fn["name"];
	if ( nm.is_null() )
	{
	    var none;
	    out = none;					// JSON null: no hover here
	    return;
	}
	text = format("in {}", nm);
    }
    var contents = { "kind": "plaintext", "value": text };
    if ( has_range )
	out = { "contents": contents, "range": range };
    else
	out = { "contents": contents };
}

// semanticTokens/full <- parse_spans, delta-encoded per the spec: five
// integers per token (deltaLine, deltaStartChar, length, tokenType,
// tokenModifiers), each delta relative to the previous token. The LEGEND is
// lsp_token_type in order; the theme is not consulted.
void lsp_token_legend(var &out)
{
    var types = {};
    for ( long k = lttKEYWORD; k <= lttFUNCTION; k = k + 1 )
    {
	var nm = lsp_token_name(k);
	types[] = nm;
    }
    var mods = {};
    out = { "tokenTypes": types, "tokenModifiers": mods };
}

void lsp_semantic_tokens(var &out, IdeSession &S, long adoc, long h)
{
    long w = S.world();
    long enc = es_int(w, S.es, "penc", peUTF16);
    var rows;
    madc::parse_spans(rows, h);
    var data = {};
    long prev_line = 0;
    long prev_char = 0;
    long n = php::count(rows);
    for ( long i = 0; i < n; i = i + 1 )
    {
	var row = rows[i];
	var cls = row["class"];
	long type = lsp_token_of_class(cls);
	if ( type == lttNONE )
	    continue;					// a class with no token type
	long line = row["line"].as_integer();
	long bcol = row["column"].as_integer();		// 0-based START byte
	long blen = row["length"].as_integer();
	if ( line < 1 || blen <= 0 )
	    continue;
	long lstart = ui::text_line_start(w, adoc, line);
	if ( lstart < 0 )
	    continue;					// a line the text no longer has
	long col = bcol;
	long len = blen;
	if ( enc == peUTF16 )
	{
	    long c0 = ui::text_col16(w, adoc, line, bcol);
	    long c1 = ui::text_col16(w, adoc, line, bcol + blen);
	    if ( c0 >= 0 && c1 >= c0 )
	    {
		col = c0;
		len = c1 - c0;
	    }
	}
	long l0 = line - 1;
	long dline = l0 - prev_line;
	long dchar = dline == 0 ? col - prev_char : col;
	// The protocol requires source order; a row that would encode a
	// negative delta is dropped rather than corrupting every token after
	// it (the rows arrive in order — this is the belt).
	if ( dline < 0 || dchar < 0 )
	    continue;
	long type_index = type - lttKEYWORD;
	data[] = dline;
	data[] = dchar;
	data[] = len;
	data[] = type_index;
	data[] = 0;					// no modifiers in v1
	prev_line = l0;
	prev_char = col;
    }
    out = { "data": data };
}

// ---- the server's capabilities -----------------------------------------
// The position ENCODING is the one real negotiation: a client offering utf-8
// saves every conversion, a client that does not gets the protocol's utf-16
// default and the honest converter.

long lsp_negotiate_encoding(var &params)
{
    if ( params.is_null() )
	return peUTF16;
    var general = params["general"];
    if ( general.is_null() )
	return peUTF16;
    var offers = general["positionEncodings"];
    if ( offers.is_null() )
	return peUTF16;
    long n = php::count(offers);
    for ( long i = 0; i < n; i = i + 1 )
    {
	var word = offers[i];
	if ( pos_encoding_of(word) == peUTF8 )
	    return peUTF8;				// cheapest, and offered
    }
    return peUTF16;
}

void lsp_capabilities(var &out, long enc)
{
    long incremental = lsp_sync_number(lsINCREMENTAL);
    var save = { "includeText": false };
    var sync = { "openClose": true, "change": incremental, "save": save };
    var legend;
    lsp_token_legend(legend);
    var semantic = { "legend": legend, "full": true };
    var encoding = pos_encoding_name(enc);
    out = { "positionEncoding": encoding, "textDocumentSync": sync,
	    "hoverProvider": true, "definitionProvider": true,
	    "referencesProvider": true, "documentSymbolProvider": true,
	    "semanticTokensProvider": semantic };
}

// ---- document synchronisation ------------------------------------------

void lsp_did_open(var &notes, IdeSession &S, long doc, var &params)
{
    var tdoc = params["textDocument"];
    long adoc = lsp_doc_for(S, doc, tdoc);
    if ( adoc == 0 )
	return;
    var text = tdoc["text"];
    if ( !text.is_null() )
	ui::text_load(S.world(), adoc, text.c_str());	// a LOAD, not an edit
    lsp_publish(notes, S, adoc);
}

// Every change in order, each against the text the previous one left. A
// change WITH a range is a splice through the one text-mutation owner (so the
// project stream and every other client see it); one without is the whole
// document, which is a load.
void lsp_did_change(var &notes, IdeSession &S, long doc, var &params)
{
    var tdoc = params["textDocument"];
    long adoc = lsp_doc_for(S, doc, tdoc);
    if ( adoc == 0 )
	return;
    long w = S.world();
    long es = S.es;
    long enc = es_int(w, es, "penc", peUTF16);
    var changes = params["contentChanges"];
    long n = changes.is_null() ? 0 : php::count(changes);
    for ( long i = 0; i < n; i = i + 1 )
    {
	var ch = changes[i];
	var text = ch["text"];
	var range = ch["range"];
	if ( range.is_null() )
	{
	    ui::text_load(w, adoc, text.is_null() ? "" : text.c_str());
	    continue;
	}
	var from = range["start"];
	var to = range["end"];
	long s = lsp_offset_of(w, adoc, enc, from);
	long e = lsp_offset_of(w, adoc, enc, to);
	if ( e < s )
	    e = s;
	if ( e > s )
	    ed_text_erase(w, es, adoc, s, e - s);
	if ( !text.is_null() && strlen(text.c_str()) > 0 )
	    ed_text_insert(w, es, adoc, s, text.c_str());
    }
    lsp_publish(notes, S, adoc);
}

void lsp_did_save(var &notes, IdeSession &S, long doc, var &params)
{
    var tdoc = params["textDocument"];
    long adoc = lsp_doc_for(S, doc, tdoc);
    if ( adoc == 0 )
	return;
    lsp_publish(notes, S, adoc);
}

// initialize: negotiate the encoding, store it, answer the capability set.
void lsp_initialize(var &out, IdeSession &S, var &idv, var &params)
{
    long enc = lsp_negotiate_encoding(params);
    ui::set(S.world(), S.es, "penc", enc);
    var caps;
    lsp_capabilities(caps, enc);
    var info = { "name": "madcide", "version": "1.0.0" };
    var result = { "capabilities": caps, "serverInfo": info };
    lsp_reply(out, idv, result);
}

// shutdown: the client asks the server to stop serving; `exit` follows. The
// result is JSON null, and whether shutdown came at all is the exit STATUS
// the spec defines (0 after a shutdown, 1 without one).
void lsp_shutdown(var &out, IdeSession &S, var &idv)
{
    ui::set(S.world(), S.es, "lspdown", 1);
    var nothing;					// JSON null, per the spec
    lsp_reply(out, idv, nothing);
}

// ---- the dispatcher -----------------------------------------------------
// One message in, at most one reply out, plus any notifications the work
// produced. `notes` is the queue the loop drains after the reply — the same
// shape the in-process test drives, so the gate sees exactly what a client
// would receive.

bool lsp_handle(IdeSession &S, long doc, long self_id, var &req, var &out, var &notes)
{
    long w = S.world();
    long es = S.es;
    var idv = req["id"];
    var method = req["method"];
    long m = lsp_method_of(method);
    bool wants_reply = !idv.is_null();
    if ( m == lmNONE )
    {
	if ( !wants_reply )
	{
	    println(stderr, "madcide --lsp: ignoring unknown notification {}", method);
	    return false;				// never answer a notification
	}
	var why = format("Method not found: {}", method);
	lsp_error(out, idv, -32601, why.c_str());
	return true;
    }
    if ( lsp_is_notification(m) && wants_reply )
    {
	// A client that put an id on a notification gets told, rather than
	// waiting forever for a reply the protocol says never comes.
	var why = format("{} is a notification and takes no id", method);
	lsp_error(out, idv, -32600, why.c_str());
	return true;
    }
    var params = req["params"];
    switch ( m )
    {
    case lmINITIALIZE:
	lsp_initialize(out, S, idv, params);
	return true;
    case lmINITIALIZED:
    case lmCANCEL:
	return false;					// nothing to do, nothing to say
    case lmSHUTDOWN:
	lsp_shutdown(out, S, idv);
	return true;
    case lmEXIT:
	ui::set(w, es, "lspexit", 1);
	return false;
    case lmDIDOPEN:
	lsp_did_open(notes, S, doc, params);
	return false;
    case lmDIDCHANGE:
	lsp_did_change(notes, S, doc, params);
	return false;
    case lmDIDSAVE:
	lsp_did_save(notes, S, doc, params);
	return false;
    case lmDIDCLOSE:
	// The client stopped tracking the file; madcide does NOT close the
	// buffer — other clients may be viewing it, and the session owns its
	// documents. Nothing to publish either.
	return false;
    }

    // What is left are the READS, and they all need the document, its layers
    // and its parse handle — resolved once, refused once.
    var tdoc;
    if ( !params.is_null() )
	tdoc = params["textDocument"];
    long adoc = lsp_doc_for(S, doc, tdoc);
    if ( adoc == 0 )
    {
	var why = format("{}: no readable file:// document in the request", method);
	lsp_error(out, idv, -32602, why.c_str());
	return true;
    }
    // The L4e layer gate, by the same owner the graph / nexus / test families
    // use: a document with no parseable layer has no answers to give, and the
    // parse handle must never open on one.
    long have = asset_layers_of(w, es, adoc, 0);
    if ( (have & alPARSEABLE) == 0 )
    {
	var words;
	layer_words(words, have);
	var joined = php::implode(", ", words);
	var why = format("{}: this document has no parseable layer (it has: {})",
			 method, joined);
	lsp_error(out, idv, -32803, why.c_str());	// RequestFailed
	return true;
    }
    long h = ensure_phandle(w, es, adoc);
    var result;
    switch ( m )
    {
    case lmDOCUMENTSYMBOL: lsp_document_symbol(result, S, adoc, h); break;
    case lmHOVER:          lsp_hover(result, S, adoc, h, params); break;
    case lmDEFINITION:     lsp_definition(result, S, adoc, h, params); break;
    case lmREFERENCES:     lsp_references(result, S, adoc, h, params); break;
    case lmSEMANTICTOKENS: lsp_semantic_tokens(result, S, adoc, h); break;
    }
    lsp_reply(out, idv, result);
    return true;
}

// ---- the server ---------------------------------------------------------
// `madcide <file> --lsp`: the standard deployment — an editor spawns this
// process and speaks over the pipe. stdio is a CHANNEL (stdio://) and the
// framing is the channel's (frame_headers), so this loop is the MCP seat's
// loop with one framing swapped. The lone stdio client is the operator:
// self_id -1 (untracked -> owner via eff_tier), so it may edit and save.

long run_lsp(const char *path, bool ro)
{
    IdeSession S;
    long doc = S.open(path, ro);
    if ( doc == 0 )
	println(stderr, "madcide: cannot read {}", path);
    if ( doc <= 0 )
	return 1;
    S.terminal(false);				// headless: no interactive terminal
    long w = S.world();
    long es = S.es;
    long enc0 = peUTF16;			// an ENUMERATOR into a slot goes
    ui::set(w, es, "penc", enc0);		// through a long (the dialect's rule)

    madc::channel io("stdio://");
    if ( !io.ok() || !io.frame_headers() )
    {
	println(stderr, "madcide --lsp: {}", io.last_error());
	S.close();
	return 1;
    }
    var line;
    while ( io.readline(line) )
    {
	var req;
	var resp;
	var notes = {};
	if ( !js::parse(req, line.c_str()) )
	{
	    var nullid;
	    lsp_error(resp, nullid, -32700, "Parse error");
	    var js = js::stringify(resp);
	    io.write(js.c_str());
	    continue;
	}
	if ( lsp_handle(S, doc, -1, req, resp, notes) )
	{
	    var js = js::stringify(resp);
	    io.write(js.c_str());
	}
	long n = php::count(notes);
	for ( long i = 0; i < n; i = i + 1 )
	{
	    var note = notes[i];
	    var js = js::stringify(note);
	    io.write(js.c_str());
	}
	if ( es_int(w, es, "lspexit", 0) != 0 )
	    break;				// the client said exit
    }
    // The spec's own verdict: exit after a shutdown is success, without one
    // is an error the client reports.
    long rc = es_int(w, es, "lspdown", 0) != 0 ? 0 : 1;
    io.close();
    S.close();
    return rc;
}
