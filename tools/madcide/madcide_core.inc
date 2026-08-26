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

// ---- theme as data (AST-2): classification -> style spec, a SCHEME ----
// One `class SPEC` pair per line: the class is the FIRST word, the spec
// the rest — JOE's style vocabulary (attribute words + a colour +
// bg_colour, e.g. "keyword bold yellow"); the renderer's tui_attr_of
// parses specs at paint. Line rule (comments/blanks) = profile_line,
// shared with the keybindings; the SPLIT differs by design — a binding
// ENDS in one action word, a theme line ENDS in a multi-word spec.
// Schemes are just theme files: profiles/<name>.theme, swapped by name
// (the profile-swap precedent). False = no such theme (the previous one
// stays); an es "scheme" records the loaded name.
bool load_theme(long w, long es, const char *name)
{
    var dir;
    ui::get(dir, w, es, "profile_dir");
    value txt;
    if ( !php::file_get_contents(txt, format("{}/{}.theme", dir, name)) )
	return false;
    value th;
    array lines;
    php::explode(lines, "\n", txt.c_str());
    long n = php::count(lines);
    for ( long i = 0; i < n; i = i + 1 )
    {
	value line;
	if ( !profile_line(line, lines, i) )
	    continue;
	const char *ls = line.c_str();
	const char *sp = strchr(ls, ' ');
	if ( sp == 0 )
	    continue;
	long cut = sp - ls;
	var cls = php::trim(perl::substr(line, 0, cut));
	var spec = php::trim(sp + 1);
	th[cls.c_str()] = spec;
    }
    ui::set(w, es, "theme", th);
    ui::set(w, es, "scheme", name);
    return true;
}

// ---- status line as data (IDE-9a): JOE's joerc format vocabulary ------
// profiles/<name>.status: "lmsg FMT" / "rmsg FMT" lines (the profile_line
// comment/blank rule; the value is everything after the FIRST space,
// kept VERBATIM — rmsg's first character is the FILL between the two
// halves, exactly joerc's convention). A missing file means the JOE
// joerc defaults — the status format is optional data, so no message
// (unlike a NAMED theme swap).
void load_status(long w, long es, const char *name)
{
    var lmsg = "\\i%k%T%W%I%X %n %m%y%R %M %x";	// joerc's defaults
    var rmsg = " %S Row %4r Col %3c";		// first char = fill
    var dir;
    ui::get(dir, w, es, "profile_dir");
    value txt;
    if ( php::file_get_contents(txt, format("{}/{}.status", dir, name)) )
    {
	array lines;
	php::explode(lines, "\n", txt.c_str());
	long n = php::count(lines);
	for ( long i = 0; i < n; i = i + 1 )
	{
	    value line;
	    if ( !profile_line(line, lines, i) )
		continue;
	    const char *ls = line.c_str();
	    const char *sp = strchr(ls, ' ');
	    if ( sp == 0 )
		continue;
	    long cut = sp - ls;
	    var key = perl::substr(line, 0, cut);
	    var fmt = sp + 1;			// verbatim: fill survives
	    if ( key == "lmsg" )
		lmsg = fmt;
	    if ( key == "rmsg" )
		rmsg = fmt;
	}
    }
    ui::set(w, es, "lmsg", lmsg);
    ui::set(w, es, "rmsg", rmsg);
}

