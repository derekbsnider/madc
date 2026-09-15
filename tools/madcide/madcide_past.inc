// madcide_past.inc — the PAST axis of the code-graph MCP (Nexus L4b; design
// docs/plans/2026-09-13-nexus-L4-design.md §4.3; plan
// 2026-09-13-nexus-L4b-past-verbs-plan.md). Reads over the L4a git substrate
// (madc::git_*) and the graph reads; owns the per-document revision cache
// (sha -> a READ-ONLY tagged parse handle + the text entity that handle was
// parsed from), the id/rev routing graph_call uses, and the six verb bodies.
// Included by madcide_mcp.inc.
//
// Which structure answers what (the index-is-not-the-graph law):
//   status   — the doc bag (phandle_text), parse_check, clog_head, git head/dirty,
//              the layer owner (asset_layers_of) and this node's offers (L4e)
//   source   — graph_span + the handle's TEXT entity (the live doc or a revision's)
//   history  — GIT: git_blame_text over the live text folded to commits;
//              EVENT: nodeop records with the node's kind+name, splices that
//              intersected the span (walked newest->oldest, inverse anchor shift)
//   commits  — the repository's log filtered by the served file
//   revision — git_show at the sha -> parse_open_tagged (the same engine)
//   diff     — graph_symbols of two handles keyed by kind+name; changed = text differs
//
// Thread contract: every cache here is per-document session-thread state like
// the live parse handle (design §7); nothing is persisted (no-program-cache law).

// The doc's git handle (0 = no repository above it) and its working-tree-
// relative path, opened once and kept on the doc bag ("ghandle", "grelpath").
long git_handle_of(long w, long es, long adoc)
{
    long gh = es_int(w, adoc, "ghandle", -1);
    if ( gh >= 0 )
	return gh;
    var p;
    ui::get(p, w, adoc, "path");
    var dir = php::dirname(p);		// ring: own the text first
    gh = madc::git_open(dir.c_str());
    ui::set(w, adoc, "ghandle", gh);
    if ( gh > 0 )
    {
	var rp;
	madc::git_relpath(rp, gh, p.c_str());
	var rel = rp["path"];
	if ( rel.is_null() )
	    rel = "";
	ui::set(w, adoc, "grelpath", rel);
    }
    return gh;
}

void git_relpath_of(var &out, long w, long adoc)
{
    ui::get(out, w, adoc, "grelpath");
    if ( out.is_null() )
	out = "";
}

// (keyed_get — the one keyed READ of a map that may still be null / the empty
// container — lives in editor_events.inc since Nexus L4c; the stream's
// compaction needs it one layer down.)

// ---- the revision cache: sha -> {sha, tag, handle, ent}, LRU-bounded ---------
// A revision's text lives in its own text ENTITY so line_start_of and every
// offset helper work unchanged (the replay-buffer precedent). "revs" is the
// map (an OBJECT keyed by sha — a null var becomes one on its first keyed
// write; the `{}` literal is an empty ARRAY and refuses keys), "rev_order"
// the recency list (oldest first).
void revs_of(var &out, long w, long adoc)
{
    ui::get(out, w, adoc, "revs");		// null until the first revision opens
}

void rev_order_of(var &out, long w, long adoc)
{
    ui::get(out, w, adoc, "rev_order");
    if ( out.is_null() )
	out = {};
}

