// madcide_git.inc — the IDE's git (plan docs/plans/2026-10-03-chthonia-
// windows-macos.md §7e stage 1, §7f item 4): the `git` command — what
// `%git VERB` in the REPL, a key and a menu row reach — is the madcgit
// module's own %git verb (git::command; its verbs and their git oracles are
// src/modules/madcgit/madcgit_verbs.cpp), run in madcide's process over
// madcide's buffers; the views (Changes, History, Blame line) read through
// the same verbs or the module's rows (git::*, read-only). A builtin module:
// builtin_plugins' row "git" (madcide_core.inc), activated by git_activate.
//
// A buffer's repository handle and the status line's %G seat are the
// core's (madcide_repo.inc): the core names no function of a builtin module.
//
// Thread contract: session-thread state, as the code-graph seat's — a
// buffer's repository handle lives on its bag (git_handle_of); a handle a
// verb opens for a file no buffer holds is closed before the verb returns.

// ---- a verb's repository and file ----
// The repository a verb reads: the one above FILE (an open buffer's, kept
// on its bag), else the active buffer's, else the working directory's.
// `owned`: the verb closes it (git_release). 0 = none, with `why`.
long git_repo_for(var &why, long w, long es, var &file, bool &owned)
{
    owned = false;
    if ( !git::available() )
    {
	why = "git is not available (the madcgit module is not installed)";
	return 0;
    }
    bool named = strlen(file.c_str()) > 0;
    long d = named ? buffer_doc_named(w, es, file.c_str()) : active_doc(w, es, 0);
    if ( d != 0 && !doc_untitled(w, d) )
    {
	long kept = git_handle_of(w, es, d);
	if ( kept > 0 )
	    return kept;
    }
    var dir = ".";
    if ( named )
	dir = php::dirname(file.c_str());
    long gh = git::open(dir.c_str());
    if ( gh <= 0 )
    {
	why = "not a git repository (or any of the parent directories)";
	return 0;
    }
    owned = true;
    return gh;
}

void git_release(long gh, bool owned)
{
    if ( owned )
	git::close(gh);
}

// FILE's path in repository `gh`, git's forward-slash spelling relative to
// its working tree. False with `why` when FILE is outside it.
bool git_path_in(var &out, var &why, long gh, var &file)
{
    var rp;
    git::relpath(rp, gh, file.c_str());
    if ( !rp["error"].is_null() )
    {
	why = rp["error"];
	return false;
    }
    out = rp["path"];
    return true;
}

// FILE's text now: its open buffer's live text, else the file's bytes ("" for
// a file that is gone).
void git_live_text(var &out, long w, long es, var &file)
{
    out = "";
    long d = buffer_doc_named(w, es, file.c_str());
    if ( d != 0 )
    {
	ui::text(out, w, d);
	return;
    }
    if ( php::file_exists(file.c_str()) )
	php::file_get_contents(out, file.c_str());
}

// ---- the reads the views share ----
// FILE's commits (every commit's with FILE ""), newest first, at most
// `limit`: git::log's rows {sha, author, email, when, summary}.
bool git_log_rows(var &out, var &why, long w, long es, var &file, long limit)
{
    bool owned = false;
    long gh = git_repo_for(why, w, es, file, owned);
    if ( gh <= 0 )
	return false;
    var rel = "";
    if ( strlen(file.c_str()) > 0 && !git_path_in(rel, why, gh, file) )
    {
	git_release(gh, owned);
	return false;
    }
    var lg;
    git::log(lg, gh, rel.c_str(), limit);
    git_release(gh, owned);
    if ( !lg["error"].is_null() )
    {
	why = lg["error"];
	return false;
    }
    out = lg["rows"];
    return true;
}

// A commit row as git log --format='%h %s (%ar, %an)' spells it.
void git_commit_line(var &out, var &c, long now)
{
    var sha = c["sha"];
    var short_sha = sha.substr(0, 7);
    var rd;
    git::relative_date(rd, c["when"].as_integer(), now);
    out = format("{} {} ({}, {})", short_sha, c["summary"], rd["text"], c["author"]);
}

