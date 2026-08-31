// vised_core.inc — the visual (TUI) half of tools/texteditor (Track
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
bool apply_event(long w, long es, long doc, var &ev)
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
void compose_tree(var &root, long w, long es, long doc)
{
    long caret = es_int(w, es, "caret", 0);

    var head;
    var p;
    ui::get(p, w, doc, "path");
    compose_heading(head, w, es, doc, p.c_str());
    var ed;
    compose_edit_node(ed, w, es, doc, caret);
    var status;
    compose_status(status, w, es, doc, caret,
	"  ^S save ^F find ^B mark ^K cut ^U paste ^Q quit ^X discard");
    var menu;
    compose_menu(menu);

    var r;
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
	var tree;
	compose_tree(tree, w, es, doc);
	ui::tui_render(t, w, tree);
	var ev;
	if ( !ui::tui_event(ev, t, w) )
	    break;
	run = apply_event(w, es, doc, ev);
    }
    ui::tui_close(t);
    ui::world_close(w);
}
