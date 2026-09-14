// madcide_serve.inc — the UNIFIED headless serve (client-server V6b-3). One
// `--serve <host:port>` port carries all three seats: the `api` (JSON lines,
// V6a), the `ws` WINDOW (a browser's page.js over an RFC6455 socket, V6b), and
// plain HTTP GETs (the page itself, so a browser can load page.js and connect
// back). madc::channel::serve_web classifies each accepted connection — an api
// JSON line, a WebSocket upgrade, or a page request — and this seat dispatches
// it. Every connection is served CONCURRENTLY on its own cooperative task (the
// V6a serve model), parked on its own socket. Depends on madcide_seat.inc
// (serve_api_on — the JSON-line seat, api AND JSON-RPC envelopes) and
// madcide_ws.inc (serve_ws_client).

// One accepted connection, classified by serve_web and dispatched: a WebSocket
// window, an api client, or a page GET (already answered — nothing more). The
// page is the engine's embedded page.js host document (ui::page_html); the
// transport stays page-agnostic, the seat supplies what to serve.
void serve_conn_task(IdeSession *S, long doc, long conn)
{
    madc::channel client;
    if ( !client.adopt(conn) )
	return;				// a spent handle — never on this path
    var page;
    ui::page_html(page);
    long kind = client.serve_web(page.c_str());
    if ( kind == 1 )			// a WebSocket WINDOW (client is now framed)
	serve_ws_client(*S, doc, client);
    else if ( kind == 2 )		// an api JSON client
	serve_api_on(*S, doc, client);
    // kind == 0 (the page was served) or kind < 0 (error): nothing more.
    client.close();
}

// The headless server: open the session with no window, bind a listener, and
// serve every accepted connection concurrently (design §2.7: cooperative
// tasks, no thread). `addr` is the listen authority (host:port; :0 asks the OS
// for an ephemeral port, reported on the startup line). Returns 1 only when
// the file or the listener could not open; the serve loop runs until the
// listener dies (the process is stopped).
long run_serve(const char *path, bool ro, const char *addr)
{
    IdeSession S;
    long doc = S.open(path, ro);
    if ( doc == 0 )
	println(stderr, "madcide: cannot read {}", path);
    if ( doc <= 0 )
	return 1;			// -1 already reported on stderr
    S.terminal(false);			// headless: no interactive terminal

    var uri = format("listen://{}", addr);
    madc::channel listener(uri.c_str());
    if ( !listener.ok() )
    {
	println(stderr, "madcide: cannot serve on {}: {}",
		addr, listener.last_error());
	S.close();
	return 1;
    }
    println(stderr, "madcide: serving {} on {} (api + ws + http)",
	    path, listener.local_endpoint());

    // The accept loop parks on the listener between connections, so the
    // scheduler runs the parked connection tasks meanwhile; `scope` joins
    // every task before S tears down — no serve task may touch the session
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
	    go serve_conn_task(&S, doc, conn);
	}
    }
    S.close();
    return 0;
}

// ---- the LSP-wearing processes (V6c-2 / V6c-3b / V6c-3c) -----------------
// madcide's serving PROCESSES live here, beside run_serve: the language server
// (run_lsp), the second face it can wear at the same time (lsp_serve_task —
// one process, one session, LSP on stdio AND api/ws/http on a port), and the
// attach relay (run_attach — become a client of a session that is already
// running). The LSP dispatcher itself is madcide_lsp.inc; these are the
// entry points that wear it, and they land after serve_conn_task because that
// is what the serve face spawns per connection.

// ---- the server ---------------------------------------------------------
// `madcide <file> --lsp`: the standard deployment — an editor spawns this
// process and speaks over the pipe. stdio is a CHANNEL (stdio://) and the
// framing is the channel's (frame_headers), so this loop is the MCP seat's
// loop with one framing swapped. The lone stdio client is the operator:
// self_id -1 (untracked -> owner via eff_tier), so it may edit and save.

