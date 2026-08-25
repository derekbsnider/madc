// vised_core.inc — the visual (TUI) half of examples/texteditor (Track
// 7.2 R5; design doc §7). The SEMANTIC core is terminal-free:
// apply_event() maps semantic events onto the SAME document actions the
// line editor uses — the shared editor event core (editor_events.inc,
// split out for madcide IDE-4) does the editing; THIS file is only
// vised's DISPATCH: Pico-style single control chords (^S save, ^Q quit,
// ^X discard, ^F find, ^B mark, ^K cut, ^U paste — fine for an example
// editor, per the owner; madcide's configurable JOE/WordStar chords ride
// the bindings table instead), the menu's choose actions, and the
// composed projection tree.
//
// Interaction state (caret, mark, search pattern, search mode) lives on
// the editor-state entity's bag (§7.2); presentation state (scroll,
// focus, selection highlight) stays inside the renderer. Transient
// player-facing text (a refusal, a verb's answer) rides the "msg" bag
// key and clears on the next event.
//
// Requires lined_core.inc + editor_events.inc included first by the
// entry file. run_visual() is the TUI loop; everything else is loop-free
// and drives headlessly (tests/testvised.mad).

// Apply ONE semantic event (the value shape ui::tui_event produces).
// False = the session should end.
bool apply_event(long w, long es, long doc, value &ev)
{
    var kind = format("{}", ev["event"]);
    long caret = clamped_caret(w, es, doc);
    ui::set(w, es, "msg", "");
    bool editable = es_flag(w, es, "editable");
    bool searching = es_flag(w, es, "searching");

    if ( kind == "text" )
    {
	edit_text_run(w, es, doc, caret, ev);
	return true;
    }
    if ( kind == "choose" )
    {
	var a = format("{}", ev["action"]);
	if ( a == "w" )
	    return do_verb(w, es, "w");
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

    if ( searching )
	return search_key(w, es, doc, caret, ev);

    var k = format("{}", ev["key"]);
    if ( k == "^s" )
	return do_verb(w, es, "w");
    if ( k == "^q" )
	return do_verb(w, es, "q");
    if ( k == "^x" )
	return do_verb(w, es, "q!");
    if ( k == "^f" )
    {
	enter_search(w, es);
	return true;
    }
    if ( k == "^b" )
    {
	long mark = es_int(w, es, "mark", -1);
	if ( mark < 0 )
	    ui::set(w, es, "mark", caret);
	else
	    ui::set(w, es, "mark", -1);
	return true;
    }
    if ( k == "^k" )
    {
	cut_selection(w, es, doc, caret, editable);
	return true;
    }
    if ( k == "^u" )
    {
	paste_clip(w, es, doc, caret);
	return true;
    }
    edit_key(w, es, doc, caret, ev);
    return true;	// unmapped keys are inert
}

// Compose the projection AS DATA: the same tree shape ui::render_tree
// would typeset sequentially — heading bar, the edit window (caret and
// mark..caret selection as hints), a status line, and the Choice menu
// (criterion 4: numbered in line mode, navigable here).
void compose_tree(value &root, long w, long es, long doc)
{
    long caret = es_int(w, es, "caret", 0);
    bool editable = es_flag(w, es, "editable");

    value head;
    head["role"] = "heading";
    var p;
    ui::get(p, w, doc, "path");
    head["label"] = p;
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
	status["content"] = format(
	    "Ln {}, Col {}  ^S save ^F find ^B mark ^K cut ^U paste ^Q quit ^X discard",
	    line, caret - start + 1);
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

    value r;
    r["role"] = "group";
    var kids = {};
    kids.push(head);
    kids.push(ed);
    kids.push(status);
    kids.push(menu);
    r["children"] = kids;
    root = r;
}

// The TUI loop: compose as data -> render -> one semantic event ->
// apply, until a quit verb says stop.
void run_visual(const char *path, bool ro)
{
    long w = ui::world_new();
    long doc = setup_editor(w, path, ro);
    if ( doc == 0 )
    {
	printf("cannot read %s\n", path);
	return;
    }
    long es = ui::entity_by_name(w, "editor-state");
    long t = ui::tui_open();
    if ( t == 0 )
	return;
    bool run = true;
    while ( run )
    {
	value tree;
	compose_tree(tree, w, es, doc);
	ui::tui_render(t, w, tree);
	value ev;
	if ( !ui::tui_event(ev, t, w) )
	    break;
	run = apply_event(w, es, doc, ev);
    }
    ui::tui_close(t);
    ui::world_close(w);
}
