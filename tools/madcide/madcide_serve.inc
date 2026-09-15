// madcide_serve.inc — madcide's SERVING PROCESSES, and the one accept loop
// they share (client-server V6b-3, V6c-2/3b/3c, V6c-4). One
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

// THE SERVE FACE as a cooperative task (V6c-3b, generalized in V6c-4): the
// accept loop, on a listener the caller already bound and detached, spawning
// the SAME serve_conn_task every --serve connection gets. ONE accept task
// serves every process that wears a second face — the language server
// (run_lsp) and the editor itself (run_tui) — so there is one loop, not one
// per face. One session, two faces; a second process would be a second session
// on the same file, which is two carets, two undo histories and last-save-wins.
//
// Shutdown: this task parks in accept, and `scope` joins it before its host
// returns, so something must make accept return. Closing the listener's fd
// under a parked poller does not reliably wake it; a CONNECTION does — the
// host sets `srvexit` and pokes its own endpoint once its own loop is over
// (serve_accept_stop), and the flag tells this loop the connection is that
// poke and not a client.
void serve_accept_task(IdeSession *S, long doc, long listen_handle, long w, long es)
{
    madc::channel listener;
    if ( !listener.adopt(listen_handle) )
	return;					// a spent handle — never on this path
    scope
    {
	while ( es_int(w, es, "srvexit", 0) == 0 )
	{
	    if ( !listener.wait_readable() )
		break;				// the listener died
	    madc::channel client;
	    long got = listener.accept(client);
	    if ( got < 0 )
		break;
	    if ( got == 0 )
		continue;			// spurious wake — nothing pending
	    if ( es_int(w, es, "srvexit", 0) != 0 )
		break;				// the exit poke, not a client
	    // A client arriving is exactly when the advertisement's document
	    // list is worth re-publishing (V6c-4): free here, and it is what a
	    // discovering client reads next.
	    session_refresh(*S, doc);
	    long conn = client.detach();
	    go serve_conn_task(S, doc, conn);
	}
    }
    listener.close();
}

// End the accept task so `scope` can join it: set the flag, then make the one
// connection that makes accept return. ONE owner — every face that spawns the
// task ends it the same way.
void serve_accept_stop(long w, long es, var &endpoint)
{
    ui::set(w, es, "srvexit", 1);
    var poke_uri = format("tcp://{}", endpoint);
    madc::channel poke(poke_uri.c_str());
    poke.close();
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
    var endpoint = listener.local_endpoint();	// ring lifetime: owned NOW
    println(stderr, "madcide: serving {} on {} (api + ws + http)", path, endpoint);
    // Publish how to reach this session (V6c-4), so a client discovers it
    // instead of being told its address.
    session_advertise(S, doc, endpoint);

    // THE accept loop — the same function the LSP process and the editor spawn
    // as a task (V6c-4). A headless server has nothing else to do, so it runs
    // it in its own flow rather than beside itself; the loop's own `scope`
    // joins every connection task before it returns, so no serve task touches
    // the session after this.
    serve_accept_task(&S, doc, listener.detach(), S.world(), S.es);
    session_withdraw(S);
    S.close();
    return 0;
}

