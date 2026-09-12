// madcide_api.inc — the trAPI CLIENT SEAT (client-server design 2026-09-09
// §2.7, slice V6a). A madcide session with ZERO windows serving remote
// clients over a byte channel: one JSON line per message. A request is
//   {"cmd":"<name>", "args":"<arg>", "seq":<n>}
// and a reply is
//   {"seq":<n>, "ok":<bool>, "errors":<n>, "text":"<projection>"}
// or, on refusal,
//   {"seq":<n>, "ok":false, "error":"<why>"}.
// The command NAME resolves ONCE here, the channel boundary, through the
// registry's one table (cmd_of — the SAME table the profiles, menus and the
// -c one-shot load through: separation of concerns, Rule #5); the rest is
// the command's ARGUMENT, exactly as IdeSession::command takes it. The seat
// never learns a command's grammar — it drives the session directly, as the
// headless harness and run_once do. The MCP seat and the LSP endpoint (V6c)
// are adapters over THIS protocol.
//
// Transport: the accepted connection is an ordinary madc::channel byte
// stream (the listen:// acceptor, V6a step 1/2a). Every accepted client is
// served CONCURRENTLY — one cooperative task per connection, each parked on
// the reactor-backed channel.readline (V6a duplex, D1). Still to come on this
// substrate: the {"event":…} push feed off the V4 change-event log (a client's
// last_seq cursor) and the permission tiers. A remote ":q" ends only that
// client's connection, never the server (cont is not honored here — a client
// leaves by disconnecting).

// Read the request's optional text ARGUMENT as a C string ("" when absent).
const char *api_arg(var &req)
{
    var a = req["args"];
    if ( a.is_null() )
	return "";
    return a.c_str();
}

// Write one JSON line (the JSONL framing the client reads with a line read).
void api_write(madc::channel &client, var &reply)
{
    var js = js::stringify(reply);
    var line = format("{}\n", js);
    client.write(line.c_str());
}

// A refusal reply — never a silent drop (the -c seat's contract over a
// channel): the request's seq rides back so the client can correlate.
void api_refuse(madc::channel &client, long seq, const char *why)
{
    var reply = { "seq": seq, "ok": false, "error": why };
    api_write(client, reply);
}

// Fan the change events past `since` on `doc` out to every OTHER connected
// api client — one {"event":rec} JSON line each — the hub broadcast-at-append
// (V6a duplex D2). `self_id` is the acting client's share id, excluded so it
// sees its edit only through its own reply, never echoed back as an event.
void broadcast_events(long w, long doc, long since, long self_id)
{
    var evs;
    events_since(evs, w, doc, since);
    for ( var e : evs )
    {
	var push = { "event": e };
	var js = js::stringify(push);
	var pl = format("{}\n", js);
	madc::conn_broadcast(self_id, pl.c_str());
    }
}

// The tier a newly-connected client is granted (V6a tiers): OWNER if the
// session has no live owner yet — the operator who launched --serve connects
// first — else OBSERVER, promotable later by an owner via clienttier. (The
// design's "local=admin" has no local window in a headless server, so
// first-owner stands in.) `self_id` is already share()d (cookie still 0), so
// the scan of the roster only finds an owner among the OTHER connections.
long grant_tier(long self_id)
{
    var ids;
    madc::conn_ids(ids);
    for ( var idv : ids )
    {
	long cid = idv.as_integer();
	if ( cid == self_id )
	    continue;
	if ( madc::conn_cookie(cid) == tierOWNER )
	    return tierOBSERVER;
    }
    return tierOWNER;
}

// The EFFECTIVE tier of a connection id. An untracked id (-1 — a direct,
// local caller such as the -c / headless-harness seat that never shared) is
// the operator itself: full rights (owner). A real network client always has
// a share()d id with a granted tier, so this only widens the local path.
long eff_tier(long id)
{
    long t = madc::conn_cookie(id);
    return t < 0 ? tierOWNER : t;
}

// clienttier <id> <tier> — the OWNER verb that re-tiers a connection (it acts
// on the SEAT's connections, not the document, so the seat owns it, not the
// editor registry). Owner-only; a bad id or tier spelling refuses with prose.
// Fills `out` with the reply BODY (the "seq" is the wire seat's to stamp, V6c):
// a transport-agnostic result the api seat and the MCP adapter both render. An
// out-parameter, the dialect's carrier-return convention (pre-L3 value-return).
void handle_clienttier(var &out, long self_id, const char *arg)
{
    if ( eff_tier(self_id) < tierOWNER )
    {
	out = { "ok": false, "error": "clienttier requires owner" };
	return;
    }
    var parts;
    php::explode(parts, " ", arg);
    if ( php::count(parts) < 2 )
    {
	out = { "ok": false, "error": "clienttier <id> <tier>" };
	return;
    }
    long target = php::intval(parts[0]);
    long tier = tier_of_name(parts[1].c_str());
    if ( tier < 0 )
    {
	var why = format("unknown tier '{}'", parts[1]);
	out = { "ok": false, "error": why };
	return;
    }
    if ( madc::conn_cookie(target) < 0 )
    {
	var why = format("no client {}", target);
	out = { "ok": false, "error": why };
	return;
    }
    madc::conn_set_cookie(target, tier);
    out = { "ok": true, "clienttier": target, "tier": tier_name(tier) };
}

