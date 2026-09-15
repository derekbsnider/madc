// madcide_seat.inc — the JSON-LINE CONNECTION SEAT (client-server V6a, the
// serve loop; Nexus L4c gave it its second envelope). ONE loop reads one JSON
// line per message off an accepted connection and dispatches by ENVELOPE
// (envelope_of, madcide_enums.inc — read once, a switch on the code):
//   envAPI   {"cmd","args","seq"}    -> api_run (madcide_api.inc), reply {seq, ok, …}
//   envHELLO {"madc":"lsp"}          -> this connection's JSON-RPC dialect (V6c-3c)
//   envRPC   {"jsonrpc","id",…}      -> mcp_handle, or lsp_handle once a hello said so
// so an MCP client (an agent host), an LSP editor (through the attach relay)
// and an api client all speak to ONE `--serve` session over the same socket,
// under the SAME tier roster (grant_tier / clienttier). The engine's serve_web
// already classifies any of these lines as an api client (a `{` first byte).
// Depends on madcide_api.inc, madcide_mcp.inc AND madcide_lsp.inc — included
// after all three; the tests that drive serve_conn_task include them too.
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
    // The JSON-RPC dialect THIS connection speaks. A local, so it is per
    // connection by construction — which is the point: one session serves an
    // editor speaking LSP, an agent speaking MCP and a browser at the same
    // time. MCP until a hello says otherwise, so no existing client moves.
    long protocol = spMCP;
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
	if ( env == envHELLO )
	{
	    // {"madc":"lsp"} — the attach relay declaring its dialect, because
	    // `initialize` exists in both protocols and the first message would
	    // otherwise be ambiguous. Answered, so the relay knows it was heard.
	    var word = req["madc"];
	    long want = seat_protocol_of(word);
	    if ( want == spNONE )
	    {
		var why = format("unknown protocol '{}'", word);
		api_refuse(client, req["seq"].as_integer(), why.c_str());
		continue;
	    }
	    protocol = want;
	    var ack = { "madc": seat_protocol_name(protocol), "ok": true };
	    api_write(client, ack);
	    continue;
	}
	if ( env == envRPC )
	{
	    var resp;
	    if ( protocol == spLSP )
	    {
		// The LSP face over the socket (V6c-3c): the same dispatcher
		// the stdio seat drives, and its NOTIFICATIONS drain the same
		// way — diagnostics and $/madc/* must reach an attached editor,
		// not only a spawned one.
		var notes = {};
		if ( lsp_handle(S, doc, self_id, req, resp, notes) )
		    api_write(client, resp);
		for ( var note : notes )
		    api_write(client, note);
		continue;
	    }
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
// hub fan-out (D2), grant its tier, serve, unshare. One owner of the seat
// setup — serve_conn_task (madcide_serve.inc), where serve_web has just
// classified the connection as an api client, is the one caller.
//
// The tier (V6a tiers): the FIRST api client TO SPEAK owns the session, the
// rest observe until clienttier promotes them. "To speak", not "to connect":
// one --serve port carries api, ws and the page, and serve_web can only tell
// them apart by a connection's first byte, so a connection is classified,
// shared and tiered when its first message arrives — a silent connection is
// not yet an api client and receives no push. (Until the V6 seam the tests
// drove a V6a-era spawner that tiered at accept, so they pinned an order
// production had stopped honouring; they now speak in the order they mean.)
void serve_api_on(IdeSession &S, long doc, madc::channel &client)
{
    long id = client.share();
    madc::conn_set_cookie(id, grant_tier(id));
    serve_line_client(S, doc, client, id);
    client.unshare(id);
}