// FILE's blame rows as it is now (its open buffer's live text): `count`
// lines from `line` (count 0 = to the end), git::blame_text's rows {line,
// count, sha, author, when, summary}; `text` is the text blamed.
bool git_blame_rows(var &out, var &text, var &why, long w, long es, var &file,
		    long line, long count)
{
    bool owned = false;
    long gh = git_repo_for(why, w, es, file, owned);
    if ( gh <= 0 )
	return false;
    var rel;
    if ( !git_path_in(rel, why, gh, file) )
    {
	git_release(gh, owned);
	return false;
    }
    git_live_text(text, w, es, file);
    var bt;
    git::blame_text(bt, gh, rel.c_str(), text.c_str(), line, count);
    git_release(gh, owned);
    if ( !bt["error"].is_null() )
    {
	why = bt["error"];
	return false;
    }
    out = bt["rows"];
    return true;
}

// `VERB ARGS` to the module's own verb's text (git::command — the session's
// %git), FILE and the current buffer read from madcide's buffers (their live
// text, unsaved edits included: madc::host_documents), `doc` the current
// one. False = refused, with `why`.
bool git_command(var &out, var &why, long w, long es, long doc, const char *arg0)
{
    var arg = arg0;			// ring discipline: own the text first
    out = "";
    if ( !git::available() )
    {
	why = "git is not available (the madcgit module is not installed)";
	return false;
    }
    madc::host_documents(w, active_doc(w, es, doc));
    var r;
    git::command(r, arg.c_str());
    madc::host_documents(0, 0);
    if ( !r["error"].is_null() )
    {
	why = r["error"];
	return false;
    }
    out = r["text"];
    return true;
}

// The `git` command (a %git line's, a key's, a menu row's): the verb's text,
// or `%git: WHY`, in the REPL's transcript; a refusal also on the status
// line.
bool git_cmd(long w, long es, long doc, const char *arg)
{
    var out;
    var why;
    if ( !git_command(out, why, w, es, doc, arg) )
    {
	ui::set(w, es, "msg", format("git: {}", why));
	out = format("%git: {}\n", why);
    }
    long len = strlen(out.c_str());
    if ( len > 0 && out.c_str()[len - 1] != '\n' )
	out = format("{}\n", out);
    repl_feed(w, es, out.c_str());
    return true;
}

// ---- the views (plan §7e stage 1: the editor shows what exists) ----
// The file a view reads: the active buffer's, refused (false, said on the
// status line) when it is a pseudo-buffer or untitled — no file, no history.
bool git_view_file(var &out, long w, long es, long doc)
{
    long ad = active_doc(w, es, doc);
    doc_name(out, w, ad);
    if ( doc_untitled(w, ad) || !path_is_asset(out) )
    {
	ui::set(w, es, "msg", format("Not a file: {}", out));
	return false;
    }
    buffer_path(out, w, ad);
    return true;
}

// The read-only pseudo-buffer NAME holding `text`, found or created, and
// shown (the editor switches to it, as edit_file switches): [changes], or a
// revision's "[FILE @ SHA7]".
void git_show_buffer(long w, long es, long from, var &name, var &text)
{
    ensure_buffer_table(w, es, from);
    long d = buffer_doc_named(w, es, name.c_str());
    if ( d == 0 )
    {
	d = new_view_text(w, name.c_str());
	push_buffer_row(w, es, d, name.c_str());
    }
    ui::text_load(w, d, text.c_str());
    ui::set(w, d, "read_only", true);
    long idx = buffer_index_of(w, es, d);
    if ( idx >= 0 && idx != es_int(w, es, "bufidx", 0) )
	switch_buffer(w, es, from, idx);
    ui::set(w, es, "caret", 0);
    derive_access(w, es);
}

// `gitchanges` (View ▸ Changes): the active buffer against its last commit
// — %git diff FILE's text (git diff HEAD -- FILE over the live text, unsaved
// edits included) — in the read-only [changes] buffer; no change is said on
// the status line.
bool git_changes_cmd(long w, long es, long doc, const char *arg)
{
    var file;
    if ( !git_view_file(file, w, es, doc) )
	return true;
    var verb = format("diff {}", python::shlex_quote(file.c_str()));
    var out;
    var why;
    if ( !git_command(out, why, w, es, doc, verb.c_str()) )
    {
	ui::set(w, es, "msg", format("git: {}", why));
	return true;
    }
    if ( strlen(out.c_str()) == 0 )
    {
	ui::set(w, es, "msg", format("No changes in {} since the last commit.", php::basename(file.c_str())));
	return true;
    }
    var name = "[changes]";
    git_show_buffer(w, es, active_doc(w, es, doc), name, out);
    return true;
}

