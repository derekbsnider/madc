// madcide_repl.inc — the panel's REPL tab (plan §41.9a slice 3; §37 item 9):
// an interactive session in its own process (madc::session_*, the running
// madc forked — nothing execs), its transcript in the tab and an input field
// under it. Included by madcide_core.inc after the Terminal tab, whose
// pseudo-buffer, pump and @scope machinery it reuses, and before
// IdeSession's dispatcher, which routes the keyboard here while the input
// has it (`replfocus`).
//
// The transcript is the [repl] buffer, fed like the Terminal's screen
// (ui::term_feed: append-only, bounded): each taken entry after the prompt
// (continuation lines indented to its width, the CLI's rule), what it
// printed, then its shown value or its diagnostics. The input is the
// [repl-input] buffer: every editing key is the personality's, run by the
// session's own dispatcher on that buffer under the field's caret, the
// editor's live state parked around it (the viewfocus hand-off's shape).
// Only the REPL's own actions are new, bound in the @repl scope (profile
// data; baked defaults in modal_default_keys_text): replenter (offer the
// input: complete runs, incomplete inserts a newline, a blank last line
// submits — D11), replcomplete, replolder / replnewer (the pane's history,
// in memory, at the input's first / last line), replunfocus.
//
// While an entry runs, the input is the program's standard input: enter
// sends the line (Thonny's shell). A crash restarts the session (the
// verbs' rule) and the transcript says so.
//
// F5 (`replrun`, plan §41.10a) runs the editor's buffer into the tab: a
// fresh session, `%run <name>` in the transcript, the buffer's live text
// loaded under its path, then its main run (its output in the transcript,
// the input its stdin), and the prompt left with the buffer's names.
//
// State on the bag: replh (the session handle), replstop (the pump's stop
// channel), repllive, replfocus, replbusy (the seq of the entry offered, or
// of F5's load / run, 0 when none), replrunning (it was taken and runs, or
// F5's main runs), replsent (its text), replcseq / replccaret / replctext
// (a pending completion), replloadseq / replrunseq / replrunpath (F5's
// load and run requests, and the path main runs under), replin (the input
// buffer) with its parked rcaret / rmark / rbend, replhist / replhpos /
// repldraft (the history), replstd (the `--std=` spelling a new session
// takes; "" = the engine's default).
//
// Thread contract: session-thread bag state; the pump is a cooperative task
// on the same thread (term_pump's contract).

// Where an event goes while the input has the keyboard.
enum repl_route : unsigned char { rrPASS = 0, rrHANDLED, rrFIELD };

long repl_doc_if_any(long w, long es)
{
    return buffer_doc_named(w, es, "[repl]");
}

long repl_doc(long w, long es)
{
    long existing = repl_doc_if_any(w, es);
    if ( existing != 0 )
	return existing;
    long nd = new_document(w, "[repl]", "");
    push_buffer_row(w, es, nd, "[repl]");
    return nd;
}

// The input field's buffer: not a buffer-table row (it is the tab's, not a
// document the ring shows).
long repl_input_doc(long w, long es)
{
    long d = es_int(w, es, "replin", 0);
    if ( d != 0 )
	return d;
    d = new_document(w, "[repl-input]", "");
    ui::set(w, es, "replin", d);
    ui::set(w, es, "rcaret", 0);
    ui::set(w, es, "rmark", -1);
    ui::set(w, es, "rbend", -1);
    return d;
}

// The prompt names the session's standard (D22, the CLI's rule).
void repl_prompt(var &out, long w, long es)
{
    var sn = madc::session_standard(es_int(w, es, "replh", 0));
    out = format("{}> ", sn);
}

// Text as the pane shows an entry: the prompt, then the text, each line
// after the first indented to the prompt's width.
void repl_entry_display(var &out, const char *prompt, const char *text)
{
    var p = prompt;
    var t = text;
    var pad = php::str_repeat(" ", strlen(p.c_str()));
    array lines;
    php::explode(lines, "\n", t.c_str());
    long n = php::count(lines);
    var s = p;
    for ( long i = 0; i < n; i = i + 1 )
    {
	var one = php::array_get_cstr(lines, i);
	if ( i == 0 )
	    s = format("{}{}", s, one);
	else
	    s = format("{}\n{}{}", s, pad, one);
    }
    out = s;
}

