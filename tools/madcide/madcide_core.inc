// madcide_core.inc — the madc IDE's core (hub doc Phase 2; plan
// docs/plans/2026-08-25-madcide-phase2.md IDE-4 + the v2 owner review).
// A CONSUMER of Track 7.2: the buffer is the texteditor machinery
// (lined_core.inc + editor_events.inc, included by the entry file — one
// implementation, never a fork); the diagnostics pane and outline are
// PROJECTIONS of stored compiler data (madc::diagnostics / madc::outline
// rows on the editor-state bag — composing a tree never calls the
// compiler; the check/save actions refresh the data); keybindings are
// PROFILES — data files mapping key sequences to the action names
// dispatched here (JOE/WordStar chords by default, owner-directed; swap
// = one ui::tui_bind_keys call, the composition path untouched), and the
// help pane is a projection OF the loaded profile's own lines.
//
// Interaction state (caret/mark/bend/clip/search/prompt + pane, diags,
// outline, profile) lives on the editor-state bag; presentation state
// stays in the renderer. apply_ide_event is terminal-free
// (tests/testmadcide.mad drives it headlessly with the exact event value
// shapes).

// ---- keybinding profiles as data --------------------------------------
// ONE rule for profile lines (parse_keys and the help pane share it):
// trimmed, non-empty, not a '#' comment. False = skip this line.
bool profile_line(value &out, array &lines, long i)
{
    var line = php::trim(php::array_get_cstr(lines, i));
    const char *ls = line.c_str();
    if ( strlen(ls) == 0 )
	return false;
    if ( ls[0] == '#' )
	return false;
    out = line;
    return true;
}

// One binding per line: "SEQ ACTION" — the action is the LAST word, the
// sequence everything before it.
void parse_keys(value &table, const char *text)
{
    value t;
    array lines;
    php::explode(lines, "\n", text);
    long n = php::count(lines);
    for ( long i = 0; i < n; i = i + 1 )
    {
	value line;
	if ( !profile_line(line, lines, i) )
	    continue;
	const char *ls = line.c_str();
	const char *sp = strrchr(ls, ' ');
	if ( sp == 0 )
	    continue;
	long cut = sp - ls;
	var action = php::trim(sp + 1);
	var seq = php::trim(perl::substr(line, 0, cut));
	t[seq.c_str()] = action;
    }
    table = t;
}

// Load a named profile from the session's profile directory and, when a
// TUI is open, install it. The raw profile text stays on the bag — the
// help pane (^K H) composes FROM it. False + a status message on failure
// (the previous table stays installed — tui_bind_keys refuses whole
// tables).
bool load_profile(long w, long es, const char *name)
{
    var dir;
    ui::get(dir, w, es, "profile_dir");
    value txt;
    if ( !php::file_get_contents(txt, format("{}/{}.keys", dir, name)) )
    {
	ui::set(w, es, "msg", format("No profile '{}'.", name));
	return false;
    }
    value table;
    parse_keys(table, txt.c_str());
    long t = es_int(w, es, "tui", 0);
    if ( t != 0 )
    {
	if ( !ui::tui_bind_keys(t, table) )
	{
	    ui::set(w, es, "msg", format("Profile '{}' rejected.", name));
	    return false;
	}
    }
    ui::set(w, es, "profile", name);
    ui::set(w, es, "bindtext", txt);
    return true;
}

// ---- theme as data (AST-2): classification -> colour, a PROFILE --------
// The theme file's lines are `class colour` pairs — the SAME line rule
// and parse as the keybinding profiles (parse_keys is the one owner).
// Missing file = an empty theme (no colouring; spans stay off).
void load_theme(long w, long es, const char *name)
{
    var dir;
    ui::get(dir, w, es, "profile_dir");
    value th;
    value txt;
    if ( php::file_get_contents(txt, format("{}/{}.theme", dir, name)) )
	parse_keys(th, txt.c_str());
    ui::set(w, es, "theme", th);
}

