// vised_core.inc — the visual (TUI) half of examples/texteditor (Track
// 7.2 R5; design doc §7). The SEMANTIC core is terminal-free:
// apply_event() maps semantic events onto the SAME document actions the
// line editor uses — the engine's range primitives (insert/erase at the
// caret) for typing-cadence edits, and the SAME script verbs for
// turn-cadence commands (^S = the w verb, ^Q = q, ^X = q!, all through
// ui::act) — design success criterion 3: two interaction techniques,
// zero duplicated mutation logic. The read-only rule stays in its ONE
// seat (checks/editable.madv): typing consults the flag DERIVED from the
// enumerated affordances; verbs are refused by the same check at
// dispatch.
//
// Interaction state (caret, mark, search pattern, search mode) lives on
// the editor-state entity's bag (§7.2); presentation state (scroll,
// focus, selection highlight) stays inside the renderer. Transient
// player-facing text (a refusal, a verb's answer) rides the "msg" bag
// key and clears on the next event.
//
// Requires lined_core.inc (setup_document, bind_lineed_verbs) included
// first by the entry file. run_visual() is the TUI loop; everything else
// is loop-free and drives headlessly (tests/testvised.mad).

long es_int(long w, long es, const char *key, long dflt)
{
    var v;
    ui::get(v, w, es, key);
    if ( v.is_null() )
	return dflt;
    return v.as_integer();
}

bool es_flag(long w, long es, const char *key)
{
    var v;
    ui::get(v, w, es, key);
    if ( v.is_null() )
	return false;
    return v.as_boolean();
}

// The 1-based line containing byte offset `off` (1 on an empty document).
long line_of(long w, long doc, long off)
{
    long total = ui::text_line_count(w, doc);
    long line = 1;
    while ( line < total && ui::text_line_start(w, doc, line + 1) <= off )
	line = line + 1;
    return line;
}

// A caret on `line` keeping column `col` when the line is long enough —
// the vertical-movement rule.
long caret_to_line(long w, long doc, long line, long col)
{
    long total = ui::text_line_count(w, doc);
    if ( line < 1 )
	line = 1;
    if ( total > 0 && line > total )
	line = total;
    long start = ui::text_line_start(w, doc, line);
    if ( start < 0 )
	return 0;
    long len = ui::text_line_len(w, doc, line);
    if ( col > len )
	col = len;
    return start + col;
}

// The typing gate, derived ONCE from the enumerated affordances: the
// read-only rule lives in the shared availability check; the editor
// reads its verdict (verb `c` disabled = the document is not editable)
// instead of re-implementing it.
bool derive_editable(long w, long es)
{
    value affs;
    ui::affordances(affs, w, es);
    for ( value a : affs )
    {
	var act = format("{}", a["action"]);
	if ( act == "c" )
	    return a["enabled"].as_boolean();
    }
    return true;
}

// Dispatch a turn-cadence command through the SAME script verb the line
// editor runs; the answer (or refusal) becomes the status message. False
// = the verb ended the session (q/q! set the quit flag).
bool do_verb(long w, long es, const char *verb)
{
    value out;
    ui::act(out, w, es, verb, "");
    ui::set(w, es, "msg", out.c_str());
    var qd;
    ui::get(qd, w, es, "quit");
    if ( !qd.is_null() && qd.as_boolean() )
	return false;
    return true;
}

void enter_search(long w, long es)
{
    ui::set(w, es, "searching", true);
    ui::set(w, es, "search", "");
}

// Cut (or, read-only, just copy) the mark..caret region: the clipboard
// is interaction state on the editor-state bag.
void cut_selection(long w, long es, long doc, long caret, bool editable)
{
    long mark = es_int(w, es, "mark", -1);
    if ( mark < 0 )
    {
	ui::set(w, es, "msg", "No mark set (^B).");
	return;
    }
    long lo = mark < caret ? mark : caret;
    long hi = mark < caret ? caret : mark;
    ui::set(w, es, "mark", -1);
    if ( lo == hi )
	return;
    value t;
    ui::text(t, w, doc);
    ui::set(w, es, "clip", perl::substr(t, lo, hi - lo));
    if ( editable )
    {
	ui::text_erase(w, doc, lo, hi - lo);
	ui::set(w, es, "caret", lo);
	ui::set(w, doc, "modified", true);
    }
    else
	ui::set(w, es, "msg", "Document is read-only. (Copied.)");
}

