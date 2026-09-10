// madcide_client.inc — the TUI CLIENT (the gateway seam, design doc
// 2026-08-31). THIS FILE IS THE ONLY PLACE A TUI HANDLE LIVES: render,
// input, refresh, and suspend belong to the client; the session
// (madcide_core.inc, class IdeSession) never touches one — gated by
// scripts/check-madcide-seam.sh. The client's whole job each cycle:
//   push what the terminal knows (viewport, presence, pending chord),
//   compose + render, hand the next event to the session, install the
//   key table when its generation moves, and service the one terminal
//   request the event may have parked (suspend -> term_exec -> pause ->
//   resume). A remote client would do the same over a transport.

// THE return-key pause (one owner, dupaudit family terminal_return_pause):
// every suspended-terminal run holds the guest's output on screen until
// enter, then the caller resumes into the full repaint.
void terminal_return_pause()
{
    println("");
    println("[madcide] press enter to return");
    var pause;
    madc::getline(pause);
}

// Install the session's key table when its generation moves (profile
// load, ^K P cycle, rescue fallback). The session validated the table
// handle-free (the one-converter contract: valid there IS bindable
// here); a refusal at this layer is an engine invariant break and
// speaks on stderr through the bind itself.
void client_bind_keys(IdeSession &S, long t, long &gen)
{
    if ( S.keys_generation() == gen )
	return;
    gen = S.keys_generation();
    var table;
    S.key_table(table);
    if ( !table.is_null() )
	ui::bind_keys(t, table);
}

// Drain the one terminal request the last event may have parked. A
// refresh repaints in place; every other kind hands the guest the REAL
// terminal (the JOE ^K Z shape) around the session's term_exec, with
// the return-key pause the request asked for.
void client_service(IdeSession &S, long t)
{
    var req;
    if ( !S.take_request(req) )
	return;
    long kind = req["kind"].as_integer();
    if ( kind == rqREFRESH )
    {
	ui::refresh(t);
	return;
    }
    if ( kind == rqFILEDIALOG )
    {
	// The native file dialog (S4): the target shows it and the answer
	// arrives as a dialog event; a target that cannot (the platform has
	// no native dialog yet) gets the request's prompt instead.
	var js = js::stringify(req);
	if ( !ui::dialog(t, js.c_str()) )
	    S.dialog_fallback(req);
	return;
    }
    if ( !ui::suspend(t) )
    {
	S.request_refused(req);		// never a silent drop (P3b-1)
	return;
    }
    S.term_exec(req);
    if ( req["pause"].as_integer() != 0 )
	terminal_return_pause();
    ui::resume(t);
}

// The IDE loop: the same compose-as-data -> render -> event -> apply
// cycle as vised, speaking to the session object only. The UI LEVEL is one
// argument (ui::level: ui::TUI = the grid frontend the tui_* names spell;
// ui::WEB shows the same composed tree in a window through <ns_ui_web>).
// One loop, one composer: the workbench layout rides layout hints the
// terminal ignores, so the ONLY per-level fact is whether a real terminal
// exists (level_of(t) <= ui::TUI) — S.terminal(has_term) gates the
// terminal-request path (a window captures build output into the panel
// region instead of suspending; interactive Run and the shell refuse
// until the embedded terminal lands).
// A CLIENT of the session (design §2.3, groundwork invariant #1). The
// record is transport/level/capability-GENERAL from the first window: a
// local window this week, but a remote platform node, an LSP editor and a
// VS Code extension are the SAME record with a different `transport` and
// capability set — a `client == local window` assumption is the corner
// that costs the mesh, so the whole shape lands now even though only
// trLOCAL is built. `t` is the frontend handle and `es` the client's own
// editor-state entity (the bag compose reads); the shared world holds the
// documents. `keysgen` is the client-local cache of the key-table
// generation it last installed (-1 = none yet); `last_seq` is its cursor
// into the change-event log (V4). `capabilities` is the symmetric
// consume/offer pair (invariant #2): a local window consumes the
// interactive surface and offers no services — a compute node fills
// `offer` with {build, run, test} later.
void make_client(var &out, long id, long t, long es, long xport, long lvl,
		 long tier)
{
    var consume = { "render": true, "input": true, "edit": true };
    var offer = {};		// a compute node fills this: build / run / test
    var caps = { "consume": consume, "offer": offer };
    var c = {
	"id": id,
	"t": t,
	"es": es,
	"transport": xport,
	"level": lvl,
	"tier": tier,
	"last_seq": 0,
	"capabilities": caps,
	"keysgen": -1
    };
    out = c;
}