// Where the input's caret falls in that display.
long repl_display_caret(const char *prompt, const char *text, long caret)
{
    long pl = strlen(prompt);
    long nl = 0;
    for ( long i = 0; i < caret && text[i] != 0; i = i + 1 )
	if ( text[i] == '\n' )
	    nl = nl + 1;
    return pl + caret + nl * pl;
}

void repl_feed(long w, long es, const char *text)
{
    var t = text;
    if ( strlen(t.c_str()) > 0 )
	term_feed_text(w, repl_doc(w, es), t.c_str());
}

// ---- the input field: the editor's live interaction state parked while the
//      field's is live, so the session's editing commands run on the input
//      buffer without touching the editor's caret, selection or colour ----
void repl_field_enter(var &parked, long w, long es)
{
    var ed;
    ui::get(ed, w, es, "editable");
    var ro;
    ui::get(ro, w, es, "ro_msg");
    var hs;
    ui::get(hs, w, es, "hspans");
    parked = { "caret": es_int(w, es, "caret", 0), "mark": es_int(w, es, "mark", -1),
	       "bend": es_int(w, es, "bend", -1), "fview": es_int(w, es, "fview", 0),
	       "anchor": es_int(w, es, "anchor", -1), "editable": ed,
	       "ro_msg": ro, "hspans": hs };
    var none = {};
    ui::set(w, es, "caret", es_int(w, es, "rcaret", 0));
    ui::set(w, es, "mark", es_int(w, es, "rmark", -1));
    ui::set(w, es, "bend", es_int(w, es, "rbend", -1));
    ui::set(w, es, "fview", 0);		// no View: nav_doc is the input itself
    ui::set(w, es, "anchor", -1);
    ui::set(w, es, "editable", true);
    ui::set(w, es, "ro_msg", "");
    ui::set(w, es, "hspans", none);
}

void repl_field_leave(var &parked, long w, long es)
{
    ui::set(w, es, "rcaret", es_int(w, es, "caret", 0));
    ui::set(w, es, "rmark", es_int(w, es, "mark", -1));
    ui::set(w, es, "rbend", es_int(w, es, "bend", -1));
    ui::set(w, es, "caret", parked["caret"]);
    ui::set(w, es, "mark", parked["mark"]);
    ui::set(w, es, "bend", parked["bend"]);
    ui::set(w, es, "fview", parked["fview"]);
    ui::set(w, es, "anchor", parked["anchor"]);
    ui::set(w, es, "editable", parked["editable"]);
    ui::set(w, es, "ro_msg", parked["ro_msg"]);
    ui::set(w, es, "hspans", parked["hspans"]);
}

// Replace `del` bytes at `off` with `ins` in the input, the caret after it
// (through the text-mutation owners, under the field's state).
void repl_field_edit(long w, long es, long off, long del, const char *ins)
{
    var t = ins;
    long rin = repl_input_doc(w, es);
    var parked;
    repl_field_enter(parked, w, es);
    if ( del > 0 )
	ed_text_erase(w, es, rin, off, del);
    if ( strlen(t.c_str()) > 0 )
	ed_text_insert(w, es, rin, off, t.c_str());
    ui::set(w, es, "caret", off + strlen(t.c_str()));
    repl_field_leave(parked, w, es);
}

// The whole input replaced (history recall, a taken entry's clearing).
void repl_set_input(long w, long es, const char *text)
{
    var t = text;
    long rin = repl_input_doc(w, es);
    ui::text_load(w, rin, t.c_str());
    ui::set(w, rin, "modified", false);
    ui::set(w, es, "rcaret", strlen(t.c_str()));
    ui::set(w, es, "rmark", -1);
    ui::set(w, es, "rbend", -1);
}

// The input takes edits unless an entry was offered and has not been taken
// yet (the classifier's moment; a running entry's input is its stdin).
bool repl_editable(long w, long es)
{
    return es_int(w, es, "replbusy", 0) == 0 || es_int(w, es, "replrunning", 0) != 0;
}

// ---- the history (in memory; the CLI's file per standard is a later slice) ----
void repl_history_push(long w, long es, const char *entry)
{
    var e = php::rtrim(entry);
    if ( strlen(e.c_str()) == 0 )
	return;
    var hist;
    ui::get(hist, w, es, "replhist");
    if ( hist.is_null() )
	hist = {};
    long n = php::count(hist);
    if ( n == 0 || hist[n - 1] != e )
	hist.push(e);
    ui::set(w, es, "replhist", hist);
    ui::set(w, es, "replhpos", -1);
}

