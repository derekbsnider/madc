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
    // The selection MODEL rides the profile's own vocabulary (owner
    // 2026-08-31: personalities are profile data): a set that binds
    // `bend` (JOE ^K K) is a TWO-POINT personality — ^K B alone must
    // not light mark..caret; a set without it keeps the live pico rule.
    bool twopoint = false;
    for ( value a : table )
	if ( a == "bend" )
	    twopoint = true;
    ui::set(w, es, "selblock", twopoint);
    ui::set(w, es, "profile", name);
    ui::set(w, es, "bindtext", txt);
    return true;
}

// The baked-in RESCUE bindings: the ONLY compiled-in chords, used when
// NO profile file loads at startup (a broken install layout must never
// trap the session in a bindingless editor — the first genuine-Win
// validation did exactly that). Still data all the way down: an inline
// profile string through the SAME parse_keys/tui_bind_keys path, a
// deliberately tiny pico-spelled set (exit/save/help class, plus ^p to
// retry a real profile once the layout is fixed). The help pane
// projects from bindtext as usual, so ^G documents the set itself.
const char *rescue_keys_text()
{
    return "# RESCUE bindings — no profile file loaded\n"
	   "^s save\n"
	   "^q quit\n"
	   "^x discard\n"
	   "^g help\n"
	   "^p profile\n";
}