// `githistory` (View ▸ History): with no argument the active file's commits
// (%git log FILE's rows, newest first) open as a choice list; a row's
// argument, REV:FILE, shows FILE as that commit holds it (%git show
// REV:FILE's text) in a read-only "[NAME @ SHA7]" buffer. A REV alone (the
// seat's, -c's) is the active file's.
bool git_history_cmd(long w, long es, long doc, const char *arg0)
{
    var arg = arg0;			// ring discipline: own the text first
    long code = cmd_of_w(w, "githistory");
    if ( strlen(arg.c_str()) > 0 && perl::index(arg.c_str(), ":") < 0 )
    {
	var active;
	if ( !git_view_file(active, w, es, doc) )
	    return true;
	arg = format("{}:{}", arg, active);
    }
    if ( strlen(arg.c_str()) > 0 )
    {
	var verb = format("show {}", python::shlex_quote(arg.c_str()));
	var out;
	var why;
	if ( !git_command(out, why, w, es, doc, verb.c_str()) )
	{
	    ui::set(w, es, "msg", format("git: {}", why));
	    return true;
	}
	long colon = perl::index(arg.c_str(), ":");
	var rev = arg.substr(0, colon);
	var file = arg.substr(colon + 1, strlen(arg.c_str()) - colon - 1);
	var short_rev = rev.substr(0, 7);
	var name = format("[{} @ {}]", php::basename(file.c_str()), short_rev);
	git_show_buffer(w, es, active_doc(w, es, doc), name, out);
	return true;
    }
    var file;
    if ( !git_view_file(file, w, es, doc) )
	return true;
    var rows;
    var why;
    if ( !git_log_rows(rows, why, w, es, file, 100) )
    {
	ui::set(w, es, "msg", format("git: {}", why));
	return true;
    }
    if ( php::count(rows) == 0 )
    {
	ui::set(w, es, "msg", format("{} has no commits.", php::basename(file.c_str())));
	return true;
    }
    long now = php::time();
    var crows = {};
    for ( var c : rows )
    {
	var title;
	git_commit_line(title, c, now);
	var rowarg = format("{}:{}", c["sha"], file);
	crows[] = { "title": title, "code": code, "arg": rowarg, "current": 0 };
    }
    var label = format("History of {}", php::basename(file.c_str()));
    choice_show(w, es, label.c_str(), crows);
    return true;
}

// `gitblame` (blame for the caret's line): who last changed the caret's
// line, on the status line — `SHA7 AUTHOR, DATE: SUMMARY` (git blame -L
// N,N's commit, the date relative) — or that no commit holds it yet.
bool git_blame_cmd(long w, long es, long doc, const char *arg)
{
    var file;
    if ( !git_view_file(file, w, es, doc) )
	return true;
    long ad = active_doc(w, es, doc);
    long line = line_of(w, ad, es_int(w, es, "caret", 0));
    var rows;
    var text;
    var why;
    if ( !git_blame_rows(rows, text, why, w, es, file, line, 1) )
    {
	ui::set(w, es, "msg", format("git: {}", why));
	return true;
    }
    if ( php::count(rows) == 0 )
    {
	ui::set(w, es, "msg", format("Line {}: nothing to blame.", line));
	return true;
    }
    var h = rows[0];
    var sha = h["sha"];
    if ( strlen(sha.c_str()) == 0 )
    {
	ui::set(w, es, "msg", format("Line {}: not committed yet.", line));
	return true;
    }
    var short_sha = sha.substr(0, 7);
    var rd;
    git::relative_date(rd, h["when"].as_integer(), php::time());
    ui::set(w, es, "msg", format("Line {}: {} {}, {}: {}", line, short_sha, h["author"],
				 rd["text"], h["summary"]));
    return true;
}

// The activation (builtin_plugins' row): the `git` command and its REPL
// command, and the views' commands, through the plugin points. False = a
// registration refused.
bool git_activate(long w)
{
    long code = plugin_command(w, "git", "Git", git_cmd);
    if ( code == 0 )
	return false;
    var row;
    git::command_row(row);		// the module's own %git row's words
    return plugin_repl_command(w, "git", row["usage"].c_str(), row["summary"].c_str(), code)
	&& plugin_command(w, "gitchanges", "Changes", git_changes_cmd) != 0
	&& plugin_command(w, "githistory", "History", git_history_cmd) != 0
	&& plugin_command(w, "gitblame", "Blame line", git_blame_cmd) != 0;
}
