// madcide_seat.inc — the JSON-LINE CONNECTION SEAT (client-server V6a, the
// serve loop; Nexus L4c gave it its second envelope). ONE loop reads one JSON
// line per message off an accepted connection and dispatches by ENVELOPE
// (envelope_of, madcide_enums.inc — read once, a switch on the code):
//   envAPI  {"cmd","args","seq"}     -> api_run (madcide_api.inc), reply {seq, ok, …}
//   envRPC  {"jsonrpc","id",…}       -> mcp_handle (madcide_mcp.inc), the JSON-RPC reply
// so an MCP client (an agent host) speaks to `--serve` over the same socket the
// api client uses, under the SAME tier roster (grant_tier / clienttier). The
// engine's serve_web already classifies either line as an api client (a `{`
// first byte). Depends on madcide_api.inc AND madcide_mcp.inc — included after
// both; the tests that drive serve_client_task include all three.
//
// Transport: the accepted connection is an ordinary madc::channel byte stream
// (the listen:// acceptor, V6a step 1/2a). Every accepted client is served
// CONCURRENTLY — one cooperative task per connection, each parked on the
// reactor-backed channel.readline (V6a duplex, D1). A remote ":q" ends only
// that client's connection, never the server (a client leaves by
// disconnecting).

// Serve ONE connected client: read JSON-line requests until EOF, dispatch each
// by envelope on the shared session, and write the reply back as a JSON line —
// the api reply with this request's `seq` stamped on it, or the JSON-RPC reply
// (a notification has none). The command semantics — name resolution, the tier
// gate, the compose, the hub fan-out — live in api_run / mcp_handle; the seat
// owns only the line envelope. `self_id` is this client's share id.
void serve_line_client(IdeSession &S, long doc, madc::channel &client, long self_id)
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
	long env = envelope_of(req);
	if ( env == envRPC )
	{
	    var resp;
	    if ( mcp_handle(S, doc, self_id, req, resp) )	// a notification has no reply
		api_write(client, resp);
	    continue;
	}
	long seq = req["seq"].as_integer();
	if ( env != envAPI )
	{
	    api_refuse(client, seq, "request has no 'cmd'");
	    continue;
	}
	var name = req["cmd"];
	var reply;
	api_run(reply, S, doc, self_id, name.c_str(), api_arg(req));
	reply["seq"] = seq;
	api_write(client, reply);
    }
}

// Run the seat over an ALREADY-ADOPTED channel: share its write side for the
// hub fan-out (D2), grant its tier (V6a tiers: first connection owns the
// session, the rest observe until clienttier promotes them), serve, unshare.
// One owner of the seat setup — both the plain serve task and the unified
// serve (madcide_serve.inc, where serve_web detects a JSON client) drive a
// connection through here.
void serve_api_on(IdeSession &S, long doc, madc::channel &client)
{
    long id = client.share();
    madc::conn_set_cookie(id, grant_tier(id));
    serve_line_client(S, doc, client, id);
    client.unshare(id);
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
    serve_api_on(*S, doc, client);
    client.close();
}