bool bind_rescue_keys(long w, long es)
{
    value table;
    parse_keys(table, rescue_keys_text());
    long t = es_int(w, es, "tui", 0);
    if ( t == 0 || !ui::tui_bind_keys(t, table) )
	return false;
    ui::set(w, es, "selblock", false);	// no bend in the rescue set
    ui::set(w, es, "profile", "rescue");
    ui::set(w, es, "bindtext", rescue_keys_text());
    ui::set(w, es, "msg",
	    "No keybinding profile found — RESCUE keys: ^S save  ^Q quit  ^X discard  ^G help  ^P profile");
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
    // Stage-2: parse_open YIELDS between slices, so two flows can overlap
    // on one doc (the spawned background parse and a synchronous check).
    // ONE handle owner: whoever finds the bag already filled closes its
    // own duplicate and adopts the stored handle.
    long prior = es_int(w, doc, "phandle", 0);
    if ( prior != 0 )
    {
	if ( h != 0 )
	    madc::parse_close(h);
	return prior;
    }
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

// Stage-2: the parse leaves the event loop. The pending step spawns this
// task; the compiler's pumps yield back to the input wait every slice, so
// typing stays live while it runs. ensure_phandle parks the handle and the
// spans on the bag (race-safe, one owner above); the loop's `wake` event
// recomposes with them when the task drains.
void background_parse(long w, long es, long doc)
{
    ensure_phandle(w, es, doc);
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

// ERROR rows in a diagnostics array (severity vocabulary: error /
// warning) — warnings never bar a build or a run.
long diag_error_count(value &diags)
{
    long n = php::count(diags);
    long errs = 0;
    for ( long i = 0; i < n; i = i + 1 )
    {
	value r = diags[i];
	if ( r["severity"] == "error" )
	    errs = errs + 1;
    }
    return errs;
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
    long start = line_start_of(w, doc, line);
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
// TOP with lmsg left / rmsg right and rmsg's fill character between. The
// one prompt mode, the search prompt, and transient messages do NOT
// overlay it — JOE puts them on the BOTTOM line (the msgbuf row); see
// compose_overlay_row. The SCREEN MODELS diverge by design: madcide is
// JOE-shaped, vised keeps the demo's bottom bar.

// The bottom message line (JOE's msgbuf): prompt > search > msg, one
// plain row. This builds CONTENT from the editor state only; WHERE the
// row goes is the screen model's decision (compose_ide_tree appends it
// last) — a different editor personality places the same row elsewhere.
bool compose_overlay_row(value &row, long w, long es)
{
    value st;
    st["role"] = "content";
    var pr;
    ui::get(pr, w, es, "prompt");
    if ( !pr.is_null() && strlen(pr.c_str()) > 0 )
    {
	var pm;
	ui::get(pm, w, es, "pmode");
	if ( pm == "confirm" )
	{
	    st["content"] = pr;	// the label spells its own keys (y,n,^C)
	    row = st;
	    return true;
	}
	var pi;
	ui::get(pi, w, es, "pinput");
	st["content"] = format("{} (^C aborts): {}", pr, pi);
	row = st;
	return true;
    }
    if ( es_flag(w, es, "searching") )
    {
	var sch;
	ui::get(sch, w, es, "search");
	st["content"] = format("Find (^C aborts): {}", sch);
	row = st;
	return true;
    }
    var msg;
    ui::get(msg, w, es, "msg");
    if ( !msg.is_null() && strlen(msg.c_str()) > 0 )
    {
	st["content"] = msg;
	row = st;
	return true;
    }
    return false;
}

// The FORMATTED JOE status line (lmsg/rmsg seat expansion). Every
// window carries its own; prompt/search/msg never touch it — they live
// on the screen-bottom message line (compose_overlay_row above).
void compose_joe_statusline(value &status, long w, long es, long doc,
			    long caret, value &fnctx)
{
    value st;
    st["role"] = "status";
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
    ui::set(w, es, "pverb", "");
}

// A single-key confirm (JOE's "(y,n,^C)?" questions): the label spells
// its own keys and the CONSEQUENCE is data — the verb y fires. Rides the
// one prompt mode, so composition and event routing stay shared; n, esc,
// or any bound action (^C included) cancels.
void start_confirm(long w, long es, const char *label, const char *verb)
{
    start_prompt(w, es, label, "confirm");
    ui::set(w, es, "pverb", verb);
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
    ed_text_insert(w, es, doc, caret, tc);
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
    var pmode;
    ui::get(pmode, w, es, "pmode");
    if ( pmode == "confirm" )
    {
	// Single-key: y fires the stored verb; n or esc cancels; ^C
	// arrives as its BOUND action (discard in the JOE profile), so
	// any action cancels too. Other keys leave the question standing.
	if ( kind == "text" )
	{
	    var t = format("{}", ev["text"]);
	    if ( t == "y" || t == "Y" )
	    {
		var vb;
		ui::get(vb, w, es, "pverb");
		end_prompt(w, es);
		return do_verb(w, es, vb.c_str());
	    }
	    if ( t == "n" || t == "N" )
		end_prompt(w, es);
	    return true;
	}
	if ( kind == "action" )
	{
	    end_prompt(w, es);
	    return true;
	}
	if ( kind == "key" && format("{}", ev["key"]) == "esc" )
	    end_prompt(w, es);
	return true;
    }
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
    if ( bend < 0 && es_flag(w, es, "selblock") )
	e = mark;	// two-point personality: no end yet = no block
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
    ed_text_insert(w, es, doc, caret, blk.c_str());
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
    ed_text_erase(w, es, doc, lo, n);
    long at = caret;
    if ( at >= hi )
	at = at - n;
    ed_text_insert(w, es, doc, at, blk.c_str());
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
    ed_text_erase(w, es, doc, lo, hi - lo);
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
    ed_text_erase(w, es, doc, start, n);
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
    ed_text_erase(w, es, doc, caret, e - caret);
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
    ui::set(w, es, "ro_msg", "Read-only view (^K A returns).");
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

// THE buffer-table row shape, one owner (dupaudit family
// madcide_buffer_row_shape): a FRESH row for a newly opened document.
// The seed row in edit_file is the one deliberate exception — it carries
// the LIVE interaction state of the buffer being adopted, not fresh
// zeroes.
void push_buffer_row(long w, long es, long nd, const char *path)
{
    value bufs;
    ui::get(bufs, w, es, "buffers");
    if ( bufs.is_null() )
    {
	var fresh = {};
	bufs = fresh;
    }
    value b;
    b["doc"] = nd;
    b["path"] = path;
    b["caret"] = 0;
    b["mark"] = -1;
    b["bend"] = -1;
    bufs.push(b);
    ui::set(w, es, "buffers", bufs);
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
    push_buffer_row(w, es, nd, p.c_str());
    switch_buffer(w, es, olddoc, n);
    ui::set(w, es, "msg",
	    fresh ? format("New file {}.", p)
		  : format("Editing {}.", p));
}

// ---- IDE-9e: windows (^K O split) --------------------------------------
// JOE's second axis onto the AST-5 ring: N STACKED windows, each with its
// own status line and caret. Bag state: `windows` rows
// { bufidx, caret, grow } + `winat`. Fewer than two rows = the
// single-window compose, byte-identical (the belt: no split, no delta).
// The ACTIVE window's live state IS the es state (bufidx/adoc/caret); its
// row holds a snapshot taken at switch time — the buffer-row shape one
// level up. While split, the WINDOW rows own the caret (two windows on
// one buffer keep separate carets); buffer rows keep serving buffer
// switches. mark/bend stay buffer-scoped (selection is active-window
// presentation). grow is a DELTA from the even split with sum-zero
// invariant (groww takes a row from a neighbor, JOE's feel); the active
// window composes UNHINTED (flexible) so panes/prompt keep taking their
// rows from the bottom exactly as today.

long window_count(long w, long es)
{
    value ws;
    ui::get(ws, w, es, "windows");
    return ws.is_null() ? 0 : php::count(ws);
}

// Each window needs its status row + this many content rows.
long window_min_rows()
{
    return 3;
}

// Save the ACTIVE window's live state into its row.
void window_save_active(long w, long es)
{
    value ws;
    ui::get(ws, w, es, "windows");
    if ( ws.is_null() || php::count(ws) < 2 )
	return;
    long at = es_int(w, es, "winat", 0);
    value r = ws[at];
    r["bufidx"] = es_int(w, es, "bufidx", 0);
    r["caret"] = es_int(w, es, "caret", 0);
    ws[at] = r;
    ui::set(w, es, "windows", ws);
}

// Make window idx active: buffer switch when it shows another buffer
// (the AST-5 machinery — spans/parse/access ride it), then the WINDOW's
// caret wins, clamped to the document (edits in the other window may
// have shortened it).
void window_activate(long w, long es, long olddoc, long idx)
{
    window_save_active(w, es);
    value ws;
    ui::get(ws, w, es, "windows");
    value t = ws[idx];
    long tb = t["bufidx"].as_integer();
    if ( tb != es_int(w, es, "bufidx", 0) )
	switch_buffer(w, es, olddoc, tb);
    long nd = active_doc(w, es, olddoc);
    ui::set(w, es, "caret",
	    clamp_caret_to(w, nd, t["caret"].as_integer()));
    ui::set(w, es, "winat", idx);
}

// ^K O: split — the new window duplicates the ACTIVE one (JOE: same
// buffer, same caret), inserted BELOW it; focus stays where it is.
void split_window(long w, long es, long doc)
{
    long t = es_int(w, es, "tui", 0);
    long trows = t != 0 ? ui::tui_rows(t) : 24;
    long n = window_count(w, es);
    long want = n < 2 ? 2 : n + 1;
    if ( want * (window_min_rows() + 1) > trows )
    {
	ui::set(w, es, "msg", "No room for another window.");
	return;
    }
    value ws;
    ui::get(ws, w, es, "windows");
    long at = 0;
    if ( ws.is_null() || php::count(ws) < 2 )
    {
	// Seed: window 0 adopts the live state.
	var seed = {};
	value w0;
	w0["bufidx"] = es_int(w, es, "bufidx", 0);
	w0["caret"] = es_int(w, es, "caret", 0);
	w0["grow"] = 0;
	seed.push(w0);
	ws = seed;
	ui::set(w, es, "winat", 0);
    }
    else
	at = es_int(w, es, "winat", 0);
    value dup;
    dup["bufidx"] = es_int(w, es, "bufidx", 0);
    dup["caret"] = es_int(w, es, "caret", 0);
    dup["grow"] = 0;
    var rebuilt = {};
    long i = 0;
    for ( value r : ws )
    {
	rebuilt.push(r);
	if ( i == at )
	    rebuilt.push(dup);
	i = i + 1;
    }
    ui::set(w, es, "windows", rebuilt);
    ui::set(w, es, "msg", format("{} windows.", php::count(rebuilt)));
}

// ^K N / ^K P: next / previous window (wrap).
void cycle_window(long w, long es, long doc, long dir)
{
    long n = window_count(w, es);
    if ( n < 2 )
    {
	ui::set(w, es, "msg", "Only one window.");
	return;
    }
    long at = es_int(w, es, "winat", 0);
    long nx = (at + dir + n) % n;
    window_activate(w, es, doc, nx);
}

// Esc 0 / Esc 1 (JOE tw0/tw1): kill this window / keep only this one.
// Down to one window the table clears — caret ownership returns to the
// es/buffer machinery, the compose returns to the single shape.
void kill_window(long w, long es, long doc, bool others)
{
    long n = window_count(w, es);
    if ( n < 2 )
    {
	ui::set(w, es, "msg", "Only one window.");
	return;
    }
    long at = es_int(w, es, "winat", 0);
    if ( others )
    {
	var none = {};
	ui::set(w, es, "windows", none);
	ui::set(w, es, "winat", 0);
	ui::set(w, es, "msg", "One window.");
	return;
    }
    // Kill the active one: the next window (wrapping) takes over FIRST
    // (its buffer/caret restore), then its row leaves the table.
    long nx = (at + 1) % n;
    window_activate(w, es, doc, nx);
    value ws;
    ui::get(ws, w, es, "windows");
    var rebuilt = {};
    long i = 0;
    long newat = 0;
    for ( value r : ws )
    {
	if ( i != at )
	{
	    if ( i == nx )
		newat = php::count(rebuilt);
	    rebuilt.push(r);
	}
	i = i + 1;
    }
    if ( php::count(rebuilt) < 2 )
    {
	var none = {};
	ui::set(w, es, "windows", none);
	ui::set(w, es, "winat", 0);
    }
    else
    {
	ui::set(w, es, "windows", rebuilt);
	ui::set(w, es, "winat", newat);
    }
    ui::set(w, es, "msg", format("{} window(s).",
				 php::count(rebuilt) < 2
				     ? 1 : php::count(rebuilt)));
}

// ^K G / ^K T: grow / shrink the active window by one row, taken from /
// returned to the NEXT window (wrap) — sum-zero deltas from the even
// split; the victim never drops under the minimum.
void grow_window(long w, long es, long dir)
{
    long n = window_count(w, es);
    if ( n < 2 )
    {
	ui::set(w, es, "msg", "Only one window.");
	return;
    }
    long t = es_int(w, es, "tui", 0);
    long trows = t != 0 ? ui::tui_rows(t) : 24;
    long base = (trows - n) / n;	// even content share
    long at = es_int(w, es, "winat", 0);
    long nx = (at + 1) % n;
    value ws;
    ui::get(ws, w, es, "windows");
    value ra = ws[at];
    value rn = ws[nx];
    long ga = ra["grow"].as_integer() + dir;
    long gn = rn["grow"].as_integer() - dir;
    if ( base + ga < window_min_rows() || base + gn < window_min_rows() )
    {
	ui::set(w, es, "msg", "Window too small.");
	return;
    }
    ra["grow"] = ga;
    rn["grow"] = gn;
    ws[at] = ra;
    ws[nx] = rn;
    ui::set(w, es, "windows", ws);
}

// ---- IDE-10a: the ^P file palette --------------------------------------
// The ONE popup-list mechanism (owner ruling 2026-08-27; plan doc
// 2026-08-27-madcide-ide-controls.md): a `choice` pane in the model's
// LIST presentation (hint list:1 — one row per option) that is
// AUTOFOCUSED (hint focus:1 — arrows/enter navigate it immediately;
// selection stays the model's presentation state, exactly like the menu
// bar). Text/backspace edit the FILTER here; enter CHOOSES through the
// model's choose event; esc — or ^P again — closes. Rows = open buffers
// first (tag "buf"), then cwd source files not already open; choosing
// routes through edit_file — THE one open-or-switch owner. The ^B build
// palette (IDE-10b) reuses this widget with action rows.

// Recompute the filtered rows onto the bag ("palrows": {path, tag} rows).
// Case-insensitive substring filter; ring-lifetime strtolower returns are
// copied into var carriers immediately (the standing ring discipline).
void palette_rows(long w, long es)
{
    var filt;
    ui::get(filt, w, es, "palfilter");
    var lf = php::strtolower(filt.c_str());
    long fn = strlen(lf.c_str());
    var rows = {};
    value seen;			// path -> 1: open buffers dedupe the glob
    value bufs;
    ui::get(bufs, w, es, "buffers");
    for ( value b : bufs )
    {
	var p = format("{}", b["path"]);
	seen[p.c_str()] = 1;
	var lp = php::strtolower(p.c_str());
	if ( fn > 0 && perl::index(lp.c_str(), lf.c_str()) < 0 )
	    continue;
	value r;
	r["path"] = p;
	r["tag"] = "buf";
	rows.push(r);
    }
    // Source-extension globs, one pattern each (glob(3) has no brace
    // expansion here) — directories excluded by construction. The
    // recursive/project-manifest walk is the named later slice.
    var pats = {"*.mad", "*.c", "*.cc", "*.cpp", "*.cxx", "*.h", "*.hpp",
		"*.inc", "*.json", "*.md", "*.txt"};
    for ( value pat : pats )
    {
	array g;
	perl::glob(g, pat.c_str());
	for ( value f : g )
	{
	    var p = format("{}", f);
	    if ( !seen[p.c_str()].is_null() )
		continue;
	    var lp = php::strtolower(p.c_str());
	    if ( fn > 0 && perl::index(lp.c_str(), lf.c_str()) < 0 )
		continue;
	    value r;
	    r["path"] = p;
	    r["tag"] = "cwd";
	    rows.push(r);
	}
    }
    ui::set(w, es, "palrows", rows);
}

void start_palette(long w, long es)
{
    ui::set(w, es, "pane", "palette");
    ui::set(w, es, "palfilter", "");
    palette_rows(w, es);
}

void end_palette(long w, long es)
{
    ui::set(w, es, "pane", "");
    ui::set(w, es, "palfilter", "");
}

bool palette_active(long w, long es)
{
    var pane;
    ui::get(pane, w, es, "pane");
    if ( pane.is_null() )
	return false;
    return pane == "palette";
}

// Every event while the palette is up (modal, like the prompt): text and
// backspace edit the filter; the MODEL owns arrows/enter (its choose
// arrives here as pal-open, option 1-based); esc or ^P closes. Other
// bound actions are swallowed — a modal palette half-running ^K chords
// would be worse than inert keys.
bool palette_event(long w, long es, long doc, value &ev)
{
    var kind = format("{}", ev["event"]);
    if ( kind == "text" )
    {
	var in;
	ui::get(in, w, es, "palfilter");
	var nx = format("{}{}", in, ev["text"]);
	ui::set(w, es, "palfilter", nx.c_str());
	palette_rows(w, es);
	return true;
    }
    if ( kind == "choose" )
    {
	value rows;
	ui::get(rows, w, es, "palrows");
	long opt = ev["option"].as_integer() - 1;	// 1-based on the wire
	if ( opt >= 0 && opt < php::count(rows) )
	{
	    value r = rows[opt];
	    var p = format("{}", r["path"]);
	    end_palette(w, es);
	    edit_file(w, es, doc, p.c_str());
	}
	return true;
    }
    if ( kind == "action" )
    {
	var a = format("{}", ev["action"]);
	if ( a == "palette" || a == "quit" )
	    end_palette(w, es);
	return true;
    }
    if ( kind != "key" )
	return true;
    var k = format("{}", ev["key"]);
    if ( k == "esc" )
	end_palette(w, es);
    else if ( k == "backspace" )
    {
	var in;
	ui::get(in, w, es, "palfilter");
	long n = strlen(in.c_str());
	if ( n > 0 )
	{
	    ui::set(w, es, "palfilter", perl::substr(in, 0, n - 1));
	    palette_rows(w, es);
	}
    }
    return true;
}

// ---- IDE-10b: the ^B build palette --------------------------------------
// The SAME popup-list widget as ^P with ACTION rows AS DATA (bag
// "buildcmds": {name, act, cmd, mode} rows — built-in defaults seeded on
// first open; a project manifest's commands section replaces them in a
// later slice). Two modes:
//   capture  — spawn the command through an exec:// channel and PUMP its
//              output into the [build] buffer as a cooperative task: the
//              MT-4b select spans {stop channel, byte readiness}, so the
//              editor stays live while the build streams and Stop fires
//              even against a silent child (cancel() SIGTERMs it).
//   terminal — suspend the tui and hand the command the REAL terminal
//              (an interactive Run needs its own stdin/stdout; the JOE
//              ^K Z shape), with a return-key pause before resume.
// Check stays in-process (run_check — the retained-parse diagnostics).

// {path} / {base} / {madc} substitution: path = the active buffer's
// file, base = path minus its extension, madc = THE RUNNING COMPILER'S
// own executable (madc::compiler_path). {madc} serves MANIFEST-DECLARED
// commands only — no default row spawns any madc binary (owner ruling
// 2026-08-27: the running madc IS the compiler; Run forks the live
// parse, madc::parse_run). Commands are DATA — no quoting layer (the
// exec:// factory splits on single spaces; see madc_process.cpp).
void build_subst(var &out, const char *cmd, long w, long doc)
{
    var p;
    ui::get(p, w, doc, "path");
    var base = p;
    const char *pc = p.c_str();
    const char *dot = strrchr(pc, '.');
    if ( dot != 0 )
	base = perl::substr(p, 0, dot - pc);
    var s1 = php::str_replace("{path}", p.c_str(), cmd);
    var s2 = php::str_replace("{base}", base.c_str(), s1.c_str());
    var self = madc::compiler_path();
    out = php::str_replace("{madc}", self.c_str(), s2.c_str());
}

// The [build] buffer: found-or-created in the buffer table WITHOUT
// switching to it (the pump appends in the background; ^P reaches it).
long build_doc(long w, long es)
{
    value bufs;
    ui::get(bufs, w, es, "buffers");
    long n = php::count(bufs);
    for ( long i = 0; i < n; i = i + 1 )
    {
	value r = bufs[i];
	if ( r["path"] == "[build]" )
	    return r["doc"].as_integer();
    }
    long nd = open_buffer_doc(w, es, "[build]");
    push_buffer_row(w, es, nd, "[build]");
    return nd;
}

// Append one line; the [build] buffer never reads as user-modified.
void append_build_line(long w, long es, long bdoc, const char *text)
{
    var line = format("{}\n", text);
    // Raw ui::text_insert, NOT ed_text_insert: bdoc is the [build] buffer,
    // not the es's viewed document — shifting the es's highlight spans by
    // another doc's bytes would corrupt them. (check-madcide-single-owners
    // allowlists exactly this line.)
    ui::text_insert(w, bdoc, ui::text_size(w, bdoc), line.c_str());
    ui::set(w, bdoc, "modified", false);
}

// The capture pump — a cooperative task (stage-2's background_parse
// pattern: long args only, the command rides the bag). The MT-4b mixed
// select is the loop: case 0 = the stop channel (a SENT value stops;
// cancel() SIGTERMs the child so close() reaps promptly), case 1 = the
// child's readable byte endpoint (readline after a fire; EOF ends the
// build). Known window (named residue for MT-3 cancellation): a child
// that emits a PARTIAL line then hangs parks the pump mid-readline where
// only fd activity wakes it — real build tools emit whole lines.
void build_pump(long w, long es, long bdoc, long stopc)
{
    var cmd;
    ui::get(cmd, w, es, "buildcmd");
    var uri = format("exec://{}", cmd);
    madc::channel bc(uri.c_str());
    if ( !bc.ok() )
    {
	var em = format("[build] cannot start: {}", bc.last_error());
	append_build_line(w, es, bdoc, em.c_str());
	ui::set(w, es, "buildlive", 0);
	return;
    }
    bc.close_write();		// builds read no stdin
    value cases;
    cases[0] = stopc;
    cases[1] = madc::chan_readable(bc);
    value got;
    value line;
    bool stopped = false;
    for ( ;; )
    {
	long idx = madc::chan_select(got, cases);
	if ( idx == 0 )
	{
	    stopped = true;
	    bc.cancel();	// SIGTERM: close() below reaps promptly
	    break;
	}
	if ( idx != 1 )
	    break;		// every case dead
	if ( !bc.readline(line) )
	    break;		// drained EOF: the build finished
	append_build_line(w, es, bdoc, line.c_str());
    }
    bc.close();
    append_build_line(w, es, bdoc, stopped ? "[stopped]" : "[done]");
    ui::set(w, es, "buildlive", 0);
}

void start_build(long w, long es, const char *cmd)
{
    if ( es_int(w, es, "buildlive", 0) != 0 )
    {
	ui::set(w, es, "msg", "A build is already running (Stop first).");
	return;
    }
    long bdoc = build_doc(w, es);
    var hdr = format("=== {} ===", cmd);
    append_build_line(w, es, bdoc, hdr.c_str());
    long stopc = madc::chan_make(1);
    ui::set(w, es, "buildstop", stopc);
    ui::set(w, es, "buildcmd", cmd);
    ui::set(w, es, "buildlive", 1);
    go build_pump(w, es, bdoc, stopc);
    ui::set(w, es, "msg", format("Building — ^P [build] to watch."));
}

void stop_build(long w, long es)
{
    if ( es_int(w, es, "buildlive", 0) == 0 )
    {
	ui::set(w, es, "msg", "No build running.");
	return;
    }
    long stopc = es_int(w, es, "buildstop", 0);
    if ( stopc != 0 )
    {
	// An exec-pump build: the stop is a SENT value (IDE-10b).
	value v;
	v = 1;
	madc::chan_send(stopc, v);
	ui::set(w, es, "msg", "Stopping the build.");
	return;
    }
    // An in-process build (MT-3b): its task's scope is the job handle —
    // scope_cancel reaches the child parse through the cancellation
    // chain. The pre-start flag covers the spawn-to-first-run window
    // (spawn never switches: Stop can land before the task opened its
    // scope); the emit phase has no yield points yet (named residue) —
    // a cancel during it lands when the parse would have.
    ui::set(w, es, "buildstopreq", 1);
    long bscope = es_int(w, es, "buildscope", 0);
    if ( bscope != 0 )
	madc::scope_cancel(bscope);
    ui::set(w, es, "msg", "Stopping the build.");
}

// The INTERNAL build task (owner ruling 2026-08-27: the running madc IS
// the compiler — the buffer's LIVE parse handle is the compilation).
// Refresh the handle from the screen text (cooperative — the editor
// stays live; unsaved edits compile), then madc::parse_build emits from
// THAT tree: no re-parse of the path, no child Program, no madc binary.
// The emit phase runs without yield points (a brief block — the named
// backend-yield residue). Diagnostics land in the diags pane
// (click-to-error), never as [build] text. Long args only (the go-spawn
// contract); the doc/path/kind/outpath ride the bag like buildcmd.
void native_build_task(long w, long es)
{
    long doc = es_int(w, es, "nbdoc", 0);
    var p, k, o;
    ui::get(p, w, es, "nbpath");
    ui::get(k, w, es, "nbkind");
    ui::get(o, w, es, "nbout");
    // The task's own scope is the STOP surface (MT-3b): Stop calls
    // scope_cancel on it, and the cancellation chain reaches the
    // buffer's re-parse (tokenize/parse abort cleanly at their next
    // yield point). buildscope is published only while the handle
    // is live — cleared BEFORE scope_end consumes it (no yield between:
    // a Stop can never see a stale handle).
    long bs = madc::scope_begin();
    ui::set(w, es, "buildscope", bs);
    bool pre = es_int(w, es, "buildstopreq", 0) != 0;
    value diags;
    bool ok = false;
    if ( !pre )
    {
	long h = reparse_buffer(w, es, doc);
	if ( h != 0 && !madc::cancelled() )
	    ok = madc::parse_build(diags, h, k.c_str(), o.c_str());
    }
    bool stopped = pre || madc::cancelled();
    ui::set(w, es, "buildscope", 0);
    try {
	madc::scope_end(bs);	// no members; throws the cancelled text
    } catch (const char *e) {	// when Stop cancelled the scope
    }
    ui::set(w, es, "buildlive", 0);
    if ( stopped )
    {
	// A deliberate stop: the cancellation diagnostic is an artifact,
	// not a finding — nothing lands in the diags pane.
	ui::set(w, es, "msg", format("Build stopped ({}).", p));
	return;
    }
    ui::set(w, es, "diags", diags);
    if ( ok )
    {
	ui::set(w, es, "msg", format("Built {} -> {}", p, o));
	return;
    }
    // Failure: surface the rows. Take the lower pane only when the user
    // has nothing else up (never stomp an open palette/help/options).
    var pane;
    ui::get(pane, w, es, "pane");
    if ( strlen(pane.c_str()) == 0 )
	ui::set(w, es, "pane", "diags");
    ui::set(w, es, "msg",
	    format("Build failed: {} problem(s).", php::count(diags)));
}

// var& params, not held const char* — a c_str() kept across build_subst's
// ring calls rots (the ring-lifetime contract).
void start_native_build(long w, long es, long doc, var &kind, var &outtmpl)
{
    if ( es_int(w, es, "buildlive", 0) != 0 )
    {
	ui::set(w, es, "msg", "A build is already running.");
	return;
    }
    var p;
    ui::get(p, w, doc, "path");
    var o;
    build_subst(o, outtmpl.c_str(), w, doc);
    ui::set(w, es, "nbdoc", doc);
    ui::set(w, es, "nbpath", p);
    ui::set(w, es, "nbkind", kind);
    ui::set(w, es, "nbout", o);
    ui::set(w, es, "buildstop", 0);	// in-process: Stop rides the scope
    ui::set(w, es, "buildscope", 0);	// published by the task when live
    ui::set(w, es, "buildstopreq", 0);
    ui::set(w, es, "buildlive", 1);
    go native_build_task(w, es);
    var md;
    ui::get(md, w, doc, "modified");
    if ( md.as_boolean() )
	ui::set(w, es, "msg",
		format("Building {} -> {} (live buffer: unsaved edits compile).", p, o));
    else
	ui::set(w, es, "msg", format("Building {} -> {} (live parse).", p, o));
}

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

// The Run row (owner ruling 2026-08-27: the running madc IS the
// compiler): fork-run the buffer's LIVE parse — refresh the handle from
// the screen text, refuse a red parse LOUD (the rows land in the diags
// pane), then hand the guest the REAL terminal (suspend, the JOE ^K Z
// shape) around madc::parse_run. The child inherits the parsed tree;
// nothing execs, nothing re-parses. A return-key pause holds the
// guest's output on screen before the full-repaint resume.
void run_buffer(long w, long es, long doc)
{
    // The parse verdict FIRST (it needs no terminal and is the more
    // informative refusal), the terminal check second, the fork last.
    refresh_diags(w, es, doc);
    value diags;
    ui::get(diags, w, es, "diags");
    long errs = diag_error_count(diags);
    if ( errs > 0 )
    {
	ui::set(w, es, "pane", "diags");
	ui::set(w, es, "msg",
		format("Run refused: {} error(s) — fix them first.", errs));
	return;
    }
    long h = es_int(w, doc, "phandle", 0);
    if ( h == 0 )
	return;			// ensure_phandle already said why
    long t = es_int(w, es, "tui", 0);
    if ( t == 0 )
    {
	ui::set(w, es, "msg", "No terminal to suspend.");
	return;
    }
    if ( !ui::tui_suspend(t) )
	return;
    long rc = madc::parse_run(h);
    terminal_return_pause();
    ui::tui_resume(t);
    if ( rc < 0 )
	ui::set(w, es, "msg", format("Run failed (rc {}).", rc));
    else
	ui::set(w, es, "msg", format("Run finished (exit {}).", rc));
}

// terminal-mode rows (Run native): the JOE ^K Z shape — the program
// gets the REAL terminal; a return-key pause holds its output on screen
// before the full-repaint resume.
void run_in_terminal(long w, long es, const char *cmd)
{
    long t = es_int(w, es, "tui", 0);
    if ( t == 0 )
    {
	ui::set(w, es, "msg", "No terminal to suspend.");
	return;
    }
    if ( !ui::tui_suspend(t) )
	return;
    system(cmd);
    terminal_return_pause();
    ui::tui_resume(t);
}

// Seed the default command rows once (data; a later slice reads a project
// manifest's commands section instead).
void ensure_build_cmds(long w, long es)
{
    value have;
    ui::get(have, w, es, "buildcmds");
    if ( !have.is_null() && php::count(have) > 0 )
	return;
    // OWNER RULING (2026-08-27, the running madc IS the compiler): every
    // default row rides the buffer's LIVE parse handle — Build emits from
    // its tree (madc::parse_build), Run forks it (madc::parse_run); no
    // row execs any madc binary. Run native runs the BUILT artifact (the
    // user's own program). The exec:// capture pump stays for manifest-
    // declared external commands (mode "capture"); no default rides it.
    // Stop is back (MT-3b): an in-process build cancels through its
    // task's scope; terminal runs stay ^C-able in their own terminal.
    var rows = {};
    value r1;
    r1["name"] = "Check (in-process)";
    r1["act"] = "bld-check";
    rows.push(r1);
    value r2;
    r2["name"] = "Build (in-process)";
    r2["kind"] = "exe";
    r2["out"] = "{base}";
    r2["act"] = "bld-native";
    rows.push(r2);
    value r3;
    r3["name"] = "Build object (in-process)";
    r3["kind"] = "obj";
    r3["out"] = "{base}.o";
    r3["act"] = "bld-native";
    rows.push(r3);
    value r4;
    r4["name"] = "Run";
    r4["act"] = "bld-run";	// fork the LIVE parse (madc::parse_run)
    rows.push(r4);
    value r5;
    r5["name"] = "Run native";
    r5["cmd"] = "./{base}";
    r5["mode"] = "terminal";
    r5["act"] = "bld-run-cmd";
    rows.push(r5);
    value r6;
    r6["name"] = "Stop";
    r6["act"] = "bld-stop";
    rows.push(r6);
    ui::set(w, es, "buildcmds", rows);
}

// One choose dispatcher for the build pane: the row (by 1-based option)
// carries everything — act, command template, mode. Data all the way.
void build_choose(long w, long es, long doc, long option)
{
    value rows;
    ui::get(rows, w, es, "buildcmds");
    long idx = option - 1;
    if ( idx < 0 || idx >= php::count(rows) )
	return;
    value r = rows[idx];
    var a = format("{}", r["act"]);
    ui::set(w, es, "pane", "");		// the palette closes on choose
    if ( a == "bld-check" )
    {
	run_check(w, es, doc);
	return;
    }
    if ( a == "bld-stop" )
    {
	stop_build(w, es);
	return;
    }
    if ( a == "bld-native" )
    {
	var k = format("{}", r["kind"]);
	var ot = format("{}", r["out"]);
	start_native_build(w, es, doc, k, ot);
	return;
    }
    if ( a == "bld-run" )
    {
	run_buffer(w, es, doc);
	return;
    }
    var cmd0 = format("{}", r["cmd"]);
    var cmd;
    build_subst(cmd, cmd0.c_str(), w, doc);
    var mode = format("{}", r["mode"]);
    if ( mode == "terminal" )
	run_in_terminal(w, es, cmd.c_str());
    else
	start_build(w, es, cmd.c_str());
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

// ^K A (IDE-9e moved it off ^K N, now nextw): original -> mc11 -> c11
// [-> c++ on a C/C++ buffer] -> original.
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
    // Stage-2: cooperative background tasks drained — the spawned parse
    // already parked its handle/spans on the bag; recompose only. Before
    // anything else: a wake is not a user gesture (it must not clear the
    // message seat or disturb a pending prompt).
    if ( kind == "wake" )
	return true;
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

    if ( palette_active(w, es) )
	return palette_event(w, es, doc, ev);

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
	{
	    // JOE ^K Q: a dirty buffer asks JOE's question on the bottom
	    // line; the lineed q verb's refusal spelling stays lined's.
	    var qm;
	    ui::get(qm, w, doc, "modified");
	    if ( !qm.is_null() && qm.as_boolean() )
	    {
		start_confirm(w, es, "Lose changes to this file (y,n,^C)?",
			      "q!");
		return true;
	    }
	    return do_verb(w, es, "q");
	}
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
	if ( a == "palette" )
	{
	    start_palette(w, es);
	    return true;
	}
	if ( a == "refresh" )
	{
	    // JOE's ^R retype: the terminal is lying — drop the delta
	    // basis and hard-repaint (ui::tui_refresh). Headless sessions
	    // have no tui on the bag; the action is safely inert there.
	    long tt = es_int(w, es, "tui", 0);
	    if ( tt != 0 )
		ui::tui_refresh(tt);
	    return true;
	}
	if ( a == "build" )
	{
	    ensure_build_cmds(w, es);
	    toggle_pane(w, es, "build");
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
	// IDE-9e windows — JOE's verbs on their JOE seats.
	if ( a == "splitw" )
	{
	    split_window(w, es, doc);
	    return true;
	}
	if ( a == "nextw" )
	{
	    cycle_window(w, es, doc, 1);
	    return true;
	}
	if ( a == "prevw" )
	{
	    cycle_window(w, es, doc, -1);
	    return true;
	}
	if ( a == "killwin" )
	{
	    kill_window(w, es, doc, false);
	    return true;
	}
	if ( a == "onlywin" )
	{
	    kill_window(w, es, doc, true);
	    return true;
	}
	if ( a == "groww" )
	{
	    grow_window(w, es, 1);
	    return true;
	}
	if ( a == "shrinkw" )
	{
	    grow_window(w, es, -1);
	    return true;
	}
	// Motion (and the direct edit keys) DELEGATE to the shared
	// edit_key vocabulary: the action name IS the key spelling
	// ("left", "pgdn", "del", ...) — a profile binds e.g. "^a home",
	// the dispatcher synthesizes the key event. One movement
	// implementation. (IDE-10a reclaimed ^B/^F/^P/^N from the
	// WordStar diamond — real arrow keys serve motion.)
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
	// Build-pane rows dispatch by ROW (act + command + mode are the
	// row's data); the act PREFIX routes — a new bld- verb needs no
	// edit here — and build_choose owns the rest.
	if ( a.index("bld-") == 0 )
	{
	    build_choose(w, es, doc, ev["option"].as_integer());
	    return true;
	}
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

    // OWNER RULING (IDE-10c): Esc backs out of ANY pane — help, options,
    // outline, diags, build alike. The palette and prompts esc-cancel in
    // their own arms above (they route before this one).
    var k = format("{}", ev["key"]);
    if ( k == "esc" )
    {
	var pane;
	ui::get(pane, w, es, "pane");
	if ( strlen(pane.c_str()) > 0 )
	{
	    ui::set(w, es, "pane", "");
	    return true;
	}
    }
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
    compose_joe_statusline(status, w, es, nav, caret, fnctx);

    value r;
    r["role"] = "group";
    var kids = {};
    // IDE-9e: with two-plus windows, each window is its own status line +
    // edit node, stacked. The ACTIVE window is the live one — the full
    // edit node composed above (views, spans, tabwidth) plus the focus
    // hint so the cursor paints in it (prompt/search/msg belong to the
    // SCREEN's bottom message line, not any window's status); it stays
    // UNHINTED (flexible) so panes/prompt keep taking their rows from the
    // bottom exactly as in the single shape. INACTIVE windows compose
    // snapshots: their buffer's plain text, their own caret (clamped —
    // edits in the active window may have shortened the doc), a fixed
    // rows hint from the even split ± grow. No selection, no spans, no
    // views on inactive windows (named residues).
    long wc = window_count(w, es);
    if ( wc >= 2 )
    {
	long t9 = es_int(w, es, "tui", 0);
	long trows = t9 != 0 ? ui::tui_rows(t9) : 24;
	long base = (trows - wc) / wc;
	if ( base < window_min_rows() )
	    base = window_min_rows();
	long at = es_int(w, es, "winat", 0);
	value ws;
	ui::get(ws, w, es, "windows");
	value bufs;
	ui::get(bufs, w, es, "buffers");
	value hf;
	hf = ed["hints"];
	hf["focus"] = 1;
	ed["hints"] = hf;
	long i = 0;
	for ( value wr : ws )
	{
	    if ( i == at )
	    {
		kids.push(status);
		kids.push(ed);
	    }
	    else
	    {
		value brow = bufs[wr["bufidx"].as_integer()];
		long wdoc = brow["doc"].as_integer();
		long wcaret = clamp_caret_to(w, wdoc,
					     wr["caret"].as_integer());
		var wfn = "";
		value wst;
		compose_joe_statusline(wst, w, es, wdoc, wcaret, wfn);
		kids.push(wst);
		value wed;
		wed["role"] = "edit";
		value wt;
		ui::text(wt, w, wdoc);
		wed["content"] = wt;
		value wh;
		wh["caret"] = wcaret;
		long wrows = base + wr["grow"].as_integer();
		if ( wrows < window_min_rows() )
		    wrows = window_min_rows();
		wh["rows"] = wrows;
		wed["hints"] = wh;
		kids.push(wed);
	    }
	    i = i + 1;
	}
    }
    else
    {
	kids.push(status);	// JOE: ONE status line at the TOP
	kids.push(ed);
    }

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
	else if ( pane == "build" )
	{
	    // IDE-10b ^B: the same popup-list widget with the command
	    // rows as data; a running build shows in the label.
	    value brows;
	    ui::get(brows, w, es, "buildcmds");
	    box["label"] = es_int(w, es, "buildlive", 0) != 0
		? "Build — running" : "Build";
	    value bh;
	    bh["list"] = 1;
	    bh["focus"] = 1;
	    box["hints"] = bh;
	    for ( value row : brows )
	    {
		value it;
		it["role"] = "item";
		value c = row["cmd"];
		if ( c.is_null() )
		    it["content"] = format("{}", row["name"]);
		else
		    it["content"] = format("{}  [{}]", row["name"], c);
		var ga = {};
		ga.push(row["act"]);
		it["actions"] = ga;
		items.push(it);
		nrows = nrows + 1;
	    }
	}
	else if ( pane == "palette" )
	{
	    // IDE-10a ^P: the LIST-presentation choice — model hints
	    // list (one row per option) + focus (arrows/enter land here
	    // without a tab). The label carries the live filter; rows
	    // come from palette_rows; enter fires pal-open.
	    var filt;
	    ui::get(filt, w, es, "palfilter");
	    value prows;
	    ui::get(prows, w, es, "palrows");
	    box["label"] = format("File: {}_", filt);
	    value ph;
	    ph["list"] = 1;
	    ph["focus"] = 1;
	    box["hints"] = ph;
	    for ( value row : prows )
	    {
		value it;
		it["role"] = "item";
		var tag = format("{}", row["tag"]);
		it["content"] = format("{} {}", tag == "buf" ? "*" : " ",
				       row["path"]);
		var ga = {};
		ga.push("pal-open");
		it["actions"] = ga;
		items.push(it);
		nrows = nrows + 1;
	    }
	    if ( nrows == 0 )
	    {
		value it;
		it["role"] = "item";
		it["content"] = "(no matches)";
		items.push(it);
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

    // The bottom message line (JOE's msgbuf): prompts, the search
    // prompt, and transient messages — appended LAST so the flexible
    // edit node yields the screen's bottom row. The startup hint
    // (IDE-9b, JOE's -xmsg) shares this line and yields to a live
    // overlay: JOE has ONE message line, never two.
    value ovl;
    if ( compose_overlay_row(ovl, w, es) )
	kids.push(ovl);
    else if ( es_int(w, es, "hint", 0) != 0 )
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
    // Take whichever separator is LAST: a Windows launch spells the source
    // path with '\' (and the include chain may mix the two). Splitting on
    // '/' alone cost the first genuine-Win validation its keybindings —
    // every chord is profile data, so the session had arrows and no exit.
    const char *sl = strrchr(self, '/');
    const char *bs = strrchr(self, '\\');
    if ( !sl || (bs && bs > sl) )
	sl = bs;
    if ( sl != 0 )
	ui::set(w, es, "profile_dir",
		format("{}/profiles", perl::substr(self, 0, sl - self)));
    else
	ui::set(w, es, "profile_dir", "profiles");
    long t = ui::tui_open();
    if ( t == 0 )
	return;
    ui::set(w, es, "tui", t);
    load_status(w, es, "joe");	// the status FORMAT rides the profile
    if ( !load_theme(w, es, "default") )
	ui::set(w, es, "msg", "No theme 'default' — colours off.");
    // The profile loads LAST of the three data loads: a broken layout
    // fails the theme too, and its message must not bury the rescue
    // announcement — the status line has to SAY the fallback is active.
    if ( !load_profile(w, es, "joe") )
    {
	// No profile file = NO bindings at all (they are pure data), so a
	// silent continue traps the session in a bindingless editor (the
	// first genuine-Win validation hit exactly this). The baked-in
	// rescue set keeps the session usable — only ITS failure is fatal.
	if ( !bind_rescue_keys(w, es) )
	{
	    var dir;
	    ui::get(dir, w, es, "profile_dir");
	    ui::tui_close(t);
	    println(stderr, "madcide: cannot load the startup profile {}/joe.keys and the built-in rescue bindings failed to install — check the install layout (tools/madcide/profiles beside madcide.mad).", dir);
	    return;
	}
    }
    ui::set(w, es, "hint", 1);	// the transient bottom startup hint
    // The buffer table (AST-5): row 0 = the launch file; ^K E grows it.
    // ONE fresh-row owner (push_buffer_row — the gated dupaudit family).
    push_buffer_row(w, es, doc, path);
    ui::set(w, es, "bufidx", 0);
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
	    // Stage-2: spawn the parse as a cooperative task instead of
	    // blocking the loop on it. The input wait hands it the CPU in
	    // slices between polls; its completion surfaces as a `wake`
	    // event (or rides the next keystroke's recompose).
	    go background_parse(w, es, adoc);
	    continue;		// keep serving keystrokes while it runs
	}
	value ev;
	if ( !ui::tui_event(ev, t, w) )
	    break;
	run = apply_ide_event(w, es, adoc, ev);
	ui::set(w, es, "hint", 0);	// first event returns the row
    }
    // Stage-2: a spawned parse may still be mid-flight at quit — drain the
    // task scope BEFORE closing the handles and the world it writes to
    // (main's-end join would run only after this teardown). A live build
    // pump gets its stop FIRST (IDE-10b): the send fires its select case,
    // it SIGTERMs the child and exits — the drain never hangs on it.
    if ( es_int(w, es, "buildlive", 0) != 0 )
    {
	long stopc = es_int(w, es, "buildstop", 0);
	if ( stopc != 0 )
	{
	    value sv;
	    sv = 1;
	    madc::chan_send(stopc, sv);
	}
    }
    madc::task_drain();
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
