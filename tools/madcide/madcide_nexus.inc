// madcide_nexus.inc — the INTENT axis of the code-graph MCP (Nexus L4d; design
// docs/plans/2026-09-13-nexus-L4-design.md §3.5, §3.7, §4.5; plan
// 2026-09-14-nexus-L4d-intent-plan.md). Records and links are EVENTS in the
// ONE stream (record: op create|update|close; link: op link|unlink); the fold
// materialises them, cached on the log entity by seq. A ref is a durable key
// (a symbol by file + kind + name — never a graph id). Included by
// madcide_mcp.inc after madcide_propose.inc (context reads the proposal fold)
// and before graph_call; graph_edit_apply reaches nexus_constraints through a
// forward declaration.
//
// Which structure answers what (the index-is-not-the-graph law):
//   record / records — the fold (records; states derived from the ops)
//   context  — the fold's links touching the ref + the proposals whose target
//              is the symbol + the symbol's git rows (graph_history) + its node
//   explain  — ONE projection over the existing verbs: what (graph.node / span
//              / type_of), depends (callees / callers / impact), history
//              (graph_history), why / plan / constraints (the fold)
// Dialect facts this file leans on: an object's keys are enumerated ONLY
// through php::array_keys (range-for over an object reads size 0); a map is a
// null var made an object by its first keyed write (keyed_get reads either).
// Thread contract: the fold is a per-session cache on the changelog entity,
// invalidated by seq; every write is one clog_append on the session thread.

// The ONE canonical key links are matched by (defined with the refs below;
// the fold reads it first — madc resolves a call against an earlier
// definition OR declaration).
void ref_key(var &out, var &ref);

// ---- the fold --------------------------------------------------------------
// out = { records: (id-key -> record), ids: [id…], links: [link…] }. A record
// = the create event's fields + the envelope {id, rkind, state, origin,
// ext_key, actor, es, doc, created_seq, updated_seq, fields}; update merges
// `fields` (the update's keys win) and may set state; close sets state. A link
// row = {seq, from, rel, to, from_key, to_key, actor}; unlink removes the exact
// (from_key, rel, to_key) triple.
void nexus_fold_compute(var &out, long w)
{
    var recs;				// id-key -> record (a null var made the map on its first keyed write)
    var ids = {};
    var links = {};
    long le = clog_find(w);
    if ( le > 0 )
    {
	var blob;
	ui::text(blob, w, le);
	var lines;
	php::explode(lines, "\n", blob.c_str());
	for ( var ln : lines )
	{
	    if ( strlen(ln.c_str()) == 0 )
		continue;
	    var rec;
	    if ( !js::parse(rec, ln) )
		break;			// a torn tail line: stop trusting the rest
	    if ( clog_kind_of(rec) != ckOTHER )
		continue;
	    long nk = nexus_kind_of(rec);
	    if ( nk == nkRECORD )
	    {
		long op = record_op_of(rec["op"]);
		if ( op == roCREATE )
		{
		    var key = format("{}", rec["seq"]);
		    var st = rec["state"];
		    if ( record_state_of(st) == rsNONE )
			st = record_state_name(rsOPEN);
		    var r = { "id": rec["seq"], "rkind": rec["rkind"], "state": st, "origin": rec["origin"],
			      "ext_key": rec["ext_key"], "actor": rec["actor"], "es": rec["es"], "doc": rec["doc"],
			      "created_seq": rec["seq"], "updated_seq": rec["seq"], "fields": rec["fields"] };
		    recs[key.c_str()] = r;
		    ids.push(rec["seq"]);
		}
		else if ( op == roUPDATE || op == roCLOSE )
		{
		    var key = format("{}", rec["id"]);
		    var r;
		    keyed_get(r, recs, key.c_str());
		    if ( r.is_null() )
			continue;		// an op on an unknown record: never trusted
		    var nf = rec["fields"];
		    if ( !nf.is_null() )
		    {
			var f = r["fields"];
			array ks;
			php::array_keys(ks, nf);
			for ( var k : ks )
			{
			    var v;
			    keyed_get(v, nf, k.c_str());
			    f[k.c_str()] = v;	// merge: the update's keys win
			}
			r["fields"] = f;
		    }
		    var st = rec["state"];
		    if ( record_state_of(st) != rsNONE )
			r["state"] = st;
		    r["updated_seq"] = rec["seq"];
		    recs[key.c_str()] = r;
		}
	    }
	    else if ( nk == nkLINK )
	    {
		long op = link_op_of(rec["op"]);
		var from = rec["from"];
		var to = rec["to"];
		var fk;
		ref_key(fk, from);
		var tk;
		ref_key(tk, to);
		if ( op == loLINK )
		{
		    var row = { "seq": rec["seq"], "from": from, "rel": rec["rel"], "to": to,
				"from_key": fk, "to_key": tk, "actor": rec["actor"] };
		    links.push(row);
		}
		else if ( op == loUNLINK )
		{
		    var kept = {};
		    for ( var l : links )
			if ( !(l["from_key"] == fk && l["rel"] == rec["rel"] && l["to_key"] == tk) )
			    kept.push(l);
		    links = kept;
		}
	    }
	}
    }
    out = { "records": recs, "ids": ids, "links": links };
}

