// madcide_serve.inc — madcide's SERVING PROCESSES, and the one accept loop
// they share (client-server V6b-3, V6c-3b/3c, V6c-4; split arc 1D). One
// `--serve <host:port>` port carries all three seats: the `api` (JSON lines,
// V6a, and MCP's JSON-RPC over the same lines), the `ws` WINDOW (a browser's
// page.js over an RFC6455 socket, V6b), and plain HTTP GETs (the page
// itself, so a browser can load page.js and connect back).
// madc::channel::serve_web classifies each accepted connection — an api
// JSON line, a WebSocket upgrade, or a page request — and this seat
// dispatches it. Every connection is served CONCURRENTLY on its own
// cooperative task (the V6a serve model), parked on its own socket. Depends
// on madcide_seat.inc (serve_api_on) and madcide_ws.inc (serve_ws_client).

// ---- the ADVERTISEMENT (V6c-4; split arc 1D: madc's discovery) -----------
// How a client finds a running session without being told its address. ⚠️
// AN ADVERTISEMENT, NOT A LOCK: madcide refuses no file; two sessions in one
// root both advertise, and a client picks. The record lives in madc's state
// directory ($MADC_SESSION_DIR, else $XDG_STATE_HOME/madc/sessions — never
// the project root, ruling R1) under madcide's own key `<root>#<pid>`
// (madc::session_claim: held for the process's life, so a crashed session
// leaves no live record), and madc stamps it {key, pid, started, version,
// host}. Every failure is NON-FATAL: no state directory, the session opens
// unadvertised. The endpoint rides the session bag (`advertep`) for the
// re-publish; the claim is this process's.
madc::session_claim *session_advert = NULL;

// The URL a browser opens for a session's endpoint — ONE spelling (the
// record's `url`; the VS Code extension mirrors it by hand and says so).
void session_url(var &out, var &endpoint)
{
    out = format("http://{}/", endpoint);
}

// The documents this session holds, canonically. The buffer table is the
// truth once seeded; a headless session that has not opened a second buffer
// still has its launch document.
void session_documents(var &out, IdeSession &S, long doc)
{
    long w = S.world();
    long es = S.es;
    var list = {};
    var bufs;
    ui::get(bufs, w, es, "buffers");
    if ( bufs.is_null() || php::count(bufs) == 0 )
    {
	var p;
	buffer_path(p, w, doc);
	if ( !p.is_null() )
	{
	    var c;
	    madc::canonical_path(c, p.c_str());
	    list.push(c);
	}
	out = list;
	return;
    }
    for ( var r : bufs )
    {
	var rp;
	keyed_get(rp, r, "path");
	if ( rp.is_null() || !ui::doc_is(w, r["doc"].as_integer()) )
	    continue;			// untitled, or a view buffer: no file
	var c;
	madc::canonical_path(c, rp.c_str());
	list.push(c);
    }
    out = list;
}

// The record: everything a client needs to connect. `serves` is what the
// connection seat accepts on this port.
// ⚠️ The endpoint travels as a CARRIER, never as a `const char *`: a var's
// c_str() is ring-lifetime, and this calls out (canonical_path, the document
// scan, format) long enough for the ring to cycle.
void session_record(var &out, IdeSession &S, long doc, var &endpoint)
{
    var root;
    madc::canonical_path(root, ".");
    var docs;
    session_documents(docs, S, doc);
    var url;
    session_url(url, endpoint);
    var serves = { "api", "ws", "http", "mcp" };
    out = { "madcide": 1, "root": root, "endpoint": endpoint, "url": url,
	    "serves": serves, "documents": docs };
}

void session_advertise(IdeSession &S, long doc, var &endpoint)
{
    long w = S.world();
    long es = S.es;
    ui::set(w, es, "advertep", endpoint);
    if ( session_advert == NULL )
	session_advert = new madc::session_claim();
    var root;
    madc::canonical_path(root, ".");
    int64_t pid = madc::sys.pid;	// portable (the UCRT has no getpid)
    var key = format("{}#{}", root, pid);
    var rec;
    session_record(rec, S, doc, endpoint);
    var holder;
    session_advert->take(holder, key.c_str(), rec);	// its own key: never held elsewhere
}

// Re-publish (the document set moved on). Silent when never advertised.
void session_refresh(IdeSession &S, long doc)
{
    var ep;
    ui::get(ep, S.world(), S.es, "advertep");
    if ( session_advert == NULL || ep.is_null() || strlen(ep.c_str()) == 0 )
	return;
    var rec;
    session_record(rec, S, doc, ep);
    session_advert->update(rec);
}

// Withdraw. Idempotent, and never an error.
void session_withdraw(IdeSession &S)
{
    if ( session_advert != NULL )
	session_advert->release();
    ui::set(S.world(), S.es, "advertep", "");
}

