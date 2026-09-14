// madcide_mcp.inc — the MCP SEAT (client-server V6c, design 2026-09-09 §2.7:
// "the MCP seat is an adapter over `api`"). It speaks the Model Context
// Protocol — JSON-RPC 2.0, one message per line — and drives every tool call
// through api_run (madcide_api.inc), the SAME command core the JSON-line api
// seat uses. There is ONE command core, not one per transport: MCP is a
// different ENVELOPE over the same registry commands, tiers, compose and hub
// fan-out.
//
// The MAPPING:
//   MCP `initialize`             -> serverInfo + { capabilities: { tools } }
//   MCP `tools/list`             -> the registry (cmd_table) as MCP tools; each
//                                   tool is a registry command, its single
//                                   `args` string the command's argument text,
//                                   its required tier shown in the description
//                                   (the API is RICH by design — the client
//                                   chooses which tools it uses, §2.7).
//   MCP `tools/call {name,args}` -> api_run(name, args) -> the composed
//                                   projection as the tool's text content; a
//                                   refusal (unknown command / tier too low)
//                                   comes back as isError content, never a
//                                   silent drop.
//   MCP `ping`                   -> {}
//   a notification (no id)       -> no reply (e.g. notifications/initialized).
//
// TRANSPORT: run_mcp over stdio — the STANDARD MCP deployment (an editor /
// agent host spawns `madcide <file> --mcp` and talks JSON-RPC over the pipe).
// mcp_handle is the transport-agnostic core; a socket transport, when one is
// wanted, is another thin loop over it (not built until it has a caller).
// Depends on madcide_api.inc (api_run) and the registry (madcide_enums.inc:
// cmd_table / cmd_min_tier / tier_name).

// The PAST axis (Nexus L4b): revision handles, the id/rev routing and the six
// graph.status / source / history / commits / revision / diff bodies.
#include "madcide_past.inc"

// A JSON-RPC 2.0 success envelope: this request's id echoed, its result.
// Fills `out` (the dialect carrier-return convention, pre-L3 value-return).
void mcp_reply(var &out, var &id, var &result)
{
    out = { "jsonrpc": "2.0", "id": id, "result": result };
}

// A JSON-RPC 2.0 error envelope (id null for a parse error before an id is
// known). `code` is the JSON-RPC error code (-32601 method-not-found, etc.).
void mcp_error(var &out, var &id, long code, const char *message)
{
    var err = { "code": code, "message": message };
    out = { "jsonrpc": "2.0", "id": id, "error": err };
}

