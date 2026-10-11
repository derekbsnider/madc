// vised_core.inc — the example editor's core, terminal-free (the loop is
// vised.mad's; tests/testvised.mad drives this headlessly). It stands on
// madc's PUBLIC surface only:
//   - ui:: — the projection tree a frontend renders, the events it sends;
//   - the document component (ui::doc_*) — the file, read-only, modified,
//     the document's undo — and an ANCHOR for the caret, which follows
//     every edit, whoever makes it;
//   - the edit notification (ui::doc_subscribe) — every change, one path;
//   - madc's colon mode (madc::ex_session) — `:w`, `:q` and every ex
//     command; ^S / ^Q / ^X are `:w` / `:q` / `:q!`.

enum vised_mode : unsigned char { vmEDIT, vmCOLON };

struct vised
{
    long w;
    long doc;
    long caret;			// an anchor id on doc
    madc::ex_session *ex;	// madc's colon mode over the world
    long mode;			// a vised_mode
    var line;			// the colon line being typed
    var msg;			// the status line's answer
};

// The edit notification's subscriber: it counts the document's changes on
// the document's bag (a subscriber may read and keep, never edit).
void vised_heard(long w, long entity, var &change)
{
    var n;
    ui::get(n, w, entity, "edits");
    ui::set(w, entity, "edits", n.is_null() ? 1 : n.as_integer() + 1);
}

// Open `path` (a missing file is a new, empty document under its name).
bool vised_open(vised &v, const char *path, bool ro)
{
    v.w = ui::world_new();
    v.doc = ui::doc_open(v.w, path);
    if ( v.doc == 0 && !php::file_exists(path) )
    {
	v.doc = ui::doc_new(v.w);
	ui::doc_set_path(v.w, v.doc, path);
    }
    if ( v.doc == 0 )
	return false;
    if ( ro )
	ui::doc_set_read_only(v.w, v.doc, true);
    v.caret = ui::anchor_add(v.w, v.doc, 0, true);
    v.ex = new madc::ex_session(v.w);
    v.ex->use(v.doc);
    v.mode = vmEDIT;
    v.line = "";
    v.msg = "";
    ui::doc_subscribe(v.w, vised_heard);
    return true;
}

void vised_close(vised &v)
{
    delete v.ex;		// its anchors are the world's: before the world
    ui::world_close(v.w);
}

long vised_caret(vised &v)
{
    return ui::anchor_offset(v.w, v.doc, v.caret);
}

void vised_put_caret(vised &v, long off)
{
    long size = ui::text_size(v.w, v.doc);
    ui::anchor_move(v.w, v.doc, v.caret, off < 0 ? 0 : (off > size ? size : off));
}

// One edit at the caret — `del` bytes from `off` erased, `ins` inserted —
// as one undo step carrying the caret. A read-only document refuses it.
void vised_edit(vised &v, long off, long del, const char *ins)
{
    if ( ui::doc_read_only(v.w, v.doc) )
    {
	v.msg = "Document is read-only.";
	return;
    }
    var meta = { "caret": vised_caret(v) };
    ui::doc_checkpoint(v.w, v.doc, meta);
    if ( del > 0 )
	ui::text_erase(v.w, v.doc, off, del);
    if ( strlen(ins) > 0 )
	ui::text_insert(v.w, v.doc, off, ins);
}

// One colon command, run by madc's ex core with `.` on the caret's line;
// the caret goes to `.` when the command moved it. False = quit.
bool vised_colon(vised &v, const char *cmd)
{
    v.ex->use(v.doc);
    v.ex->set_line(ui::text_line_of(v.w, v.doc, vised_caret(v)));
    long before = v.ex->line();
    var r;
    v.ex->run(r, cmd);
    if ( v.ex->line() != before && v.ex->line() > 0 )
	vised_put_caret(v, ui::text_line_start(v.w, v.doc, v.ex->line()));
    if ( !r["ok"].as_boolean() )
	v.msg = r["error"];
    else
	v.msg = php::rtrim(r["shown"].c_str());
    return !r["quit"].as_boolean();
}

// The document's undo: the text, its anchors and the caret it carried.
void vised_undo(vised &v)
{
    var now = { "caret": vised_caret(v) };
    var back;
    if ( !ui::doc_undo(back, v.w, v.doc, now) )
    {
	v.msg = "Nothing to undo.";
	return;
    }
    if ( !back["caret"].is_null() )
	vised_put_caret(v, back["caret"].as_integer());
}

