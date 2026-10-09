// madcide_line.inc — the ui::LINE CLIENT (client-server design 2026-09-09
// §2.3b, slice V2.5): the ex / edlin line mode. `madcide <file> --line`.
//
// A LINE client renders the composed projection tree through the level-0
// sequential typesetter (the engine's ui_line_frontend) to stdout and
// reads ONE line of stdin per cycle. It is the vi ':' command mode without
// the TUI part (owner) — no cursor addressing, so it works over a pipe, in
// a dumb terminal, and as an MCP seat's transcript. No new command
// language: every colon verb (w q wq x e r, :N goto, the view* verbs,
// !shell) is reached exactly as the TUI ':' prompt and the -c one-shot
// reach it — through IdeSession::command(cmdCOLON, …).
//
// Same session, same composer as the grid and the window (run_ide): the
// ONLY difference is the per-input step. run_ide dispatches a key event;
// here the frontend delivers a raw line as a `text` event and
// IdeSession::line_input classifies it (':' = a colon command, anything
// else = text inserted verbatim + a newline). The engine ui:: layer never
// learns the tool's line grammar — it lives in the session, next to the
// registry and the colon interpreter (separation of concerns, Rule #5).
//
// Facts this client pushes: no terminal (a Run that needs one refuses on
// the status line, as the headless runs always have) and no region panel
// (LINE renders no chrome bands). The parse is synchronous, as the
// one-shot's is: diagnostics update on an explicit ':check', not on a
// background task — the frontend's stdin read is the ONE blocking decision
// (design §2.3b, the thread contract) and does not pump cooperative tasks.
void run_line(const char *path, bool ro, var &more, const char *profile = "", long std = 0)
{
    IdeSession S;
    long doc = S.open(path, ro, profile, std);
    if ( doc <= 0 )
	return;			// -1 already reported on stderr
    open_more_files(S.world(), S.es, doc, more);	// the command line's other files
    long t = ui::open(ui::LINE);
    if ( t == 0 )
	return;			// open refused (the reason on stderr)
    S.terminal(false);		// no interactive terminal to hand over
    S.panel(false);		// LINE renders no regions
    S.tabs(false);		// ... and no strips
    S.menus(false);		// ... and no menus
    bool run = true;
    while ( run )
    {
	long adoc = S.active(doc);
	var tree;
	S.compose_ide_tree(tree, adoc);
	ui::render(t, S.world(), tree);
	var ev;
	if ( !ui::event(ev, t, S.world()) )
	    break;		// stdin ended: the session closes
	bool cont = true;
	S.line_input(adoc, ev["text"].c_str(), cont);
	run = cont;
    }
    S.close();
    ui::close(t);
}
