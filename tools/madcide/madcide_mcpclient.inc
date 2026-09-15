// madcide_mcpclient.inc — madc as an MCP CLIENT (Nexus L4d; design §4.5, §5
// flow 4, §10 (3)): an external system's MCP server is a child over
// madc::channel exec:// (stdio JSON-RPC, one message per line); its manifest
// is DATA (<profiles>/mcp/<source>.mcp.json) loaded ONCE into an enum-indexed
// vocabulary table; nexus.sync pulls its rows into `record` events with
// origin = the source (the cached projection of design §3.5 ruling 2). ONE
// child per sync (a channel is not a value; a sync is rare). Transport: stdio
// only (mtSTDIO); http:// waits on a madcdis channel — a stdio<->HTTP bridge
// process (mcp-remote) is the owner's configuration meanwhile.
//
// Manifest shape: { "name": "mycenode", "transport": "stdio",
//   "command": ["{madc}", "--no-config", "path/to/server.mad"],  // joined by single spaces; {madc} / {project} substitute (build_subst — the ONE owner)
//   "env": { "TOKEN": "…" },                                      // rendered as an `env K=V …` prefix (posix); refused on win32
//   "vocabulary": { "list_tasks": { "tool": "list_tasks", "args": {}, "map": { "summary": "title", "status": "state" } },
//                   "get_task": …, "create_task": …, "update_task": …, "comment": … } }
// An unknown vocabulary key, a missing name / command, or (at sync) a tool the
// server does not advertise REFUSES with the file and the key — the enum-law
// boundary. Thread contract: manifests are immutable after load; a sync's
// child lives inside the one verb call (cooperative reads; a deadline through
// poll_state + sleep_ms, never a blocking thread).

// The session's sources entity (created on first need): "sources" = the map
// name -> source, "names" = the load order.
long sources_entity(long w)
{
    long se = ui::entity_by_name(w, "nexus-sources");
    if ( se > 0 )
	return se;
    se = ui::create(w, "nexus-sources");
    var none = {};
    ui::set(w, se, "names", none);
    return se;
}

// Load every <dir>/*.mcp.json into the table {name -> source}; false with
// out_err = "<file>: <why>" on the first refusal (nothing replaced).
bool manifests_load(var &out_err, long w, long es, long doc, const char *dir0)
{
    var dir = dir0;
    array files;
    var pat = format("{}/*.mcp.json", dir);
    perl::glob(files, pat.c_str());
    php::sort(files);
    var table;				// name -> source (a null var made the map on its first keyed write)
    var names = {};
    for ( var fpath : files )
    {
	var text;
	if ( !php::file_get_contents(text, fpath.c_str()) )
	{
	    out_err = format("{}: unreadable", fpath);
	    return false;
	}
	var m;
	if ( !js::parse(m, text) )
	{
	    out_err = format("{}: not valid JSON", fpath);
	    return false;
	}
	var name = m["name"];
	if ( name.is_null() || strlen(name.c_str()) == 0 )
	{
	    out_err = format("{}: no name", fpath);
	    return false;
	}
	long tr = mcp_transport_of(m["transport"]);
	if ( tr == mtNONE )
	    tr = mtSTDIO;
	if ( tr != mtSTDIO )
	{
	    out_err = format("{}: transport '{}' is not served yet (stdio only; bridge with mcp-remote)", fpath, m["transport"]);
	    return false;
	}
	var cmd = m["command"];
	if ( cmd.is_null() || php::count(cmd) == 0 )
	{
	    out_err = format("{}: no command", fpath);
	    return false;
	}
	var joined = "";
	for ( var word : cmd )
	{
	    if ( strlen(joined.c_str()) == 0 )
		joined = word;
	    else
		joined = format("{} {}", joined, word);
	}
	var env = m["env"];
	var prefix = "";
	if ( !env.is_null() )
	{
	    if ( strcmp(madc::sys.platform, "win32") == 0 )
	    {
		out_err = format("{}: env: not supported on this platform", fpath);
		return false;
	    }
	    prefix = "env";
	    array ek;
	    php::array_keys(ek, env);
	    for ( var k : ek )
	    {
		var v;
		keyed_get(v, env, k.c_str());
		prefix = format("{} {}={}", prefix, k, v);
	    }
	}
	var full = joined;
	if ( strlen(prefix.c_str()) > 0 )
	    full = format("{} {}", prefix, joined);
	var uri0;
	build_subst(uri0, full.c_str(), w, es, doc);	// {madc} / {project} — the ONE owner
	var uri = format("exec://{}", uri0);
	// the vocabulary: enum-indexed, every key known
	var vocab = m["vocabulary"];
	var ops;				// op-code-key -> {tool, args, map}
	if ( !vocab.is_null() )
	{
	    array vk;
	    php::array_keys(vk, vocab);
	    for ( var k : vk )
	    {
		long op = nexus_op_of(k);
		if ( op == noNONE )
		{
		    out_err = format("{}: unknown vocabulary key '{}'", fpath, k);
		    return false;
		}
		var entry;
		keyed_get(entry, vocab, k.c_str());
		var tool = entry["tool"];
		if ( tool.is_null() )
		{
		    out_err = format("{}: vocabulary '{}' names no tool", fpath, k);
		    return false;
		}
		var okey = format("{}", op);
		ops[okey.c_str()] = entry;
	    }
	}
	var src = { "name": name, "file": fpath, "transport": mcp_transport_name(tr), "uri": uri, "ops": ops };
	table[name.c_str()] = src;
	names.push(name);
    }
    long se = sources_entity(w);
    ui::set(w, se, "sources", table);
    ui::set(w, se, "names", names);
    return true;
}