// The cached fold: recomputed when the stream's head moved.
void nexus_fold(var &out, long w)
{
    long le = clog_find(w);
    long head = clog_head(w, 0);
    if ( le > 0 )
    {
	long at = es_int(w, le, "nfold_seq", -1);
	if ( at == head )
	{
	    ui::get(out, w, le, "nfold");
	    if ( !out.is_null() )
		return;
	}
    }
    nexus_fold_compute(out, w);
    if ( le > 0 )
    {
	ui::set(w, le, "nfold", out);
	ui::set(w, le, "nfold_seq", head);
    }
}

// ---- refs ------------------------------------------------------------------
// The wire shape {ref: <word>, …}: converted ONCE here; refNONE = refused.
long ref_of(var &out, var &wire)
{
    if ( wire.is_null() || !wire.is_object() )
	return refNONE;
    long k = ref_kind_of(wire["ref"]);
    if ( k == refNONE )
	return refNONE;
    out = wire;
    return k;
}

// The ONE canonical key links are matched by.
void ref_key(var &out, var &ref)
{
    long k = ref_kind_of(ref["ref"]);
    switch ( k )
    {
    case refRECORD:   out = format("record:{}", ref["id"]); return;
    case refSYMBOL:   out = format("symbol:{}\t{}\t{}", ref["file"], ref["kind"], ref["name"]); return;
    case refCOMMIT:   out = format("commit:{}", ref["sha"]); return;
    case refPROPOSAL: out = format("proposal:{}", ref["seq"]); return;
    case refEXTERNAL: out = format("external:{}\t{}", ref["source"], ref["key"]); return;
    }
    out = "";
}

// A symbol ref for a node of the served document (the file is the doc's path
// — the same spelling the stream's records carry).
void symbol_ref_of_doc(var &out, long w, long adoc, var &node)
{
    var p;
    ui::get(p, w, adoc, "path");
    var word = ref_kind_name(refSYMBOL);
    out = { "ref": word, "file": p, "kind": node["kind"], "name": node["name"] };
}

// Resolve a symbol ref against the served document's live tree: graph.definition
// by name, filtered by kind; the file must be the served one. false = no such
// node here.
bool symbol_node_of(var &node, long w, long es, long adoc, long h, var &ref)
{
    var p;
    ui::get(p, w, adoc, "path");
    if ( !(ref["file"] == p) )
	return false;
    var nm = ref["name"];
    var d;
    madc::graph_definition(d, h, nm.c_str());
    var nodes = d["nodes"];
    if ( nodes.is_null() )
	return false;
    for ( var n : nodes )
	if ( n["kind"] == ref["kind"] )
	{
	    node = n;
	    return true;
	}
    return false;
}

// ---- writers ---------------------------------------------------------------
// A record's id is the seq of its create event (derived by the fold); update /
// close name the id. `actor` = the connection's share id (-1 = the operator).
long record_create(long w, long es, long adoc, long self_id, long rkind, var &fields,
		   const char *origin0, const char *ext_key0, long state)
{
    var origin = origin0;
    var ext_key = ext_key0;
    long st = state;
    if ( st == rsNONE )
	st = rsOPEN;
    var rec = { "kind": "record", "op": record_op_name(roCREATE), "es": es, "doc": adoc, "actor": self_id,
		"rkind": record_kind_name(rkind), "state": record_state_name(st),
		"origin": origin, "ext_key": ext_key, "fields": fields };
    return clog_append(w, adoc, rec);
}

long record_update(long w, long es, long adoc, long self_id, long id, var &fields, long state, long op)
{
    var rec = { "kind": "record", "op": record_op_name(op), "es": es, "doc": adoc, "actor": self_id,
		"id": id, "fields": fields };
    if ( state != rsNONE )
	rec["state"] = record_state_name(state);
    return clog_append(w, adoc, rec);
}