// Code-graph MCP L1 (design 2026-09-12): the seven L1 graph verbs, PLUS the
// two L1b body-graph verbs (graph.body/graph.children), with their argument
// shape, for tools/list. A distinct family from the editor
// registry commands below: an agent chooses a semantic navigation verb
// over a keystroke command. Defined here (ahead of mcp_tools, which calls
// it) — madc resolves a call against what has been defined/declared
// earlier in the same file, not a whole-file prescan (confirmed against a
// tmp/ reducer before wiring this in).
void graph_tool_descriptors(var &out)
{
    var idprop = { "type": "integer", "description": "a node id from a graph.* result (a type-id, a global decl-id or a body node id)" };
    var nameprop = { "type": "string", "description": "a symbol name" };
    var lineprop = { "type": "integer", "description": "1-based line" };
    var colprop = { "type": "integer", "description": "1-based column" };
    // Every "properties" object is built as its own named var first — a
    // literal cannot nest a brace list directly as another literal's value
    // (confirmed against a tmp/ reducer: "Nested brace list in a value
    // literal is not supported (yet)"), so pre-build then reference by name,
    // the same idiom mcp_tools already uses for `props`/`schema`.
    var noprops = {};
    var noargs = { "type": "object", "properties": noprops };
    var idprops = { "id": idprop };
    var idargs = { "type": "object", "properties": idprops };
    var nameprops = { "name": nameprop };
    var nameargs = { "type": "object", "properties": nameprops };
    var posprops = { "line": lineprop, "column": colprop };
    var posargs = { "type": "object", "properties": posprops };
    // Code-graph MCP L1b (design 2026-09-12): graph.body/graph.children —
    // the body-graph verbs. `depth` is optional on both (server-side
    // default: graph.children -> 1, graph.body -> -1/full).
    var funcprop = { "type": "integer", "description": "a function node id (type-id)" };
    var depthprop = { "type": "integer", "description": "subtree depth (optional; default: full for graph.body, 1 for graph.children)" };
    var bodyprops = { "func": funcprop, "depth": depthprop };
    var bodyargs = { "type": "object", "properties": bodyprops };
    var childprops = { "id": idprop, "depth": depthprop };
    var childargs = { "type": "object", "properties": childprops };
    var tools = {};
    var t0 = { "name": "graph.symbols", "code": gvSYMBOLS,
	"description": "the TU's own functions as graph nodes {id,kind,name,span}",
	"inputSchema": noargs };
    tools[] = t0;
    var t1 = { "name": "graph.definition", "code": gvDEFINITION,
	"description": "resolve a name to its definition node(s)",
	"inputSchema": nameargs };
    tools[] = t1;
    var t2 = { "name": "graph.node", "code": gvNODE,
	"description": "the node for a node id", "inputSchema": idargs };
    tools[] = t2;
    var t3 = { "name": "graph.type_of", "code": gvTYPE_OF,
	"description": "the type node of a node (HAS_TYPE)",
	"inputSchema": idargs };
    tools[] = t3;
    var t4 = { "name": "graph.members", "code": gvMEMBERS,
	"description": "a struct/class's members with their types (MEMBER_OF)",
	"inputSchema": idargs };
    tools[] = t4;
    var t5 = { "name": "graph.bases", "code": gvBASES,
	"description": "a class's direct base types (INHERITS)",
	"inputSchema": idargs };
    tools[] = t5;
    var t6 = { "name": "graph.enclosing", "code": gvENCLOSING,
	"description": "the innermost enclosing function at a position (ENCLOSES)",
	"inputSchema": posargs };
    tools[] = t6;
    var t7 = { "name": "graph.body", "code": gvBODY,
	"description": "a function's body subtree {nodes,edges:CONTAINS} over the live parse tree",
	"inputSchema": bodyargs };
    tools[] = t7;
    var t8 = { "name": "graph.children", "code": gvCHILDREN,
	"description": "structural descent from any node (a body id or a function id)",
	"inputSchema": childargs };
    tools[] = t8;
    // Code-graph MCP L2 (design 2026-09-12): the derived-edge verbs.
    var kindprop = { "type": "string", "description": "a node kind filter (optional; v1: Function, Global)" };
    var subprop = { "type": "string", "description": "a name substring to match" };
    var searchprops = { "kind": kindprop, "name": subprop };
    var searchargs = { "type": "object", "properties": searchprops };
    var funcargprops = { "func": funcprop };
    var funcargs = { "type": "object", "properties": funcargprops };
    var defprop = { "type": "integer", "description": "a function type-id or a global decl-id" };
    var defprops = { "func": defprop };
    var defargs = { "type": "object", "properties": defprops };
    var t9 = { "name": "graph.callees", "code": gvCALLEES,
	"description": "the functions a function calls (CALLS out-edges)",
	"inputSchema": funcargs };
    tools[] = t9;
    var t10 = { "name": "graph.callers", "code": gvCALLERS,
	"description": "the functions that call a function (CALLS in-edges)",
	"inputSchema": funcargs };
    tools[] = t10;
    var t11 = { "name": "graph.references", "code": gvREFERENCES,
	"description": "every use-site of a function or global (REFERENCES + enclosing)",
	"inputSchema": defargs };
    tools[] = t11;
    var t12 = { "name": "graph.search", "code": gvSEARCH,
	"description": "find decl nodes by kind + name substring (v1: functions, globals)",
	"inputSchema": searchargs };
    tools[] = t12;
    var t13 = { "name": "graph.impact", "code": gvIMPACT,
	"description": "batched callers + references + enclosing for a function or global",
	"inputSchema": idargs };
    tools[] = t13;
    // Code-graph MCP L3 (design 2026-09-12): extents, position lookup and the
    // three mutations. `src` is a SOURCE FRAGMENT (the parser validates it —
    // no builder DSL); `where` is before|after the target.
    var srcprop = { "type": "string", "description": "source text: the statement(s) or definition to place (validated by the parser before commit)" };
    var whereprop = { "type": "string", "description": "before | after (relative to the target node)" };
    // graph.at's column is the SPAN convention (0-based byte column), unlike the
    // 1-based cursor column graph.enclosing takes — its own schema says so.
    var atcolprop = { "type": "integer", "description": "0-based byte column (the span convention: where the node's text starts)" };
    var atprops = { "line": lineprop, "column": atcolprop };
    var atargs = { "type": "object", "properties": atprops };
    // Nexus L4c (design §3.6): `propose` stores the op as a PROPOSAL instead of
    // applying it — the proposer tier always does, an editor may ask for it.
    var proposeprop = { "type": "boolean", "description": "store the edit as a PROPOSAL for an editor to accept instead of applying it (the proposer tier always does; optional)" };
    var insprops = { "id": idprop, "where": whereprop, "src": srcprop, "propose": proposeprop };
    var insargs = { "type": "object", "properties": insprops };
    var repprops = { "id": idprop, "src": srcprop, "propose": proposeprop };
    var repargs = { "type": "object", "properties": repprops };
    var delprops = { "id": idprop, "propose": proposeprop };
    var delargs = { "type": "object", "properties": delprops };
    var t14 = { "name": "graph.span", "code": gvSPAN,
	"description": "a node's exact source extent {span:{line,column,end_line,end_column}} (statement / function / global)",
	"inputSchema": idargs };
    tools[] = t14;
    var t15 = { "name": "graph.at", "code": gvAT,
	"description": "the editable node whose span starts at a position (1-based line, 0-based column)",
	"inputSchema": atargs };
    tools[] = t15;
    var t16 = { "name": "graph.insert", "code": gvINSERT,
	"description": "insert src before|after a node (proposer tier stores a proposal; editor tier applies: validated, logged, undoable)",
	"inputSchema": insargs };
    tools[] = t16;
    var t17 = { "name": "graph.replace", "code": gvREPLACE,
	"description": "replace a node's source with src (proposer tier stores a proposal; editor tier applies: validated, logged, undoable)",
	"inputSchema": repargs };
    tools[] = t17;
    var t18 = { "name": "graph.delete", "code": gvDELETE,
	"description": "delete a node (proposer tier stores a proposal; editor tier applies: validated, logged, undoable)",
	"inputSchema": delargs };
    tools[] = t18;
    // Nexus L4b (design 2026-09-13 §4.3): the PAST verbs. `rev` is any
    // rev-parse spec or a sha; "" = the live buffer. Every id-less read also
    // accepts `rev`; an id routes itself to the handle that minted it.
    var revprop = { "type": "string", "description": "a git revision (rev-parse spec or sha); \"\" = the live buffer" };
    var limitprop = { "type": "integer", "description": "at most this many rows (optional; default 20)" };
    var revprops = { "rev": revprop };
    var revargs = { "type": "object", "properties": revprops };
    var limitprops = { "limit": limitprop };
    var limitargs = { "type": "object", "properties": limitprops };
    var diffprops = { "a": revprop, "b": revprop };
    var diffargs = { "type": "object", "properties": diffprops };
    var t19 = { "name": "graph.status", "code": gvSTATUS,
	"description": "the served asset in one call: handle, generation, synced, error count, log head, open revisions, git head/branch/dirty, layers",
	"inputSchema": noargs };
    tools[] = t19;
    var t20 = { "name": "graph.source", "code": gvSOURCE,
	"description": "a node's source text (a live or revision id)", "inputSchema": idargs };
    tools[] = t20;
    var t21 = { "name": "graph.history", "code": gvHISTORY,
	"description": "who changed this node and when: git commits (blame) + session events (node-ops, text edits), provenance-tagged",
	"inputSchema": idargs };
    tools[] = t21;
    var t22 = { "name": "graph.commits", "code": gvCOMMITS,
	"description": "the commits touching the served file, newest first", "inputSchema": limitargs };
    tools[] = t22;
    var t23 = { "name": "graph.revision", "code": gvREVISION,
	"description": "open the served file at a git revision as a read-only handle; its ids route themselves to it",
	"inputSchema": revargs };
    tools[] = t23;
    var t24 = { "name": "graph.diff", "code": gvDIFF,
	"description": "declaration-level diff between two revisions (\"\" = live): added / removed / changed symbols",
	"inputSchema": diffargs };
    tools[] = t24;
    // Nexus L4c (design §3.4, §4.4): the FUTURE verbs. A proposal is a stored,
    // validated, previewed node-op transaction; its status is derived from the
    // decision records; landing re-checks the target byte-exact.
    var seqprop = { "type": "integer", "description": "a proposal seq (from a proposer's graph.insert/replace/delete answer or graph.proposals)" };
    var statusprop = { "type": "string", "description": "open | accepted | rejected | withdrawn (optional; default: every status)" };
    var reasonprop = { "type": "string", "description": "why (optional)" };
    var seqprops = { "seq": seqprop };
    var seqargs = { "type": "object", "properties": seqprops };
    var statusprops = { "status": statusprop };
    var statusargs = { "type": "object", "properties": statusprops };
    var rejprops = { "seq": seqprop, "reason": reasonprop };
    var rejargs = { "type": "object", "properties": rejprops };
    var t25 = { "name": "graph.proposals", "code": gvPROPOSALS,
	"description": "the served file's proposals (compact rows; optional status filter)",
	"inputSchema": statusargs };
    tools[] = t25;
    var t26 = { "name": "graph.proposal", "code": gvPROPOSAL,
	"description": "one proposal in full: ops, preview, diagnostics, status, decision",
	"inputSchema": seqargs };
    tools[] = t26;
    var t27 = { "name": "graph.accept", "code": gvACCEPT,
	"description": "land an open proposal (editor tier): re-checks the target byte-exact, applies, records the decision",
	"inputSchema": seqargs };
    tools[] = t27;
    var t28 = { "name": "graph.reject", "code": gvREJECT,
	"description": "reject an open proposal with a reason (editor tier)",
	"inputSchema": rejargs };
    tools[] = t28;
    var t29 = { "name": "graph.withdraw", "code": gvWITHDRAW,
	"description": "withdraw an open proposal (its proposer, or an editor)",
	"inputSchema": seqargs };
    tools[] = t29;
    out = tools;
}

