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

#include "madcide_enums.inc"	// ide_cmd + the state discriminators, as codes

// The bag's discriminator slots — ONE reader and ONE writer each (V0.5d):
// integers on the bag, an enumerator at every site, a name only where text
// leaves the program (pane_name & co. in madcide_enums.inc).
long cur_pane(long w, long es)		{ return es_int(w, es, "pane", paneNONE); }
void set_pane(long w, long es, long p)	{ ui::set(w, es, "pane", p); }
long cur_pmode(long w, long es)		{ return es_int(w, es, "pmode", pmNONE); }
void set_pmode(long w, long es, long m)	{ ui::set(w, es, "pmode", m); }
long cur_vimode(long w, long es)	{ return es_int(w, es, "vimode", viOFF); }
void set_vimode(long w, long es, long m) { ui::set(w, es, "vimode", m); }

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
	long spawn_view(long doc);	// a NEW client view over the shared
					// world + `doc` (viewwindow): a fresh
					// es built like open()'s, returned to
					// the client to open a frontend on;
					// 0 = the profile + rescue both failed
	bool apply_ide_event(long doc, var &ev);	// false = quit
	void pointer_event(long doc, long nav, var &ev);	// the pointer
					// arm: the hit node's window tag
					// (else its subject) picks the
					// window, edit_pointer places
	void compose_ide_tree(var &root, long doc);
	bool vi_event(long doc, long nav, var &ev, bool &cont);
					// the vi modes-as-scopes arm:
					// true = consumed; cont carries
					// the continue/quit verdict
	bool vi_exec(long doc, long code, long n);	// one command (its
					// code), n times, through
					// apply_ide_event
	bool post(long doc, long code);	// ONE command by CODE through the
					// one dispatcher (a client that
					// speaks codes); false = quit
	bool command(long doc, long code, const char *arg, bool &cont);
					// a command WITH ITS ARGUMENT
					// (the -c line, the api seat, a
					// `:` verb): the argument is what
					// the prompt the command opens
					// would have been typed; false =
					// refused (an argument the command
					// does not take); cont = the
					// continue / quit verdict
	bool line_input(long doc, const char *line, bool &cont);
					// ONE line of the ex / edlin client
					// (ui::LINE, V2.5): a ':' line is a
					// colon command (posted through
					// command(cmdCOLON, …)); any other
					// line is text inserted verbatim +
					// a newline. false = refused; cont =
					// the continue / quit verdict
	long error_count();		// ERROR rows in the problems
					// projection (a one-shot's verdict)
	void vi_open_line(long doc, bool above);	// o / O / cc's line
	long world();
	long active(long fallback);	// the active buffer's document
	bool parse_pending(long adoc);	// spawn the parked background
					// parse; true = spawned (client
					// keeps serving input)
	void viewport(long cols, long rows);	// client-pushed facts
	void terminal(bool has);
	void dialogs(bool has);		// the client can show native file
					// dialogs (S4): open / save-as park a
					// filedialog request instead of a prompt
	void panel(bool has);		// the client renders REGIONS (a
					// window): runs and the shell go to a
					// pty in the Terminal tab; the panel
					// itself is the layout's (V2)
	void dialog_fallback(var &req);	// the client could not show it:
					// the request's prompt instead
	void request_refused(var &req);	// the client cannot hand over a
					// terminal for this request: say so
	void pending_chord(const char *pcs);
	long keys_generation();		// moves when the key table changes
	void key_table(var &out);
	bool take_request(var &out);	// drain the parked terminal request
	bool take_spawn(long &doc);	// drain a parked viewwindow spawn
					// (the doc the new window views);
					// false = none
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

// The persisted layout sits BESIDE the manifest (V2c, design §5): the
// manifest path minus its extension, plus `.layout` — `foo.prj.json` ->
// `foo.prj.layout`, `myproj.json` -> `myproj.layout`. One derivation for
// the save (layout_persist) and the load (proj_load), so both name the
// same file.
void proj_layout_path(var &out, var &pf)
{
    var base;
    path_sans_ext(base, pf);
    out = format("{}.layout", base);
}

// Where the profile/theme/status data lives (packaging arc PK7): the
// source tree keeps profiles beside THIS file (__FILE__ is the
// include-resolved path — from any cwd, and php::dirname splits both
// separators for a Windows launch). An INSTALLED madcide is an AOT
// binary whose __FILE__ names a build tree that box never had, so the
// running binary's own directory (madc::compiler_path — the process's
// resolved executable, madcide itself in the AOT shape) anchors the
// install layouts: <exedir>/../share/madcide/profiles (the Linux/mac
// package layout) then <exedir>/profiles (the Windows zip: data beside
// the exe). First existing directory wins; nothing found keeps the
// source-tree spelling so the rescue report names a real place to fix.
void resolve_profile_dir(var &dir)
{
    dir = format("{}/profiles", php::dirname(__FILE__));
    if ( php::file_exists(dir) )
	return;
    var exedir = php::dirname(madc::compiler_path());
    var share = format("{}/../share/madcide/profiles", exedir);
    if ( php::file_exists(share) )
    {
	dir = share;
	return;
    }
    var beside = format("{}/profiles", exedir);
    if ( php::file_exists(beside) )
	dir = beside;
}

// One binding per line: "SEQ ACTION" — the action is the LAST word, the
// sequence everything before it. A leading "@name" token SCOPES the
// binding to that pane/modal widget (the owner keybinding law: pane and
// modal keys are profile DATA through this ONE parser, never a hardwired
// dispatch arm): "@confirm y pyes" binds y inside the confirm question.
// Unscoped lines fill `table` (the engine's main bindings); scoped lines
// fill `scopes` ({scope: {key: action}}, keys stored AS WRITTEN — the
// scoped alphabet is single key/character spellings; a lowercase
// spelling matches either case at lookup (scoped_action), spelling BOTH
// cases is what makes a scope case-sensitive: the vi @normal alphabet's
// a/A, i/I, o/O). Later lines overwrite earlier per (scope, key), which
// is how the baked defaults merge under a profile's own respellings.
// The ONE scoped-line rule (`@scope name value`): scope is the word after
// '@' up to the first space; value is the LAST space-separated token; name
// is everything between. False = not a scoped line, or malformed. Shared by
// parse_keys (@scope key -> action, the modal key tables) and load_theme
// (@gui prop -> CSS value) so the two sections cannot drift — later lines
// win at the caller. profile_line already stripped comments/blanks.
bool scope_line_parts(var &line, var &scope, var &name, var &value)
{
    const char *ls = line.c_str();
    if ( ls[0] != '@' )
	return false;
    const char *sp1 = strchr(ls, ' ');
    if ( sp1 == 0 )
	return false;
    scope = perl::substr(line, 1, sp1 - ls - 1);
    var rest = php::trim(sp1 + 1);
    const char *rs = rest.c_str();
    const char *sp2 = strrchr(rs, ' ');
    if ( sp2 == 0 )
	return false;
    value = php::trim(sp2 + 1);
    name = php::trim(perl::substr(rest, 0, sp2 - rs));
    return true;
}

// Action NAMES resolve to their command CODES here (owner law 2026-09-09:
// enums, not strings) — the profile text is the input boundary: the tables
// hold ide_cmd codes (the engine binds a code-valued table as codes; the
// scoped tables' values are codes), and an unknown name REFUSES the whole
// text — false, `err` naming the line and the word — so a misspelt binding
// is a load-time error the status line reports, never a dead key.
bool parse_keys(var &table, var &scopes, const char *text, var &err)
{
    var t;
    var sc = scopes;		// accumulate over what the caller seeded
    var ct;
    cmd_table(ct);
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
	    var scope;
	    var key;
	    var action;
	    if ( !scope_line_parts(line, scope, key, action) )
		continue;
	    long code = cmd_of_in(ct, action.c_str());
	    if ( code == cmdNONE )
	    {
		err = format("line {}: unknown action '{}'", i + 1, action);
		return false;
	    }
	    var one = sc[scope.c_str()];
	    if ( one.is_null() )
	    {
		var fresh;
		fresh[key.c_str()] = code;
		sc[scope.c_str()] = fresh;
	    }
	    else
	    {
		one[key.c_str()] = code;
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
	long code = cmd_of_in(ct, action.c_str());
	if ( code == cmdNONE )
	{
	    err = format("line {}: unknown action '{}'", i + 1, action);
	    return false;
	}
	t[seq.c_str()] = code;
    }
    table = t;
    scopes = sc;
    return true;
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
	   "@prompt ^c pcancel\n"
	   "@prompt backspace pdelchar\n"
	   "@confirm y pyes\n"
	   "@confirm n pcancel\n"
	   "@confirm esc pcancel\n"
	   "@project esc projclose\n"
	   "@project backspace projdelchar\n"
	   "@project ins projadd\n"
	   "@project del projdel\n"
	   "@project enter projopen\n"
	   "@modes : colon\n"
	   "@modes v vimode\n"
	   "@modes enter modesopen\n"
	   "@modes esc modesclose\n"
	   "@pane esc paneclose\n"
	   "@terminal ^] termunfocus\n";
}

// Modal key->action lookup over the bag's "modekeys" (the @scope tables).
// Key events look up by key name, text events by their single character,
// ACTION events by the sequence that completed (`seq`): a chord the global
// table binds still reaches a modal scope that binds the same spelling —
// the modal owns its keys while it is up (JOE's ^C aborts every prompt
// though ^C is `discard` in the JOE profile: "@prompt ^c pcancel" wins).
// The key matches AS WRITTEN first, then by its lowercase spelling — so
// a scope whose data is all lowercase stays case-insensitive (the JOE
// convention, every modal scope), and one spelling both cases is
// case-sensitive (the vi @normal alphabet). Which event KINDS consult a
// scope is that modal's semantics (a confirm reads characters, a
// prompt's characters are its input) — the caller picks. False = the
// scope binds nothing for this event. Sessions that never loaded a
// profile (the headless harness) lazily seed the baked defaults.
// The modal @scope tables — a loaded profile's, else the baked defaults
// seeded lazily (sessions that never loaded a profile: the headless
// harness). ONE reader for every consumer (the lookup below, the confirm
// dialog's choices).
void modal_keys(var &mk, long w, long es)
{
    ui::get(mk, w, es, "modekeys");
    if ( mk.is_null() )
    {
	var t0;
	var err;
	if ( !parse_keys(t0, mk, modal_default_keys_text(), err) )
	    ui::set(w, es, "msg", format("Baked modal keys rejected: {}.", err));
	ui::set(w, es, "modekeys", mk);
    }
}

bool scoped_action(var &out, long w, long es, const char *scope, var &ev)
{
    var mk;
    modal_keys(mk, w, es);
    var sc = mk[scope];
    if ( sc.is_null() )
	return false;
    var kind = ev["event_code"];
    var k = "";
    if ( kind == ui::event_kind::key )
	k = format("{}", ev["key"]);
    else if ( kind == ui::event_kind::text )
    {
	var t = format("{}", ev["text"]);
	if ( strlen(t.c_str()) != 1 )
	    return false;
	k = t;
    }
    else if ( kind == ui::event_kind::action )
    {
	if ( ev["seq"].is_null() )
	    return false;		// fired by name (a menu, a button): no keys
	k = format("{}", ev["seq"]);
	if ( strlen(k.c_str()) == 0 )
	    return false;
    }
    else
	return false;
    var a = sc[k.c_str()];		// as written (the vi case alphabet)
    if ( a.is_null() )
    {
	var lk = php::strtolower(k.c_str());
	a = sc[lk.c_str()];		// lowercase spellings match either case
    }
    if ( a.is_null() )
	return false;
    out = a;
    return true;
}

// The ACTION twin of scoped_action: a client fired one of a scope's
// actions by NAME — a dialog button, a popup's dismissal (the menu-bar
// rule: native chrome posts the action id, never a key). Accepted only
// when the scope binds that name to some key, so a button can reach
// exactly what a key can; any other action is not the scope's. False =
// not an action event, or not this scope's.
bool scope_action_named(var &out, long w, long es, const char *scope,
			var &ev)
{
    var kind = ev["event_code"];
    if ( !(kind == ui::event_kind::action) )
	return false;
    var mk;
    modal_keys(mk, w, es);
    var sc = mk[scope];
    if ( sc.is_null() )
	return false;
    var code = ev["action_code"];
    if ( code.is_null() || code.as_integer() == cmdNONE )
	return false;
    for ( var a : sc )
	if ( a == code )
	{
	    out = code;
	    return true;
	}
    return false;
}

// The vi mode setter (defined with the vi machinery below — the loaders
// need it: a modal personality starts in normal mode).
void vi_enter(long w, long es, long mode);

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
    // own @scope line; one that says nothing keeps the defaults. Parsed
    // FIRST, the profile over them — so a refusal names the PROFILE's own
    // line: "Profile 'x' line 12: unknown action 'svae'."
    var full = format("{}\n{}", modal_default_keys_text(), txt);
    var table;
    var scopes;
    var err;
    parse_keys(table, scopes, modal_default_keys_text(), err);
    if ( !parse_keys(table, scopes, txt.c_str(), err) )
    {
	ui::set(w, es, "msg", format("Profile '{}' {}.", pname, err));
	return false;
    }
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
	if ( a == cmdBEND )
	    twopoint = true;
    ui::set(w, es, "selblock", twopoint);
    // A profile that carries @normal key data IS a modal personality
    // (the vi modes-as-scopes rule): it loads in vi normal mode; any
    // other profile clears vi mode — the tables its arm routes through
    // are gone. Data decides, never the profile's name.
    var vt = scopes["normal"];
    vi_enter(w, es, vt.is_null() ? viOFF : viNORMAL);
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
    var err;
    parse_keys(table, scopes, modal_default_keys_text(), err);
    if ( !parse_keys(table, scopes, rescue_keys_text(), err) )
	return false;
    if ( !ui::tui_validate_keys(table) )
	return false;
    ui::set(w, es, "keytable", table);
    ui::set(w, es, "keysgen", es_int(w, es, "keysgen", 0) + 1);
    ui::set(w, es, "modekeys", scopes);	// prompts stay usable in rescue
    ui::set(w, es, "selblock", false);	// no bend in the rescue set
    vi_enter(w, es, viOFF);		// no @normal data in the rescue set
    ui::set(w, es, "profile", "rescue");
    ui::set(w, es, "bindtext", full);
    ui::set(w, es, "msg",
	    "No keybinding profile found — RESCUE keys: ^S save  ^Q quit  ^X discard  ^G help  ^P profile");
    return true;
}

// ---- layouts as data (client-server arc V2; design doc §2.2; plan
//      2026-09-09-v2-containers-layouts-plan.md) ----------------------------
// A LAYOUT is a tree of containers holding Views, owned by the client and
// stored on the bag as "layout":
//   { "window": name, "editor": <node>, "chrome": [ <pane>... ] }
//   pane  { "kind": ctPANE, "slot", "side", "size", "mode", "tabs": [ <tab>... ], "active": 0, "focus", "hidden" }
//   tab   = layout_tab(kind, view): the CONTAINER — its View and its own navigation (caret, mark, bend, grow)
//   split { "kind": ctSPLIT, "dir": ui::split, "size", "children": [ <node>... ] }
// The `.layout` text is the input boundary (profiles/default.layout, the
// grammar in its header): every word converts ONCE here — slot_of, pmode_of,
// view_of, ui::side_code, ui::split_code — and an unknown word REFUSES the
// text with its line; a ragged indentation, a chrome pane inside a split, a
// pane with children, a split with fewer than two children, two editor
// regions or two windows refuse the same way (parse_keys' shape). The baked
// default rides the same parser (the rescue-keys pattern).

// THE layout-tab row shape, one owner (the single-owners gate counts the
// container-navigation shape: this, push_buffer_row and edit_file's seed): a
// fresh CONTAINER over a View of `kind` — its navigation starts at the top
// with no block (design §6 Q1: caret / mark / bend live on the container).
void layout_tab(var &out, long kind, long view)
{
    out = { "kind": kind, "view": view, "caret": 0, "mark": -1, "bend": -1,
	    "grow": 0 };
}

// Split a trimmed line into its whitespace-separated words.
void layout_words(var &out, var &line)
{
    var words = {};
    const char *ls = line.c_str();
    long n = strlen(ls);
    long start = -1;
    for ( long i = 0; i <= n; i = i + 1 )
    {
	bool sep = i == n || ls[i] == ' ' || ls[i] == '\t';
	if ( !sep && start < 0 )
	    start = i;
	else if ( sep && start >= 0 )
	{
	    var wd = perl::substr(line, start, i - start);
	    words.push(wd);
	    start = -1;
	}
    }
    out = words;
}

// A size word "NN%" -> NN (1..90); -1 when the word is not a size.
long layout_size_of(var &word)
{
    const char *ws = word.c_str();
    long n = strlen(ws);
    if ( n < 2 || ws[n - 1] != '%' )
	return -1;
    for ( long i = 0; i < n - 1; i = i + 1 )
	if ( ws[i] < '0' || ws[i] > '9' )
	    return -1;
    long v = php::intval(perl::substr(word, 0, n - 1));
    if ( v < 1 || v > 90 )
	return -1;
    return v;
}

// The leading whitespace of a raw line (its nesting depth).
long layout_depth_of(var &raw)
{
    const char *rs = raw.c_str();
    long d = 0;
    while ( rs[d] == ' ' || rs[d] == '\t' )
	d = d + 1;
    return d;
}

// One `pane` line's words (after the keyword) into a flat row. False + err.
bool layout_pane_row(var &row, var &words, long lineno, var &err)
{
    long nw = php::count(words);
    if ( nw < 2 )
    {
	err = format("line {}: pane without a slot", lineno);
	return false;
    }
    var sw = words[1];
    long slot = slot_of(sw);
    if ( slot == slotNONE || slot == slotPOPUP )
    {
	err = format("line {}: unknown slot '{}'", lineno, sw);
	return false;
    }
    long side = ui::side_code("");
    long size = 0;
    long mode = lmNONE;
    bool collecting = false;
    bool focus = false;
    bool hidden = false;
    var tabs = {};
    for ( long i = 2; i < nw; i = i + 1 )
    {
	var wd = words[i];
	if ( wd == "views" )
	{
	    collecting = true;
	    continue;
	}
	if ( wd == "focus" )
	{
	    focus = true;
	    collecting = false;
	    continue;
	}
	if ( wd == "hidden" )
	{
	    hidden = true;
	    collecting = false;
	    continue;
	}
	if ( collecting )
	{
	    long k = view_of(wd);
	    if ( k == viewNONE )
	    {
		err = format("line {}: unknown view '{}'", lineno, wd);
		return false;
	    }
	    var tb;
	    layout_tab(tb, k, 0);
	    tabs.push(tb);
	    continue;
	}
	long sz = layout_size_of(wd);
	if ( sz > 0 )
	{
	    size = sz;
	    continue;
	}
	const char *wds = wd.c_str();
	if ( strlen(wds) > 1 && wds[strlen(wds) - 1] == '%' )
	{
	    err = format("line {}: bad size '{}' (1%..90%)", lineno, wd);
	    return false;
	}
	long sd = ui::side_code(wd.c_str());
	if ( sd != ui::side_code("") )
	{
	    side = sd;
	    continue;
	}
	long pm = pmode_of(wd);
	if ( pm != lmNONE )
	{
	    mode = pm;
	    continue;
	}
	err = format("line {}: unknown word '{}'", lineno, wd);
	return false;
    }
    if ( php::count(tabs) == 0 )
    {
	err = format("line {}: pane without views", lineno);
	return false;
    }
    // The slot's side rule: a sidebar docks left or right, a panel bottom or
    // top, the editor region has no side.
    var sidev = side;
    bool sideok = true;
    if ( slot == slotSIDEBAR )
	sideok = sidev == ui::side::left || sidev == ui::side::right;
    else if ( slot == slotPANEL )
	sideok = sidev == ui::side::bottom || sidev == ui::side::top;
    else
	sideok = sidev == ui::side::none;
    if ( !sideok )
    {
	err = format("line {}: a {} pane cannot dock '{}'", lineno,
		     slot_name(slot), ui::side_name(side));
	return false;
    }
    if ( size == 0 && slot == slotSIDEBAR )
	size = 20;
    if ( size == 0 && slot == slotPANEL )
	size = 25;
    if ( mode == lmNONE )
	mode = lmTABS;
    long f = focus ? 1 : 0;
    long h = hidden ? 1 : 0;
    row = { "kind": ctPANE, "slot": slot, "side": side, "size": size,
	    "mode": mode, "tabs": tabs, "active": 0, "focus": f, "hidden": h };
    return true;
}

// One `split` line's words into a flat row. False + err.
bool layout_split_row(var &row, var &words, long lineno, var &err)
{
    long nw = php::count(words);
    if ( nw < 2 )
    {
	err = format("line {}: split without a direction", lineno);
	return false;
    }
    var dw = words[1];
    long dir = ui::split_code(dw.c_str());
    if ( dir == ui::split_code("") )
    {
	err = format("line {}: unknown split direction '{}'", lineno, dw);
	return false;
    }
    long size = 0;
    for ( long i = 2; i < nw; i = i + 1 )
    {
	var wd = words[i];
	long sz = layout_size_of(wd);
	if ( sz <= 0 )
	{
	    err = format("line {}: unknown word '{}'", lineno, wd);
	    return false;
	}
	size = sz;
    }
    row = { "kind": ctSPLIT, "dir": dir, "size": size };
    return true;
}

// Build the tree node for flat row `idx`: a pane as is; a split with its
// children (the rows whose parent is idx, in order) built recursively.
void layout_build(var &node, var &rows, long idx)
{
    var r = rows[idx];
    if ( r["kind"] == ctSPLIT )
    {
	var kids = {};
	long n = php::count(rows);
	for ( long i = 0; i < n; i = i + 1 )
	{
	    var c = rows[i];
	    if ( c["parent"] == idx )
	    {
		var child;
		layout_build(child, rows, i);
		kids.push(child);
	    }
	}
	node = { "kind": ctSPLIT, "dir": r["dir"], "size": r["size"],
		 "children": kids };
	return;
    }
    node = { "kind": ctPANE, "slot": r["slot"], "side": r["side"],
	     "size": r["size"], "mode": r["mode"], "tabs": r["tabs"],
	     "active": r["active"], "focus": r["focus"],
	     "hidden": r["hidden"] };
}

// The parser: text -> the layout tree. False + err (the line and the word).
bool parse_layout(var &layout, const char *text, var &err)
{
    var window = "";
    var rows = {};		// flat: every pane / split with depth + parent
    var open = {};		// indices of the split rows still open
    array lines;
    php::explode(lines, "\n", text);
    long n = php::count(lines);
    for ( long i = 0; i < n; i = i + 1 )
    {
	var raw = php::array_get_cstr(lines, i);
	var line;
	if ( !profile_line(line, lines, i) )
	    continue;
	long lineno = i + 1;
	var words;
	layout_words(words, line);
	var kw = words[0];
	if ( kw == "@window" )
	{
	    if ( strlen(window.c_str()) > 0 )
	    {
		err = format("line {}: a second window (one window; V3 adds windows)", lineno);
		return false;
	    }
	    if ( php::count(words) < 2 )
	    {
		err = format("line {}: window without a name", lineno);
		return false;
	    }
	    window = words[1];
	    continue;
	}
	long ct = container_of(kw);
	if ( ct == ctNONE )
	{
	    err = format("line {}: unknown word '{}'", lineno, kw);
	    return false;
	}
	long depth = layout_depth_of(raw);
	// The open splits: a line at or above an open split's depth closes it.
	while ( php::count(open) > 0 )
	{
	    var top = rows[open[php::count(open) - 1].as_integer()];
	    if ( top["depth"].as_integer() < depth )
		break;
	    var shorter = {};
	    long no = php::count(open) - 1;
	    for ( long k = 0; k < no; k = k + 1 )
		shorter.push(open[k]);
	    open = shorter;
	}
	long parent = -1;
	if ( php::count(open) > 0 )
	{
	    parent = open[php::count(open) - 1].as_integer();
	    var pr = rows[parent];
	    long cd = pr["childdepth"].as_integer();
	    if ( cd == 0 )
	    {
		pr["childdepth"] = depth;
		rows[parent] = pr;
	    }
	    else if ( cd != depth )
	    {
		err = format("line {}: ragged layout", lineno);
		return false;
	    }
	}
	else if ( depth > 0 )
	{
	    err = format("line {}: ragged layout", lineno);
	    return false;
	}
	var row;
	if ( ct == ctPANE )
	{
	    if ( !layout_pane_row(row, words, lineno, err) )
		return false;
	    if ( parent >= 0 && row["slot"] != slotEDITOR )
	    {
		err = format("line {}: a {} pane never splits", lineno,
			     slot_name(row["slot"].as_integer()));
		return false;
	    }
	}
	else
	{
	    if ( !layout_split_row(row, words, lineno, err) )
		return false;
	}
	row["depth"] = depth;
	row["parent"] = parent;
	row["childdepth"] = 0;
	row["line"] = lineno;
	rows.push(row);
	if ( ct == ctSPLIT )
	    open.push(php::count(rows) - 1);
    }
    // Every split holds two or more children; exactly one editor region;
    // the chrome panes are top-level; one focus (the first editor pane when
    // none is named).
    long nr = php::count(rows);
    long editor = -1;
    long focused = 0;
    var chrome = {};
    for ( long i = 0; i < nr; i = i + 1 )
    {
	var r = rows[i];
	if ( r["kind"] == ctSPLIT )
	{
	    long kids = 0;
	    for ( long j = 0; j < nr; j = j + 1 )
	    {
		var c = rows[j];
		if ( c["parent"] == i )
		    kids = kids + 1;
	    }
	    if ( kids < 2 )
	    {
		err = format("line {}: a split needs two or more children", r["line"]);
		return false;
	    }
	}
	else if ( r["focus"].as_integer() != 0 )
	    focused = focused + 1;
	if ( r["parent"].as_integer() != -1 )
	    continue;
	if ( r["kind"] == ctSPLIT || r["slot"] == slotEDITOR )
	{
	    if ( editor >= 0 )
	    {
		err = format("line {}: a second editor region", r["line"]);
		return false;
	    }
	    editor = i;
	}
	else
	    chrome.push(i);
    }
    if ( editor < 0 )
    {
	err = "no editor region (a `pane editor` or a `split`)";
	return false;
    }
    if ( focused > 1 )
    {
	err = "more than one pane carries focus";
	return false;
    }
    if ( focused == 0 )
    {
	for ( long i = 0; i < nr; i = i + 1 )
	{
	    var r = rows[i];
	    if ( r["kind"] == ctPANE && r["slot"] == slotEDITOR )
	    {
		r["focus"] = 1;
		rows[i] = r;
		break;
	    }
	}
    }
    var ed;
    layout_build(ed, rows, editor);
    var ch = {};
    for ( var ci : chrome )
    {
	var cnode;
	layout_build(cnode, rows, ci.as_integer());
	ch.push(cnode);
    }
    if ( strlen(window.c_str()) == 0 )
	window = "main";
    layout = { "window": window, "editor": ed, "chrome": ch };
    return true;
}

