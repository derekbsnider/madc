// madcide_repl.inc — the panel's REPL tab (plan §41.9a slice 3; §37 item 9):
// an interactive session in its own process (madc::session_*, the running
// madc forked — nothing execs), its transcript in the tab and an input field
// under it. madcide's first BUILTIN plugin (plan §41.11a step 8): included
// at the end of madcide_core.inc, it registers through the plugin points at
// its activation (repl_activate) — its ten commands, its console view
// `repl` (vsCONSOLE: the core composes it from `repl.pane` and routes the
// keyboard to its input while the input has it, `consolefocus`) and its
// handlers of the session's open, stop and close — and the core names none
// of its functions.
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
// State on the bag: repl.pane (the console view's state: { transcript,
// input, prompt, editable }, kept by repl_pane_sync), replh (the session
// handle), replstop (the pump's stop channel), repllive, replbusy (the seq
// of the entry offered, or
// of F5's load / run, 0 when none), replrunning (it was taken and runs, or
// F5's main runs), replsent (its text), replcseq / replccaret / replctext
// (a pending completion), replloadseq / replrunseq / replrunpath /
// replrunargv (a file's load and run requests — F5's or a typed %run's
// or %load's — the path it loads under and main's argv, empty when main
// does not run), replbseq (a
// pending bindings request, replbindings), replin (the input
// buffer, a field: its caret is the field's own), replhist / replhpos /
// repldraft (the history). Its standard is the session's (madcide_lang.inc):
// a new backend takes the one the buffer it starts for compiles under.
//
// Thread contract: session-thread bag state; the pump is a cooperative task
// on the same thread (term_pump's contract).

// The pane's own registrations in a world (its view's kind), kept at its
// activation on the world's `repl-pane`
// entity: a code converted once, never a name looked up at use.
long repl_world(long w)
{
    long e = ui::entity_by_name(w, "repl-pane");
    if ( e == 0 )
	e = ui::create(w, "repl-pane");
    return e;
}

long repl_view(long w)
{
    return es_int(w, repl_world(w), "view", viewNONE);
}

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
    d = field_document(w, "[repl-input]");
    ui::set(w, es, "replin", d);
    return d;
}

// The prompt names the session's standard (D22, the CLI's rule).
void repl_prompt(var &out, long w, long es)
{
    var sn = madc::session_standard(es_int(w, es, "replh", 0));
    out = format("{}> ", sn);
}

void repl_feed(long w, long es, const char *text)
{
    var t = text;
    if ( strlen(t.c_str()) > 0 )
	term_feed_text(w, repl_doc(w, es), t.c_str());
}