// The message loop itself — ONE loop, whether or not this process is also
// wearing the serve face (two copies would be two protocols).
void lsp_stdio_loop(IdeSession &S, long doc, madc::channel &io, long w, long es)
{
    var line;
    while ( io.readline(line) )
    {
	var req;
	var resp;
	var notes = {};
	if ( !js::parse(req, line.c_str()) )
	{
	    var nullid;
	    lsp_error(resp, nullid, -32700, "Parse error");
	    var js = js::stringify(resp);
	    io.write(js.c_str());
	    continue;
	}
	if ( lsp_handle(S, doc, -1, req, resp, notes) )
	{
	    var js = js::stringify(resp);
	    io.write(js.c_str());
	}
	long n = php::count(notes);
	for ( long i = 0; i < n; i = i + 1 )
	{
	    var note = notes[i];
	    var js = js::stringify(note);
	    io.write(js.c_str());
	}
	if ( es_int(w, es, "lspexit", 0) != 0 )
	    break;				// the client said exit
    }
}

// The SERVE FACE of an LSP process (V6c-3b): run_serve's accept loop, on a
// listener the caller already bound and detached, spawning the SAME
// serve_conn_task every --serve connection gets. One session, two faces — a
// second process would be a second session on the same file, which is two
// carets, two undo histories and last-save-wins.
//
// Shutdown: this task parks in accept, and `scope` joins it before run_lsp
// returns, so something must make accept return. Closing the listener's fd
// under a parked poller does not reliably wake it; a CONNECTION does — run_lsp
// pokes its own endpoint once the LSP client has said exit, and the flag tells
// this loop the connection is that poke and not a client.
void lsp_serve_task(IdeSession *S, long doc, long listen_handle, long w, long es)
{
    madc::channel listener;
    if ( !listener.adopt(listen_handle) )
	return;					// a spent handle — never on this path
    scope
    {
	while ( es_int(w, es, "lspexit", 0) == 0 )
	{
	    if ( !listener.wait_readable() )
		break;				// the listener died
	    madc::channel client;
	    long got = listener.accept(client);
	    if ( got < 0 )
		break;
	    if ( got == 0 )
		continue;			// spurious wake — nothing pending
	    if ( es_int(w, es, "lspexit", 0) != 0 )
		break;				// the exit poke, not a client
	    long conn = client.detach();
	    go serve_conn_task(S, doc, conn);
	}
    }
    listener.close();
}

// ---- the ATTACH RELAY (V6c-3c) -------------------------------------------
// `madcide <file> --lsp --attach <addr>` (and `--mcp --attach <addr>`): the
// editor or the agent host spawns this process as it always does, but instead
// of opening a PRIVATE session it becomes a client of one that is already
// running. That is the gatekeeper shape — VS Code, an agent's MCP client and a
// browser window all driving one session's buffers.
//
// This relay opens NO session: it is pure transport. It holds no protocol
// knowledge beyond its OWN LIFECYCLE, which is the one thing it must not
// forward — `shutdown` and `exit` end this CONNECTION, and a session other
// clients are still on must outlive one editor quitting.
//
// Two cooperative pumps, so a server push and a client request never wait on
// each other. `<file>` is accepted on the command line (the CLI's shape) and
// unused here: the session already has its documents, and an editor adds any
// others with didOpen.

// The session -> client pump: whatever the seat sends, framed back to stdio.
void relay_down(madc::channel *sock, madc::channel *io, bool framed)
{
    var line;
    while ( sock->readline(line) )
    {
	if ( framed )
	    io->write(line.c_str());
	else
	{
	    var out = format("{}\n", line);
	    io->write(out.c_str());
	}
    }
    io->close_write();			// the session is gone: say so downstream
}