// The wire name -> verb code, over the descriptor table (ONE list of names:
// what tools/list advertises is exactly what dispatch knows). gvNONE = unknown.
long graph_verb_of(const char *name)
{
    var nm = name;			// own the text FIRST (ring discipline: `name` is
					// a ring c_str(); every c_str() below would clobber it)
    var tools;
    graph_tool_descriptors(tools);
    for ( var t : tools )
	if ( t["name"] == nm )		// var == var compares text (tmp reducer 2026-09-13)
	    return t["code"].as_integer();
    return gvNONE;
}

// The OUTPUT converter: a verb code to the wire name (the nodeop record's
// `verb`, a proposal op's `verb`), over the same descriptor table.
void graph_verb_name(var &out, long gv)
{
    var tools;
    graph_tool_descriptors(tools);
    for ( var t : tools )
	if ( t["code"] == gv )
	{
	    out = t["name"];
	    return;
	}
    out = "";
}

// The tool list: every registry command (cmd_table) as an MCP tool. One shared
// input schema — a single optional `args` string (IdeSession::command takes one
// argument string) — and a description that names the tier the command needs,
// so the client knows which tools mutate before it calls them.
void mcp_tools(var &out)
{
    var ct;
    cmd_table(ct);
    var argprop = { "type": "string",
	"description": "the command's argument text (optional)" };
    var props = { "args": argprop };
    var schema = { "type": "object", "properties": props };
    var tools = {};
    for ( var r : ct )
    {
	var nm = r["name"];
	long tier = cmd_min_tier(r["code"].as_integer());
	var desc = format("madcide '{}' command (requires {} tier)",
		nm, tier_name(tier));
	var tool = { "name": nm, "description": desc, "inputSchema": schema };
	tools[] = tool;
    }
    // The graph.* family: project the WIRE fields only — a descriptor's `code`
    // is dispatch data (graph_verb_of), never advertised.
    var gtools;
    graph_tool_descriptors(gtools);
    for ( var gt : gtools )
    {
	var tool = { "name": gt["name"], "description": gt["description"],
		     "inputSchema": gt["inputSchema"] };
	tools[] = tool;
    }
    out = tools;
}

// One `tools/call`: run the named tool through api_run, then return the
// resulting STRUCTURED STATE as the tool's content — the programmatic API the
// owner asked for, NOT the rendered editor tree. The tool's optional argument
// is `arguments.args`. A refusal (body carries "error") is isError content
// (the prose). Otherwise the content is a compact JSON snapshot — ok, errors,
// and the raw projections (cursor / diagnostics / outline, session_state) — so
// an agent reads the compiler's data, not the whole composed view. (The
// composed tree stays available on the api seat's `text`; a follow-on can add
// a `fields`/`view` selector for a client that wants it or wants less.)
void mcp_call(var &out, IdeSession &S, long doc, long self_id, var &nm, var &params)
{
    const char *argstr = "";
    var argsobj = params["arguments"];
    var av;
    if ( !argsobj.is_null() )
	av = argsobj["args"];
    if ( !av.is_null() )
	argstr = av.c_str();
    var body;
    api_run(body, S, doc, self_id, nm.c_str(), argstr);
    var err = body["error"];
    bool iserr = !err.is_null();
    var txt;
    if ( iserr )
	txt = err;			// a refusal: the prose, flagged isError
    else
    {
	var snap;
	session_state(snap, S, doc);	// the raw projections after the command
	snap["ok"] = true;
	snap["errors"] = body["errors"];
	var js = js::stringify(snap);	// copy the ring const char*
	txt = js;
    }
    var item = { "type": "text", "text": txt };
    var content = {};
    content[] = item;
    out = { "content": content, "isError": iserr };
}