// Resolve `spec` (a rev-parse spec or a sha) to the revision's entry, opening
// it on a miss and evicting the least-recently-used entry past REV_HANDLES_MAX.
// out = {sha, tag, handle, ent} or {error}.
void rev_handle_of(var &out, long w, long es, long adoc, const char *spec0)
{
    var spec = spec0;			// ring discipline
    long gh = git_handle_of(w, es, adoc);
    if ( gh <= 0 )
    {
	out = { "error": "no repository above the served file" };
	return;
    }
    var rp;
    madc::git_revparse(rp, gh, spec.c_str());
    var sha = rp["sha"];
    if ( sha.is_null() )
    {
	out = { "error": rp["error"] };
	return;
    }
    var revs;
    revs_of(revs, w, adoc);
    var order;
    rev_order_of(order, w, adoc);
    var hit;
    keyed_get(hit, revs, sha.c_str());
    if ( !hit.is_null() )
    {
	// refresh recency: move the sha to the end of the order
	var reordered = {};
	for ( var s : order )
	    if ( !(s == sha) )
		reordered.push(s);
	reordered.push(sha);
	ui::set(w, adoc, "rev_order", reordered);
	out = hit;
	return;
    }
    var rel;
    git_relpath_of(rel, w, adoc);
    var blob;
    madc::git_show(blob, gh, sha.c_str(), rel.c_str());
    var text = blob["text"];
    if ( text.is_null() )
    {
	out = { "error": blob["error"] };
	return;
    }
    var p;
    ui::get(p, w, adoc, "path");
    long h = madc::parse_open_tagged(text.c_str(), p.c_str());
    if ( h <= 0 )
    {
	out = { "error": "the revision's text did not open as a parse handle" };
	return;
    }
    long ent = ui::create(w, "revision-text");
    ui::text_load(w, ent, text.c_str());
    long tag = madc::parse_generation(h);
    // evict the least recently used past the bound: close its handle (its ids
    // route nowhere from now on), rebuild the map from the kept shas
    while ( php::count(order) >= REV_HANDLES_MAX )
    {
	var victim = order[0];
	var ve;
	keyed_get(ve, revs, victim.c_str());
	if ( !ve.is_null() )
	{
	    madc::parse_close(ve["handle"].as_integer());
	    ui::text_load(w, ve["ent"].as_integer(), "");
	}
	var rest = {};
	var kept;				// the map rebuilt from the kept shas (dynamic keys)
	for ( long i = 1; i < php::count(order); i = i + 1 )
	{
	    var s = order[i];
	    rest.push(s);
	    var kv;
	    keyed_get(kv, revs, s.c_str());
	    kept[s.c_str()] = kv;
	}
	order = rest;
	revs = kept;
    }
    var entry = { "sha": sha, "tag": tag, "handle": h, "ent": ent };
    revs[sha.c_str()] = entry;
    order.push(sha);
    ui::set(w, adoc, "revs", revs);
    ui::set(w, adoc, "rev_order", order);
    out = entry;
}

// The revision entry that owns tagged handle `h` (out stays null for none /
// the live handle). Walks the recency list — the one iterable spelling.
void rev_entry_of_handle(var &out, long w, long adoc, long h)
{
    var revs;
    revs_of(revs, w, adoc);
    var order;
    rev_order_of(order, w, adoc);
    for ( var s : order )
    {
	var e;
	keyed_get(e, revs, s.c_str());
	if ( !e.is_null() && e["handle"].as_integer() == h )
	    out = e;
    }
}

// An optional argument is read through keyed_get (editor_events.inc) — the
// promoted twin of the arg_of this file carried until the V6 seam.

// The handle a verb should run against: an id routes itself (graph_route —
// 0 = unknown/stale -> the live handle, whose verb refuses with the reason);
// an id-less read takes an optional `rev`. `is_rev` tells the caller the
// answer comes from a revision (mutations refuse).
long route_handle(long w, long es, long adoc, long live, var &args, bool &is_rev)
{
    is_rev = false;
    var idv;
    keyed_get(idv, args, "id");
    if ( idv.is_null() )
	keyed_get(idv, args, "func");
    if ( !idv.is_null() )
    {
	long h = madc::graph_route(live, idv.as_integer());
	if ( h == 0 )
	    return live;
	is_rev = h != live;
	return h;
    }
    var rv;
    keyed_get(rv, args, "rev");
    if ( rv.is_null() || strlen(rv.c_str()) == 0 )
	return live;
    var e;
    rev_handle_of(e, w, es, adoc, rv.c_str());
    if ( e["handle"].is_null() )
	return live;
    is_rev = true;
    return e["handle"].as_integer();
}

// The text entity a handle was parsed from: the live doc, or a revision's.
long handle_text_ent(long w, long adoc, long live, long h)
{
    if ( h == live )
	return adoc;
    var e;
    rev_entry_of_handle(e, w, adoc, h);
    if ( e.is_null() )
	return adoc;
    return e["ent"].as_integer();
}

// ---- the verbs --------------------------------------------------------------

// graph.source: a node's text = the span's bytes in ITS handle's text entity.
void graph_source(var &out, long w, long adoc, long live, long h, long id)
{
    var sp;
    madc::graph_span(sp, h, id);
    var span = sp["span"];
    if ( span.is_null() )
    {
	out = { "error": sp["error"] };
	return;
    }
    long ent = handle_text_ent(w, adoc, live, h);
    long at = line_start_of(w, ent, span["line"].as_integer()) + span["column"].as_integer();
    long end = line_start_of(w, ent, span["end_line"].as_integer()) + span["end_column"].as_integer();
    var t;
    ui::text(t, w, ent);
    var text = perl::substr(t, at, end - at);	// ring: owned by the var
    out = { "node": sp, "text": text };
}

