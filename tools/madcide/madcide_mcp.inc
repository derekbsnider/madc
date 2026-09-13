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
    var idprop = { "type": "integer", "description": "a node id (type-id)" };
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
    var insprops = { "id": idprop, "where": whereprop, "src": srcprop };
    var insargs = { "type": "object", "properties": insprops };
    var repprops = { "id": idprop, "src": srcprop };
    var repargs = { "type": "object", "properties": repprops };
    var t14 = { "name": "graph.span", "code": gvSPAN,
	"description": "a node's exact source extent {span:{line,column,end_line,end_column}} (statement / function / global)",
	"inputSchema": idargs };
    tools[] = t14;
    var t15 = { "name": "graph.at", "code": gvAT,
	"description": "the editable node whose span starts at a position (1-based line, 0-based column)",
	"inputSchema": posargs };
    tools[] = t15;
    var t16 = { "name": "graph.insert", "code": gvINSERT,
	"description": "insert src before|after a node (editor tier; validated, logged, undoable)",
	"inputSchema": insargs };
    tools[] = t16;
    var t17 = { "name": "graph.replace", "code": gvREPLACE,
	"description": "replace a node's source with src (editor tier; validated, logged, undoable)",
	"inputSchema": repargs };
    tools[] = t17;
    var t18 = { "name": "graph.delete", "code": gvDELETE,
	"description": "delete a node (editor tier; validated, logged, undoable)",
	"inputSchema": idargs };
    tools[] = t18;
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
    long n = strlen(t.c_str());
    long i = ls;
    while ( i < n && (t.c_str()[i] == ' ' || t.c_str()[i] == '\t') )
	i = i + 1;
    out = perl::substr(t, ls, i - ls);
}

// Does [at, end) own its line(s): only whitespace before it on its first line
// and after it on its last? Then a delete takes the whole line(s).
bool region_owns_lines(var &t, long at, long end)
{
    long n = strlen(t.c_str());
    long i = at - 1;
    while ( i >= 0 && t.c_str()[i] != '\n' )
    {
	if ( t.c_str()[i] != ' ' && t.c_str()[i] != '\t' )
	    return false;
	i = i - 1;
    }
    long j = end;
    while ( j < n && t.c_str()[j] != '\n' )
    {
	if ( t.c_str()[j] != ' ' && t.c_str()[j] != '\t' )
	    return false;
	j = j + 1;
    }
    return true;
}