// Older (dir -1) / newer (+1). Only at the input's first line (older) or
// last line (newer), Julia's and IPython's rule; elsewhere the arrow moves
// the caret (false: the caller forwards the motion).
bool repl_history_step(long w, long es, long dir)
{
    var text;
    ui::text(text, w, repl_input_doc(w, es));
    const char *s = text.c_str();
    long caret = es_int(w, es, "rcaret", 0);
    long len = strlen(s);
    if ( dir < 0 )
    {
	for ( long i = 0; i < caret && i < len; i = i + 1 )
	    if ( s[i] == '\n' )
		return false;
    }
    else
    {
	for ( long i = caret; i < len; i = i + 1 )
	    if ( s[i] == '\n' )
		return false;
    }
    var hist;
    ui::get(hist, w, es, "replhist");
    long n = hist.is_null() ? 0 : php::count(hist);
    long pos = es_int(w, es, "replhpos", -1);
    if ( dir < 0 )
    {
	if ( n == 0 )
	    return true;
	if ( pos == -1 )
	{
	    ui::set(w, es, "repldraft", text);
	    pos = n - 1;
	}
	else if ( pos > 0 )
	    pos = pos - 1;
	else
	    return true;			// the oldest already
    }
    else
    {
	if ( pos == -1 )
	    return false;
	pos = pos + 1;
    }
    if ( pos >= n )
    {
	var draft;
	ui::get(draft, w, es, "repldraft");
	repl_set_input(w, es, draft.is_null() ? "" : draft.c_str());
	ui::set(w, es, "replhpos", -1);
	return true;
    }
    var one = hist[pos];
    repl_set_input(w, es, one.c_str());
    ui::set(w, es, "replhpos", pos);
    return true;
}

// ---- the session: open, pump, replies ----
void repl_pump(long w, long es, long h, long stopc);

void repl_start(long w, long es)
{
    if ( es_int(w, es, "repllive", 0) != 0 )
	return;
    long h = es_int(w, es, "replh", 0);
    if ( h == 0 )
    {
	var sp;
	ui::get(sp, w, es, "replstd");
	h = madc::session_open(sp.is_null() ? "" : sp.c_str());
	ui::set(w, es, "replh", h);
    }
    if ( !madc::session_running(h) )
    {
	var why = madc::session_error(h);
	repl_feed(w, es, format("[the session did not start: {}]\n", why));
	return;
    }
    repl_input_doc(w, es);
    long stopc = madc::chan_make(1);
    ui::set(w, es, "replstop", stopc);
    ui::set(w, es, "repllive", 1);
    go repl_pump(w, es, h, stopc);
}

// A completion's answer: one name replaces the word, several their common
// prefix when it is longer than the word, else they are listed (the CLI's
// rule). Stale when the input changed since the request.
void repl_completed(long w, long es, var &r)
{
    long rin = repl_input_doc(w, es);
    var now;
    ui::text(now, w, rin);
    var asked;
    ui::get(asked, w, es, "replctext");
    if ( asked.is_null() || now != asked )
	return;
    var names = r["names"];
    long n = php::count(names);
    if ( n == 0 )
    {
	ui::set(w, es, "msg", "No completions.");
	return;
    }
    long start = r["start"].as_integer();
    long caret = es_int(w, es, "replccaret", 0);
    var first = names[0];
    long common = strlen(first.c_str());
    for ( long i = 1; i < n; i = i + 1 )
    {
	var other = names[i];
	const char *a = first.c_str();
	const char *b = other.c_str();
	long k = 0;
	while ( k < common && a[k] != 0 && a[k] == b[k] )
	    k = k + 1;
	common = k;
    }
    if ( n > 1 && common <= caret - start )
    {
	var listed = php::implode("  ", names);
	repl_feed(w, es, format("{}\n", listed));
	return;
    }
    var word = first.substr(0, common);
    repl_field_edit(w, es, start, caret - start, word.c_str());
}