// The client that spoke: ui::event_any tags the event with the frontend
// handle it came from (out["target"]); find its record. -1 = none (a
// handle that left the roster between the wait and the drain).
long client_index_by_t(var &clients, long t)
{
    long n = php::count(clients);
    for ( long i = 0; i < n; i = i + 1 )
	if ( clients[i]["t"].as_integer() == t )
	    return i;
    return -1;
}

void run_ide(const char *path, bool ro, ui::level lvl)
{
    IdeSession S;
    long doc = S.open(path, ro);
    if ( doc == 0 )
	printf("cannot read %s\n", path);
    if ( doc <= 0 )
	return;			// -1 already reported on stderr
    long w = S.world();		// the shared world (documents + every es)
    long t = ui::open(lvl);
    if ( t == 0 )
	return;			// open refused (e.g. no webview) with a reason
    // The client-pushed session facts (terminal / dialogs / panel) are
    // per-client — they write into THIS client's es, so set them on the
    // es open() built before it joins the roster.
    bool has_term = ui::level_of(t) <= ui::TUI;
    S.terminal(has_term);
    S.dialogs(ui::dialogs(t));	// a window's host may draw native file dialogs
    S.panel(!has_term);		// a window renders regions: the bottom panel (P3a)
    // Client 0 = the launching window/terminal over open()'s es. `clients`
    // is a dialect var array of the general records; ONE IdeSession is
    // re-pointed per client via S.attach(w, es) — attach is the seam.
    var clients = {};
    var c0;
    make_client(c0, 0, t, S.es, trLOCAL, lvl, tierOWNER);
    clients.push(c0);
    bool run = true;
    while ( run )
    {
	// Compose + render EVERY client, then wait on ANY of them. For a
	// single client this is byte-identical to the pre-multi-client loop:
	// compose one, wait, apply. `targets` collects the frontends the
	// blocking decision (ui::event_any) chooses among.
	bool parked = false;
	var targets = {};
	long n = php::count(clients);
	for ( long i = 0; i < n; i = i + 1 )
	{
	    var c = clients[i];
	    long ct = c["t"].as_integer();
	    S.attach(w, c["es"].as_integer());
	    // What the target knows arrives as facts, pushed before every
	    // compose: the viewport (resizes land between events) and the
	    // pending chord (the status line's %k echo).
	    S.viewport(ui::cols(ct), ui::rows(ct));
	    var pc;
	    ui::pending(pc, ct);
	    S.pending_chord(pc.is_null() ? "" : pc.c_str());
	    long adoc = S.active(doc);
	    var tree;
	    S.compose_ide_tree(tree, adoc);
	    ui::render(ct, w, tree);
	    if ( S.parse_pending(adoc) )
		parked = true;		// keep serving keystrokes while it runs
	    // Install this client's key table when its generation moves; the
	    // cache rides the client record (copy-mutate-store).
	    long gen = c["keysgen"].as_integer();
	    client_bind_keys(S, ct, gen);
	    c["keysgen"] = gen;
	    clients[i] = c;
	    targets.push(ct);
	}
	if ( parked )
	    continue;
	var ev;
	if ( !ui::event_any(ev, targets, w) )
	    break;
	// Apply the event to the client it came from (event_any tagged it).
	long idx = client_index_by_t(clients, ev["target"].as_integer());
	if ( idx < 0 )
	    continue;			// a handle that left the roster
	var cc = clients[idx];
	long ct = cc["t"].as_integer();
	S.attach(w, cc["es"].as_integer());
	long adoc = S.active(doc);
	run = S.apply_ide_event(adoc, ev);
	client_service(S, ct);		// the next compose re-binds keys
    }
    // Close every client's frontend, then the session (the shared world).
    long nc = php::count(clients);
    S.close();
    for ( long i = 0; i < nc; i = i + 1 )
	ui::close(clients[i]["t"].as_integer());
}
