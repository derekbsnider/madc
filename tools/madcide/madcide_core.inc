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
// = one key-table generation the client rebinds on), and the
// help pane is a projection OF the loaded profile's own lines.
//
// Interaction state (caret/mark/bend/clip/search/prompt + pane, diags,
// outline, profile) lives on the editor-state bag; presentation state
// stays in the renderer. apply_ide_event is terminal-free
// (tests/testmadcide.mad drives it headlessly with the exact event value
// shapes).

// ---- the IDE session (the gateway seam, design doc 2026-08-31) ---------
// THIS FILE IS THE SESSION LAYER. IdeSession owns the IDE's state and
// semantics; apply_ide_event / compose_ide_tree are its surface. The ui
// entity bags are the session's private storage (initially — state
// migrates onto members in later gateway slices); the free functions
// below are its private implementation over them.
//
// The session NEVER touches a tui handle (gated:
// scripts/check-madcide-seam.sh). The TUI CLIENT (madcide_client.inc)
// keeps render / input / refresh / suspend and feeds the session what a
// terminal knows:
//   - FACTS the client pushes: viewport (cols/rows), terminal presence,
//     the pending chord (JOE's %k seat).
//   - the KEY TABLE the session loads as data: the client asks for it
//     (key_table) and installs it whenever keys_generation moves; the
//     session validates profiles handle-free (ui::tui_validate_keys), so
//     refusal semantics stay its own.
//   - TERMINAL REQUESTS the session parks (term_request: run / projrun /
//     cmd / shell / refresh): the client drains one after each event
//     (take_request), suspends, calls term_exec back to run the guest,
//     pauses, resumes. No terminal pushed = no request parks — headless
//     sessions refuse with "No terminal to suspend." exactly as before.
// A remote client would push the same facts and service the same
// requests over a transport; keys stay client-private either way.
class IdeSession
{
    public:
	long w;		// the ui world (documents + state entity live in it)
	long es;	// the editor-state entity — the bags

	long open(const char *path0, bool ro);	// full session startup;
						// > 0 = the launch doc,
						// 0 = unreadable path,
						// -1 = reported fatal
	void attach(long w0, long es0);	// a session over an EXISTING
					// world + state entity (the
					// headless harness, embedders)
	bool apply_ide_event(long doc, var &ev);	// false = quit
	void compose_ide_tree(var &root, long doc);
	long world();
	long active(long fallback);	// the active buffer's document
	bool parse_pending(long adoc);	// spawn the parked background
					// parse; true = spawned (client
					// keeps serving input)
	void viewport(long cols, long rows);	// client-pushed facts
	void terminal(bool has);
	void pending_chord(const char *pcs);
	long keys_generation();		// moves when the key table changes
	void key_table(var &out);
	bool take_request(var &out);	// drain the parked terminal request
	void term_exec(var &req);	// run the guest between the
					// client's suspend and resume
	void close();			// stop builds, drain tasks, close
					// parse handles, close the world
};

// ---- keybinding profiles as data --------------------------------------
// ONE rule for profile lines (parse_keys and the help pane share it):
// trimmed, non-empty, not a '#' comment. False = skip this line.
bool profile_line(var &out, array &lines, long i)
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

// path minus its extension — THE {base} rule (build_subst's substitution,
// the profile cycle's names, and the manifest default share it).
void path_sans_ext(var &out, var &p)
{
    out = p;
    const char *pc = p.c_str();
    const char *dot = strrchr(pc, '.');
    if ( dot != 0 )
	out = perl::substr(p, 0, dot - pc);
}

// One binding per line: "SEQ ACTION" — the action is the LAST word, the
// sequence everything before it. A leading "@name" token SCOPES the
// binding to that pane/modal widget (the owner keybinding law: pane and
// modal keys are profile DATA through this ONE parser, never a hardwired
// dispatch arm): "@confirm y pyes" binds y inside the confirm question.
// Unscoped lines fill `table` (the engine's main bindings); scoped lines
// fill `scopes` ({scope: {key: action}}, keys stored lowercase — the
// scoped alphabet is single key/character spellings). Later lines
// overwrite earlier per (scope, key), which is how the baked defaults
// merge under a profile's own respellings.
void parse_keys(var &table, var &scopes, const char *text)
{
    var t;
    var sc = scopes;		// accumulate over what the caller seeded
    array lines;
    php::explode(lines, "\n", text);
    long n = php::count(lines);
    for ( long i = 0; i < n; i = i + 1 )
    {
	var line;
	if ( !profile_line(line, lines, i) )
	    continue;
	const char *ls = line.c_str();
	if ( ls[0] == '@' )
	{
	    const char *sp1 = strchr(ls, ' ');
	    if ( sp1 == 0 )
		continue;
	    var scope = perl::substr(line, 1, sp1 - ls - 1);
	    var rest = php::trim(sp1 + 1);
	    const char *rs = rest.c_str();
	    const char *sp2 = strrchr(rs, ' ');
	    if ( sp2 == 0 )
		continue;
	    var action = php::trim(sp2 + 1);
	    var key = php::trim(perl::substr(rest, 0, sp2 - rs));
	    var lk = php::strtolower(key.c_str());
	    var one = sc[scope.c_str()];
	    if ( one.is_null() )
	    {
		var fresh;
		fresh[lk.c_str()] = action;
		sc[scope.c_str()] = fresh;
	    }
	    else
	    {
		one[lk.c_str()] = action;
		sc[scope.c_str()] = one;
	    }
	    continue;
	}
	const char *sp = strrchr(ls, ' ');
	if ( sp == 0 )
	    continue;
	long cut = sp - ls;
	var action = php::trim(sp + 1);
	var seq = php::trim(perl::substr(line, 0, cut));
	t[seq.c_str()] = action;
    }
    table = t;
    scopes = sc;
}

// The baked MODAL defaults (owner keybinding law — REPEAT correction
// 2026-08-31: key->action is NEVER hard-coded, no exceptions; modal
// widget keys included): an inline DATA profile through the SAME
// parse_keys — the rescue-keys pattern, never a hardwired dispatch arm.
// Every profile load parses this block FIRST, so a profile respells any
// of it with its own @scope lines and an old profile keeps working.
// Named residue (design doc 2026-08-31): arrows/enter INSIDE the
// engine's focused-choice widget stay engine navigation — @project's
// enter names what a CHOOSE means, not the raw key.
const char *modal_default_keys_text()
{
    return "# baked modal defaults (@scope key action) — profile data\n"
	   "@prompt enter pcommit\n"
	   "@prompt esc pcancel\n"
	   "@prompt backspace pdelchar\n"
	   "@confirm y pyes\n"
	   "@confirm n pcancel\n"
	   "@confirm esc pcancel\n"
	   "@project esc projclose\n"
	   "@project backspace projdelchar\n"
	   "@project ins projadd\n"
	   "@project del projdel\n"
	   "@project enter projopen\n"
	   "@pane esc paneclose\n";
}

// Modal key->action lookup over the bag's "modekeys" (the @scope tables).
// Key events look up by key name, text events by their single character
// — both case-insensitive (the JOE convention; parse_keys stores scoped
// keys lowercase). Which event KINDS consult a scope is that modal's
// semantics (a confirm reads characters, a prompt's characters are its
// input) — the caller picks. False = the scope binds nothing for this
// event. Sessions that never loaded a profile (the headless harness)
// lazily seed the baked defaults.
bool scoped_action(var &out, long w, long es, const char *scope, var &ev)
{
    var mk;
    ui::get(mk, w, es, "modekeys");
    if ( mk.is_null() )
    {
	var t0;
	parse_keys(t0, mk, modal_default_keys_text());
	ui::set(w, es, "modekeys", mk);
    }
    var sc = mk[scope];
    if ( sc.is_null() )
	return false;
    var kind = format("{}", ev["event"]);
    var k = "";
    if ( kind == "key" )
	k = format("{}", ev["key"]);
    else if ( kind == "text" )
    {
	var t = format("{}", ev["text"]);
	if ( strlen(t.c_str()) != 1 )
	    return false;
	k = t;
    }
    else
	return false;
    var lk = php::strtolower(k.c_str());
    var a = sc[lk.c_str()];
    if ( a.is_null() )
	return false;
    out = a;
    return true;
}