// The leading whitespace of buffer line `line` — the indentation a fragment
// placed on a new line inherits.
void line_indent_at(var &out, var &t, long w, long doc, long line)
{
    long ls = line_start_of(w, doc, line);
    const char *s = t.c_str();		// ONE copy per scan: a c_str() copies the whole text
    long n = strlen(s);
    long i = ls;
    while ( i < n && (s[i] == ' ' || s[i] == '\t') )
	i = i + 1;
    out = perl::substr(t, ls, i - ls);
}

// Does [at, end) own its line(s): only whitespace before it on its first line
// and after it on its last? Then a delete takes the whole line(s) and an
// insert takes a line of its own; a node sharing its line is placed inline.
bool region_owns_lines(var &t, long at, long end)
{
    const char *s = t.c_str();		// ONE copy per scan
    long n = strlen(s);
    long i = at - 1;
    while ( i >= 0 && s[i] != '\n' )
    {
	if ( s[i] != ' ' && s[i] != '\t' )
	    return false;
	i = i - 1;
    }
    long j = end;
    while ( j < n && s[j] != '\n' )
    {
	if ( s[j] != ' ' && s[j] != '\t' )
	    return false;
	j = j + 1;
    }
    return true;
}

// ONE node-op (code-graph MCP L3, design §6.6; the TRANSACTION over an ops
// LIST since Nexus L4c, design §3.4): span -> splice -> candidate -> VALIDATED
// swap -> commit through the ONE text-mutation owner. Order matters, one
// helper per step:
//   0. edit_preconditions — the document is editable and the tree is THIS
//      text's parse (a desynced tree is re-synced; the caller's ids go stale)
//   1. edit_op_plan — the target's extent from the tree (graph.span, never an
//      agent offset) and the splice {at, del, ins} for the op (replace /
//      delete / insert), plus the node's bytes (the landing re-check)
//   2. edit_candidate — the live buffer with EVERY op's splice, highest offset
//      first, built in the doc's scratch buffer through the engine's
//      text_replace (no string surgery)
//   3. graph_edit_apply — ONE validation: emAPPLY parse_refresh_checked (swap)
//      or emPROPOSE parse_would_accept (verdict only); rejected -> {ok:false,
//      diagnostics}, nothing changed and nothing stored
//   4. emAPPLY accepted -> one undo checkpoint; per op the node-op RECORD then
//      its splice(s) via ed_text_erase / ed_text_insert (anchors shift,
//      journal_splice logs); spans refresh from the already-swapped handle;
//      the hub fan-out; the answer = the node now at the edit position + seq.
//      emPROPOSE accepted -> the preview (edit_preview), ONE proposal record,
//      the answer = {proposal: seq, preview, diagnostics}
// The checked refresh may yield (Stage-2 slices): if the buffer changed under
// it, abort — re-sync the handle to the live text and refuse (the agent
// retries against fresh ids).

// The L3 preconditions every mutation shares: the document is editable (the
// ONE read-only rule — derive_access's verdict on the es bag, the same check
// the typing verbs consult) and the tree is THIS text's parse (the handle
// re-parses only on check / save / outline / an accepted node-op — typing,
// undo or a remote splice moves the buffer under ids that are still "fresh";
// the text the handle was parsed from rides the doc bag; a mismatch re-syncs
// the tree so the caller's ids go loudly stale). false with `why` = the
// refusal prose; `t` = the live text either way.
bool edit_preconditions(var &why, long w, long es, long adoc, var &t)
{
    ui::text(t, w, adoc);
    if ( !es_flag(w, es, "editable") )
    {
	var ro;
	ui::get(ro, w, es, "ro_msg");
	why = "the document is read-only";
	if ( !ro.is_null() )
	    why = ro;
	return false;
    }
    var pt;
    ui::get(pt, w, adoc, "phandle_text");
    if ( pt.is_null() || !(pt == t) )
    {
	reparse_buffer(w, es, adoc);
	why = "the buffer changed since these ids were minted (the tree has been re-synced to it) - re-query";
	return false;
    }
    return true;
}