// The baked default: the shipped profiles/default.layout as inline data
// through the SAME parser — what a missing or broken file falls back to.
const char *default_layout_text()
{
    return "# baked default layout — profile data\n"
	   "@window main\n"
	   "pane editor tabs views source focus\n"
	   "pane sidebar left 20% tabs views outline hidden\n"
	   "pane panel bottom 25% tabs views problems output terminal hidden\n";
}

// Load a named layout from the profile directory onto the bag ("layout");
// a missing or refused file falls back to the baked default and SAYS so on
// the status line (a layout must exist: the composer places by it). False =
// the fallback was taken.
bool load_layout(long w, long es, const char *name)
{
    var lname = name;		// ring discipline: own the text first
    var dir;
    ui::get(dir, w, es, "profile_dir");
    var txt;
    var layout;
    var err;
    bool ok = php::file_get_contents(txt, format("{}/{}.layout", dir, lname));
    if ( ok )
	ok = parse_layout(layout, txt.c_str(), err);
    else
	err = "no such file";
    if ( !ok )
    {
	var fallback;
	var ferr;
	parse_layout(fallback, default_layout_text(), ferr);
	ui::set(w, es, "layout", fallback);
	ui::set(w, es, "msg", format("Layout '{}' {} — the default layout is in use.", lname, err));
	return false;
    }
    ui::set(w, es, "layout", layout);
    return true;
}

// ---- the layout tree -> `.layout` text (parse_layout's inverse) -----------
// V2c persistence (design §5): the live layout is written beside the manifest
// so a docked pane / a resized band comes back on the next project open. The
// emitter is parse_layout's STRUCTURAL inverse — window, the editor region,
// the chrome panes — so parse_layout(layout_to_text(lo)) rebuilds an equal
// tree. It omits what the grammar omits (a transient code render, the active
// tab): those are session state, not layout (design §5: positions / sizes /
// modes / hidden flags, NOT the tabs' contents).

// One indentation prefix: two spaces per depth level. parse_layout reads a
// line's depth as its leading-whitespace count and only requires siblings to
// agree, so any consistent step round-trips.
void layout_indent(var &out, long depth)
{
    var s = "";
    for ( long i = 0; i < depth; i = i + 1 )
	s = format("{}  ", s);
    out = s;
}

// One pane line: `pane <slot> [<side>] [<size>%] <mode> views <kind>...
// [focus] [hidden]` — mode and size BEFORE `views` and the flags after (so
// the `views` list stops at a flag), the exact word order parse_layout reads.
// An editor pane persists as a SOURCE pane (its tabs' Views are session
// state); a chrome pane persists its tool tabs (fixed kinds).
void layout_emit_pane(var &out, var &node, long depth)
{
    var ind;
    layout_indent(ind, depth);
    long slot = node["slot"].as_integer();
    var line = format("{}pane {}", ind, slot_name(slot));
    long side = node["side"].as_integer();
    if ( side != ui::side::none )
	line = format("{} {}", line, ui::side_name(side));
    long size = node["size"].as_integer();
    if ( size > 0 )
	line = format("{} {}%", line, size);
    line = format("{} {} views", line, pmode_name(node["mode"].as_integer()));
    if ( slot == slotEDITOR )
	line = format("{} {}", line, view_name(viewSOURCE));
    else
	for ( var t : node["tabs"] )
	    line = format("{} {}", line, view_name(t["kind"].as_integer()));
    if ( node["focus"].as_integer() != 0 )
	line = format("{} focus", line);
    if ( node["hidden"].as_integer() != 0 )
	line = format("{} hidden", line);
    out = format("{}\n", line);
}

// One split header line: `split <dir> [<size>%]`; the children follow indented
// one level deeper.
void layout_emit_split(var &out, var &node, long depth)
{
    var ind;
    layout_indent(ind, depth);
    var line = format("{}split {}", ind, ui::split_name(node["dir"].as_integer()));
    long size = node["size"].as_integer();
    if ( size > 0 )
	line = format("{} {}%", line, size);
    out = format("{}\n", line);
}

// A node (a pane or a split subtree) -> its text, children recursed one level
// deeper.
void layout_emit_node(var &out, var &node, long depth)
{
    if ( node["kind"] == ctSPLIT )
    {
	var body;
	layout_emit_split(body, node, depth);
	for ( var c : node["children"] )
	{
	    var cs;
	    layout_emit_node(cs, c, depth + 1);
	    body = format("{}{}", body, cs);
	}
	out = body;
	return;
    }
    layout_emit_pane(out, node, depth);
}

// The whole tree -> text: `@window`, the editor region (a top-level pane or
// split), then the chrome panes (top-level).
void layout_to_text(var &out, var &lo)
{
    var s = "# madcide layout — written by the session; edit or delete freely\n";
    s = format("{}@window {}\n", s, lo["window"]);
    var ed;
    layout_emit_node(ed, lo["editor"], 0);
    s = format("{}{}", s, ed);
    for ( var c : lo["chrome"] )
    {
	var cs;
	layout_emit_pane(cs, c, 0);
	s = format("{}{}", s, cs);
    }
    out = s;
}

// The client's layout tree; a session that never ran open (the headless
// harness attaches over its own world) gets the baked default on first use.
void layout_get(var &lo, long w, long es)
{
    ui::get(lo, w, es, "layout");
    if ( lo.is_null() )
    {
	var err;
	parse_layout(lo, default_layout_text(), err);
	ui::set(w, es, "layout", lo);
    }
}

// The chrome pane hosting a View KIND: its index in the layout's chrome and
// the tab's index within it; -1 when no pane hosts the kind.
long chrome_hosting(var &lo, long kind, long &tab)
{
    long ci = 0;
    for ( var c : lo["chrome"] )
    {
	long ti = 0;
	for ( var t : c["tabs"] )
	{
	    if ( t["kind"] == kind )
	    {
		tab = ti;
		return ci;
	    }
	    ti = ti + 1;
	}
	ci = ci + 1;
    }
    tab = -1;
    return -1;
}

// The kind a chrome pane SHOWS: its active tab's, viewNONE while hidden.
long chrome_shown(var &c)
{
    if ( c["hidden"].as_integer() != 0 )
	return viewNONE;
    var tabs = c["tabs"];
    long a = c["active"].as_integer();
    if ( a < 0 || a >= php::count(tabs) )
	return viewNONE;
    var t = tabs[a];
    return t["kind"].as_integer();
}

// The kind shown in the chrome pane of a SLOT (viewNONE: hidden or no such
// pane) — a probe's read.
long slot_shown(long w, long es, long slot)
{
    var lo;
    layout_get(lo, w, es);
    for ( var c : lo["chrome"] )
	if ( c["slot"] == slot )
	    return chrome_shown(c);
    return viewNONE;
}

// Is a tool View SHOWN (its pane visible with it the active tab)?
bool view_shown(long w, long es, long kind)
{
    var lo;
    layout_get(lo, w, es);
    long tab = -1;
    long ci = chrome_hosting(lo, kind, tab);
    if ( ci < 0 )
	return false;
    var c = lo["chrome"][ci];
    return c["hidden"].as_integer() == 0 && c["active"].as_integer() == tab;
}

// SHOW a tool View: the chrome pane hosting it comes up with that tab active
// — the ONE surface (the terminal's diags pane and the window's panel used
// to be two). Focus stays where it is (Tab reaches the rows, as ever; V2b's
// viewfocus moves it). Remembers the kind as the one the @pane esc hides.
// False + a status message for a kind no pane of this layout hosts.
bool show_view(long w, long es, long kind)
{
    var lo;
    layout_get(lo, w, es);
    long tab = -1;
    long ci = chrome_hosting(lo, kind, tab);
    if ( ci < 0 )
    {
	ui::set(w, es, "msg", format("No pane of the layout hosts the {} view.",
				     view_name(kind)));
	return false;
    }
    var ch = lo["chrome"];
    var c = ch[ci];
    c["hidden"] = 0;
    c["active"] = tab;
    ch[ci] = c;
    lo["chrome"] = ch;
    ui::set(w, es, "layout", lo);
    ui::set(w, es, "chromelast", kind);
    return true;
}

// HIDE the chrome pane hosting a kind; true when it was visible.
bool hide_view(long w, long es, long kind)
{
    var lo;
    layout_get(lo, w, es);
    long tab = -1;
    long ci = chrome_hosting(lo, kind, tab);
    if ( ci < 0 )
	return false;
    var ch = lo["chrome"];
    var c = ch[ci];
    bool was = c["hidden"].as_integer() == 0;
    c["hidden"] = 1;
    ch[ci] = c;
    lo["chrome"] = ch;
    ui::set(w, es, "layout", lo);
    if ( es_int(w, es, "chromelast", viewNONE) == kind )
    {
	long none = viewNONE;
	ui::set(w, es, "chromelast", none);
    }
    return was;
}

// The @pane esc on the chrome: hide the pane that showed the LAST kind; false
// when nothing is up to hide (the key falls through).
bool hide_last_view(long w, long es)
{
    long k = es_int(w, es, "chromelast", viewNONE);
    if ( k == viewNONE )
	return false;
    long none = viewNONE;
	ui::set(w, es, "chromelast", none);
    return hide_view(w, es, k);
}

// TOGGLE a slot's chrome pane (the `panel` command): hidden <-> shown with
// its active tab; a layout without that slot says so.
void toggle_slot(long w, long es, long slot)
{
    var lo;
    layout_get(lo, w, es);
    var ch = lo["chrome"];
    long n = php::count(ch);
    for ( long i = 0; i < n; i = i + 1 )
    {
	var c = ch[i];
	if ( !(c["slot"] == slot) )
	    continue;
	bool hide = c["hidden"].as_integer() == 0;
	c["hidden"] = hide ? 1 : 0;
	ch[i] = c;
	lo["chrome"] = ch;
	ui::set(w, es, "layout", lo);
	long last = hide ? viewNONE : chrome_shown(c);
	ui::set(w, es, "chromelast", last);
	return;
    }
    ui::set(w, es, "msg", format("The layout has no {} pane.", slot_name(slot)));
}

// ---- theme as data (AST-2): classification -> style spec, a SCHEME ----
// One `class SPEC` pair per line: the class is the FIRST word, the spec
// the rest — JOE's style vocabulary (attribute words + a colour +
// bg_colour, e.g. "keyword bold yellow"); the renderer's ui_style_of
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
    var gth;			// the @gui scope: CSS custom properties
    var pth;			// the @presence scope: round-robin caret colours
    array lines;
    php::explode(lines, "\n", txt.c_str());
    long n = php::count(lines);
    for ( long i = 0; i < n; i = i + 1 )
    {
	var line;
	if ( !profile_line(line, lines, i) )
	    continue;
	const char *ls = line.c_str();
	// A scoped line (@gui prop value): the web target's colours/fonts as
	// CSS custom properties, through the ONE scope_line_parts rule the
	// key tables use. The unscoped JOE-vocabulary lines below still feed
	// the TUI theme unchanged — one file, two renderings, later lines win.
	if ( ls[0] == '@' )
	{
	    // @presence <slot> <style spec>: the round-robin caret colour a
	    // client is dealt at connect (client-server V3c). A spec may be
	    // multi-word ("bold red"), so read the tail whole — the shared scope
	    // rule (scope_line_parts) keeps only the last word.
	    if ( perl::index(ls, "@presence ") == 0 )
	    {
		var rest = php::trim(ls + 10);
		const char *rc = rest.c_str();
		const char *gap = strchr(rc, ' ');
		if ( gap != 0 )
		{
		    var slot = php::trim(perl::substr(rest, 0, gap - rc));
		    pth[slot.c_str()] = php::trim(gap + 1);
		}
		continue;
	    }
	    var scope;
	    var prop;
	    var val;
	    if ( scope_line_parts(line, scope, prop, val) && scope == "gui" )
		gth[prop.c_str()] = val;
	    continue;
	}
	const char *sp = strchr(ls, ' ');
	if ( sp == 0 )
	    continue;
	long cut = sp - ls;
	var cls = php::trim(perl::substr(line, 0, cut));
	var spec = php::trim(sp + 1);
	th[cls.c_str()] = spec;
    }
    ui::set(w, es, "theme", th);
    ui::set(w, es, "gui_theme", gth);	// empty clears a prior scheme's vars
    ui::set(w, es, "presence_theme", pth);	// empty clears the prior palette
    ui::set(w, es, "scheme", tname);
    return true;
}

// ---- menus / commands as data (S1, 2026-09-08) --------------------------
// profiles/<name>.menu: the command / menu CONTRIBUTION (the VS Code shape:
// a command registry + a menu-location map, one action vocabulary — the
// ids the key profiles bind and the dispatcher understands). One line per
// item "MENU COMMAND TITLE... [WHEN]", "MENU -" a separator, comments and
// blanks by the profile_line rule. The bag holds `menudef`: the menus in
// first-appearance order, each { title, items: [{ id, title, when } |
// { sep }] }. Key spellings are NOT part of it — a renderer shows the
// chord the LOADED profile binds to an id, so the composed tree stays
// profile-independent (testmadcide's swap-identity gate). The TUI reads
// none of it: JOE has no menu bar; the web target's chrome (S2) and a
// command palette render it. Missing file = no menu (optional data, like
// the status format), false.
bool load_menu(long w, long es, const char *name)
{
    var mname = name;		// ring discipline: own the text first
    var dir;
    ui::get(dir, w, es, "profile_dir");
    var txt;
    if ( !php::file_get_contents(txt, format("{}/{}.menu", dir, mname)) )
	return false;
    var menus = {};
    var at;			// menu title -> index into menus
    var ct;
    cmd_table(ct);		// ids resolve to codes HERE (the input boundary)
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
	var menu = perl::substr(line, 0, sp - ls);
	var rest = php::trim(sp + 1);
	var item;
	if ( rest == "-" )
	    item = { "sep": 1 };
	else
	{
	    const char *rs = rest.c_str();
	    const char *sp2 = strchr(rs, ' ');
	    var id = rest;
	    var title = "";
	    if ( sp2 != 0 )
	    {
		id = perl::substr(rest, 0, sp2 - rs);
		title = php::trim(sp2 + 1);
	    }
	    var when = "";
	    // A trailing [when] clause: the context that enables the item.
	    const char *ts = title.c_str();
	    long tl = strlen(ts);
	    if ( tl > 0 && ts[tl - 1] == ']' )
	    {
		const char *lb = strrchr(ts, '[');
		if ( lb != 0 )
		{
		    when = perl::substr(title, lb - ts + 1, tl - (lb - ts) - 2);
		    title = php::trim(perl::substr(title, 0, lb - ts));
		}
	    }
	    // The command id resolves to its code at load; an unknown id
	    // refuses the menu naming its line (enums, not strings).
	    long code = cmd_of_in(ct, id.c_str());
	    if ( code == cmdNONE )
	    {
		ui::set(w, es, "msg", format("Menu '{}' line {}: unknown command '{}'.",
					     mname, i + 1, id));
		return false;
	    }
	    item = { "id": id, "code": code, "title": title, "when": when };
	}
	var idx = at[menu.c_str()];
	if ( idx.is_null() )
	{
	    var items = {};
	    items.push(item);
	    menus[] = { "title": menu, "items": items };
	    at[menu.c_str()] = php::count(menus) - 1;
	}
	else
	{
	    long k = idx.as_integer();
	    var m = menus[k];
	    var items = m["items"];
	    items.push(item);
	    m["items"] = items;
	    menus[k] = m;
	}
    }
    ui::set(w, es, "menudef", menus);
    return true;
}

// The window table's size (defined with the window machinery below —
// the menu context reads it).
long window_count(long w, long es);
void ensure_build_cmds(long w, long es);	// the Build menu composes the ^B rows

// The context a [when] clause is judged against: named FACTS of the
// session, computed once per compose. A clause names keys from this bag
// only; an unknown key never holds.
void menu_context(var &ctx, long w, long es, long doc, long nav)
{
    var mod;
    ui::get(mod, w, doc, "modified");
    var pf;
    ui::get(pf, w, es, "projfile");
    var bufs;
    ui::get(bufs, w, es, "buffers");
    bool editable = es_flag(w, es, "editable");
    bool dirty = !mod.is_null() && mod.as_boolean();
    bool selection = es_int(w, es, "mark", -1) >= 0;
    bool split = window_count(w, es) >= 2;
    bool buffers = !bufs.is_null() && php::count(bufs) > 1;
    bool project = !pf.is_null() && strlen(pf.c_str()) > 0;
    bool building = es_int(w, es, "buildlive", 0) != 0;
    bool modal = cur_vimode(w, es) != viOFF;
    bool viewing = nav != doc;
    ctx = { "editable": editable, "dirty": dirty, "selection": selection,
	    "split": split, "buffers": buffers, "project": project,
	    "building": building, "modal": modal, "viewing": viewing };
}

// Does a [when] clause hold? `key`, `!key`, joined by `&&`; an empty
// clause always holds; an unknown key never does.
bool when_holds(var &ctx, var &when)
{
    if ( strlen(when.c_str()) == 0 )
	return true;
    array terms;
    php::explode(terms, "&&", when.c_str());
    long n = php::count(terms);
    for ( long i = 0; i < n; i = i + 1 )
    {
	var t = php::trim(php::array_get_cstr(terms, i));
	const char *ts = t.c_str();
	bool neg = ts[0] == '!';
	var key = t;
	if ( neg )
	    key = php::trim(ts + 1);
	var v = ctx[key.c_str()];
	bool holds = !v.is_null() && v.as_boolean();
	if ( holds == neg )
	    return false;
    }
    return true;
}

// The menu BAR as the composed tree carries it: the bar's menus in order
// (the "palette" menu is palette-only and stays off the bar), each item
// { id, title } plus `enabled: 0` when its [when] fails now. Titles and
// ids only — profile-independent by construction. (compose_menu is the
// shared editor helpers' vised-style menu LINE — a different node.)
bool compose_menu_bar(var &out, long w, long es, long doc, long nav)
{
    var def;
    ui::get(def, w, es, "menudef");
    if ( def.is_null() || php::count(def) == 0 )
	return false;
    var ctx;
    menu_context(ctx, w, es, doc, nav);
    var bar = {};
    for ( var m : def )
    {
	if ( m["title"] == "palette" )
	    continue;
	var items = {};
	for ( var it : m["items"] )
	{
	    if ( !it["sep"].is_null() )
	    {
		items[] = { "sep": 1 };
		continue;
	    }
	    var row = { "id": it["id"], "code": it["code"], "title": it["title"] };
	    var when = it["when"];
	    if ( !when_holds(ctx, when) )
		row["enabled"] = 0;
	    items.push(row);
	}
	// The Build menu carries the ^B palette's rows as DIRECT items
	// (owner 2026-09-08: Build -> Run runs, no overlay): the same
	// `buildcmds` data the palette lists — the built-ins for the
	// buffer or the project, plus a manifest's own commands — one item
	// per row, id `build-<n>` (its 1-based row, the palette's choose
	// contract), after a separator. Stop is enabled only while a build
	// runs, the rest only while none does (what the palette's rows do).
	if ( m["title"] == "Build" )
	{
	    ensure_build_cmds(w, es);
	    var brows;
	    ui::get(brows, w, es, "buildcmds");
	    if ( !brows.is_null() && php::count(brows) > 0 )
	    {
		items[] = { "sep": 1 };
		bool live = es_int(w, es, "buildlive", 0) != 0;
		long n = 0;
		for ( var br : brows )
		{
		    n = n + 1;
		    var brow = { "id": format("build-{}", n), "code": cmdBUILDROW,
				 "title": format("{}", br["name"]) };
		    bool stop = br["act"] == cmdBLD_STOP;
		    if ( stop != live )
			brow["enabled"] = 0;
		    items.push(brow);
		}
	    }
	}
	bar[] = { "title": m["title"], "items": items };
    }
    out = { "bar": bar };
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
// (source coordinates) converted to the byte-offset rows the edit node's
// hints speak: {s, e, c} — the byte range and the THEME's style spec for
// the span's class, JOE's vocabulary (`bold`, `cyan`, `bold yellow`). The
// spec is the ONE fact both renderers read: the grid paints it through the
// one spec parser, the window renders the same style as classes over its
// palette — the scheme the terminal shows is the scheme the window shows
// (the colour-unification slice, 2026-09-08; before it the rows also
// carried the semantic class and the web coloured THAT from a palette of
// its own). A class the scheme does not style is plain on both, so its
// span is not emitted. ONE converter, three feeders: parse_spans
// (refresh_spans, the full-parse state), lex_spans (colour at load
// before the parse lands) and the view lens (enter_view: the lexer's
// classes over the EMITTED text). `doc` is the buffer whose lines the
// rows address; `dest` is the entity the rows land on — the state bag
// for the document's own spans, the view buffer for a lens's (owner
// 2026-09-09: every emitted code view is coloured). Like the status
// line, spans go stale with edits until the next check/save. A plain
// identifier (styled by no scheme) or a stale line skips.
void spans_to_hspans(long w, long es, long doc, var &rows, long dest)
{
    var hs = {};
    var th;
    ui::get(th, w, es, "theme");
    for ( var r : rows )
    {
	var cls = r["class"];
	if ( cls == "ident" )
	    continue;			// a plain identifier: styled by no scheme
	var c;
	if ( !th.is_null() )
	    c = th[cls.c_str()];
	if ( c.is_null() )
	    continue;			// a class this scheme leaves plain
	long ls = ui::text_line_start(w, doc, r["line"].as_integer());
	if ( ls < 0 )
	    continue;			// a stale line (edited since the parse)
	long s = ls + r["column"].as_integer();
	hs[] = { "s": s, "e": s + r["length"].as_integer(), "c": c };
    }
    ui::set(w, dest, "hspans", hs);
}

void refresh_spans(long w, long es, long doc, long h)
{
    var rows;
    madc::parse_spans(rows, h);
    spans_to_hspans(w, es, doc, rows, es);
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
    spans_to_hspans(w, es, doc, rows, es);
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
	if ( r["severity_code"] == madc::diag_severity::error )	// the row's enumerator (V0.5b), never its display word
	    errs = errs + 1;
    }
    return errs;
}

// ---- the tool Views' ONE surface (V2: the layout's chrome panes) ----------
// The diagnostics show where the layout puts the problems View (the panel's
// Problems tab by default — in the terminal too, the ruled relaxation); a
// build's stream where it puts the output View. Six sites used to set the
// pane by hand; a second surface is where they would have drifted.
void show_diags(long w, long es)
{
    show_view(w, es, viewPROBLEMS);
}

void show_output(long w, long es)
{
    show_view(w, es, viewOUTPUT);
}

