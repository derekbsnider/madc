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

// True when the document ends in '\n': offset == size is then a caret
// on the EMPTY line past the content lines — the phantom line. The
// caret already reaches it sideways (right, typing, ^K V = bottom); the
// text_buffer counts content lines only, so the navigability rule lives
// here, in the editor model, and every line/column owner below consults
// it — without it vertical motion stops one row short of the bottom.
bool has_phantom_line(long w, long doc)
{
    long total = ui::text_line_count(w, doc);
    if ( total < 1 )
	return false;
    long lst = ui::text_line_start(w, doc, total);
    if ( lst < 0 )
	return false;
    return lst + ui::text_line_len(w, doc, total) < ui::text_size(w, doc);
}

// The 1-based line containing byte offset `off` (1 on an empty document;
// total+1 on the phantom line).
long line_of(long w, long doc, long off)
{
    long total = ui::text_line_count(w, doc);
    long line = 1;
    while ( line < total && ui::text_line_start(w, doc, line + 1) <= off )
	line = line + 1;
    if ( off >= ui::text_size(w, doc) && has_phantom_line(w, doc) )
	line = total + 1;
    return line;
}

// Line start for column math: the phantom line starts at the document's
// end; any other absent line answers 0 (the empty document).
long line_start_of(long w, long doc, long line)
{
    long start = ui::text_line_start(w, doc, line);
    if ( start >= 0 )
	return start;
    if ( line > ui::text_line_count(w, doc) && has_phantom_line(w, doc) )
	return ui::text_size(w, doc);
    return 0;
}

// ---- the ONE text-mutation owner --------------------------------------
// Every editor insert/erase routes through ed_text_insert / ed_text_erase,
// never ui::text_* directly: the es bag carries byte-anchored rows (the
// themed highlight spans) that must SHIFT with the text they were computed
// for — without it every keystroke above a span repaints the document one
// byte off per character (the madcide "typing spaces re-shades everything"
// defect). Spans still go stale in CONTENT until the next (re)parse
// replaces them; their geometry stays true. A span an erase fully covers
// is dropped; one an insert lands inside grows.
void shift_hspans(long w, long es, long off, long delta)
{
    var hs;
    ui::get(hs, w, es, "hspans");
    if ( hs.is_null() || php::count(hs) == 0 )
	return;
    var out = {};
    for ( var r : hs )
    {
	long s = r["s"].as_integer();
	long e = r["e"].as_integer();
	if ( delta < 0 )
	{
	    long dl = -delta;
	    if ( s >= off )
		s = s >= off + dl ? s + delta : off;
	    if ( e >= off )
		e = e >= off + dl ? e + delta : off;
	}
	else
	{
	    if ( s >= off )
		s = s + delta;
	    if ( e > off )
		e = e + delta;
	}
	if ( e > s )
	{
	    var nr;
	    nr["s"] = s;
	    nr["e"] = e;
	    nr["c"] = r["c"];
	    out.push(nr);
	}
    }
    ui::set(w, es, "hspans", out);
}

void ed_text_insert(long w, long es, long doc, long off, const char *s)
{
    ui::text_insert(w, doc, off, s);
    shift_hspans(w, es, off, strlen(s));
}

void ed_text_erase(long w, long es, long doc, long off, long n)
{
    ui::text_erase(w, doc, off, n);
    shift_hspans(w, es, off, -n);
}

// A caret on `line` keeping column `col` when the line is long enough —
// the vertical-movement rule. A target past the content lines lands on
// the phantom line when there is one (down/pgdn reach the bottom; a
// too-big go-to-line lands at the end, JOE's rule) and clamps otherwise.
long caret_to_line(long w, long doc, long line, long col)
{
    long total = ui::text_line_count(w, doc);
    if ( line < 1 )
	line = 1;
    if ( total > 0 && line > total )
    {
	if ( has_phantom_line(w, doc) )
	    return ui::text_size(w, doc);
	line = total;
    }
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
    var affs;
    ui::affordances(affs, w, es);
    for ( var a : affs )
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
    ui::set(w, es, "adoc", doc);	// the subject-document rule's key
    derive_access(w, es);
    return doc;
}