// Load a named profile from the session's profile directory: parse it,
// VALIDATE it handle-free (ui::tui_validate_keys — same rules the
// client's bind applies, so a table stored here installs there), and
// store the main table on the bag ("keytable" + a moved "keysgen" — the
// client rebinds when the generation moves). The raw profile text stays
// on the bag — the help pane (^K H) composes FROM it. False + a status
// message on failure (the previous table and its data stay).
bool load_profile(long w, long es, const char *name)
{
    // Ring discipline: `name` may lend a ring slot (a caller's .c_str());
    // the parse below makes hundreds of ring calls, so OWN the text
    // FIRST — the stored profile name rotted to garbage without this.
    var pname = name;
    var dir;
    ui::get(dir, w, es, "profile_dir");
    var txt;
    if ( !php::file_get_contents(txt, format("{}/{}.keys", dir, pname)) )
    {
	ui::set(w, es, "msg", format("No profile '{}'.", pname));
	return false;
    }
    // The baked modal defaults ride UNDER every profile (same parser,
    // later lines win): a profile respells a modal key by carrying its
    // own @scope line; one that says nothing keeps the defaults.
    var full = format("{}\n{}", modal_default_keys_text(), txt);
    var table;
    var scopes;
    parse_keys(table, scopes, full.c_str());
    if ( !ui::tui_validate_keys(table) )
    {
	ui::set(w, es, "msg", format("Profile '{}' rejected.", pname));
	return false;
    }
    ui::set(w, es, "keytable", table);
    ui::set(w, es, "keysgen", es_int(w, es, "keysgen", 0) + 1);
    ui::set(w, es, "modekeys", scopes);
    // The selection MODEL rides the profile's own vocabulary (owner
    // 2026-08-31: personalities are profile data): a set that binds
    // `bend` (JOE ^K K) is a TWO-POINT personality — ^K B alone must
    // not light mark..caret; a set without it keeps the live pico rule.
    bool twopoint = false;
    for ( var a : table )
	if ( a == "bend" )
	    twopoint = true;
    ui::set(w, es, "selblock", twopoint);
    ui::set(w, es, "profile", pname);
    ui::set(w, es, "bindtext", full);	// help documents the modal
					// defaults along with the profile
    return true;
}