// ---- the EDITOR that also serves (V6c-4) ---------------------------------
// `madcide <file>` — the TUI (and `--gui`), which by default LISTENS on a
// loopback ephemeral port and advertises it, so an LSP editor, an agent's MCP
// client or a browser can join THIS session instead of opening a second one on
// the same file. Ruling R2 of the V6c-4 plan: nothing is discoverable unless
// the editor listens, so it listens; `--no-serve` (addr == NULL) opts out.
//
// ⚠️ NO AUTH, NO TLS — the existing --serve caveat, now on by default: bind
// loopback and tunnel over ssh, never a public bind.
//
// The session's LIFETIME lives here rather than in the loop, because the accept
// task must be joined BEFORE IdeSession::close() (which drains every task — a
// parked accept would hang that drain). The client loop therefore closes its
// frontends and returns, and the session closes here, after the join.
long run_tui(const char *path, bool ro, ui::level lvl, const char *addr)
{
    IdeSession S;
    long doc = S.open(path, ro);
    if ( doc == 0 )
	println(stderr, "madcide: cannot read {}", path);
    if ( doc <= 0 )
	return 1;			// -1 already reported on stderr
    long w = S.world();
    long es = S.es;

    // Someone else may already hold this file. That is not a refusal — madcide
    // has never refused an open file — it is worth SAYING, with the one command
    // that joins them (ruling R3: a second TUI does not silently become a
    // remote client; that needs the thin client, which is its own slice).
    var other;
    if ( session_find(other, path) )
    {
	var line;
	session_report(line, other);
	var ep = other["endpoint"];
	ui::set(w, es, "msg",
		format("Also open in session {} — join it: madcide {} --lsp --attach {}",
		       line, path, ep));
    }

    // The serve face. Bound HERE so the real endpoint (`:0` asks the OS for a
    // port) is known before the task starts and before it is advertised.
    long listen_handle = 0;
    var endpoint = "";
    if ( addr != NULL )
    {
	var uri = format("listen://{}", addr);
	madc::channel listener(uri.c_str());
	if ( listener.ok() )
	{
	    endpoint = listener.local_endpoint();	// ring lifetime: owned NOW
	    session_advertise(S, doc, endpoint);
	    listen_handle = listener.detach();
	}
	// A port that will not bind is NOT fatal: the editor opens, unadvertised.
    }

    scope
    {
	if ( listen_handle != 0 )
	    go serve_accept_task(&S, doc, listen_handle, w, es);
	run_ide(S, doc, lvl);
	if ( listen_handle != 0 )
	    serve_accept_stop(w, es, endpoint);
    }
    session_withdraw(S);
    S.close();
    return 0;
}

// Every live session, for a human (`madcide --sessions`). Returns 0 when any
// were found — a shell can branch on it.
long run_sessions()
{
    var all;
    session_scan(all);
    long n = php::count(all);
    if ( n == 0 )
    {
	println("no madcide sessions running");
	return 1;
    }
    for ( long i = 0; i < n; i = i + 1 )
    {
	var rec = all[i];
	var line;
	session_report(line, rec);
	println("{}", line);
    }
    println("");
    println("join one:  madcide <file> --lsp --attach <endpoint>   (or --mcp)");
    println("           madcide <file> --lsp --attach              (discovers)");
    println("⚠️  a madcide port has NO AUTH and NO TLS: loopback only, tunnel over ssh.");
    return 0;
}

// ---- the LSP-wearing processes (V6c-2 / V6c-3b / V6c-3c) -----------------
// madcide's serving PROCESSES live here, beside run_serve and run_tui: the
// language server (run_lsp), the second face it can wear at the same time (the
// shared serve_accept_task — one process, one session, LSP on stdio AND
// api/ws/http on a port), and the attach relay (run_attach — become a client of
// a session that is already running). The LSP dispatcher itself is
// madcide_lsp.inc; these are the entry points that wear it, and they land after
// serve_conn_task because that is what the serve face spawns per connection.

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
	println(stderr, "madcide: serving {} on {} (api + ws + http) beside --lsp",
		path, endpoint);
	// Publishes the endpoint on the session bag (`advertep`) as well as to the
	// state directory — the ONE slot $/madc/serve announces from.
	session_advertise(S, doc, endpoint);	// discoverable (V6c-4)
	listen_handle = listener.detach();
    }

    scope
    {
	if ( listen_handle != 0 )
	    go serve_accept_task(&S, doc, listen_handle, w, es);
	lsp_stdio_loop(S, doc, io, w, es);
	if ( listen_handle != 0 )
	    serve_accept_stop(w, es, endpoint);
    }
    session_withdraw(S);
    // The spec's own verdict: exit after a shutdown is success, without one
    // is an error the client reports.
    long rc = es_int(w, es, "lspdown", 0) != 0 ? 0 : 1;
    io.close();
    S.close();
    return rc;
}