// ONE node-op PLAN: verb + args -> the op (design §3.4's ops[] element):
//   target = graph_span's answer {id, kind, name, span} (the tree's extent)
//   text   = the node's bytes NOW — the byte-exact re-check at landing
//   splice = {at, del, ins} with the L3 line rules (a node that owns its
//            line(s) is deleted whole / gets a line of its own)
//   line / column = where the edited node starts afterwards (the answer's graph.at)
// false with op = {error} on a miss.
bool edit_op_plan(var &op, long w, long adoc, long h, var &t, long gv, var &args)
{
    long id = args["id"].as_integer();
    var srcv;
    arg_of(srcv, args, "src");
    var src = "";
    if ( !srcv.is_null() )
	src = srcv;
    long where = gwNONE;
    var wherev = "";
    if ( gv == gvINSERT )
    {
	var wv;
	arg_of(wv, args, "where");
	if ( !wv.is_null() )
	{
	    where = graph_where_of(wv.c_str());
	    wherev = wv;
	}
	if ( where == gwNONE )
	{
	    op = { "error": "graph.insert needs where = before | after" };
	    return false;
	}
    }
    if ( gv != gvDELETE && strlen(src.c_str()) == 0 )
    {
	op = { "error": "src is empty (use graph.delete to remove a node)" };
	return false;
    }
    // 1. the extent
    var sp;
    madc::graph_span(sp, h, id);
    var span = sp["span"];		// probe the SUCCESS key: an error answer has
    if ( span.is_null() )		// no span (and `sp` is the logged target below —
    {					// a subscript probe would add an "error":null key)
	op = { "error": sp["error"] };
	return false;
    }
    long l0 = span["line"].as_integer();
    long c0 = span["column"].as_integer();
    long l1 = span["end_line"].as_integer();
    long c1 = span["end_column"].as_integer();
    long size = ui::text_size(w, adoc);
    long at = line_start_of(w, adoc, l0) + c0;
    long end = line_start_of(w, adoc, l1) + c1;
    long at0 = at;			// the node's own extent (before a delete widens it)
    long end0 = end;
    // 2. the splice
    long del = 0;
    var ins = "";
    long new_line = l0;
    long new_col = c0;
    var indent;
    line_indent_at(indent, t, w, adoc, l0);
    long indent_len = strlen(indent.c_str());
    bool owns = region_owns_lines(t, at, end);	// nothing but whitespace beside it on its line(s)
    switch ( gv )
    {
    case gvREPLACE:
	del = end - at;
	ins = src;
	break;
    case gvDELETE:
	if ( owns )
	{
	    // The node owns its line(s): take them whole, newline included.
	    const char *s = t.c_str();		// ONE copy for this scan
	    at = line_start_of(w, adoc, l0);
	    long nl = end;
	    while ( nl < size && s[nl] != '\n' )
		nl = nl + 1;
	    end = nl < size ? nl + 1 : size;
	}
	del = end - at;
	break;
    case gvINSERT:
	if ( !owns )
	{
	    // The target shares its line (`x = 1; y = 2;`): place at the
	    // target's exact boundary, one space apart — never at the line's
	    // edge, which would land beside the NEIGHBOUR.
	    if ( where == gwBEFORE )
		ins = format("{} ", src);
	    else
	    {
		at = end;
		ins = format(" {}", src);
		new_line = l1;
		new_col = c1 + 1;
	    }
	    break;
	}
	if ( where == gwBEFORE )
	    at = line_start_of(w, adoc, l0);
	else
	{
	    const char *s = t.c_str();		// ONE copy for this scan
	    long nl = end;
	    while ( nl < size && s[nl] != '\n' )
		nl = nl + 1;
	    at = nl < size ? nl + 1 : size;
	    new_line = l1 + 1;
	    if ( nl >= size && (size == 0 || s[size - 1] != '\n') )
	    {
		// The target ends the file with no trailing newline: the
		// fragment still gets a line of its own.
		ins = format("\n{}{}\n", indent, src);
		new_col = indent_len;
		break;
	    }
	}
	ins = format("{}{}\n", indent, src);
	new_col = indent_len;
	break;
    }
    var text = perl::substr(t, at0, end0 - at0);
    var verb;
    graph_verb_name(verb, gv);
    var splice = { "at": at, "del": del, "ins": ins };
    op = { "verb": verb, "where": wherev, "target": sp, "text": text, "src": src,
	   "splice": splice, "line": new_line, "column": new_col };
    return true;
}

// The CANDIDATE: the live text with EVERY op's splice applied on the doc's
// scratch buffer (replaybuf_of: clog_replay's scratch, re-loaded by every
// user — one scratch per doc, the engine's text_replace, no string surgery),
// HIGHEST offset first so an earlier splice never moves a later target
// (design §3.4: N ops, one candidate). `order` = the op indices in that
// order — the commit walks the same order.
void edit_candidate(var &cand, var &order, long w, long adoc, var &t, var &ops)
{
    long n = php::count(ops);
    var taken = {};
    for ( long i = 0; i < n; i = i + 1 )
	taken.push(0);
    order = {};
    for ( long k = 0; k < n; k = k + 1 )
    {
	long best = -1;
	long bestat = -1;
	for ( long i = 0; i < n; i = i + 1 )
	{
	    if ( taken[i].as_integer() != 0 )
		continue;
	    var s = ops[i]["splice"];
	    long at = s["at"].as_integer();
	    if ( best < 0 || at > bestat )
	    {
		best = i;
		bestat = at;
	    }
	}
	taken[best] = 1;
	order.push(best);
    }
    long scr = replaybuf_of(w, adoc);
    ui::text_load(w, scr, t.c_str());
    for ( var iv : order )
    {
	var s = ops[iv.as_integer()]["splice"];
	var ins = s["ins"];
	ui::text_replace(w, scr, s["at"].as_integer(), s["del"].as_integer(), ins.c_str());
    }
    ui::text(cand, w, scr);
}

// The proposal's PREVIEW (design §3.4): the touched LINES, live vs candidate.
// line0/line1 = the first and last live line any splice touches; `before` =
// those lines' text, `after` = the same region in the candidate (its end
// shifted by the net byte delta).
void edit_preview(var &out, long w, long adoc, var &t, var &cand, var &ops)
{
    long lo = -1;
    long hi = -1;
    long delta = 0;
    for ( var op : ops )
    {
	var s = op["splice"];
	long at = s["at"].as_integer();
	long del = s["del"].as_integer();
	if ( lo < 0 || at < lo )
	    lo = at;
	if ( at + del > hi )
	    hi = at + del;
	delta = delta + strlen(s["ins"].c_str()) - del;
    }
    long line0 = line_of(w, adoc, lo);
    long line1 = line_of(w, adoc, hi > lo ? hi - 1 : hi);
    long rs = line_start_of(w, adoc, line0);
    long re = line_start_of(w, adoc, line1) + ui::text_line_len(w, adoc, line1);
    if ( re < hi )
	re = hi;
    var before = perl::substr(t, rs, re - rs);
    var after = perl::substr(cand, rs, re + delta - rs);
    out = { "line0": line0, "line1": line1, "before": before, "after": after };
}