// Highlight spans onto the bag (AST-2): the engine's classification rows
// (madc::parse_spans, source coordinates) through the THEME (class ->
// colour name), converted to the byte-offset { s, e, c } rows the edit
// node's hints speak. Computed at (re)parse — composition just attaches
// them; like the status line, they go stale with edits until the next
// check/save. An unthemed class (or a stale line past the buffer) skips.
void refresh_spans(long w, long es, long doc, long h)
{
    var hs = {};
    value th;
    ui::get(th, w, es, "theme");
    if ( !th.is_null() )
    {
	value rows;
	madc::parse_spans(rows, h);
	for ( value r : rows )
	{
	    value c = th[r["class"].c_str()];
	    if ( c.is_null() )
		continue;
	    long ls = ui::text_line_start(w, doc, r["line"].as_integer());
	    if ( ls < 0 )
		continue;
	    long s = ls + r["column"].as_integer();
	    value hr;
	    hr["s"] = s;
	    hr["e"] = s + r["length"].as_integer();
	    hr["c"] = c;
	    hs.push(hr);
	}
    }
    ui::set(w, es, "hspans", hs);
}

// ---- compiler data on the bag (the meta-level, hub demand 3) ----------
// The buffer's PARSE HANDLE (AST-1): one live parse per buffer, opened on
// first need (run_ide opens it at load), refreshed WHOLE on check / save /
// outline. Composition reads its RETAINED state — the status line's
// enclosing-function query never runs the compiler.
long ensure_phandle(long w, long es, long doc)
{
    long h = es_int(w, doc, "phandle", 0);
    if ( h != 0 )
	return h;
    value t;
    ui::text(t, w, doc);
    var p;
    ui::get(p, w, doc, "path");
    h = madc::parse_open(t.c_str(), p.c_str());
    ui::set(w, doc, "phandle", h);
    refresh_spans(w, es, doc, h);
    return h;
}

// Whole-TU re-parse of the buffer's handle from the live text — the ONE
// compiler entry (check, save, outline all route here); the themed
// highlight spans refresh with it.
long reparse_buffer(long w, long es, long doc)
{
    long h = ensure_phandle(w, es, doc);
    value t;
    ui::text(t, w, doc);
    madc::parse_refresh(h, t.c_str());
    refresh_spans(w, es, doc, h);
    return h;
}

// Refresh the stored diagnostics rows from the LIVE buffer — data only;
// composition reads the bag, and only this (check, save) runs the
// compiler.
long refresh_diags(long w, long es, long doc)
{
    long h = reparse_buffer(w, es, doc);
    value diags;
    madc::parse_check(diags, h);
    ui::set(w, es, "diags", diags);
    return php::count(diags);
}

// The check ACTION: refresh + report + show the pane.
void run_check(long w, long es, long doc)
{
    long n = refresh_diags(w, es, doc);
    ui::set(w, es, "pane", "diags");
    if ( n == 0 )
	ui::set(w, es, "msg", "Check: clean.");
    else
	ui::set(w, es, "msg", format("Check: {} problem(s).", n));
}

void refresh_outline(long w, long es, long doc)
{
    long h = reparse_buffer(w, es, doc);
    value o;
    madc::parse_outline(o, h);
    ui::set(w, es, "outline", o);
}

// A choose on the pane: move the caret to the chosen row's line. The
// rows live on the bag in pane order — the 1-based option is the index.
void goto_pane_row(long w, long es, long doc, long option)
{
    var pane;
    ui::get(pane, w, es, "pane");
    value rows;
    if ( pane == "outline" )
	ui::get(rows, w, es, "outline");
    else
	ui::get(rows, w, es, "diags");
    long i = 1;
    for ( value row : rows )
    {
	if ( i == option )
	{
	    ui::set(w, es, "caret",
		    caret_to_line(w, doc, row["line"].as_integer(), 0));
	    return;
	}
	i = i + 1;
    }
}

// ---- one prompt mode (find / goto-line / insert-file) -----------------
// Label + input + completion mode on the bag; the shared status line
// projects "label: input". While a prompt is up it consumes every event:
// text appends, backspace trims, enter completes, esc cancels.
void start_prompt(long w, long es, const char *label, const char *mode)
{
    ui::set(w, es, "prompt", label);
    ui::set(w, es, "pmode", mode);
    ui::set(w, es, "pinput", "");
}

void end_prompt(long w, long es)
{
    ui::set(w, es, "prompt", "");
    ui::set(w, es, "pmode", "");
    ui::set(w, es, "pinput", "");
}

bool prompt_active(long w, long es)
{
    var pr;
    ui::get(pr, w, es, "prompt");
    if ( pr.is_null() )
	return false;
    return strlen(pr.c_str()) > 0;
}