// Apply ONE semantic event (the value shape ui::tui_event produces).
// False = the session should end.
bool apply_event(long w, long es, long doc, value &ev)
{
    var kind = format("{}", ev["event"]);
    long caret = es_int(w, es, "caret", 0);
    long size = ui::text_size(w, doc);
    if ( caret > size )
	caret = size;
    if ( caret < 0 )
	caret = 0;
    ui::set(w, es, "msg", "");
    bool editable = es_flag(w, es, "editable");
    bool searching = es_flag(w, es, "searching");

    if ( kind == "text" )
    {
	var t = format("{}", ev["text"]);
	if ( searching )
	{
	    var s;
	    ui::get(s, w, es, "search");
	    var pat = format("{}{}", s, t);
	    ui::set(w, es, "search", pat.c_str());
	}
	else if ( !editable )
	    ui::set(w, es, "msg", "Document is read-only.");
	else
	{
	    ui::text_insert(w, doc, caret, t.c_str());
	    ui::set(w, es, "caret", caret + strlen(t.c_str()));
	    ui::set(w, doc, "modified", true);
	}
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

    var k = format("{}", ev["key"]);
    if ( searching )
    {
	if ( k == "enter" )
	{
	    var pat;
	    ui::get(pat, w, es, "search");
	    ui::set(w, es, "searching", false);
	    if ( strlen(pat.c_str()) == 0 )
		return true;
	    long hit = ui::text_find(w, doc, caret + 1, pat.c_str());
	    if ( hit < 0 )
		hit = ui::text_find(w, doc, 0, pat.c_str());
	    if ( hit < 0 )
		ui::set(w, es, "msg", "Not found.");
	    else
		ui::set(w, es, "caret", hit);
	    return true;
	}
	if ( k == "esc" )
	{
	    ui::set(w, es, "searching", false);
	    return true;
	}
	if ( k == "backspace" )
	{
	    var s;
	    ui::get(s, w, es, "search");
	    long n = strlen(s.c_str());
	    if ( n > 0 )
		ui::set(w, es, "search", perl::substr(s, 0, n - 1));
	    return true;
	}
	return true;	// other keys are inert while searching
    }

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
	var c;
	ui::get(c, w, es, "clip");
	if ( !c.is_null() && strlen(c.c_str()) > 0 )
	{
	    if ( !editable )
		ui::set(w, es, "msg", "Document is read-only.");
	    else
	    {
		ui::text_insert(w, doc, caret, c.c_str());
		ui::set(w, es, "caret", caret + strlen(c.c_str()));
		ui::set(w, doc, "modified", true);
	    }
	}
	return true;
    }
    if ( k == "left" )
    {
	if ( caret > 0 )
	    ui::set(w, es, "caret", caret - 1);
	return true;
    }
    if ( k == "right" )
    {
	if ( caret < size )
	    ui::set(w, es, "caret", caret + 1);
	return true;
    }
    long line = line_of(w, doc, caret);
    long start = ui::text_line_start(w, doc, line);
    if ( start < 0 )
	start = 0;
    long col = caret - start;
    if ( k == "home" )
    {
	ui::set(w, es, "caret", start);
	return true;
    }
    if ( k == "end" )
    {
	long len = ui::text_line_len(w, doc, line);
	if ( len < 0 )
	    len = 0;
	ui::set(w, es, "caret", start + len);
	return true;
    }
    if ( k == "up" )
    {
	ui::set(w, es, "caret", caret_to_line(w, doc, line - 1, col));
	return true;
    }
    if ( k == "down" )
    {
	ui::set(w, es, "caret", caret_to_line(w, doc, line + 1, col));
	return true;
    }
    if ( k == "pgup" )
    {
	ui::set(w, es, "caret", caret_to_line(w, doc, line - 10, col));
	return true;
    }
    if ( k == "pgdn" )
    {
	ui::set(w, es, "caret", caret_to_line(w, doc, line + 10, col));
	return true;
    }
    if ( k == "backspace" )
    {
	if ( !editable )
	    ui::set(w, es, "msg", "Document is read-only.");
	else if ( caret > 0 )
	{
	    ui::text_erase(w, doc, caret - 1, 1);
	    ui::set(w, es, "caret", caret - 1);
	    ui::set(w, doc, "modified", true);
	}
	return true;
    }
    if ( k == "del" )
    {
	if ( !editable )
	    ui::set(w, es, "msg", "Document is read-only.");
	else if ( caret < size )
	{
	    ui::text_erase(w, doc, caret, 1);
	    ui::set(w, doc, "modified", true);
	}
	return true;
    }
    if ( k == "enter" )
    {
	if ( !editable )
	    ui::set(w, es, "msg", "Document is read-only.");
	else
	{
	    ui::text_insert(w, doc, caret, "\n");
	    ui::set(w, es, "caret", caret + 1);
	    ui::set(w, doc, "modified", true);
	}
	return true;
    }
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
    long doc = setup_document(w, path, ro);
    if ( doc == 0 )
    {
	printf("cannot read %s\n", path);
	return;
    }
    long es = ui::create(w, "editor-state");
    bind_lineed_verbs(w);
    ui::set(w, es, "caret", 0);
    ui::set(w, es, "mark", -1);
    ui::set(w, es, "editable", derive_editable(w, es));
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