// Highlight spans onto the bag (AST-2): the engine's classification rows
// (source coordinates) through the THEME (class -> colour name),
// converted to the byte-offset { s, e, c } rows the edit node's hints
// speak. ONE converter, two feeders: parse_spans (refresh_spans, the
// full-parse state) and lex_spans (colour at load before the parse
// lands). Like the status line, spans go stale with edits until the
// next check/save. An unthemed class (or a stale line) skips.
void spans_to_hspans(long w, long es, long doc, value &rows)
{
    var hs = {};
    value th;
    ui::get(th, w, es, "theme");
    if ( !th.is_null() )
    {
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

void refresh_spans(long w, long es, long doc, long h)
{
    value rows;
    madc::parse_spans(rows, h);
    spans_to_hspans(w, es, doc, rows);
}

// Lexical colour at load (staged parsing stage 1): keyword / number /
// string / comment from LEXING the buffer alone — milliseconds where a
// C++ parse is seconds — so the FIRST paint is already in colour. The
// full parse's spans replace these when the handle lands.
void lex_first_spans(long w, long es, long doc)
{
    value t;
    ui::text(t, w, doc);
    var p;
    ui::get(p, w, doc, "path");
    value rows;
    madc::lex_spans(rows, t.c_str(), p.c_str());
    spans_to_hspans(w, es, doc, rows);
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
    if ( h == 0 )
    {
	// LOUD, never silent: no handle means no status-line function,
	// no diagnostics, no highlighting — say so where it happened.
	ui::set(w, es, "msg", format("Parse open failed for {}.", p));
	return 0;
    }
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

// The keybinding-profile toggle (joe <-> pico): ONE implementation for
// the ^K P action and the options pane's Keymap row.
void toggle_profile(long w, long es)
{
    var cur;
    ui::get(cur, w, es, "profile");
    if ( cur == "joe" )
    {
	load_profile(w, es, "pico");
	load_status(w, es, "pico");	// the format rides the profile
    }
    else
    {
	load_profile(w, es, "joe");
	load_status(w, es, "joe");
    }
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

// THE view-active rule, one seat (dupaudit family view_active_predicate):
// a view is active iff the editor-state bag carries a non-empty "view"
// name. Out = the name; false = the original (identity lens). Defined
// here, ahead of the prompt machinery (the theme completion consults it)
// and of the view seam below.
bool view_name(value &out, long w, long es)
{
    var vw;
    ui::get(vw, w, es, "view");
    if ( vw.is_null() || strlen(vw.c_str()) == 0 )
	return false;
    out = vw;
    return true;
}

// Render one joerc-style format string. Supported seats: %n name,
// %m "(Modified) ", %R "(Read only) ", %r row / %c col (an optional
// width digit-run left-pads: %4r), %o byte offset, %k the pending chord
// (JOE's chord echo, via ui::tui_pending), %x the enclosing context —
// madcide renders its AST-backed enclosing function here, superior to
// JOE's textual backward heuristic — %y the active VIEW name in JOE's
// "(syntax)" seat, and %% a literal percent. UNSUPPORTED joerc escapes
// render EMPTY and light up as their features land (%T %W %I %X %M %S
// ...). \X attribute escapes are consumed — the status row renders
// inverse whole-line; intra-line styling is a named seat.
// The format and the context arrive as VALUES: .c_str() text is
// ring-lifetime (the pre-L3 contract) and this loop makes many
// ring-returning calls — the format is copied into a frame-owned buffer
// at entry, never held as a borrowed pointer.
void status_expand(value &out, long w, long es, long doc, long caret,
		   value &fmtv, value &fnctxv)
{
    char fmt[240];
    strncpy(fmt, fmtv.c_str(), 239);
    fmt[239] = 0;
    var s = "";
    long line = line_of(w, doc, caret);
    long start = ui::text_line_start(w, doc, line);
    if ( start < 0 )
	start = 0;
    long col = caret - start + 1;
    long fl = strlen(fmt);
    long i = 0;
    long run = 0;
    while ( i < fl )
    {
	char c = fmt[i];
	if ( c != '%' && c != '\\' )
	{
	    i = i + 1;
	    continue;
	}
	if ( i > run )
	    s = format("{}{}", s, perl::substr(fmt, run, i - run));
	if ( c == '\\' )
	{
	    i = i + 2;				// attribute escape: consumed
	    run = i;
	    continue;
	}
	long j = i + 1;
	long width = 0;
	while ( j < fl && fmt[j] >= '0' && fmt[j] <= '9' )
	{
	    width = width * 10 + (fmt[j] - '0');
	    j = j + 1;
	}
	if ( j >= fl )
	    break;				// a trailing bare %: dropped
	char e = fmt[j];
	var seat = "";
	if ( e == '%' )
	    seat = "%";
	if ( e == 'n' )
	{
	    var p;
	    ui::get(p, w, doc, "path");
	    seat = p;
	}
	if ( e == 'm' )
	{
	    var m;
	    ui::get(m, w, doc, "modified");
	    if ( !m.is_null() && m.as_boolean() )
		seat = "(Modified) ";
	}
	if ( e == 'R' )
	{
	    if ( !es_flag(w, es, "editable") )
		seat = "(Read only) ";
	}
	if ( e == 'r' )
	    seat = format("{}", line);
	if ( e == 'c' )
	    seat = format("{}", col);
	if ( e == 'o' )
	    seat = format("{}", caret);
	if ( e == 'k' )
	{
	    long t = es_int(w, es, "tui", 0);
	    if ( t != 0 )
	    {
		value pc;
		ui::tui_pending(pc, t);
		if ( !pc.is_null() && strlen(pc.c_str()) > 0 )
		    seat = format("{} ", pc);
	    }
	}
	if ( e == 'x' )
	    seat = fnctxv;
	if ( e == 'y' )
	{
	    value vw;
	    if ( view_name(vw, w, es) )
		seat = format("({}) ", vw);
	}
	long pad = width - strlen(seat.c_str());
	if ( pad > 0 )
	    s = format("{}{}", s, php::str_repeat(" ", pad));
	s = format("{}{}", s, seat);
	i = j + 1;
	run = i;
    }
    if ( i > run )
	s = format("{}{}", s, perl::substr(fmt, run, i - run));
    out = s;
}

// The JOE screen model's status line (IDE-9a/9b): ONE inverse row at the
// TOP, lmsg left / rmsg right with rmsg's fill character between; the
// one prompt mode, the search prompt, and transient messages OVERLAY it
// (the same priority rule as the shared compose_status — restated here
// because the SCREEN MODELS diverge by design: madcide is JOE-shaped,
// vised keeps the demo's bottom bar).
void compose_joe_status(value &status, long w, long es, long doc,
			long caret, value &fnctx)
{
    value st;
    st["role"] = "status";
    var pr;
    ui::get(pr, w, es, "prompt");
    if ( !pr.is_null() && strlen(pr.c_str()) > 0 )
    {
	var pi;
	ui::get(pi, w, es, "pinput");
	st["content"] = format("{} (^C aborts): {}", pr, pi);
	status = st;
	return;
    }
    if ( es_flag(w, es, "searching") )
    {
	var sch;
	ui::get(sch, w, es, "search");
	st["content"] = format("Find (^C aborts): {}", sch);
	status = st;
	return;
    }
    var msg;
    ui::get(msg, w, es, "msg");
    if ( !msg.is_null() && strlen(msg.c_str()) > 0 )
    {
	st["content"] = msg;
	status = st;
	return;
    }
    var lm;
    ui::get(lm, w, es, "lmsg");
    if ( lm.is_null() || strlen(lm.c_str()) == 0 )
    {
	// Lazy first-compose load (an embedder that never ran run_ide
	// still gets a real status line); load_status never leaves the
	// formats empty, so this runs once.
	var prof;
	ui::get(prof, w, es, "profile");
	load_status(w, es, prof.is_null() ? "joe" : prof.c_str());
	ui::get(lm, w, es, "lmsg");
    }
    var rm;
    ui::get(rm, w, es, "rmsg");
    value lout;
    status_expand(lout, w, es, doc, caret, lm, fnctx);
    var fill = " ";
    var rfmt = "";
    if ( strlen(rm.c_str()) > 0 )
    {
	fill = perl::substr(rm, 0, 1);
	rfmt = perl::substr(rm, 1, strlen(rm.c_str()) - 1);
    }
    value rout;
    status_expand(rout, w, es, doc, caret, rfmt, fnctx);
    long t = es_int(w, es, "tui", 0);
    long cols = t != 0 ? ui::tui_cols(t) : 80;
    if ( cols < 1 )
	cols = 80;
    // The renderer's status arm prepends one space — the usable width is
    // cols - 1. A gap that would go negative just joins with one fill.
    long gap = cols - 1 - strlen(lout.c_str()) - strlen(rout.c_str());
    if ( gap < 1 )
	gap = 1;
    st["content"] = format("{}{}{}", lout, php::str_repeat(fill.c_str(), gap),
			   rout);
    status = st;
}

// ---- one prompt mode (find / goto-line / insert-file / theme) ---------
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

// AST-5 buffers (defined after the view machinery): ^K E open-or-switch.
void edit_file(long w, long es, long olddoc, const char *path);

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
    {
	insert_file(w, es, doc, caret, in.c_str());
	return;
    }
    if ( mode == "editfile" )
    {
	edit_file(w, es, doc, in.c_str());
	return;
    }
    if ( mode == "tabwidth" )
    {
	long tw = php::intval(in);
	if ( tw < 1 || tw > 16 )
	{
	    ui::set(w, es, "msg", format("Tab width '{}' — need 1..16.", in));
	    return;
	}
	ui::set(w, es, "tabwidth", tw);
	ui::set(w, es, "msg", format("Tab width {}.", tw));
	return;
    }
    if ( mode == "theme" )
    {
	// A colour SCHEME swap (owner: like the keybinding profiles):
	// load the named theme and recolour from the handle's retained
	// state. `doc` here is the NAV target — while a view is up the
	// stored buffer recolours on exit/reparse instead (spans are
	// stored-space only), so the immediate refresh is skipped.
	if ( !load_theme(w, es, in.c_str()) )
	{
	    ui::set(w, es, "msg", format("No theme '{}'.", in));
	    return;
	}
	value vw;
	if ( !view_name(vw, w, es) )
	    refresh_spans(w, es, doc, ensure_phandle(w, es, doc));
	ui::set(w, es, "msg", format("Theme: {}.", in));
    }
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
    ui::set(w, vd, "path", p);	// the status %n seat: a view is a lens
				// over the file — its name IS the file's
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

// ---- AST-5: buffers (IDE-8) ------------------------------------------
// JOE's model: ^K E prompts for a file; an open path SWITCHES to that
// buffer (caret where it was), a new path opens a new buffer, a missing
// file opens EMPTY ("New File"). The table lives on the bag: `buffers`
// rows { doc, path, caret, mark, bend } + `bufidx`; doc-scoped facts
// (path, modified, read_only, phandle, undo history) already live on
// each document. The verbs follow the switch through the bag's `adoc`
// (the subject-document rule, verbs/_subject.madv).

// The ACTIVE buffer's document; `fallback` serves table-less sessions
// (the headless harness drives apply_ide_event without run_ide's seed).
long active_doc(long w, long es, long fallback)
{
    long ad = es_int(w, es, "adoc", 0);
    return ad != 0 ? ad : fallback;
}

// Open a document for the buffer table: an unreadable path is a NEW
// EMPTY file (JOE's "New File"), not a refusal — setup_document's
// 0-on-missing contract stays what vised/lined need.
long open_buffer_doc(long w, long es, const char *path)
{
    long nd = setup_document(w, path, false);
    if ( nd != 0 )
	return nd;
    nd = ui::create(w, "document");
    ui::text_load(w, nd, "");
    ui::set(w, nd, "path", path);
    ui::set(w, nd, "modified", false);
    return nd;
}

// Switch the active buffer: leave any view (it lenses the OLD buffer),
// save the old row's interaction state, restore the target's, colour
// immediately from the lexical spans and leave the FULL parse to the
// loop's paint-before-parse step (`pending_parse` — the black-startup
// rule, one mechanism for startup and switches).
void switch_buffer(long w, long es, long olddoc, long idx)
{
    value vw;
    if ( view_name(vw, w, es) )
	exit_view(w, es, olddoc);
    value bufs;
    ui::get(bufs, w, es, "buffers");
    long cur = es_int(w, es, "bufidx", 0);
    value row = bufs[cur];
    row["caret"] = es_int(w, es, "caret", 0);
    row["mark"] = es_int(w, es, "mark", -1);
    row["bend"] = es_int(w, es, "bend", -1);
    bufs[cur] = row;
    ui::set(w, es, "buffers", bufs);
    value t = bufs[idx];
    long nd = t["doc"].as_integer();
    ui::set(w, es, "bufidx", idx);
    ui::set(w, es, "adoc", nd);
    ui::set(w, es, "caret", t["caret"].as_integer());
    ui::set(w, es, "mark", t["mark"].as_integer());
    ui::set(w, es, "bend", t["bend"].as_integer());
    derive_access(w, es);
    lex_first_spans(w, es, nd);
    ui::set(w, es, "pending_parse", 1);
}

// ^K E's commit: an open path switches; anything else opens (missing =
// New File). A session without the table (headless) seeds it from the
// buffer it was driving.
void edit_file(long w, long es, long olddoc, const char *path0)
{
    // Value-first: the caller hands a prompt value's ring-lifetime
    // c_str(); ONE immediate copy into a value carrier owns the text
    // for the whole call (char[] buffers are off the table here — the
    // {}-format/value-assign char-array mistype is a banked engine
    // defect, KG madc_char_array_text_boundary).
    var p = path0;
    value bufs;
    ui::get(bufs, w, es, "buffers");
    if ( bufs.is_null() || php::count(bufs) == 0 )
    {
	var seed = {};
	value b0;
	b0["doc"] = olddoc;
	var op;
	ui::get(op, w, olddoc, "path");
	b0["path"] = op;
	b0["caret"] = es_int(w, es, "caret", 0);
	b0["mark"] = -1;
	b0["bend"] = -1;
	seed.push(b0);
	ui::set(w, es, "buffers", seed);
	ui::set(w, es, "bufidx", 0);
	ui::set(w, es, "adoc", olddoc);
	ui::get(bufs, w, es, "buffers");
    }
    long n = php::count(bufs);
    for ( long i = 0; i < n; i = i + 1 )
    {
	value r = bufs[i];
	if ( r["path"] == p )
	{
	    if ( i == es_int(w, es, "bufidx", 0) )
	    {
		ui::set(w, es, "msg", format("Already editing {}.", p));
		return;
	    }
	    switch_buffer(w, es, olddoc, i);
	    ui::set(w, es, "msg", format("Editing {}.", p));
	    return;
	}
    }
    bool fresh = !php::file_exists(p);
    long nd = open_buffer_doc(w, es, p.c_str());
    value b;
    b["doc"] = nd;
    b["path"] = p;
    b["caret"] = 0;
    b["mark"] = -1;
    b["bend"] = -1;
    bufs.push(b);
    ui::set(w, es, "buffers", bufs);
    switch_buffer(w, es, olddoc, n);
    ui::set(w, es, "msg",
	    fresh ? format("New file {}.", p)
		  : format("Editing {}.", p));
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
	if ( a == "theme" )
	{
	    start_prompt(w, es, "Theme", "theme");
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
	if ( a == "options" )
	{
	    toggle_pane(w, es, "options");
	    return true;
	}
	if ( a == "editfile" )
	{
	    start_prompt(w, es, "Name of file to edit", "editfile");
	    return true;
	}
	if ( a == "shell" )
	{
	    open_shell(w, es);
	    return true;
	}
	if ( a == "profile" )
	{
	    toggle_profile(w, es);
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
	if ( a == "opt-tabwidth" )
	{
	    start_prompt(w, es, "Tab width (1-16)", "tabwidth");
	    return true;
	}
	if ( a == "opt-scheme" )
	{
	    start_prompt(w, es, "Theme", "theme");
	    return true;
	}
	if ( a == "opt-keymap" )
	{
	    toggle_profile(w, es);
	    return true;
	}
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

    // The JOE screen model (IDE-9b): no heading banner — the path, the
    // modified/read-only badges, and the view name all render in the
    // status line's %n / %m / %R / %y seats.
    value ed;
    if ( viewing )
    {
	value vw;
	view_name(vw, w, es);
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

    // The display tab width (IDE-9d ^T option) rides the edit node's
    // hints like the caret — the model owns the expansion math.
    long tabw = es_int(w, es, "tabwidth", 0);
    if ( tabw > 0 )
    {
	value ht;
	ht = ed["hints"];
	ht["tabwidth"] = tabw;
	ed["hints"] = ht;
    }

    // The status line's enclosing-function seat (%x, AST-1): a RETAINED-
    // state read of the buffer's parse handle — stored-space only (a
    // view's caret is display-space), and only once something opened the
    // handle; composition never runs the compiler.
    var fnctx = "";
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
		fnctx = format("fn {}", encl["name"]);
	}
    }
    value status;
    compose_joe_status(status, w, es, nav, caret, fnctx);

    value r;
    r["role"] = "group";
    var kids = {};
    kids.push(status);		// JOE: ONE status line at the TOP
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
	else if ( pane == "options" )
	{
	    // ^T options (IDE-9d): letter row = live value + the action
	    // its ENTER fires — the theme prompt and profile toggle are
	    // the EXISTING actions; only tabwidth's prompt mode is new.
	    box["label"] = "Options";
	    long tw = es_int(w, es, "tabwidth", 8);
	    var scheme;
	    ui::get(scheme, w, es, "scheme");
	    var prof;
	    ui::get(prof, w, es, "profile");
	    var rows = {};
	    value o1;
	    o1["content"] = format("D Tab width   {}", tw);
	    o1["act"] = "opt-tabwidth";
	    rows.push(o1);
	    value o2;
	    o2["content"] = format("S Scheme      {}", scheme);
	    o2["act"] = "opt-scheme";
	    rows.push(o2);
	    value o3;
	    o3["content"] = format("K Keymap      {}", prof);
	    o3["act"] = "opt-keymap";
	    rows.push(o3);
	    for ( value row : rows )
	    {
		value it;
		it["role"] = "item";
		it["content"] = row["content"];
		var ga = {};
		ga.push(row["act"]);
		it["actions"] = ga;
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

    // The transient startup hint on the bottom row (IDE-9b, JOE's -xmsg):
    // plain content, shown until the first applied event returns the row
    // to the document.
    if ( es_int(w, es, "hint", 0) != 0 )
    {
	value hn;
	hn["role"] = "content";
	hn["content"] =
	    " madcide ** ^K Q exits / ^K H shows the loaded profile's help **";
	kids.push(hn);
    }
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
    // The profiles live beside THIS file: derive the directory from
    // __FILE__ (the include-resolved path), so madcide works from any
    // cwd — the old cwd-relative "tools/madcide/profiles" only worked
    // from the repository root.
    const char *self = __FILE__;
    const char *sl = strrchr(self, '/');
    if ( sl != 0 )
	ui::set(w, es, "profile_dir",
		format("{}/profiles", perl::substr(self, 0, sl - self)));
    else
	ui::set(w, es, "profile_dir", "profiles");
    long t = ui::tui_open();
    if ( t == 0 )
	return;
    ui::set(w, es, "tui", t);
    load_profile(w, es, "joe");	// msgs its own failures
    load_status(w, es, "joe");	// the status FORMAT rides the profile
    if ( !load_theme(w, es, "default") )
	ui::set(w, es, "msg", "No theme 'default' — colours off.");
    ui::set(w, es, "hint", 1);	// the transient bottom startup hint
    // The buffer table (AST-5): row 0 = the launch file; ^K E grows it.
    {
	var bufs = {};
	value b0;
	b0["doc"] = doc;
	b0["path"] = path;
	b0["caret"] = 0;
	b0["mark"] = -1;
	b0["bend"] = -1;
	bufs.push(b0);
	ui::set(w, es, "buffers", bufs);
	ui::set(w, es, "bufidx", 0);
    }
    // PAINT BEFORE THE PARSE (owner report, 2026-08-26: parse-on-load
    // blocked the first render for seconds on a real C++ file): colour
    // rides the first paint from the LEXICAL spans; the full parse runs
    // as the loop's pending step AFTER a render — the ONE mechanism
    // startup and buffer switches share.
    lex_first_spans(w, es, doc);
    ui::set(w, es, "pending_parse", 1);
    bool run = true;
    while ( run )
    {
	long adoc = active_doc(w, es, doc);
	value tree;
	compose_ide_tree(tree, w, es, adoc);
	ui::tui_render(t, w, tree);
	if ( es_int(w, es, "pending_parse", 0) != 0 )
	{
	    ui::set(w, es, "pending_parse", 0);
	    long h = ensure_phandle(w, es, adoc);
	    if ( h != 0 )
		refresh_spans(w, es, adoc, h);
	    continue;		// recompose with the parse's spans
	}
	value ev;
	if ( !ui::tui_event(ev, t, w) )
	    break;
	run = apply_ide_event(w, es, adoc, ev);
	ui::set(w, es, "hint", 0);	// first event returns the row
    }
    // Every buffer's parse handle closes with the session.
    value bufs;
    ui::get(bufs, w, es, "buffers");
    for ( value b : bufs )
    {
	long bd = b["doc"].as_integer();
	long ph = es_int(w, bd, "phandle", 0);
	if ( ph != 0 )
	    madc::parse_close(ph);
    }
    ui::tui_close(t);
    ui::world_close(w);
}