// ^K R: insert a file's bytes at the caret (one undo step).
void insert_file(long w, long es, long doc, long caret, const char *path)
{
    if ( !es_flag(w, es, "editable") )
    {
	refuse_edit(w, es);
	return;
    }
    value txt;
    if ( !php::file_get_contents(txt, path) )
    {
	ui::set(w, es, "msg", format("Cannot read {}.", path));
	return;
    }
    edit_checkpoint(w, es, doc, caret);
    const char *tc = txt.c_str();
    long tn = strlen(tc);
    ui::text_insert(w, doc, caret, tc);
    ui::set(w, es, "caret", caret + tn);
    ui::set(w, doc, "modified", true);
    ui::set(w, es, "msg", format("Inserted {} bytes.", tn));
}

// Prompt completion (enter): dispatch on the stored mode.
void finish_prompt(long w, long es, long doc, long caret)
{
    var mode;
    ui::get(mode, w, es, "pmode");
    var in;
    ui::get(in, w, es, "pinput");
    end_prompt(w, es);
    if ( strlen(in.c_str()) == 0 )
	return;
    if ( mode == "find" )
    {
	ui::set(w, es, "search", in.c_str());
	do_find(w, es, doc, caret);
	return;
    }
    if ( mode == "gotoline" )
    {
	ui::set(w, es, "caret", caret_to_line(w, doc, php::intval(in), 0));
	return;
    }
    if ( mode == "insertfile" )
	insert_file(w, es, doc, caret, in.c_str());
}

// Every event while the prompt is up.
bool prompt_event(long w, long es, long doc, long caret, value &ev)
{
    var kind = format("{}", ev["event"]);
    if ( kind == "text" )
    {
	var in;
	ui::get(in, w, es, "pinput");
	var nx = format("{}{}", in, ev["text"]);
	ui::set(w, es, "pinput", nx.c_str());
	return true;
    }
    if ( kind != "key" )
	return true;
    var k = format("{}", ev["key"]);
    if ( k == "enter" )
	finish_prompt(w, es, doc, caret);
    else if ( k == "esc" )
	end_prompt(w, es);
    else if ( k == "backspace" )
    {
	var in;
	ui::get(in, w, es, "pinput");
	long n = strlen(in.c_str());
	if ( n > 0 )
	    ui::set(w, es, "pinput", perl::substr(in, 0, n - 1));
    }
    return true;
}

// ---- the block model (JOE ^K B / ^K K / ^K C / ^K M / ^K Y) -----------
// begin = es "mark", end = es "bend"; when the end is unset the block
// runs mark..caret — the SAME rule the shared selection highlight
// projects. `r` answers {lo, hi}; false = no block, and this owner
// spells the ONE refusal message.
bool block_range(value &r, long w, long es, long caret)
{
    long mark = es_int(w, es, "mark", -1);
    long bend = es_int(w, es, "bend", -1);
    long e = bend >= 0 ? bend : caret;
    value b;
    b["lo"] = mark < e ? mark : e;
    b["hi"] = mark < e ? e : mark;
    r = b;
    long lo = b["lo"].as_integer();
    long hi = b["hi"].as_integer();
    if ( mark < 0 || lo >= hi )
    {
	ui::set(w, es, "msg", "No block (^K B, ^K K).");
	return false;
    }
    return true;
}

// ^K C: copy the block's text to the caret (the cursor stays on the
// insertion point, JOE-style); markers past the insertion shift with
// the text so the block keeps highlighting the ORIGINAL.
void block_copy(long w, long es, long doc, long caret)
{
    value r;
    if ( !block_range(r, w, es, caret) )
	return;
    if ( !es_flag(w, es, "editable") )
    {
	refuse_edit(w, es);
	return;
    }
    long lo = r["lo"].as_integer();
    long hi = r["hi"].as_integer();
    long n = hi - lo;
    value t;
    ui::text(t, w, doc);
    var blk = perl::substr(t, lo, n);
    edit_checkpoint(w, es, doc, caret);
    ui::text_insert(w, doc, caret, blk.c_str());
    ui::set(w, doc, "modified", true);
    long mark = es_int(w, es, "mark", -1);
    long bend = es_int(w, es, "bend", -1);
    if ( mark >= 0 && caret <= mark )
	ui::set(w, es, "mark", mark + n);
    if ( bend >= 0 && caret <= bend )
	ui::set(w, es, "bend", bend + n);
}