long run_attach(const char *addr, bool framed)
{
    madc::channel io("stdio://");
    if ( !io.ok() || (framed && !io.frame_headers()) )
    {
	println(stderr, "madcide --attach: {}", io.last_error());
	return 1;
    }
    var uri = format("tcp://{}", addr);
    madc::channel sock(uri.c_str());
    if ( !sock.ok() )
    {
	println(stderr, "madcide: cannot attach to {}: {}", addr, sock.last_error());
	return 1;
    }
    // Declare the dialect: `initialize` exists in both protocols, so the seat
    // must be told rather than left to guess at the first message.
    var hello = { "madc": framed ? seat_protocol_name(spLSP)
				 : seat_protocol_name(spMCP) };
    var hjs = js::stringify(hello);
    var hline = format("{}\n", hjs);
    sock.write(hline.c_str());
    var ack;
    sock.readline(ack);			// the seat's acknowledgement
    println(stderr, "madcide: attached to {} ({})", addr, hjs);

    bool down = false;			// a shutdown arrived before exit
    scope
    {
	go relay_down(&sock, &io, framed);
	var line;
	while ( io.readline(line) )
	{
	    // The only messages this relay reads rather than carries: its own
	    // lifecycle. Forwarding them would ask a SHARED session to shut
	    // down because one editor closed.
	    var req;
	    if ( js::parse(req, line.c_str()) )
	    {
		long m = lsp_method_of(req["method"]);
		if ( m == lmSHUTDOWN )
		{
		    var idv = req["id"];
		    var nothing;
		    var resp;
		    lsp_reply(resp, idv, nothing);
		    var js = js::stringify(resp);
		    io.write(js.c_str());
		    down = true;
		    continue;
		}
		if ( m == lmEXIT )
		    break;
	    }
	    var out = format("{}\n", line);
	    sock.write(out.c_str());
	}
	// The down-pump is PARKED on the socket, and `scope` joins it before
	// this returns. Closing the fd under a parked poller does not reliably
	// wake it — half-closing does the job properly: the seat reads EOF,
	// ends this connection's loop and closes its end, which is a real
	// readable event, so the pump returns. (lsp_serve_task's accept park
	// has the same shape and solves it with a self-connect poke.)
	sock.close_write();
    }
    sock.close();
    io.close();
    // The LSP rule, for an LSP relay; an MCP host just closes stdin.
    return framed && !down ? 1 : 0;
}

long run_lsp(const char *path, bool ro, const char *serve)
{
    IdeSession S;
    long doc = S.open(path, ro);
    if ( doc == 0 )
	println(stderr, "madcide: cannot read {}", path);
    if ( doc <= 0 )
	return 1;
    S.terminal(false);				// headless: no interactive terminal
    long w = S.world();
    long es = S.es;
    long enc0 = peUTF16;			// an ENUMERATOR into a slot goes
    lsp_set_encoding(w, es, -1, enc0);		// through a long (the dialect's rule)

    madc::channel io("stdio://");
    if ( !io.ok() || !io.frame_headers() )
    {
	println(stderr, "madcide --lsp: {}", io.last_error());
	S.close();
	return 1;
    }

    // The serve face, when asked for. The listener is bound HERE so its real
    // endpoint (`:0` asks the OS for a port) is known before the task starts
    // and can be told to the client in-protocol ($/madc/serve, on initialized).
    long listen_handle = 0;
    var endpoint = "";
    if ( serve != NULL )
    {
	var uri = format("listen://{}", serve);
	madc::channel listener(uri.c_str());
	if ( !listener.ok() )
	{
	    println(stderr, "madcide --lsp: cannot serve on {}: {}",
		    serve, listener.last_error());
	    io.close();
	    S.close();
	    return 1;
	}
	endpoint = listener.local_endpoint();	// ring lifetime: owned NOW
	ui::set(w, es, "lspserve", endpoint);
	println(stderr, "madcide: serving {} on {} (api + ws + http) beside --lsp",
		path, endpoint);
	listen_handle = listener.detach();
    }

    scope
    {
	if ( listen_handle != 0 )
	    go lsp_serve_task(&S, doc, listen_handle, w, es);
	lsp_stdio_loop(S, doc, io, w, es);
	if ( listen_handle != 0 )
	{
	    // Wake the accept park so `scope` can join it (see lsp_serve_task).
	    ui::set(w, es, "lspexit", 1);
	    var poke_uri = format("tcp://{}", endpoint);
	    madc::channel poke(poke_uri.c_str());
	    poke.close();
	}
    }
    // The spec's own verdict: exit after a shutdown is success, without one
    // is an error the client reports.
    long rc = es_int(w, es, "lspdown", 0) != 0 ? 0 : 1;
    io.close();
    S.close();
    return rc;
}