// nexus.sources: the loaded manifests (names, transports, the tools each maps).
void nexus_sources(var &out, long w)
{
    long se = sources_entity(w);
    var names;
    ui::get(names, w, se, "names");
    var table;
    ui::get(table, w, se, "sources");
    var rows = {};
    if ( !names.is_null() )
	for ( var nm : names )
	{
	    var s;
	    keyed_get(s, table, nm.c_str());
	    if ( s.is_null() )
		continue;
	    var tools = {};
	    var ops = s["ops"];
	    if ( !ops.is_null() )
		for ( long op = noLIST_TASKS; op <= noCOMMENT; op = op + 1 )
		{
		    var okey = format("{}", op);
		    var e;
		    keyed_get(e, ops, okey.c_str());
		    if ( !e.is_null() )
		    {
			var trow = { "op": nexus_op_name(op), "tool": e["tool"] };
			tools.push(trow);
		    }
		}
	    var row = { "name": s["name"], "transport": s["transport"], "file": s["file"], "tools": tools };
	    rows.push(row);
	}
    out = { "rows": rows };
}

// ---- the client ------------------------------------------------------------
// One JSON-RPC request over the child's stdio; the reply matched by id (a
// server notification — no id — is skipped); a dead child (EOF / error) or a
// silent one past `deadline_ms` answers {error, exit_status}. Cooperative: the
// wait is poll_state + sleep_ms, never a blocking read on a silent pipe.
bool mcp_client_call(var &out, madc::channel &c, long &next_id, const char *method, var &params, long deadline_ms)
{
    long id = next_id;
    next_id = next_id + 1;
    var req;
    jsonrpc_request(req, id, method, params);
    var js = js::stringify(req);
    var line = format("{}\n", js);
    if ( !c.write(line.c_str()) )
    {
	var why = c.last_error();
	out = { "error": why, "exit_status": c.exit_status() };
	return false;
    }
    long waited = 0;
    for ( ;; )
    {
	long st = c.poll_state();
	if ( st < 0 )
	{
	    out = { "error": "the server closed its pipe", "exit_status": c.exit_status() };
	    return false;
	}
	if ( st == 0 )
	{
	    if ( waited >= deadline_ms )
	    {
		var why = format("no reply to {} within {} ms", method, deadline_ms);
		out = { "error": why, "exit_status": -1 };
		return false;
	    }
	    madc::sleep_ms(10);
	    waited = waited + 10;
	    continue;
	}
	var r;
	if ( !c.readline(r) )
	{
	    out = { "error": "the server exited", "exit_status": c.exit_status() };
	    return false;
	}
	var resp;
	if ( !js::parse(resp, r) )
	    continue;			// not JSON: a stray line on stdout — skipped, never trusted
	var rid = resp["id"];
	if ( rid.is_null() || rid.as_integer() != id )
	    continue;			// a notification or another id
	var err = resp["error"];
	if ( !err.is_null() )
	{
	    out = { "error": err["message"], "code": err["code"] };
	    return false;
	}
	out = resp["result"];
	return true;
    }
    return false;
}

// A notification (no id, no reply).
void mcp_client_notify(madc::channel &c, const char *method)
{
    var params = {};
    var req;
    jsonrpc_notify(req, method, params);
    var js = js::stringify(req);
    var line = format("{}\n", js);
    c.write(line.c_str());
}

// A tool result's ROWS: the first text content parsed as JSON — an array, or
// an object with `rows`. Empty on anything else.
void tool_result_rows(var &rows, var &result)
{
    rows = {};
    var content = result["content"];
    if ( content.is_null() || php::count(content) == 0 )
	return;
    var item0 = content[0];
    var txt = item0["text"];
    if ( txt.is_null() )
	return;
    var parsed;
    if ( !js::parse(parsed, txt.c_str()) )
	return;
    if ( parsed.is_object() )
    {
	var r = parsed["rows"];
	if ( !r.is_null() )
	    rows = r;
	return;
    }
    rows = parsed;
}