// ^K M: move the block to the caret (one undo step; the cursor lands on
// the moved block's start; the block markers clear).
void block_move(long w, long es, long doc, long caret)
{
    value r;
    if ( !block_range(r, w, es, caret) )
	return;
    long lo = r["lo"].as_integer();
    long hi = r["hi"].as_integer();
    if ( caret > lo && caret < hi )
    {
	ui::set(w, es, "msg", "Cannot move a block into itself.");
	return;
    }
    if ( !es_flag(w, es, "editable") )
    {
	refuse_edit(w, es);
	return;
    }
    long n = hi - lo;
    value t;
    ui::text(t, w, doc);
    var blk = perl::substr(t, lo, n);
    edit_checkpoint(w, es, doc, caret);
    ui::text_erase(w, doc, lo, n);
    long at = caret;
    if ( at >= hi )
	at = at - n;
    ui::text_insert(w, doc, at, blk.c_str());
    ui::set(w, es, "caret", at);
    ui::set(w, es, "mark", -1);
    ui::set(w, es, "bend", -1);
    ui::set(w, doc, "modified", true);
}

// ^K Y: delete the block (the clip takes its text — read-only copies,
// like the shared cut rule); the caret lands on the cut point.
void block_del(long w, long es, long doc, long caret)
{
    value r;
    if ( !block_range(r, w, es, caret) )
	return;
    long lo = r["lo"].as_integer();
    long hi = r["hi"].as_integer();
    value t;
    ui::text(t, w, doc);
    ui::set(w, es, "clip", perl::substr(t, lo, hi - lo));
    ui::set(w, es, "mark", -1);
    ui::set(w, es, "bend", -1);
    if ( !es_flag(w, es, "editable") )
    {
	var ro;
	ui::get(ro, w, es, "ro_msg");
	var m = format("{} (Copied.)", ro);
	ui::set(w, es, "msg", m.c_str());
	return;
    }
    edit_checkpoint(w, es, doc, caret);
    ui::text_erase(w, doc, lo, hi - lo);
    long nc = caret;
    if ( nc > hi )
	nc = nc - (hi - lo);
    else if ( nc > lo )
	nc = lo;
    ui::set(w, es, "caret", nc);
    ui::set(w, doc, "modified", true);
}

// ---- direct editing (^Y delete line, ^W delete word right) ------------
void delete_line(long w, long es, long doc, long caret)
{
    if ( !es_flag(w, es, "editable") )
    {
	refuse_edit(w, es);
	return;
    }
    long line = line_of(w, doc, caret);
    long start = ui::text_line_start(w, doc, line);
    if ( start < 0 )
	return;
    long len = ui::text_line_len(w, doc, line);
    long n = len;
    if ( start + len < ui::text_size(w, doc) )
	n = len + 1;	// the terminating newline goes too
    if ( n == 0 )
	return;
    edit_checkpoint(w, es, doc, caret);
    ui::text_erase(w, doc, start, n);
    ui::set(w, es, "caret", start);
    ui::set(w, doc, "modified", true);
}

void delete_word(long w, long es, long doc, long caret)
{
    if ( !es_flag(w, es, "editable") )
    {
	refuse_edit(w, es);
	return;
    }
    long e = ui::text_word_right(w, doc, caret);
    if ( e <= caret )
	return;
    edit_checkpoint(w, es, doc, caret);
    ui::text_erase(w, doc, caret, e - caret);
    ui::set(w, doc, "modified", true);
}

// ---- the view seam (AST-3): read-only code views -----------------------
// A VIEW is a document lens applied at composition: the stored text stays
// the document truth; what is DISPLAYED is the lens's rendered text
// (madc::emit over the live buffer), held in a view-buffer entity so the
// ONE shared navigation implementation (edit_key, find, goto-line) works
// over the display space unchanged. The lens's coordinate map rides as
// data on the bag ("vmap" — EMPTY for a wholly rendered view: nothing
// corresponds; see ui::lens_to_display) and onto the edit node's hints;
// the stored caret parks on "ocaret" and returns on exit; mutations and
// stored-space history are refused through the one editable gate. The
// identity lens (no view) adds NOTHING — the composed tree stays
// byte-identical to today's.

// THE view-active rule, one seat (dupaudit family view_active_predicate):
// a view is active iff the editor-state bag carries a non-empty "view"
// name. Out = the name; false = the original (identity lens).
bool view_name(value &out, long w, long es)
{
    var vw;
    ui::get(vw, w, es, "view");
    if ( vw.is_null() || strlen(vw.c_str()) == 0 )
	return false;
    out = vw;
    return true;
}