// The baked-in RESCUE bindings: the ONLY compiled-in chords, used when
// NO profile file loads at startup (a broken install layout must never
// trap the session in a bindingless editor — the first genuine-Win
// validation did exactly that). Still data all the way down: an inline
// profile string through the SAME parse_keys/validate/store path, a
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
    var full = format("{}\n{}", modal_default_keys_text(), rescue_keys_text());
    var table;
    var scopes;
    parse_keys(table, scopes, full.c_str());
    if ( !ui::tui_validate_keys(table) )
	return false;
    ui::set(w, es, "keytable", table);
    ui::set(w, es, "keysgen", es_int(w, es, "keysgen", 0) + 1);
    ui::set(w, es, "modekeys", scopes);	// prompts stay usable in rescue
    ui::set(w, es, "selblock", false);	// no bend in the rescue set
    ui::set(w, es, "profile", "rescue");
    ui::set(w, es, "bindtext", full);
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
    var tname = name;		// ring discipline: own the text first
    var dir;
    ui::get(dir, w, es, "profile_dir");
    var txt;
    if ( !php::file_get_contents(txt, format("{}/{}.theme", dir, tname)) )
	return false;
    var th;
    array lines;
    php::explode(lines, "\n", txt.c_str());
    long n = php::count(lines);
    for ( long i = 0; i < n; i = i + 1 )
    {
	var line;
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
    ui::set(w, es, "scheme", tname);
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
    var sname = name;		// ring discipline: own the text first
    var lmsg = "\\i%k%T%W%I%X %n %m%y%R %M %x";	// joerc's defaults
    var rmsg = " %S Row %4r Col %3c";		// first char = fill
    var dir;
    ui::get(dir, w, es, "profile_dir");
    var txt;
    if ( php::file_get_contents(txt, format("{}/{}.status", dir, sname)) )
    {
	array lines;
	php::explode(lines, "\n", txt.c_str());
	long n = php::count(lines);
	for ( long i = 0; i < n; i = i + 1 )
	{
	    var line;
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
void spans_to_hspans(long w, long es, long doc, var &rows)
{
    var hs = {};
    var th;
    ui::get(th, w, es, "theme");
    if ( !th.is_null() )
    {
	for ( var r : rows )
	{
	    var c = th[r["class"].c_str()];
	    if ( c.is_null() )
		continue;
	    long ls = ui::text_line_start(w, doc, r["line"].as_integer());
	    if ( ls < 0 )
		continue;
	    long s = ls + r["column"].as_integer();
	    var hr;
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
    var rows;
    madc::parse_spans(rows, h);
    spans_to_hspans(w, es, doc, rows);
}

// Lexical colour at load (staged parsing stage 1): keyword / number /
// string / comment from LEXING the buffer alone — milliseconds where a
// C++ parse is seconds — so the FIRST paint is already in colour. The
// full parse's spans replace these when the handle lands.
void lex_first_spans(long w, long es, long doc)
{
    var t;
    ui::text(t, w, doc);
    var p;
    ui::get(p, w, doc, "path");
    var rows;
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
    var t;
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
    var t;
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
    var diags;
    madc::parse_check(diags, h);
    ui::set(w, es, "diags", diags);
    return php::count(diags);
}

// ERROR rows in a diagnostics array (severity vocabulary: error /
// warning) — warnings never bar a build or a run.
long diag_error_count(var &diags)
{
    long n = php::count(diags);
    long errs = 0;
    for ( long i = 0; i < n; i = i + 1 )
    {
	var r = diags[i];
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
    var o;
    madc::parse_outline(o, h);
    ui::set(w, es, "outline", o);
}

// The keybinding-profile cycle (the `profile` action + the options
// pane's Keymap row): the NEXT .keys file in the profile directory,
// alphabetically wrapping — profiles are FILES on disk, never a name
// ladder (Rule #7; emacs joined joe/pico, and a user-dropped profile
// joins the cycle with zero code). An unknown current name lands on
// the first profile. The status format rides the profile.
void toggle_profile(long w, long es)
{
    var dir;
    ui::get(dir, w, es, "profile_dir");
    array g;
    perl::glob(g, format("{}/*.keys", dir));
    php::sort(g);	// glob answers readdir order — the cycle must be
			// deterministic (alphabetical) on every filesystem
    long n = php::count(g);
    if ( n == 0 )
	return;
    var cur;
    ui::get(cur, w, es, "profile");
    long at = -1;
    var names = {};
    for ( long i = 0; i < n; i = i + 1 )
    {
	var p = php::array_get_cstr(g, i);
	const char *pc = p.c_str();
	const char *sl = strrchr(pc, '/');
	var nm = sl != 0 ? sl + 1 : pc;
	var base;
	path_sans_ext(base, nm);
	names.push(base);
	if ( base == cur )
	    at = i;
    }
    var nx = format("{}", names[(at + 1) % n]);
    load_profile(w, es, nx.c_str());
    load_status(w, es, nx.c_str());	// the format rides the profile
}

// A choose on the pane: move the caret to the chosen row's line. The
// rows live on the bag in pane order — the 1-based option is the index.
// A row carrying a "file" (a PROJECT check's rows span TUs) first opens
// or switches to that file through edit_file — THE one open-or-switch
// owner — then the caret lands in the now-active buffer.
void edit_file(long w, long es, long olddoc, const char *path0);
long active_doc(long w, long es, long fallback);
void goto_pane_row(long w, long es, long doc, long option)
{
    var pane;
    ui::get(pane, w, es, "pane");
    var rows;
    if ( pane == "outline" )
	ui::get(rows, w, es, "outline");
    else
	ui::get(rows, w, es, "diags");
    long i = 1;
    for ( var row : rows )
    {
	if ( i == option )
	{
	    long target = doc;
	    var f = row["file"];
	    if ( !f.is_null() )
	    {
		var cp;
		ui::get(cp, w, doc, "path");
		if ( !(f == cp.c_str()) )
		{
		    var ff = format("{}", f);
		    edit_file(w, es, doc, ff.c_str());
		    target = active_doc(w, es, doc);
		}
	    }
	    ui::set(w, es, "caret",
		    caret_to_line(w, target, row["line"].as_integer(), 0));
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
bool view_name(var &out, long w, long es)
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
// (JOE's chord echo — the client-pushed fact), %x the enclosing context —
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
void status_expand(var &out, long w, long es, long doc, long caret,
		   var &fmtv, var &fnctxv)
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
	    // The pending chord is a CLIENT-pushed fact (keys are the
	    // client's; the session only typesets the echo seat).
	    var pc;
	    ui::get(pc, w, es, "pendingchord");
	    if ( !pc.is_null() && strlen(pc.c_str()) > 0 )
		seat = format("{} ", pc);
	}
	if ( e == 'x' )
	    seat = fnctxv;
	if ( e == 'y' )
	{
	    var vw;
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
bool compose_overlay_row(var &row, long w, long es)
{
    var st;
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
void compose_joe_statusline(var &status, long w, long es, long doc,
			    long caret, var &fnctx)
{
    var st;
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
    var lout;
    status_expand(lout, w, es, doc, caret, lm, fnctx);
    var fill = " ";
    var rfmt = "";
    if ( strlen(rm.c_str()) > 0 )
    {
	fill = perl::substr(rm, 0, 1);
	rfmt = perl::substr(rm, 1, strlen(rm.c_str()) - 1);
    }
    var rout;
    status_expand(rout, w, es, doc, caret, rfmt, fnctx);
    // The viewport is a CLIENT-pushed fact (headless sessions typeset
    // the joerc default width).
    long cols = es_int(w, es, "vcols", 80);
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
    var txt;
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
// The ^P project window (defined after the buffer machinery): the prompt
// commits membership through these.
void proj_add(long w, long es, long doc, const char *path0);
bool project_active(long w, long es);
void project_rows(long w, long es, long doc);

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
    if ( mode == "projadd" )
    {
	// The prompt is a human client's ARGUMENT collector (gateway
	// ruling): the command is projadd(path).
	proj_add(w, es, doc, in.c_str());
	if ( project_active(w, es) )
	    project_rows(w, es, doc);
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
	var vw;
	if ( !view_name(vw, w, es) )
	    refresh_spans(w, es, doc, ensure_phandle(w, es, doc));
	ui::set(w, es, "msg", format("Theme: {}.", in));
    }
}

// Every event while the prompt is up. The keys are @prompt/@confirm
// profile data (the baked defaults: enter pcommit / esc pcancel /
// backspace pdelchar; y pyes / n pcancel / esc pcancel) — this arm
// dispatches the looked-up NAMES, never the keys themselves.
bool prompt_event(long w, long es, long doc, long caret, var &ev)
{
    var kind = format("{}", ev["event"]);
    var pmode;
    ui::get(pmode, w, es, "pmode");
    if ( pmode == "confirm" )
    {
	// Single-key question: characters AND keys consult @confirm
	// (a confirm reads its answer characters — unlike a prompt,
	// whose characters are input). ^C arrives as its BOUND action
	// (discard in the JOE profile), so any action cancels too.
	// Unbound keys leave the question standing.
	var a;
	if ( scoped_action(a, w, es, "confirm", ev) )
	{
	    if ( a == "pyes" )
	    {
		var vb;
		ui::get(vb, w, es, "pverb");
		end_prompt(w, es);
		return do_verb(w, es, vb.c_str());
	    }
	    if ( a == "pcancel" )
		end_prompt(w, es);
	    return true;
	}
	if ( kind == "action" )
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
    var a;
    if ( !scoped_action(a, w, es, "prompt", ev) )
	return true;			// unbound keys are inert
    if ( a == "pcommit" )
	finish_prompt(w, es, doc, caret);
    else if ( a == "pcancel" )
	end_prompt(w, es);
    else if ( a == "pdelchar" )
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
bool block_range(var &r, long w, long es, long caret)
{
    long mark = es_int(w, es, "mark", -1);
    long bend = es_int(w, es, "bend", -1);
    long e = bend >= 0 ? bend : caret;
    if ( bend < 0 && es_flag(w, es, "selblock") )
	e = mark;	// two-point personality: no end yet = no block
    var b;
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
    var r;
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
    var t;
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
    var r;
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
    var t;
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
    var r;
    if ( !block_range(r, w, es, caret) )
	return;
    long lo = r["lo"].as_integer();
    long hi = r["hi"].as_integer();
    var t;
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
    var vw;
    if ( !view_name(vw, w, es) )
	return doc;
    long vd = es_int(w, es, "vdoc", 0);
    return vd != 0 ? vd : doc;
}

bool enter_view(long w, long es, long doc, const char *name)
{
    var t;
    ui::text(t, w, doc);
    var p;
    ui::get(p, w, doc, "path");
    var emitted;
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
    var cur;
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
    var vw;
    if ( view_name(vw, w, es) )
	exit_view(w, es, olddoc);
    var bufs;
    ui::get(bufs, w, es, "buffers");
    long cur = es_int(w, es, "bufidx", 0);
    var row = bufs[cur];
    row["caret"] = es_int(w, es, "caret", 0);
    row["mark"] = es_int(w, es, "mark", -1);
    row["bend"] = es_int(w, es, "bend", -1);
    bufs[cur] = row;
    ui::set(w, es, "buffers", bufs);
    var t = bufs[idx];
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
    var bufs;
    ui::get(bufs, w, es, "buffers");
    if ( bufs.is_null() )
    {
	var fresh = {};
	bufs = fresh;
    }
    var b;
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
    var bufs;
    ui::get(bufs, w, es, "buffers");
    if ( bufs.is_null() || php::count(bufs) == 0 )
    {
	var seed = {};
	var b0;
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
	var r = bufs[i];
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
    var ws;
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
    var ws;
    ui::get(ws, w, es, "windows");
    if ( ws.is_null() || php::count(ws) < 2 )
	return;
    long at = es_int(w, es, "winat", 0);
    var r = ws[at];
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
    var ws;
    ui::get(ws, w, es, "windows");
    var t = ws[idx];
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
    long trows = es_int(w, es, "vrows", 24);	// client-pushed viewport
    long n = window_count(w, es);
    long want = n < 2 ? 2 : n + 1;
    if ( want * (window_min_rows() + 1) > trows )
    {
	ui::set(w, es, "msg", "No room for another window.");
	return;
    }
    var ws;
    ui::get(ws, w, es, "windows");
    long at = 0;
    if ( ws.is_null() || php::count(ws) < 2 )
    {
	// Seed: window 0 adopts the live state.
	var seed = {};
	var w0;
	w0["bufidx"] = es_int(w, es, "bufidx", 0);
	w0["caret"] = es_int(w, es, "caret", 0);
	w0["grow"] = 0;
	seed.push(w0);
	ws = seed;
	ui::set(w, es, "winat", 0);
    }
    else
	at = es_int(w, es, "winat", 0);
    var dup;
    dup["bufidx"] = es_int(w, es, "bufidx", 0);
    dup["caret"] = es_int(w, es, "caret", 0);
    dup["grow"] = 0;
    var rebuilt = {};
    long i = 0;
    for ( var r : ws )
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
    var ws;
    ui::get(ws, w, es, "windows");
    var rebuilt = {};
    long i = 0;
    long newat = 0;
    for ( var r : ws )
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
    long trows = es_int(w, es, "vrows", 24);	// client-pushed viewport
    long base = (trows - n) / n;	// even content share
    long at = es_int(w, es, "winat", 0);
    long nx = (at + 1) % n;
    var ws;
    ui::get(ws, w, es, "windows");
    var ra = ws[at];
    var rn = ws[nx];
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

// ---- IDE-10a: the ^P PROJECT window (owner rulings 2026-08-31) ----------
// ^P's PRIMARY function is the PROJECT (quick-open rides the filter as
// the secondary); it correlates directly with --project. The ONE
// popup-list mechanism (owner ruling 2026-08-27): a `choice` pane in the
// model's LIST presentation (hint list:1) that is AUTOFOCUSED (hint
// focus:1 — arrows/enter navigate it immediately; selection stays the
// model's presentation state). A POPUP, never a permanent window: ^P
// raises it, esc/^P dismisses it. Rows come in GROUPS: [project] TUs
// (manifest order) -> [open] buffers not in the project (dirty-marked)
// -> [cwd] source files while a filter is typed. Text edits the filter;
// enter/ins/del/esc/backspace are @project PROFILE DATA (the keybinding
// law) — enter=projopen routes through edit_file, THE one open-or-switch
// owner; ins=projadd (the prompt collects the argument), del=projdel on
// the focused row (the engine hands its selection with the key event).
// Removing a member never touches the file; opening files stays on
// ^K E/the window; new files land in [open].
//
// The manifest is the NATIVE object shape (mirrors ProjectManifest:
// tus/entry/output; --project reads it interchangeably with the
// compile_commands.json import). The RAW decoded value lives on the bag
// ("proj") so fields this window does not interpret (a future commands
// section, per-TU options) round-trip UNTOUCHED through membership
// edits. IMPLICIT single-file project: no manifest on disk — the launch
// file is the one TU; the manifest MATERIALIZES on the second file's
// projadd (default <base>.prj.json beside the launch file, immediate
// persistence, status-line announce). The ^B build palette (IDE-10b)
// reuses the same widget with action rows.

// The project GENERATION: every project-state change bumps it, and
// consumers that derive data from the project (the ^B rows) rebuild
// when their recorded generation differs — no consumer re-reads the
// manifest per event.
void proj_touch(long w, long es)
{
    ui::set(w, es, "projgen", es_int(w, es, "projgen", 0) + 1);
}

// The launch file: the implicit project's one TU and the manifest
// default's <base>. Headless sessions without run_ide's seed fall back
// to the driven document's path.
void proj_launch_path(var &out, long w, long es, long doc)
{
    var lp;
    ui::get(lp, w, es, "launchpath");
    if ( !lp.is_null() && strlen(lp.c_str()) > 0 )
    {
	out = lp;
	return;
    }
    ui::get(out, w, doc, "path");
}

// The project's TUs as {path, ti} rows (ti = the tus-array index; -1 =
// the implicit TU). A native TU entry is a bare string or an object
// carrying "file" — only that field is interpreted here; the rest rides.
void proj_tu_rows(var &out, long w, long es, long doc)
{
    var rows = {};
    var proj;
    ui::get(proj, w, es, "proj");
    if ( proj.is_null() )
    {
	var lp;
	proj_launch_path(lp, w, es, doc);
	var r;
	r["path"] = lp;
	r["ti"] = -1;
	rows.push(r);
	out = rows;
	return;
    }
    var tus = proj["tus"];
    long i = 0;
    for ( var t : tus )
    {
	// A bare string IS the file; an object TU names its file. The
	// kind question comes FIRST — a keyed access on a scalar is a
	// script error by design (madarray_key_slot).
	var r;
	if ( t.is_string() )
	    r["path"] = t;
	else
	    r["path"] = t["file"];
	r["ti"] = i;
	rows.push(r);
	i = i + 1;
    }
    out = rows;
}

bool proj_member(long w, long es, long doc, const char *path)
{
    var rows;
    proj_tu_rows(rows, w, es, doc);
    for ( var r : rows )
	if ( r["path"] == path )
	    return true;
    return false;
}

// Persist the manifest — IMMEDIATELY (no dirty-manifest state, the owner
// default). The first write derives the default path (<base>.prj.json
// beside the launch file) and announces the materialization on the
// status line.
bool proj_write(long w, long es, long doc)
{
    var proj;
    ui::get(proj, w, es, "proj");
    var pf;
    ui::get(pf, w, es, "projfile");
    bool fresh = pf.is_null() || strlen(pf.c_str()) == 0;
    if ( fresh )
    {
	var lp;
	proj_launch_path(lp, w, es, doc);
	var base;
	path_sans_ext(base, lp);
	pf = format("{}.prj.json", base);
	ui::set(w, es, "projfile", pf);
    }
    var txt = format("{}\n", js::stringify(proj, 2));
    if ( php::file_put_contents(pf.c_str(), txt.c_str()) < 0 )
    {
	ui::set(w, es, "msg", format("Cannot write {}.", pf));
	return false;
    }
    if ( fresh )
	ui::set(w, es, "msg", format("Project file created: {}", pf));
    return true;
}

// projadd — the argument-taking membership verb (a prompt collects the
// argument for a human client; a remote client passes it in the
// command). The SECOND file materializes an implicit project's manifest.
void proj_add(long w, long es, long doc, const char *path0)
{
    var p = path0;		// ring discipline: own the text
    if ( strlen(p.c_str()) == 0 )
	return;
    const char *pc = p.c_str();
    if ( pc[0] == '[' )
    {
	// [build]-style pseudo-buffers are not files.
	ui::set(w, es, "msg", format("Not a file: {}", p));
	return;
    }
    if ( proj_member(w, es, doc, p.c_str()) )
    {
	ui::set(w, es, "msg", format("Already in the project: {}", p));
	return;
    }
    var proj;
    ui::get(proj, w, es, "proj");
    if ( proj.is_null() )
    {
	var np;
	var lp;
	proj_launch_path(lp, w, es, doc);
	var tus = {};
	tus.push(lp);
	tus.push(p);
	np["tus"] = tus;
	np["entry"] = "main";
	proj = np;
    }
    else
    {
	var tus = proj["tus"];
	tus.push(p);
	proj["tus"] = tus;
    }
    ui::set(w, es, "proj", proj);
    proj_touch(w, es);
    var pf0;
    ui::get(pf0, w, es, "projfile");
    bool had = !pf0.is_null() && strlen(pf0.c_str()) > 0;
    if ( proj_write(w, es, doc) && had )
	ui::set(w, es, "msg", format("Added {} to the project.", p));
}

// projaddcur — add the ACTIVE buffer's file (^K J's proposed joe seat).
void proj_addcur(long w, long es, long doc)
{
    var p;
    ui::get(p, w, doc, "path");
    if ( p.is_null() || strlen(p.c_str()) == 0 )
	return;
    proj_add(w, es, doc, p.c_str());
}

// projdel — remove the FOCUSED row's TU (the engine hands the focused
// choice's selection with the key event). Never touches the file; a TU
// entry leaves whole, its uninterpreted fields with it.
void proj_del(long w, long es, long doc, long option)
{
    var rows;
    ui::get(rows, w, es, "projrows");
    long idx = option - 1;		// 1-based on the wire
    if ( idx < 0 || idx >= php::count(rows) )
	return;
    var row = rows[idx];
    if ( !(row["tag"] == "prj") )
    {
	ui::set(w, es, "msg", format("Not in the project: {}", row["path"]));
	return;
    }
    var proj;
    ui::get(proj, w, es, "proj");
    if ( proj.is_null() )
    {
	// The implicit project has no manifest to edit — its one TU is
	// the session's own launch file.
	ui::set(w, es, "msg",
		"No project file yet (ins adds a file, creating it).");
	return;
    }
    long ti = row["ti"].as_integer();
    var tus = proj["tus"];
    var rebuilt = {};
    long i = 0;
    for ( var t : tus )
    {
	if ( i != ti )
	    rebuilt.push(t);
	i = i + 1;
    }
    proj["tus"] = rebuilt;
    ui::set(w, es, "proj", proj);
    proj_touch(w, es);
    if ( proj_write(w, es, doc) )
	ui::set(w, es, "msg",
		format("Removed {} from the project.", row["path"]));
    project_rows(w, es, doc);
}

// Startup manifest read (run_ide): a <base>.prj.json beside the launch
// file IS the project — persistence would be pointless if nothing read
// it back. Malformed content refuses LOUD; the session stays implicit.
void proj_startup(long w, long es, long doc)
{
    var lp;
    proj_launch_path(lp, w, es, doc);
    var base;
    path_sans_ext(base, lp);
    var pf = format("{}.prj.json", base);
    if ( !php::file_exists(pf) )
	return;
    var txt;
    if ( !php::file_get_contents(txt, pf.c_str()) )
    {
	ui::set(w, es, "msg", format("Cannot read {}.", pf));
	return;
    }
    var proj;
    if ( !js::parse(proj, txt) )
    {
	ui::set(w, es, "msg", format("{} is not valid JSON.", pf));
	return;
    }
    if ( !proj.is_object() )
    {
	// Valid JSON but not the native OBJECT shape (a scalar, or a
	// compile_commands array — the read-only import is --project's
	// lane, not the window's).
	ui::set(w, es, "msg",
		format("{} is not a madc project manifest.", pf));
	return;
    }
    var tus = proj["tus"];
    if ( tus.is_null() )
    {
	ui::set(w, es, "msg",
		format("{} has no \"tus\" — not a project manifest.", pf));
	return;
    }
    ui::set(w, es, "proj", proj);
    ui::set(w, es, "projfile", pf);
    proj_touch(w, es);
    ui::set(w, es, "msg", format("Project {}: {} TU(s).", pf,
				 php::count(tus)));
}

// Recompute the window's rows onto the bag ("projrows": {path, tag, ti,
// content} — content precomputed here so composition stays a renderer).
// Case-insensitive substring filter; ring-lifetime strtolower returns
// are copied into var carriers immediately (the ring discipline).
// Markers: 'p' = project member, 'o' = open non-member, '*' = modified.
void project_rows(long w, long es, long doc)
{
    var filt;
    ui::get(filt, w, es, "projfilter");
    var lf = php::strtolower(filt.c_str());
    long fn = strlen(lf.c_str());
    var rows = {};
    var seen;			// path -> 1: earlier groups dedupe later
    var bufs;
    ui::get(bufs, w, es, "buffers");
    // [project] — manifest order.
    var tus;
    proj_tu_rows(tus, w, es, doc);
    for ( var tr : tus )
    {
	var p = format("{}", tr["path"]);
	seen[p.c_str()] = 1;
	var lp = php::strtolower(p.c_str());
	if ( fn > 0 && perl::index(lp.c_str(), lf.c_str()) < 0 )
	    continue;
	var mk = " ";
	for ( var b : bufs )
	{
	    if ( b["path"] == p )
	    {
		var m;
		ui::get(m, w, b["doc"].as_integer(), "modified");
		if ( !m.is_null() && m.as_boolean() )
		    mk = "*";
	    }
	}
	var r;
	r["path"] = p;
	r["tag"] = "prj";
	r["ti"] = tr["ti"];
	r["content"] = format("p{} {}", mk, p);
	rows.push(r);
    }
    // [open] — buffers not in the project, dirty-marked.
    for ( var b : bufs )
    {
	var p = format("{}", b["path"]);
	if ( !seen[p.c_str()].is_null() )
	    continue;
	seen[p.c_str()] = 1;
	var lp = php::strtolower(p.c_str());
	if ( fn > 0 && perl::index(lp.c_str(), lf.c_str()) < 0 )
	    continue;
	var m;
	ui::get(m, w, b["doc"].as_integer(), "modified");
	var mk = " ";
	if ( !m.is_null() && m.as_boolean() )
	    mk = "*";
	var r;
	r["path"] = p;
	r["tag"] = "open";
	r["content"] = format("o{} {}", mk, p);
	rows.push(r);
    }
    // [cwd] — quick-open rides the FILTER (the window is the project's;
    // an empty filter shows membership, not the directory). Source-
    // extension globs, one pattern each (glob(3) has no brace expansion
    // here); the recursive walk stays the named later slice.
    if ( fn > 0 )
    {
	var pats = {"*.mad", "*.c", "*.cc", "*.cpp", "*.cxx", "*.h",
		    "*.hpp", "*.inc", "*.json", "*.md", "*.txt"};
	for ( var pat : pats )
	{
	    array g;
	    perl::glob(g, pat.c_str());
	    for ( var f : g )
	    {
		var p = format("{}", f);
		if ( !seen[p.c_str()].is_null() )
		    continue;
		var lp = php::strtolower(p.c_str());
		if ( perl::index(lp.c_str(), lf.c_str()) < 0 )
		    continue;
		var r;
		r["path"] = p;
		r["tag"] = "cwd";
		r["content"] = format("   {}", p);
		rows.push(r);
	    }
	}
    }
    ui::set(w, es, "projrows", rows);
}

void proj_show(long w, long es, long doc)
{
    ui::set(w, es, "pane", "project");
    ui::set(w, es, "projfilter", "");
    project_rows(w, es, doc);
}

void proj_hide(long w, long es)
{
    ui::set(w, es, "pane", "");
    ui::set(w, es, "projfilter", "");
}

bool project_active(long w, long es)
{
    var pane;
    ui::get(pane, w, es, "pane");
    if ( pane.is_null() )
	return false;
    return pane == "project";
}

// Every event while the project window is up (modal, like the prompt):
// text edits the FILTER; keys dispatch through the @project scope's
// looked-up NAMES (projopen / projadd / projdel / projclose /
// projdelchar) — never the keys themselves. Enter's meaning is data
// too: the engine CHOOSES, @project's enter row names what a choose
// does. Other bound actions are swallowed — a modal window half-running
// ^K chords would be worse than inert keys.
bool project_event(long w, long es, long doc, var &ev)
{
    var kind = format("{}", ev["event"]);
    if ( kind == "text" )
    {
	var in;
	ui::get(in, w, es, "projfilter");
	var nx = format("{}{}", in, ev["text"]);
	ui::set(w, es, "projfilter", nx.c_str());
	project_rows(w, es, doc);
	return true;
    }
    if ( kind == "choose" )
    {
	var a;
	var ke;
	ke["event"] = "key";
	ke["key"] = "enter";
	if ( !scoped_action(a, w, es, "project", ke) )
	    return true;
	if ( a == "projopen" )
	{
	    var rows;
	    ui::get(rows, w, es, "projrows");
	    long opt = ev["option"].as_integer() - 1;	// 1-based wire
	    if ( opt >= 0 && opt < php::count(rows) )
	    {
		var r = rows[opt];
		var p = format("{}", r["path"]);
		proj_hide(w, es);
		edit_file(w, es, doc, p.c_str());
	    }
	}
	return true;
    }
    if ( kind == "action" )
    {
	var a = format("{}", ev["action"]);
	if ( a == "project" || a == "quit" )
	    proj_hide(w, es);
	else if ( a == "projaddcur" )
	{
	    proj_addcur(w, es, doc);
	    project_rows(w, es, doc);
	}
	return true;
    }
    if ( kind != "key" )
	return true;
    var a;
    if ( !scoped_action(a, w, es, "project", ev) )
	return true;			// unbound keys are inert
    if ( a == "projclose" )
	proj_hide(w, es);
    else if ( a == "projdelchar" )
    {
	var in;
	ui::get(in, w, es, "projfilter");
	long n = strlen(in.c_str());
	if ( n > 0 )
	{
	    ui::set(w, es, "projfilter", perl::substr(in, 0, n - 1));
	    project_rows(w, es, doc);
	}
    }
    else if ( a == "projadd" )
	start_prompt(w, es, "File to add to project", "projadd");
    else if ( a == "projdel" )
    {
	var opt = ev["option"];
	if ( opt.is_null() )
	    ui::set(w, es, "msg", "No row focused.");
	else
	    proj_del(w, es, doc, opt.as_integer());
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

// {path} / {base} / {project} / {madc} substitution: path = the active
// buffer's file, base = path minus its extension, project = the open
// manifest's path ("" without one), madc = THE RUNNING COMPILER'S own
// executable (madc::compiler_path). {madc}/{project} serve
// MANIFEST-DECLARED commands only — no default row spawns any madc
// binary (owner ruling 2026-08-27: the running madc IS the compiler;
// Run forks the live parse / the --project lane). Commands are DATA —
// no quoting layer (the exec:// factory splits on single spaces; see
// madc_process.cpp).
void build_subst(var &out, const char *cmd, long w, long es, long doc)
{
    var p;
    ui::get(p, w, doc, "path");
    var base;
    path_sans_ext(base, p);	// THE {base} rule, shared with the
				// manifest default
    var pf;
    ui::get(pf, w, es, "projfile");
    var s1 = php::str_replace("{path}", p.c_str(), cmd);
    var s2 = php::str_replace("{base}", base.c_str(), s1.c_str());
    var s3 = php::str_replace("{project}", pf.c_str(), s2.c_str());
    var self = madc::compiler_path();
    out = php::str_replace("{madc}", self.c_str(), s3.c_str());
}

// The [build] buffer: found-or-created in the buffer table WITHOUT
// switching to it (the pump appends in the background; ^P reaches it).
long build_doc(long w, long es)
{
    var bufs;
    ui::get(bufs, w, es, "buffers");
    long n = php::count(bufs);
    for ( long i = 0; i < n; i = i + 1 )
    {
	var r = bufs[i];
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
    var cases;
    cases[0] = stopc;
    cases[1] = madc::chan_readable(bc);
    var got;
    var line;
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
	var v;
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
    var diags;
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
    build_subst(o, outtmpl.c_str(), w, es, doc);
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

// Park a TERMINAL REQUEST for the client to execute (the gateway seam:
// render/input/refresh/suspend are the TUI client's — the session only
// says WHAT it needs). One request slot; the client drains it right
// after the event that parked it (take_request), so nothing intervenes.
// A request rides {kind, pause, ...}: kind = run / projrun / cmd /
// shell / refresh; pause = hold the guest's output until enter (the
// return-key pause is the CLIENT's — one owner, terminal_return_pause
// in madcide_client.inc). No terminal pushed = no request parks; false
// lets the caller refuse ("No terminal to suspend.") exactly as the
// headless sessions always have.
bool term_request(long w, long es, var &req)
{
    if ( !es_flag(w, es, "hasterm") )
	return false;
    ui::set(w, es, "termreq", req);
    return true;
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
    var diags;
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
    var rq;
    rq["kind"] = "run";
    rq["doc"] = doc;
    rq["pause"] = 1;
    if ( !term_request(w, es, rq) )
	ui::set(w, es, "msg", "No terminal to suspend.");
}

// terminal-mode rows (Run native): the JOE ^K Z shape — the program
// gets the REAL terminal; a return-key pause holds its output on screen
// before the full-repaint resume.
void run_in_terminal(long w, long es, const char *cmd)
{
    var rq;
    rq["kind"] = "cmd";
    rq["cmd"] = cmd;
    rq["pause"] = 1;
    if ( !term_request(w, es, rq) )
	ui::set(w, es, "msg", "No terminal to suspend.");
}

// ---- the ^B project correlation (owner design doc 2026-08-31) ----------
// "With a manifest open, Build = the --project build": the default rows
// become project-wide (check every TU, build/run the linked project),
// and the manifest's `commands` section APPENDS its own rows (the
// ide-controls plan: a project declares its own entries — appended, not
// replacing, so declaring one command never costs the built-ins). An
// IMPLICIT project (no manifest) keeps the single-buffer rows — a
// one-TU project's build IS the buffer build.

// The whole project's diagnostics rows: a project handle (every TU
// parsed with its manifest options — diagnostics match the --project
// build), each TU's parse_check rows tagged with its FILE (the pane
// renders it; goto opens it). False = the manifest did not open.
bool proj_check_rows(var &merged, long w, long es)
{
    var pf;
    ui::get(pf, w, es, "projfile");
    long h = madc::project_open(pf.c_str());
    if ( h == 0 )
    {
	ui::set(w, es, "msg", format("Cannot open project {}.", pf));
	return false;
    }
    var tus;
    madc::project_tus(tus, h);
    var rows = {};
    for ( var t : tus )
    {
	long th = t["handle"].as_integer();
	if ( th == 0 )
	{
	    // The TU never opened (unreadable file / refused options):
	    // visible as a row, never silent.
	    var r;
	    r["file"] = t["file"];
	    r["line"] = 1;
	    r["column"] = 0;
	    r["severity"] = "error";
	    r["message"] = "cannot open this TU (file or manifest options)";
	    rows.push(r);
	    continue;
	}
	var trows;
	madc::parse_check(trows, th);
	for ( var r : trows )
	{
	    r["file"] = t["file"];
	    rows.push(r);
	}
    }
    madc::project_close(h);
    merged = rows;
    return true;
}

// The project Check ACTION (bld-proj-check): rows + the pane + the tally.
void proj_run_check(long w, long es, long doc)
{
    var pf;
    ui::get(pf, w, es, "projfile");
    if ( pf.is_null() || strlen(pf.c_str()) == 0 )
    {
	// Implicit single-file project: the buffer check IS the check.
	run_check(w, es, doc);
	return;
    }
    var rows;
    if ( !proj_check_rows(rows, w, es) )
	return;
    ui::set(w, es, "diags", rows);
    ui::set(w, es, "pane", "diags");
    long n = php::count(rows);
    if ( n == 0 )
	ui::set(w, es, "msg", "Project check: clean.");
    else
	ui::set(w, es, "msg", format("Project check: {} problem(s).", n));
}

// The project build task (bld-proj-build): madc::project_build — the
// --project AOT lane in-process. NO yield points inside the call (the
// project-scale instance of the named backend-yield residue: the editor
// blocks for the build's duration; Stop lands only pre-start via the
// buildstopreq flag). Long args only; the manifest/kind/out ride the
// bag like buildcmd.
void project_build_task(long w, long es)
{
    var pf, k, o;
    ui::get(pf, w, es, "pbfile");
    ui::get(k, w, es, "pbkind");
    ui::get(o, w, es, "pbout");
    bool pre = es_int(w, es, "buildstopreq", 0) != 0;
    var diags;
    bool ok = false;
    if ( !pre )
	ok = madc::project_build(diags, pf.c_str(), k.c_str(), o.c_str());
    ui::set(w, es, "buildlive", 0);
    if ( pre )
    {
	ui::set(w, es, "msg", format("Build stopped ({}).", pf));
	return;
    }
    ui::set(w, es, "diags", diags);
    if ( ok )
    {
	ui::set(w, es, "msg", format("Built project {} -> {}", pf, o));
	return;
    }
    var pane;
    ui::get(pane, w, es, "pane");
    if ( strlen(pane.c_str()) == 0 )
	ui::set(w, es, "pane", "diags");
    ui::set(w, es, "msg",
	    format("Project build failed: {} problem(s).", php::count(diags)));
}

void start_project_build(long w, long es, long doc, var &kind, var &out)
{
    var pf;
    ui::get(pf, w, es, "projfile");
    if ( pf.is_null() || strlen(pf.c_str()) == 0 )
    {
	// Implicit single-file project: the buffer build IS the build.
	var ot = "{base}";
	start_native_build(w, es, doc, kind, ot);
	return;
    }
    if ( es_int(w, es, "buildlive", 0) != 0 )
    {
	ui::set(w, es, "msg", "A build is already running.");
	return;
    }
    ui::set(w, es, "pbfile", pf);
    ui::set(w, es, "pbkind", kind);
    ui::set(w, es, "pbout", out);
    ui::set(w, es, "buildstop", 0);
    ui::set(w, es, "buildscope", 0);	// no cancellation chain reaches
    ui::set(w, es, "buildstopreq", 0);	// the project lane (named residue)
    ui::set(w, es, "buildlive", 1);
    go project_build_task(w, es);
    ui::set(w, es, "msg", format("Building project {} -> {}", pf, out));
}

// The project Run (bld-proj-run): the parse verdict FIRST (run_buffer's
// shape — a red project must not run; the rows land in the diags pane),
// then the terminal handoff around madc::project_run.
void run_project(long w, long es, long doc)
{
    var pf;
    ui::get(pf, w, es, "projfile");
    if ( pf.is_null() || strlen(pf.c_str()) == 0 )
    {
	// Implicit single-file project: the buffer Run IS the run.
	run_buffer(w, es, doc);
	return;
    }
    var rows;
    if ( !proj_check_rows(rows, w, es) )
	return;
    ui::set(w, es, "diags", rows);
    long errs = diag_error_count(rows);
    if ( errs > 0 )
    {
	ui::set(w, es, "pane", "diags");
	ui::set(w, es, "msg",
		format("Run refused: {} error(s) — fix them first.", errs));
	return;
    }
    var rq;
    rq["kind"] = "projrun";
    rq["pause"] = 1;
    if ( !term_request(w, es, rq) )
	ui::set(w, es, "msg", "No terminal to suspend.");
}

// Seed the command rows AS DATA. Regeneration rides the project
// GENERATION (proj_touch): rows rebuild when their recorded generation
// differs from the project's; rows set from OUTSIDE this seeder (a
// harness override) carry no generation and are ADOPTED at the current
// one — kept until the project next changes.
void ensure_build_cmds(long w, long es)
{
    long pg = es_int(w, es, "projgen", 0);
    var have;
    ui::get(have, w, es, "buildcmds");
    if ( !have.is_null() && php::count(have) > 0 )
    {
	long bg = es_int(w, es, "buildgen", -1);
	if ( bg == pg )
	    return;
	if ( bg < 0 )
	{
	    ui::set(w, es, "buildgen", pg);
	    return;
	}
    }
    ui::set(w, es, "buildgen", pg);
    // OWNER RULING (2026-08-27, the running madc IS the compiler): no
    // row execs any madc binary. Without a manifest, every default row
    // rides the buffer's LIVE parse handle — Build emits from its tree
    // (madc::parse_build), Run forks it (madc::parse_run). WITH one
    // (owner design doc 2026-08-31), Build = the --project build: the
    // rows go project-wide (madc::project_build / project_run — the
    // --project lanes as library verbs), and the manifest's `commands`
    // section appends its own rows. Run native runs the BUILT artifact
    // (the user's own program). The exec:// capture pump serves
    // manifest-declared commands (mode "capture"); no default rides it.
    // Stop (MT-3b): a buffer build cancels through its task's scope;
    // a project build only pre-start (named residue); terminal runs
    // stay ^C-able in their own terminal.
    var proj;
    ui::get(proj, w, es, "proj");
    var pf;
    ui::get(pf, w, es, "projfile");
    bool projected = !proj.is_null() && !pf.is_null()
		  && strlen(pf.c_str()) > 0;
    var rows = {};
    if ( projected )
    {
	// The project's OUTPUT: the manifest's `output` field, else the
	// manifest's own base name.
	var pout;
	var o = proj["output"];
	if ( o.is_null() )
	    path_sans_ext(pout, pf);
	else
	    pout = format("{}", o);
	var r1;
	r1["name"] = "Check project (in-process)";
	r1["act"] = "bld-proj-check";
	rows.push(r1);
	var r2;
	r2["name"] = "Build project (in-process)";
	r2["kind"] = "exe";
	r2["out"] = pout;
	r2["act"] = "bld-proj-build";
	rows.push(r2);
	var r3;
	r3["name"] = "Run project";
	r3["act"] = "bld-proj-run";	// the --project JIT lane, forked
	rows.push(r3);
	var r4;
	r4["name"] = "Run native";
	r4["cmd"] = format("./{}", pout);
	r4["mode"] = "terminal";
	r4["act"] = "bld-run-cmd";
	rows.push(r4);
	var r5;
	r5["name"] = "Stop";
	r5["act"] = "bld-stop";
	rows.push(r5);
	// The manifest's commands section: {name, cmd, mode?} rows AS
	// DATA, appended after the built-ins ({path}/{base}/{project}/
	// {madc} substitute at choose time — build_subst). A malformed
	// entry (not an object, no name/cmd) skips.
	var cmds = proj["commands"];
	if ( !cmds.is_null() )
	{
	    for ( var c : cmds )
	    {
		if ( !c.is_object() )
		    continue;
		var cn = c["name"];
		var cc = c["cmd"];
		if ( cn.is_null() || cc.is_null() )
		    continue;
		var r;
		r["name"] = cn;
		r["cmd"] = cc;
		var cm = c["mode"];
		if ( cm.is_null() )
		    r["mode"] = "capture";
		else
		    r["mode"] = cm;
		r["act"] = "bld-run-cmd";
		rows.push(r);
	    }
	}
	ui::set(w, es, "buildcmds", rows);
	return;
    }
    var r1;
    r1["name"] = "Check (in-process)";
    r1["act"] = "bld-check";
    rows.push(r1);
    var r2;
    r2["name"] = "Build (in-process)";
    r2["kind"] = "exe";
    r2["out"] = "{base}";
    r2["act"] = "bld-native";
    rows.push(r2);
    var r3;
    r3["name"] = "Build object (in-process)";
    r3["kind"] = "obj";
    r3["out"] = "{base}.o";
    r3["act"] = "bld-native";
    rows.push(r3);
    var r4;
    r4["name"] = "Run";
    r4["act"] = "bld-run";	// fork the LIVE parse (madc::parse_run)
    rows.push(r4);
    var r5;
    r5["name"] = "Run native";
    r5["cmd"] = "./{base}";
    r5["mode"] = "terminal";
    r5["act"] = "bld-run-cmd";
    rows.push(r5);
    var r6;
    r6["name"] = "Stop";
    r6["act"] = "bld-stop";
    rows.push(r6);
    ui::set(w, es, "buildcmds", rows);
}

// One choose dispatcher for the build pane: the row (by 1-based option)
// carries everything — act, command template, mode. Data all the way.
void build_choose(long w, long es, long doc, long option)
{
    var rows;
    ui::get(rows, w, es, "buildcmds");
    long idx = option - 1;
    if ( idx < 0 || idx >= php::count(rows) )
	return;
    var r = rows[idx];
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
    // The project rows (^B correlation): check every TU / build the
    // linked image / run the --project lane.
    if ( a == "bld-proj-check" )
    {
	proj_run_check(w, es, doc);
	return;
    }
    if ( a == "bld-proj-build" )
    {
	var k = format("{}", r["kind"]);
	var ot = format("{}", r["out"]);
	start_project_build(w, es, doc, k, ot);
	return;
    }
    if ( a == "bld-proj-run" )
    {
	run_project(w, es, doc);
	return;
    }
    var cmd0 = format("{}", r["cmd"]);
    var cmd;
    build_subst(cmd, cmd0.c_str(), w, es, doc);
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
    var vw;
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
// The shell request: the client suspends the grid, term_exec runs the
// user's shell on the real tty, the client resumes into a full repaint.
// No return-key pause — leaving the shell IS the gesture.
void open_shell(long w, long es)
{
    var rq;
    rq["kind"] = "shell";
    rq["pause"] = 0;
    if ( !term_request(w, es, rq) )
	ui::set(w, es, "msg", "No terminal to suspend.");
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

// Apply ONE semantic event (the value shapes the engine's event query
// produces — action events from the bindings table included) — the
// session surface. False = quit.
bool IdeSession::apply_ide_event(long doc, var &ev)
{
    // The startup hint yields to the first event of any kind — the same
    // "first event returns the row" rule run_ide's loop used to apply.
    ui::set(w, es, "hint", 0);
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

    if ( project_active(w, es) )
	return project_event(w, es, doc, ev);

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
	if ( a == "project" )
	{
	    proj_show(w, es, doc);
	    return true;
	}
	if ( a == "projaddcur" )
	{
	    proj_addcur(w, es, doc);
	    return true;
	}
	if ( a == "refresh" )
	{
	    // JOE's ^R retype: the terminal is lying — a hard repaint is
	    // the CLIENT's to perform (it owns refresh); the session only
	    // parks the request. Headless sessions have no terminal
	    // pushed; the action is safely inert there, as it always was.
	    var rq;
	    rq["kind"] = "refresh";
	    rq["pause"] = 0;
	    term_request(w, es, rq);
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
	var ke;
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

    // OWNER RULING (IDE-10c): backing out of ANY pane — help, options,
    // outline, diags, build alike — is @pane profile data (the baked
    // default: esc paneclose). The project window and prompts cancel in
    // their own scoped arms above (they route before this one).
    var pane;
    ui::get(pane, w, es, "pane");
    if ( strlen(pane.c_str()) > 0 )
    {
	var pa;
	if ( scoped_action(pa, w, es, "pane", ev) && pa == "paneclose" )
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
void IdeSession::compose_ide_tree(var &root, long doc)
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
    var ed;
    if ( viewing )
    {
	var vw;
	view_name(vw, w, es);
	var ed2;
	compose_edit_node(ed2, w, es, nav, caret);
	var h;
	h = ed2["hints"];
	h["view"] = vw;
	var vm;
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
	var hs;
	ui::get(hs, w, es, "hspans");
	if ( !hs.is_null() && php::count(hs) > 0 )
	{
	    var h2;
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
	var ht;
	ht = ed["hints"];
	ht["tabwidth"] = tabw;
	ed["hints"] = ht;
    }

    // The status line's enclosing-function seat (%x, AST-1): a RETAINED-
    // state read of the buffer's parse handle — stored-space only (a
    // view's caret is display-space), and only once something opened the
    // handle; composition never runs the compiler.
    var fnctx = "";
    var vw_st;
    if ( !view_name(vw_st, w, es) )
    {
	long ph = es_int(w, doc, "phandle", 0);
	if ( ph != 0 )
	{
	    long cl = line_of(w, doc, caret);
	    long cs = line_start_of(w, doc, cl);
	    var encl;
	    madc::parse_enclosing(encl, ph, cl, caret - cs);
	    if ( !encl.is_null() )
		fnctx = format("fn {}", encl["name"]);
	}
    }
    var status;
    compose_joe_statusline(status, w, es, nav, caret, fnctx);

    var r;
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
	long trows = es_int(w, es, "vrows", 24);	// client viewport
	long base = (trows - wc) / wc;
	if ( base < window_min_rows() )
	    base = window_min_rows();
	long at = es_int(w, es, "winat", 0);
	var ws;
	ui::get(ws, w, es, "windows");
	var bufs;
	ui::get(bufs, w, es, "buffers");
	var hf;
	hf = ed["hints"];
	hf["focus"] = 1;
	ed["hints"] = hf;
	long i = 0;
	for ( var wr : ws )
	{
	    if ( i == at )
	    {
		kids.push(status);
		kids.push(ed);
	    }
	    else
	    {
		var brow = bufs[wr["bufidx"].as_integer()];
		long wdoc = brow["doc"].as_integer();
		long wcaret = clamp_caret_to(w, wdoc,
					     wr["caret"].as_integer());
		var wfn = "";
		var wst;
		compose_joe_statusline(wst, w, es, wdoc, wcaret, wfn);
		kids.push(wst);
		var wed;
		wed["role"] = "edit";
		var wt;
		ui::text(wt, w, wdoc);
		wed["content"] = wt;
		var wh;
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
	var box;
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
		var line;
		if ( !profile_line(line, lines, i) )
		    continue;
		var it;
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
	    var o1;
	    o1["content"] = format("D Tab width   {}", tw);
	    o1["act"] = "opt-tabwidth";
	    rows.push(o1);
	    var o2;
	    o2["content"] = format("S Scheme      {}", scheme);
	    o2["act"] = "opt-scheme";
	    rows.push(o2);
	    var o3;
	    o3["content"] = format("K Keymap      {}", prof);
	    o3["act"] = "opt-keymap";
	    rows.push(o3);
	    for ( var row : rows )
	    {
		var it;
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
	    var brows;
	    ui::get(brows, w, es, "buildcmds");
	    box["label"] = es_int(w, es, "buildlive", 0) != 0
		? "Build — running" : "Build";
	    var bh;
	    bh["list"] = 1;
	    bh["focus"] = 1;
	    box["hints"] = bh;
	    for ( var row : brows )
	    {
		var it;
		it["role"] = "item";
		var c = row["cmd"];
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
	else if ( pane == "project" )
	{
	    // IDE-10a ^P — the PROJECT window (owner rulings 2026-08-31):
	    // the LIST-presentation choice — model hints list (one row
	    // per option) + focus (arrows/enter land here without a
	    // tab). The label carries the manifest (or its absence) and
	    // the live filter; rows come precomposed from project_rows
	    // in groups ([project] -> [open] -> [cwd] while filtering).
	    // Rows carry NO actions: enter's meaning is @project data
	    // (the choose arm looks it up), never row-baked.
	    var filt;
	    ui::get(filt, w, es, "projfilter");
	    var pf;
	    ui::get(pf, w, es, "projfile");
	    var pfd = "(no project file)";
	    if ( !pf.is_null() && strlen(pf.c_str()) > 0 )
		pfd = pf;
	    var prows;
	    ui::get(prows, w, es, "projrows");
	    box["label"] = format("Project {}: {}_", pfd, filt);
	    var ph;
	    ph["list"] = 1;
	    ph["focus"] = 1;
	    box["hints"] = ph;
	    for ( var row : prows )
	    {
		var it;
		it["role"] = "item";
		it["content"] = row["content"];
		items.push(it);
		nrows = nrows + 1;
	    }
	    if ( nrows == 0 )
	    {
		var it;
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
	    var rows;
	    if ( outline_pane )
		ui::get(rows, w, es, "outline");
	    else
		ui::get(rows, w, es, "diags");
	    if ( outline_pane )
		box["label"] = "Outline";
	    else
		box["label"] = "Diagnostics";
	    var dpath;
	    ui::get(dpath, w, doc, "path");
	    for ( var row : rows )
	    {
		var it;
		it["role"] = "item";
		if ( outline_pane )
		    it["content"] = format("{} {} :{}", row["kind"],
					   row["name"], row["line"]);
		else if ( !row["file"].is_null()
		       && !(row["file"] == dpath.c_str()) )
		    // A project check's rows span TUs — a row from
		    // ANOTHER file leads with it (goto opens it; the
		    // row carries the file as data either way). Rows on
		    // the shown buffer stay unprefixed (the JOE shape).
		    it["content"] = format("{} {}:{} {} {}", row["file"],
					   row["line"], row["column"],
					   row["severity"], row["message"]);
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
		var it;
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
    var ovl;
    if ( compose_overlay_row(ovl, w, es) )
	kids.push(ovl);
    else if ( es_int(w, es, "hint", 0) != 0 )
    {
	var hn;
	hn["role"] = "content";
	hn["content"] =
	    " madcide ** ^K Q exits / ^K H shows the loaded profile's help **";
	kids.push(hn);
    }
    r["children"] = kids;
    root = r;
}

// ---- the session lifecycle (the gateway seam) ---------------------------

// Full session startup: the world, the launch document, the data loads
// (status format, theme, keybinding profile — all validated handle-free),
// the buffer table, the project read-back, and the first lexical colour.
// No terminal anywhere: the TUI client opens its own AFTER this and asks
// for the key table. > 0 = the launch doc; 0 = unreadable path (the
// caller reports); -1 = fatal, already reported on stderr.
long IdeSession::open(const char *path0, bool ro)
{
    var p = path0;		// ring discipline: own the text first
    w = ui::world_new();
    long doc = setup_editor(w, p.c_str(), ro);
    if ( doc == 0 )
	return 0;
    es = ui::entity_by_name(w, "editor-state");
    // The profiles live beside THIS file: derive the directory from
    // __FILE__ (the include-resolved path), so madcide works from any
    // cwd. php::dirname owns the split — on a WINDOWS host '\' separates
    // too (a Windows launch spells the source path with '\'; splitting
    // on '/' alone cost the first genuine-Win validation its keybindings,
    // and every chord is profile data). No separator answers ".".
    ui::set(w, es, "profile_dir",
	    format("{}/profiles", php::dirname(__FILE__)));
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
	    println(stderr, "madcide: cannot load the startup profile {}/joe.keys and the built-in rescue bindings failed to install — check the install layout (tools/madcide/profiles beside madcide.mad).", dir);
	    return -1;
	}
    }
    ui::set(w, es, "hint", 1);	// the transient bottom startup hint
    // The buffer table (AST-5): row 0 = the launch file; ^K E grows it.
    // ONE fresh-row owner (push_buffer_row — the gated dupaudit family).
    push_buffer_row(w, es, doc, p.c_str());
    ui::set(w, es, "bufidx", 0);
    // The project (owner rulings 2026-08-31): the launch file anchors the
    // implicit single-file project; a <base>.prj.json beside it IS the
    // project and loads back (announced on the status line).
    ui::set(w, es, "launchpath", p);
    proj_startup(w, es, doc);
    // PAINT BEFORE THE PARSE (owner report, 2026-08-26: parse-on-load
    // blocked the first render for seconds on a real C++ file): colour
    // rides the first paint from the LEXICAL spans; the full parse runs
    // as the client loop's pending step AFTER a render — the ONE
    // mechanism startup and buffer switches share (parse_pending).
    lex_first_spans(w, es, doc);
    ui::set(w, es, "pending_parse", 1);
    return doc;
}

// A session over an EXISTING world + state entity: the headless harness
// (tests/testmadcide.mad) and embedders seed their own documents and
// drive the surface directly.
void IdeSession::attach(long w0, long es0)
{
    w = w0;
    es = es0;
}

long IdeSession::world()
{
    return w;
}

long IdeSession::active(long fallback)
{
    return active_doc(w, es, fallback);
}

// The paint-before-parse step: when a parse is parked (startup, buffer
// switch), spawn it as a cooperative task AFTER the client rendered.
// True = spawned — the client keeps serving keystrokes while it runs;
// its completion surfaces as a `wake` event (or rides the next
// keystroke's recompose).
bool IdeSession::parse_pending(long adoc)
{
    if ( es_int(w, es, "pending_parse", 0) == 0 )
	return false;
    ui::set(w, es, "pending_parse", 0);
    go background_parse(w, es, adoc);
    return true;
}

// ---- client-pushed terminal facts. The session never touches a tui
// handle; what a terminal knows arrives as data.
void IdeSession::viewport(long cols, long rows)
{
    ui::set(w, es, "vcols", cols);
    ui::set(w, es, "vrows", rows);
}

void IdeSession::terminal(bool has)
{
    ui::set(w, es, "hasterm", has);
}

void IdeSession::pending_chord(const char *pcs)
{
    var pc = pcs;		// ring discipline: own the text first
    ui::set(w, es, "pendingchord", pc);
}

// ---- the key-table seam: the session validates and stores the main
// bindings as data; the client installs them whenever the generation
// moves (profile load, ^K P cycle, rescue fallback).
long IdeSession::keys_generation()
{
    return es_int(w, es, "keysgen", 0);
}

void IdeSession::key_table(var &out)
{
    ui::get(out, w, es, "keytable");
}

// ---- the terminal-request drain: the client takes the one parked
// request after each event, suspends, calls term_exec to run the guest,
// pauses (per the request), resumes.
bool IdeSession::take_request(var &out)
{
    var rq;
    ui::get(rq, w, es, "termreq");
    if ( rq.is_null() )
	return false;
    var none;
    ui::set(w, es, "termreq", none);
    out = rq;
    return true;
}

// Run the parked guest — the session half of the terminal handoff (the
// verdicts already ran when the request parked; nothing intervened, the
// drain follows the event immediately). The run kinds report their
// exit on the status line; command/shell output speaks for itself.
void IdeSession::term_exec(var &req)
{
    // The guest writes to the SAME fd 1: flush the host's buffered
    // stdout first, or a block-buffered run (piped/headless) splices the
    // guest's output into the middle of a host line.
    fflush(stdout);
    var kind = format("{}", req["kind"]);
    if ( kind == "run" )
    {
	// Fork the buffer's LIVE parse (owner ruling 2026-08-27: the
	// running madc IS the compiler) — the child inherits the tree.
	long doc = req["doc"].as_integer();
	long h = es_int(w, doc, "phandle", 0);
	if ( h == 0 )
	    return;
	long rc = madc::parse_run(h);
	if ( rc < 0 )
	    ui::set(w, es, "msg", format("Run failed (rc {}).", rc));
	else
	    ui::set(w, es, "msg", format("Run finished (exit {}).", rc));
	return;
    }
    if ( kind == "projrun" )
    {
	// The --project JIT lane in a fork child (win: a child of self).
	var pf;
	ui::get(pf, w, es, "projfile");
	long rc = madc::project_run(pf.c_str());
	if ( rc < 0 )
	    ui::set(w, es, "msg", format("Run failed (rc {}).", rc));
	else
	    ui::set(w, es, "msg", format("Run finished (exit {}).", rc));
	return;
    }
    if ( kind == "cmd" )
    {
	var c = format("{}", req["cmd"]);
	system(c.c_str());
	return;
    }
    if ( kind == "shell" )
    {
	// $SHELL falls back to /bin/sh INSIDE the command — libc system()
	// inherits the terminal, and no getenv is needed (the dlsym
	// fallback would hand the pointer back as a long).
	system("exec \"${SHELL:-/bin/sh}\"");
    }
}

// Session teardown. Stage-2: a spawned parse may still be mid-flight at
// quit — drain the task scope BEFORE closing the handles and the world
// it writes to. A live build pump gets its stop FIRST (IDE-10b): the
// send fires its select case, it SIGTERMs the child and exits — the
// drain never hangs on it.
void IdeSession::close()
{
    if ( es_int(w, es, "buildlive", 0) != 0 )
    {
	long stopc = es_int(w, es, "buildstop", 0);
	if ( stopc != 0 )
	{
	    var sv;
	    sv = 1;
	    madc::chan_send(stopc, sv);
	}
    }
    madc::task_drain();
    // Every buffer's parse handle closes with the session.
    var bufs;
    ui::get(bufs, w, es, "buffers");
    for ( var b : bufs )
    {
	long bd = b["doc"].as_integer();
	long ph = es_int(w, bd, "phandle", 0);
	if ( ph != 0 )
	    madc::parse_close(ph);
    }
    ui::world_close(w);
}