// A live madcide session (a record carrying an endpoint) serving `file`, best
// first (madc::session_find: one holding the file beats one whose root holds
// it). False = none — and for no file (an untitled buffer is no one's).
bool session_find_joinable(var &out, const char *file)
{
    if ( file == 0 || strlen(file) == 0 )
	return false;
    var rec;
    madc::session_find(rec, file);
    var ep;
    keyed_get(ep, rec, "endpoint");
    if ( ep.is_null() )
	return false;
    out = rec;
    return true;
}

// ONE rendering of a session for a human — `--sessions`, the editor's status
// line and the attach hint all read it, so they never drift apart.
void session_report(var &out, var &rec)
{
    var ep = rec["endpoint"];
    var pid = rec["pid"];
    var root = rec["root"];
    var docs = rec["documents"];
    long dn = php::count(docs);
    out = format("{}  pid {}  {}  {} doc(s)", ep, pid, root, dn);
}

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
// serves every process that wears a second face — the editor itself
// (run_tui) beside the headless server — so there is one loop, not one per
// face. One session, two faces; a second process would be a second session
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

// Bind the serve face and PUBLISH it — the one owner of "this session
// listens" (the V6 seam consolidation: three faces had restated this bind, and
// the copies had already diverged — two reported a failed bind, the editor's
// swallowed it). Binds `listen://addr`, learns the endpoint the OS actually
// gave (`:0` asks for an ephemeral port), advertises it (the state directory +
// the `advertep` slot every in-protocol announcement reads), and returns the
// DETACHED listener handle for serve_accept_task. Returns 0 when the bind
// failed, with `why` the reason: the caller decides whether that is fatal (an
// explicit --serve) or advisory (the editor's default listen) — but never
// silent, because an unadvertised session is invisible to discovery, and
// invisible is indistinguishable from absent.
long serve_listen(IdeSession &S, long doc, const char *addr, var &endpoint, var &why)
{
    endpoint = "";
    why = "";
    var uri = format("listen://{}", addr);
    madc::channel listener(uri.c_str());
    if ( !listener.ok() )
    {
	why = format("cannot serve on {}: {}", addr, listener.last_error());
	return 0;
    }
    endpoint = listener.local_endpoint();	// ring lifetime: owned NOW
    session_advertise(S, doc, endpoint);
    return listener.detach();
}

// The headless server: open the session with no window, bind a listener, and
// serve every accepted connection concurrently (design §2.7: cooperative
// tasks, no thread). `addr` is the listen authority (host:port; :0 asks the OS
// for an ephemeral port, reported on the startup line). Returns 1 only when
// the file or the listener could not open; the serve loop runs until the
// listener dies (the process is stopped).
long run_serve(const char *path, bool ro, const char *addr,
	       const char *profile = "", long std = 0)
{
    IdeSession S;
    long doc = S.open(path, ro, profile, std);
    if ( doc <= 0 )
	return 1;			// -1 already reported on stderr
    S.terminal(false);			// headless: no interactive terminal

    // Bind + publish through the one owner (V6c-4: a client discovers this
    // session instead of being told its address). An explicit --serve that
    // cannot bind is fatal.
    var endpoint;
    var why;
    long listen_handle = serve_listen(S, doc, addr, endpoint, why);
    if ( listen_handle == 0 )
    {
	println(stderr, "{}: {}", product_in_use().name, why);
	S.close();
	return 1;
    }
    println(stderr, "{}: serving {} on {} (api + ws + http)", product_in_use().name, path, endpoint);

    // THE accept loop — the same function the editor spawns as a task
    // (V6c-4). A headless server has nothing else to do, so it runs
    // it in its own flow rather than beside itself; the loop's own `scope`
    // joins every connection task before it returns, so no serve task touches
    // the session after this.
    serve_accept_task(&S, doc, listen_handle, S.world(), S.es);
    session_withdraw(S);
    S.close();
    return 0;
}

// ---- the EDITOR that also serves (V6c-4) ---------------------------------
// `madcide <file>` — the TUI (and `--gui`), which by default LISTENS on a
// loopback ephemeral port and advertises it, so an agent's MCP client or a
// browser can join THIS session instead of opening a second one on
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
long run_tui(const char *path, bool ro, var &more, ui::level lvl, const char *addr,
	     const char *profile = "", long std = 0)
{
    IdeSession S;
    long doc = S.open(path, ro, profile, std);
    if ( doc <= 0 )
	return 1;			// -1 already reported on stderr
    long w = S.world();
    long es = S.es;
    open_more_files(w, es, doc, more);	// the command line's other files

    // Someone else may already hold this file. That is not a refusal — madcide
    // has never refused an open file — it is worth SAYING, with the one command
    // that joins them (ruling R3: a second TUI does not silently become a
    // remote client; that needs the thin client, which is its own slice).
    var other;
    if ( session_find_joinable(other, path) )
    {
	var line;
	session_report(line, other);
	var ep = other["endpoint"];
	ui::set(w, es, "msg",
		format("Also open in session {} — join it: madcide {} --mcp --attach {}",
		       line, path, ep));
    }

    // The serve face. Bound HERE so the real endpoint (`:0` asks the OS for a
    // port) is known before the task starts and before it is advertised.
    long listen_handle = 0;
    var endpoint = "";
    if ( addr != NULL )
    {
	var why;
	listen_handle = serve_listen(S, doc, addr, endpoint, why);
	// A port that will not bind is NOT fatal: the editor opens,
	// unadvertised — and SAYS so on the status line. The silent form of
	// this bind was the one that had diverged from its siblings (V6 seam).
	if ( listen_handle == 0 )
	    ui::set(w, es, "msg", format("Not advertised — {}", why));
    }

    scope
    {
	if ( listen_handle != 0 )
	    go serve_accept_task(&S, doc, listen_handle, w, es);
	// The session's pumps (a REPL tab's, a build's, a Terminal's) are
	// spawned inside this scope, so its join waits on them: run_ide
	// stops every client's before it returns, never close() after it.
	run_ide(S, doc, lvl);
	if ( listen_handle != 0 )
	    serve_accept_stop(w, es, endpoint);
    }
    session_withdraw(S);
    S.close();
    return 0;
}