// The TRANSACTION (design §3.4): every op's splice on ONE candidate, ONE
// validation, then — emAPPLY — one undo checkpoint, a nodeop record per op
// AHEAD of its splices, every splice through the ONE text-mutation owner
// (highest offset first), spans refreshed, the hub fan-out; or — emPROPOSE —
// the candidate's verdict WITHOUT the swap (parse_would_accept), the preview,
// ONE proposal record {ops, preview, diagnostics, constraints, checks,
// base_seq, actor}, and nothing else touched (not the buffer, not the tree,
// not the undo stack). A rejected candidate answers {ok:false, diagnostics}
// and stores NOTHING in either mode. `proposal_seq` > 0 = this landing is a
// proposal's: the nodeops carry it. The ONE seat of both validator calls
// (gate: check-madcide-single-owners.sh). `actor` = the connection's share id
// (-1 = the local operator) — design §3.2 as amended by the L4c plan.
void graph_edit_apply(var &out, IdeSession &S, long doc, long self_id, var &ops, long mode, long proposal_seq)
{
    long w = S.world();
    long es = S.es;
    long adoc = S.active(doc);
    long h = ensure_phandle(w, es, adoc);
    var t;
    var why;
    if ( !edit_preconditions(why, w, es, adoc, t) )
    {
	out = { "ok": false, "error": why };
	return;
    }
    var cand;
    var order;
    edit_candidate(cand, order, w, adoc, t, ops);
    var diags;
    if ( mode == emPROPOSE )
    {
	if ( !madc::parse_would_accept(diags, h, cand.c_str()) )
	{
	    out = { "ok": false, "error": "proposal rejected: the result introduces a parse/semantic error (nothing stored)",
		    "diagnostics": diags };
	    return;
	}
	var preview;
	edit_preview(preview, w, adoc, t, cand, ops);
	var constraints = {};		// L4d: the decisions linked to the target's symbol key
	var checks = {};		// L4e: linked test runs
	long base = clog_head(w, adoc);
	var rec = { "kind": "proposal", "es": es, "doc": adoc, "actor": self_id, "base_seq": base,
		    "ops": ops, "preview": preview, "diagnostics": diags,
		    "constraints": constraints, "checks": checks };
	long pseq = clog_append(w, adoc, rec);
	broadcast_events(w, adoc, base, self_id);
	out = { "ok": true, "proposal": pseq, "preview": preview, "diagnostics": diags,
		"constraints": constraints };
	return;
    }
    bool accepted = madc::parse_refresh_checked(diags, h, cand.c_str());
    if ( !accepted )
    {
	out = { "ok": false, "error": "edit rejected: the result introduces a parse/semantic error (nothing changed)",
		"diagnostics": diags };
	return;
    }
    var t2;
    ui::text(t2, w, adoc);
    if ( !(t2 == t) )			// var == var compares the text (never two ring c_str()s)
    {
	reparse_buffer(w, es, adoc);	// re-sync the handle to the live text
	out = { "ok": false, "error": "buffer changed while validating: nothing applied, re-query" };
	return;
    }
    ui::set(w, adoc, "phandle_text", cand);	// the swapped tree describes the post-splice text
    long head_before = clog_head(w, adoc);
    edit_checkpoint(w, es, adoc, clamped_caret(w, es, adoc));	// ONE undo step for the transaction
    long nodeop_seq = 0;
    for ( var iv : order )
    {
	var op = ops[iv.as_integer()];
	var rec = { "kind": "nodeop", "es": es, "doc": adoc, "actor": self_id, "verb": op["verb"],
		    "target": op["target"], "src": op["src"] };
	if ( proposal_seq > 0 )
	    rec["proposal"] = proposal_seq;
	long ns = clog_append(w, adoc, rec);
	if ( nodeop_seq == 0 )
	    nodeop_seq = ns;
	var s = op["splice"];
	long at = s["at"].as_integer();
	long del = s["del"].as_integer();
	var ins = s["ins"];
	if ( del > 0 )
	    ed_text_erase(w, es, adoc, at, del);
	if ( strlen(ins.c_str()) > 0 )
	    ed_text_insert(w, es, adoc, at, ins.c_str());
    }
    refresh_spans(w, es, adoc, h);
    broadcast_events(w, adoc, head_before, self_id);
    var first = ops[0];
    var node = {};
    if ( graph_verb_of(first["verb"].c_str()) != gvDELETE )
	madc::graph_at(node, h, first["line"].as_integer(), first["column"].as_integer());
    var fs = first["splice"];
    long seq = clog_head(w, adoc);
    out = { "ok": true, "node": node, "seq": seq, "nodeop_seq": nodeop_seq,
	    "splice_at": fs["at"], "splice_del": fs["del"] };
}

// The FUTURE axis (Nexus L4c): the proposal fold and the five verbs — after
// graph_edit_apply (graph_accept lands through it), before graph_call.
#include "madcide_propose.inc"