// A key on the caret: motion, the editing keys, the chords. False = quit.
bool vised_key(vised &v, const char *k)
{
    long c = vised_caret(v);
    long line = ui::text_line_of(v.w, v.doc, c);
    long col = c - ui::text_line_start(v.w, v.doc, line);
    if ( strcmp(k, "left") == 0 )
	vised_put_caret(v, c - 1);
    else if ( strcmp(k, "right") == 0 )
	vised_put_caret(v, c + 1);
    else if ( strcmp(k, "home") == 0 )
	vised_put_caret(v, c - col);
    else if ( strcmp(k, "end") == 0 )
	vised_put_caret(v, c - col + ui::text_line_len(v.w, v.doc, line));
    else if ( strcmp(k, "up") == 0 || strcmp(k, "down") == 0 )
    {
	long to = strcmp(k, "up") == 0 ? line - 1 : line + 1;
	if ( to >= 1 && to <= ui::text_line_count(v.w, v.doc) )
	{
	    long len = ui::text_line_len(v.w, v.doc, to);
	    vised_put_caret(v, ui::text_line_start(v.w, v.doc, to) + (col < len ? col : len));
	}
    }
    else if ( strcmp(k, "backspace") == 0 && c > 0 )
	vised_edit(v, c - 1, 1, "");
    else if ( strcmp(k, "del") == 0 && c < ui::text_size(v.w, v.doc) )
	vised_edit(v, c, 1, "");
    else if ( strcmp(k, "enter") == 0 )
	vised_edit(v, c, 0, "\n");
    else if ( strcmp(k, "^z") == 0 )
	vised_undo(v);
    else if ( strcmp(k, "^s") == 0 )
	return vised_colon(v, "w");
    else if ( strcmp(k, "^q") == 0 )
	return vised_colon(v, "q");
    else if ( strcmp(k, "^x") == 0 )
	return vised_colon(v, "q!");
    else if ( strcmp(k, "esc") == 0 )
    {
	v.mode = vmCOLON;
	v.line = "";
    }
    return true;
}

// ONE event (the value ui::event produces). False = the session ends.
bool vised_event(vised &v, var &ev)
{
    v.msg = "";
    long kind = ev["event_code"].as_integer();
    if ( v.mode == vmCOLON )
    {
	if ( kind == ui::event_kind::text )
	    v.line = format("{}{}", v.line, ev["text"]);
	else if ( kind == ui::event_kind::key )
	{
	    var k = ev["key"];
	    if ( k == "enter" )
	    {
		v.mode = vmEDIT;
		return vised_colon(v, v.line.c_str());
	    }
	    if ( k == "esc" )
		v.mode = vmEDIT;
	    else if ( k == "backspace" && strlen(v.line.c_str()) > 0 )
		v.line = perl::substr(v.line, 0, strlen(v.line.c_str()) - 1);
	}
	return true;
    }
    if ( kind == ui::event_kind::text )
    {
	long c = vised_caret(v);
	vised_edit(v, c, 0, ev["text"].c_str());
	return true;
    }
    if ( kind == ui::event_kind::pointer && ev["offset"].as_integer() >= 0 )
    {
	vised_put_caret(v, ev["offset"].as_integer());
	return true;
    }
    if ( kind == ui::event_kind::key )
	return vised_key(v, ev["key"].c_str());
    return true;		// focus, resize: the loop recomposes
}

// The projection: the file's heading, the edit window (the caret as a
// hint), the status line (the colon line while one is typed).
void vised_tree(var &root, vised &v)
{
    var path;
    ui::doc_path(path, v.w, v.doc);
    var head = { "role": "heading", "label": path };
    if ( ui::doc_read_only(v.w, v.doc) )
	head["content"] = "[read-only]";
    else if ( ui::doc_modified(v.w, v.doc) )
	head["content"] = "[modified]";
    var text;
    ui::text(text, v.w, v.doc);
    var ed = { "role": "edit", "content": text, "subject": v.doc };
    ed["hints"] = { "caret": vised_caret(v) };
    var st = { "role": "status" };
    long c = vised_caret(v);
    long line = ui::text_line_of(v.w, v.doc, c);
    if ( v.mode == vmCOLON )
	st["content"] = format(":{}", v.line);
    else if ( strlen(v.msg.c_str()) > 0 )
	st["content"] = v.msg;
    else
	st["content"] = format("Ln {}, Col {}  ^S save ^Z undo ^Q quit ^X discard Esc :command",
			       line, c - ui::text_line_start(v.w, v.doc, line) + 1);
    var kids = {};
    kids.push(head);
    kids.push(ed);
    kids.push(st);
    root = { "role": "group", "children": kids };
}