// Dispatch a turn-cadence command through the SAME script verb the line
// editor runs; the answer (or refusal) becomes the status message. False
// = the verb ended the session (q/q! set the quit flag).
bool do_verb(long w, long es, const char *verb)
{
    var out;
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

// THE clamp-a-caret-to-a-document rule — one owner (IDE-9e windows
// carry per-window carets that go stale when the other window edits;
// their restore and snapshot both ride this).
long clamp_caret_to(long w, long doc, long caret)
{
    long size = ui::text_size(w, doc);
    if ( caret > size )
	caret = size;
    if ( caret < 0 )
	caret = 0;
    return caret;
}

// The ES caret clamped to the live document — every event application
// starts here.
long clamped_caret(long w, long es, long doc)
{
    return clamp_caret_to(w, doc, es_int(w, es, "caret", 0));
}

// The {caret, modified} payload rule — ONE spelling; checkpoint, undo,
// and redo all ride it, so every history entry restores document and
// interaction state together.
void edit_meta(var &m, long w, long es, long doc, long caret)
{
    var p;
    p["caret"] = caret;
    var mod;
    ui::get(mod, w, doc, "modified");
    // Through a bool VARIABLE: a bare `a && b` is int-typed (C), and the
    // keyed slot would store integer kind — restore_meta reads boolean.
    bool dirty = !mod.is_null() && mod.as_boolean();
    p["modified"] = dirty;
    m = p;
}

void restore_meta(long w, long es, long doc, var &m)
{
    ui::set(w, es, "caret", m["caret"].as_integer());
    ui::set(w, doc, "modified", m["modified"].as_boolean());
}

// One undo step (IDE-2): checkpoint BEFORE a mutation with the
// interaction state that belongs to that moment.
void edit_checkpoint(long w, long es, long doc, long caret)
{
    var m;
    edit_meta(m, w, es, doc, caret);
    ui::text_checkpoint(w, doc, m);
}

// Undo/redo (madcide v2 grew redo): the CURRENT payload rides to the
// opposite stack, so walking either direction restores the caret that
// belonged to the document being restored.
bool undo_edit(long w, long es, long doc)
{
    var now;
    edit_meta(now, w, es, doc, clamped_caret(w, es, doc));
    var m;
    if ( !ui::text_undo(m, w, doc, now) )
    {
	ui::set(w, es, "msg", "Nothing to undo.");
	return false;
    }
    restore_meta(w, es, doc, m);
    return true;
}

bool redo_edit(long w, long es, long doc)
{
    var now;
    edit_meta(now, w, es, doc, clamped_caret(w, es, doc));
    var m;
    if ( !ui::text_redo(m, w, doc, now) )
    {
	ui::set(w, es, "msg", "Nothing to redo.");
	return false;
    }
    restore_meta(w, es, doc, m);
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
    var t;
    ui::text(t, w, doc);
    ui::set(w, es, "clip", perl::substr(t, lo, hi - lo));
    if ( editable )
    {
	edit_checkpoint(w, es, doc, caret);
	ed_text_erase(w, es, doc, lo, hi - lo);
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
	ed_text_insert(w, es, doc, caret, c.c_str());
	ui::set(w, es, "caret", caret + strlen(c.c_str()));
	ui::set(w, doc, "modified", true);
    }
}

// The kind=="text" arm both editors share: search-pattern append while
// searching, else a gated insert at the caret.
void edit_text_run(long w, long es, long doc, long caret, var &ev)
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
	ed_text_insert(w, es, doc, caret, t.c_str());
	ui::set(w, es, "caret", caret + strlen(t.c_str()));
	ui::set(w, doc, "modified", true);
    }
}

// THE find rule, one seat: the stored es "search" pattern from caret+1
// with wrap to the top; "Not found." or the caret moves. vised's search
// enter and madcide's find prompt / ^L find-next all repeat it.
void do_find(long w, long es, long doc, long caret)
{
    var pat;
    ui::get(pat, w, es, "search");
    if ( pat.is_null() || strlen(pat.c_str()) == 0 )
    {
	ui::set(w, es, "msg", "No search pattern.");
	return;
    }
    long hit = ui::text_find(w, doc, caret + 1, pat.c_str());
    if ( hit < 0 )
	hit = ui::text_find(w, doc, 0, pat.c_str());
    if ( hit < 0 )
	ui::set(w, es, "msg", "Not found.");
    else
	ui::set(w, es, "caret", hit);
}