// The check ACTION: refresh + report + show the pane.
void run_check(long w, long es, long doc)
{
    long n = refresh_diags(w, es, doc);
    show_diags(w, es);
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
bool save_as(long w, long es, long doc, const char *path0);	// S4: Save As
long active_doc(long w, long es, long fallback);
void goto_pane_row(long w, long es, long doc, long option, bool outline)
{
    var rows;
    if ( outline )
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

// ---- Views (client-server arc V1; design doc §2.1) ------------------------
// A View is DATA on the session — es "views", rows
//   { id, subject, kind, lang, revision, generator, vbuf, rbuf, map }
// id: 1-based, never reused in a session (the row's index + 1); subject:
// the document entity the View projects (an entity handle, never a byte
// offset); kind: ide_view; lang: the representation's language (madc::fk*,
// <bits/file_kinds>) — the subject's own kind for a source View, the render
// target for a code View; revision: 0 = current (an event sequence number
// from V4); generator: ide_gen (genMADC for a render, genNONE for stored
// text); vbuf: the entity holding the View's TEXT — the subject for stored
// text, the View's own render buffer for a code View (rbuf: that buffer,
// allocated once per View and kept between renders); map: the coordinate
// map to the subject's stored space (EMPTY for a wholly rendered view,
// ui::lens_to_display; V5 fills it). A View's highlight SPANS live on its
// text entity — the render buffer's "hspans" for a code View, the state
// bag's stored-space "hspans" (edit-shifted by ed_text_insert / erase) for
// stored text; view_spans is the one reader.
// NAVIGATION (caret / mark / bend, the parked stored-space caret "ocaret")
// is NOT View state (OWNER 2026-09-09, design §6 Q1): it belongs to the
// CONTAINER that shows the View — the state bag for the focused editor tab
// (the shared editor machinery's live cursor), the buffer row for the other
// tabs, the window row for the S5 stack — so every client sits at its own
// place in one shared text.
// The FOCUSED View is es "fview": the View the focused container (the
// active editor tab) shows; every buffer row carries its View as "view". A
// session without the buffer table (the headless harness) gets its focused
// View created on first need, over the document it drives. Defined here,
// ahead of the prompt machinery (the theme completion consults the lens
// predicate) and of the lens seam below.

bool view_row(var &out, long w, long es, long id)
{
    if ( id <= 0 )
	return false;
    var vs;
    ui::get(vs, w, es, "views");
    if ( vs.is_null() || php::count(vs) < id )
	return false;
    out = vs[id - 1];
    return true;
}

void view_store(long w, long es, var &v)
{
    var vs;
    ui::get(vs, w, es, "views");
    vs[v["id"].as_integer() - 1] = v;
    ui::set(w, es, "views", vs);
}

// A new View over `subject`, its text the subject's own buffer; the id.
long view_new(long w, long es, long subject, long kind, long lang)
{
    var vs;
    ui::get(vs, w, es, "views");
    if ( vs.is_null() )
    {
	var fresh = {};
	vs = fresh;
    }
    long id = php::count(vs) + 1;
    var nomap = {};
    vs[] = { "id": id, "subject": subject, "kind": kind, "lang": lang,
	     "revision": 0, "generator": genNONE, "vbuf": subject,
	     "rbuf": 0, "map": nomap };
    ui::set(w, es, "views", vs);
    return id;
}

long focused_view(long w, long es)
{
    return es_int(w, es, "fview", 0);
}

// The focused View, created over `doc` (a source View) when the session
// has none yet.
long ensure_focused_view(long w, long es, long doc)
{
    long id = focused_view(w, es);
    var v;
    if ( view_row(v, w, es, id) )
	return id;
    id = view_new(w, es, doc, viewSOURCE, doc_kind(w, doc));
    ui::set(w, es, "fview", id);
    return id;
}

// The entity a View's text is read from: its render buffer while it is a
// code View, else the document — the ONE place display-vs-stored routing
// happens (nav_doc and the composer both read it). A null row (a session
// with no View yet) is the document.
long view_text_entity(var &v, long doc)
{
    if ( v.is_null() || v["kind"].as_integer() != viewCODE )
	return doc;
    long vb = v["vbuf"].as_integer();
    return vb != 0 ? vb : doc;
}

// The one reader of a View's highlight rows (see the table comment).
void view_spans(var &out, long w, long es, var &v)
{
    if ( !v.is_null() && v["kind"].as_integer() == viewCODE )
	ui::get(out, w, v["vbuf"].as_integer(), "hspans");
    else
	ui::get(out, w, es, "hspans");
}

// THE lens-up rule, one seat (dupaudit family view_active_predicate): a
// lens is up iff the focused View is a code render (viewCODE). Out = the
// render language's name (madc::file_kind_name: "mc11" / "c11" / "c++"),
// "" otherwise; false = the identity lens (a source View, or no View yet).
bool lens_name(var &out, long w, long es)
{
    out = "";
    var v;
    if ( !view_row(v, w, es, focused_view(w, es)) )
	return false;
    if ( v["kind"].as_integer() != viewCODE )
	return false;
    out = madc::file_kind_name(v["lang"].as_integer());
    return true;
}

// Render one joerc-style format string. Supported seats: %n name,
// %m "(Modified) ", %R "(Read only) ", %r row / %c col (an optional
// width digit-run left-pads: %4r), %o byte offset, %k the pending chord
// (JOE's chord echo — the client-pushed fact; the vi grammar's pending
// count/operator echoes here too), %M the vi MODE seat (vim's
// showmode), %x the enclosing context — madcide renders its AST-backed
// enclosing function here, superior to JOE's textual backward
// heuristic — %y the active VIEW name in JOE's "(syntax)" seat, and %%
// a literal percent. UNSUPPORTED joerc escapes render EMPTY and light
// up as their features land (%T %W %I %X %S
// ...). \X attribute escapes are consumed — the status row renders
// inverse whole-line; intra-line styling is a named seat.
// The format and the context arrive as VALUES: .c_str() text is
// ring-lifetime (the pre-L3 contract) and this loop makes many
// ring-returning calls — the format is copied into a frame-owned buffer
// at entry, never held as a borrowed pointer.
// The SAME expansion also yields the line's SEGMENTS (S3, the status bar
// as chrome): one row per seat that expanded to text — { seat: the
// format letter, label: the literal text before it (trimmed: "Row" for
// `Row %4r`), text: the seat's text (trimmed) } — so a chrome renderer
// lays discrete, themeable items where the terminal shows one string. An
// empty seat (an unsupported escape, no chord pending) contributes no
// segment and drops the literal before it; `%%` is literal text. One
// walk, two renderings.
void status_expand(var &out, var &segs, long w, long es, long doc, long caret,
		   var &fmtv, var &fnctxv)
{
    char fmt[240];
    strncpy(fmt, fmtv.c_str(), 239);
    fmt[239] = 0;
    var s = "";
    var lit = "";		// literal text since the last seat
    var rows = {};
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
	{
	    var piece = perl::substr(fmt, run, i - run);
	    s = format("{}{}", s, piece);
	    lit = format("{}{}", lit, piece);
	}
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
	    // client's; the session only typesets the echo seat). With
	    // no chord in flight the seat echoes the vi grammar's
	    // pending count/operator instead (vim's showcmd).
	    var pc;
	    ui::get(pc, w, es, "pendingchord");
	    if ( !pc.is_null() && strlen(pc.c_str()) > 0 )
		seat = format("{} ", pc);
	    else
	    {
		var vp;
		ui::get(vp, w, es, "vipend");
		if ( !vp.is_null() && strlen(vp.c_str()) > 0 )
		    seat = format("{} ", vp);
	    }
	}
	if ( e == 'M' )
	{
	    // The vi mode seat (vim's showmode; empty outside vi mode).
	    long vm = cur_vimode(w, es);
	    if ( vm == viINSERT )
		seat = "-- INSERT -- ";
	    else if ( vm == viNORMAL )
		seat = "-- NORMAL -- ";
	}
	if ( e == 'x' )
	    seat = fnctxv;
	if ( e == 'y' )
	{
	    var vw;
	    if ( lens_name(vw, w, es) )
		seat = format("({}) ", vw);
	}
	long pad = width - strlen(seat.c_str());
	if ( pad > 0 )
	    s = format("{}{}", s, php::str_repeat(" ", pad));
	s = format("{}{}", s, seat);
	if ( e == '%' )
	    lit = format("{}%", lit);		// literal, not a seat
	else
	{
	    var text = php::trim(seat);
	    if ( strlen(text.c_str()) > 0 )
	    {
		var letter = perl::substr(fmt, j, 1);
		var label = php::trim(lit);
		rows[] = { "seat": letter, "label": label, "text": text };
	    }
	    lit = "";
	}
	i = j + 1;
	run = i;
    }
    if ( i > run )
	s = format("{}{}", s, perl::substr(fmt, run, i - run));
    out = s;
    segs = rows;
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
// The confirm question's answers as a chrome-drawing client shows them
// (the dialog's buttons): the @confirm scope's ACTIONS — profile data
// binds their keys; this table gives each its title and the order a
// dialog lays them. An action the scope does not bind gets no button, so
// a profile that respells the question's keys keeps the dialog honest.
void confirm_choices(var &out, long w, long es)
{
    var mk;
    modal_keys(mk, w, es);
    var sc = mk["confirm"];
    var titles = {};
    titles[] = { "code": cmdPYES, "label": "Yes" };
    titles[] = { "code": cmdPCANCEL, "label": "No" };
    var rows = {};
    for ( var t : titles )
    {
	var want = t["code"];
	bool bound = false;
	if ( !sc.is_null() )
	    for ( var a : sc )
		if ( a == want )
		    bound = true;
	if ( bound )
	{
	    var nm;
	    cmd_name(nm, want.as_integer());
	    t["action"] = nm;		// the wire id the page posts back
	    rows.push(t);
	}
    }
    out = rows;
}

// The action a modal scope binds to one key — the scope's own data
// (profile-loaded or the baked defaults), looked up through the ONE
// modal reader by a synthetic key event. Null = the scope binds nothing
// for that key. Used to name a dialog's buttons after what the keys do.
void scope_key_action(var &out, long w, long es, const char *scope,
		      const char *key)
{
    var none;
    out = none;
    var ke = { "event": "key", "event_code": ui::event_kind::key };
    ke["key"] = key;
    ke["key_code"] = ui::key_code(key);
    var a;
    if ( scoped_action(a, w, es, scope, ke) )
	out = a;
}

// A list pane's hints as a DIALOG for a chrome-drawing client (polish P2,
// the S6 treatment for the list overlays): ADDITIVELY, beside the `list`
// and `focus` hints the grid model reads, `popup` (the overlay), `dialog`
// {title, filter?, buttons} and `dismiss` (a press outside = the scope's
// cancel). The buttons are DATA named after what the pane's keys do:
// the primary picks the selected row (`choose` — the focus owner's enter
// contract, so every pane's choose arm runs unchanged) under the label
// the caller gives (null = no primary: a pane whose rows do nothing on
// choose); Close posts the action the pane's @scope binds to esc — a
// scope that binds no cancel gets no Close button (confirm_choices'
// honesty rule). The filter is the text the core holds (null = the pane
// does not filter). The terminal reads none of it.
void dialog_hints(var &box, long w, long es, const char *scope,
		  const char *primary, var &filter)
{
    var bh = box["hints"];
    bh["popup"] = 1;
    var title = box["label"];
    var dlg = { "title": title };
    if ( !filter.is_null() )
	dlg["filter"] = filter;
    var btns = {};
    if ( primary != 0 && strlen(primary) > 0 )
    {
	var pb = { "label": primary, "choose": 1 };
	btns.push(pb);
    }
    var cancel;
    scope_key_action(cancel, w, es, scope, "esc");
    if ( !cancel.is_null() )
    {
	// The scope's cancel CODE; its name is the wire id the page posts
	// (the engine converts it back to the code at that boundary).
	var cname;
	cmd_name(cname, cancel.as_integer());
	var cb = { "label": "Close" };
	cb["action"] = cname;
	cb["code"] = cancel;
	btns.push(cb);
	bh["dismiss"] = cname;
	bh["dismiss_code"] = cancel;
    }
    dlg["buttons"] = btns;
    bh["dialog"] = dlg;
    box["hints"] = bh;
}

// A prompt row's hints for a window (S6, the prompts as GUI dialogs):
// the same row the terminal shows as its bottom line carries, ADDITIVELY,
// the quick-input data a chrome-drawing client renders as a floating
// popup — `popup` (the overlay), `prompt` {label, input} (the core owns
// the input text: keys reach it exactly as in the terminal, the client
// only draws it — Neovim's ext_cmdline shape), `dismiss` (the action a
// click outside the popup fires: the scope's cancel). The terminal reads
// none of it (its "unknown hints are ignored" rule).
void prompt_hints(var &st, var &label, var &input, bool dismiss)
{
    var qi = { "label": label, "input": input };
    var h = { "popup": 1 };
    if ( dismiss )
    {
	var pc;
	cmd_name(pc, cmdPCANCEL);
	h["dismiss"] = pc;
	h["dismiss_code"] = cmdPCANCEL;
    }
    h["prompt"] = qi;
    st["hints"] = h;
}

bool compose_overlay_row(var &row, long w, long es)
{
    var st = { "role": "content" };
    var pr;
    ui::get(pr, w, es, "prompt");
    if ( !pr.is_null() && strlen(pr.c_str()) > 0 )
    {
	if ( cur_pmode(w, es) == pmCONFIRM )
	{
	    st["content"] = pr;	// the label spells its own keys (y,n,^C)
	    // The question as a DIALOG in a window: the label and its
	    // answers as buttons — each fires the scope's action by name
	    // (scope_action_named), what its key does in the terminal.
	    var ch;
	    confirm_choices(ch, w, es);
	    var cf = { "label": pr };
	    cf["choices"] = ch;
	    var h = { "popup": 1 };
	    h["confirm"] = cf;
	    st["hints"] = h;
	    row = st;
	    return true;
	}
	var pi;
	ui::get(pi, w, es, "pinput");
	st["content"] = format("{} (^C aborts): {}", pr, pi);
	prompt_hints(st, pr, pi, true);
	row = st;
	return true;
    }
    if ( es_flag(w, es, "searching") )
    {
	var sch;
	ui::get(sch, w, es, "search");
	st["content"] = format("Find (^C aborts): {}", sch);
	var fl = "Find";
	prompt_hints(st, fl, sch, false);	// its cancel is the search's own
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
    var st = { "role": "status" };
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
    var lsegs;
    status_expand(lout, lsegs, w, es, doc, caret, lm, fnctx);
    var fill = " ";
    var rfmt = "";
    if ( strlen(rm.c_str()) > 0 )
    {
	fill = perl::substr(rm, 0, 1);
	rfmt = perl::substr(rm, 1, strlen(rm.c_str()) - 1);
    }
    var rout;
    var rsegs;
    status_expand(rout, rsegs, w, es, doc, caret, rfmt, fnctx);
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
    // The status bar as discrete items (slice 3 Task 5; S3 as chrome): the
    // SAME expansion's SEGMENTS (left half, right half — each seat that
    // showed text, with its label) ride the hints so the web target lays
    // them as discrete, themeable items; the TUI reads the combined
    // `content` string above (the fill between the halves is a terminal
    // real-estate detail). No new seat logic — one expansion, two
    // renderings.
    var items = { "left": lsegs, "right": rsegs };
    var sh = { "items": items };
    st["hints"] = sh;
    status = st;
}

// ---- one prompt mode (find / goto-line / insert-file / theme) ---------
// Label + input + completion mode on the bag; the shared status line
// projects "label: input". While a prompt is up it consumes every event:
// text appends, backspace trims, enter completes, esc cancels.
void start_prompt(long w, long es, const char *label, long mode)
{
    ui::set(w, es, "prompt", label);
    set_pmode(w, es, mode);
    ui::set(w, es, "pinput", "");
}

void end_prompt(long w, long es)
{
    ui::set(w, es, "prompt", "");
    set_pmode(w, es, pmNONE);
    ui::set(w, es, "pinput", "");
    ui::set(w, es, "pverb", "");
}

// A single-key confirm (JOE's "(y,n,^C)?" questions): the label spells
// its own keys and the CONSEQUENCE is data — the verb y fires. Rides the
// one prompt mode, so composition and event routing stay shared; n, esc,
// or any bound action (^C included) cancels.
void start_confirm(long w, long es, const char *label, const char *verb)
{
    start_prompt(w, es, label, pmCONFIRM);
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
void proj_open(long w, long es, long doc, const char *path0);	// polish P2b: Open Project… (the prompt commits through it)
void switch_buffer(long w, long es, long olddoc, long idx);	// polish P4: the bufsel prompt commits through it
bool project_active(long w, long es);
bool proj_write(long w, long es, long doc);	// polish P2c: the kind toggle persists
void proj_show(long w, long es, long doc);	// polish P2b: Open Project… shows the window
void project_rows(long w, long es, long doc);
// The vi colon line (defined after the verbs it translates to): one ex
// command, interpreted against the EXISTING vocabulary. False = quit.
bool colon_command(long w, long es, long doc, long caret, var &in);

// The view* verbs (V2b) — defined with the dispatcher, reached from the
// colon line too (forward-declared for that earlier caller).
void view_split(long w, long es, long doc, long dir, long lang);
void view_focus(long w, long es, long doc, long dir);
void view_close(long w, long es, long doc);
void view_sync(long w, long es, var &arg);			// V5 cursor sync toggle
long viewsync_leaf_caret(long w, long es, long vid, long fallback, long &follow);	// V5 read-side projection
void view_open(long w, long es, long doc, long lang);
void view_dock(long w, long es, long slot, long side);	// V2c: chrome placement
void view_size(long w, long es, long slot, long size);	// V2c: the splitter's size
void view_window(long w, long es, long doc);		// V3b: a second window
long viewsplit_dir_of(var &arg);
long viewfocus_dir_of(var &arg);
long viewrepr_lang_of(var &word);
void viewsplit_args(var &arg, long &dir, long &lang);
void viewdock_target(var &arg, long &slot, long &side);
void viewsize_args(var &arg, long &slot, long &size);

// Prompt completion (enter): dispatch on the stored mode. False = quit
// (a committed :q — every other mode continues the session).
bool finish_prompt(long w, long es, long doc, long caret)
{
    long mode = cur_pmode(w, es);
    var in;
    ui::get(in, w, es, "pinput");
    end_prompt(w, es);
    if ( strlen(in.c_str()) == 0 )
	return true;
    if ( mode == pmCOLON )
	return colon_command(w, es, doc, caret, in);
    if ( mode == pmFIND )
    {
	ui::set(w, es, "search", in.c_str());
	do_find(w, es, doc, caret);
	return true;
    }
    if ( mode == pmGOTOLINE )
    {
	ui::set(w, es, "caret", caret_to_line(w, doc, php::intval(in), 0));
	return true;
    }
    if ( mode == pmINSERTFILE )
    {
	insert_file(w, es, doc, caret, in.c_str());
	return true;
    }
    if ( mode == pmEDITFILE )
    {
	edit_file(w, es, doc, in.c_str());
	return true;
    }
    if ( mode == pmSAVEAS )
	return save_as(w, es, doc, in.c_str());
    if ( mode == pmOPENPROJECT )
    {
	proj_open(w, es, doc, in.c_str());
	return true;
    }
    if ( mode == pmBUFSEL )
    {
	// The palette's Switch to Buffer…: the ring row whose path or base
	// name is the answer.
	var bufs;
	ui::get(bufs, w, es, "buffers");
	long nb = php::count(bufs);
	for ( long i = 0; i < nb; i = i + 1 )
	{
	    var bp = format("{}", bufs[i]["path"]);
	    var bn = bp;
	    const char *sl = strrchr(bp.c_str(), '/');
	    if ( sl != 0 )
		bn = sl + 1;
	    if ( bp == in.c_str() || bn == in.c_str() )
	    {
		if ( i != es_int(w, es, "bufidx", 0) )
		    switch_buffer(w, es, doc, i);
		return true;
	    }
	}
	ui::set(w, es, "msg", format("No buffer '{}'.", in));
	return true;
    }
    if ( mode == pmPROJADD )
    {
	// The prompt is a human client's ARGUMENT collector (gateway
	// ruling): the command is projadd(path).
	proj_add(w, es, doc, in.c_str());
	if ( project_active(w, es) )
	    project_rows(w, es, doc);
	return true;
    }
    if ( mode == pmTABWIDTH )
    {
	long tw = php::intval(in);
	if ( tw < 1 || tw > 16 )
	{
	    ui::set(w, es, "msg", format("Tab width '{}' — need 1..16.", in));
	    return true;
	}
	ui::set(w, es, "tabwidth", tw);
	ui::set(w, es, "msg", format("Tab width {}.", tw));
	return true;
    }
    if ( mode == pmTHEME )
    {
	// A colour SCHEME swap (owner: like the keybinding profiles):
	// load the named theme and recolour from the handle's retained
	// state. `doc` here is the NAV target — while a view is up the
	// stored buffer recolours on exit/reparse instead (spans are
	// stored-space only), so the immediate refresh is skipped.
	if ( !load_theme(w, es, in.c_str()) )
	{
	    ui::set(w, es, "msg", format("No theme '{}'.", in));
	    return true;
	}
	var vw;
	if ( !lens_name(vw, w, es) )
	    refresh_spans(w, es, doc, ensure_phandle(w, es, doc));
	ui::set(w, es, "msg", format("Theme: {}.", in));
    }
    return true;
}

// Every event while the prompt is up. The keys are @prompt/@confirm
// profile data (the baked defaults: enter pcommit / esc pcancel /
// backspace pdelchar; y pyes / n pcancel / esc pcancel) — this arm
// dispatches the looked-up NAMES, never the keys themselves. A window's
// dialog reaches the same names as ACTION events (a confirm button, the
// quick input's click-outside dismissal): scope_action_named admits an
// action the scope binds; every other action stays what it was here —
// a confirm cancels on it, a prompt ignores it.
bool prompt_event(long w, long es, long doc, long caret, var &ev)
{
    var kind = ev["event_code"];
    if ( cur_pmode(w, es) == pmCONFIRM )
    {
	// Single-key question: characters AND keys consult @confirm
	// (a confirm reads its answer characters — unlike a prompt,
	// whose characters are input). ^C arrives as its BOUND action
	// (discard in the JOE profile), so any action cancels too.
	// Unbound keys leave the question standing.
	var a;
	if ( scoped_action(a, w, es, "confirm", ev)
	  || scope_action_named(a, w, es, "confirm", ev) )
	{
	    if ( a == cmdPYES )
	    {
		var vb;
		ui::get(vb, w, es, "pverb");
		end_prompt(w, es);
		return do_verb(w, es, vb.c_str());
	    }
	    if ( a == cmdPCANCEL )
		end_prompt(w, es);
	    return true;
	}
	if ( kind == ui::event_kind::action )
	    end_prompt(w, es);
	return true;
    }
    if ( kind == ui::event_kind::text )
    {
	var in;
	ui::get(in, w, es, "pinput");
	var nx = format("{}{}", in, ev["text"]);
	ui::set(w, es, "pinput", nx.c_str());
	return true;
    }
    if ( kind == ui::event_kind::choose )
    {
	// A FOCUSED pane makes the engine hand enter back as a CHOOSE
	// (its focused-choice contract). While the prompt is modal that
	// enter belongs to the prompt — route it through the same
	// @prompt data as the key, or a prompt opened from a focused
	// pane row (tabwidth, theme, keymap) can never commit.
	var ke = { "event": "key", "event_code": ui::event_kind::key,
		   "key": "enter", "key_code": ui::key::enter };
	var ca;
	if ( scoped_action(ca, w, es, "prompt", ke) && ca == cmdPCOMMIT )
	    return finish_prompt(w, es, doc, caret);
	return true;
    }
    var a;
    if ( kind == ui::event_kind::action )
    {
	// By name (a dialog's button, a dismissal) or by the sequence that
	// completed (^C, bound globally, is the prompt's cancel by data);
	// any other command mid-prompt is inert.
	if ( !scope_action_named(a, w, es, "prompt", ev)
	  && !scoped_action(a, w, es, "prompt", ev) )
	    return true;
    }
    else if ( kind != ui::event_kind::key )
	return true;
    else if ( !scoped_action(a, w, es, "prompt", ev) )
	return true;			// unbound keys are inert
    if ( a == cmdPCOMMIT )
	return finish_prompt(w, es, doc, caret);	// false = a :q quit
    else if ( a == cmdPCANCEL )
	end_prompt(w, es);
    else if ( a == cmdPDELCHAR )
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
    long lo = mark < e ? mark : e;
    long hi = mark < e ? e : mark;
    var b = { "lo": lo, "hi": hi };
    r = b;
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
    // The markers past the insertion follow the text so the block keeps
    // highlighting the ORIGINAL: the anchor registry (shift_anchors, in
    // ed_text_insert) shifts mark / bend by the inserted length — this used
    // to be a hand-rolled copy of that splice here (V3a removed it).
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

// ---- the lens seam (AST-3, re-expressed on Views in V1): read-only code
//      views ---------------------------------------------------------------
// A LENS is the focused View re-represented as a code View: the stored text
// stays the document truth; what is DISPLAYED is the render (madc::emit
// over the live buffer, by the target's KIND), held in the View's own
// render buffer so the ONE shared navigation implementation (edit_key,
// find, goto-line) works over the display space unchanged. The View's
// coordinate map (EMPTY for a wholly rendered view: nothing corresponds;
// see ui::lens_to_display) rides onto the edit node's hints; the
// container's stored-space caret parks on "ocaret" and returns on exit;
// mutations and stored-space history are refused through the one editable
// gate. The identity lens (a source View) adds NOTHING — the composed tree
// stays byte-identical.

// The navigation target: the View's text entity — the render buffer while
// a lens is up, else the document (view_text_entity, the one routing rule).
long nav_doc(long w, long es, long doc)
{
    var v;
    view_row(v, w, es, focused_view(w, es));
    return view_text_entity(v, doc);
}

// Re-represent the focused View as a code View in `lang` (madc::fkMC11 /
// fkC11 / fkCPP — the kinds the emitter renders). False = the buffer does
// not translate (the compiler's own rows surface; the View is unchanged).
bool enter_lens(long w, long es, long doc, long lang)
{
    var t;
    ui::text(t, w, doc);
    var p;
    ui::get(p, w, doc, "path");
    var emitted;
    var vmap;			// V5: the source↔display coordinate map, beside the text
    if ( !madc::emit(emitted, vmap, t.c_str(), p.c_str(), lang) )
    {
	// The buffer does not translate: surface the compiler's own rows
	// instead of a bare refusal.
	run_check(w, es, doc);
	return false;
    }
    long id = ensure_focused_view(w, es, doc);
    var v;
    view_row(v, w, es, id);
    long vd = v["rbuf"].as_integer();
    if ( vd == 0 )
    {
	vd = ui::create(w, "view-buffer");
	v["rbuf"] = vd;
    }
    ui::text_load(w, vd, emitted.c_str());
    ui::set(w, vd, "path", p);	// the status %n seat: a lens is over the
				// file — its name IS the file's (a display
				// path only: the render buffer is no
				// document and carries no kind)
    // Colour on the lens (owner 2026-09-09: every emitted code view is
    // syntax-coloured): the render buffer carries its OWN highlight spans —
    // the lexer-alone classification (the colour-at-load path) over the
    // EMITTED text, through the one converter, landing on the render
    // buffer; the composer attaches the spans of whichever buffer it
    // shows (view_spans). The emitted text is already indented — the
    // emitter's own layout (src/cir_emit_c.cpp), one owner for --emit and
    // every lens.
    var vrows;
    madc::lex_spans(vrows, emitted.c_str(), p.c_str());
    spans_to_hspans(w, es, vd, vrows, vd);
    if ( v["kind"].as_integer() != viewCODE )
	ui::set(w, es, "ocaret", es_int(w, es, "caret", 0));	// the CONTAINER
				// parks its stored-space caret (never the row)
    v["kind"] = viewCODE;
    v["lang"] = lang;
    v["generator"] = genMADC;
    v["vbuf"] = vd;
    v["map"] = vmap;		// V5: fills the vmap that was EMPTY (design §2.6)
    view_store(w, es, v);
    ui::set(w, es, "caret", 0);
    ui::set(w, es, "mark", -1);
    ui::set(w, es, "bend", -1);
    ui::set(w, es, "editable", false);
    ui::set(w, es, "ro_msg", "Read-only view (^K A returns).");
    ui::set(w, es, "msg", format("View: {}", madc::file_kind_name(lang)));
    return true;
}

// Back to the source View: the row's representation returns to the
// subject's own text and kind; the container's parked caret returns.
void exit_lens(long w, long es, long doc)
{
    var v;
    if ( view_row(v, w, es, focused_view(w, es)) )
    {
	long subject = v["subject"].as_integer();
	v["kind"] = viewSOURCE;
	v["lang"] = doc_kind(w, subject);
	v["generator"] = genNONE;
	v["vbuf"] = subject;
	var nomap = {};
	v["map"] = nomap;
	view_store(w, es, v);
    }
    ui::set(w, es, "caret", es_int(w, es, "ocaret", 0));
    ui::set(w, es, "mark", -1);
    ui::set(w, es, "bend", -1);
    derive_access(w, es);	// the file's own verdict returns
}

// A STANDALONE code View of `subject` in `lang` (madc::fkMC11 / fkC11 /
// fkCPP): a NEW View with its own render buffer, coloured like the lens, the
// subject untouched — for a `viewsplit right mc11` second pane (source left,
// MC11 right) that does NOT re-represent the focused View. 0 = the subject
// does not translate. The focused re-representation stays enter_lens; this is
// its sibling for a fresh View (the emit / colour body is the same shape).
long make_code_view(long w, long es, long subject, long lang)
{
    var t;
    ui::text(t, w, subject);
    var p;
    ui::get(p, w, subject, "path");
    var emitted;
    var vmap;			// V5: the source↔display coordinate map
    if ( !madc::emit(emitted, vmap, t.c_str(), p.c_str(), lang) )
	return 0;
    long id = view_new(w, es, subject, viewCODE, lang);
    var v;
    view_row(v, w, es, id);
    long vd = ui::create(w, "view-buffer");
    ui::text_load(w, vd, emitted.c_str());
    ui::set(w, vd, "path", p);		// the status %n seat: a display path
    var vrows;
    madc::lex_spans(vrows, emitted.c_str(), p.c_str());
    spans_to_hspans(w, es, vd, vrows, vd);
    v["generator"] = genMADC;
    v["vbuf"] = vd;
    v["map"] = vmap;		// V5: the fresh code View carries its map
    view_store(w, es, v);
    return id;
}

// The edit-history View (client-server arc V4; design doc S2.4): a View of
// `doc` at a past revision `n` (an event seq), its text the change log
// replayed to n onto a fresh render buffer. revision = n (0 = current); a
// non-zero revision is a reconstructed past state, the subject untouched.
long make_history_view(long w, long es, long doc, long n)
{
    var t;
    clog_replay(t, w, doc, n);
    long id = view_new(w, es, doc, viewSOURCE, doc_kind(w, doc));
    var v;
    view_row(v, w, es, id);
    long vd = ui::create(w, "view-buffer");
    ui::text_load(w, vd, t.c_str());
    var p;
    ui::get(p, w, doc, "path");
    ui::set(w, vd, "path", p);
    v["revision"] = n;
    v["generator"] = genNONE;
    v["vbuf"] = vd;
    view_store(w, es, v);
    return id;
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
    doc_set_path(w, nd, path);
    ui::set(w, nd, "modified", false);
    return nd;
}

// The View a buffer row (an editor tab) shows; a row from before the table
// carried Views gets its source View here.
long buffer_view(long w, long es, long idx)
{
    var bufs;
    ui::get(bufs, w, es, "buffers");
    if ( bufs.is_null() || php::count(bufs) <= idx )
	return 0;
    var r = bufs[idx];
    var v = r["view"];
    if ( !v.is_null() && v.as_integer() != 0 )
	return v.as_integer();
    long vid = view_new(w, es, r["doc"].as_integer(), viewSOURCE,
			doc_kind(w, r["doc"].as_integer()));
    r["view"] = vid;
    bufs[idx] = r;
    ui::set(w, es, "buffers", bufs);
    return vid;
}

// Switch the active buffer: leave any view (it lenses the OLD buffer),
// save the old row's interaction state, restore the target's, colour
// immediately from the lexical spans and leave the FULL parse to the
// loop's paint-before-parse step (`pending_parse` — the black-startup
// rule, one mechanism for startup and switches).
void switch_buffer(long w, long es, long olddoc, long idx)
{
    var vw;
    if ( lens_name(vw, w, es) )
	exit_lens(w, es, olddoc);
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
    ui::set(w, es, "fview", buffer_view(w, es, idx));	// the focused
				// container's View is the tab's
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
    long vid = view_new(w, es, nd, viewSOURCE, doc_kind(w, nd));	// the tab's
				// View: the source View over its document
    bufs[] = { "doc": nd, "path": path, "caret": 0,
	       "mark": -1, "bend": -1, "view": vid };
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
	var op;
	ui::get(op, w, olddoc, "path");
	long lv = ensure_focused_view(w, es, olddoc);	// the live View too
	seed[] = { "doc": olddoc, "path": op,
		   "caret": es_int(w, es, "caret", 0),
		   "mark": -1, "bend": -1, "view": lv };
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

// The FIRST window (tree order) showing document `doc`; -1 when none.
// The active window's buffer is the LIVE es state (its row is saved
// only on a switch — window_save_active), the others' are their rows:
// the same active-live / inactive-row rule compose_ide_tree renders by.
long window_showing(long w, long es, long doc)
{
    var ws;
    ui::get(ws, w, es, "windows");
    var bufs;
    ui::get(bufs, w, es, "buffers");
    if ( ws.is_null() || bufs.is_null() )
	return -1;
    long at = es_int(w, es, "winat", 0);
    long i = 0;
    for ( var wr : ws )
    {
	long bi = i == at ? es_int(w, es, "bufidx", 0)
			  : wr["bufidx"].as_integer();
	var brow = bufs[bi];
	if ( brow["doc"].as_integer() == doc )
	    return i;
	i = i + 1;
    }
    return -1;
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
	seed[] = { "bufidx": es_int(w, es, "bufidx", 0),
		   "caret": es_int(w, es, "caret", 0), "grow": 0 };
	ws = seed;
	ui::set(w, es, "winat", 0);
    }
    else
	at = es_int(w, es, "winat", 0);
    var dup = { "bufidx": es_int(w, es, "bufidx", 0),
		"caret": es_int(w, es, "caret", 0), "grow": 0 };
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
	rows[] = { "path": lp, "ti": -1 };
	out = rows;
	return;
    }
    var tus = proj["tus"];
    long i = 0;
    for ( var t : tus )
    {
	// A bare string IS the file; an object TU names its file. The
	// kind question comes FIRST — a keyed access on a scalar is a
	// script error by design (madarray_key_slot). (An if/else, not a
	// ternary: carrier-armed ternaries are a banked lowering gap.)
	var tp;
	if ( t.is_string() )
	    tp = t;
	else
	    tp = t["file"];
	rows[] = { "path": tp, "ti": i };
	i = i + 1;
    }
    out = rows;
}

// The project's KIND (polish P2c): the manifest's "kind" — console
// (absent) or gui. It decides the Windows executable's subsystem on a
// project build (the engine reads the manifest itself) and where Run sends
// the program's output. The manifest's word converts HERE (ide_projkind).
long proj_kind(long w, long es)
{
    var proj;
    ui::get(proj, w, es, "proj");
    if ( proj.is_null() )
	return pkCONSOLE;
    var k = proj["kind"];
    return projkind_of(k);
}

// A manifest is OPEN (loaded or materialized): the kind row shows, the
// build rows go project-wide. (project_active is the WINDOW being up — a
// different fact.)
bool proj_has_manifest(long w, long es)
{
    var pf;
    ui::get(pf, w, es, "projfile");
    return !pf.is_null() && strlen(pf.c_str()) > 0;
}

// The change-log file beside the manifest (client-server arc V4; design doc
// S2.4): <base>.prj.events, a sibling of the layout cache. Only once a
// manifest is open -- the implicit single-file project is memory-only (the
// no-surprise-artifact law, design S6). false = no manifest.
bool proj_events_path(var &out, long w, long es)
{
    if ( !proj_has_manifest(w, es) )
	return false;
    var pf;
    ui::get(pf, w, es, "projfile");
    var base;
    path_sans_ext(base, pf);
    out = format("{}.events", base);
    return true;
}

// Flush the doc's change log to <base>.prj.events (the whole buffer -> the
// file; the buffer text IS the file's content -- one serialisation). A
// no-op with no manifest. Follow-ups: also flush on 	S doc-save; unify to
// one project events file across docs for a multi-file manifest (the log is
// per-doc today, single-file being V4's gated case).
void clog_persist(long w, long es, long doc)
{
    var path;
    if ( !proj_events_path(path, w, es) )
	return;
    long le = es_int(w, doc, "changelog", 0);
    if ( le <= 0 )
	return;
    var blob;
    ui::text(blob, w, le);
    php::file_put_contents(path.c_str(), blob.c_str());
}

// Restore the doc's change log from <base>.prj.events on project open; seq
// resumes at the last record's, so new events continue the sequence. A
// no-op with no manifest or no file.
void clog_restore(long w, long es, long doc)
{
    var path;
    if ( !proj_events_path(path, w, es) )
	return;
    var blob;
    if ( !php::file_get_contents(blob, path.c_str()) )
	return;
    long le = changelog_of(w, doc);
    ui::text_load(w, le, blob.c_str());
    var lines;
    php::explode(lines, "\n", blob.c_str());
    long hi = 0;
    for ( var ln : lines )
    {
	if ( strlen(ln.c_str()) == 0 )
	    continue;
	var rec;
	if ( js::parse(rec, ln) )
	    hi = rec["seq"].as_integer();
    }
    ui::set(w, le, "seq", hi);
}

// Toggle console <-> gui and persist the manifest AT ONCE (the immediate-
// persistence rule) — the ^T Options row's action (opt-projkind). Only a
// project with a manifest has a kind to set; the implicit project refuses
// with the reason (its manifest materializes on the second file).
void proj_toggle_kind(long w, long es, long doc)
{
    var proj;
    ui::get(proj, w, es, "proj");
    if ( proj.is_null() )
    {
	ui::set(w, es, "msg",
		"No project manifest yet — add a second file (^K I) first.");
	return;
    }
    long nk = proj_kind(w, es) == pkCONSOLE ? pkGUI : pkCONSOLE;
    proj["kind"] = projkind_name(nk);	// the manifest file's word
    ui::set(w, es, "proj", proj);
    proj_touch(w, es);
    if ( proj_write(w, es, doc) )
	ui::set(w, es, "msg", format("Project kind: {}.", projkind_name(nk)));
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
    clog_persist(w, es, doc);
    return true;
}

// Persist the live layout BESIDE the manifest (V2c) — silently (a layout
// change carries its own status message; the file is a convenience cache, not
// a document). ONLY when a manifest is open: the implicit single-file project
// writes no artifact (the no-surprise-artifact law, design §6). Called by the
// layout mutators (viewdock today; the splitter's `layout` event next).
void layout_persist(long w, long es)
{
    if ( !proj_has_manifest(w, es) )
	return;
    var pf;
    ui::get(pf, w, es, "projfile");
    if ( pf.is_null() || strlen(pf.c_str()) == 0 )
	return;
    var lf;
    proj_layout_path(lf, pf);
    var lo;
    layout_get(lo, w, es);
    var txt;
    layout_to_text(txt, lo);
    php::file_put_contents(lf.c_str(), txt.c_str());
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

// THE manifest reader (polish P2b factored it out of the startup read so
// File -> Open Project… and run_ide's startup are ONE reader): load the
// manifest at `pf` onto the bag ("proj" + "projfile"), bump the project
// generation, announce. Malformed content refuses LOUD and leaves the
// session as it was. False = not loaded (the message says why).
bool proj_load(long w, long es, var &pf)
{
    var txt;
    if ( !php::file_get_contents(txt, pf.c_str()) )
    {
	ui::set(w, es, "msg", format("Cannot read {}.", pf));
	return false;
    }
    var proj;
    if ( !js::parse(proj, txt) )
    {
	ui::set(w, es, "msg", format("{} is not valid JSON.", pf));
	return false;
    }
    if ( !proj.is_object() )
    {
	// Valid JSON but not the native OBJECT shape (a scalar, or a
	// compile_commands array — the read-only import is --project's
	// lane, not the window's).
	ui::set(w, es, "msg",
		format("{} is not a madc project manifest.", pf));
	return false;
    }
    var tus = proj["tus"];
    if ( tus.is_null() )
    {
	ui::set(w, es, "msg",
		format("{} has no \"tus\" — not a project manifest.", pf));
	return false;
    }
    ui::set(w, es, "proj", proj);
    ui::set(w, es, "projfile", pf);
    proj_touch(w, es);
    // A persisted layout beside the manifest (V2c) restores the client's
    // docked panes / band sizes. A missing file keeps the current layout; a
    // malformed one is ignored (the cache never blocks a project open).
    var lf;
    proj_layout_path(lf, pf);
    if ( php::file_exists(lf) )
    {
	var ltxt;
	if ( php::file_get_contents(ltxt, lf.c_str()) )
	{
	    var newlo;
	    var lerr;
	    if ( parse_layout(newlo, ltxt.c_str(), lerr) )
		ui::set(w, es, "layout", newlo);
	}
    }
    ui::set(w, es, "msg", format("Project {}: {} TU(s).", pf,
				 php::count(tus)));
    return true;
}

// Startup manifest read (run_ide): a <base>.prj.json beside the launch
// file IS the project — persistence would be pointless if nothing read
// it back. No file = the implicit single-file project.
void proj_startup(long w, long es, long doc)
{
    var lp;
    proj_launch_path(lp, w, es, doc);
    var base;
    path_sans_ext(base, lp);
    var pf = format("{}.prj.json", base);
    if ( !php::file_exists(pf) )
	return;
    proj_load(w, es, pf);
    clog_restore(w, es, doc);
}

// File -> Open Project… (polish P2b): the manifest the user chose (the
// native dialog's answer, or the prompt's) becomes THE project — the same
// reader run_ide's startup uses — and the Project window opens on it. An
// empty path (a cancelled dialog) does nothing.
void proj_open(long w, long es, long doc, const char *path0)
{
    var pf = path0;		// ring discipline: own the text first
    if ( strlen(pf.c_str()) == 0 )
	return;
    if ( !proj_load(w, es, pf) )
	return;
    clog_restore(w, es, doc);
    proj_show(w, es, doc);
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
	rows[] = { "path": p, "tag": "prj", "ti": tr["ti"],
		   "content": format("p{} {}", mk, p) };
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
	rows[] = { "path": p, "tag": "open",
		   "content": format("o{} {}", mk, p) };
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
		rows[] = { "path": p, "tag": "cwd",
			   "content": format("   {}", p) };
	    }
	}
    }
    ui::set(w, es, "projrows", rows);
}

void proj_show(long w, long es, long doc)
{
    set_pane(w, es, panePROJECT);
    ui::set(w, es, "projfilter", "");
    project_rows(w, es, doc);
}

void proj_hide(long w, long es)
{
    set_pane(w, es, paneNONE);
    ui::set(w, es, "projfilter", "");
}

bool project_active(long w, long es)
{
    return cur_pane(w, es) == panePROJECT;
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
    var kind = ev["event_code"];
    if ( kind == ui::event_kind::text )
    {
	var in;
	ui::get(in, w, es, "projfilter");
	var nx = format("{}{}", in, ev["text"]);
	ui::set(w, es, "projfilter", nx.c_str());
	project_rows(w, es, doc);
	return true;
    }
    if ( kind == ui::event_kind::choose )
    {
	var a;
	var ke = { "event": "key", "event_code": ui::event_kind::key,
		   "key": "enter", "key_code": ui::key::enter };
	if ( !scoped_action(a, w, es, "project", ke) )
	    return true;
	if ( a == cmdPROJOPEN )
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
    if ( kind == ui::event_kind::action )
    {
	long a = ev_code(ev);
	if ( a == cmdPROJECT || a == cmdQUIT )
	    proj_hide(w, es);
	else if ( a == cmdPROJADDCUR )
	{
	    proj_addcur(w, es, doc);
	    project_rows(w, es, doc);
	}
	else
	{
	    // The dialog's Close button / a press outside it (polish P2):
	    // the @project cancel posted BY NAME.
	    var pn;
	    if ( scope_action_named(pn, w, es, "project", ev) && pn == cmdPROJCLOSE )
		proj_hide(w, es);
	}
	return true;
    }
    if ( kind != ui::event_kind::key )
	return true;
    var a;
    if ( !scoped_action(a, w, es, "project", ev) )
	return true;			// unbound keys are inert
    if ( a == cmdPROJCLOSE )
	proj_hide(w, es);
    else if ( a == cmdPROJDELCHAR )
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
    else if ( a == cmdPROJADD )
	start_prompt(w, es, "File to add to project", pmPROJADD);
    else if ( a == cmdPROJDEL )
    {
	var opt = ev["option"];
	if ( opt.is_null() )
	    ui::set(w, es, "msg", "No row focused.");
	else
	    proj_del(w, es, doc, opt.as_integer());
    }
    return true;
}

// ---- the vi MODE: modes-as-scopes (owner ruling 2026-08-31) -------------
// vim/neovim is MODAL: normal mode's printable keys are COMMANDS, insert
// mode types. The substrate is the @scope key tables (the design doc's
// modes-as-scopes): @normal/@insert are profile DATA through the one
// parse_keys — profiles/neovim.keys is their home — and the session's
// vi arm (IdeSession::vi_event) routes the MAIN key stream through
// them; the engine's chord table (control keys) stays live in every
// mode. Scoped keys match AS WRITTEN, so the data itself spells the vi
// case alphabet (a/A, i/I, o/O, x/X). The GRAMMAR — counts, pending
// operator + motion, doubled operators (dd/cc) — is machinery here; the
// key->name associations stay data (the keybinding law). A profile that
// carries @normal data IS a modal personality: it loads in normal mode;
// the modes palette's `v` row (action `vimode`) toggles modal editing
// anywhere the data exists. Deliberate simplifications, documented and
// never pretended around: operator spans are BYTE-wise over the motion
// (dj is a char span, not two lines), no registers/put (y/p), no text
// objects (iw), no repeat (.), and the caret is the shared byte caret
// (it may sit one past a line's last character where vim's would not).
//
// State bags: "vimode" (ide_vimode: viOFF / viNORMAL / viINSERT), "vicount" (the digits so
// far), "viop"+"viopn" (the pending operator and its stashed count —
// 2d3w multiplies to 6), "vipend" (the literal echo, vim's showcmd —
// the %k seat renders it when no engine chord is in flight).

bool vi_active(long w, long es)
{
    return cur_vimode(w, es) != viOFF;
}

void vi_pending_clear(long w, long es)
{
    ui::set(w, es, "vicount", "");
    ui::set(w, es, "viop", (long)cmdNONE);	// the pending operator's code
    ui::set(w, es, "viopn", 0);
    ui::set(w, es, "vipend", "");
}

void vi_enter(long w, long es, long mode)
{
    set_vimode(w, es, mode);
    vi_pending_clear(w, es);
}

bool vi_pending(long w, long es)
{
    var c;
    ui::get(c, w, es, "vicount");
    if ( !c.is_null() && strlen(c.c_str()) > 0 )
	return true;
    if ( es_int(w, es, "viop", cmdNONE) != cmdNONE )	// an operator pends
	return true;
    return false;
}

// The showcmd echo: every consumed grammar character appends.
void vi_echo_push(long w, long es, const char *c)
{
    var p;
    ui::get(p, w, es, "vipend");
    if ( p.is_null() )
	p = "";
    ui::set(w, es, "vipend", format("{}{}", p, c));
}

// The motion VOCABULARY an operator composes over — the shared movement
// action names (the dispatcher's own arms + edit_key's key spellings).
bool vi_is_motion(long a)
{
    switch ( a )
    {
	case cmdLEFT: case cmdRIGHT: case cmdUP: case cmdDOWN:
	case cmdHOME: case cmdEND: case cmdPGUP: case cmdPGDN:
	case cmdWORDLEFT: case cmdWORDRIGHT:
	case cmdTOP: case cmdBOTTOM: case cmdVIGOTO:
	    return true;
	default:
	    break;
    }
    return false;
}

// The operator payload: erase [lo, hi) as ONE undo step, caret to lo —
// block_del's splice shape without the block markers or the clip.
void vi_span_del(long w, long es, long doc, long lo, long hi)
{
    if ( hi <= lo )
	return;
    if ( !es_flag(w, es, "editable") )
    {
	refuse_edit(w, es);
	return;
    }
    edit_checkpoint(w, es, doc, lo);
    ed_text_erase(w, es, doc, lo, hi - lo);
    ui::set(w, es, "caret", lo);
    ui::set(w, doc, "modified", true);
}

// Toggle modal editing (the palette's `v` row, or a profile's own
// `vimode` binding). Entering needs @normal key data — the neovim
// profile carries it; a profile without any refuses with the road there.
void vi_toggle(long w, long es)
{
    if ( vi_active(w, es) )
    {
	vi_enter(w, es, viOFF);
	ui::set(w, es, "msg", "vi mode off.");
	return;
    }
    var mk;
    ui::get(mk, w, es, "modekeys");
    var nt;
    if ( !mk.is_null() )
	nt = mk["normal"];
    if ( nt.is_null() )
    {
	ui::set(w, es, "msg",
		"No @normal keys in this profile (^T Keymap row: neovim).");
	return;
    }
    vi_enter(w, es, viNORMAL);
    ui::set(w, es, "msg", "vi NORMAL mode (i inserts, ^N v leaves).");
}

// ---- the MODES palette (owner ruling 2026-08-31) ------------------------
// "No reason our editor/ide cannot have modes": the `modes` action (joe
// seats it on ^N — profile data) raises the one popup-list widget over
// the available editing MODES; the keys inside it are @modes PROFILE
// DATA through the one parse_keys (baked defaults: ":" enters the vi
// colon line, "v" toggles vi modal editing, enter opens the focused
// row, esc dismisses). ^N : is two keystrokes into colon mode from a
// non-modal personality — and it is the only legal spelling: the
// engine's chord tables refuse a sequence that shadows a shorter
// binding, so ^n-the-palette and ^n-:-the-chord cannot coexist in the
// main table. A modal personality enters colon from its own @normal ":"
// instead — one colon implementation either way.

// The rows as data ({act, content} — precomposed like project_rows).
void modes_show(long w, long es)
{
    var rows = {};
    rows[] = { "act": cmdCOLON,
	       "content": " :  vi command line — one ex command (w q e N !cmd ...)" };
    rows[] = { "act": cmdVIMODE,
	       "content": " v  vi normal/insert editing — toggle modal mode" };
    ui::set(w, es, "modesrows", rows);
    set_pane(w, es, paneMODES);
}

void modes_hide(long w, long es)
{
    set_pane(w, es, paneNONE);
}

bool modes_active(long w, long es)
{
    return cur_pane(w, es) == paneMODES;
}

// Enter the mode a palette row (or a @modes key) names. The palette
// closes first — a mode takes the whole stage. False = quit (none of
// the current modes quit; the contract rides the dispatch).
bool modes_dispatch(long w, long es, long doc, long caret, long a)
{
    if ( a == cmdCOLON )
    {
	modes_hide(w, es);
	start_prompt(w, es, "Command", pmCOLON);
	return true;
    }
    if ( a == cmdVIMODE )
    {
	modes_hide(w, es);
	vi_toggle(w, es);
	return true;
    }
    var an;
    cmd_name(an, a);
    ui::set(w, es, "msg", format("Unknown mode action '{}'.", an));
    return true;
}

// Every event while the modes palette is up (modal, the project window's
// shape): single characters and keys dispatch through the @modes scope's
// looked-up NAMES (colon / modesopen / modesclose) — never the keys
// themselves; enter's meaning is data too (the engine CHOOSES, @modes'
// enter row names what a choose does). Other bound actions are
// swallowed; `modes` itself (and quit) toggles the palette shut.
bool modes_event(long w, long es, long doc, long caret, var &ev)
{
    var kind = ev["event_code"];
    if ( kind == ui::event_kind::text )
    {
	var a;
	if ( scoped_action(a, w, es, "modes", ev) )
	    return modes_dispatch(w, es, doc, caret, a.as_integer());
	return true;			// other typing is inert
    }
    if ( kind == ui::event_kind::choose )
    {
	var a;
	var ke = { "event": "key", "event_code": ui::event_kind::key,
		   "key": "enter", "key_code": ui::key::enter };
	if ( !scoped_action(a, w, es, "modes", ke) )
	    return true;
	if ( a == cmdMODESOPEN )
	{
	    var rows;
	    ui::get(rows, w, es, "modesrows");
	    long opt = ev["option"].as_integer() - 1;	// 1-based wire
	    if ( opt >= 0 && opt < php::count(rows) )
	    {
		var r = rows[opt];
		long ra = r["act"].as_integer();
		return modes_dispatch(w, es, doc, caret, ra);
	    }
	}
	return true;
    }
    if ( kind == ui::event_kind::action )
    {
	long a = ev_code(ev);
	if ( a == cmdMODES || a == cmdQUIT )
	    modes_hide(w, es);
	else
	{
	    // The dialog's Close button / a press outside it (polish P2):
	    // the @modes cancel posted BY NAME.
	    var mn;
	    if ( scope_action_named(mn, w, es, "modes", ev) && mn == cmdMODESCLOSE )
		modes_hide(w, es);
	}
	return true;
    }
    if ( kind != ui::event_kind::key )
	return true;
    var a;
    if ( !scoped_action(a, w, es, "modes", ev) )
	return true;			// unbound keys are inert
    if ( a == cmdMODESCLOSE )
    {
	modes_hide(w, es);
	return true;
    }
    if ( a == cmdMODESOPEN )
	return true;			// enter means CHOOSE; the engine
					// hands it back as one when a row
					// is focused
    return modes_dispatch(w, es, doc, caret, a.as_integer());
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

// The [build] buffer if it already exists, else 0 — a NON-creating lookup
// for read-only callers (the composer's live-build panel), so composition
// never mints a buffer as a side effect.
// ONE finder for a pseudo-buffer by its bracketed name ([build],
// [terminal]) — non-creating: composition never mints a buffer.
long buffer_doc_named(long w, long es, const char *name)
{
    var bufs;
    ui::get(bufs, w, es, "buffers");
    long n = php::count(bufs);
    for ( long i = 0; i < n; i = i + 1 )
    {
	var r = bufs[i];
	if ( r["path"] == name )
	    return r["doc"].as_integer();
    }
    return 0;
}

long build_doc_if_any(long w, long es)
{
    return buffer_doc_named(w, es, "[build]");
}

// The [build] buffer: found-or-created in the buffer table WITHOUT
// switching to it (the pump appends in the background; ^P reaches it).
long build_doc(long w, long es)
{
    long existing = build_doc_if_any(w, es);	// ONE finder, no duplicate scan
    if ( existing != 0 )
	return existing;
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
    // The capture SOURCE (polish P3b-1): a URI the registry opens —
    // exec://<cmd> for a build command, madcrun://<handle> for the live
    // parse run in a child, madcproj://<manifest> for the project run —
    // one pump for every stream the Output tab shows.
    var uri;
    ui::get(uri, w, es, "buildsrc");
    if ( uri.is_null() || strlen(uri.c_str()) == 0 )
    {
	var cmd;
	ui::get(cmd, w, es, "buildcmd");
	uri = format("exec://{}", cmd);
    }
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
    // The child's exit status (a run's verdict; a build tool's rc) rides
    // the stream's last line — what the terminal's Run reports on its
    // status line.
    long xc = bc.exit_status();
    if ( stopped )
	append_build_line(w, es, bdoc, "[stopped]");
    else if ( xc >= 0 )
    {
	var xl = format("[exit {}]", xc);
	append_build_line(w, es, bdoc, xl.c_str());
    }
    else
	append_build_line(w, es, bdoc, "[done]");
    ui::set(w, es, "buildlive", 0);
}

// ONE capture starter for every stream the [build] buffer / the Output
// tab shows (polish P3b-1): a build command (exec://), a run of the live
// parse in a child (madcrun://), the project run (madcproj://). `title`
// heads the stream; the pump appends the child's exit status at its end.
void start_capture(long w, long es, const char *title, const char *uri)
{
    var t = title;			// ring discipline: own the texts first
    var u = uri;
    if ( es_int(w, es, "buildlive", 0) != 0 )
    {
	ui::set(w, es, "msg", "A build is already running (Stop first).");
	return;
    }
    long bdoc = build_doc(w, es);
    var hdr = format("=== {} ===", t);
    append_build_line(w, es, bdoc, hdr.c_str());
    long stopc = madc::chan_make(1);
    ui::set(w, es, "buildstop", stopc);
    ui::set(w, es, "buildsrc", u);
    ui::set(w, es, "buildlive", 1);
    show_output(w, es);
    go build_pump(w, es, bdoc, stopc);
    ui::set(w, es, "msg", format("{} — the Output tab shows it.", t));
}

void start_build(long w, long es, const char *cmd)
{
    var c = cmd;			// ring discipline
    ui::set(w, es, "buildcmd", c);
    var uri = format("exec://{}", c);
    start_capture(w, es, c.c_str(), uri.c_str());
}

// ---- the embedded TERMINAL (polish P3b-2) --------------------------------
// The panel's Terminal tab: a program on a pseudo-terminal — a console
// program run from the window (madcrun://<handle>?pty), the project run
// (madcproj://<manifest>?pty), a manifest command in terminal mode or the
// Shell (pty://<cmd>). The child's bytes fold into the [terminal] buffer
// through ui::term_feed (the bounded screen: \r overwrite, \b, tabs,
// CSI K / 2J; other sequences dropped); what the user types while the
// terminal has focus (`termfocus`) travels the one input path to the core,
// which sends the BYTES (ui::key_bytes — the inverse of the terminal's own
// key parser) down a value channel the pump selects on (`termin`) and
// writes to the pty. State on the bag: termlive, termfocus, termin,
// termstop, termsrc. ^] (the @terminal scope's one binding, profile data)
// gives the keyboard back; a press in an editor window does too.
long term_doc_if_any(long w, long es)
{
    return buffer_doc_named(w, es, "[terminal]");
}

long term_doc(long w, long es)
{
    long existing = term_doc_if_any(w, es);
    if ( existing != 0 )
	return existing;
    long nd = open_buffer_doc(w, es, "[terminal]");
    push_buffer_row(w, es, nd, "[terminal]");
    return nd;
}

void term_feed_text(long w, long tdoc, const char *text)
{
    var t = text;			// ring discipline
    ui::term_feed(w, tdoc, t.c_str(), strlen(t.c_str()));
    ui::set(w, tdoc, "modified", false);
}

// The pump: the MT-4b select over {stop, the pty's readable bytes, the
// keyboard channel}. Bytes read fold into the screen; a keyboard value is
// written to the child as-is (already bytes); the stop channel SIGTERMs
// the child (cancel) so close() reaps promptly. EOF = the program ended;
// its exit status ends the screen.
void term_pump(long w, long es, long tdoc, long stopc, long inc)
{
    var uri;
    ui::get(uri, w, es, "termsrc");
    madc::channel tc(uri.c_str());
    if ( !tc.ok() )
    {
	var em = format("[terminal] cannot start: {}\n", tc.last_error());
	term_feed_text(w, tdoc, em.c_str());
	ui::set(w, es, "termlive", 0);
	return;
    }
    // A pty gives the program a line discipline (echo, CR->LF); on pipes
    // (Windows until ConPTY) the pump emulates the two a console program
    // needs: what was typed is echoed onto the screen and Enter ends the
    // line with LF, or fgets would wait forever for a newline.
    bool tty = tc.is_terminal();
    var cases;
    cases[0] = stopc;
    cases[1] = madc::chan_readable(tc);
    cases[2] = inc;
    var got;
    bool stopped = false;
    char buf[4096];
    for ( ;; )
    {
	long idx = madc::chan_select(got, cases);
	if ( idx == 0 )
	{
	    stopped = true;
	    tc.cancel();
	    break;
	}
	if ( idx == 2 )
	{
	    if ( tty )
		tc.write(got);		// the keystrokes' bytes to the child
	    else
	    {
		var b = got;
		if ( b == "\r" )
		    b = "\n";
		ui::term_feed(w, tdoc, b.c_str(), strlen(b.c_str()));
		ui::set(w, tdoc, "modified", false);
		tc.write(b);
	    }
	    continue;
	}
	if ( idx != 1 )
	    break;			// every case dead
	long n = tc.read(buf, 4096);
	if ( n <= 0 )
	    break;			// EOF: the program ended
	ui::term_feed(w, tdoc, buf, n);
	ui::set(w, tdoc, "modified", false);
    }
    tc.close();
    long xc = tc.exit_status();
    if ( stopped )
	term_feed_text(w, tdoc, "\n[stopped]\n");
    else if ( xc >= 0 )
    {
	var xl = format("\n[exit {}]\n", xc);
	term_feed_text(w, tdoc, xl.c_str());
    }
    else
	term_feed_text(w, tdoc, "\n[done]\n");
    ui::set(w, es, "termlive", 0);
}

// Start a program in the Terminal tab: the screen shows the title, the
// tab shows, the keyboard is the program's until it ends or ^] hands it
// back. One program at a time (^C it — the tty delivers SIGINT).
void start_terminal(long w, long es, const char *title, const char *uri)
{
    var t = title;			// ring discipline: own the texts first
    var u = uri;
    if ( es_int(w, es, "termlive", 0) != 0 )
    {
	ui::set(w, es, "msg", "A program is already running in the Terminal (^C it first).");
	return;
    }
    long tdoc = term_doc(w, es);
    var hdr = format("=== {} ===\n", t);
    term_feed_text(w, tdoc, hdr.c_str());
    long stopc = madc::chan_make(1);
    long inc = madc::chan_make(64);
    ui::set(w, es, "termstop", stopc);
    ui::set(w, es, "termin", inc);
    ui::set(w, es, "termsrc", u);
    ui::set(w, es, "termlive", 1);
    ui::set(w, es, "termfocus", 1);
    show_view(w, es, viewTERMINAL);
    go term_pump(w, es, tdoc, stopc, inc);
    ui::set(w, es, "msg", format("{} — in the Terminal (^] returns the keyboard).", t));
}

// Send bytes to the running program (the pump writes them to the pty).
void term_send(long w, long es, var &bytes)
{
    long inc = es_int(w, es, "termin", 0);
    if ( inc != 0 && es_int(w, es, "termlive", 0) != 0 )
	madc::chan_send(inc, bytes);
}

// The keyboard while the Terminal has focus: text is its own bytes; a
// key is the bytes a terminal would send for it (ui::key_bytes); a chord
// the global table resolved to an action is the keys the user typed — its
// sequence's spellings, each to bytes — except the @terminal scope's own
// binding (^] termunfocus, profile data), which hands the keyboard back.
// True = consumed. Pointer, choose, dialog and focus events pass through
// (a press elsewhere is how the mouse leaves the terminal).
bool terminal_event(long w, long es, var &ev)
{
    var kind = ev["event_code"];
    if ( kind == ui::event_kind::text )
    {
	var t = ev["text"];
	term_send(w, es, t);
	return true;
    }
    if ( kind == ui::event_kind::key || kind == ui::event_kind::action )
    {
	var a;
	if ( scoped_action(a, w, es, "terminal", ev) && a == cmdTERMUNFOCUS )
	{
	    ui::set(w, es, "termfocus", 0);
	    ui::set(w, es, "msg", "Keyboard back to the editor.");
	    return true;
	}
	var spell;
	if ( kind == ui::event_kind::key )
	    spell = format("{}", ev["key"]);
	else
	    spell = format("{}", ev["seq"]);
	if ( strlen(spell.c_str()) == 0 )
	    return true;
	// A chord's spellings are space-separated: each key's bytes in turn.
	array keys;
	php::explode(keys, " ", spell.c_str());
	long n = php::count(keys);
	for ( long i = 0; i < n; i = i + 1 )
	{
	    var one = php::array_get_cstr(keys, i);
	    var bytes;
	    if ( ui::key_bytes(bytes, one.c_str()) )
		term_send(w, es, bytes);
	}
	return true;
    }
    return false;
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
    if ( cur_pane(w, es) == paneNONE )
	show_diags(w, es);
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
    show_output(w, es);
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

bool save_and_check(long w, long es, long doc);	// defined with the actions below

// A native file dialog request (S4): the SAME parked-request slot the
// terminal requests ride (kinds are data; the client dispatches by kind),
// gated on the client's own fact (hasdialogs — a window whose host draws
// chrome pushes it; the terminal and a headless session never do, so
// their open / save-as keep prompting exactly as before). The answer
// returns as a dialog EVENT the dispatcher handles (dialog_result).
bool dialog_request(long w, long es, long dlg, const char *title,
		    var &initial)
{
    if ( !es_flag(w, es, "hasdialogs") )
	return false;
    // The host speaks the dialog MODE (open | save — ui::dialog_mode's
    // names, the wire word); the IDE's own KIND — what the answer means —
    // rides the request for the fallback prompt and waits on the bag for
    // the answer (dialog_result).
    var rq = { "kind": rqFILEDIALOG, "dlg": dlg, "mode": dlg_host_mode(dlg),
	       "title": title, "path": initial, "pause": 0 };
    ui::set(w, es, "dlgkind", dlg);
    ui::set(w, es, "termreq", rq);
    return true;
}

// Save the buffer under a NEW path (Save As…): the document's path moves,
// its buffer row follows (the ring addresses buffers by path), then the
// one write verb runs — the same `w` every save uses.
bool save_as(long w, long es, long doc, const char *path0)
{
    var p = path0;			// ring discipline: own the text first
    if ( strlen(p.c_str()) == 0 )
	return true;
    doc_set_path(w, doc, p.c_str());	// the kind follows the new name
    var bufs;
    ui::get(bufs, w, es, "buffers");
    if ( !bufs.is_null() )
    {
	long n = php::count(bufs);
	for ( long i = 0; i < n; i = i + 1 )
	{
	    var r = bufs[i];
	    if ( r["doc"].as_integer() == doc )
	    {
		r["path"] = p;
		bufs[i] = r;
	    }
	}
	ui::set(w, es, "buffers", bufs);
    }
    return save_and_check(w, es, doc);
}

// A native file dialog answered (the dialog event, S4): open edits the
// chosen file (the same path editfile's prompt takes), save writes the
// buffer under it; an empty path is the user's cancel.
void dialog_result(long w, long es, long doc, var &ev)
{
    long dlg = es_int(w, es, "dlgkind", dlgOPEN);	// the request this answers
    var path = ev["path"];
    if ( path.is_null() || strlen(path.c_str()) == 0 )
    {
	ui::set(w, es, "msg", "Cancelled.");
	return;
    }
    switch ( dlg )
    {
	case dlgSAVE:
	    save_as(w, es, doc, path.c_str());
	    return;
	case dlgOPENPROJECT:
	    proj_open(w, es, doc, path.c_str());
	    return;
	default:
	    break;
    }
    edit_file(w, es, doc, path.c_str());
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
	show_diags(w, es);
	ui::set(w, es, "msg",
		format("Run refused: {} error(s) — fix them first.", errs));
	return;
    }
    long h = es_int(w, doc, "phandle", 0);
    if ( h == 0 )
	return;			// ensure_phandle already said why
    if ( es_flag(w, es, "haspanel") )
    {
	// A window (polish P3b): the live parse runs in a child — the
	// running madc is still the compiler (the tree is forked, nothing
	// execs). By the project KIND: a console program runs on a pty in
	// the Terminal tab with its keyboard; a gui program opens its own
	// window and its stdout/stderr stream into Output. The terminal
	// below keeps the inherited-stdio handoff.
	var rp;
	ui::get(rp, w, doc, "path");
	var title = format("Run {}", rp);
	if ( proj_kind(w, es) == pkGUI )
	{
	    var uri = format("madcrun://{}", h);
	    start_capture(w, es, title.c_str(), uri.c_str());
	}
	else
	{
	    var uri = format("madcrun://{}?pty", h);
	    start_terminal(w, es, title.c_str(), uri.c_str());
	}
	return;
    }
    var rq = { "kind": rqRUN, "doc": doc, "pause": 1 };
    if ( !term_request(w, es, rq) )
	ui::set(w, es, "msg", "No terminal to suspend.");
}

// terminal-mode rows (Run native): the JOE ^K Z shape — the program
// gets the REAL terminal; a return-key pause holds its output on screen
// before the full-repaint resume.
void run_in_terminal(long w, long es, const char *cmd)
{
    if ( es_flag(w, es, "haspanel") )
    {
	// A window: a terminal-mode command runs on a pty in the Terminal tab.
	var c = cmd;
	var uri = format("pty://{}", c);
	start_terminal(w, es, c.c_str(), uri.c_str());
	return;
    }
    var rq = { "kind": rqCMD, "cmd": cmd, "pause": 1 };
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
	    rows[] = { "file": t["file"], "line": 1, "column": 0,
		       "severity": "error",
		       "severity_code": madc::diag_severity::error,
		       "message": "cannot open this TU (file or manifest options)" };
	    continue;
	}
	var trows;
	madc::parse_check(trows, th);
	for ( var r : trows )
	{
	    r["file"] = t["file"];
	    rows[] = r;
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
    show_diags(w, es);
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
    if ( cur_pane(w, es) == paneNONE )
	show_diags(w, es);
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
    show_output(w, es);
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
	show_diags(w, es);
	ui::set(w, es, "msg",
		format("Run refused: {} error(s) — fix them first.", errs));
	return;
    }
    if ( es_flag(w, es, "haspanel") )
    {
	// A window: the --project JIT lane in a child — the Terminal tab
	// for a console project, Output for a gui one (the kind).
	var title = format("Run project {}", pf);
	if ( proj_kind(w, es) == pkGUI )
	{
	    var uri = format("madcproj://{}", pf);
	    start_capture(w, es, title.c_str(), uri.c_str());
	}
	else
	{
	    var uri = format("madcproj://{}?pty", pf);
	    start_terminal(w, es, title.c_str(), uri.c_str());
	}
	return;
    }
    var rq = { "kind": rqPROJRUN, "pause": 1 };
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
	rows[] = { "name": "Check project (in-process)",
		   "act": cmdBLD_PROJ_CHECK };
	rows[] = { "name": "Build project (in-process)", "kind": "exe",
		   "out": pout, "act": cmdBLD_PROJ_BUILD };
	// bld-proj-run = the --project JIT lane, forked
	rows[] = { "name": "Run project", "act": cmdBLD_PROJ_RUN };
	rows[] = { "name": "Run native", "cmd": format("./{}", pout),
		   "mode": bmTERMINAL, "act": cmdBLD_RUN_CMD };
	rows[] = { "name": "Stop", "act": cmdBLD_STOP };
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
		// (An if/else default, not a ternary: mixed scalar/carrier
		// ternary arms are a banked lowering gap.)
		var cmw = c["mode"];		// the manifest's word
		long cm = bmode_of(cmw);	// converted where the row is built
		rows[] = { "name": cn, "cmd": cc, "mode": cm,
			   "act": cmdBLD_RUN_CMD };
	    }
	}
	ui::set(w, es, "buildcmds", rows);
	return;
    }
    rows[] = { "name": "Check (in-process)", "act": cmdBLD_CHECK };
    rows[] = { "name": "Build (in-process)", "kind": "exe",
	       "out": "{base}", "act": cmdBLD_NATIVE };
    rows[] = { "name": "Build object (in-process)", "kind": "obj",
	       "out": "{base}.o", "act": cmdBLD_NATIVE };
    // bld-run = fork the LIVE parse (madc::parse_run)
    rows[] = { "name": "Run", "act": cmdBLD_RUN };
    rows[] = { "name": "Run native", "cmd": "./{base}",
	       "mode": bmTERMINAL, "act": cmdBLD_RUN_CMD };
    rows[] = { "name": "Stop", "act": cmdBLD_STOP };
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
    long a = r["act"].as_integer();
    set_pane(w, es, paneNONE);		// the palette closes on choose
    switch ( a )
    {
	case cmdBLD_CHECK:
	    run_check(w, es, doc);
	    return;
	case cmdBLD_STOP:
	    stop_build(w, es);
	    return;
	case cmdBLD_NATIVE:
	{
	    var k = format("{}", r["kind"]);
	    var ot = format("{}", r["out"]);
	    start_native_build(w, es, doc, k, ot);
	    return;
	}
	case cmdBLD_RUN:
	    run_buffer(w, es, doc);
	    return;
	// The project rows (^B correlation): check every TU / build the
	// linked image / run the --project lane.
	case cmdBLD_PROJ_CHECK:
	    proj_run_check(w, es, doc);
	    return;
	case cmdBLD_PROJ_BUILD:
	{
	    var k = format("{}", r["kind"]);
	    var ot = format("{}", r["out"]);
	    start_project_build(w, es, doc, k, ot);
	    return;
	}
	case cmdBLD_PROJ_RUN:
	    run_project(w, es, doc);
	    return;
	default:
	    break;			// cmdBLD_RUN_CMD: the row's own command
    }
    var cmd0 = format("{}", r["cmd"]);
    var cmd;
    build_subst(cmd, cmd0.c_str(), w, es, doc);
    if ( r["mode"].as_integer() == bmTERMINAL )
	run_in_terminal(w, es, cmd.c_str());
    else
	start_build(w, es, cmd.c_str());
}