// ONE node-op (code-graph MCP L3, design §6.6): span -> splice -> candidate ->
// VALIDATED swap -> commit through the ONE text-mutation owner. Order matters:
//   1. the target's extent from the tree (graph.span) — never an agent offset
//   2. the splice {at, del, ins} for the op (replace / delete / insert)
//   3. the candidate = the live buffer with that splice, built in the doc's
//      scratch buffer through the engine's text_replace (no string surgery)
//   4. parse_refresh_checked(candidate): rejected -> {ok:false, diagnostics},
//      nothing changed (not the tree, not the buffer, not the log)
//   5. accepted -> one undo checkpoint; the node-op RECORD; the splice(s) via
//      ed_text_erase / ed_text_insert (anchors shift, journal_splice logs);
//      spans refresh from the already-swapped handle; the hub fan-out
//   6. the answer: the node now at the edit position (graph.at) + seq
// The checked refresh may yield (Stage-2 slices): if the buffer changed under
// it, abort — re-sync the handle to the live text and refuse (the agent
// retries against fresh ids).
void graph_edit_apply(var &out, IdeSession &S, long doc, long self_id, long gv, var &args)
{
    long w = S.world();
    long es = S.es;
    long adoc = S.active(doc);
    long h = ensure_phandle(w, es, adoc);
    long id = args["id"].as_integer();
    var srcv = args["src"];
    var src = "";
    if ( !srcv.is_null() )
	src = srcv;
    long where = gwNONE;
    if ( gv == gvINSERT )
    {
	var wv = args["where"];
	if ( !wv.is_null() )
	    where = graph_where_of(wv.c_str());
	if ( where == gwNONE )
	{
	    out = { "ok": false, "error": "graph.insert needs where = before | after" };
	    return;
	}
    }
    if ( gv != gvDELETE && strlen(src.c_str()) == 0 )
    {
	out = { "ok": false, "error": "src is empty (use graph.delete to remove a node)" };
	return;
    }
    // 1. the extent
    var sp;
    madc::graph_span(sp, h, id);
    var span = sp["span"];		// probe the SUCCESS key: an error answer has
    if ( span.is_null() )		// no span (and `sp` is the logged target below —
    {					// a subscript probe would add an "error":null key)
	out = { "ok": false, "error": sp["error"] };
	return;
    }
    long l0 = span["line"].as_integer();
    long c0 = span["column"].as_integer();
    long l1 = span["end_line"].as_integer();
    long c1 = span["end_column"].as_integer();
    var t;
    ui::text(t, w, adoc);
    long size = strlen(t.c_str());
    long at = line_start_of(w, adoc, l0) + c0;
    long end = line_start_of(w, adoc, l1) + c1;
    // 2. the splice
    long del = 0;
    var ins = "";
    long new_line = l0;
    long new_col = c0;
    var indent;
    line_indent_at(indent, t, w, adoc, l0);
    long indent_len = strlen(indent.c_str());
    switch ( gv )
    {
    case gvREPLACE:
	del = end - at;
	ins = src;
	break;
    case gvDELETE:
	if ( region_owns_lines(t, at, end) )
	{
	    at = line_start_of(w, adoc, l0);
	    long nl = end;
	    while ( nl < size && t.c_str()[nl] != '\n' )
		nl = nl + 1;
	    end = nl < size ? nl + 1 : size;
	}
	del = end - at;
	break;
    case gvINSERT:
	if ( where == gwBEFORE )
	    at = line_start_of(w, adoc, l0);
	else
	{
	    long nl = end;
	    while ( nl < size && t.c_str()[nl] != '\n' )
		nl = nl + 1;
	    at = nl < size ? nl + 1 : size;
	    new_line = l1 + 1;
	}
	ins = format("{}{}\n", indent, src);
	new_col = indent_len;
	break;
    }
    // 3. the candidate, through the engine's text_replace on the scratch buffer
    long scr = replaybuf_of(w, adoc);
    ui::text_load(w, scr, t.c_str());
    ui::text_replace(w, scr, at, del, ins.c_str());
    var cand;
    ui::text(cand, w, scr);
    // 4. validate + swap (atomic)
    var diags;
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
    // 5. commit
    long head_before = clog_head(w, adoc);
    edit_checkpoint(w, es, adoc, clamped_caret(w, es, adoc));
    var verb = "graph.insert";
    if ( gv == gvREPLACE )
	verb = "graph.replace";
    else if ( gv == gvDELETE )
	verb = "graph.delete";
    var rec = { "kind": "nodeop", "es": es, "doc": adoc, "verb": verb,
		"target": sp, "src": src };
    clog_append(w, adoc, rec);
    if ( del > 0 )
	ed_text_erase(w, es, adoc, at, del);
    if ( strlen(ins.c_str()) > 0 )
	ed_text_insert(w, es, adoc, at, ins.c_str());
    refresh_spans(w, es, adoc, h);
    broadcast_events(w, adoc, head_before, self_id);
    // 6. the answer
    var node = {};
    if ( gv != gvDELETE )
	madc::graph_at(node, h, new_line, new_col);
    long seq = clog_head(w, adoc);
    out = { "ok": true, "node": node, "seq": seq, "splice_at": at, "splice_del": del };
}

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
    var snap;
    switch ( gv )
    {
    case gvSYMBOLS:    madc::graph_symbols(snap, h); break;
    case gvNODE:       madc::graph_node(snap, h, args["id"].as_integer()); break;
    case gvTYPE_OF:    madc::graph_type_of(snap, h, args["id"].as_integer()); break;
    case gvDEFINITION: madc::graph_definition(snap, h, args["name"].c_str()); break;
    case gvMEMBERS:    madc::graph_members(snap, h, args["id"].as_integer()); break;
    case gvBASES:      madc::graph_bases(snap, h, args["id"].as_integer()); break;
    case gvENCLOSING:  madc::graph_enclosing(snap, h, args["line"].as_integer(), args["column"].as_integer()); break;
    // Code-graph MCP L1b: the body-graph verbs. `depth` is optional — unset
    // means "the natural default for this verb" (graph.body: -1/full,
    // server-capped; graph.children: 1, immediate children).
    case gvBODY:
    {
	var dv = args["depth"];
	long d = dv.is_null() ? -1 : dv.as_integer();
	madc::graph_body(snap, h, args["func"].as_integer(), d);
	break;
    }
    case gvCHILDREN:
    {
	var dv = args["depth"];
	long d = dv.is_null() ? 1 : dv.as_integer();
	madc::graph_children(snap, h, args["id"].as_integer(), d);
	break;
    }
    // Code-graph MCP L2: the derived-edge verbs. func is a function type-id;
    // references/impact accept a function type-id OR a global decl-id.
    case gvCALLEES:    madc::graph_callees(snap, h, args["func"].as_integer()); break;
    case gvCALLERS:    madc::graph_callers(snap, h, args["func"].as_integer()); break;
    case gvREFERENCES: madc::graph_references(snap, h, args["func"].as_integer()); break;
    case gvSEARCH:     madc::graph_search(snap, h, args["kind"].c_str(), args["name"].c_str()); break;
    case gvIMPACT:     madc::graph_impact(snap, h, args["id"].as_integer()); break;
    // Code-graph MCP L3: extents, position lookup and the three mutations.
    case gvSPAN:       madc::graph_span(snap, h, args["id"].as_integer()); break;
    case gvAT:         madc::graph_at(snap, h, args["line"].as_integer(), args["column"].as_integer()); break;
    case gvINSERT:
    case gvREPLACE:
    case gvDELETE:     graph_edit_apply(snap, S, doc, self_id, gv, args); break;
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
