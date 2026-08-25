// editor_events.inc — the SHARED editor event core (madcide IDE-4 split
// out of vised_core.inc, behavior-preserving: testvised pins it). Both
// visual editors (vised, madcide) drive the SAME document actions with
// zero duplicated mutation logic: typing-cadence edits are engine range
// primitives at the caret; turn-cadence commands are the SAME script
// verbs through ui::act. What differs per editor is only the DISPATCH —
// vised's Pico single chords, madcide's bindings-table actions — layered
// on these functions.
//
// Undo (IDE-2) lives here: every mutating arm checkpoints BEFORE the
// edit, carrying {caret, modified} as the opaque payload, so undo_edit
// restores document, caret, and dirtiness together. One coalesced event
// = one semantic edit = one undo step.
//
// Requires lined_core.inc (setup_document, bind_lineed_verbs) included
// first by the entry file. Everything here is loop-free and terminal-
// free (the headless gates drive it directly).

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

// The typing gate AND its refusal prose, derived ONCE from the
// enumerated affordances: the read-only rule lives in the shared
// availability check; the editor stores its verdict and its reason
// (verb `c`'s enumerated state) on the editor-state bag instead of
// re-implementing or re-spelling either.
void derive_access(long w, long es)
{
    bool editable = true;
    value affs;
    ui::affordances(affs, w, es);
    for ( value a : affs )
    {
	var act = format("{}", a["action"]);
	if ( act == "c" )
	{
	    editable = a["enabled"].as_boolean();
	    var reason = format("{}", a["reason"]);
	    ui::set(w, es, "ro_msg", reason.c_str());
	}
    }
    ui::set(w, es, "editable", editable);
}

// A typing-cadence edit was refused: the message is the CHECK's own
// reason, read back off the bag — one rule, one spelling.
void refuse_edit(long w, long es)
{
    var ro;
    ui::get(ro, w, es, "ro_msg");
    ui::set(w, es, "msg", ro.c_str());
}

// The editor session every entry (vised, madcide, the headless gates)
// opens the same way: document + editor-state + the shared verbs/check +
// the derived access verdict.
long setup_editor(long w, const char *path, bool ro)
{
    long doc = setup_document(w, path, ro);
    if ( doc == 0 )
	return 0;
    long es = ui::create(w, "editor-state");
    bind_lineed_verbs(w);
    ui::set(w, es, "caret", 0);
    ui::set(w, es, "mark", -1);
    derive_access(w, es);
    return doc;
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

// The caret clamped to the live document — every event application
// starts here.
long clamped_caret(long w, long es, long doc)
{
    long caret = es_int(w, es, "caret", 0);
    long size = ui::text_size(w, doc);
    if ( caret > size )
	caret = size;
    if ( caret < 0 )
	caret = 0;
    return caret;
}

// One undo step (IDE-2): checkpoint BEFORE a mutation with the
// interaction state that belongs to that moment.
void edit_checkpoint(long w, long es, long doc, long caret)
{
    value m;
    m["caret"] = caret;
    var mod;
    ui::get(mod, w, doc, "modified");
    // Through a bool VARIABLE: a bare `a && b` is int-typed (C), and the
    // keyed slot would store integer kind — undo_edit reads boolean.
    bool dirty = !mod.is_null() && mod.as_boolean();
    m["modified"] = dirty;
    ui::text_checkpoint(w, doc, m);
}

bool undo_edit(long w, long es, long doc)
{
    value m;
    if ( !ui::text_undo(m, w, doc) )
    {
	ui::set(w, es, "msg", "Nothing to undo.");
	return false;
    }
    ui::set(w, es, "caret", m["caret"].as_integer());
    ui::set(w, doc, "modified", m["modified"].as_boolean());
    return true;
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
	edit_checkpoint(w, es, doc, caret);
	ui::text_erase(w, doc, lo, hi - lo);
	ui::set(w, es, "caret", lo);
	ui::set(w, doc, "modified", true);
    }
    else
    {
	var ro;
	ui::get(ro, w, es, "ro_msg");
	var m = format("{} (Copied.)", ro);
	ui::set(w, es, "msg", m.c_str());
    }
}

// Insert the clipboard at the caret (gated; a silent no-op when empty).
void paste_clip(long w, long es, long doc, long caret)
{
    var c;
    ui::get(c, w, es, "clip");
    if ( c.is_null() || strlen(c.c_str()) == 0 )
	return;
    if ( !es_flag(w, es, "editable") )
	refuse_edit(w, es);
    else
    {
	edit_checkpoint(w, es, doc, caret);
	ui::text_insert(w, doc, caret, c.c_str());
	ui::set(w, es, "caret", caret + strlen(c.c_str()));
	ui::set(w, doc, "modified", true);
    }
}

// The kind=="text" arm both editors share: search-pattern append while
// searching, else a gated insert at the caret.
void edit_text_run(long w, long es, long doc, long caret, value &ev)
{
    var t = format("{}", ev["text"]);
    if ( es_flag(w, es, "searching") )
    {
	var s;
	ui::get(s, w, es, "search");
	var pat = format("{}{}", s, t);
	ui::set(w, es, "search", pat.c_str());
    }
    else if ( !es_flag(w, es, "editable") )
	refuse_edit(w, es);
    else
    {
	edit_checkpoint(w, es, doc, caret);
	ui::text_insert(w, doc, caret, t.c_str());
	ui::set(w, es, "caret", caret + strlen(t.c_str()));
	ui::set(w, doc, "modified", true);
    }
}

// Search-mode key handling; every key is consumed while searching.
bool search_key(long w, long es, long doc, long caret, value &ev)
{
    var k = format("{}", ev["key"]);
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

// Caret movement and the direct edit keys (backspace/del/enter). True =
// the key was one of ours; false = the dispatcher's key, not handled.
bool edit_key(long w, long es, long doc, long caret, value &ev)
{
    var k = format("{}", ev["key"]);
    bool editable = es_flag(w, es, "editable");
    long size = ui::text_size(w, doc);
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
	    refuse_edit(w, es);
	else if ( caret > 0 )
	{
	    edit_checkpoint(w, es, doc, caret);
	    ui::text_erase(w, doc, caret - 1, 1);
	    ui::set(w, es, "caret", caret - 1);
	    ui::set(w, doc, "modified", true);
	}
	return true;
    }
    if ( k == "del" )
    {
	if ( !editable )
	    refuse_edit(w, es);
	else if ( caret < size )
	{
	    edit_checkpoint(w, es, doc, caret);
	    ui::text_erase(w, doc, caret, 1);
	    ui::set(w, doc, "modified", true);
	}
	return true;
    }
    if ( k == "enter" )
    {
	if ( !editable )
	    refuse_edit(w, es);
	else
	{
	    edit_checkpoint(w, es, doc, caret);
	    ui::text_insert(w, doc, caret, "\n");
	    ui::set(w, es, "caret", caret + 1);
	    ui::set(w, doc, "modified", true);
	}
	return true;
    }
    return false;	// not an edit-core key — the dispatcher's business
}