// The c++ view applies when the BUFFER'S OWN kind is in the C or C++
// family (doc_kind — stamped once at open from the file name, never
// re-derived here): its render is source-shaped (madc::emit for madc::fkCPP
// echoes the retained source), and a madc-dialect buffer has no C++
// respelling yet (the named seat, design doc §3.2). A range test on the
// one file-kind vocabulary, never an extension ladder.
bool cxx_view_applies(long w, long es, long doc)
{
    long k = doc_kind(w, doc);
    return (k >= madc::fkC && k <= madc::fkC_LAST)
	|| (k >= madc::fkCPP && k <= madc::fkCPP_LAST);
}

// ^K A (IDE-9e moved it off ^K N, now nextw): original -> mc11 -> c11
// [-> c++ on a C/C++ buffer] -> original.
// A failed enter falls back to the original — the session is never
// trapped inside a stale view.
// The cycle REPLACES the focused View's representation in place (one View
// per tab; its navigation stays the container's). A switch on the render
// language's code, never its spelling.
void cycle_view(long w, long es, long doc)
{
    var v;
    bool up = view_row(v, w, es, focused_view(w, es))
	   && v["kind"].as_integer() == viewCODE;
    if ( !up )
    {
	enter_lens(w, es, doc, madc::fkMC11);
	return;
    }
    switch ( v["lang"].as_integer() )
    {
	case madc::fkMC11:
	    if ( !enter_lens(w, es, doc, madc::fkC11) )
		exit_lens(w, es, doc);
	    return;
	case madc::fkC11:
	    if ( cxx_view_applies(w, es, doc) )
	    {
		if ( !enter_lens(w, es, doc, madc::fkCPP) )
		    exit_lens(w, es, doc);
		return;
	    }
	    break;
    }
    exit_lens(w, es, doc);
}