// Search-mode key handling; every key is consumed while searching.
bool search_key(long w, long es, long doc, long caret, var &ev)
{
    var k = format("{}", ev["key"]);
    if ( k == "enter" )
    {
	var pat;
	ui::get(pat, w, es, "search");
	ui::set(w, es, "searching", false);
	if ( strlen(pat.c_str()) == 0 )
	    return true;	// silent on empty (the pinned transcript)
	do_find(w, es, doc, caret);
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

// ---- shared projection pieces (one rule, one seat; scoped-dupaudit
// consolidation — vised and madcide compose the same document nodes) ----

// The heading bar: the caller's label + the badge rule (read-only wins
// over modified).
void compose_heading(var &head, long w, long es, long doc,
		     const char *label)
{
    var h;
    h["role"] = "heading";
    h["label"] = label;
    var m;
    ui::get(m, w, doc, "modified");
    if ( !es_flag(w, es, "editable") )
	h["content"] = "[read-only]";
    else if ( !m.is_null() && m.as_boolean() )
	h["content"] = "[modified]";
    head = h;
}

// The edit window bound to the document: caret + selection as hints
// ({caret, sel_start, sel_end} byte offsets). The selection runs
// [mark, bend-else-caret] — except under a TWO-POINT personality (the
// es "selblock" flag, derived from the loaded profile's vocabulary:
// JOE's ^K B / ^K K), where a mark WITHOUT an end lights nothing; an
// editor that never sets `bend` or the flag (vised) keeps the original
// live mark..caret rule unchanged.
void compose_edit_node(var &ed, long w, long es, long doc, long caret)
{
    var e;
    e["role"] = "edit";
    var t;
    ui::text(t, w, doc);
    e["content"] = t;
    var hints;
    hints["caret"] = caret;
    long mark = es_int(w, es, "mark", -1);
    long bend = es_int(w, es, "bend", -1);
    long send = bend >= 0 ? bend : caret;
    if ( bend < 0 && es_flag(w, es, "selblock") )
	send = mark;
    if ( mark >= 0 && mark != send )
    {
	hints["sel_start"] = mark < send ? mark : send;
	hints["sel_end"] = mark < send ? send : mark;
    }
    e["hints"] = hints;
    ed = e;
}

// The status line's priority rule: prompt (madcide's one prompt mode —
// vised never sets it) > search prompt > transient message > "Ln N,
// Col M" + the caller's idle suffix (vised's key legend; empty for
// madcide).
void compose_status(var &status, long w, long es, long doc, long caret,
		    const char *idle_suffix)
{
    var st;
    st["role"] = "status";
    var msg;
    ui::get(msg, w, es, "msg");
    var pr;
    ui::get(pr, w, es, "prompt");
    if ( !pr.is_null() && strlen(pr.c_str()) > 0 )
    {
	var pi;
	ui::get(pi, w, es, "pinput");
	st["content"] = format("{}: {}", pr, pi);
    }
    else if ( es_flag(w, es, "searching") )
    {
	var s;
	ui::get(s, w, es, "search");
	st["content"] = format("Find: {}", s);
    }
    else if ( !msg.is_null() && strlen(msg.c_str()) > 0 )
	st["content"] = msg;
    else
    {
	long line = line_of(w, doc, caret);
	long start = line_start_of(w, doc, line);
	st["content"] = format("Ln {}, Col {}{}", line, caret - start + 1,
			       idle_suffix);
    }
    status = st;
}

// The Save/Find/Quit menu — the SAME w/f/q actions both editors
// dispatch to the same script verbs (criterion 4's choice tree).
void compose_menu(var &menu)
{
    var save;
    save["role"] = "item";
    save["content"] = "Save";
    var sa = {};
    sa.push("w");
    save["actions"] = sa;
    var find;
    find["role"] = "item";
    find["content"] = "Find";
    var fa = {};
    fa.push("f");
    find["actions"] = fa;
    var quit;
    quit["role"] = "item";
    quit["content"] = "Quit";
    var qa = {};
    qa.push("q");
    quit["actions"] = qa;
    var mn;
    mn["role"] = "choice";
    var opts = {};
    opts.push(save);
    opts.push(find);
    opts.push(quit);
    mn["children"] = opts;
    menu = mn;
}

// Caret movement and the direct edit keys (backspace/del/enter). True =
// the key was one of ours; false = the dispatcher's key, not handled.
bool edit_key(long w, long es, long doc, long caret, var &ev)
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
    long start = line_start_of(w, doc, line);
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
	    ed_text_erase(w, es, doc, caret - 1, 1);
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
	    ed_text_erase(w, es, doc, caret, 1);
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
	    ed_text_insert(w, es, doc, caret, "\n");
	    ui::set(w, es, "caret", caret + 1);
	    ui::set(w, doc, "modified", true);
	}
	return true;
    }
    return false;	// not an edit-core key — the dispatcher's business
}