// graph.status: what this asset can answer, in one call — its LAYERS (the
// design §3.9 wire words from the ONE owner, asset_layers_of) and this node's
// offers beside the handle facts. `live` is 0 for an asset with no parseable
// layer (a binary member): no tree, no diagnostics, nothing to be out of sync.
void graph_status(var &out, long w, long es, long adoc, long live)
{
    bool synced = true;
    long errors = 0;
    long gen = 0;
    if ( live != 0 )
    {
	var t;
	ui::text(t, w, adoc);
	var pt;
	ui::get(pt, w, adoc, "phandle_text");
	synced = !pt.is_null() && pt == t;
	var diags;
	madc::parse_check(diags, live);
	errors = diag_error_count(diags);	// the ONE reader of the severity code (madcide_core.inc)
	gen = madc::parse_generation(live);
    }
    var revs;
    revs_of(revs, w, adoc);
    var order;
    rev_order_of(order, w, adoc);
    var revrows = {};
    for ( var s : order )
    {
	var e;
	keyed_get(e, revs, s.c_str());
	if ( e.is_null() )
	    continue;
	var row = { "sha": e["sha"], "tag": e["tag"] };
	revrows.push(row);
    }
    var layers;
    layer_words(layers, asset_layers_of(w, es, adoc, 0));
    long gh = git_handle_of(w, es, adoc);
    var git;
    if ( gh > 0 )
    {
	var hd;
	madc::git_head(hd, gh);
	var rel;
	git_relpath_of(rel, w, adoc);
	var dt;
	madc::git_dirty(dt, gh, rel.c_str());
	var dirty = dt["dirty"];
	git = { "head": hd["sha"], "branch": hd["branch"], "detached": hd["detached"],
		"dirty": dirty, "path": rel };
    }
    var offers;
    node_offers(offers, w, es);
    long head = clog_head(w, adoc);
    out = { "handle": live, "generation": gen, "synced": synced, "errors": errors,
	    "log_head": head, "revisions": revrows, "git": git, "layers": layers, "node": offers };
}

// graph.commits: the commits touching the served file, newest first.
void graph_commits(var &out, long w, long es, long adoc, long limit)
{
    long gh = git_handle_of(w, es, adoc);
    if ( gh <= 0 )
    {
	out = { "error": "no repository above the served file" };
	return;
    }
    var rel;
    git_relpath_of(rel, w, adoc);
    madc::git_log(out, gh, rel.c_str(), limit > 0 ? limit : 20);
}

// graph.revision: open (or reuse) the revision handle; answer its identity.
void graph_revision(var &out, long w, long es, long adoc, const char *spec)
{
    var e;
    rev_handle_of(e, w, es, adoc, spec);
    if ( e["handle"].is_null() )
    {
	out = { "error": e["error"] };
	return;
    }
    out = { "sha": e["sha"], "tag": e["tag"] };
}

// The symbols of a handle keyed by kind+name: `keys` (the iterable list) and
// `nodes` (the map) — two carriers, one walk.
void symbol_index(var &keys, var &nodes, long h)
{
    var sy;
    madc::graph_symbols(sy, h);
    keys = {};					// the list (an array)
    // `nodes` stays the caller's null var until the first keyed write makes
    // it an object; keyed_get reads it either way.
    var list = sy["nodes"];
    if ( list.is_null() )
	return;
    for ( var n : list )
    {
	var key = format("{}\t{}", n["kind"], n["name"]);
	keys.push(key);
	nodes[key.c_str()] = n;
    }
}

// graph.diff at declaration granularity: symbols keyed by kind+name; changed =
// the node's text differs. "" = the live buffer.
void graph_diff(var &out, long w, long es, long adoc, long live, const char *a0, const char *b0)
{
    var a = a0;
    var b = b0;
    long ha = live;
    long hb = live;
    if ( strlen(a.c_str()) > 0 )
    {
	var ea;
	rev_handle_of(ea, w, es, adoc, a.c_str());
	if ( ea["handle"].is_null() )
	{
	    out = { "error": ea["error"] };
	    return;
	}
	ha = ea["handle"].as_integer();
    }
    if ( strlen(b.c_str()) > 0 )
    {
	var eb;
	rev_handle_of(eb, w, es, adoc, b.c_str());
	if ( eb["handle"].is_null() )
	{
	    out = { "error": eb["error"] };
	    return;
	}
	hb = eb["handle"].as_integer();
    }
    var ka;
    var na;
    symbol_index(ka, na, ha);
    var kb;
    var nb;
    symbol_index(kb, nb, hb);
    var added = {};
    var removed = {};
    var changed = {};
    for ( var key : ka )
    {
	var xa;
	keyed_get(xa, na, key.c_str());
	var xb;
	keyed_get(xb, nb, key.c_str());
	if ( xb.is_null() )
	{
	    removed.push(xa);
	    continue;
	}
	var sa;
	graph_source(sa, w, adoc, live, ha, xa["id"].as_integer());
	var sb;
	graph_source(sb, w, adoc, live, hb, xb["id"].as_integer());
	var ta = sa["text"];
	var tb = sb["text"];
	if ( !(ta == tb) )
	{
	    var pair = { "a": xa, "b": xb };
	    changed.push(pair);
	}
    }
    for ( var key : kb )
    {
	var xa;
	keyed_get(xa, na, key.c_str());
	if ( xa.is_null() )
	{
	    var xb;
	    keyed_get(xb, nb, key.c_str());
	    added.push(xb);
	}
    }
    out = { "added": added, "removed": removed, "changed": changed };
}