// Run ONE command by NAME on the shared session and return the reply BODY —
// the transport-agnostic api core (V6c). The command NAME resolves ONCE here
// (cmd_of, the registry's one table); the tier gate refuses a command above
// the caller's tier with prose; a mutating command fans its new events to the
// OTHER connected clients (the hub broadcast, D2). Every seat over `api` — the
// JSON-line api seat AND the MCP adapter (madcide_mcp.inc) — drives a command
// through HERE, so there is ONE command core, not one per transport. The
// returned body carries "ok" plus either "errors"+"text" (ran), "error"
// (refused/unknown), or the clienttier fields; the wire seat stamps "seq" /
// wraps it in its own envelope. `self_id` is the caller's share id (-1 = a
// local, untracked caller — the operator, full rights via eff_tier).
void api_run(var &out, IdeSession &S, long doc, long self_id, const char *name, const char *arg)
{
    // clienttier <id> <tier> — an OWNER verb the SEAT owns (it re-tiers a
    // connection, not the document); handled before the editor registry.
    if ( strcmp(name, "clienttier") == 0 )
    {
	handle_clienttier(out, self_id, arg);
	return;
    }
    long code = cmd_of(name);
    if ( code == cmdNONE )
    {
	var why = format("unknown command '{}'", name);
	out = { "ok": false, "error": why };
	return;
    }
    // Permission gate (V6a tiers): a command above this caller's tier is
    // refused with prose, never run silently.
    long my_tier = eff_tier(self_id);
    long need = cmd_min_tier(code);
    if ( my_tier < need )
    {
	var why = format("'{}' requires {} (you are {})",
		name, tier_name(need), tier_name(my_tier));
	out = { "ok": false, "error": why };
	return;
    }
    long w = S.world();
    long adoc = S.active(doc);
    long head_before = clog_head(w, adoc);
    bool cont = true;
    bool ok = S.command(adoc, code, arg, cont);
    long errors = S.error_count();
    var tree;
    S.compose_ide_tree(tree, S.active(adoc));
    var text;
    ui::render_tree(text, S.world(), tree);
    out = { "ok": ok, "errors": errors, "text": text };
    // An editing command advanced the change log — push the new events to the
    // other connected clients (the hub fan-out, D2).
    if ( clog_head(w, adoc) > head_before )
	broadcast_events(w, adoc, head_before, self_id);
}

// Serve ONE connected client: read JSON-line requests until EOF, running each
// through api_run (the shared command core) on the shared session, and writing
// the reply body back as a JSON line with this request's `seq` stamped on it.
// The command semantics — name resolution, the tier gate, the compose, the
// hub fan-out — live in api_run; the seat owns only the JSON-LINE envelope
// (parse a line, read seq/cmd, stamp seq on the reply). `self_id` is this
// client's share id.
void serve_api_client(IdeSession &S, long doc, madc::channel &client, long self_id)
{
    var line;
    while ( client.readline(line) )
    {
	var req;
	if ( !js::parse(req, line) )
	{
	    api_refuse(client, 0, "malformed JSON request");
	    continue;
	}
	long seq = req["seq"].as_integer();
	var name = req["cmd"];
	if ( name.is_null() )
	{
	    api_refuse(client, seq, "request has no 'cmd'");
	    continue;
	}
	var reply;
	api_run(reply, S, doc, self_id, name.c_str(), api_arg(req));
	reply["seq"] = seq;
	api_write(client, reply);
    }
}

// One spawnable client task (V6a duplex): adopt the handed-off connection
// and run the seat loop on it until the client hangs up. `S` rides as a
// POINTER — `go` carries only long/double/pointer slots, not the non-copyable
// channel or a session reference — and the session outlives every task
// (run_serve's `scope` joins them before it returns, so `&S` never dangles).
// Cooperative: only one task runs at a time and yields only inside readline,
// so a client's command runs to completion against the shared session between
// yields (the single-thread task contract — no lock, the thread-safety law's
// stated cooperative contract).
// Run the api seat over an ALREADY-ADOPTED channel: share its write side for
// the hub fan-out (D2), grant its tier (V6a tiers: first connection owns the
// session, the rest observe until clienttier promotes them), serve, unshare.
// One owner of the seat setup — both the plain api serve task and the unified
// serve (madcide_serve.inc, where serve_web detects an api client) drive a
// connection through here.
void serve_api_on(IdeSession &S, long doc, madc::channel &client)
{
    long id = client.share();
    madc::conn_set_cookie(id, grant_tier(id));
    serve_api_client(S, doc, client, id);
    client.unshare(id);
}

void serve_client_task(IdeSession *S, long doc, long conn)
{
    madc::channel client;
    if ( !client.adopt(conn) )
	return;			// a spent handle — never on this path
    serve_api_on(*S, doc, client);
    client.close();
}