// Every live madcide session, for a human (`madcide --sessions`): the
// advertisements carrying an endpoint (madc's other records — a `madc --mcp
// --project` server, a project's memory claim — are no madcide session).
// Returns 0 when any were found — a shell can branch on it.
long run_sessions()
{
    var all;
    madc::session_scan(all);
    long n = 0;
    for ( var rec : all )
    {
	var ep;
	keyed_get(ep, rec, "endpoint");
	if ( ep.is_null() )
	    continue;
	var line;
	session_report(line, rec);
	println("{}", line);
	n = n + 1;
    }
    if ( n == 0 )
    {
	println("no madcide sessions running");
	return 1;
    }
    println("");
    println("join one:  madcide <file> --mcp --attach <endpoint>");
    println("           madcide <file> --mcp --attach              (discovers)");
    println("⚠️  a madcide port has NO AUTH and NO TLS: loopback only, tunnel over ssh.");
    return 0;
}

// ---- the MCP server (`madcide <file> --mcp`; split arc 1D) ---------------
// The standard MCP deployment — a client (an editor, an agent host) spawns
// this process and speaks JSON-RPC 2.0 over the pipe, one message per line —
// served by the session's tool host (ide_serve_mcp: madc's tools and
// madcide's commands). stdout carries ONLY JSON-RPC (status goes to
// stderr); the session is headless; the lone stdio client is the operator.
long run_mcp(const char *path, bool ro, const char *profile = "", long std = 0)
{
    IdeSession S;
    long doc = S.open(path, ro, profile, std);
    if ( doc <= 0 )
	return 1;
    S.terminal(false);			// headless: no interactive terminal
    madc::channel io("stdio://");
    if ( !io.ok() )
    {
	println(stderr, "{} --mcp: {}", product_in_use().name, io.last_error());
	S.close();
	return 1;
    }
    long rc = ide_serve_mcp(S, doc, io);
    io.close();
    S.close();
    return rc;
}

// ---- the ATTACH RELAY (V6c-3c) -------------------------------------------
// `madcide <file> --mcp --attach <addr>`: the agent host spawns this process
// as it always does, but instead of opening a PRIVATE session it becomes a
// client of one that is already running — an agent's MCP client and a
// browser window driving one session's buffers. Pure transport: it opens NO
// session (the seat speaks MCP on every connection). Two cooperative pumps,
// so a server push and a client request never wait on each other. `<file>`
// is accepted on the command line (the CLI's shape) and unused here.

// The session -> client pump: whatever the seat sends, back to stdio.
void relay_down(madc::channel *sock, madc::channel *io)
{
    var line;
    while ( sock->readline(line) )
    {
	var out = format("{}\n", line);
	io->write(out.c_str());
    }
    io->close_write();			// the session is gone: say so downstream
}

long run_attach(const char *addr)
{
    madc::channel io("stdio://");
    if ( !io.ok() )
    {
	println(stderr, "{} --attach: {}", product_in_use().name, io.last_error());
	return 1;
    }
    var uri = format("tcp://{}", addr);
    madc::channel sock(uri.c_str());
    if ( !sock.ok() )
    {
	println(stderr, "{}: cannot attach to {}: {}", product_in_use().name, addr, sock.last_error());
	return 1;
    }
    println(stderr, "{}: attached to {} (mcp)", product_in_use().name, addr);
    scope
    {
	go relay_down(&sock, &io);
	var line;
	while ( io.readline(line) )
	{
	    var out = format("{}\n", line);
	    sock.write(out.c_str());
	}
	// The down-pump is PARKED on the socket, and `scope` joins it before
	// this returns. Half-closing wakes it properly: the seat reads EOF,
	// ends this connection's loop and closes its end — a real readable
	// event, so the pump returns.
	sock.close_write();
    }
    sock.close();
    io.close();
    return 0;
}
