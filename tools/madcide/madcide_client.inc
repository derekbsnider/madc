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
	ui::tui_bind_keys(t, table);
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
    var kind = format("{}", req["kind"]);
    if ( kind == "refresh" )
    {
	ui::tui_refresh(t);
	return;
    }
    if ( !ui::tui_suspend(t) )
	return;
    S.term_exec(req);
    if ( req["pause"].as_integer() != 0 )
	terminal_return_pause();
    ui::tui_resume(t);
}

// The IDE loop: the same compose-as-data -> render -> event -> apply
// cycle as vised, speaking to the session object only.
void run_ide(const char *path, bool ro)
{
    IdeSession S;
    long doc = S.open(path, ro);
    if ( doc == 0 )
	printf("cannot read %s\n", path);
    if ( doc <= 0 )
	return;			// -1 already reported on stderr
    long t = ui::tui_open();
    if ( t == 0 )
	return;
    S.terminal(true);
    long keysgen = -1;
    client_bind_keys(S, t, keysgen);
    bool run = true;
    while ( run )
    {
	// What the terminal knows arrives as facts, pushed before every
	// compose: the viewport (resizes land between events) and the
	// pending chord (the status line's %k echo).
	S.viewport(ui::tui_cols(t), ui::tui_rows(t));
	var pc;
	ui::tui_pending(pc, t);
	S.pending_chord(pc.is_null() ? "" : pc.c_str());
	long adoc = S.active(doc);
	var tree;
	S.compose_ide_tree(tree, adoc);
	ui::tui_render(t, S.world(), tree);
	if ( S.parse_pending(adoc) )
	    continue;		// keep serving keystrokes while it runs
	var ev;
	if ( !ui::tui_event(ev, t, S.world()) )
	    break;
	run = S.apply_ide_event(adoc, ev);
	client_bind_keys(S, t, keysgen);
	client_service(S, t);
    }
    S.close();
    ui::tui_close(t);
}
