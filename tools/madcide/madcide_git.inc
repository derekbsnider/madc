// madcide_git.inc — the IDE's git (plan docs/plans/2026-10-03-chthonia-
// windows-macos.md §7e stage 1, §7f item 4): the read verbs over the madcgit
// module (git::*, read-only), each ONE implementation that `%git VERB` in the
// REPL, a key and a menu row reach through the `git` command. Each prints
// what git prints for the same question (the git command named at each
// verb is its oracle). A builtin module: builtin_plugins' row "git"
// (madcide_core.inc), activated by git_activate.
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

// ---- the spellings git prints ----
void git_ago(var &out, long n, const char *unit)
{
    out = format("{} {}{} ago", n, unit, n == 1 ? "" : "s");
}

// A time as git's --date=relative spells it, `now` the clock (git's date.c,
// show_date_relative: the same thresholds and roundings).
void git_relative_date(var &out, long when, long now)
{
    if ( now < when )
    {
	out = "in the future";
	return;
    }
    long diff = now - when;
    if ( diff < 90 )
    {
	git_ago(out, diff, "second");
	return;
    }
    diff = (diff + 30) / 60;			// minutes
    if ( diff < 90 )
    {
	git_ago(out, diff, "minute");
	return;
    }
    diff = (diff + 30) / 60;			// hours
    if ( diff < 36 )
    {
	git_ago(out, diff, "hour");
	return;
    }
    diff = (diff + 12) / 24;			// days
    if ( diff < 14 )
    {
	git_ago(out, diff, "day");
	return;
    }
    if ( diff < 70 )
    {
	git_ago(out, (diff + 3) / 7, "week");
	return;
    }
    if ( diff < 365 )
    {
	git_ago(out, (diff + 15) / 30, "month");
	return;
    }
    if ( diff < 1825 )
    {
	long totalmonths = (diff * 12 * 2 + 365) / (365 * 2);
	long years = totalmonths / 12;
	long months = totalmonths % 12;
	if ( months == 0 )
	{
	    git_ago(out, years, "year");
	    return;
	}
	out = format("{} year{}, {} month{} ago", years, years == 1 ? "" : "s",
		     months, months == 1 ? "" : "s");
	return;
    }
    git_ago(out, (diff + 183) / 365, "year");
}

// A side's state as git status's short format spells it (wt-status.c).
const char *git_state_letter(var &s)
{
    if ( s == git::file_state::added )
	return "A";
    if ( s == git::file_state::modified )
	return "M";
    if ( s == git::file_state::deleted )
	return "D";
    if ( s == git::file_state::renamed )
	return "R";
    if ( s == git::file_state::typechange )
	return "T";
    if ( s == git::file_state::untracked )
	return "?";
    if ( s == git::file_state::ignored )
	return "!";
    if ( s == git::file_state::conflicted )
	return "U";
    return " ";
}

// ---- the reads the verbs and the views share ----
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
    git_relative_date(rd, c["when"].as_integer(), now);
    out = format("{} {} ({}, {})", short_sha, c["summary"], rd, c["author"]);
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

// ---- the verbs ----
// `%git log [-n N] [FILE]`: git log --format='%h %s (%ar, %an)' -N [-- FILE],
// newest first; N defaults to 20; %h is the sha's first 7 characters.
bool git_log_text(var &out, var &why, long w, long es, var &words)
{
    long limit = 20;
    var file = "";
    long n = php::count(words);
    for ( long i = 1; i < n; i = i + 1 )
    {
	var a = words[i];
	if ( a == "-n" )
	{
	    var count = "";
	    if ( i + 1 < n )
		count = words[i + 1];
	    limit = php::ctype_digit(count) ? php::intval(count.c_str()) : 0;
	    if ( limit <= 0 )
	    {
		why = "log -n takes a count of commits";
		return false;
	    }
	    i = i + 1;
	    continue;
	}
	if ( strlen(file.c_str()) > 0 )
	{
	    why = "log takes one FILE";
	    return false;
	}
	file = a;
    }
    var rows;
    if ( !git_log_rows(rows, why, w, es, file, limit) )
	return false;
    long now = php::time();
    out = "";
    for ( var c : rows )
    {
	var line;
	git_commit_line(line, c, now);
	out = format("{}{}\n", out, line);
    }
    return true;
}