// The whole input replaced (history recall, a taken entry's clearing).
void repl_set_input(long w, long es, const char *text)
{
    var t = text;
    long rin = repl_input_doc(w, es);
    ui::text_load(w, rin, t.c_str());
    ui::set(w, rin, "modified", false);
    field_reset(w, rin, strlen(t.c_str()));
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
    long rin = repl_input_doc(w, es);
    var text;
    ui::text(text, w, rin);
    const char *s = text.c_str();
    long caret = field_caret(w, rin);
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
void repl_pane_sync(long w, long es);
bool repl_run_payload(long w, long es, var &r);
bool repl_restart(long w, long es, long doc);
void repl_ide_help(var &out, long w);

// F5's rows into Problems (plan §41.11a step 3): the replies to its load
// and to its run carry the rows the unit recorded, citing the buffer's
// path, so each is navigable as a check's row is (cmdGOTO), and a clean
// load or run clears the stale ones. An entry's own rows (REPL[N]) stay
// out: they cite no file the editor holds.
void repl_problems(long w, long es, var &r)
{
    var rows = {};
    var got;
    keyed_get(got, r, "diagnostics");
    if ( !got.is_null() )
	rows = got;
    ui::set(w, es, "diags", rows);
}

// The in-flight marks: the seqs an entry, a completion, F5's load and its
// run keep while their replies are owed, and the running flag. A restart or
// a stopped session owes none of them.
void repl_clear_marks(long w, long es)
{
    ui::set(w, es, "replbusy", 0);
    ui::set(w, es, "replrunning", 0);
    ui::set(w, es, "replcseq", 0);
    ui::set(w, es, "replloadseq", 0);
    ui::set(w, es, "replrunseq", 0);
    ui::set(w, es, "replbseq", 0);
    ui::set(w, es, "replruncall", 0);
    ui::set(w, es, "replbuildseq", 0);
    ui::set(w, es, "replhelpseq", 0);
}

// The `--std=` spelling a backend for `doc` takes: the standard the buffer
// compiles under (doc_standard — the session's, else its file's), "" for
// the engine's default.
void repl_std_of(var &out, long w, long doc)
{
    out = "";
    long k = doc_standard(w, doc);
    if ( k != 0 )
	out = madc::file_kind_name(k);
}

void repl_start(long w, long es)
{
    if ( es_int(w, es, "repllive", 0) != 0 )
	return;
    long h = es_int(w, es, "replh", 0);
    if ( h == 0 )
    {
	var sp;
	repl_std_of(sp, w, active_doc(w, es, 0));
	h = madc::session_open(sp.c_str());
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
    field_edit(w, es, rin, start, caret - start, word.c_str());
}

// The replies a plugin hears (repl_event): published after the pane has
// taken the reply in, to the buffer the pane is serving.
void repl_publish(long w, long es, long kind, var &r)
{
    publish_event(w, es, active_doc(w, es, 0), kind, r);
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
	console_entry_display(shown, p.c_str(), sent.c_str());
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
	bool quits = taken && r["payload"] == madc::session_payload::quit;
	// A file command's payload keeps the pane busy into its file's load
	// (repl_load_file sets the load's seq), so it never reads idle between.
	bool files = taken && !quits
		     && !(r["payload"] == madc::session_payload::none);
	if ( !files )
	    ui::set(w, es, "replbusy", 0);
	ui::set(w, es, "replrunning", 0);
	if ( !taken )
	{
	    // Incomplete or extendable: keep editing on a new line.
	    long rin = repl_input_doc(w, es);
	    field_edit(w, es, rin, field_caret(w, rin), 0, "\n");
	    return;
	}
	var sv = r["shown"];
	if ( strlen(sv.c_str()) > 0 )
	    repl_feed(w, es, format("{}\n", sv));
	if ( seq == es_int(w, es, "replhelpseq", 0) )
	{
	    // %help: the IDE's own commands follow the session's.
	    ui::set(w, es, "replhelpseq", 0);
	    var more;
	    repl_ide_help(more, w);
	    repl_feed(w, es, more.c_str());
	}
	var rd = r["rendered"];
	repl_feed(w, es, rd.c_str());
	repl_publish(w, es, reTAKEN, r);
	if ( quits )
	{
	    // %quit (.q, .exit): the session ends, and the pane starts a fresh
	    // one, as when a backend ends (Thonny's exit()).
	    if ( repl_restart(w, es, active_doc(w, es, 0)) )
		repl_feed(w, es, "[the session ended; a new one started]\n");
	    return;
	}
	if ( files && !repl_run_payload(w, es, r) )
	    ui::set(w, es, "replbusy", 0);
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
    if ( r["kind"] == madc::session_reply::bindings )
    {
	if ( seq != es_int(w, es, "replbseq", 0) )
	    return;
	ui::set(w, es, "replbseq", 0);
	repl_publish(w, es, reBINDINGS, r);
	return;
    }
    if ( r["kind"] == madc::session_reply::load )
    {
	// A file loaded (F5's buffer, a typed %run's, %load's or %call's
	// file): its main, when it defined one, runs (%call's: its call), and
	// the lines typed meanwhile are its stdin, as for a running entry;
	// %load's runs nothing. Refused:
	// its diagnostics, and the prompt is the session's (a fresh run's is
	// fresh and empty).
	if ( seq != es_int(w, es, "replloadseq", 0) )
	    return;
	ui::set(w, es, "replloadseq", 0);
	// %call's file: the session makes the call (the function named after
	// the file, else its main), whether or not the file defined main.
	bool calls = es_int(w, es, "replruncall", 0) != 0;
	ui::set(w, es, "replruncall", 0);
	var rd = r["rendered"];
	repl_feed(w, es, rd.c_str());
	repl_problems(w, es, r);
	if ( !(r["ok"] == true) )
	{
	    ui::set(w, es, "replbusy", 0);
	    repl_publish(w, es, reREFUSED, r);
	    return;
	}
	var argv;
	ui::get(argv, w, es, "replrunargv");
	if ( !calls && (php::count(argv) == 0 || !(r["main"] == true)) )
	{
	    // Nothing runs (%load, or a file without main: a main an
	    // earlier unit defined is not the file's): its work is done.
	    ui::set(w, es, "replbusy", 0);
	    repl_publish(w, es, reRAN, r);
	    return;
	}
	long h = es_int(w, es, "replh", 0);
	long rs = calls ? madc::session_call(h, argv) : madc::session_run(h, argv);
	ui::set(w, es, "replrunseq", rs);
	ui::set(w, es, "replbusy", rs);
	ui::set(w, es, "replrunning", 1);
	return;
    }
    if ( r["kind"] == madc::session_reply::run )
    {
	// F5's main (or %call's call) returned: what it printed came before
	// this reply.
	if ( seq != es_int(w, es, "replrunseq", 0) )
	    return;
	ui::set(w, es, "replrunseq", 0);
	ui::set(w, es, "replbusy", 0);
	ui::set(w, es, "replrunning", 0);
	var sv = r["shown"];			// a %call's value
	if ( strlen(sv.c_str()) > 0 )
	    repl_feed(w, es, format("{}\n", sv));
	var rd = r["rendered"];
	repl_feed(w, es, rd.c_str());
	repl_problems(w, es, r);
	repl_publish(w, es, reRAN, r);
	return;
    }
    if ( r["kind"] == madc::session_reply::build )
    {
	// %build's file built (or refused): what was built, its diagnostics,
	// and its rows into Problems, as F5's are.
	if ( seq != es_int(w, es, "replbuildseq", 0) )
	    return;
	ui::set(w, es, "replbuildseq", 0);
	ui::set(w, es, "replbusy", 0);
	var bsv = r["shown"];
	if ( strlen(bsv.c_str()) > 0 )
	    repl_feed(w, es, format("{}\n", bsv));
	var brd = r["rendered"];
	repl_feed(w, es, brd.c_str());
	repl_problems(w, es, r);
	return;
    }
    if ( r["kind"] == madc::session_reply::stopped )
    {
	repl_clear_marks(w, es);
	// A Windows backend's crash status is an NTSTATUS (0xC0000005: an
	// access violation), negative as an int: spelled as Windows tools
	// spell it. POSIX statuses (128 + signal for a signal) stay decimal.
	long code = r["exit_status"].as_integer();
	var st = format("{}", code);
	if ( code < 0 )
	    st = format("0x{:08X}", code & 4294967295);
	if ( r["restarted"] == true )
	    repl_feed(w, es, format("[the session stopped (exit {}); a new one started]\n", st));
	else
	    repl_feed(w, es, format("[the session stopped (exit {})]\n", st));
	repl_publish(w, es, reSTOPPED, r);
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
	{
	    repl_pane_sync(w, es);
	    return;
	}
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
// The IDE's own REPL commands (plan §7f; ide::repl_command) as %help lists
// them after the session's: a heading, then each usage and summary. Empty
// when no plugin registered one.
void repl_ide_help(var &out, long w)
{
    out = "";
    var rows;
    registry_rows(rows, w, "repl");
    if ( php::count(rows) == 0 )
	return;
    long width = 0;
    for ( var r : rows )
    {
	var u = r["usage"];
	if ( strlen(u.c_str()) > width )
	    width = strlen(u.c_str());
    }
    out = "IDE commands:\n";
    for ( var r : rows )
    {
	var u = r["usage"];
	var sm = r["summary"];
	out = format("{}{}{}{}\n", out, u, php::str_repeat(" ", width + 2 - strlen(u.c_str())), sm);
    }
}

// An IDE REPL command typed (query: session_command_of's row for the
// entry): answered here, before the session sees the entry — the entry shows
// and goes to history as a taken one does, then its command runs through the
// one dispatcher with the rest of the line as its argument. False = the
// entry names no IDE command.
bool repl_ide_command(long w, long es, var &text, var &query)
{
    var word = query["word"];
    long code = plugin_repl_code(w, word);
    if ( code == cmdNONE || !(query["command"] == madc::session_command::none) )
	return false;
    var p;
    repl_prompt(p, w, es);
    var shown;
    console_entry_display(shown, p.c_str(), text.c_str());
    repl_feed(w, es, format("{}\n", shown));
    repl_history_push(w, es, text.c_str());
    repl_set_input(w, es, "");
    var arg = query["argument"];
    ide_api_run(w, es, active_doc(w, es, 0), code, arg.c_str());
    return true;
}

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
    // What the entry is as a command, by the session's own rule.
    var query;
    bool command = madc::session_command_of(query, s);
    if ( command && repl_ide_command(w, es, text, query) )
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
    bool help = command && query["command"] == madc::session_command::help;
    ui::set(w, es, "replhelpseq", help ? seq : 0);
}

void repl_complete(long w, long es)
{
    long h = es_int(w, es, "replh", 0);
    if ( !madc::session_running(h) || es_int(w, es, "replbusy", 0) != 0 )
	return;
    long rin = repl_input_doc(w, es);
    var text;
    ui::text(text, w, rin);
    long caret = field_caret(w, rin);
    long seq = madc::session_complete(h, text.c_str(), caret);
    ui::set(w, es, "replcseq", seq);
    ui::set(w, es, "replccaret", caret);
    ui::set(w, es, "replctext", text);
}

// replbindings (plan §41.11a step 3d): ask the session for the names it
// defined. The answer is published as reBINDINGS with the reply's rows; only
// the newest request's is (an older seq's reply is dropped, as a
// completion's is). The session answers between entries.
void repl_bindings(long w, long es)
{
    long h = es_int(w, es, "replh", 0);
    if ( !madc::session_running(h) )
	return;
    ui::set(w, es, "replbseq", madc::session_bindings(h));
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
    show_view(w, es, repl_view(w));
    repl_start(w, es);
    ui::set(w, es, "termfocus", 0);
    ui::set(w, es, "consolefocus", repl_view(w));
}

// A fresh backend for the tab's session: F5's, Stop's and the language's
// one restart (Thonny's Stop/Restart), under the standard `doc` compiles
// under (repl_std_of), so a language change restarts through it too. The old
// state is gone, so no reply is owed and the stopped event is published, as
// when a backend ends (a plugin's rows of the old state clear). False = the
// restart failed; the transcript says why.
bool repl_restart(long w, long es, long doc)
{
    long h = es_int(w, es, "replh", 0);
    var spelling;
    repl_std_of(spelling, w, doc);
    if ( !madc::session_restart(h, spelling.c_str()) )
    {
	var why = madc::session_error(h);
	repl_feed(w, es, format("[the session did not start: {}]\n", why));
	return false;
    }
    repl_clear_marks(w, es);
    var gone = { "kind": madc::session_reply::stopped, "restarted": true };
    repl_publish(w, es, reSTOPPED, gone);
    return true;
}

// Stop (`replstop`, plan §41.11a): the tab's session restarts whether or not
// an entry runs, and the transcript says so. With no session there is
// nothing to stop.
void repl_stop_restart(long w, long es)
{
    if ( es_int(w, es, "replh", 0) == 0 )
    {
	ui::set(w, es, "msg", "The shell has no session to stop.");
	return;
    }
    if ( repl_restart(w, es, active_doc(w, es, 0)) )
	repl_feed(w, es, "[stopped; a new session started]\n");
}

// The session's language changed (seLANGUAGE, madcide_lang.inc): a live
// backend restarts under the standard the active buffer now compiles under
// (its state is lost, as Stop's is) and the transcript says so; without one,
// the next session opens under it.
void repl_on_language(long w, long es, long doc, var &event)
{
    if ( es_int(w, es, "replh", 0) == 0 )
	return;
    if ( !repl_restart(w, es, active_doc(w, es, 0)) )
	return;
    var sp;
    repl_std_of(sp, w, active_doc(w, es, 0));
    if ( strlen(sp.c_str()) == 0 )
	sp = "madc";
    repl_feed(w, es, format("[the language is {}; a new session started]\n", sp));
    repl_pane_sync(w, es);
}

// A file loaded into the tab's session: `text` under the unit name `p`, then
// its main given `argv` once the load is answered (repl_reply) — none when
// argv is empty (%load). F5's and the typed file commands' one path
// (repl_run; the offer reply's payloads, plan §7f). `fresh` (F5, %run)
// restarts a session that already existed under `doc`'s standard first, so
// the run is clean and an entry still running stops with the old one.
// `line`, when not empty, goes to the transcript as the prompt's entry (F5's
// `%run name args`; a typed command's line is already there). `call`
// (%call): argv is the file and the call's text, and the session makes the
// call (session_call) instead of running main.
bool repl_load_file(long w, long es, long doc, const char *p, const char *text,
		    var &argv, const char *line, bool fresh, bool call)
{
    var path = p;			// ring discipline: own the texts first
    var body = text;
    var shown_line = line;
    bool had = es_int(w, es, "replh", 0) != 0;
    repl_show(w, es);
    long h = es_int(w, es, "replh", 0);
    if ( !madc::session_running(h) )
	return false;			// repl_start said why
    if ( fresh && had && !repl_restart(w, es, doc) )
	return false;
    if ( fresh )
	repl_clear_marks(w, es);	// a fresh session owes nothing either
    if ( strlen(shown_line.c_str()) > 0 )
    {
	var pr;
	repl_prompt(pr, w, es);
	var shown;
	console_entry_display(shown, pr.c_str(), shown_line.c_str());
	repl_feed(w, es, format("{}\n", shown));
    }
    ui::set(w, es, "replruncall", call ? 1 : 0);
    long seq = madc::session_load(h, path.c_str(), body.c_str());
    ui::set(w, es, "replloadseq", seq);
    ui::set(w, es, "replrunpath", path);
    ui::set(w, es, "replrunargv", argv);
    ui::set(w, es, "replbusy", seq);
    return true;
}

// F5, `replrun` (plan §41.10a; Thonny's F5, IPython's %run): the buffer
// `doc` run into a fresh session (repl_load_file). The buffer's live text
// runs, unsaved edits included, under its path and the standard it compiles
// under (repl_std_of), with argv [path, the Program arguments]. An untitled
// buffer runs under the unit name `untitled` (doc_name), as Thonny's runs its
// <untitled>.
void repl_run(long w, long es, long doc)
{
    var p;
    doc_name(p, w, doc);
    if ( !doc_untitled(w, doc) && !path_is_asset(p) )
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
    var name = php::basename(p.c_str());
    // The Program arguments (the core's progargs) as typed, taken now: the
    // %run line shows them (Thonny's `%Run hello.py a b`), the run passes
    // them.
    var args;
    ui::get(args, w, es, "progargs");
    if ( args.is_null() )
	args = "";
    var line = format("%run {}", name);
    if ( strlen(args.c_str()) > 0 )
	line = format("%run {} {}", name, args);
    var argv;
    program_argv(argv, p.c_str(), args.c_str());
    repl_load_file(w, es, doc, p.c_str(), text.c_str(), argv, line.c_str(), true,
		   false);
}

// A typed file command (plan §7f): the engine split the line and asks, as
// IPython's payloads do, for its file to be loaded — %load (nothing runs),
// %run -i (main runs in this session), %run (main runs in a fresh one,
// which is this pane's to start), %call (the session then calls the
// function named after the file, else its main), %build (the session
// builds it) or %open / %edit (the editor opens it). An open buffer of the file is its live
// text and compiles under its standard, as F5 runs it; otherwise the file
// loads from disk (session_load's empty text reads it) under the active
// buffer's. False when nothing was sent.
bool repl_run_payload(long w, long es, var &r)
{
    var argv = r["argv"];
    if ( php::count(argv) == 0 )
	return false;
    var p = argv[0];
    long doc = buffer_doc_named(w, es, p.c_str());
    var text = "";
    if ( doc != 0 )
	ui::text(text, w, doc);
    else
	doc = active_doc(w, es, 0);
    if ( r["payload"] == madc::session_payload::load )
    {
	var noargs = {};
	return repl_load_file(w, es, doc, p.c_str(), text.c_str(), noargs, "", false,
			      false);
    }
    if ( r["payload"] == madc::session_payload::open )
    {
	// %open, %edit: the file in the editor, at the line when one is
	// named, and the editor takes the keyboard. Nothing is owed.
	long from = active_doc(w, es, 0);
	edit_file(w, es, from, p.c_str());
	if ( php::count(argv) > 1 )
	{
	    var ln = argv[1];
	    ui::set(w, es, "caret", caret_to_line(w, active_doc(w, es, from),
						  php::intval(ln), 0));
	}
	ui::set(w, es, "consolefocus", 0);
	return false;
    }
    if ( r["payload"] == madc::session_payload::build )
    {
	// %build: the session builds the file's live text to argv[1].
	if ( php::count(argv) < 2 )
	    return false;
	var out = argv[1];
	long bs = madc::session_build(es_int(w, es, "replh", 0), p.c_str(),
				      text.c_str(), out.c_str());
	ui::set(w, es, "replbuildseq", bs);
	ui::set(w, es, "replbusy", bs);
	return bs != 0;
    }
    if ( r["payload"] == madc::session_payload::call )	// then the call
	return repl_load_file(w, es, doc, p.c_str(), text.c_str(), argv, "", false,
			      true);
    return repl_load_file(w, es, doc, p.c_str(), text.c_str(), argv, "",
			  r["payload"] == madc::session_payload::run, false);
}

// The console view's state (repl.pane), as composition reads it: the
// transcript and the input, the prompt (none while an entry runs: the input
// is its standard input) and whether the input takes edits. Kept after
// every handler and every pass of the pump, the pane's only writers.
void repl_pane_sync(long w, long es)
{
    var p = "";
    if ( es_int(w, es, "replrunning", 0) == 0 )
	repl_prompt(p, w, es);
    var st = { "transcript": repl_doc_if_any(w, es), "input": es_int(w, es, "replin", 0),
	       "prompt": p, "editable": repl_editable(w, es) };
    ui::set(w, es, "repl.pane", st);
}

// ---- the pane's commands (cmd_handler), registered at its activation ----
bool repl_cmd_show(long w, long es, long doc, const char *arg)
{
    repl_show(w, es);
    repl_pane_sync(w, es);
    return true;
}

// F5: the stored buffer (never a view of it) into the REPL tab.
bool repl_cmd_run(long w, long es, long doc, const char *arg)
{
    repl_run(w, es, doc);
    repl_pane_sync(w, es);
    return true;
}

bool repl_cmd_stop(long w, long es, long doc, const char *arg)
{
    repl_stop_restart(w, es);
    repl_pane_sync(w, es);
    return true;
}

// Clear (`replclear`, Thonny's Edit ▸ Clear shell): the transcript empties —
// the screen model's own erase-display (CSI 2J, term_screen), the one way
// its text is cleared; the session, its names and an entry running keep on.
bool repl_cmd_clear(long w, long es, long doc, const char *arg)
{
    if ( repl_doc_if_any(w, es) != 0 )
	repl_feed(w, es, "\033[2J");
    repl_pane_sync(w, es);
    return true;
}

bool repl_cmd_bindings(long w, long es, long doc, const char *arg)
{
    repl_bindings(w, es);
    return true;
}

// The @repl scope's own: offer the input, complete, the history, the
// keyboard back to the editor.
bool repl_cmd_enter(long w, long es, long doc, const char *arg)
{
    repl_enter(w, es);
    repl_pane_sync(w, es);
    return true;
}

bool repl_cmd_complete(long w, long es, long doc, const char *arg)
{
    repl_complete(w, es);
    return true;
}

bool repl_cmd_unfocus(long w, long es, long doc, const char *arg)
{
    ui::set(w, es, "consolefocus", 0);
    ui::set(w, es, "msg", "Keyboard back to the editor.");
    return true;
}

// Older / newer: the history at the input's first / last line, else the
// arrow's own motion on the input (run on the field's document, so under the
// field's state).
bool repl_history_key(long w, long es, bool older)
{
    bool cont = true;
    if ( repl_editable(w, es) && !repl_history_step(w, es, older ? -1 : 1) )
	cont = ide_api_run(w, es, repl_input_doc(w, es), older ? cmdUP : cmdDOWN, "");
    repl_pane_sync(w, es);
    return cont;
}

bool repl_cmd_older(long w, long es, long doc, const char *arg)
{
    return repl_history_key(w, es, true);
}

bool repl_cmd_newer(long w, long es, long doc, const char *arg)
{
    return repl_history_key(w, es, false);
}

// ---- the session's events (event_handler) ----
// The open: the session starts when the layout SHOWS the pane — a
// visible pane whose active View is the REPL, as Thonny starts its backend
// with its window (chthonia.layout). Layout data decides, never the bundle's
// name; the default layout keeps the REPL hidden, so it forks nothing.
void repl_on_open(long w, long es, long doc, var &event)
{
    if ( view_shown(w, es, repl_view(w)) )
	repl_start(w, es);
    repl_pane_sync(w, es);
}

// The stop: the pump stops before any join of it (IdeSession::stop_tasks).
void repl_on_stop(long w, long es, long doc, var &event)
{
    repl_stop(w, es);
}

// The close: the session, after the drain.
void repl_on_close(long w, long es, long doc, var &event)
{
    repl_close(w, es);
}

// The activation (builtin_plugins' row): the pane's commands, its console
// view and its handlers of the session's events, through the plugin points.
// False = a registration refused (builtins_activate takes the rest back).
bool repl_activate(long w)
{
    long show = plugin_command(w, "repl", "REPL", repl_cmd_show);
    long view = plugin_view_shaped(w, "repl", "REPL", "repl.pane", show, vsCONSOLE);
    if ( show == 0 || view == viewNONE )
	return false;
    long rw = repl_world(w);
    ui::set(w, rw, "view", view);
    return plugin_command(w, "replrun", "Run in REPL", repl_cmd_run) != 0
	&& plugin_command(w, "replstop", "Stop the REPL", repl_cmd_stop) != 0
	&& plugin_command(w, "replclear", "Clear the shell", repl_cmd_clear) != 0
	&& plugin_command(w, "replbindings", "REPL bindings", repl_cmd_bindings) != 0
	&& plugin_command(w, "replenter", "REPL enter", repl_cmd_enter) != 0
	&& plugin_command(w, "replcomplete", "REPL complete", repl_cmd_complete) != 0
	&& plugin_command(w, "replolder", "REPL older", repl_cmd_older) != 0
	&& plugin_command(w, "replnewer", "REPL newer", repl_cmd_newer) != 0
	&& plugin_command(w, "replunfocus", "REPL unfocus", repl_cmd_unfocus) != 0
	&& plugin_event(w, seOPEN, repl_on_open)
	&& plugin_event(w, seSTOP, repl_on_stop)
	&& plugin_event(w, seCLOSE, repl_on_close)
	&& plugin_event(w, seLANGUAGE, repl_on_language);
}