// The navigation target: the view buffer while a view is up, else the
// document — the ONE place display-vs-stored routing happens.
long nav_doc(long w, long es, long doc)
{
    value vw;
    if ( !view_name(vw, w, es) )
	return doc;
    long vd = es_int(w, es, "vdoc", 0);
    return vd != 0 ? vd : doc;
}

bool enter_view(long w, long es, long doc, const char *name)
{
    value t;
    ui::text(t, w, doc);
    var p;
    ui::get(p, w, doc, "path");
    value emitted;
    if ( !madc::emit(emitted, t.c_str(), p.c_str(), name) )
    {
	// The buffer does not translate: surface the compiler's own rows
	// instead of a bare refusal.
	run_check(w, es, doc);
	return false;
    }
    long vd = es_int(w, es, "vdoc", 0);
    if ( vd == 0 )
    {
	vd = ui::create(w, "view-buffer");
	ui::set(w, es, "vdoc", vd);
    }
    ui::text_load(w, vd, emitted.c_str());
    value cur;
    if ( !view_name(cur, w, es) )
	ui::set(w, es, "ocaret", es_int(w, es, "caret", 0));
    ui::set(w, es, "view", name);
    var vm = {};
    ui::set(w, es, "vmap", vm);
    ui::set(w, es, "caret", 0);
    ui::set(w, es, "mark", -1);
    ui::set(w, es, "bend", -1);
    ui::set(w, es, "editable", false);
    ui::set(w, es, "ro_msg", "Read-only view (^K N returns).");
    ui::set(w, es, "msg", format("View: {}", name));
    return true;
}

void exit_view(long w, long es, long doc)
{
    ui::set(w, es, "view", "");
    ui::set(w, es, "caret", es_int(w, es, "ocaret", 0));
    ui::set(w, es, "mark", -1);
    ui::set(w, es, "bend", -1);
    derive_access(w, es);	// the file's own verdict returns
}

// The c++ view applies when the BUFFER'S OWN language is C/C++ (by
// extension — the app's document-kind rule): its render is source-shaped
// (madc::emit "c++" echoes the retained source), and a madc-dialect
// buffer has no C++ respelling yet (the named seat, design doc §3.2).
bool cxx_view_applies(long w, long es, long doc)
{
    var p;
    ui::get(p, w, doc, "path");
    const char *pc = p.c_str();
    const char *dot = strrchr(pc, '.');
    if ( dot == 0 )
	return false;
    var ext = format("{}", dot);
    return ext == ".c" || ext == ".cc" || ext == ".cpp" || ext == ".cxx"
	|| ext == ".h" || ext == ".hpp";
}

// ^K N: original -> mc11 -> c11 [-> c++ on a C/C++ buffer] -> original.
// A failed enter falls back to the original — the session is never
// trapped inside a stale view.
void cycle_view(long w, long es, long doc)
{
    value vw;
    if ( !view_name(vw, w, es) )
    {
	enter_view(w, es, doc, "mc11");
	return;
    }
    if ( vw == "mc11" )
    {
	if ( !enter_view(w, es, doc, "c11") )
	    exit_view(w, es, doc);
	return;
    }
    if ( vw == "c11" && cxx_view_applies(w, es, doc) )
    {
	if ( !enter_view(w, es, doc, "c++") )
	    exit_view(w, es, doc);
	return;
    }
    exit_view(w, es, doc);
}

// ---- ^K Z: hand the terminal to a shell -------------------------------
// Suspend the grid, run the user's shell on the real tty, resume with a
// full repaint. $SHELL falls back to /bin/sh INSIDE the command — libc
// system() inherits the terminal, and no getenv is needed (the dlsym
// fallback would hand the pointer back as a long).
void open_shell(long w, long es)
{
    long t = es_int(w, es, "tui", 0);
    if ( t == 0 )
    {
	ui::set(w, es, "msg", "No terminal to suspend.");
	return;
    }
    if ( !ui::tui_suspend(t) )
	return;
    system("exec \"${SHELL:-/bin/sh}\"");
    ui::tui_resume(t);
}

// ---- the action dispatch ----------------------------------------------
// Save, then quit if the save took (the JOE ^K X shape) — two verbs from
// the DISPATCHER, zero duplicated verb logic (verbs never re-enter act).
bool save_quit(long w, long es, long doc)
{
    if ( !do_verb(w, es, "w") )
	return false;
    var m;
    ui::get(m, w, doc, "modified");
    if ( m.is_null() || !m.as_boolean() )
	return do_verb(w, es, "q");
    return true;	// the save failed; its message stands
}