// ---- ^K Z: hand the terminal to a shell -------------------------------
// The shell request: the client suspends the grid, term_exec runs the
// user's shell on the real tty, the client resumes into a full repaint.
// No return-key pause — leaving the shell IS the gesture.
void open_shell(long w, long es)
{
    if ( es_flag(w, es, "haspanel") )
    {
	// A window: the shell on a pty in the Terminal tab. $SHELL falls back
	// to /bin/sh INSIDE the command (no getenv: the dlsym fallback hands
	// the pointer back as a long); the exec factory splits on spaces,
	// so the one word ${SHELL:-/bin/sh} is the whole -c script.
	start_terminal(w, es, "Shell", "pty://sh -c ${SHELL:-/bin/sh}");
	return;
    }
    var rq = { "kind": rqSHELL, "pause": 0 };
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

// The vi COLON LINE (owner ruling 2026-08-31: modes are general — joe
// reaches it through the ^N modes palette, a modal personality's
// @normal ":" will enter the same line). One ex command, interpreted
// against the EXISTING vocabulary — colon is a SPELLING layer over the
// verbs and internals the chords already call, and the human face of
// the gateway's commands-with-arguments (the prompt collects what a
// remote client passes in the command). `:q` refuses a dirty buffer
// with the q verb's own message (vim's shape — no confirm question);
// `!cmd` parks a terminal request like any suspended run. False = quit.
bool colon_command(long w, long es, long doc, long caret, var &in)
{
    var cmd = php::trim(in.c_str());
    const char *cs = cmd.c_str();
    long n = strlen(cs);
    if ( n == 0 )
	return true;
    if ( cs[0] == '!' )
    {
	var rest = php::trim(cs + 1);
	if ( strlen(rest.c_str()) > 0 )
	    run_in_terminal(w, es, rest.c_str());
	return true;
    }
    bool digits = true;
    for ( long i = 0; i < n; i = i + 1 )
	if ( cs[i] < '0' || cs[i] > '9' )
	    digits = false;
    if ( digits )
    {
	// :N — go to line N (clamped like the goto-line prompt).
	ui::set(w, es, "caret", caret_to_line(w, doc, php::intval(cmd), 0));
	return true;
    }
    const char *sp = strchr(cs, ' ');
    var verb = cmd;
    var arg = "";
    if ( sp != 0 )
    {
	verb = php::trim(perl::substr(cmd, 0, sp - cs));
	arg = php::trim(sp + 1);
    }
    if ( verb == "w" )
	return save_and_check(w, es, doc);
    if ( verb == "q" )
	return do_verb(w, es, "q");
    if ( verb == "q!" )
	return do_verb(w, es, "q!");
    if ( verb == "wq" || verb == "x" )
	return save_quit(w, es, doc);
    if ( verb == "e" && strlen(arg.c_str()) > 0 )
    {
	edit_file(w, es, doc, arg.c_str());
	return true;
    }
    if ( verb == "r" && strlen(arg.c_str()) > 0 )
    {
	insert_file(w, es, doc, caret, arg.c_str());
	return true;
    }
    // The view* verbs (V2b) reach the colon line too — `:viewsplit right
    // mc11`, `:viewfocus next`, `:viewclose`, `:viewopen mc11`; the same
    // owners the dispatcher calls.
    if ( verb == "viewsplit" )
    {
	long vdir;
	long vlang;
	viewsplit_args(arg, vdir, vlang);
	view_split(w, es, doc, vdir, vlang);
	return true;
    }
    if ( verb == "viewfocus" )
    {
	view_focus(w, es, doc, viewfocus_dir_of(arg));
	return true;
    }
    if ( verb == "viewclose" )
    {
	view_close(w, es, doc);
	return true;
    }
    if ( verb == "viewopen" )
    {
	view_open(w, es, doc, viewrepr_lang_of(arg));
	return true;
    }
    if ( verb == "viewdock" )
    {
	long dslot;
	long dside;
	viewdock_target(arg, dslot, dside);
	view_dock(w, es, dslot, dside);
	return true;
    }
    if ( verb == "viewsize" )
    {
	long zslot;
	long zsize;
	viewsize_args(arg, zslot, zsize);
	view_size(w, es, zslot, zsize);
	return true;
    }
    if ( verb == "viewwindow" )
    {
	view_window(w, es, doc);
	return true;
    }
    if ( verb == "viewsync" )
    {
	view_sync(w, es, arg);
	return true;
    }
    ui::set(w, es, "msg", format("Unknown command :{}", cmd));
    return true;
}

// Toggle the lower pane between `name` and hidden.
void toggle_pane(long w, long es, long p)
{
    set_pane(w, es, cur_pane(w, es) == p ? paneNONE : p);
}

// The pointer arm (IDE-9e aware): a press picks the WINDOW first. The
// event echoes the hit edit node's `tag` — the window index the composer
// stamped on every window's edit node while split — the one answer when
// two windows show the same buffer (the ^K O default duplicates the
// active window); a client that carries no tag (a terminal's mouse
// reporting later) is read by the projected subject: the first window
// showing that document, in tree order. A press in another window
// activates it (the owner's mouse-click switch); a press on a window's
// HEADER arrives with no text position (offset -1) and activates only,
// the window's own caret kept. Then the SHARED edit_pointer places the
// caret and, off a stored document, the selection — a displayed view keeps
// the gesture to the caret, as its mark/bend actions refuse.
void IdeSession::pointer_event(long doc, long nav, var &ev)
{
    // The Terminal tab (polish P3b-2): a press on its node — the [terminal]
    // buffer is its subject — gives the program the keyboard; a press
    // anywhere else takes it back. The window logic below never sees it.
    long tsubj = ev["subject"].is_null() ? 0 : ev["subject"].as_integer();
    long tdoc = term_doc_if_any(w, es);
    if ( tdoc != 0 && tsubj == tdoc )
    {
	ui::set(w, es, "termfocus", 1);
	return;
    }
    ui::set(w, es, "termfocus", 0);
    long idx = -1;
    long tag = ev["tag"].is_null() ? -1 : ev["tag"].as_integer();
    if ( tag >= 0 && tag < window_count(w, es) )
	idx = tag;
    else
    {
	long subj = ev["subject"].is_null() ? 0 : ev["subject"].as_integer();
	if ( subj != 0 && subj != nav )
	    idx = window_showing(w, es, subj);
    }
    long ad = doc;
    if ( idx >= 0 && idx != es_int(w, es, "winat", 0) )
    {
	window_activate(w, es, doc, idx);
	ad = active_doc(w, es, doc);
    }
    if ( ev["offset"].as_integer() < 0 )
	return;			// the header: the window, no position
    long target = nav_doc(w, es, ad);
    edit_pointer(w, es, target, ev, target == ad);
}

// ---- the view* verbs (V2b): the editor region a real split tree ---------
// `viewsplit` turns the focused leaf into a `split` holding it and a NEW
// leaf — a second window on the same subject (JOE's ^K O feel, side by
// side); `viewfocus` moves the focus between the leaves; `viewclose` closes
// the focused leaf (its parent split collapses to the sibling). The focused
// leaf's active container IS the live es caret; an unfocused leaf parks its
// caret on its node (`pcaret` / `pmark` / `pbend`), read by the composer's
// snapshot. Every mutation rewrites the value-typed tree (one editor tree,
// one owner: the bag's "layout").

// The leaf count of an editor tree.
long editor_leaf_count(var &node)
{
    if ( node["kind"] != ctSPLIT )
	return 1;
    long n = 0;
    for ( var c : node["children"] )
	n = n + editor_leaf_count(c);
    return n;
}

// The tree-order index of the focused leaf (-1 = none), a running counter.
long editor_focus_scan(var &node, long &seen)
{
    if ( node["kind"] != ctSPLIT )
    {
	long me = seen;
	seen = seen + 1;
	return node["focus"].as_integer() != 0 ? me : -1;
    }
    for ( var c : node["children"] )
    {
	long r = editor_focus_scan(c, seen);
	if ( r >= 0 )
	    return r;
    }
    return -1;
}
long editor_focused_index(var &node)
{
    long seen = 0;
    return editor_focus_scan(node, seen);
}

// Rewrite: focus=1 on leaf `target` (tree order), 0 elsewhere.
void editor_focus_set(var &out, var &node, long target, long &seen)
{
    if ( node["kind"] == ctSPLIT )
    {
	var kids = {};
	for ( var c : node["children"] )
	{
	    var nc;
	    editor_focus_set(nc, c, target, seen);
	    kids.push(nc);
	}
	var nn = node;
	nn["children"] = kids;
	out = nn;
	return;
    }
    var leaf = node;
    leaf["focus"] = seen == target ? 1 : 0;
    seen = seen + 1;
    out = leaf;
}
void editor_do_focus(var &out, var &node, long target)
{
    long seen = 0;
    editor_focus_set(out, node, target, seen);
}

// Rewrite: park (caret, mark, bend) on leaf `target`.
void editor_park_scan(var &out, var &node, long target, long ca, long ma,
		      long be, long &seen)
{
    if ( node["kind"] == ctSPLIT )
    {
	var kids = {};
	for ( var c : node["children"] )
	{
	    var nc;
	    editor_park_scan(nc, c, target, ca, ma, be, seen);
	    kids.push(nc);
	}
	var nn = node;
	nn["children"] = kids;
	out = nn;
	return;
    }
    var leaf = node;
    if ( seen == target )
    {
	leaf["pcaret"] = ca;
	leaf["pmark"] = ma;
	leaf["pbend"] = be;
    }
    seen = seen + 1;
    out = leaf;
}
void editor_do_park(var &out, var &node, long target, long ca, long ma,
		    long be)
{
    long seen = 0;
    editor_park_scan(out, node, target, ca, ma, be, seen);
}

// Read leaf `target` (a copy); out stays null when not found.
void editor_read_scan(var &out, var &node, long target, long &seen)
{
    if ( node["kind"] == ctSPLIT )
    {
	for ( var c : node["children"] )
	{
	    editor_read_scan(out, c, target, seen);
	    if ( !out.is_null() )
		return;
	}
	return;
    }
    if ( seen == target )
	out = node;
    seen = seen + 1;
}
void editor_read_leaf(var &out, var &node, long target)
{
    long seen = 0;
    editor_read_scan(out, node, target, seen);
}

// Rewrite: replace leaf `target` with `repl`.
void editor_replace_scan(var &out, var &node, long target, var &repl,
			 long &seen)
{
    if ( node["kind"] == ctSPLIT )
    {
	var kids = {};
	for ( var c : node["children"] )
	{
	    var nc;
	    editor_replace_scan(nc, c, target, repl, seen);
	    kids.push(nc);
	}
	var nn = node;
	nn["children"] = kids;
	out = nn;
	return;
    }
    if ( seen == target )
    {
	seen = seen + 1;
	out = repl;
	return;
    }
    seen = seen + 1;
    out = node;
}
void editor_do_replace(var &out, var &node, long target, var &repl)
{
    long seen = 0;
    editor_replace_scan(out, node, target, repl, seen);
}

// Rewrite: remove leaf `target`; a split that drops to one child collapses
// to it. A removed node leaves `out` NULL (the reliable signal — a subscript
// of a missing key is an empty container here, not null), which the parent
// filters out.
void editor_remove_scan(var &out, var &node, long target, long &seen)
{
    if ( node["kind"] == ctSPLIT )
    {
	var kids = {};
	for ( var c : node["children"] )
	{
	    var nc;
	    editor_remove_scan(nc, c, target, seen);
	    if ( !nc.is_null() )
		kids.push(nc);
	}
	if ( php::count(kids) == 1 )
	{
	    out = kids[0];
	    return;
	}
	var nn = node;
	nn["children"] = kids;
	out = nn;
	return;
    }
    if ( seen == target )
    {
	seen = seen + 1;
	return;			// out stays null = removed
    }
    seen = seen + 1;
    out = node;
}
void editor_do_remove(var &out, var &node, long target)
{
    long seen = 0;
    editor_remove_scan(out, node, target, seen);
}

// "right" | "bottom" (the viewsplit direction word) -> the split enum.
// A missing word splits vertically (the owner's side-by-side default).
long viewsplit_dir_of(var &arg)
{
    if ( arg.is_null() )
	return ui::split::vertical;
    var a = format("{}", arg);
    if ( a == "bottom" || a == "down" || a == "below" )
	return ui::split::horizontal;
    return ui::split::vertical;		// right = side by side (the default)
}

// "next" | "prev" (the viewfocus direction word) -> +1 / -1.
long viewfocus_dir_of(var &arg)
{
    if ( arg.is_null() )
	return 1;
    var a = format("{}", arg);
    if ( a == "prev" || a == "up" || a == "left" || a == "back" )
	return -1;
    return 1;
}

// A viewsplit / viewtab / viewopen REPR word -> its render language
// (madc::fkMC11 / fkC11 / fkCPP); 0 = a source pane (no code render).
long viewrepr_lang_of(var &word)
{
    if ( word.is_null() )
	return 0;
    var a = format("{}", word);
    if ( a == "mc11" )
	return madc::fkMC11;
    if ( a == "c11" )
	return madc::fkC11;
    if ( a == "cpp" || a == "c++" || a == "cxx" )
	return madc::fkCPP;
    return 0;			// source / src / unknown = a source pane
}

// `viewsplit right mc11` -> dir + render language: the FIRST word the
// direction, the SECOND the representation (both optional).
void viewsplit_args(var &arg, long &dir, long &lang)
{
    dir = ui::split::vertical;
    lang = 0;
    if ( arg.is_null() )
	return;
    var words;
    layout_words(words, arg);
    long n = php::count(words);
    if ( n >= 1 )
    {
	var w0 = words[0];
	dir = viewsplit_dir_of(w0);
    }
    if ( n >= 2 )
    {
	var w1 = words[1];
	lang = viewrepr_lang_of(w1);
    }
}

// The View a leaf's active container shows: the primary leaf (tree order's
// first, the ring host) shows the ring's current View; a secondary leaf its
// active tab's. One reader for the composer's snapshot and the focus handoff.
long leaf_view_of(long w, long es, var &leaf, bool primary)
{
    if ( primary )
	return buffer_view(w, es, es_int(w, es, "bufidx", 0));
    var tabs = leaf["tabs"];
    var tab = tabs[leaf["active"].as_integer()];
    return tab["view"].as_integer();
}

// viewsplit: the focused leaf becomes a split of itself and a NEW leaf. With
// no repr the new leaf is a second window on the current subject (the SAME
// View, JOE's ^K O feel); with a repr (`mc11` / `c11` / `cpp`) it is a code
// View of that representation over the subject (source left, MC11 right — the
// owner's side-by-side). Focus stays on the original leaf. Refused while the
// S5 window stack is up (one axis at a time in V2b; the stack is JOE's).
void view_split(long w, long es, long doc, long dir, long lang)
{
    if ( window_count(w, es) >= 2 )
    {
	ui::set(w, es, "msg",
		"Close the extra windows (^K 1) before splitting the view.");
	return;
    }
    long subject = active_doc(w, es, doc);
    long ca = es_int(w, es, "caret", 0);
    long nv;
    long pcar;
    long tkind;
    if ( lang != 0 )
    {
	nv = make_code_view(w, es, subject, lang);
	if ( nv == 0 )
	{
	    ui::set(w, es, "msg",
		    format("Cannot render {} — the buffer does not translate.",
			   madc::file_kind_name(lang)));
	    return;
	}
	pcar = 0;			// a fresh code render starts at the top
	tkind = viewCODE;
    }
    else
    {
	nv = focused_view(w, es);	// the same View — a second window
	pcar = ca;
	tkind = viewSOURCE;
    }
    var lo;
    layout_get(lo, w, es);
    var etree = lo["editor"];
    long fi = editor_focused_index(etree);
    if ( fi < 0 )
	fi = 0;
    var tab;
    layout_tab(tab, tkind, nv);
    var tabs = {};
    tabs.push(tab);
    long snone = ui::side_code("");
    long ksed = slotEDITOR;
    long mtabs = lmTABS;
    var newleaf = { "kind": ctPANE, "slot": ksed, "side": snone, "size": 0,
		    "mode": mtabs, "tabs": tabs, "active": 0, "focus": 0,
		    "hidden": 0, "pcaret": pcar, "pmark": -1, "pbend": -1 };
    var focleaf;
    editor_read_leaf(focleaf, etree, fi);
    var kids = {};
    kids.push(focleaf);
    kids.push(newleaf);
    var split = { "kind": ctSPLIT, "dir": dir, "size": 0, "children": kids };
    var newtree;
    editor_do_replace(newtree, etree, fi, split);
    lo["editor"] = newtree;
    ui::set(w, es, "layout", lo);
    ui::set(w, es, "msg", format("{} views.", editor_leaf_count(newtree)));
}

// Make the leaf `arriving` (tree order) the live one: its parked caret goes
// live, and its container's View becomes es "fview" — so the focused edit is
// that leaf's (a code View routes display-vs-stored through nav_doc, and its
// read-only verdict returns). The primary leaf's View is the ring's current;
// a secondary's is its tab's.
void editor_enter_leaf(long w, long es, var &tree, long arriving)
{
    var tleaf;
    editor_read_leaf(tleaf, tree, arriving);
    bool primary = arriving == 0;
    long vid = leaf_view_of(w, es, tleaf, primary);
    ui::set(w, es, "fview", vid);
    ui::set(w, es, "caret", tleaf["pcaret"].as_integer());
    ui::set(w, es, "mark", tleaf["pmark"].as_integer());
    ui::set(w, es, "bend", tleaf["pbend"].as_integer());
    var v;
    view_row(v, w, es, vid);
    if ( !v.is_null() && v["kind"].as_integer() == viewCODE )
    {
	// A code View is read-only — the lens contract (enter_lens's rule).
	ui::set(w, es, "editable", false);
	ui::set(w, es, "ro_msg", "Read-only view (^K A returns).");
    }
    else
	derive_access(w, es);		// the file's own verdict
}

// viewfocus: move the focus between leaves. The live caret parks on the
// leaving leaf's node; the arriving leaf becomes live (its View, its caret) —
// the full handoff, so a code-View pane edits (refuses) as itself.
void view_focus(long w, long es, long doc, long dir)
{
    var lo;
    layout_get(lo, w, es);
    var etree = lo["editor"];
    long n = editor_leaf_count(etree);
    if ( n < 2 )
    {
	ui::set(w, es, "msg", "Only one view.");
	return;
    }
    long cur = editor_focused_index(etree);
    if ( cur < 0 )
	cur = 0;
    long nx = (cur + dir + n) % n;
    var parked;
    editor_do_park(parked, etree, cur, es_int(w, es, "caret", 0),
		   es_int(w, es, "mark", -1), es_int(w, es, "bend", -1));
    var focused;
    editor_do_focus(focused, parked, nx);
    lo["editor"] = focused;
    ui::set(w, es, "layout", lo);
    editor_enter_leaf(w, es, focused, nx);
    ui::set(w, es, "msg", format("View {} of {}.", nx + 1, n));
}

// viewsync (client-server arc V5; design §2.6): toggle cursor synchronization
// between a source View and its MC11/C11 View. On = the unfocused pane of a
// source↔code pair draws its caret PROJECTED from the focused caret through
// the coordinate map, never its own parked one — the sync is a read-side
// projection at compose time (viewsync_leaf_caret), so no caret state is
// duplicated. `arg` is on|off at the input boundary (a boolean flag), bare =
// toggle; the stored state es "viewsync" is an integer.
void view_sync(long w, long es, var &arg)
{
    long cur = es_int(w, es, "viewsync", 0);
    var a = arg;
    long on;
    if ( a == "on" )
	on = 1;
    else if ( a == "off" )
	on = 0;
    else
	on = cur ? 0 : 1;
    ui::set(w, es, "viewsync", on);
    ui::set(w, es, "msg", on ? "Cursor sync ON (source ↔ view)."
			     : "Cursor sync off.");
}

// V5 read-side projection: the caret an unfocused leaf showing View `vid`
// should draw when cursor-sync is on — the FOCUSED caret projected into this
// View's coordinate space through the source↔MC11 pair's map. Only a pair of
// one code View (carrying the map) and one source View projects; any other
// combination (same representation, no map, no focused View) returns the
// leaf's own parked caret (`fallback`). A refused projection (-1: an empty or
// malformed map) also falls back — never a bad caret. `follow` is set to 1
// only when a projection actually happened: the composer marks that leaf so
// the frontend SCROLLS to (and marks) the synced caret, an unfocused pane it
// would otherwise leave parked.
long viewsync_leaf_caret(long w, long es, long vid, long fallback, long &follow)
{
    follow = 0;
    long fvid = focused_view(w, es);
    if ( fvid == 0 || fvid == vid )
	return fallback;
    var fv;
    var tv;
    if ( !view_row(fv, w, es, fvid) || !view_row(tv, w, es, vid) )
	return fallback;
    long fcaret = es_int(w, es, "caret", 0);
    bool fcode = fv["kind"].as_integer() == viewCODE;
    bool tcode = tv["kind"].as_integer() == viewCODE;
    if ( tcode && !fcode )
    {
	// focused = source (stored space) → this = code (display space): the
	// code View's map carries the stored↔display correspondence.
	var m = tv["map"];
	if ( php::count(m) == 0 )
	    return fallback;
	long d = ui::lens_to_display(m, fcaret);
	if ( d < 0 )
	    return fallback;
	follow = 1;
	return d;
    }
    if ( fcode && !tcode )
    {
	// focused = code (display space) → this = source (stored space).
	var m = fv["map"];
	if ( php::count(m) == 0 )
	    return fallback;
	long s = ui::lens_to_stored(m, fcaret);
	if ( s < 0 )
	    return fallback;
	follow = 1;
	return s;
    }
    return fallback;		// same representation: nothing to project
}

// viewclose: close the focused leaf; its parent split collapses to the
// sibling and the focus moves to the previous leaf. The primary leaf (tree
// order's first, the ring host) never closes — quit does that.
void view_close(long w, long es, long doc)
{
    var lo;
    layout_get(lo, w, es);
    var etree = lo["editor"];
    long n = editor_leaf_count(etree);
    if ( n < 2 )
    {
	ui::set(w, es, "msg", "Only one view.");
	return;
    }
    long fi = editor_focused_index(etree);
    if ( fi <= 0 )
    {
	ui::set(w, es, "msg", "The primary view stays open (^K Q quits).");
	return;
    }
    var removed;
    editor_do_remove(removed, etree, fi);
    long nf = fi - 1;
    var focused;
    editor_do_focus(focused, removed, nf);
    lo["editor"] = focused;
    ui::set(w, es, "layout", lo);
    editor_enter_leaf(w, es, focused, nf);	// the survivor becomes live
    ui::set(w, es, "msg", format("View closed — {} left.",
				 editor_leaf_count(focused)));
}

// viewopen: re-represent the FOCUSED leaf's View as `lang` (a code render) or
// back to source (lang 0). es "fview" IS the focused leaf's View, so this is
// the lens re-representation the `view` cycle already owns — one owner for
// the focused View whichever leaf holds it.
void view_open(long w, long es, long doc, long lang)
{
    if ( lang == 0 )
    {
	exit_lens(w, es, doc);
	ui::set(w, es, "msg", "View: source");
	return;
    }
    if ( !enter_lens(w, es, doc, lang) )
	exit_lens(w, es, doc);
}

// The viewdock direction word -> a (slot, side): `left`/`right` dock the
// sidebar, `top`/`bottom` the panel (the slot rule parse_layout enforces). An
// absent or unknown word is `ui::side::none` — view_dock refuses it, never
// docks to a default place.
void viewdock_target(var &arg, long &slot, long &side)
{
    side = ui::side::none;
    slot = slotSIDEBAR;
    if ( arg.is_null() )
	return;
    var words;
    layout_words(words, arg);
    if ( php::count(words) < 1 )
	return;
    var w0 = words[0];
    side = ui::side_code(w0.c_str());
    if ( side == ui::side::top || side == ui::side::bottom )
	slot = slotPANEL;
    else
	slot = slotSIDEBAR;		// left | right (none is refused below)
}

// viewdock <left|right|top|bottom>: move the FOCUSED chrome pane to a slot and
// side. "Focused" is the chrome View last brought up (`chromelast`, the @pane
// esc's target) — viewfocus reaching chrome is a V3 residue, so today docking
// retargets the pane you just opened. The placement PERSISTS beside the
// manifest (design §5). No direction, or no chrome open, is a refusal that
// says what to do.
void view_dock(long w, long es, long slot, long side)
{
    if ( side == ui::side::none )
    {
	ui::set(w, es, "msg", "viewdock: dock where? left | right | top | bottom.");
	return;
    }
    long k = es_int(w, es, "chromelast", viewNONE);
    if ( k == viewNONE )
    {
	ui::set(w, es, "msg",
		"Open a pane (a sidebar or a panel view) before docking it.");
	return;
    }
    var lo;
    layout_get(lo, w, es);
    long tab = -1;
    long ci = chrome_hosting(lo, k, tab);
    if ( ci < 0 )
    {
	ui::set(w, es, "msg",
		format("No chrome pane hosts the {} view.", view_name(k)));
	return;
    }
    var ch = lo["chrome"];
    var c = ch[ci];
    c["slot"] = slot;
    c["side"] = side;
    ch[ci] = c;
    lo["chrome"] = ch;
    ui::set(w, es, "layout", lo);
    layout_persist(w, es);
    ui::set(w, es, "msg", format("Docked {} to the {}.", view_name(k),
				 ui::side_name(side)));
}

// `viewsize <sidebar|panel> <percent>` -> the chrome slot + a clamped size.
// slotNONE / 0 for a missing or unknown word (view_size refuses). The slot
// word converts at THIS input boundary (slot_of); the size is runtime numeric
// data (a splitter's pixel drag turned into a percent), clamped to the
// grammar's 1%..90%.
void viewsize_args(var &arg, long &slot, long &size)
{
    slot = slotNONE;
    size = 0;
    if ( arg.is_null() )
	return;
    var words;
    layout_words(words, arg);
    long n = php::count(words);
    if ( n >= 1 )
	slot = slot_of(words[0]);
    if ( n >= 2 )
    {
	var w1 = words[1];
	long v = php::intval(w1);
	if ( v < 1 )
	    v = 1;
	if ( v > 90 )
	    v = 90;
	size = v;
    }
}

// viewsize: set the chrome pane of a slot to a size percent and PERSIST — so
// the SESSION's layout owns the size (it rides <base>.prj.layout and the TUI
// shares it), not the page's per-viewer localStorage. The splitter drag posts
// it (`{action:'viewsize', arg:'sidebar 40'}`); `:viewsize panel 30` reaches
// it from the keyboard too. Refuses a non-band slot (the editor's sizing is
// the split's) or a slot the layout has no pane for.
void view_size(long w, long es, long slot, long size)
{
    if ( slot != slotSIDEBAR && slot != slotPANEL )
    {
	ui::set(w, es, "msg", "viewsize: size which? sidebar | panel <percent>.");
	return;
    }
    if ( size < 1 )
    {
	ui::set(w, es, "msg", "viewsize: a size 1%..90%.");
	return;
    }
    var lo;
    layout_get(lo, w, es);
    var ch = lo["chrome"];
    long n = php::count(ch);
    for ( long i = 0; i < n; i = i + 1 )
    {
	var c = ch[i];
	if ( !(c["slot"] == slot) )
	    continue;
	c["size"] = size;
	ch[i] = c;
	lo["chrome"] = ch;
	ui::set(w, es, "layout", lo);
	layout_persist(w, es);
	ui::set(w, es, "msg", format("{} sized to {}%.", slot_name(slot), size));
	return;
    }
    ui::set(w, es, "msg", format("The layout has no {} pane.", slot_name(slot)));
}

// viewwindow (client-server V3b-2b): a SECOND window on the session — a new
// client viewing the same document (design §2.3). Opening a frontend is the
// CLIENT layer's job (the gateway seam: the session never touches a
// frontend handle), so this parks the doc-to-view on the requesting
// client's es; run_ide's loop drains it (take_spawn), opens the frontend,
// and adds the client record. Only a windowed client can host a second
// window in one process — a terminal client (a single tty) refuses (the
// TUI's own multi-pane shape is its layout, not a second frontend).
void view_window(long w, long es, long doc)
{
    if ( !es_flag(w, es, "haspanel") )
    {
	ui::set(w, es, "msg", "viewwindow: a second window needs a window target (--gui).");
	return;
    }
    ui::set(w, es, "spawnview", doc);
    ui::set(w, es, "msg", "Opening a new window…");
}

// Apply ONE semantic event (the value shapes the engine's event query
// produces — action events from the bindings table included) — the
// session surface. False = quit.
bool IdeSession::apply_ide_event(long doc, var &ev_in)
{
    // The startup hint yields to the first event of any kind — the same
    // "first event returns the row" rule run_ide's loop used to apply.
    ui::set(w, es, "hint", 0);
    // The session works on its OWN copy of the event: the command code is
    // filled in below, never written into the client's object (a client
    // may reuse one event value for its next post — the headless harness
    // does — and a stale code would then shadow the new name).
    var ev = ev_in;
    var kind = ev["event_code"];
    // A command that arrived by NAME only — a client that speaks names (the
    // headless harness, an API client; a control the target could not
    // stamp) — converts ONCE here, the session's input boundary; the engine
    // has already stamped a code-bound chord's code and a stamped control's.
    // An unknown name is cmdNONE: refused below as "Unknown action".
    if ( kind == ui::event_kind::action || kind == ui::event_kind::choose )
    {
	if ( ev_code(ev) == cmdNONE )
	{
	    var an = ev["action"];
	    if ( !an.is_null() && strlen(an.c_str()) > 0 )
		ev["action_code"] = cmd_of(an.c_str());
	}
    }
    // Stage-2: cooperative background tasks drained — the spawned parse
    // already parked its handle/spans on the bag; recompose only. Before
    // anything else: a wake is not a user gesture (it must not clear the
    // message seat or disturb a pending prompt).
    if ( kind == ui::event_kind::wake )
	return true;
    // Display-vs-stored routing (the view seam): navigation, find, and the
    // caret run over the NAV target — the view buffer while a view is up.
    // Document semantics (save, check, outline, the mutation gates) stay
    // on the stored document.
    long nav = nav_doc(w, es, doc);
    bool viewing = nav != doc;
    long caret = clamped_caret(w, es, nav);
    ui::set(w, es, "msg", "");

    // The embedded Terminal (polish P3b-2) owns the keyboard while it has
    // focus and a program runs; pointer/choose/dialog events pass through.
    if ( es_int(w, es, "termfocus", 0) != 0 && es_int(w, es, "termlive", 0) != 0 )
    {
	if ( terminal_event(w, es, ev) )
	    return true;
    }

    if ( prompt_active(w, es) )
	return prompt_event(w, es, nav, caret, ev);

    if ( project_active(w, es) )
	return project_event(w, es, doc, ev);

    if ( modes_active(w, es) )
	return modes_event(w, es, nav, caret, ev);

    // vi MODES-AS-SCOPES (owner 2026-08-31): while a vi mode is active,
    // this arm routes printable keys through the @normal/@insert scope
    // tables and the count/operator grammar; what it does not consume
    // (chords-as-actions, arrows, insert-mode typing) falls through to
    // the shared handling below.
    if ( vi_active(w, es) )
    {
	bool vcont = true;
	if ( vi_event(doc, nav, ev, vcont) )
	    return vcont;
    }

    // A pointing-device gesture (the web target's hit test; a terminal's
    // mouse reporting later): the caret model is the keyboard's.
    if ( kind == ui::event_kind::dialog )
    {
	// A native file dialog answered (S4): open or save-as by the path.
	dialog_result(w, es, doc, ev);
	return true;
    }
    if ( kind == ui::event_kind::pointer )
    {
	pointer_event(doc, nav, ev);
	return true;
    }

    if ( kind == ui::event_kind::action )
    {
	// The NAME, where one rode along — an argument carrier (a Build menu
	// row's "build-<n>"), the unknown-action message; never a compare.
	var a = format("{}", ev["action"]);
	long code = ev_code(ev);
	if ( code == cmdNONE )
	{
	    if ( strlen(a.c_str()) == 0 )
		ui::set(w, es, "msg", format("Unbound: {}", ev["seq"]));
	    else
		ui::set(w, es, "msg", format("Unknown action '{}'.", a));
	    return true;
	}
	// Stored-space interaction state (block markers) and history are
	// meaningless against a displayed view: the one refusal, up front
	// (mutating actions below refuse through the editable gate).
	if ( viewing && (code == cmdMARK || code == cmdBEND
		     || code == cmdUNDO || code == cmdREDO) )
	{
	    refuse_edit(w, es);
	    return true;
	}
	// THE dispatcher: a switch on the command CODE (enums, not strings —
	// owner law 2026-09-09). Every arm leaves; a code no arm claims falls
	// to the scope / motion handling below.
	switch ( code )
	{
	    case cmdVIEW:
	    {
		cycle_view(w, es, doc);
		return true;
	    }
	    // The view* verbs (V2b): the editor region a real split tree.
	    // `viewsplit right|bottom [repr]`, `viewfocus next|prev`,
	    // `viewclose`, `viewopen [repr]` — the words ride the event's
	    // `arg` (the colon line, a menu row's argument), read once into
	    // the split / focus enum and the render language.
	    case cmdVIEWSPLIT:
	    {
		long vdir;
		long vlang;
		viewsplit_args(ev["arg"], vdir, vlang);
		view_split(w, es, doc, vdir, vlang);
		return true;
	    }
	    case cmdVIEWFOCUS:
	    {
		view_focus(w, es, doc, viewfocus_dir_of(ev["arg"]));
		return true;
	    }
	    case cmdVIEWCLOSE:
	    {
		view_close(w, es, doc);
		return true;
	    }
	    case cmdVIEWOPEN:
	    {
		view_open(w, es, doc, viewrepr_lang_of(ev["arg"]));
		return true;
	    }
	    case cmdVIEWDOCK:
	    {
		long dslot;
		long dside;
		viewdock_target(ev["arg"], dslot, dside);
		view_dock(w, es, dslot, dside);
		return true;
	    }
	    case cmdVIEWSIZE:
	    {
		long zslot;
		long zsize;
		viewsize_args(ev["arg"], zslot, zsize);
		view_size(w, es, zslot, zsize);
		return true;
	    }
	    case cmdVIEWWINDOW:
		view_window(w, es, doc);
		return true;
	    case cmdVIEWSYNC:
		view_sync(w, es, ev["arg"]);
		return true;
	    case cmdSAVE:
		return save_and_check(w, es, doc);
	    case cmdSAVEQUIT:
		return save_quit(w, es, doc);
	    case cmdQUIT:
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
	    case cmdDISCARD:
		return do_verb(w, es, "q!");
	    case cmdFIND:
	    {
		start_prompt(w, es, "Find", pmFIND);
		return true;
	    }
	    case cmdFINDNEXT:
	    {
		do_find(w, es, nav, caret);
		return true;
	    }
	    case cmdGOTOLINE:
	    {
		start_prompt(w, es, "Go to line", pmGOTOLINE);
		return true;
	    }
	    case cmdINSERTFILE:
	    {
		start_prompt(w, es, "Insert file", pmINSERTFILE);
		return true;
	    }
	    case cmdTHEME:
	    {
		start_prompt(w, es, "Theme", pmTHEME);
		return true;
	    }
	    case cmdMARK:
	    {
		ui::set(w, es, "mark", caret);
		return true;
	    }
	    case cmdBEND:
	    {
		ui::set(w, es, "bend", caret);
		return true;
	    }
	    case cmdBLOCKCOPY:
	    {
		block_copy(w, es, doc, caret);
		return true;
	    }
	    case cmdBLOCKMOVE:
	    {
		block_move(w, es, doc, caret);
		return true;
	    }
	    case cmdBLOCKDEL:
	    {
		block_del(w, es, doc, caret);
		return true;
	    }
	    case cmdUNDO:
	    {
		undo_edit(w, es, doc);
		return true;
	    }
	    case cmdREDO:
	    {
		redo_edit(w, es, doc);
		return true;
	    }
	    case cmdDELLINE:
	    {
		delete_line(w, es, doc, caret);
		return true;
	    }
	    case cmdDELWORD:
	    {
		delete_word(w, es, doc, caret);
		return true;
	    }
	    case cmdWORDLEFT:
	    {
		ui::set(w, es, "caret", ui::text_word_left(w, nav, caret));
		return true;
	    }
	    case cmdWORDRIGHT:
	    {
		ui::set(w, es, "caret", ui::text_word_right(w, nav, caret));
		return true;
	    }
	    case cmdTOP:
	    {
		ui::set(w, es, "caret", 0);
		return true;
	    }
	    case cmdBOTTOM:
	    {
		ui::set(w, es, "caret", ui::text_size(w, nav));
		return true;
	    }
	    case cmdCHECK:
	    {
		run_check(w, es, doc);
		return true;
	    }
	    case cmdOUTLINE:
	    {
		// The outline View toggles where the layout hosts it (the
		// sidebar by default): shown -> hidden; else refresh + show.
		if ( view_shown(w, es, viewOUTLINE) )
		    hide_view(w, es, viewOUTLINE);
		else
		{
		    refresh_outline(w, es, doc);
		    show_view(w, es, viewOUTLINE);
		}
		return true;
	    }
	    case cmdHELP:
	    {
		toggle_pane(w, es, paneHELP);
		return true;
	    }
	    case cmdOPTIONS:
	    {
		toggle_pane(w, es, paneOPTIONS);
		return true;
	    }
	    case cmdPROJECT:
	    {
		proj_show(w, es, doc);
		return true;
	    }
	    case cmdPROJADDCUR:
	    {
		proj_addcur(w, es, doc);
		return true;
	    }
	    case cmdMODES:
	    {
		modes_show(w, es);
		return true;
	    }
	    case cmdVIMODE:
	    {
		vi_toggle(w, es);
		return true;
	    }
	    case cmdCOLON:
	    {
		// The colon line directly (a profile may seat it on its own
		// chord; the palette's ":" row is the discoverable door).
		start_prompt(w, es, "Command", pmCOLON);
		return true;
	    }
	    case cmdREFRESH:
	    {
		// JOE's ^R retype: the terminal is lying — a hard repaint is
		// the CLIENT's to perform (it owns refresh); the session only
		// parks the request. Headless sessions have no terminal
		// pushed; the action is safely inert there, as it always was.
		var rq = { "kind": rqREFRESH, "pause": 0 };
		term_request(w, es, rq);
		return true;
	    }
	    case cmdBUILD:
	    {
		ensure_build_cmds(w, es);
		toggle_pane(w, es, paneBUILD);
		return true;
	    }
	    case cmdEDITFILE:
	    {
		// A window with native dialogs shows the platform's Open; the
		// terminal and a headless session keep JOE's prompt.
		var none = "";
		if ( !dialog_request(w, es, dlgOPEN, "Open File", none) )
		    start_prompt(w, es, "Name of file to edit", pmEDITFILE);
		return true;
	    }
	    case cmdSAVEAS:
	    {
		var cur;
		ui::get(cur, w, doc, "path");
		if ( !dialog_request(w, es, dlgSAVE, "Save As", cur) )
		    start_prompt(w, es, "Save as", pmSAVEAS);
		return true;
	    }
	    case cmdOPENPROJECT:
	    {
		// File -> Open Project… (polish P2b): the native Open over the
		// manifests; the terminal and a headless session keep a prompt.
		var none = "";
		if ( !dialog_request(w, es, dlgOPENPROJECT, "Open Project", none) )
		    start_prompt(w, es, "Project file to open", pmOPENPROJECT);
		return true;
	    }
	    case cmdPANEL:
	    {
		// The panel pane's visibility (V2: the layout's chrome; every
		// client renders it — the terminal as a band).
		toggle_slot(w, es, slotPANEL);
		return true;
	    }
	    case cmdPROBLEMS:
	    {
		show_view(w, es, viewPROBLEMS);
		return true;
	    }
	    case cmdOUTPUT:
	    {
		show_view(w, es, viewOUTPUT);
		return true;
	    }
	    case cmdTERMINAL:
	    {
		show_view(w, es, viewTERMINAL);
		if ( es_int(w, es, "termlive", 0) != 0 )
		    ui::set(w, es, "termfocus", 1);
		return true;
	    }
	    case cmdBUFSEL:
	    {
		// Switch to a buffer (polish P4): by ring index when the command
		// carries its argument (a tab), else the prompt collects a name.
		var arg = ev["arg"];
		if ( !arg.is_null() && strlen(arg.c_str()) > 0 )
		{
		    var bufs;
		    ui::get(bufs, w, es, "buffers");
		    long idx = php::intval(arg);
		    if ( idx >= 0 && idx < php::count(bufs)
		      && idx != es_int(w, es, "bufidx", 0) )
			switch_buffer(w, es, doc, idx);
		}
		else
		    start_prompt(w, es, "Buffer", pmBUFSEL);
		return true;
	    }
	    case cmdSHELL:
	    {
		open_shell(w, es);
		return true;
	    }
	    case cmdPROFILE:
	    {
		toggle_profile(w, es);
		return true;
	    }
	    // IDE-9e windows — JOE's verbs on their JOE seats.
	    case cmdSPLITW:
	    {
		split_window(w, es, doc);
		return true;
	    }
	    case cmdNEXTW:
	    {
		cycle_window(w, es, doc, 1);
		return true;
	    }
	    case cmdPREVW:
	    {
		cycle_window(w, es, doc, -1);
		return true;
	    }
	    case cmdKILLWIN:
	    {
		kill_window(w, es, doc, false);
		return true;
	    }
	    case cmdONLYWIN:
	    {
		kill_window(w, es, doc, true);
		return true;
	    }
	    case cmdGROWW:
	    {
		grow_window(w, es, 1);
		return true;
	    }
	    case cmdSHRINKW:
	    {
		grow_window(w, es, -1);
		return true;
	    }
	    // A Build MENU row (the ^B palette's rows as menu items): the id
	    // carries the 1-based row; build_choose owns the rest — the same
	    // row data, the same dispatcher the palette's choose reaches.
	    case cmdBUILDROW:
	    {
		ensure_build_cmds(w, es);
		build_choose(w, es, doc,
			     php::intval(perl::substr(a, 6, strlen(a.c_str()) - 6)));
		return true;
	    }
	    default:
		break;
	}
	// A chrome dialog's Close button or a press outside it (polish P2):
	// the @pane scope's cancel posted as an ACTION — what esc does in
	// the terminal (the S6 rule: buttons post the scope's actions; the
	// project and modes dialogs admit theirs the same way above). It is
	// admitted only when the @pane scope binds the code to some key, so
	// a button reaches exactly what a key can.
	var pc;
	if ( scope_action_named(pc, w, es, "pane", ev) && pc == cmdPANECLOSE )
	{
	    // A popup closes first; with none up, the chrome pane shown last
	    // hides (V2) — the pick list's Close and the panel's esc share the
	    // one @pane action.
	    if ( cur_pane(w, es) != paneNONE )
		set_pane(w, es, paneNONE);
	    else
		hide_last_view(w, es);
	    return true;
	}
	// Motion (and the direct edit keys) DELEGATE to the shared
	// edit_key vocabulary: the action name IS the key spelling
	// ("left", "pgdn", "del", ...) — a profile binds e.g. "^a home",
	// the dispatcher synthesizes the key event. One movement
	// implementation. (IDE-10a reclaimed ^B/^F/^P/^N from the
	// WordStar diamond — real arrow keys serve motion.)
	ui::key mk = motion_key_of(code);
	if ( mk != ui::key::none )
	{
	    var kn;
	    cmd_name(kn, code);
	    var ke = { "event": "key", "event_code": ui::event_kind::key,
		       "key": kn, "key_code": mk };
	    if ( edit_key(w, es, nav, caret, ke) )
		return true;
	}
	if ( strlen(a.c_str()) == 0 )
	    cmd_name(a, code);
	ui::set(w, es, "msg", format("Unknown action '{}'.", a));
	return true;
    }
    if ( kind == ui::event_kind::text )
    {
	edit_text_run(w, es, nav, caret, ev);
	return true;
    }
    if ( kind == ui::event_kind::choose )
    {
	// The chosen row's verb: its `code` hint (a code-stamped row) or the
	// name a client posted, converted at the entry above.
	switch ( ev_code(ev) )
	{
	    // Build-pane rows dispatch by ROW (act + command + mode are the
	    // row's data): the bld-* codes route here and build_choose owns
	    // the rest.
	    case cmdBLD_CHECK:
	    case cmdBLD_NATIVE:
	    case cmdBLD_RUN:
	    case cmdBLD_RUN_CMD:
	    case cmdBLD_STOP:
	    case cmdBLD_PROJ_CHECK:
	    case cmdBLD_PROJ_BUILD:
	    case cmdBLD_PROJ_RUN:
		build_choose(w, es, doc, ev["option"].as_integer());
		return true;
	    case cmdGOTO:
	    case cmdGOTODEF:
		// Pane rows carry STORED line numbers — meaningless against a
		// displayed view. Two verbs, one handler: a problems row's
		// goto and a definition's gotodef never make a choose guess
		// which list it came from (both lists can be up at once).
		if ( viewing )
		    refuse_edit(w, es);
		else
		    goto_pane_row(w, es, doc, ev["option"].as_integer(),
				  ev_code(ev) == cmdGOTODEF);
		return true;
	    // The shared editor menu's rows (compose_menu: Save / Find / Quit).
	    case cmdW:
		return save_and_check(w, es, doc);
	    case cmdF:
		start_prompt(w, es, "Find", pmFIND);
		return true;
	    case cmdQ:
		return do_verb(w, es, "q");
	    // The ^T Options rows.
	    case cmdOPT_TABWIDTH:
		start_prompt(w, es, "Tab width (1-16)", pmTABWIDTH);
		return true;
	    case cmdOPT_SCHEME:
		start_prompt(w, es, "Theme", pmTHEME);
		return true;
	    case cmdOPT_KEYMAP:
		toggle_profile(w, es);
		return true;
	    case cmdOPT_PROJKIND:
		proj_toggle_kind(w, es, doc);
		return true;
	    default:
		break;
	}
	return true;
    }
    if ( kind != ui::event_kind::key )
	return true;	// focus / resize: the loop recomposes anyway

    // OWNER RULING (IDE-10c): backing out of ANY pane — help, options,
    // outline, diags, build alike — is @pane profile data (the baked
    // default: esc paneclose). The project window and prompts cancel in
    // their own scoped arms above (they route before this one).
    if ( cur_pane(w, es) != paneNONE )
    {
	var pa;
	if ( scoped_action(pa, w, es, "pane", ev) && pa == cmdPANECLOSE )
	{
	    set_pane(w, es, paneNONE);
	    return true;
	}
    }
    else if ( es_int(w, es, "chromelast", viewNONE) != viewNONE )
    {
	// The chrome pane shown last hides on the @pane esc (V2).
	var pa;
	if ( scoped_action(pa, w, es, "pane", ev) && pa == cmdPANECLOSE
	  && hide_last_view(w, es) )
	    return true;
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
// The diagnostics / outline rows as navigable items carrying `goto` — ONE
// builder for the terminal's diags/outline pane and the window's Problems
// tab (polish P3a). A project check's rows span TUs: a row from ANOTHER
// file leads with it (goto opens it; the row carries the file as data
// either way); rows on the shown buffer stay unprefixed (the JOE shape).
// Returns the row count (0 = the caller adds its empty-state line).
long diag_items(var &items, long w, long es, long doc, bool outline_pane)
{
    var rows;
    if ( outline_pane )
	ui::get(rows, w, es, "outline");
    else
	ui::get(rows, w, es, "diags");
    var dpath;
    ui::get(dpath, w, doc, "path");
    long nrows = 0;
    for ( var row : rows )
    {
	var it = { "role": "item" };
	if ( outline_pane )
	    it["content"] = format("{} {} :{}", row["kind"],
				   row["name"], row["line"]);
	else if ( !row["file"].is_null()
	       && !(row["file"] == dpath.c_str()) )
	    it["content"] = format("{} {}:{} {} {}", row["file"],
				   row["line"], row["column"],
				   row["severity"], row["message"]);
	else
	    it["content"] = format("{}:{} {} {}", row["line"],
				   row["column"], row["severity"],
				   row["message"]);
	long verb = outline_pane ? cmdGOTODEF : cmdGOTO;
	var gh = { "code": verb };	// a navigable row: its verb as a code
	it["hints"] = gh;
	items[] = it;
	nrows = nrows + 1;
    }
    return nrows;
}

// Compose the edit node of a View, by id: the node presents the View's TEXT
// entity (its render buffer, or the subject's own buffer for stored text)
// through the ONE shared edit-node composer; the caller's single hints RMW
// attaches the lens data — never a second read-modify-write of the hints
// (the measured 3 ms copies, 2026-09-07). A session with no View yet
// composes the document itself: byte-identical.
void compose_view_node(var &ed, long w, long es, long id, long doc,
		       long caret)
{
    var v;
    view_row(v, w, es, id);
    compose_edit_node(ed, w, es, view_text_entity(v, doc), caret);
}

// The lens data a code View's edit node carries as hints: the render
// language — its enumerator ("lang_code", what a client switches on) and
// its name ("view", what a client shows) — the coordinate map, and the
// View's own highlight spans. A source View adds nothing.
void view_lens_hints(var &eh, long w, long es, var &v)
{
    long lang = v["lang"].as_integer();
    eh["view"] = madc::file_kind_name(lang);
    eh["lang_code"] = lang;
    eh["map"] = v["map"];
    var vhs;
    view_spans(vhs, w, es, v);
    if ( !vhs.is_null() && php::count(vhs) > 0 )
	eh["spans"] = vhs;
}

// ---- the chrome panes (V2): composed FROM the layout ----------------------
// A visible chrome pane is a group docked by its slot (`region`, the wire
// word), placed by its `side` and `size` (the grid carves a band; the page
// sets the slot's track), heading its tabs as a strip when it holds more
// than one (each tab posts the command that shows its View — the S1 rule),
// and holding the active tab's projection: Problems = the diagnostics rows,
// Outline = the definitions (both through diag_items, the one builder),
// Output = the [build] text (its tail), Terminal = the [terminal] screen. A
// kind with no builder yet names itself. The pane's `focus` autofocuses its
// list; Tab reaches it as ever.
void compose_chrome_pane(var &out, long w, long es, long doc, var &c)
{
    var strip = {};
    long active = c["active"].as_integer();
    long ntabs = php::count(c["tabs"]);
    long shown = viewNONE;
    long ti = 0;
    for ( var t : c["tabs"] )
    {
	long k = t["kind"].as_integer();
	if ( ti == active )
	    shown = k;
	var nm = view_name(k);
	var title = php::ucfirst(nm);
	long sc = view_show_cmd(k);
	var an;
	cmd_name(an, sc);
	var tb = { "title": title, "action": an, "code": sc };
	if ( ti == active )
	    tb["active"] = 1;
	strip.push(tb);
	ti = ti + 1;
    }
    bool focus = c["focus"].as_integer() != 0;
    var pkids = {};
    switch ( shown )
    {
	case viewTERMINAL:
	{
	    // The screen: an edit node over the [terminal] buffer (the
	    // line-DOM, the caret at the end) whose subject is that buffer —
	    // a press on it is how the program gets the keyboard
	    // (pointer_event). Empty until something ran.
	    long tdoc = term_doc_if_any(w, es);
	    var tt = "";
	    long tsz = 0;
	    if ( tdoc != 0 )
	    {
		ui::text(tt, w, tdoc);
		tsz = ui::text_size(w, tdoc);
	    }
	    var term = { "role": "edit" };
	    term["content"] = tt;
	    if ( tdoc != 0 )
		term["subject"] = tdoc;
	    var th = { "terminal": 1 };
	    th["caret"] = tsz;
	    if ( es_int(w, es, "termfocus", 0) != 0 )
		th["focus"] = 1;
	    term["hints"] = th;
	    pkids.push(term);
	    break;
	}
	case viewOUTPUT:
	{
	    var outn = { "role": "content" };
	    var bt = "";
	    long bdoc = build_doc_if_any(w, es);
	    if ( bdoc != 0 )
		ui::text(bt, w, bdoc);
	    outn["content"] = bt;
	    outn["hints"] = { "tail": 1 };	// a band shows the stream's END
	    pkids.push(outn);
	    break;
	}
	case viewOUTLINE:
	case viewPROBLEMS:
	{
	    bool outline = shown == viewOUTLINE;
	    var pb = { "role": "choice" };
	    pb["label"] = outline ? "Outline" : "Problems";
	    var pitems = {};
	    long pn = diag_items(pitems, w, es, doc, outline);
	    if ( pn == 0 )
		pitems[] = { "role": "item",
			     "content": outline ? "(no definitions)" : "(clean)" };
	    pb["children"] = pitems;
	    var ph = { "list": 1 };
	    if ( focus )
		ph["focus"] = 1;
	    pb["hints"] = ph;
	    pkids.push(pb);
	    break;
	}
	default:
	{
	    var nn = { "role": "content" };
	    nn["content"] = format("({} view)", view_name(shown));
	    pkids.push(nn);
	    break;
	}
    }
    var g = { "role": "group" };
    long slot = c["slot"].as_integer();
    // The wire carries the WORDS for the layout discriminators (the `region`
    // precedent): slot -> region, the side enum -> its name; the renderers
    // convert once back to the enum. `size` is the percent int.
    var gh = { "region": slot_name(slot),
	       "side": ui::side_name(c["side"].as_integer()),
	       "size": c["size"] };
    if ( ntabs > 1 )
	gh["tabs"] = strip;
    g["hints"] = gh;
    g["children"] = pkids;
    out = g;
}

// ---- the editor region (V2b, design §2.3): the client's editor slot is
//      `layout["editor"]`, a tree of `split` nodes over editor-pane leaves.
//      A SOLE leaf emits its status + edit FLAT into the slot — the JOE
//      screen, byte-identical with the pre-split composition. A `viewsplit`
//      turns the focused leaf into a split; the split composer (below) wraps
//      each leaf in its own group. `compose_editor_flat` is the sole-leaf /
//      primary-leaf emission: the buffer-ring strip (a window's fact) then
//      the S5 window stack (two-plus windows) or the single status + edit.

void compose_editor_flat(var &kids, long w, long es, var &ed, var &status,
			 long wc)
{
    // The editor TABS (polish P4): under a window's fact, the buffer ring
    // as a tab strip above the window stack — a content node docked FIRST
    // into the editor region whose `tabs` hint carries the strip as data:
    // one tab per buffer (its base name, `*` while modified), the active
    // buffer marked, each tab naming the `bufsel` command WITH its argument
    // (the ring index) — a command that takes an argument, the gateway
    // shape (a human client's prompt collects the same argument). The
    // terminal keeps ^P's buffer rows; it never composes this node.
    if ( es_flag(w, es, "haspanel") )
    {
	var bufs;
	ui::get(bufs, w, es, "buffers");
	long nb = 0;
	if ( !bufs.is_null() )
	    nb = php::count(bufs);
	if ( nb > 0 )
	{
	    long cur = es_int(w, es, "bufidx", 0);
	    var strip = {};
	    for ( long i = 0; i < nb; i = i + 1 )
	    {
		var b = bufs[i];
		var bp = format("{}", b["path"]);
		var bn = bp;
		const char *sl = strrchr(bp.c_str(), '/');
		if ( sl != 0 )
		    bn = sl + 1;
		var m;
		ui::get(m, w, b["doc"].as_integer(), "modified");
		var title = bn;
		if ( !m.is_null() && m.as_boolean() )
		    title = format("{}*", bn);
		var tb = { "title": title, "action": "bufsel", "code": cmdBUFSEL };
		tb["arg"] = format("{}", i);
		if ( i == cur )
		    tb["active"] = 1;
		strip.push(tb);
	    }
	    var tabsnode = { "role": "content", "content": "" };
	    var tnh = { "region": "editor" };
	    tnh["tabs"] = strip;
	    tabsnode["hints"] = tnh;
	    kids.push(tabsnode);
	}
    }
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
    // views on inactive windows (named residues). Every window's status
    // line and edit node docks into the editor slot (S5): the window
    // stacks them in tree order, a status line heading its window.
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
	long i = 0;
	for ( var wr : ws )
	{
	    // Every window's edit node carries its WINDOW INDEX as the
	    // node's `tag` — echoed on the pointer events the node yields, so
	    // a press picks the window even when two windows show the same
	    // document (the ^K O default), where the projected subject
	    // cannot tell them apart.
	    if ( i == at )
	    {
		var ah = ed["hints"];
		ah["tag"] = i;
		ed["hints"] = ah;
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
		var wsh = wst["hints"];
		wsh["region"] = "editor";
		wst["hints"] = wsh;
		kids.push(wst);
		var wed = { "role": "edit" };
		var wt;
		ui::text(wt, w, wdoc);
		wed["content"] = wt;
		wed["subject"] = wdoc;
		var wh = { "caret": wcaret, "region": "editor", "tag": i };
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
}

// One editor leaf's contents: a status header then the edit node, docked in
// the editor region. The FOCUSED leaf is the live one — the status + edit
// composed by the caller from es state, focus-hinted, carrying its leaf tag
// (the pointer arm maps a press back to it). An unfocused leaf is a SNAPSHOT:
// the primary leaf (tree order's first, the ring host) snapshots the active
// buffer row (its caret parked there when focus left it); a secondary leaf
// snapshots its active tree container. No spans / selection on a snapshot
// (the S5 inactive-window rule).
void compose_editor_leaf(var &lk, long w, long es, long doc, var &node,
			 bool primary, bool focused, long myleaf, var &ed,
			 var &status)
{
    // A leaf's status + edit are the flex-column CHILDREN of its leaf group
    // (`.node[data-split] > .node`); the group is what carries `region:
    // editor` (compose_editor_split), so the nodes inside carry NO region —
    // a region here would make the page dock them into a nested workbench
    // instead of stacking them in the column.
    if ( focused )
    {
	var st = status;
	var sh = st["hints"];
	sh["region"] = "";
	st["hints"] = sh;
	lk.push(st);
	var e = ed;
	var eh = e["hints"];
	eh["region"] = "";
	eh["focus"] = 1;
	eh["tag"] = myleaf;
	if ( es_int(w, es, "viewsync", 0) )
	    eh["sync"] = 1;		// V5: a linked-scroll partner (the map rides too)
	e["hints"] = eh;
	lk.push(e);
	return;
    }
    // An unfocused leaf is a SNAPSHOT of its container's View at its parked
    // caret (`pcaret`): the primary leaf shows the ring's current View, a
    // secondary its active tab's. A code View snapshots its RENDER buffer and
    // carries the lens hints (its name, coordinate map, colours); a source
    // View its subject text and the doc's spans — the composer's single
    // display-vs-stored routing (view_text_entity), the S5 inactive rule (no
    // selection).
    long vid = leaf_view_of(w, es, node, primary);
    var v;
    view_row(v, w, es, vid);
    long sdoc = view_text_entity(v, v["subject"].as_integer());
    long pc = node["pcaret"].as_integer();
    long follow = 0;
    if ( es_int(w, es, "viewsync", 0) )
	pc = viewsync_leaf_caret(w, es, vid, pc, follow);	// V5: draw the projected caret
    long sc = clamp_caret_to(w, sdoc, pc);
    var wfn = "";
    var wst;
    compose_joe_statusline(wst, w, es, sdoc, sc, wfn);
    lk.push(wst);
    var wed = { "role": "edit" };
    var wt;
    ui::text(wt, w, sdoc);
    wed["content"] = wt;
    wed["subject"] = sdoc;
    var wh = { "caret": sc, "tag": myleaf };
    if ( follow )
	wh["follow"] = 1;	// V5: a synced pane scrolls to (and marks) its caret
    if ( es_int(w, es, "viewsync", 0) )
	wh["sync"] = 1;		// V5: a linked-scroll partner (the code pane's map rides too)
    if ( !v.is_null() && v["kind"].as_integer() == viewCODE )
	view_lens_hints(wh, w, es, v);		// the lens name + map + colours
    else
    {
	var hs;
	view_spans(hs, w, es, v);
	if ( !hs.is_null() && php::count(hs) > 0 )
	    wh["spans"] = hs;
    }
    wed["hints"] = wh;
    lk.push(wed);
}

// The editor tree as nodes: a split becomes a `split` GROUP (its direction
// the wire word, the grid dividing its rect and the page a flex box) whose
// children are the nodes' groups; a leaf becomes a group of [status, edit].
// `leafno` numbers leaves in tree order — leaf 0 is the PRIMARY. Each node's
// `size` (its share of the parent split) rides its group's hints.
void compose_editor_node(var &out, long w, long es, long doc, var &node,
			 var &ed, var &status, long &leafno)
{
    if ( node["kind"] == ctSPLIT )
    {
	var gkids = {};
	for ( var c : node["children"] )
	{
	    var cg;
	    compose_editor_node(cg, w, es, doc, c, ed, status, leafno);
	    gkids.push(cg);
	}
	var g = { "role": "group", "children": gkids };
	var gh = { "split": ui::split_name(node["dir"].as_integer()) };
	long ssz = node["size"].as_integer();
	if ( ssz > 0 )
	    gh["size"] = ssz;
	g["hints"] = gh;
	out = g;
	return;
    }
    long myleaf = leafno;
    bool primary = leafno == 0;
    leafno = leafno + 1;
    bool focused = node["focus"].as_integer() != 0;
    var lk = {};
    compose_editor_leaf(lk, w, es, doc, node, primary, focused, myleaf, ed,
			status);
    var g = { "role": "group", "children": lk };
    long sz = node["size"].as_integer();
    if ( sz > 0 )
    {
	var gh = { "size": sz };
	g["hints"] = gh;
    }
    out = g;
}

// Compose a split editor tree into the editor slot: one `split` group carrying
// `region: editor` so it docks into the editor slot (the page's workbench grid)
// and the terminal reads it as the centre flow — the leaves inside it flex.
void compose_editor_split(var &kids, long w, long es, long doc, var &node,
			  var &ed, var &status)
{
    long leafno = 0;
    var g;
    compose_editor_node(g, w, es, doc, node, ed, status, leafno);
    var gh = g["hints"];
    gh["region"] = "editor";
    g["hints"] = gh;
    kids.push(g);
}

void IdeSession::compose_ide_tree(var &root, long doc)
{
    long caret = es_int(w, es, "caret", 0);
    // The View seam at composition: the edit node presents the focused
    // View's text (its render buffer while a lens is up) and, for a code
    // View, carries the lens data — the render language and its
    // coordinate map — as hints. The identity lens (a source View, or no
    // View yet) takes neither branch: byte-identical.
    long fv = focused_view(w, es);
    var fvrow;
    view_row(fvrow, w, es, fv);
    long nav = view_text_entity(fvrow, doc);
    bool viewing = nav != doc;

    // The JOE screen model (IDE-9b): no heading banner — the path, the
    // modified/read-only badges, and the view name all render in the
    // status line's %n / %m / %R / %y seats.
    var ed;
    compose_view_node(ed, w, es, fv, doc, caret);
    // ONE read-modify-write of the edit node's hints: every hint below
    // lands on this local and it is written back ONCE, after the region
    // block. A var assignment is a deep copy, and with the highlight
    // spans aboard (4841 rows on madcide_core.inc) each of the former
    // three RMW passes cost two ~3 ms copies per composed keystroke
    // (measured 2026-09-07).
    var eh = ed["hints"];
    if ( viewing )
	view_lens_hints(eh, w, es, fvrow);
    else
    {
	// Highlight spans (AST-2): the bag's themed byte-range rows ride
	// the hints — the document's stored-space rows, computed at
	// (re)parse, just attached here (a code View's rows live on its
	// render buffer — view_spans is the one reader).
	var hs;
	view_spans(hs, w, es, fvrow);
	if ( !hs.is_null() && php::count(hs) > 0 )
	    eh["spans"] = hs;
    }

    // The display tab width (IDE-9d ^T option) rides the edit node's
    // hints like the caret — the model owns the expansion math.
    long tabw = es_int(w, es, "tabwidth", 0);
    if ( tabw > 0 )
	eh["tabwidth"] = tabw;

    // The status line's enclosing-function seat (%x, AST-1): a RETAINED-
    // state read of the buffer's parse handle — stored-space only (a
    // view's caret is display-space), and only once something opened the
    // handle; composition never runs the compiler.
    var fnctx = "";
    var vw_st;
    if ( !lens_name(vw_st, w, es) )
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

    // Layout regions (slice 3, madcide GUI): the edit node is the editor
    // group (a tab strip when more than one buffer is open), the status
    // line is the status bar. These are ADDITIVE hint keys the grid model
    // never reads (tui_model reads caret/sel/focus/list/tabwidth/spans),
    // so the terminal composition is byte-identical; web_model lays a
    // region'd node into the workbench grid. The active window's ed and
    // status are stamped here, before the multi-window branch's own
    // read-modify-write of ed["hints"] preserves the region.
    long wc = window_count(w, es);
    {
	eh["region"] = "editor";
	// With two-plus windows the active window carries the focus hint
	// (the multi-window branch below composes the others); it rides
	// the same single write-back.
	if ( wc >= 2 )
	    eh["focus"] = 1;
	ed["hints"] = eh;
    }
    {
	// While split (S5), the active window's status line is its HEADER
	// in the editor slot like every other window's — the JOE shape, one
	// status line per window, stacked in tree order; the status BAR is
	// the single window's presentation. The window (the page) stacks a
	// slot's nodes in order, so no window's node is left unplaced.
	var sh = status["hints"];
	sh["region"] = wc >= 2 ? "editor" : "statusbar";
	status["hints"] = sh;
    }

    var r = { "role": "group" };
    var kids = {};
    // The editor slot from the layout's editor tree (V2b). A sole editor
    // leaf emits flat — the JOE screen, byte-identical; a `viewsplit` makes
    // it a split and the split composer wraps each leaf in its own group.
    var elo;
    layout_get(elo, w, es);
    var etree = elo["editor"];
    if ( etree["kind"] == ctSPLIT )
	compose_editor_split(kids, w, es, doc, etree, ed, status);
    else
	compose_editor_flat(kids, w, es, ed, status, wc);

    long pane = cur_pane(w, es);
    if ( pane != paneNONE )
    {
	var box = { "role": "choice" };
	var items = {};
	long nrows = 0;
	if ( pane == paneHELP )
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
		items[] = { "role": "item", "content": line };
		nrows = nrows + 1;
	    }
	    var nofilter;
	    dialog_hints(box, w, es, "pane", "", nofilter);	// no primary: rows do nothing on choose
	}
	else if ( pane == paneOPTIONS )
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
	    rows[] = { "content": format("D Tab width   {}", tw),
		       "act": cmdOPT_TABWIDTH };
	    rows[] = { "content": format("S Scheme      {}", scheme),
		       "act": cmdOPT_SCHEME };
	    rows[] = { "content": format("K Keymap      {}", prof),
		       "act": cmdOPT_KEYMAP };
	    // The project kind (polish P2c): console | gui — shown while a
	    // manifest is open (the implicit project has no manifest).
	    if ( proj_has_manifest(w, es) )
	    {
		rows[] = { "content": format("P Project kind {}",
					     projkind_name(proj_kind(w, es))),
			   "act": cmdOPT_PROJKIND };
	    }
	    for ( var row : rows )
	    {
		var it = { "role": "item", "content": row["content"] };
		var gh = { "code": row["act"] };	// the row's verb, a code
		it["hints"] = gh;
		items.push(it);
		nrows = nrows + 1;
	    }
	    var nofilter;
	    dialog_hints(box, w, es, "pane", "Change", nofilter);
	}
	else if ( pane == paneBUILD )
	{
	    // IDE-10b ^B: the same popup-list widget with the command
	    // rows as data; a running build shows in the label.
	    var brows;
	    ui::get(brows, w, es, "buildcmds");
	    box["label"] = es_int(w, es, "buildlive", 0) != 0
		? "Build — running" : "Build";
	    box["hints"] = { "list": 1, "focus": 1 };
	    for ( var row : brows )
	    {
		var c = row["cmd"];
		var it = { "role": "item",
			   "content": c.is_null()
			       ? format("{}", row["name"])
			       : format("{}  [{}]", row["name"], c) };
		var gh = { "code": row["act"] };	// the row's verb, a code
		it["hints"] = gh;
		items[] = it;
		nrows = nrows + 1;
	    }
	    var nofilter;
	    dialog_hints(box, w, es, "pane", "Run", nofilter);
	}
	else if ( pane == paneMODES )
	{
	    // The MODES palette: the same focused-list widget; rows come
	    // precomposed from modes_show. Rows carry NO actions — what a
	    // choose means is @modes data (modes_event looks it up).
	    var mrows;
	    ui::get(mrows, w, es, "modesrows");
	    box["label"] = "Modes";
	    box["hints"] = { "list": 1, "focus": 1 };
	    for ( var row : mrows )
	    {
		items[] = { "role": "item", "content": row["content"] };
		nrows = nrows + 1;
	    }
	    var nofilter;
	    dialog_hints(box, w, es, "modes", "Select", nofilter);
	}
	else if ( pane == panePROJECT )
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
	    box["hints"] = { "list": 1, "focus": 1 };
	    for ( var row : prows )
	    {
		items[] = { "role": "item", "content": row["content"] };
		nrows = nrows + 1;
	    }
	    if ( nrows == 0 )
		items[] = { "role": "item", "content": "(no matches)" };
	    // The dialog's title names the manifest; the label keeps the
	    // terminal's one-line form (the filter inline). The filter text
	    // is the core's — the field shows it, keys keep reaching it.
	    var ftxt = "";
	    if ( !filt.is_null() )
		ftxt = format("{}", filt);
	    dialog_hints(box, w, es, "project", "Open", ftxt);
	    var dh = box["hints"];
	    var dg = dh["dialog"];
	    dg["title"] = format("Project — {}", pfd);
	    dh["dialog"] = dg;
	    box["hints"] = dh;
	}
	box["children"] = items;
	// Every pane here is a PICK LIST (V2: the tool Views live in the
	// layout's chrome panes below); dialog_hints floated it as a popup.
	kids.push(box);
    }

    // The CHROME panes (V2, design §2.2): every visible fixed-slot pane of
    // the client's layout — after the popup, so a pick list keeps its place
    // in the tree. The grid carves their bands by `side` / `size`; the page
    // docks them by `region`.
    var lo;
    layout_get(lo, w, es);
    for ( var c : lo["chrome"] )
    {
	if ( c["hidden"].as_integer() != 0 )
	    continue;
	var cp;
	compose_chrome_pane(cp, w, es, doc, c);
	kids.push(cp);
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
	var hn = { "role": "content" };
	hn["content"] =
	    " madcide ** ^K Q exits / ^K H shows the loaded profile's help **";
	kids.push(hn);
    }
    r["children"] = kids;
    // The @gui theme (slice 3): the loaded scheme's CSS custom properties
    // ride the root group's hints; web_model emits them and the page applies
    // them as `--` variables. A hint the TUI ignores. Empty scope = nothing
    // attached (the JOE default look).
    var gt;
    ui::get(gt, w, es, "gui_theme");
    // The root's hints: each field conditional (a loaded scheme, loaded
    // menu data), so the bag is built by keyed assignment (`{}` would be
    // an empty ARRAY); nothing loaded = no hints, as before.
    var rh;
    if ( !gt.is_null() && php::count(gt) > 0 )
	rh["theme"] = gt;
    // The @presence palette (V3c): the scheme's round-robin caret colours
    // ride the root hints; a client renders another client's presence caret
    // by its dealt slot. A hint the TUI ignores.
    var pt;
    ui::get(pt, w, es, "presence_theme");
    if ( !pt.is_null() && php::count(pt) > 0 )
	rh["presence"] = pt;
    // The menu / command data (S1): the bar's menus with their items'
    // ids, titles and enablement — a root hint the TUI ignores and a
    // chrome-rendering client (S2) draws natively.
    var menu;
    if ( compose_menu_bar(menu, w, es, doc, nav) )
	rh["menu"] = menu;
    if ( !rh.is_null() )
	r["hints"] = rh;
    root = r;
}

// ---- the session lifecycle (the gateway seam) ---------------------------

// The per-CLIENT view initialisation (client-server V3b-2). Everything a
// client's editor-state bag needs once its world + document exist: its own
// profile / theme / menu / layout, the buffer table's launch row, the
// project read-back, and the first lexical colour. open() runs it for
// client 0 over the es setup_editor created; spawn_view_es runs it for
// every additional window onto the SAME session (viewwindow). ONE builder,
// so a spawned window's bag is constructed by the same code as the launch
// window's — a second window that renders the shared document identically.
// Returns false ONLY when the startup profile AND the built-in rescue
// bindings both fail (fatal; the caller reports the exit).
bool init_view_es(long w, long es, long doc, const char *path)
{
    var p = path;		// ring discipline: own the text first — the
				// data loads below cycle the ring, so a bare
				// `path` pointer read empty at launchpath.
    // resolve_profile_dir owns the search: source tree (beside this
    // file, from any cwd — php::dirname splits both separators; '/'
    // alone cost the first genuine-Win validation its keybindings),
    // then the installed layouts anchored on the running binary (PK7).
    var pdir;
    resolve_profile_dir(pdir);
    ui::set(w, es, "profile_dir", pdir.c_str());
    load_status(w, es, "joe");	// the status FORMAT rides the profile
    if ( !load_theme(w, es, "default") )
	ui::set(w, es, "msg", "No theme 'default' — colours off.");
    load_menu(w, es, "default");	// the menu/command data (optional, S1)
    load_layout(w, es, "default");	// WHERE Views live (V2): the client's
					// layout; the baked default when the
					// file is missing or refused
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
	    println(stderr, "madcide: cannot load the startup profile {}/joe.keys and the built-in rescue bindings failed to install — check the install layout (source tree: profiles/ beside madcide_core.inc; installed: <exedir>/../share/madcide/profiles, or profiles/ beside the executable).", dir);
	    return false;
	}
    }
    ui::set(w, es, "hint", 1);	// the transient bottom startup hint
    // The buffer table (AST-5): row 0 = the launch file; ^K E grows it.
    // ONE fresh-row owner (push_buffer_row — the gated dupaudit family).
    push_buffer_row(w, es, doc, p.c_str());
    ui::set(w, es, "bufidx", 0);
    // Presence (client-server V3c): register this client as a viewer of the
    // launch document — the anchor registry shifts every client's caret in
    // one pass and compose draws the others, and this deals the es its
    // round-robin presence colour slot.
    es_view_doc(w, es, doc);
    ui::set(w, es, "fview", buffer_view(w, es, 0));	// the focused
				// container's View: the launch tab's
    // The project (owner rulings 2026-08-31): the launch file anchors the
    // implicit single-file project; a <base>.prj.json beside it IS the
    // project and loads back (announced on the status line). launchpath
    // rides as a VAR (proj_add / layout_persist read it to place the
    // manifest and the layout beside the file — a bare const char* here
    // read back empty and dropped both artifacts at the root base).
    ui::set(w, es, "launchpath", p);
    proj_startup(w, es, doc);
    // PAINT BEFORE THE PARSE (owner report, 2026-08-26: parse-on-load
    // blocked the first render for seconds on a real C++ file): colour
    // rides the first paint from the LEXICAL spans; the full parse runs
    // as the client loop's pending step AFTER a render — the ONE
    // mechanism startup and buffer switches share (parse_pending).
    lex_first_spans(w, es, doc);
    ui::set(w, es, "pending_parse", 1);
    return true;
}

