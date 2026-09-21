// madcide_propose.inc — the FUTURE axis of the code-graph MCP (Nexus L4c;
// design docs/plans/2026-09-13-nexus-L4-design.md §3.4, §3.6, §4.4; plan
// 2026-09-14-nexus-L4c-propose-plan.md). The proposal FOLD over the ONE stream
// and the five verbs. Included by madcide_mcp.inc AFTER graph_edit_apply
// (graph_accept lands through it) and BEFORE graph_call (which dispatches here).
//
// Which structure answers what (the index-is-not-the-graph law):
//   proposals / proposal — this doc's proposal records + the decision naming
//                          each (status DERIVED: open until a decision; the
//                          last decision wins) + the testrun events recorded
//                          under each (its `checks`, L4e — derived too)
//   accept   — the fold (open?), the L3 preconditions, the three re-checks PER
//              OP against the fresh tree, the ordinary graph_edit_apply(emAPPLY),
//              then a decision record {accepted, landed_seq}
//   reject / withdraw — the fold (open?; withdraw at the proposer tier: own
//              only), a decision record
// Thread contract: the fold is per-call state over the session-thread stream
// (design §7); nothing here is persisted beyond the stream itself.

// Fold this document's proposals: `seqs` (creation order) and `props` (a map
// seq-key -> the record with `status` (the wire word) and `decision` (the
// deciding record) — a null var made an object by its first keyed write).
void proposal_fold(var &seqs, var &props, long w, long adoc)
{
    seqs = {};
    long le = clog_find(w);
    if ( le <= 0 )
	return;
    var recs;
    clog_records(recs, w, le);		// the ONE reader, parsed once per revision
    for ( var rec : recs )
    {
	if ( rec["doc"].as_integer() != adoc || clog_kind_of(rec) != ckOTHER )
	    continue;
	long nk = nexus_kind_of(rec);
	if ( nk == nkPROPOSAL )
	{
	    var key = format("{}", rec["seq"]);
	    rec["status"] = proposal_status_name(psOPEN);
	    props[key.c_str()] = rec;
	    seqs.push(rec["seq"]);
	}
	else if ( nk == nkDECISION )
	{
	    var key = format("{}", rec["proposal"]);
	    var p;
	    keyed_get(p, props, key.c_str());
	    if ( p.is_null() )
		continue;		// a decision naming no proposal here: never trusted
	    p["status"] = rec["decision"];	// the LAST decision wins (design §3.4)
	    p["decision"] = rec;
	    props[key.c_str()] = p;
	}
	else if ( nk == nkTESTRUN )
	{
	    // A run recorded under a proposal (L4e, design §3.10): its CHECKS
	    // are derived like its status — the events, never a rewrite.
	    var pv = rec["proposal"];
	    if ( pv.is_null() )
		continue;
	    var key = format("{}", pv);
	    var p;
	    keyed_get(p, props, key.c_str());
	    if ( p.is_null() )
		continue;
	    var chk = p["checks"];
	    if ( chk.is_null() )
		chk = {};
	    chk.push(rec);
	    p["checks"] = chk;
	    props[key.c_str()] = p;
	}
    }
}

// One proposal by seq: `out` = the folded record, or left null.
void proposal_of(var &out, long w, long adoc, long seq)
{
    var seqs;
    var props;
    proposal_fold(seqs, props, w, adoc);
    var key = format("{}", seq);
    keyed_get(out, props, key.c_str());
}

// graph.proposals(status?): compact rows, creation order; `want` filters
// (psNONE = every status).
void graph_proposals(var &out, long w, long adoc, long want)
{
    var seqs;
    var props;
    proposal_fold(seqs, props, w, adoc);
    var rows = {};
    for ( var sv : seqs )
    {
	var key = format("{}", sv);
	var p;
	keyed_get(p, props, key.c_str());
	if ( want != psNONE && proposal_status_of(p["status"]) != want )
	    continue;
	var ops = p["ops"];
	var first = ops[0];
	var tgt = first["target"];
	long nops = php::count(ops);
	var row = { "seq": p["seq"], "status": p["status"], "actor": p["actor"], "base_seq": p["base_seq"],
		    "ts": p["ts"], "ops": nops, "verb": first["verb"], "kind": tgt["kind"], "name": tgt["name"] };
	rows.push(row);
    }
    out = { "rows": rows };
}

// graph.proposal(seq): the whole folded record.
void graph_proposal(var &out, long w, long adoc, long seq)
{
    var p;
    proposal_of(p, w, adoc, seq);
    if ( p.is_null() )
    {
	var why = format("no proposal {}", seq);
	out = { "error": why };
	return;
    }
    out = p;
}