// Save + a silent diagnostics refresh (check-on-save): the verb's own
// answer ("Wrote N bytes." or the refusal) stays the status message.
bool save_and_check(long w, long es, long doc)
{
    bool run = do_verb(w, es, "w");
    refresh_diags(w, es, doc);
    return run;
}

// Toggle the lower pane between `name` and hidden.
void toggle_pane(long w, long es, const char *name)
{
    var pane;
    ui::get(pane, w, es, "pane");
    if ( pane == name )
	ui::set(w, es, "pane", "");
    else
	ui::set(w, es, "pane", name);
}

// Apply ONE semantic event (the value shapes ui::tui_event produces —
// action events from the bindings table included). False = quit.
bool apply_ide_event(long w, long es, long doc, value &ev)
{
    var kind = format("{}", ev["event"]);
    // Display-vs-stored routing (the view seam): navigation, find, and the
    // caret run over the NAV target — the view buffer while a view is up.
    // Document semantics (save, check, outline, the mutation gates) stay
    // on the stored document.
    long nav = nav_doc(w, es, doc);
    bool viewing = nav != doc;
    long caret = clamped_caret(w, es, nav);
    ui::set(w, es, "msg", "");

    if ( prompt_active(w, es) )
	return prompt_event(w, es, nav, caret, ev);

    if ( kind == "action" )
    {
	var a = format("{}", ev["action"]);
	if ( strlen(a.c_str()) == 0 )
	{
	    ui::set(w, es, "msg", format("Unbound: {}", ev["seq"]));
	    return true;
	}
	if ( a == "view" )
	{
	    cycle_view(w, es, doc);
	    return true;
	}
	// Stored-space interaction state (block markers) and history are
	// meaningless against a displayed view: the one refusal, up front
	// (mutating actions below refuse through the editable gate).
	if ( viewing )
	{
	    if ( a == "mark" || a == "bend" || a == "undo" || a == "redo" )
	    {
		refuse_edit(w, es);
		return true;
	    }
	}
	if ( a == "save" )
	    return save_and_check(w, es, doc);
	if ( a == "savequit" )
	    return save_quit(w, es, doc);
	if ( a == "quit" )
	    return do_verb(w, es, "q");
	if ( a == "discard" )
	    return do_verb(w, es, "q!");
	if ( a == "find" )
	{
	    start_prompt(w, es, "Find", "find");
	    return true;
	}
	if ( a == "findnext" )
	{
	    do_find(w, es, nav, caret);
	    return true;
	}
	if ( a == "gotoline" )
	{
	    start_prompt(w, es, "Go to line", "gotoline");
	    return true;
	}
	if ( a == "insertfile" )
	{
	    start_prompt(w, es, "Insert file", "insertfile");
	    return true;
	}
	if ( a == "mark" )
	{
	    ui::set(w, es, "mark", caret);
	    return true;
	}
	if ( a == "bend" )
	{
	    ui::set(w, es, "bend", caret);
	    return true;
	}
	if ( a == "blockcopy" )
	{
	    block_copy(w, es, doc, caret);
	    return true;
	}
	if ( a == "blockmove" )
	{
	    block_move(w, es, doc, caret);
	    return true;
	}
	if ( a == "blockdel" )
	{
	    block_del(w, es, doc, caret);
	    return true;
	}
	if ( a == "undo" )
	{
	    undo_edit(w, es, doc);
	    return true;
	}
	if ( a == "redo" )
	{
	    redo_edit(w, es, doc);
	    return true;
	}
	if ( a == "delline" )
	{
	    delete_line(w, es, doc, caret);
	    return true;
	}
	if ( a == "delword" )
	{
	    delete_word(w, es, doc, caret);
	    return true;
	}
	if ( a == "wordleft" )
	{
	    ui::set(w, es, "caret", ui::text_word_left(w, nav, caret));
	    return true;
	}
	if ( a == "wordright" )
	{
	    ui::set(w, es, "caret", ui::text_word_right(w, nav, caret));
	    return true;
	}
	if ( a == "top" )
	{
	    ui::set(w, es, "caret", 0);
	    return true;
	}
	if ( a == "bottom" )
	{
	    ui::set(w, es, "caret", ui::text_size(w, nav));
	    return true;
	}
	if ( a == "check" )
	{
	    run_check(w, es, doc);
	    return true;
	}
	if ( a == "outline" )
	{
	    var pane;
	    ui::get(pane, w, es, "pane");
	    if ( pane == "outline" )
		ui::set(w, es, "pane", "");
	    else
	    {
		refresh_outline(w, es, doc);
		ui::set(w, es, "pane", "outline");
	    }
	    return true;
	}
	if ( a == "help" )
	{
	    toggle_pane(w, es, "help");
	    return true;
	}
	if ( a == "shell" )
	{
	    open_shell(w, es);
	    return true;
	}
	if ( a == "profile" )
	{
	    var cur;
	    ui::get(cur, w, es, "profile");
	    if ( cur == "joe" )
		load_profile(w, es, "pico");
	    else
		load_profile(w, es, "joe");
	    return true;
	}
	// Motion (and the direct edit keys) DELEGATE to the shared
	// edit_key vocabulary: the action name IS the key spelling
	// ("left", "pgdn", "del", ...) — the profile binds "^b left",
	// the dispatcher synthesizes the key event. One movement
	// implementation.
	value ke;
	ke["event"] = "key";
	ke["key"] = a;
	if ( edit_key(w, es, nav, caret, ke) )
	    return true;
	ui::set(w, es, "msg", format("Unknown action '{}'.", a));
	return true;
    }
    if ( kind == "text" )
    {
	edit_text_run(w, es, nav, caret, ev);
	return true;
    }
    if ( kind == "choose" )
    {
	var a = format("{}", ev["action"]);
	if ( a == "goto" )
	{
	    // Pane rows carry STORED line numbers — meaningless against a
	    // displayed view.
	    if ( viewing )
		refuse_edit(w, es);
	    else
		goto_pane_row(w, es, doc, ev["option"].as_integer());
	    return true;
	}
	if ( a == "w" )
	    return save_and_check(w, es, doc);
	if ( a == "f" )
	{
	    start_prompt(w, es, "Find", "find");
	    return true;
	}
	if ( a == "q" )
	    return do_verb(w, es, "q");
	return true;
    }
    if ( kind != "key" )
	return true;	// focus / resize: the loop recomposes anyway

    edit_key(w, es, nav, caret, ev);
    return true;	// unbound keys are inert (chords arrive as actions)
}