void repl_reply(long w, long es, var &r)
{
    long seq = r["seq"].as_integer();
    if ( r["kind"] == madc::session_reply::running )
    {
	if ( seq != es_int(w, es, "replbusy", 0) )
	    return;
	var sent;
	ui::get(sent, w, es, "replsent");
	var p;
	repl_prompt(p, w, es);
	var shown;
	repl_entry_display(shown, p.c_str(), sent.c_str());
	repl_feed(w, es, format("{}\n", shown));
	repl_history_push(w, es, sent.c_str());
	repl_set_input(w, es, "");
	ui::set(w, es, "replrunning", 1);
	return;
    }
    if ( r["kind"] == madc::session_reply::offer )
    {
	if ( seq != es_int(w, es, "replbusy", 0) )
	    return;
	bool taken = r["state"] == madc::offer_state::taken;
	ui::set(w, es, "replbusy", 0);
	ui::set(w, es, "replrunning", 0);
	if ( !taken )
	{
	    // Incomplete or extendable: keep editing on a new line.
	    long caret = es_int(w, es, "rcaret", 0);
	    repl_field_edit(w, es, caret, 0, "\n");
	    return;
	}
	var sv = r["shown"];
	if ( strlen(sv.c_str()) > 0 )
	    repl_feed(w, es, format("{}\n", sv));
	var rd = r["rendered"];
	repl_feed(w, es, rd.c_str());
	return;
    }
    if ( r["kind"] == madc::session_reply::complete )
    {
	if ( seq != es_int(w, es, "replcseq", 0) )
	    return;
	ui::set(w, es, "replcseq", 0);
	repl_completed(w, es, r);
	return;
    }
    if ( r["kind"] == madc::session_reply::load )
    {
	// F5's buffer loaded: its main runs, and the lines typed meanwhile
	// are main's stdin, as for a running entry. Refused: its diagnostics,
	// and the prompt is the fresh, empty session's.
	if ( seq != es_int(w, es, "replloadseq", 0) )
	    return;
	ui::set(w, es, "replloadseq", 0);
	var rd = r["rendered"];
	repl_feed(w, es, rd.c_str());
	if ( !(r["ok"] == true) )
	{
	    ui::set(w, es, "replbusy", 0);
	    return;
	}
	var rp;
	ui::get(rp, w, es, "replrunpath");
	var argv = {};
	argv.push(rp);
	long rs = madc::session_run(es_int(w, es, "replh", 0), argv);
	ui::set(w, es, "replrunseq", rs);
	ui::set(w, es, "replbusy", rs);
	ui::set(w, es, "replrunning", 1);
	return;
    }
    if ( r["kind"] == madc::session_reply::run )
    {
	// F5's main returned: what it printed came before this reply.
	if ( seq != es_int(w, es, "replrunseq", 0) )
	    return;
	ui::set(w, es, "replrunseq", 0);
	ui::set(w, es, "replbusy", 0);
	ui::set(w, es, "replrunning", 0);
	var rd = r["rendered"];
	repl_feed(w, es, rd.c_str());
	return;
    }
    if ( r["kind"] == madc::session_reply::stopped )
    {
	ui::set(w, es, "replbusy", 0);
	ui::set(w, es, "replrunning", 0);
	ui::set(w, es, "replcseq", 0);
	ui::set(w, es, "replloadseq", 0);
	ui::set(w, es, "replrunseq", 0);
	var st = r["exit_status"];
	if ( r["restarted"] == true )
	    repl_feed(w, es, format("[the session stopped (exit {}); a new one started]\n", st));
	else
	    repl_feed(w, es, format("[the session stopped (exit {})]\n", st));
    }
}

// Everything that waits: a running notice before the output after it, any
// other reply after the output before it (the session's order).
void repl_drain(long w, long es, long h)
{
    for ( ;; )
    {
	var r;
	long got = madc::session_poll(r, h);
	bool running = got == 1 && r["kind"] == madc::session_reply::running;
	if ( running )
	    repl_reply(w, es, r);
	var o;
	madc::session_output(o, h);
	repl_feed(w, es, o.c_str());
	if ( got == 0 )
	    return;
	if ( !running )
	    repl_reply(w, es, r);
    }
}

// The pump: the select over {stop, the session}; what arrives lands in the
// transcript, and the loop's task-drain wakes the UI (the wake event).
void repl_pump(long w, long es, long h, long stopc)
{
    var cases;
    cases[0] = stopc;
    cases[1] = madc::session_readable(h);
    for ( ;; )
    {
	var fired;
	if ( madc::chan_select(fired, cases) != 1 )
	    break;
	repl_drain(w, es, h);
    }
    ui::set(w, es, "repllive", 0);
}

