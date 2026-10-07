// madcide_preview.inc — View ▸ Markdown Preview (plan
// docs/plans/2026-10-03-chthonia-windows-macos.md §7d): the Markdown buffer
// being edited, rendered beside it by the one Markdown renderer
// (madcide_markdown.inc) and current as it changes.
//
// The preview follows the active buffer: a Markdown one (its kind,
// madc::fkMARKDOWN, stamped at open — never an extension test here) shows its
// rows; any other says it is not Markdown. preview_follow re-renders when the
// view is shown and the buffer's text differs from the text the rows were
// made from; it runs at the head of every compose (IdeSession::
// compose_ide_tree, the one seat every client's frame passes through), so an
// edit shows in the next frame and an idle frame costs one text compare.
//
// The rows are session state on the bag: "mdpreviewrows" (the renderer's
// {content, line, target?}), the text they came from on "mdpreviewtext", its
// document on "mdpreviewdoc". A choose on a row (mdpreviewrow) moves the
// editor to the row's source line (goto_pane_row, the panes' one owner); on a
// link row it opens the linked file beside the buffer (edit_file, the one
// open-or-switch owner) or says the link (a URL, an anchor).
//
// Thread contract: the session's thread; the rows are the session's.

bool show_view(long w, long es, long kind);		// madcide_core.inc
bool view_shown(long w, long es, long kind);		// madcide_core.inc
bool dismiss_view(long w, long es, long kind);		// madcide_core.inc
void goto_pane_row(long w, long es, long doc, long option, long view);	// madcide_core.inc
void edit_file(long w, long es, long olddoc, const char *path0);	// madcide_core.inc

// Re-render the preview's rows for `doc` when its text changed since the
// last render (or the preview follows another document now).
void preview_follow(long w, long es, long doc)
{
    if ( !view_shown(w, es, viewMDPREVIEW) )
	return;
    if ( doc_kind(w, doc) != madc::fkMARKDOWN )
    {
	if ( es_int(w, es, "mdpreviewdoc", 0) != 0 )
	{
	    var none = {};
	    long zero = 0;
	    ui::set(w, es, "mdpreviewrows", none);
	    ui::set(w, es, "mdpreviewdoc", zero);
	}
	return;
    }
    var t;
    ui::text(t, w, doc);
    var last;
    ui::get(last, w, es, "mdpreviewtext");
    if ( es_int(w, es, "mdpreviewdoc", 0) == doc && !last.is_null() && last == t )
	return;
    var rows = {};
    md_text_rows(rows, t.c_str());		// empty without the madcmark module
    ui::set(w, es, "mdpreviewrows", rows);
    ui::set(w, es, "mdpreviewtext", t);
    ui::set(w, es, "mdpreviewdoc", doc);
}

// View ▸ Markdown Preview (`mdpreview`): shown -> dismissed; else shown,
// rendered now.
void preview_toggle(long w, long es, long doc)
{
    if ( view_shown(w, es, viewMDPREVIEW) )
    {
	dismiss_view(w, es, viewMDPREVIEW);
	return;
    }
    if ( show_view(w, es, viewMDPREVIEW) )
	preview_follow(w, es, doc);
}

// A link's target as a file beside `doc`: a relative path without a scheme
// (`docs/build.md`, `../LICENSE`), its anchor dropped; "" for a URL, a bare
// anchor or a mail address.
void preview_link_path(var &out, long w, long doc, var &url)
{
    out = "";
    var u = format("{}", url);
    if ( strlen(u.c_str()) == 0 || perl::index(u.c_str(), "#") == 0
      || perl::index(u.c_str(), "://") >= 0 || perl::index(u.c_str(), "mailto:") == 0 )
	return;
    long hash = perl::index(u.c_str(), "#");
    if ( hash > 0 )
	u = perl::substr(u, 0, hash);
    var p;
    doc_name(p, w, doc);
    out = format("{}/{}", php::dirname(p.c_str()), u);
}

// A choose on a preview row (1-based `option`): a link row opens its file or
// says its link; any other row moves the editor to its source line.
void preview_choose(long w, long es, long doc, long option)
{
    var rows;
    ui::get(rows, w, es, "mdpreviewrows");
    if ( rows.is_null() || option < 1 || option > php::count(rows) )
	return;
    long pdoc = es_int(w, es, "mdpreviewdoc", 0);
    if ( pdoc == 0 )
	return;
    var target;
    keyed_get(target, rows[option - 1], "target");
    if ( target.is_null() )
    {
	goto_pane_row(w, es, pdoc, option, viewMDPREVIEW);
	return;
    }
    var path;
    preview_link_path(path, w, pdoc, target);
    if ( strlen(path.c_str()) > 0 && php::file_exists(path.c_str()) )
	edit_file(w, es, pdoc, path.c_str());
    else if ( strlen(path.c_str()) > 0 )
	ui::set(w, es, "msg", format("No file {}.", path));
    else
	ui::set(w, es, "msg", format("Link: {}", target));
}

// The preview's node: the rows as a list, every row navigable (its verb
// mdpreviewrow); the label names the buffer it shows.
void preview_view_node(var &out, long w, long es, bool focus)
{
    var items = {};
    long pdoc = es_int(w, es, "mdpreviewdoc", 0);
    var label = "Markdown Preview";
    if ( !markdown::available() )
	items[] = { "role": "item",
		    "content": "(Markdown Preview needs the madcmark module, which this installation does not have)" };
    else if ( pdoc == 0 )
	items[] = { "role": "item", "content": "(the buffer is not Markdown)" };
    else
    {
	var p;
	doc_name(p, w, pdoc);
	label = format("Markdown Preview — {}", php::basename(p.c_str()));
	var rows;
	ui::get(rows, w, es, "mdpreviewrows");
	if ( !rows.is_null() )
	    for ( var r : rows )
	    {
		var it = { "role": "item", "content": r["content"] };
		var gh = { "code": cmdMDPREVIEWROW };
		it["hints"] = gh;
		items[] = it;
	    }
	if ( php::count(items) == 0 )
	    items[] = { "role": "item", "content": "(empty)" };
    }
    var cb = { "role": "choice", "label": label };
    cb["children"] = items;
    var ch = { "list": 1 };
    if ( focus )
	ch["focus"] = 1;
    cb["hints"] = ch;
    out = cb;
}