// A fresh client VIEW onto an existing world + document (client-server
// V3b-2, viewwindow). A second window is a second CLIENT with its OWN
// editor-state bag over the SHARED document — its own caret, buffer ring,
// layout and focus; the documents live in the world both share, so an edit
// in either window mutates the one document and the other window shows it
// on its next compose. Mirrors setup_editor's es half (the world verbs are
// already bound and the document already loaded), then the SAME per-client
// init open() runs. Returns the new es, or 0 if the profile + rescue both
// failed.
long spawn_view_es(long w, long doc, const char *path)
{
    var p = path;		// ring discipline: own before the ring-using
				// derive_access / init below clobber the slot
    long es = ui::create(w, "editor-state");
    ui::set(w, es, "caret", 0);
    ui::set(w, es, "mark", -1);
    ui::set(w, es, "adoc", doc);
    derive_access(w, es);
    if ( !init_view_es(w, es, doc, p.c_str()) )
	return 0;
    return es;
}

// The session half of a viewwindow spawn (client-server V3b-2b): a fresh
// client VIEW over the shared world + `doc`. The client layer (run_ide)
// asks for it, then opens a frontend on the returned es and adds the
// client record — the seam keeps the session free of frontend handles.
long IdeSession::spawn_view(long doc)
{
    var dp;
    ui::get(dp, w, doc, "path");
    return spawn_view_es(w, doc, dp.c_str());
}

