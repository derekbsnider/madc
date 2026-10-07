// madcide_repo.inc — a buffer's git repository (plan docs/plans/2026-10-03-
// chthonia-windows-macos.md §7e stage 1): its handle, kept on the buffer's
// bag, and the status line's %G seat (the branch, `*` when the buffer
// differs from its last commit). The core's, above the builtin modules: the
// git module (madcide_git.inc) and the code-graph seat read it, and
// status_expand names git_seat.
//
// Thread contract: session-thread state — a handle and the seat's answer
// live on the buffer's bag; the madcgit module's handles are used only from
// the thread that opened them.

// The doc's git handle (0 = no repository above it) and its working-tree-
// relative path, opened once and kept on the doc bag ("ghandle", "grelpath").
// Read by the git module (madcide_git.inc), the status seat below and the
// code-graph seat (madcide_past.inc, madcide_layers.inc).
long git_handle_of(long w, long es, long adoc)
{
    long gh = es_int(w, adoc, "ghandle", -1);
    if ( gh >= 0 )
	return gh;
    if ( doc_untitled(w, adoc) )
	return 0;		// no file, no repository (not cached: Save As names it)
    var p;
    ui::get(p, w, adoc, "path");
    var dir = php::dirname(p);		// ring: own the text first
    // The madcgit module is LAZY (libgit2 is the system's, a dependency of
    // this nexus — never part of madc): without it there is no repository to
    // ask, the same degradation as a path outside any repository (handle 0,
    // the versioned layer absent).
    gh = git::available() ? git::open(dir.c_str()) : 0;
    ui::set(w, adoc, "ghandle", gh);
    if ( gh > 0 )
    {
	var rp;
	git::relpath(rp, gh, p.c_str());
	var rel = rp["path"];
	if ( rel.is_null() )
	    rel = "";
	ui::set(w, adoc, "grelpath", rel);
    }
    return gh;
}

void git_relpath_of(var &out, long w, long adoc)
{
    ui::get(out, w, adoc, "grelpath");
    if ( out.is_null() )
	out = "";
}

// The %G status seat: the buffer's branch (its commit's short sha when
// detached) and `*` when the buffer differs from its last commit — unsaved
// edits, or the file changed on disk; "" outside a repository. The
// repository's answer is kept on the buffer's bag for two seconds: a seat
// composes with every key, the buffer's own edits count at once.
void git_seat(var &out, long w, long es, long doc)
{
    out = "";
    if ( doc == 0 || doc_untitled(w, doc) || !git::available() )
	return;
    long now = php::time();
    long at = es_int(w, doc, "gseatat", -1);
    if ( at < 0 || now - at >= 2 )
    {
	var branch = "";
	bool dirty = false;
	long gh = git_handle_of(w, es, doc);
	if ( gh > 0 )
	{
	    var hd;
	    git::head(hd, gh);
	    if ( hd["error"].is_null() )
	    {
		var sha = hd["sha"];
		branch = hd["branch"];
		if ( hd["detached"].as_boolean() )
		    branch = sha.substr(0, 7);
	    }
	    var rel;
	    git_relpath_of(rel, w, doc);
	    var dt;
	    git::dirty(dt, gh, rel.c_str());
	    dirty = dt["error"].is_null() && dt["dirty"].as_boolean();
	}
	ui::set(w, doc, "gbranch", branch);
	ui::set(w, doc, "gdirty", dirty);
	ui::set(w, doc, "gseatat", now);
    }
    var branch;
    ui::get(branch, w, doc, "gbranch");
    if ( branch.is_null() || strlen(branch.c_str()) == 0 )
	return;
    bool differs = es_flag(w, doc, "gdirty") || doc_modified(w, doc);
    out = format("{}{}", branch, differs ? "*" : "");
}