long link_write(long w, long es, long adoc, long self_id, long op, var &from, long rel, var &to)
{
    var rec = { "kind": "link", "op": link_op_name(op), "es": es, "doc": adoc, "actor": self_id,
		"from": from, "rel": rel_name(rel), "to": to };
    return clog_append(w, adoc, rec);
}

// ---- the verbs -------------------------------------------------------------
void nexus_record(var &out, long w, long id)
{
    var f;
    nexus_fold(f, w);
    var recs = f["records"];
    var key = format("{}", id);
    var r;
    keyed_get(r, recs, key.c_str());
    if ( r.is_null() )
    {
	var why = format("no record {}", id);
	out = { "error": why };
	return;
    }
    out = r;
}

// rkind rkNONE = every kind; state rsNONE = the LIVE states (open, active,
// done) — a superseded or rejected record answers only when asked for.
void nexus_records(var &out, long w, long rkind, long state)
{
    var f;
    nexus_fold(f, w);
    var recs = f["records"];
    var rows = {};
    for ( var idv : f["ids"] )
    {
	var key = format("{}", idv);
	var r;
	keyed_get(r, recs, key.c_str());
	if ( r.is_null() )
	    continue;
	if ( rkind != rkNONE && record_kind_of(r["rkind"]) != rkind )
	    continue;
	long st = record_state_of(r["state"]);
	if ( state == rsNONE )
	{
	    if ( st == rsSUPERSEDED || st == rsREJECTED )
		continue;
	}
	else if ( st != state )
	    continue;
	rows.push(r);
    }
    out = { "rows": rows };
}

// The links touching a key, as context rows (provenance record), each with the
// record on the other end when there is one.
void context_link_rows(var &rows, var &f, var &key)
{
    var pv = provenance_name(pvRECORD);
    var recs = f["records"];
    for ( var l : f["links"] )
    {
	bool from_me = l["from_key"] == key;
	bool to_me = l["to_key"] == key;
	if ( !from_me && !to_me )
	    continue;
	var other = l["from"];
	var dir = "in";
	if ( from_me )
	{
	    other = l["to"];
	    dir = "out";
	}
	var row = { "provenance": pv, "rel": l["rel"], "direction": dir, "ref": other, "seq": l["seq"] };
	if ( ref_kind_of(other["ref"]) == refRECORD )
	{
	    var rkey = format("{}", other["id"]);
	    var r;
	    keyed_get(r, recs, rkey.c_str());
	    if ( !r.is_null() )
		row["record"] = r;
	}
	rows.push(row);
    }
}

// nexus.context(ref): the RAW list of everything linked to a ref, each row
// provenance-tagged — links + records (record), the proposals whose target is
// the symbol (record), the symbol's git / event rows (graph_history) and its
// node (compiler).
void nexus_context(var &out, long w, long es, long adoc, long h, var &ref)
{
    var f;
    nexus_fold(f, w);
    var key;
    ref_key(key, ref);
    var rows = {};
    context_link_rows(rows, f, key);
    long k = ref_kind_of(ref["ref"]);
    if ( k == refSYMBOL )
    {
	var node;
	if ( symbol_node_of(node, w, es, adoc, h, ref) )
	{
	    var nrow = { "provenance": provenance_name(pvCOMPILER), "node": node };
	    rows.push(nrow);
	    var hist;
	    graph_history(hist, w, es, adoc, h, node["id"].as_integer());
	    var hrows = hist["rows"];
	    if ( !hrows.is_null() )
		for ( var hr : hrows )
		    rows.push(hr);
	}
	// the proposals that touched this symbol (the record axis)
	var seqs;
	var props;
	proposal_fold(seqs, props, w, adoc);
	var rpv = provenance_name(pvRECORD);
	for ( var sv : seqs )
	{
	    var pkey = format("{}", sv);
	    var p;
	    keyed_get(p, props, pkey.c_str());
	    if ( p.is_null() )
		continue;
	    for ( var op : p["ops"] )
	    {
		var tgt = op["target"];
		if ( tgt["kind"] == ref["kind"] && tgt["name"] == ref["name"] )
		{
		    var prow = { "provenance": rpv, "proposal": p["seq"], "status": p["status"], "verb": op["verb"] };
		    rows.push(prow);
		    break;
		}
	    }
	}
    }
    out = { "ref": ref, "key": key, "rows": rows };
}