// Drain a parked viewwindow spawn: the doc a new window should view, left
// on the requesting client's es by the viewwindow command. 0 = none.
bool IdeSession::take_spawn(long &doc)
{
    long d = es_int(w, es, "spawnview", 0);
    if ( d == 0 )
	return false;
    ui::set(w, es, "spawnview", 0);
    doc = d;
    return true;
}

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
    if ( !init_view_es(w, es, doc, p.c_str()) )
	return -1;		// the profile AND rescue bindings both failed
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

void IdeSession::dialogs(bool has)
{
    ui::set(w, es, "hasdialogs", has);
}

void IdeSession::panel(bool has)
{
    ui::set(w, es, "haspanel", has);
}

// The client could not show the dialog it was asked for (no native dialog
// on this platform): the request's prompt instead — the same prompts the
// terminal uses, so the command still completes.
void IdeSession::dialog_fallback(var &req)
{
    var dk = req["dlg"];
    long dlg = dlgOPEN;
    if ( !dk.is_null() )
	dlg = dk.as_integer();
    switch ( dlg )
    {
	case dlgSAVE:
	    start_prompt(w, es, "Save as", pmSAVEAS);
	    return;
	case dlgOPENPROJECT:
	    start_prompt(w, es, "Project file to open", pmOPENPROJECT);
	    return;
	default:
	    break;
    }
    start_prompt(w, es, "Name of file to edit", pmEDITFILE);
}