// Code-graph MCP L1..L3 (design 2026-09-12): a distinct graph.* tool family, a
// semantic navigation + EDITING surface beside the editor-registry commands
// above. One graph.* tool call: resolve the served doc's parse handle (the SAME
// ensure_phandle session_state uses — one handle, never a second), convert the
// wire name ONCE to its verb code (graph_verb_of — the enum law), gate it by
// tier (graph_min_tier, the registry's refusal shape), dispatch by a switch on
// the code to the matching madc::graph_* accessor (or graph_edit_apply for the
// three mutations), and return the terse node projection as the tool's
// structured content. An unknown verb, a tier refusal and a result carrying
// "error" (a stale id, a non-editable target, a rejected edit) are isError
// content — never a silent drop. `params.arguments` carries the verb args.
void graph_call(var &out, IdeSession &S, long doc, long self_id, var &nm, var &params)
{
    long w = S.world();
    long es = S.es;
    long adoc = S.active(doc);
    long h = ensure_phandle(w, es, adoc);
    var args = params["arguments"];
    if ( args.is_null() )
	args = {};
    long gv = graph_verb_of(nm.c_str());
    var content = {};
    if ( gv == gvNONE )
    {
	var why = format("unknown graph verb '{}'", nm);
	var item = { "type": "text", "text": why };
	content[] = item;
	out = { "content": content, "isError": true };
	return;
    }
    // Permission gate (V6a tiers): the SAME refusal shape api_run gives a
    // registry command above the caller's tier — prose, never a silent run.
    long my_tier = eff_tier(self_id);
    long need = graph_min_tier(gv);
    if ( my_tier < need )
    {
	var why = format("'{}' requires {} (you are {})", nm, tier_name(need), tier_name(my_tier));
	var item = { "type": "text", "text": why };
	content[] = item;
	out = { "content": content, "isError": true };
	return;
    }
    // L4b routing (design §3.3): an id names its handle (graph_route), an
    // id-less read may name a revision (`rev`); `hh` is the handle a read runs
    // against. A mutation — or a proposal — aimed at a revision refuses:
    // revision handles are read-only (the engine refuses their refresh too).
    bool is_rev = false;
    long hh = route_handle(w, es, adoc, h, args, is_rev);
    if ( is_rev && need >= tierPROPOSER )
    {
	var item = { "type": "text", "text": "revision handles are read-only" };
	content[] = item;
	out = { "content": content, "isError": true };
	return;
    }
    var snap;
    switch ( gv )
    {
    case gvSYMBOLS:    madc::graph_symbols(snap, hh); break;
    case gvNODE:       madc::graph_node(snap, hh, args["id"].as_integer()); break;
    case gvTYPE_OF:    madc::graph_type_of(snap, hh, args["id"].as_integer()); break;
    case gvDEFINITION: madc::graph_definition(snap, hh, args["name"].c_str()); break;
    case gvMEMBERS:    madc::graph_members(snap, hh, args["id"].as_integer()); break;
    case gvBASES:      madc::graph_bases(snap, hh, args["id"].as_integer()); break;
    case gvENCLOSING:  madc::graph_enclosing(snap, hh, args["line"].as_integer(), args["column"].as_integer()); break;
    // Code-graph MCP L1b: the body-graph verbs. `depth` is optional — unset
    // means "the natural default for this verb" (graph.body: -1/full,
    // server-capped; graph.children: 1, immediate children).
    case gvBODY:
    {
	var dv = args["depth"];
	long d = dv.is_null() ? -1 : dv.as_integer();
	madc::graph_body(snap, hh, args["func"].as_integer(), d);
	break;
    }
    case gvCHILDREN:
    {
	var dv = args["depth"];
	long d = dv.is_null() ? 1 : dv.as_integer();
	madc::graph_children(snap, hh, args["id"].as_integer(), d);
	break;
    }
    // Code-graph MCP L2: the derived-edge verbs. func is a function type-id;
    // references/impact accept a function type-id OR a global decl-id.
    case gvCALLEES:    madc::graph_callees(snap, hh, args["func"].as_integer()); break;
    case gvCALLERS:    madc::graph_callers(snap, hh, args["func"].as_integer()); break;
    case gvREFERENCES: madc::graph_references(snap, hh, args["func"].as_integer()); break;
    case gvSEARCH:     madc::graph_search(snap, hh, args["kind"].c_str(), args["name"].c_str()); break;
    case gvIMPACT:     madc::graph_impact(snap, hh, args["id"].as_integer()); break;
    // Code-graph MCP L3: extents, position lookup and the three mutations.
    case gvSPAN:       madc::graph_span(snap, hh, args["id"].as_integer()); break;
    case gvAT:         madc::graph_at(snap, hh, args["line"].as_integer(), args["column"].as_integer()); break;
    case gvINSERT:
    case gvREPLACE:
    case gvDELETE:
    {
	// The proposer's reroute (design §3.6): at exactly tierPROPOSER — or
	// any higher tier asking `propose: true` — the op is STORED as a
	// proposal; an editor's plain call lands it. ONE path up to the
	// candidate's validation (graph_edit_apply); the mode decides.
	long mode = emAPPLY;
	var pv;
	arg_of(pv, args, "propose");
	bool asked = !pv.is_null() && pv.as_boolean();
	if ( my_tier == tierPROPOSER || asked )
	    mode = emPROPOSE;
	var t;
	var why;
	if ( !edit_preconditions(why, w, es, adoc, t) )
	{
	    snap = { "ok": false, "error": why };
	    break;
	}
	var op;
	if ( !edit_op_plan(op, w, adoc, h, t, gv, args) )
	{
	    snap = { "ok": false, "error": op["error"] };
	    break;
	}
	var ops = {};
	ops.push(op);
	graph_edit_apply(snap, S, doc, self_id, ops, mode, 0);
	break;
    }
    // Nexus L4c: the FUTURE verbs (madcide_propose.inc).
    case gvPROPOSALS:
    {
	var sv;
	arg_of(sv, args, "status");
	long want = psNONE;
	if ( !sv.is_null() && strlen(sv.c_str()) > 0 )
	{
	    want = proposal_status_of(sv);
	    if ( want == psNONE )
	    {
		var why = format("unknown proposal status '{}'", sv);
		snap = { "error": why };
		break;
	    }
	}
	graph_proposals(snap, w, adoc, want);
	break;
    }
    case gvPROPOSAL: graph_proposal(snap, w, adoc, args["seq"].as_integer()); break;
    case gvACCEPT:   graph_accept(snap, S, doc, self_id, args["seq"].as_integer()); break;
    case gvREJECT:
    {
	var rv;
	arg_of(rv, args, "reason");
	var reason = "";
	if ( !rv.is_null() )
	    reason = rv;
	proposal_decide(snap, w, es, adoc, self_id, args["seq"].as_integer(), psREJECTED, reason.c_str(), false);
	break;
    }
    case gvWITHDRAW:
	proposal_decide(snap, w, es, adoc, self_id, args["seq"].as_integer(), psWITHDRAWN, "", my_tier < tierEDITOR);
	break;
    // Nexus L4b: the PAST verbs (madcide_past.inc).
    case gvSTATUS:     graph_status(snap, w, es, adoc, h); break;
    case gvSOURCE:     graph_source(snap, w, adoc, h, hh, args["id"].as_integer()); break;
    case gvHISTORY:    graph_history(snap, w, es, adoc, h, args["id"].as_integer()); break;
    case gvCOMMITS:
    {
	var lv;
	arg_of(lv, args, "limit");
	graph_commits(snap, w, es, adoc, lv.is_null() ? 20 : lv.as_integer());
	break;
    }
    case gvREVISION:
    {
	var rv;
	arg_of(rv, args, "rev");
	var spec = "HEAD";
	if ( !rv.is_null() )
	    spec = rv;
	graph_revision(snap, w, es, adoc, spec.c_str());
	break;
    }
    case gvDIFF:
    {
	var av;
	arg_of(av, args, "a");
	var bv;
	arg_of(bv, args, "b");
	var a = "";
	var b = "";
	if ( !av.is_null() )
	    a = av;
	if ( !bv.is_null() )
	    b = bv;
	graph_diff(snap, w, es, adoc, h, a.c_str(), b.c_str());
	break;
    }
    }
    // A result that carries an error (a stale id, a non-editable target, a
    // rejected edit) is isError content — an agent must notice, never read
    // an empty node list as "nothing there". Serialise FIRST: a subscript
    // probe creates the key (`"error":null`) in the carrier, and the wire
    // stays terse (§6.5).
    var js = js::stringify(snap);		// copy the ring const char*
    var txt = js;
    bool iserr = !snap["error"].is_null();
    var item = { "type": "text", "text": txt };
    content[] = item;
    out = { "content": content, "isError": iserr };
}