// graph.accept(seq): land an OPEN proposal through the ORDINARY apply after
// the three re-checks per op (design §3.4): (1) the doc is text-synced (the
// L3 precondition); (2) the node at the target's span start has the same
// kind + name; (3) its text equals the proposal's byte-exact copy. Any miss
// refuses and the proposal stays open. The ops are RE-PLANNED against the
// fresh tree (a stored id is generation-bound) — one plan owner, edit_op_plan.
void graph_accept(var &out, IdeSession &S, long doc, long self_id, long seq)
{
    long w = S.world();
    long es = S.es;
    long adoc = S.active(doc);
    var p;
    proposal_of(p, w, adoc, seq);
    if ( p.is_null() )
    {
	var why = format("no proposal {}", seq);
	out = { "error": why };
	return;
    }
    if ( proposal_status_of(p["status"]) != psOPEN )
    {
	var why = format("proposal {} is {}", seq, p["status"]);
	out = { "error": why };
	return;
    }
    long h = ensure_phandle(w, es, adoc);
    var t;
    var why;
    if ( !edit_preconditions(why, w, es, adoc, t) )
    {
	out = { "error": why };
	return;
    }
    var changed = format("proposal target changed since seq {}; re-propose against fresh ids", seq);
    var fresh = {};
    for ( var op : p["ops"] )
    {
	var tgt = op["target"];
	var span = tgt["span"];
	var node;
	madc::graph_at(node, h, span["line"].as_integer(), span["column"].as_integer());
	if ( node["id"].is_null() || !(node["kind"] == tgt["kind"]) || !(node["name"] == tgt["name"]) )
	{
	    out = { "error": changed };
	    return;
	}
	var src;
	graph_source(src, w, adoc, h, h, node["id"].as_integer());
	if ( !(src["text"] == op["text"]) )
	{
	    out = { "error": changed };
	    return;
	}
	var args = { "id": node["id"], "src": op["src"], "where": op["where"] };
	var fop;
	if ( !edit_op_plan(fop, w, adoc, h, t, graph_verb_of(op["verb"].c_str()), args) )
	{
	    out = { "error": fop["error"] };
	    return;
	}
	fresh.push(fop);
    }
    var landed;
    graph_edit_apply(landed, S, doc, self_id, fresh, emAPPLY, seq);
    if ( !landed["ok"].as_boolean() )
    {
	out = { "error": landed["error"], "diagnostics": landed["diagnostics"] };
	return;
    }
    long head = clog_head(w, adoc);
    var dec = { "kind": "decision", "es": es, "doc": adoc, "actor": self_id, "proposal": seq,
		"decision": proposal_status_name(psACCEPTED), "landed_seq": landed["nodeop_seq"], "reason": "" };
    clog_append(w, adoc, dec);
    // The CHECKS (L4e, design §3.10): the tests linked to every op's target,
    // run here and recorded under the proposal — informing, never blocking
    // (the decision stands; the runs ride the same push).
    var chk;
    proposal_checks(chk, S, doc, self_id, p["ops"], seq);
    broadcast_events(w, adoc, head, self_id);
    out = { "ok": true, "proposal": seq, "node": landed["node"], "seq": landed["seq"],
	    "landed_seq": landed["nodeop_seq"], "checks": chk };
}

// A decision on an OPEN proposal (graph.reject / graph.withdraw): the record
// and the answer. `want` = the status the decision sets; `own_only` = the
// proposer tier's rule (only the proposal's own actor may withdraw it).
void proposal_decide(var &out, long w, long es, long adoc, long self_id, long seq, long want,
		     const char *reason0, bool own_only)
{
    var reason = reason0;		// ring discipline
    var p;
    proposal_of(p, w, adoc, seq);
    if ( p.is_null() )
    {
	var why = format("no proposal {}", seq);
	out = { "error": why };
	return;
    }
    if ( proposal_status_of(p["status"]) != psOPEN )
    {
	var why = format("proposal {} is {}", seq, p["status"]);
	out = { "error": why };
	return;
    }
    if ( own_only && p["actor"].as_integer() != self_id )
    {
	var why = format("proposal {} is not yours (actor {})", seq, p["actor"]);
	out = { "error": why };
	return;
    }
    long head = clog_head(w, adoc);
    var dec = { "kind": "decision", "es": es, "doc": adoc, "actor": self_id, "proposal": seq,
		"decision": proposal_status_name(want), "landed_seq": 0, "reason": reason };
    clog_append(w, adoc, dec);
    broadcast_events(w, adoc, head, self_id);
    out = { "ok": true, "proposal": seq, "status": proposal_status_name(want) };
}