// The decisions that constrain a node: records of kind decision linked by
// affects / documents to the node's symbol key (either direction). Informing,
// never blocking (design §3.4) — a proposal's `constraints`.
void nexus_constraints(var &out, long w, long adoc, var &target)
{
    out = {};
    var ref;
    symbol_ref_of_doc(ref, w, adoc, target);
    var key;
    ref_key(key, ref);
    var f;
    nexus_fold(f, w);
    var recs = f["records"];
    for ( var l : f["links"] )
    {
	long rel = rel_of(l["rel"]);
	if ( rel != relAFFECTS && rel != relDOCUMENTS )
	    continue;
	var other;
	if ( l["to_key"] == key )
	    other = l["from"];
	else if ( l["from_key"] == key )
	    other = l["to"];
	else
	    continue;
	if ( ref_kind_of(other["ref"]) != refRECORD )
	    continue;
	var rkey = format("{}", other["id"]);
	var r;
	keyed_get(r, recs, rkey.c_str());
	if ( r.is_null() || record_kind_of(r["rkind"]) != rkDECISION )
	    continue;
	var flds = r["fields"];
	var title;
	keyed_get(title, flds, "title");
	var row = { "id": r["id"], "state": r["state"], "title": title, "rel": l["rel"] };
	out.push(row);
    }
}

// nexus.explain(ref, detail): ONE compound projection over the existing verbs
// (design §4.5) — what / depends / history / why / plan / constraints. v1
// takes a symbol ref resolved against the served document.
void nexus_explain(var &out, long w, long es, long adoc, long h, var &ref, long detail)
{
    if ( ref_kind_of(ref["ref"]) != refSYMBOL )
    {
	out = { "error": "explain takes a symbol ref {ref: symbol, file, kind, name} (v1)" };
	return;
    }
    var node;
    if ( !symbol_node_of(node, w, es, adoc, h, ref) )
    {
	var why = format("no {} named {} in the served document", ref["kind"], ref["name"]);
	out = { "error": why };
	return;
    }
    long id = node["id"].as_integer();
    var cpv = provenance_name(pvCOMPILER);
    var sp;
    madc::graph_span(sp, h, id);
    var what = { "provenance": cpv, "node": node, "span": sp["span"] };
    if ( detail == edFULL )
    {
	var ty;
	madc::graph_type_of(ty, h, id);
	what["type"] = ty;
    }
    // The call edges exist for a function only: the graph verbs answer
    // {nodes, edges} for one (the subject node rides in `nodes` — the COUNT is
    // the edges) and an error for anything else — ask them, never a kind word.
    var depends = { "provenance": cpv };
    var ce;
    madc::graph_callees(ce, h, id);
    var cee = ce["edges"];
    if ( !cee.is_null() )
    {
	var cr;
	madc::graph_callers(cr, h, id);
	var cre = cr["edges"];
	var im;
	madc::graph_impact(im, h, id);
	depends["callees"] = php::count(cee);
	depends["callers"] = php::count(cre);
	depends["impact"] = im;
    }
    var hist;
    graph_history(hist, w, es, adoc, h, id);
    var hrows = hist["rows"];
    if ( detail != edFULL && !hrows.is_null() && php::count(hrows) > 10 )
    {
	var capped = {};
	for ( long i = 0; i < 10; i = i + 1 )
	    capped.push(hrows[i]);
	hrows = capped;
    }
    var history = { "rows": hrows, "complete": hist["complete"] };
    // the record axis: why (decisions + requirements + the proposals that
    // touched it), plan (live tasks / milestones), constraints (affects /
    // documents decisions)
    var f;
    nexus_fold(f, w);
    var key;
    ref_key(key, ref);
    var lrows = {};
    context_link_rows(lrows, f, key);
    var why = {};
    var plan = {};
    for ( var lr : lrows )
    {
	var r = lr["record"];
	if ( r.is_null() )
	    continue;
	long rk = record_kind_of(r["rkind"]);
	long st = record_state_of(r["state"]);
	long rel = rel_of(lr["rel"]);
	if ( rk == rkDECISION || rk == rkREQUIREMENT )
	    why.push(lr);
	bool live = st != rsDONE && st != rsSUPERSEDED && st != rsREJECTED;
	bool planned = rel == relPLANNED_FOR || rel == relREQUESTED_BY || rel == relIMPLEMENTS;
	if ( (rk == rkTASK || rk == rkMILESTONE) && live && planned )
	    plan.push(lr);
    }
    var ctx;
    nexus_context(ctx, w, es, adoc, h, ref);
    var rpv = provenance_name(pvRECORD);
    for ( var cr : ctx["rows"] )
	if ( cr["provenance"] == rpv && !cr["proposal"].is_null() )
	    why.push(cr);
    var constraints;
    nexus_constraints(constraints, w, adoc, node);
    out = { "ref": ref, "what": what, "depends": depends, "history": history, "why": why, "plan": plan,
	    "constraints": constraints };
}