// nexus.sync(source): spawn the manifest's server, handshake, validate the
// vocabulary against tools/list, pull list_tasks, fold each row into a
// `record` event with origin = source (upsert by (origin, ext_key)); close.
void nexus_sync(var &out, long w, long es, long adoc, long self_id, const char *source0)
{
    var source = source0;
    long se = sources_entity(w);
    var table;
    ui::get(table, w, se, "sources");
    var s;
    keyed_get(s, table, source.c_str());
    if ( s.is_null() )
    {
	var why = format("no source '{}' (nexus.sources lists the loaded manifests)", source);
	out = { "error": why };
	return;
    }
    var uri = s["uri"];
    madc::channel c(uri.c_str());
    if ( !c.ok() )
    {
	var why = format("{}: cannot spawn: {}", source, c.last_error());
	out = { "error": why };
	return;
    }
    long next_id = 1;
    var caps = {};
    var info;
    madcide_self_info(info);
    var proto = mcp_protocol_version();
    var iparams = { "protocolVersion": proto, "capabilities": caps, "clientInfo": info };
    var ires;
    if ( !mcp_client_call(ires, c, next_id, "initialize", iparams, 5000) )
    {
	c.close();			// reaps the child: exit_status is real only after it
	long xs = c.exit_status();
	out = { "error": ires["error"], "exit_status": xs, "source": source };
	return;
    }
    mcp_client_notify(c, "notifications/initialized");
    var lparams = {};
    var lres;
    if ( !mcp_client_call(lres, c, next_id, "tools/list", lparams, 5000) )
    {
	c.close();
	long xs = c.exit_status();
	out = { "error": lres["error"], "exit_status": xs, "source": source };
	return;
    }
    var advertised;			// tool name -> 1
    var ltools = lres["tools"];
    if ( !ltools.is_null() )
	for ( var t : ltools )
	{
	    var tn = t["name"];
	    advertised[tn.c_str()] = 1;
	}
    var ops = s["ops"];
    for ( long op = noLIST_TASKS; op <= noCOMMENT; op = op + 1 )
    {
	var okey = format("{}", op);
	var e;
	keyed_get(e, ops, okey.c_str());
	if ( e.is_null() )
	    continue;
	var tool = e["tool"];
	var hit;
	keyed_get(hit, advertised, tool.c_str());
	if ( hit.is_null() )
	{
	    c.close();
	    var why = format("{}: tool '{}' (vocabulary {}) is not advertised by the server", source, tool, nexus_op_name(op));
	    out = { "error": why };
	    return;
	}
    }
    long lop = noLIST_TASKS;		// through a long: an unsigned-char enumerator formats as a CHARACTER
    var lkey = format("{}", lop);
    var lt;
    keyed_get(lt, ops, lkey.c_str());
    if ( lt.is_null() )
    {
	c.close();
	var why = format("{}: the manifest maps no list_tasks", source);
	out = { "error": why };
	return;
    }
    var args = lt["args"];
    if ( args.is_null() )
	args = {};
    var cparams = { "name": lt["tool"], "arguments": args };
    var cres;
    if ( !mcp_client_call(cres, c, next_id, "tools/call", cparams, 10000) )
    {
	c.close();
	long xs = c.exit_status();
	out = { "error": cres["error"], "exit_status": xs, "source": source };
	return;
    }
    c.close_write();
    c.close();
    var rows;
    tool_result_rows(rows, cres);
    var map = lt["map"];
    long created = 0;
    long updated = 0;
    long skipped = 0;
    var f;
    nexus_fold(f, w);
    var recs = f["records"];
    for ( var row : rows )
    {
	// map the result's fields to record fields (a flat rename table)
	var fields;
	long state = rsOPEN;
	var ext_key = "";
	array rk;
	php::array_keys(rk, row);
	for ( var k : rk )
	{
	    var v;
	    keyed_get(v, row, k.c_str());
	    var to = k;
	    if ( !map.is_null() )
	    {
		var m;
		keyed_get(m, map, k.c_str());
		if ( !m.is_null() )
		    to = m;
	    }
	    if ( to == "key" )
		ext_key = v;
	    else if ( to == "state" )
	    {
		long st = record_state_of(v);
		if ( st != rsNONE )
		    state = st;
	    }
	    else
		fields[to.c_str()] = v;
	}
	if ( strlen(ext_key.c_str()) == 0 )
	{
	    skipped = skipped + 1;	// a row with no key cannot be upserted
	    continue;
	}
	long have = 0;
	for ( var idv : f["ids"] )
	{
	    var rkey = format("{}", idv);
	    var r;
	    keyed_get(r, recs, rkey.c_str());
	    if ( r.is_null() )
		continue;
	    if ( r["origin"] == source && r["ext_key"] == ext_key )
	    {
		have = idv.as_integer();
		break;
	    }
	}
	if ( have > 0 )
	{
	    record_update(w, es, adoc, self_id, have, fields, state, roUPDATE);
	    updated = updated + 1;
	}
	else
	{
	    record_create(w, es, adoc, self_id, rkTASK, fields, source.c_str(), ext_key.c_str(), state);
	    created = created + 1;
	}
    }
    long nrows = php::count(rows);
    out = { "ok": true, "source": source, "rows": nrows, "created": created, "updated": updated, "skipped": skipped };
}