// ---- the REPL's commands ----
void repl_enter(long w, long es)
{
    long h = es_int(w, es, "replh", 0);
    if ( !madc::session_running(h) )
    {
	ui::set(w, es, "msg", "No session (repl starts one).");
	return;
    }
    var text;
    ui::text(text, w, repl_input_doc(w, es));
    if ( es_int(w, es, "replrunning", 0) != 0 )
    {
	// The entry runs: the line is its standard input.
	var line = format("{}\n", text);
	madc::session_input(h, line.c_str());
	repl_feed(w, es, line.c_str());
	repl_set_input(w, es, "");
	return;
    }
    if ( es_int(w, es, "replbusy", 0) != 0 )
    {
	ui::set(w, es, "msg", "The session is busy.");
	return;
    }
    const char *s = text.c_str();
    long len = strlen(s);
    var trimmed = php::rtrim(s);
    if ( strlen(trimmed.c_str()) == 0 )
	return;
    // D11: a blank last line submits what is there (a finished if with no
    // else runs; an unfinished entry is refused).
    long last = len;
    while ( last > 0 && s[last - 1] != '\n' )
	last = last - 1;
    bool blank_last = last > 0;
    for ( long i = last; i < len && blank_last; i = i + 1 )
	if ( s[i] != ' ' && s[i] != '\t' )
	    blank_last = false;
    long seq = madc::session_offer(h, s, blank_last);
    ui::set(w, es, "replbusy", seq);
    ui::set(w, es, "replsent", text);
}

void repl_complete(long w, long es)
{
    long h = es_int(w, es, "replh", 0);
    if ( !madc::session_running(h) || es_int(w, es, "replbusy", 0) != 0 )
	return;
    var text;
    ui::text(text, w, repl_input_doc(w, es));
    long caret = es_int(w, es, "rcaret", 0);
    long seq = madc::session_complete(h, text.c_str(), caret);
    ui::set(w, es, "replcseq", seq);
    ui::set(w, es, "replccaret", caret);
    ui::set(w, es, "replctext", text);
}

// The session's end with the IDE's (IdeSession::stop_tasks): the pump stops
// before any join of it, and the session after (repl_close). Sent once: the
// handle is spent (a second send into the one-slot channel would park).
void repl_stop(long w, long es)
{
    long stopc = es_int(w, es, "replstop", 0);
    if ( es_int(w, es, "repllive", 0) != 0 && stopc != 0 )
    {
	var sv = 1;
	madc::chan_send(stopc, sv);
	ui::set(w, es, "replstop", 0);
    }
}

void repl_close(long w, long es)
{
    long h = es_int(w, es, "replh", 0);
    if ( h != 0 )
	madc::session_close(h);
    ui::set(w, es, "replh", 0);
}

// The `repl` command: show the tab, start the session once, and give the
// input the keyboard.
void repl_show(long w, long es)
{
    show_view(w, es, viewREPL);
    repl_start(w, es);
    ui::set(w, es, "termfocus", 0);
    ui::set(w, es, "replfocus", 1);
}

// F5, `replrun` (plan §41.10a; Thonny's F5, IPython's %run): the buffer
// `doc` run into the tab's session. A session that already existed is
// restarted, so the run is clean and an entry still running stops with the
// old one. The buffer's live text runs, unsaved edits included, under its
// path and the tab's standard; its main runs once the load is answered
// (repl_reply), with argv [path].
void repl_run(long w, long es, long doc)
{
    var p;
    ui::get(p, w, doc, "path");
    if ( !path_is_asset(p) )
    {
	ui::set(w, es, "msg", format("Not a file: {}", p));
	return;
    }
    var text;
    ui::text(text, w, doc);
    if ( strlen(text.c_str()) == 0 )
    {
	// session_load's empty text reads the file: the buffer is what runs.
	ui::set(w, es, "msg", "The buffer is empty.");
	return;
    }
    bool had = es_int(w, es, "replh", 0) != 0;
    repl_show(w, es);
    long h = es_int(w, es, "replh", 0);
    if ( !madc::session_running(h) )
	return;				// repl_start said why
    if ( had && !madc::session_restart(h) )
    {
	var why = madc::session_error(h);
	repl_feed(w, es, format("[the session did not start: {}]\n", why));
	return;
    }
    ui::set(w, es, "replbusy", 0);
    ui::set(w, es, "replrunning", 0);
    ui::set(w, es, "replcseq", 0);
    ui::set(w, es, "replrunseq", 0);
    var name = php::basename(p.c_str());
    var line = format("%run {}", name);
    var pr;
    repl_prompt(pr, w, es);
    var shown;
    repl_entry_display(shown, pr.c_str(), line.c_str());
    repl_feed(w, es, format("{}\n", shown));
    long seq = madc::session_load(h, p.c_str(), text.c_str());
    ui::set(w, es, "replloadseq", seq);
    ui::set(w, es, "replrunpath", p);
    ui::set(w, es, "replbusy", seq);
}