// A terminal request the client cannot serve (no terminal to suspend —
// a window, a headless session): the status line says so; the request
// was silently dropped before polish P3b-1 (fix-what-you-find).
void IdeSession::request_refused(var &req)
{
    ui::set(w, es, "msg",
	    format("This client has no terminal to hand over for '{}'.",
		   req_name(req["kind"].as_integer())));
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
    switch ( req["kind"].as_integer() )
    {
	case rqRUN:
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
	case rqPROJRUN:
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
	case rqCMD:
    {
	var c = format("{}", req["cmd"]);
	system(c.c_str());
	return;
    }
	case rqSHELL:
	    // $SHELL falls back to /bin/sh INSIDE the command — libc system()
	    // inherits the terminal, and no getenv is needed (the dlsym
	    // fallback would hand the pointer back as a long).
	    system("exec \"${SHELL:-/bin/sh}\"");
	    return;
	default:
	    break;
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

// ---- the vi arm (modes-as-scopes — the session owns mode routing) -------

// ONE command by CODE through the ONE dispatcher: the action event a
// code-bound chord produces, without the chord (kind "action" never
// re-enters the vi arm, so motions, the edit keys, and the IDE verbs all
// serve a code-speaking client without a second dispatch). The vi grammar
// (vi_exec), the `-c` one-shot and the api seat post through here. False =
// the session ended (a quit propagates).
bool IdeSession::post(long doc, long code)
{
    var e = { "event": "action", "event_code": ui::event_kind::action,
	      "action": "", "action_code": code };
    return apply_ide_event(doc, e);
}

// Run a named action n times through the ONE dispatcher.
bool IdeSession::vi_exec(long doc, long code, long n)
{
    for ( long i = 0; i < n; i = i + 1 )
	if ( !post(doc, code) )
	    return false;
    return true;
}

// A command WITH ITS ARGUMENT — the shape every non-interactive input
// boundary posts (client-server design §2.3b: the `-c` command line now;
// the `:` line's `:find x` and the api seat's {"cmd", "args"} later). The
// argument is what the PROMPT the command opens would have been typed:
// `find add` = the find prompt, "add", commit; `gotoline 7`; `colon w`; a
// confirm reads it as its answer (`quit y`). The commit posts the @prompt
// scope's pcommit by CODE (scope_action_named: a code reaches exactly what
// a key can), so a profile's spelling of the commit key is not this
// method's concern. False = refused: an argument for a command that opened
// no prompt (the message says so — the caller's exit status 2). `cont` is
// the continue / quit verdict the dispatcher returned.
bool IdeSession::command(long doc, long code, const char *arg, bool &cont)
{
    var a = arg;			// ring discipline: own the text first
    cont = post(doc, code);
    if ( !cont || strlen(a.c_str()) == 0 )
	return true;
    if ( !prompt_active(w, es) )
    {
	var nm;
	cmd_name(nm, code);
	ui::set(w, es, "msg", format("'{}' takes no argument.", nm));
	return false;
    }
    var t = { "event": "text", "event_code": ui::event_kind::text, "text": a };
    cont = apply_ide_event(doc, t);
    if ( !cont || !prompt_active(w, es) )
	return true;			// a confirm answered (y / n)
    cont = post(doc, cmdPCOMMIT);
    return true;
}

// ONE line of the ex / edlin client (ui::LINE, client-server design §2.3b,
// slice V2.5): the classifier the line frontend's raw-line `text` event
// feeds (run_line). The engine frontend stays dumb — the tool's line
// grammar lives HERE, where the registry and the colon interpreter do:
//   - a ':' line is a colon command — the rest is the ex command, posted
//     through the ONE argument primitive (post cmdCOLON to open the colon
//     line, feed the text, commit by code), the SAME path the -c one-shot
//     and the TUI ':' prompt reach colon_command by. A bare ':' runs
//     nothing. Every colon verb (w q wq x e r, :N goto, the view* verbs,
//     !shell) is reached with no new command language (owner: "the vi ':'
//     mode without the TUI part").
//   - any other line is TEXT: inserted verbatim (its bytes preserved — no
//     trim, so indentation survives), then a newline, so successive lines
//     stack. A read-only view refuses, as it does for a typed key.
// cont carries the continue / quit verdict (a committed :q ends the loop).
bool IdeSession::line_input(long doc, const char *line, bool &cont)
{
    cont = true;
    var raw = line;			// ring discipline: own the text first
    long n = strlen(raw.c_str());
    if ( n > 0 && raw.c_str()[0] == ':' )
    {
	var rest = php::trim(perl::substr(raw, 1, n - 1));
	if ( strlen(rest.c_str()) == 0 )
	    return true;		// a bare ':' — nothing to run
	command(doc, cmdCOLON, rest.c_str(), cont);
	return cont;
    }
    var t = { "event": "text", "event_code": ui::event_kind::text, "text": raw };
    cont = apply_ide_event(doc, t);
    if ( cont )
	cont = post(doc, cmdENTER);
    return cont;
}

// ERROR rows in the problems projection after the last command — the
// one-shot's verdict (`madcide f -c check` exits 1 on a broken file);
// warnings never count, exactly as they never bar a build.
long IdeSession::error_count()
{
    var diags;
    ui::get(diags, w, es, "diags");
    if ( diags.is_null() )
	return 0;
    return diag_error_count(diags);
}

// o / O (and cc's emptied line): break the line at the caret, land on
// the EMPTY side, enter insert mode. The caller positions the caret
// first (o: end of line; O / cc: start of line, stepping back onto the
// opened line).
void IdeSession::vi_open_line(long doc, bool above)
{
    vi_exec(doc, cmdENTER, 1);
    if ( above )
	vi_exec(doc, cmdLEFT, 1);
    vi_enter(w, es, viINSERT);
}

// The vi arm: routes the main key stream while a vi mode is active.
// INSERT consumes only what the @insert table binds (esc -> vinormal,
// baked into the neovim profile's data); NORMAL consumes printables —
// digits accumulate a count, other characters look up @normal (as
// written, then lowercase) and feed the grammar: operators pend
// (doubled = the line-wise form), motions move or bound a pending
// operator's span, insert entries flip the mode, and everything else
// the table names runs count times through vi_exec. Keys other than a
// bound esc (arrows, del, enter, backspace) and engine-chord actions
// fall through — the shared implementations serve every mode. True =
// consumed; cont carries the continue/quit verdict.
bool IdeSession::vi_event(long doc, long nav, var &ev, bool &cont)
{
    cont = true;
    long mode = cur_vimode(w, es);
    var kind = ev["event_code"];
    if ( mode == viINSERT )
    {
	if ( kind != ui::event_kind::key )
	    return false;		// typing types; chords act
	var ia;
	if ( !scoped_action(ia, w, es, "insert", ev) )
	    return false;
	if ( ia == cmdVINORMAL )
	{
	    vi_enter(w, es, viNORMAL);
	    return true;
	}
	return false;
    }
    // NORMAL mode. Keys stay on the shared implementations; esc clears
    // a pending count/operator (nothing pending: the pane's, or inert).
    if ( kind == ui::event_kind::key )
    {
	if ( ev["key_code"] != ui::key::esc || !vi_pending(w, es) )
	    return false;
	vi_pending_clear(w, es);
	return true;
    }
    if ( kind != ui::event_kind::text )
	return false;
    var t = format("{}", ev["text"]);
    if ( strlen(t.c_str()) != 1 )
	return true;	// a paste is not a command stream — inert here
    const char *tc = t.c_str();
    var cnt;
    ui::get(cnt, w, es, "vicount");
    if ( cnt.is_null() )
	cnt = "";
    if ( (tc[0] >= '1' && tc[0] <= '9')
	 || (tc[0] == '0' && strlen(cnt.c_str()) > 0) )
    {
	// Count digits accumulate (a bare 0 is the home motion). Seven
	// digits cap the multiplier — the liberal-but-finite knob.
	if ( strlen(cnt.c_str()) < 7 )
	{
	    ui::set(w, es, "vicount", format("{}{}", cnt, t));
	    vi_echo_push(w, es, tc);
	}
	return true;
    }
    var a;
    if ( !scoped_action(a, w, es, "normal", ev) )
    {
	vi_pending_clear(w, es);
	ui::set(w, es, "msg", format("vi: '{}' is not bound.", t));
	return true;
    }
    long n = php::intval(cnt);
    if ( n < 1 )
	n = 1;
    bool counted = strlen(cnt.c_str()) > 0;
    long ops = es_int(w, es, "viop", cmdNONE);	// the pending operator
    long opn = es_int(w, es, "viopn", 1);
    if ( opn < 1 )
	opn = 1;
    // Operators PEND (their count stashed aside, so d3w and 2d3w both
    // multiply); the SAME operator again is the line-wise form (dd/cc).
    if ( a == cmdVIDELETE || a == cmdVICHANGE )
    {
	if ( ops != a.as_integer() )
	{
	    ui::set(w, es, "viop", a);
	    ui::set(w, es, "viopn", n);
	    ui::set(w, es, "vicount", "");
	    vi_echo_push(w, es, tc);
	    return true;
	}
	vi_pending_clear(w, es);
	long total = opn * n;
	for ( long i = 0; i < total; i = i + 1 )
	{
	    long before = ui::text_size(w, doc);
	    delete_line(w, es, doc, clamped_caret(w, es, nav));
	    if ( ui::text_size(w, doc) >= before )
		break;		// the edge (or a refusal): stop repeating
	}
	if ( a == cmdVICHANGE )
	    vi_open_line(doc, true);	// cc leaves an opened empty line
	return true;
    }
    if ( vi_is_motion(a.as_integer()) )
    {
	long c0 = clamped_caret(w, es, nav);
	if ( a == cmdVIGOTO )
	{
	    // vim's G: a count is the LINE (5G); bare G is the bottom —
	    // the same landing rule as the colon line's :N.
	    if ( counted )
		ui::set(w, es, "caret", caret_to_line(w, nav, n, 0));
	    else if ( !vi_exec(doc, cmdBOTTOM, 1) )
	    {
		cont = false;
		return true;
	    }
	}
	else
	{
	    long steps = opn * n;
	    for ( long i = 0; i < steps; i = i + 1 )
	    {
		long before = es_int(w, es, "caret", 0);
		if ( !vi_exec(doc, a.as_integer(), 1) )
		{
		    cont = false;
		    return true;
		}
		if ( es_int(w, es, "caret", 0) == before )
		    break;	// the edge: further repeats are no-ops
	    }
	}
	if ( ops == cmdNONE )
	{
	    vi_pending_clear(w, es);	// a bare motion spends its count
	    return true;
	}
	// Operator over the motion's span (byte-wise). Offsets are NAV
	// space; the mutation gate refuses when a view is displayed.
	long c1 = clamped_caret(w, es, nav);
	vi_pending_clear(w, es);
	vi_span_del(w, es, doc, c0 < c1 ? c0 : c1, c0 < c1 ? c1 : c0);
	if ( ops == cmdVICHANGE )
	    vi_enter(w, es, viINSERT);
	return true;
    }
    // Insert-mode entries (a pending operator aborts — no text objects).
    vi_pending_clear(w, es);
    if ( a == cmdVIINSERT )
    {
	vi_enter(w, es, viINSERT);
	return true;
    }
    if ( a == cmdVIAPPEND )
    {
	cont = vi_exec(doc, cmdRIGHT, 1);
	vi_enter(w, es, viINSERT);
	return true;
    }
    if ( a == cmdVIAPPENDEOL || a == cmdVIINSERTBOL )
    {
	cont = vi_exec(doc, a == cmdVIAPPENDEOL ? cmdEND : cmdHOME, 1);
	vi_enter(w, es, viINSERT);
	return true;
    }
    if ( a == cmdVIOPENBELOW || a == cmdVIOPENABOVE )
    {
	bool above = a == cmdVIOPENABOVE;
	cont = vi_exec(doc, above ? cmdHOME : cmdEND, 1);
	if ( cont )
	    vi_open_line(doc, above);
	return true;
    }
    // Everything else the @normal table names (x del, u undo, n
    // findnext, : colon, / find ...) runs count times through the ONE
    // dispatcher.
    cont = vi_exec(doc, a.as_integer(), n);
    return true;
}
