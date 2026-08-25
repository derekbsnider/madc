// madcide_core.inc — the madc IDE's core (hub doc Phase 2; plan
// docs/plans/2026-08-25-madcide-phase2.md IDE-4). A CONSUMER of Track
// 7.2: the buffer is the texteditor machinery (lined_core.inc +
// editor_events.inc, included by the entry file — one implementation,
// never a fork); the diagnostics pane and outline are PROJECTIONS of
// stored compiler data (madc::diagnostics / madc::outline rows on the
// editor-state bag — composing a tree never calls the compiler; the
// check/save actions refresh the data); keybindings are PROFILES —
// data files mapping key sequences to the action names dispatched here
// (JOE/WordStar ^K chords by default, owner-directed; swap = one
// ui::tui_bind_keys call, the composition path untouched — gate 3).
//
// Interaction state (caret/mark/clip/search + pane, diags, outline,
// profile) lives on the editor-state bag; presentation state stays in
// the renderer. apply_ide_event is terminal-free (tests/testmadcide.mad
// drives it headlessly with the exact event value shapes).

// ---- keybinding profiles as data --------------------------------------
// One binding per line: "SEQ ACTION" — the action is the LAST word, the
// sequence everything before it. '#' lines and blanks skip.
void parse_keys(value &table, const char *text)
{
    value t;
    array lines;
    php::explode(lines, "\n", text);
    long n = php::count(lines);
    for ( long i = 0; i < n; i = i + 1 )
    {
	var line = php::trim(php::array_get_cstr(lines, i));
	const char *ls = line.c_str();
	if ( strlen(ls) == 0 )
	    continue;
	if ( ls[0] == '#' )
	    continue;
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
// TUI is open, install it. False + a status message on failure (the
// previous table stays installed — tui_bind_keys refuses whole tables).
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
    return true;
}

// ---- compiler data on the bag (the meta-level, hub demand 3) ----------
// Refresh the stored diagnostics rows from the LIVE buffer — data only;
// composition reads the bag, and only this (check, save) runs the
// compiler.
long refresh_diags(long w, long es, long doc)
{
    value t;
    ui::text(t, w, doc);
    var p;
    ui::get(p, w, doc, "path");
    value diags;
    madc::diagnostics(diags, t.c_str(), p.c_str());
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
    value t;
    ui::text(t, w, doc);
    var p;
    ui::get(p, w, doc, "path");
    value o;
    madc::outline(o, t.c_str(), p.c_str());
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

// Apply ONE semantic event (the value shapes ui::tui_event produces —
// action events from the bindings table included). False = quit.
bool apply_ide_event(long w, long es, long doc, value &ev)
{
    var kind = format("{}", ev["event"]);
    long caret = clamped_caret(w, es, doc);
    ui::set(w, es, "msg", "");
    bool editable = es_flag(w, es, "editable");

    if ( kind == "action" )
    {
	var a = format("{}", ev["action"]);
	if ( strlen(a.c_str()) == 0 )
	{
	    ui::set(w, es, "msg", format("Unbound: {}", ev["seq"]));
	    return true;
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
	    enter_search(w, es);
	    return true;
	}
	if ( a == "mark" )
	{
	    long mark = es_int(w, es, "mark", -1);
	    if ( mark < 0 )
		ui::set(w, es, "mark", caret);
	    else
		ui::set(w, es, "mark", -1);
	    return true;
	}
	if ( a == "cut" )
	{
	    cut_selection(w, es, doc, caret, editable);
	    return true;
	}
	if ( a == "paste" )
	{
	    paste_clip(w, es, doc, caret);
	    return true;
	}
	if ( a == "undo" )
	{
	    undo_edit(w, es, doc);
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
	ui::set(w, es, "msg", format("Unknown action '{}'.", a));
	return true;
    }
    if ( kind == "text" )
    {
	edit_text_run(w, es, doc, caret, ev);
	return true;
    }
    if ( kind == "choose" )
    {
	var a = format("{}", ev["action"]);
	if ( a == "goto" )
	{
	    goto_pane_row(w, es, doc, ev["option"].as_integer());
	    return true;
	}
	if ( a == "w" )
	    return save_and_check(w, es, doc);
	if ( a == "f" )
	{
	    enter_search(w, es);
	    return true;
	}
	if ( a == "q" )
	    return do_verb(w, es, "q");
	return true;
    }
    if ( kind != "key" )
	return true;	// focus / resize: the loop recomposes anyway

    if ( es_flag(w, es, "searching") )
	return search_key(w, es, doc, caret, ev);
    edit_key(w, es, doc, caret, ev);
    return true;	// unbound keys are inert (chords arrive as actions)
}

// ---- the projection ----------------------------------------------------
// The tree is PROFILE-INDEPENDENT by construction: swapping keybindings
// re-renders (the loop recomposes) without this function changing shape
// or content — the Phase-2 gate's third clause, pinned headlessly.
void compose_ide_tree(value &root, long w, long es, long doc)
{
    long caret = es_int(w, es, "caret", 0);
    bool editable = es_flag(w, es, "editable");

    value head;
    head["role"] = "heading";
    var p;
    ui::get(p, w, doc, "path");
    head["label"] = format("madcide: {}", p);
    var m;
    ui::get(m, w, doc, "modified");
    if ( !editable )
	head["content"] = "[read-only]";
    else if ( !m.is_null() && m.as_boolean() )
	head["content"] = "[modified]";

    value ed;
    ed["role"] = "edit";
    value t;
    ui::text(t, w, doc);
    ed["content"] = t;
    value hints;
    hints["caret"] = caret;
    long mark = es_int(w, es, "mark", -1);
    if ( mark >= 0 && mark != caret )
    {
	hints["sel_start"] = mark < caret ? mark : caret;
	hints["sel_end"] = mark < caret ? caret : mark;
    }
    ed["hints"] = hints;

    value r;
    r["role"] = "group";
    var kids = {};
    kids.push(head);
    kids.push(ed);

    // The lower pane: a projection of the STORED compiler data — a
    // navigable choice whose rows carry the `goto` action.
    var pane;
    ui::get(pane, w, es, "pane");
    if ( !pane.is_null() && strlen(pane.c_str()) > 0 )
    {
	bool outline_pane = pane == "outline";
	value rows;
	if ( outline_pane )
	    ui::get(rows, w, es, "outline");
	else
	    ui::get(rows, w, es, "diags");
	value box;
	box["role"] = "choice";
	if ( outline_pane )
	    box["label"] = "Outline";
	else
	    box["label"] = "Diagnostics";
	var items = {};
	long nrows = 0;
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
	box["children"] = items;
	kids.push(box);
    }

    value status;
    status["role"] = "status";
    var msg;
    ui::get(msg, w, es, "msg");
    if ( es_flag(w, es, "searching") )
    {
	var s;
	ui::get(s, w, es, "search");
	status["content"] = format("Find: {}", s);
    }
    else if ( !msg.is_null() && strlen(msg.c_str()) > 0 )
	status["content"] = msg;
    else
    {
	long line = line_of(w, doc, caret);
	long start = ui::text_line_start(w, doc, line);
	if ( start < 0 )
	    start = 0;
	status["content"] = format("Ln {}, Col {}", line, caret - start + 1);
    }

    value save;
    save["role"] = "item";
    save["content"] = "Save";
    var sa = {};
    sa.push("w");
    save["actions"] = sa;
    value find;
    find["role"] = "item";
    find["content"] = "Find";
    var fa = {};
    fa.push("f");
    find["actions"] = fa;
    value quit;
    quit["role"] = "item";
    quit["content"] = "Quit";
    var qa = {};
    qa.push("q");
    quit["actions"] = qa;
    value menu;
    menu["role"] = "choice";
    var opts = {};
    opts.push(save);
    opts.push(find);
    opts.push(quit);
    menu["children"] = opts;

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
    ui::set(w, es, "profile_dir", "examples/madcide/profiles");
    long t = ui::tui_open();
    if ( t == 0 )
	return;
    ui::set(w, es, "tui", t);
    load_profile(w, es, "joe");
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
    ui::tui_close(t);
    ui::world_close(w);
}