// ---- the projection ----------------------------------------------------
// The tree is PROFILE-INDEPENDENT by construction where the content is
// document/compiler data; the HELP pane is deliberately profile-DATA —
// it projects the loaded profile's own lines. The Phase-2 gate's third
// clause (swap leaves the tree identical) is pinned with the help pane
// hidden.
void compose_ide_tree(value &root, long w, long es, long doc)
{
    long caret = es_int(w, es, "caret", 0);
    // The view seam at composition: the edit node presents the NAV
    // target's text (the lens's get) and, while a view is up, carries the
    // lens data — the view name and its coordinate map — as hints. The
    // identity lens (no view) takes neither branch: byte-identical.
    long nav = nav_doc(w, es, doc);
    bool viewing = nav != doc;

    value head;
    value ed;
    var p;
    ui::get(p, w, doc, "path");
    if ( viewing )
    {
	value vw;
	view_name(vw, w, es);
	compose_heading(head, w, es, doc,
			format("madcide: {} [{}]", p, vw));
	value ed2;
	compose_edit_node(ed2, w, es, nav, caret);
	value h;
	h = ed2["hints"];
	h["view"] = vw;
	value vm;
	ui::get(vm, w, es, "vmap");
	h["map"] = vm;
	ed2["hints"] = h;
	ed = ed2;
    }
    else
    {
	compose_heading(head, w, es, doc, format("madcide: {}", p));
	compose_edit_node(ed, w, es, nav, caret);
	// Highlight spans (AST-2): the bag's themed byte-range rows ride
	// the hints — stored-space only (a view shows a render), computed
	// at (re)parse, just attached here. Same RMW as the view hints.
	value hs;
	ui::get(hs, w, es, "hspans");
	if ( !hs.is_null() && php::count(hs) > 0 )
	{
	    value h2;
	    h2 = ed["hints"];
	    h2["spans"] = hs;
	    ed["hints"] = h2;
	}
    }

    value r;
    r["role"] = "group";
    var kids = {};
    kids.push(head);
    kids.push(ed);

    var pane;
    ui::get(pane, w, es, "pane");
    if ( !pane.is_null() && strlen(pane.c_str()) > 0 )
    {
	value box;
	box["role"] = "choice";
	var items = {};
	long nrows = 0;
	if ( pane == "help" )
	{
	    // help = a projection of the loaded profile's own data
	    var prof;
	    ui::get(prof, w, es, "profile");
	    box["label"] = format("Help — {} profile", prof);
	    var bt;
	    ui::get(bt, w, es, "bindtext");
	    array lines;
	    php::explode(lines, "\n", bt.c_str());
	    long n = php::count(lines);
	    for ( long i = 0; i < n; i = i + 1 )
	    {
		value line;
		if ( !profile_line(line, lines, i) )
		    continue;
		value it;
		it["role"] = "item";
		it["content"] = line;
		items.push(it);
		nrows = nrows + 1;
	    }
	}
	else
	{
	    // diagnostics / outline: projections of the STORED compiler
	    // data — navigable rows carrying the `goto` action.
	    bool outline_pane = pane == "outline";
	    value rows;
	    if ( outline_pane )
		ui::get(rows, w, es, "outline");
	    else
		ui::get(rows, w, es, "diags");
	    if ( outline_pane )
		box["label"] = "Outline";
	    else
		box["label"] = "Diagnostics";
	    for ( value row : rows )
	    {
		value it;
		it["role"] = "item";
		if ( outline_pane )
		    it["content"] = format("{} {} :{}", row["kind"],
					   row["name"], row["line"]);
		else
		    it["content"] = format("{}:{} {} {}", row["line"],
					   row["column"], row["severity"],
					   row["message"]);
		var ga = {};
		ga.push("goto");
		it["actions"] = ga;
		items.push(it);
		nrows = nrows + 1;
	    }
	    if ( nrows == 0 )
	    {
		value it;
		it["role"] = "item";
		if ( outline_pane )
		    it["content"] = "(no definitions)";
		else
		    it["content"] = "(clean)";
		items.push(it);
	    }
	}
	box["children"] = items;
	kids.push(box);
    }

    value status;
    // The status line's enclosing-function suffix (AST-1): a RETAINED-state
    // read of the buffer's parse handle — stored-space only (a view's caret
    // is display-space), and only once something opened the handle;
    // composition never runs the compiler.
    var fnsuffix = "";
    value vw_st;
    if ( !view_name(vw_st, w, es) )
    {
	long ph = es_int(w, doc, "phandle", 0);
	if ( ph != 0 )
	{
	    long cl = line_of(w, doc, caret);
	    long cs = ui::text_line_start(w, doc, cl);
	    if ( cs < 0 )
		cs = 0;
	    value encl;
	    madc::parse_enclosing(encl, ph, cl, caret - cs);
	    if ( !encl.is_null() )
		fnsuffix = format("  fn {}", encl["name"]);
	}
    }
    compose_status(status, w, es, nav, caret, fnsuffix.c_str());
    value menu;
    compose_menu(menu);

    kids.push(status);
    kids.push(menu);
    r["children"] = kids;
    root = r;
}

// The IDE loop: the same compose-as-data -> render -> event -> apply
// cycle as vised, with the JOE profile installed at open.
void run_ide(const char *path, bool ro)
{
    long w = ui::world_new();
    long doc = setup_editor(w, path, ro);
    if ( doc == 0 )
    {
	printf("cannot read %s\n", path);
	return;
    }
    long es = ui::entity_by_name(w, "editor-state");
    ui::set(w, es, "profile_dir", "tools/madcide/profiles");
    long t = ui::tui_open();
    if ( t == 0 )
	return;
    ui::set(w, es, "tui", t);
    load_profile(w, es, "joe");
    load_theme(w, es, "default");
    ensure_phandle(w, es, doc);	// parse-on-load: the status line and the
				// highlight spans have state before the
				// first check
    bool run = true;
    while ( run )
    {
	value tree;
	compose_ide_tree(tree, w, es, doc);
	ui::tui_render(t, w, tree);
	value ev;
	if ( !ui::tui_event(ev, t, w) )
	    break;
	run = apply_ide_event(w, es, doc, ev);
    }
    long ph = es_int(w, doc, "phandle", 0);
    if ( ph != 0 )
	madc::parse_close(ph);
    ui::tui_close(t);
    ui::world_close(w);
}
