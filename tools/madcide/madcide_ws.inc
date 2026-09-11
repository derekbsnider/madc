// madcide_ws.inc — the trWS CLIENT SEAT (client-server design §2.7, slice
// V6b-2). A ws client is a WINDOW, not a headless command channel: it drives
// the SAME compose -> render -> event -> apply cycle the local window does
// (madcide_client.inc), only the frontend is the ws host (<ns_ui_ws>) over an
// RFC6455 channel — DOM ops down the socket, the page's event JSON back up
// into ui::post_event. run_ide / compose_ide_tree / web_model / page.js /
// apply_ide_event are all reused; this file is only the transport seat.
//
// Unlike the api seat (madcide_api.inc, which drives IdeSession::command by
// name), the ws seat renders the composed tree and applies UI events — a
// remote WINDOW. Each accepted connection is served CONCURRENTLY by its own
// cooperative task (the V6a serve model), parked on its ws readline; the
// shared session is driven one task at a time between yields.

// Serve ONE ws-upgraded client as a web window over the shared session. The
// channel is already past the handshake; bind it to a ui::WEB frontend (by
// name, so it never collides with a local webview host), give the client its
// own view (an independent caret over the shared world), and run the window
// loop until the socket closes. The channel stays the caller's — close it
// there, after this returns.
void serve_ws_client(IdeSession &S, long doc, madc::channel &client)
{
    ui_ws::attach(&client);		// hand the socket to the next open("ws")
    long t = ui::open("ws");
    if ( t == 0 )
	return;				// open refused (reason on stderr)
    long w = S.world();
    long es = S.spawn_view(doc);		// this client's own es (its caret)
    if ( es == 0 )
    {
	ui::close(t);
	return;
    }
    long gen = -1;			// this client's key-table generation cache
    bool run = true;
    while ( run )
    {
	S.attach(w, es);
	// A window, never a terminal: build output lands in the panel region,
	// the terminal-suspend path stays refused (client_service handles it).
	S.terminal(false);
	S.dialogs(ui::dialogs(t));
	S.panel(true);
	S.viewport(ui::cols(t), ui::rows(t));
	var pc;
	ui::pending(pc, t);
	S.pending_chord(pc.is_null() ? "" : pc.c_str());
	long adoc = S.active(doc);
	var tree;
	S.compose_ide_tree(tree, adoc);
	ui::render(t, w, tree);			// DOM ops -> one ws frame
	client_bind_keys(S, t, gen);
	// ui::event pumps the ws host's run() (readline PARKS the task); a
	// background parse streams to the client through read_events' wake
	// synthesis, so no busy re-render loop is needed here (unlike the
	// local roster, which must not flood a socket with every parse tick).
	var ev;
	if ( !ui::event(ev, t, w) )
	    break;				// the socket closed
	run = S.apply_ide_event(adoc, ev);
	client_service(S, t);			// drain the one parked request
    }
    ui::close(t);
}

// One spawnable ws client task (V6b-2): adopt the handed-off connection,
// perform the SERVER-role WebSocket upgrade (the HTTP read parks this task,
// never the OS thread), and run the window seat until the client hangs up.
// `S` rides as a POINTER — `go` carries only long/double/pointer slots — and
// the session outlives every task (the serve loop joins them before it
// returns).
void serve_ws_task(IdeSession *S, long doc, long conn)
{
    madc::channel client;
    if ( !client.adopt(conn) )
	return;				// a spent handle — never on this path
    if ( !client.upgrade_websocket() )
    {
	client.close();			// not a WebSocket client (reason logged)
	return;
    }
    serve_ws_client(*S, doc, client);
    client.close();
}