// An @repl binding: the REPL's own actions, or a motion the history hands
// on to the field (the caret off the first / last line), or any other
// command the profile bound there, which runs as itself.
long repl_command(long w, long es, long code, var &ev)
{
    switch ( code )
    {
	case cmdREPLENTER:
	    repl_enter(w, es);
	    return rrHANDLED;
	case cmdREPLCOMPLETE:
	    repl_complete(w, es);
	    return rrHANDLED;
	case cmdREPLUNFOCUS:
	    ui::set(w, es, "replfocus", 0);
	    ui::set(w, es, "msg", "Keyboard back to the editor.");
	    return rrHANDLED;
	case cmdREPLOLDER:
	case cmdREPLNEWER:
	{
	    bool older = code == cmdREPLOLDER;
	    if ( repl_editable(w, es) && repl_history_step(w, es, older ? -1 : 1) )
		return rrHANDLED;
	    code = older ? cmdUP : cmdDOWN;
	    break;
	}
    }
    // The binding's command, as an action event (the key's own spelling
    // stays in `seq`, the one a scope looks up).
    var an;
    cmd_name(an, code);
    ev["event"] = "action";
    ev["event_code"] = ui::event_kind::action;
    ev["action"] = an;
    ev["action_code"] = code;
    if ( !cmd_edits_field(code) )
	return rrPASS;
    return repl_editable(w, es) ? rrFIELD : rrHANDLED;
}

// Route one event while the input has the keyboard: text and the field's
// commands edit the input, @repl bindings act, the rest passes on.
long repl_route(long w, long es, var &ev)
{
    var kind = ev["event_code"];
    if ( kind != ui::event_kind::text && kind != ui::event_kind::key
	 && kind != ui::event_kind::action )
	return rrPASS;
    var a;
    if ( scoped_action(a, w, es, "repl", ev) )
	return repl_command(w, es, a.as_integer(), ev);
    if ( kind == ui::event_kind::action && !cmd_edits_field(ev_code(ev)) )
	return rrPASS;
    return repl_editable(w, es) ? rrFIELD : rrHANDLED;
}

// The tab's node: one edit node over the transcript, then the prompt and
// the input (while an entry runs, its input line without a prompt), the
// caret in the input. Its subject is the input buffer: a press on it gives
// the input the keyboard (pointer_event).
void repl_compose(var &out, long w, long es)
{
    var tt = "";
    long rdoc = repl_doc_if_any(w, es);
    if ( rdoc != 0 )
	ui::text(tt, w, rdoc);
    long tlen = strlen(tt.c_str());
    if ( tlen > 0 && tt.c_str()[tlen - 1] != '\n' )
    {
	tt = format("{}\n", tt);		// a fresh row after output with no newline
	tlen = tlen + 1;
    }
    long rin = es_int(w, es, "replin", 0);
    var content = tt;
    long caret = tlen;
    if ( rin != 0 && repl_editable(w, es) )
    {
	var in;
	ui::text(in, w, rin);
	var p = "";
	if ( es_int(w, es, "replrunning", 0) == 0 )
	    repl_prompt(p, w, es);
	var disp;
	repl_entry_display(disp, p.c_str(), in.c_str());
	content = format("{}{}", tt, disp);
	caret = tlen + repl_display_caret(p.c_str(), in.c_str(), es_int(w, es, "rcaret", 0));
    }
    var hints = { "caret": caret, "repl": 1 };
    if ( es_int(w, es, "replfocus", 0) != 0 )
    {
	hints["focus"] = 1;
	hints["tabkey"] = 1;		// tab completes; ^] hands the keyboard back
    }
    var node = { "role": "edit", "content": content, "hints": hints };
    if ( rin != 0 )
	node["subject"] = rin;
    out = node;
}