// `%git show REV:FILE`: FILE as REV holds it (git show REV:FILE). `%git
// show REV`: the commit's header — git show --no-patch --format='commit
// %H%nAuthor: %an <%ae>%nDate:   %ar%n%n    %s' REV.
bool git_show_text(var &out, var &why, long w, long es, var &words)
{
    if ( php::count(words) != 2 )
    {
	why = "show takes REV or REV:FILE";
	return false;
    }
    var spec = words[1];
    var rev = spec;
    var file = "";
    long colon = perl::index(spec.c_str(), ":");
    if ( colon >= 0 )
    {
	rev = spec.substr(0, colon);
	file = spec.substr(colon + 1, strlen(spec.c_str()) - colon - 1);
	if ( strlen(rev.c_str()) == 0 || strlen(file.c_str()) == 0 )
	{
	    why = "show takes REV or REV:FILE";
	    return false;
	}
    }
    bool owned = false;
    long gh = git_repo_for(why, w, es, file, owned);
    if ( gh <= 0 )
	return false;
    var answer;
    if ( strlen(file.c_str()) > 0 )
    {
	var rel;
	if ( !git_path_in(rel, why, gh, file) )
	{
	    git_release(gh, owned);
	    return false;
	}
	git::show(answer, gh, rev.c_str(), rel.c_str());
	out = answer["text"];
    }
    else
    {
	git::commit(answer, gh, rev.c_str());
	var rd;
	git_relative_date(rd, answer["when"].as_integer(), php::time());
	out = format("commit {}\nAuthor: {} <{}>\nDate:   {}\n\n    {}\n", answer["sha"],
		     answer["author"], answer["email"], rd, answer["summary"]);
    }
    git_release(gh, owned);
    if ( !answer["error"].is_null() )
    {
	why = answer["error"];
	return false;
    }
    return true;
}

// `%git blame FILE[:LINE]`: git blame --date=relative's fields for each line
// of FILE as it is now (its open buffer's live text: git blame --contents),
// or for LINE alone — `SHA8 (AUTHOR DATE LINE) TEXT`; a line no commit
// holds is git's `00000000 (Not Committed Yet LINE) TEXT`.
bool git_blame_text(var &out, var &why, long w, long es, var &words)
{
    if ( php::count(words) != 2 )
    {
	why = "blame takes FILE or FILE:LINE";
	return false;
    }
    var spec = words[1];
    var file = spec;
    long line = 0;
    long colon = perl::rindex(spec.c_str(), ":");
    if ( colon > 0 )
    {
	var tail = spec.substr(colon + 1, strlen(spec.c_str()) - colon - 1);
	if ( php::ctype_digit(tail) )
	{
	    line = php::intval(tail.c_str());
	    file = spec.substr(0, colon);
	    if ( line < 1 )
	    {
		why = "blame's LINE counts from 1";
		return false;
	    }
	}
    }
    var text;
    var rows;
    if ( !git_blame_rows(rows, text, why, w, es, file, line > 0 ? line : 1, line > 0 ? 1 : 0) )
	return false;
    var lines;
    php::explode(lines, "\n", text.c_str());
    long nlines = php::count(lines);
    long now = php::time();
    out = "";
    for ( var h : rows )
    {
	var sha = h["sha"];
	var who;
	if ( strlen(sha.c_str()) == 0 )
	    who = "00000000 (Not Committed Yet";
	else
	{
	    var short_sha = sha.substr(0, 8);
	    var rd;
	    git_relative_date(rd, h["when"].as_integer(), now);
	    who = format("{} ({} {}", short_sha, h["author"], rd);
	}
	long first = h["line"].as_integer();
	long count = h["count"].as_integer();
	for ( long ln = first; ln < first + count; ln = ln + 1 )
	{
	    var src = "";
	    if ( ln - 1 < nlines )
		src = lines[ln - 1];
	    out = format("{}{} {}) {}\n", out, who, ln, src);
	}
    }
    return true;
}

// `%git status`: git status -sb's text — `## BRANCH` (`## HEAD (no branch)`
// when detached, `## No commits yet` before the first commit), then each
// changed file as `XY PATH` (a rename `XY FROM -> PATH`; X the index's
// state, Y the working tree's), the untracked ones (`?? PATH`) last.
bool git_status_text(var &out, var &why, long w, long es, var &words)
{
    if ( php::count(words) != 1 )
    {
	why = "status takes no arguments";
	return false;
    }
    var none = "";
    bool owned = false;
    long gh = git_repo_for(why, w, es, none, owned);
    if ( gh <= 0 )
	return false;
    var hd;
    git::head(hd, gh);
    var st;
    git::status(st, gh);
    git_release(gh, owned);
    if ( !st["error"].is_null() )
    {
	why = st["error"];
	return false;
    }
    if ( !hd["error"].is_null() )
	out = "## No commits yet\n";
    else if ( hd["detached"].as_boolean() )
	out = "## HEAD (no branch)\n";
    else
	out = format("## {}\n", hd["branch"]);
    for ( long pass = 0; pass < 2; pass = pass + 1 )
	for ( var r : st["rows"] )
	{
	    var x = r["index"];
	    var y = r["worktree"];
	    bool untracked = y == git::file_state::untracked;
	    if ( untracked != (pass == 1) )
		continue;
	    var path = r["path"];
	    var from = r["from"];
	    if ( untracked )
		out = format("{}?? {}\n", out, path);
	    else if ( strlen(from.c_str()) > 0 )
		out = format("{}{}{} {} -> {}\n", out, git_state_letter(x),
			     git_state_letter(y), from, path);
	    else
		out = format("{}{}{} {}\n", out, git_state_letter(x),
			     git_state_letter(y), path);
	}
    return true;
}

