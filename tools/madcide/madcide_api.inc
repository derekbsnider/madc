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
void handle_clienttier(madc::channel &client, long seq, long self_id, const char *arg)
{
    if ( eff_tier(self_id) < tierOWNER )
    {
	api_refuse(client, seq, "clienttier requires owner");
	return;
    }
    var parts;
    php::explode(parts, " ", arg);
    if ( php::count(parts) < 2 )
    {
	api_refuse(client, seq, "clienttier <id> <tier>");
	return;
    }
    long target = php::intval(parts[0]);
    long tier = tier_of_name(parts[1].c_str());
    if ( tier < 0 )
    {
	var why = format("unknown tier '{}'", parts[1]);
	api_refuse(client, seq, why.c_str());
	return;
    }
    if ( madc::conn_cookie(target) < 0 )
    {
	var why = format("no client {}", target);
	api_refuse(client, seq, why.c_str());
	return;
    }
    madc::conn_set_cookie(target, tier);
    var reply = { "seq": seq, "ok": true,
	"clienttier": target, "tier": tier_name(tier) };
    api_write(client, reply);
}

// Serve ONE connected client: read JSON-line requests until EOF, running each
// on the shared session, replying with the verdict + the composed projection
// (the same tree ui::render_tree typesets for the -c seat), and — when the
// command advanced the change log — pushing the new events to the OTHER
// clients (V6a duplex D2). `self_id` is this client's share id.
void serve_api_client(IdeSession &S, long doc, madc::channel &client, long self_id)
{
    long w = S.world();
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
	// clienttier <id> <tier> — an OWNER verb the SEAT owns (it re-tiers a
	// connection, not the document); handled before the editor registry.
	if ( strcmp(name.c_str(), "clienttier") == 0 )
	{
	    handle_clienttier(client, seq, self_id, api_arg(req));
	    continue;
	}
	long code = cmd_of(name.c_str());
	if ( code == cmdNONE )
	{
	    var why = format("unknown command '{}'", name);
	    api_refuse(client, seq, why.c_str());
	    continue;
	}
	// Permission gate (V6a tiers): a command above this client's tier is
	// refused with prose, never run silently.
	long my_tier = eff_tier(self_id);
	long need = cmd_min_tier(code);
	if ( my_tier < need )
	{
	    var why = format("'{}' requires {} (you are {})",
		    name, tier_name(need), tier_name(my_tier));
	    api_refuse(client, seq, why.c_str());
	    continue;
	}
	long adoc = S.active(doc);
	long head_before = clog_head(w, adoc);
	bool cont = true;
	bool ok = S.command(adoc, code, api_arg(req), cont);
	long errors = S.error_count();
	var tree;
	S.compose_ide_tree(tree, S.active(adoc));
	var text;
	ui::render_tree(text, S.world(), tree);
	var reply = { "seq": seq, "ok": ok, "errors": errors, "text": text };
	api_write(client, reply);
	// An editing command advanced the change log — push the new events to
	// the other connected clients (the hub fan-out, D2).
	if ( clog_head(w, adoc) > head_before )
	    broadcast_events(w, adoc, head_before, self_id);
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
void serve_client_task(IdeSession *S, long doc, long conn)
{
    madc::channel client;
    if ( !client.adopt(conn) )
	return;			// a spent handle — never on this path
    // Share this client's WRITE side so a peer's task can push change events
    // down it (the hub fan-out, D2); unshare before close so no broadcast
    // ever reaches a torn-down channel.
    long id = client.share();
    // Grant its permission tier (V6a tiers): the first connection owns the
    // session, the rest observe until an owner promotes them (clienttier).
    madc::conn_set_cookie(id, grant_tier(id));
    serve_api_client(*S, doc, client, id);
    client.unshare(id);
    client.close();
}

// The trAPI SERVER: open the session headless (no window), bind a listener,
// and serve every accepted client CONCURRENTLY — each on its own cooperative
// task parked on the reactor-backed channel.readline (design §2.7: cooperative
// tasks, no thread). `addr` is the listen authority (host:port; :0 asks the OS
// for an ephemeral port, which the startup line reports). Returns 1 only when
// the file or the listener could not open (the reason on stderr); the serve
// loop itself runs until the listener dies (the process is stopped).
long run_serve(const char *path, bool ro, const char *addr)
{
    IdeSession S;
    long doc = S.open(path, ro);
    if ( doc == 0 )
	println(stderr, "madcide: cannot read {}", path);
    if ( doc <= 0 )
	return 1;			// -1 already reported on stderr
    S.terminal(false);		// headless: no interactive terminal

    var uri = format("listen://{}", addr);
    madc::channel listener(uri.c_str());
    if ( !listener.ok() )
    {
	println(stderr, "madcide: cannot serve on {}: {}",
		addr, listener.last_error());
	S.close();
	return 1;
    }
    println(stderr, "madcide: serving {} on {} (api)",
	    path, listener.local_endpoint());

    // The accept loop parks on the listener between connections, so the
    // scheduler runs the parked client tasks meanwhile; `scope` joins every
    // client task before S tears down — no serve task may touch the session
    // after run_serve returns.
    scope
    {
	bool serving = true;
	while ( serving )
	{
	    if ( !listener.wait_readable() )
		break;			// the listener died
	    madc::channel client;
	    long got = listener.accept(client);
	    if ( got < 0 )
		break;			// accept failed
	    if ( got == 0 )
		continue;			// spurious wake — nothing pending
	    long conn = client.detach();	// hand the stream off as a long
	    go serve_client_task(&S, doc, conn);
	}
    }
    S.close();
    return 0;
}