// Handle ONE parsed JSON-RPC message. Returns true with `out` filled when there
// is a reply to send; false for a notification (no id — no reply). The dispatch
// is on the MCP method; every path that touches the document goes through
// api_run, so the tier gate and the hub fan-out apply to an MCP client exactly
// as to an api client. `self_id` is the caller's share id (-1 = the local
// operator, full rights).
bool mcp_handle(IdeSession &S, long doc, long self_id, var &req, var &out)
{
    var idv = req["id"];
    if ( idv.is_null() )
	return false;			// a notification — no reply
    var method = req["method"];
    var params = req["params"];
    if ( method.is_null() )
    {
	mcp_error(out, idv, -32600, "Invalid Request: no method");
	return true;
    }
    const char *m = method.c_str();
    if ( strcmp(m, "initialize") == 0 )
    {
	var proto = "2024-11-05";	// echo the client's version if given
	if ( !params.is_null() )
	{
	    var pv = params["protocolVersion"];
	    if ( !pv.is_null() )
		proto = pv;
	}
	var toolscap = { "listChanged": false };
	var caps = { "tools": toolscap };
	var info = { "name": "madcide", "version": "1.0.0" };
	var result = { "protocolVersion": proto,
	    "capabilities": caps, "serverInfo": info };
	mcp_reply(out, idv, result);
	return true;
    }
    if ( strcmp(m, "ping") == 0 )
    {
	var empty = {};
	mcp_reply(out, idv, empty);
	return true;
    }
    if ( strcmp(m, "tools/list") == 0 )
    {
	var tools;
	mcp_tools(tools);
	var result = { "tools": tools };
	mcp_reply(out, idv, result);
	return true;
    }
    if ( strcmp(m, "tools/call") == 0 )
    {
	if ( params.is_null() )
	{
	    mcp_error(out, idv, -32602, "tools/call needs params");
	    return true;
	}
	var nm = params["name"];
	if ( nm.is_null() )
	{
	    mcp_error(out, idv, -32602, "tools/call needs a tool name");
	    return true;
	}
	// Code-graph MCP L1: a graph.* verb is a distinct tool family, dispatched
	// by name BEFORE the editor-registry path (api_run/mcp_call), so it never
	// falls through as an unknown registry command. strncmp over the
	// dialect's c_str() — the plan's guessed php::substr does not exist in
	// this namespace (grep include/madc/ns_php* 2026-09-12: no `substr`
	// public); a prefix test dispatched once, not a per-verb ladder here.
	if ( strncmp(nm.c_str(), "graph.", 6) == 0 )
	{
	    var result;
	    graph_call(result, S, doc, self_id, nm, params);
	    mcp_reply(out, idv, result);
	    return true;
	}
	var result;
	mcp_call(result, S, doc, self_id, nm, params);
	mcp_reply(out, idv, result);
	return true;
    }
    var msg = format("Method not found: {}", method);
    mcp_error(out, idv, -32601, msg.c_str());
    return true;
}

// The MCP server over stdio (`madcide <file> --mcp`): the STANDARD MCP
// deployment — a client (an editor, an agent host) spawns this process and
// speaks JSON-RPC 2.0 over the pipe, one message per line. stdout carries ONLY
// JSON-RPC (status goes to stderr); the session is headless. The lone local
// client is the operator: self_id -1 (untracked -> owner via eff_tier, the -c
// seat's rule), so it may edit and save. Drives mcp_handle over the SAME
// api_run core the --serve api seat uses.
long run_mcp(const char *path, bool ro)
{
    IdeSession S;
    long doc = S.open(path, ro);
    if ( doc == 0 )
	println(stderr, "madcide: cannot read {}", path);
    if ( doc <= 0 )
	return 1;
    S.terminal(false);			// headless: no interactive terminal

    var line;
    while ( madc::getline(line) )
    {
	var req;
	if ( !js::parse(req, line) )
	{
	    var nullid;
	    var resp;
	    mcp_error(resp, nullid, -32700, "Parse error");
	    var js = js::stringify(resp);
	    println("{}", js);
	    continue;
	}
	var resp;
	if ( mcp_handle(S, doc, -1, req, resp) )
	{
	    var js = js::stringify(resp);
	    println("{}", js);
	}
    }
    S.close();
    return 0;
}
