// madcide_serve.inc — the UNIFIED headless serve (client-server V6b-3). One
// `--serve <host:port>` port carries all three seats: the `api` (JSON lines,
// V6a), the `ws` WINDOW (a browser's page.js over an RFC6455 socket, V6b), and
// plain HTTP GETs (the page itself, so a browser can load page.js and connect
// back). madc::channel::serve_web classifies each accepted connection — an api
// JSON line, a WebSocket upgrade, or a page request — and this seat dispatches
// it. Every connection is served CONCURRENTLY on its own cooperative task (the
// V6a serve model), parked on its own socket. Depends on madcide_api.inc
// (serve_api_on) and madcide_ws.inc (serve_ws_client).

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
