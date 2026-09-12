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