// graph.history: GIT rows (blame of the live text, folded to distinct
// commits) + EVENT rows (nodeops whose target has the node's kind+name;
// intersecting splices walked newest->oldest with the inverse anchor shift
// up to the most recent overlapping one — after it, coordinates are not
// derivable, so `complete` says false). Live ids only.
void graph_history(var &out, long w, long es, long adoc, long live, long id)
{
    var sp;
    madc::graph_span(sp, live, id);
    var span = sp["span"];
    if ( span.is_null() )
    {
	out = { "error": sp["error"] };
	return;
    }
    long l0 = span["line"].as_integer();
    long l1 = span["end_line"].as_integer();
    long a = line_start_of(w, adoc, l0) + span["column"].as_integer();
    long b = line_start_of(w, adoc, l1) + span["end_column"].as_integer();
    var rows = {};
    // GIT: blame the live text; uncommitted hunks (sha "") are the event rows' job
    long gh = git_handle_of(w, es, adoc);
    if ( gh > 0 )
    {
	var rel;
	git_relpath_of(rel, w, adoc);
	var t;
	ui::text(t, w, adoc);
	var bl;
	madc::git_blame_text(bl, gh, rel.c_str(), t.c_str(), l0, l1 - l0 + 1);
	var seen;				// sha -> 1 once listed (dynamic keys)
	var hunks = bl["rows"];
	if ( !hunks.is_null() )
	    for ( var hk : hunks )
	    {
		var sha = hk["sha"];
		var was;
		keyed_get(was, seen, sha.c_str());
		if ( strlen(sha.c_str()) == 0 || !was.is_null() )
		    continue;
		seen[sha.c_str()] = 1;
		var row = { "provenance": provenance_name(pvGIT), "sha": sha, "author": hk["author"],
			    "when": hk["when"], "summary": hk["summary"] };
		rows.push(row);
	    }
    }
    // EVENT: walk the session stream newest -> oldest (this doc's records)
    bool complete = true;
    long le = clog_find(w);
    if ( le > 0 )
    {
	var recs;
	clog_records(recs, w, le);	// the ONE reader, parsed once per revision
	var evrows = {};
	bool walking = true;
	for ( long i = php::count(recs) - 1; i >= 0; i = i - 1 )
	{
	    var rec = recs[i];
	    if ( rec["doc"].as_integer() != adoc )
		continue;
	    long ck = clog_kind_of(rec);
	    if ( ck == ckSPLICE && walking )
	    {
		long at = rec["at"].as_integer();
		long del = rec["del"].as_integer();
		long ins = strlen(rec["ins"].c_str());
		if ( at + ins <= a )
		{   // entirely before the span: undo its shift
		    a = a - (ins - del);
		    b = b - (ins - del);
		}
		else if ( at < b )
		{   // intersects: the most recent text edit touching this node
		    var row = { "provenance": provenance_name(pvEVENT), "seq": rec["seq"], "ts": rec["ts"],
				"es": rec["es"], "verb": "splice", "at": at, "del": del, "ins": ins };
		    evrows.push(row);
		    walking = false;	// earlier coordinates are not derivable
		    complete = false;
		}
	    }
	    else if ( ck == ckOTHER && nexus_kind_of(rec) == nkNODEOP )
	    {
		var tgt = rec["target"];
		if ( !tgt.is_null() && tgt["kind"] == sp["kind"] && tgt["name"] == sp["name"] )
		{
		    var row = { "provenance": provenance_name(pvEVENT), "seq": rec["seq"], "ts": rec["ts"],
				"es": rec["es"], "verb": rec["verb"] };
		    evrows.push(row);
		}
	    }
	}
	// oldest first, after the git rows
	for ( long i = php::count(evrows) - 1; i >= 0; i = i - 1 )
	    rows.push(evrows[i]);
    }
    out = { "node": sp, "rows": rows, "complete": complete };
}