// Does a status row hold a change git diff HEAD shows (the index or the
// working tree differs; untracked and ignored files are not diffed)?
bool git_row_diffed(var &r)
{
    var x = r["index"];
    var y = r["worktree"];
    if ( y == git::file_state::untracked || y == git::file_state::ignored )
	return false;
    return !(x == git::file_state::unmodified) || !(y == git::file_state::unmodified);
}

// `%git diff [FILE]`: FILE as it is now (its open buffer's live text, else
// the file) against its last commit — git diff HEAD -- FILE, a buffer's
// unsaved edits included; with no FILE, each file git status names as
// changed, from disk (git diff HEAD). Nothing changed prints nothing.
bool git_diff_text(var &out, var &why, long w, long es, var &words)
{
    long n = php::count(words);
    if ( n > 2 )
    {
	why = "diff takes at most one FILE";
	return false;
    }
    var file = "";
    if ( n == 2 )
	file = words[1];
    bool owned = false;
    long gh = git_repo_for(why, w, es, file, owned);
    if ( gh <= 0 )
	return false;
    out = "";
    if ( n == 2 )
    {
	var rel;
	if ( !git_path_in(rel, why, gh, file) )
	{
	    git_release(gh, owned);
	    return false;
	}
	var text;
	git_live_text(text, w, es, file);
	var df;
	git::diff(df, gh, "HEAD", rel.c_str(), text.c_str());
	git_release(gh, owned);
	if ( !df["error"].is_null() )
	{
	    why = df["error"];
	    return false;
	}
	out = df["patch"];
	return true;
    }
    var st;
    git::status(st, gh);
    if ( !st["error"].is_null() )
    {
	git_release(gh, owned);
	why = st["error"];
	return false;
    }
    var root = st["workdir"];
    for ( var r : st["rows"] )
    {
	if ( !git_row_diffed(r) )
	    continue;
	var path = r["path"];
	var disk = format("{}{}", root, path);
	var text = "";
	if ( php::file_exists(disk.c_str()) )
	    php::file_get_contents(text, disk.c_str());
	var df;
	git::diff(df, gh, "HEAD", path.c_str(), text.c_str());
	if ( !df["error"].is_null() )
	{
	    git_release(gh, owned);
	    why = df["error"];
	    return false;
	}
	out = format("{}{}", out, df["patch"]);
    }
    git_release(gh, owned);
    return true;
}

// The verbs' usage, one line: %help's row and an unknown verb's refusal.
void git_usage(var &out)
{
    var t;
    git_verb_table(t);
    out = "";
    for ( var r : t )
    {
	if ( strlen(out.c_str()) == 0 )
	    out = r["usage"];
	else
	    out = format("{} | {}", out, r["usage"]);
    }
}

// `VERB ARGS` (the shell's word rules) to the verb's text. False = refused,
// with `why`.
bool git_command(var &out, var &why, long w, long es, const char *arg0)
{
    var arg = arg0;			// ring discipline: own the text first
    var words;
    out = "";
    if ( !python::shlex_split(words, arg.c_str()) )
    {
	why = "a quote is not closed";
	return false;
    }
    var usage;
    git_usage(usage);
    if ( php::count(words) == 0 )
    {
	why = format("which verb? {}", usage);
	return false;
    }
    var verb = words[0];
    switch ( git_verb_of(verb) )
    {
	case gtLOG:
	    return git_log_text(out, why, w, es, words);
	case gtSHOW:
	    return git_show_text(out, why, w, es, words);
	case gtBLAME:
	    return git_blame_text(out, why, w, es, words);
	case gtSTATUS:
	    return git_status_text(out, why, w, es, words);
	case gtDIFF:
	    return git_diff_text(out, why, w, es, words);
    }
    why = format("unknown verb '{}': {}", verb, usage);
    return false;
}

// The `git` command (a %git line's, a key's, a menu row's): the verb's text,
// or `%git: WHY`, in the REPL's transcript; a refusal also on the status
// line.
bool git_cmd(long w, long es, long doc, const char *arg)
{
    var out;
    var why;
    if ( !git_command(out, why, w, es, arg) )
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
    ui::get(out, w, ad, "path");
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
	d = new_document(w, name.c_str(), "");
	push_buffer_row(w, es, d, name.c_str());
    }
    ui::text_load(w, d, text.c_str());
    ui::set(w, d, "modified", false);
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
    var words = { "diff" };
    words[] = file;
    var out;
    var why;
    if ( !git_diff_text(out, why, w, es, words) )
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
	var words = { "show" };
	words[] = arg;
	var out;
	var why;
	if ( !git_show_text(out, why, w, es, words) )
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
    git_relative_date(rd, h["when"].as_integer(), php::time());
    ui::set(w, es, "msg", format("Line {}: {} {}, {}: {}", line, short_sha, h["author"], rd,
				 h["summary"]));
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
    var usage;
    git_usage(usage);
    var summary = format("the repository's {}", usage);
    return plugin_repl_command(w, "git", "%git VERB", summary.c_str(), code)
	&& plugin_command(w, "gitchanges", "Changes", git_changes_cmd) != 0
	&& plugin_command(w, "githistory", "History", git_history_cmd) != 0
	&& plugin_command(w, "gitblame", "Blame line", git_blame_cmd) != 0;
}
