// modules/git/git_verbs.cpp — `%git` (split arc 1B.10; design
// docs/plans/2026-10-10-git-module-command-design.md): the read verbs over
// GitRepo — log, show, blame, status, diff — each printing what git prints
// for the same question (the git command named at each verb is its oracle),
// and the module's command row (madc_module_commands.h), which a session
// registers when its Program imports the module. FILE's live text and the
// current buffer are the host's: the session's buffer question
// (madc::session_buffer).
//
// Thread contract: a verb runs on its session's thread; its repository is
// opened and closed inside it.

#include "git_verbs.h"
#include "git_repo.h"
#include "madc_module_commands.h"
#include "madc_session.h"		// session_buffer: the host's buffers
#include "madc_posix_io.h"		// host_path_dirname
#include "ns_common.h"			// shell_words: a verb's words

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

namespace madc {

namespace {

enum class git_verb : unsigned char { none, log, show, blame, status, diff };

// The one list of the verbs: their names, codes and usages (%git's refusals
// and its usage line). A word converts ONCE, here.
struct git_verb_row
{
    const char *name;
    git_verb code;
    const char *usage;
};

const git_verb_row git_verbs[] = {
    { "log", git_verb::log, "log [-n N] [FILE]" },
    { "show", git_verb::show, "show REV[:FILE]" },
    { "blame", git_verb::blame, "blame FILE[:LINE]" },
    { "status", git_verb::status, "status" },
    { "diff", git_verb::diff, "diff [FILE]" },
};

git_verb git_verb_of(const std::string &word)
{
    for ( const git_verb_row &r : git_verbs )
	if ( word == r.name )
	    return r.code;
    return git_verb::none;
}

std::string git_usage()
{
    std::string out;
    for ( const git_verb_row &r : git_verbs )
	out += (out.empty() ? "" : " | ") + std::string(r.usage);
    return out;
}

bool all_digits(const std::string &s)
{
    if ( s.empty() )
	return false;
    for ( char c : s )
	if ( !isdigit((unsigned char)c) )
	    return false;
    return true;
}

// ---- the spellings git prints ----
std::string ago(int64_t n, const char *unit)
{
    return std::to_string(n) + " " + unit + (n == 1 ? "" : "s") + " ago";
}

} // namespace

std::string git_relative_date(int64_t when, int64_t now)
{
    if ( now < when )
	return "in the future";
    int64_t diff = now - when;
    if ( diff < 90 )
	return ago(diff, "second");
    diff = (diff + 30) / 60;			// minutes
    if ( diff < 90 )
	return ago(diff, "minute");
    diff = (diff + 30) / 60;			// hours
    if ( diff < 36 )
	return ago(diff, "hour");
    diff = (diff + 12) / 24;			// days
    if ( diff < 14 )
	return ago(diff, "day");
    if ( diff < 70 )
	return ago((diff + 3) / 7, "week");
    if ( diff < 365 )
	return ago((diff + 15) / 30, "month");
    if ( diff < 1825 )
    {
	const int64_t totalmonths = (diff * 12 * 2 + 365) / (365 * 2);
	const int64_t years = totalmonths / 12;
	const int64_t months = totalmonths % 12;
	if ( months == 0 )
	    return ago(years, "year");
	return std::to_string(years) + " year" + (years == 1 ? "" : "s") + ", "
	     + std::to_string(months) + " month" + (months == 1 ? "" : "s") + " ago";
    }
    return ago((diff + 183) / 365, "year");
}

namespace {

// A side's state as git status's short format spells it (wt-status.c).
const char *state_letter(git::file_state s)
{
    switch ( s )
    {
	case git::file_state::added:      return "A";
	case git::file_state::modified:   return "M";
	case git::file_state::deleted:    return "D";
	case git::file_state::renamed:    return "R";
	case git::file_state::typechange: return "T";
	case git::file_state::untracked:  return "?";
	case git::file_state::ignored:    return "!";
	case git::file_state::conflicted: return "U";
	case git::file_state::unmodified: break;
    }
    return " ";
}

// A commit row as git log --format='%h %s (%ar, %an)' spells it.
std::string commit_line(const GitCommit &c, int64_t now)
{
    return c.sha.substr(0, 7) + " " + c.summary + " (" + git_relative_date(c.when, now)
	 + ", " + c.author + ")";
}

// ---- a verb's repository and file ----
// FILE ("" none) through the host's buffers: the path it names, and its
// text now — a buffer's live text, else the file's bytes ("" for a file that
// is gone). `why` when there is nothing to read.
struct verb_file
{
    std::string path;
    std::string text;
    bool has_text;
    verb_file() : has_text(false) {}
};

bool read_file_bytes(const std::string &path, std::string &out)
{
    out.clear();
    FILE *f = fopen(path.c_str(), "rb");
    if ( !f )
	return false;
    char chunk[8192];
    size_t got;
    while ( (got = fread(chunk, 1, sizeof chunk, f)) > 0 )
	out.append(chunk, got);
    fclose(f);
    return true;
}

// The repository a verb reads: the one above FILE; with no FILE, the one
// above the host's current buffer; else the working directory's. FILE, when
// named, comes back resolved through the host's buffers.
bool open_repo(GitRepo &repo, const std::string &file, verb_file &f, std::string &why)
{
    std::string dir;
    std::string bwhy;
    if ( !file.empty() )
    {
	if ( !session_buffer(file, f.path, f.text, f.has_text, bwhy) )
	{
	    why = bwhy;
	    return false;
	}
	dir = detail::host_path_dirname(f.path);
    }
    else
    {
	verb_file current;
	if ( session_buffer("", current.path, current.text, current.has_text, bwhy) )
	    dir = detail::host_path_dirname(current.path);
    }
    if ( dir.empty() )
	dir = ".";
    if ( !repo.open(dir) )
    {
	why = "not a git repository (or any of the parent directories)";
	return false;
    }
    return true;
}

// FILE's text now: the host's buffer's, else the disk's.
const std::string &live_text(verb_file &f)
{
    if ( !f.has_text )
	read_file_bytes(f.path, f.text);
    return f.text;
}

bool refuse(std::string &why, const error &err)
{
    why = err.message;
    return false;
}

// ---- the verbs ----
// `log [-n N] [FILE]`: git log --format='%h %s (%ar, %an)' -N [-- FILE],
// newest first; N defaults to 20.
bool verb_log(const std::vector<std::string> &w, std::string &out, std::string &why)
{
    size_t limit = 20;
    std::string file;
    for ( size_t i = 1; i < w.size(); ++i )
    {
	if ( w[i] == "-n" )
	{
	    const std::string count = i + 1 < w.size() ? w[i + 1] : std::string();
	    const long n = all_digits(count) ? atol(count.c_str()) : 0;
	    if ( n <= 0 )
	    {
		why = "log -n takes a count of commits";
		return false;
	    }
	    limit = (size_t)n;
	    ++i;
	    continue;
	}
	if ( !file.empty() )
	{
	    why = "log takes one FILE";
	    return false;
	}
	file = w[i];
    }
    GitRepo repo;
    verb_file f;
    if ( !open_repo(repo, file, f, why) )
	return false;
    std::string rel;
    error err;
    if ( !file.empty() && !repo.relpath(f.path, rel, &err) )
	return refuse(why, err);
    std::vector<GitCommit> rows;
    if ( !repo.log(rows, rel, limit, &err) )
	return refuse(why, err);
    const int64_t now = (int64_t)time(0);
    for ( const GitCommit &c : rows )
	out += commit_line(c, now) + "\n";
    return true;
}

// `show REV:FILE`: FILE as REV holds it (git show REV:FILE). `show REV`: the
// commit's header — git show --no-patch --format='commit %H%nAuthor: %an
// <%ae>%nDate:   %ar%n%n    %s' REV.
bool verb_show(const std::vector<std::string> &w, std::string &out, std::string &why)
{
    if ( w.size() != 2 )
    {
	why = "show takes REV or REV:FILE";
	return false;
    }
    const std::string &spec = w[1];
    std::string rev = spec, file;
    const size_t colon = spec.find(':');
    if ( colon != std::string::npos )
    {
	rev = spec.substr(0, colon);
	file = spec.substr(colon + 1);
	if ( rev.empty() || file.empty() )
	{
	    why = "show takes REV or REV:FILE";
	    return false;
	}
    }
    GitRepo repo;
    verb_file f;
    if ( !open_repo(repo, file, f, why) )
	return false;
    error err;
    if ( !file.empty() )
    {
	std::string rel;
	if ( !repo.relpath(f.path, rel, &err) || !repo.show(rev, rel, out, &err) )
	    return refuse(why, err);
	return true;
    }
    GitCommit c;
    if ( !repo.commit(rev, c, &err) )
	return refuse(why, err);
    out = "commit " + c.sha + "\nAuthor: " + c.author + " <" + c.email + ">\nDate:   "
	+ git_relative_date(c.when, (int64_t)time(0)) + "\n\n    " + c.summary + "\n";
    return true;
}

// `blame FILE[:LINE]`: git blame --date=relative's fields for each line of
// FILE as it is now (the host's buffer's live text: git blame --contents),
// or for LINE alone — `SHA8 (AUTHOR DATE LINE) TEXT`; a line no commit holds
// is git's `00000000 (Not Committed Yet LINE) TEXT`.
bool verb_blame(const std::vector<std::string> &w, std::string &out, std::string &why)
{
    if ( w.size() != 2 )
    {
	why = "blame takes FILE or FILE:LINE";
	return false;
    }
    const std::string &spec = w[1];
    std::string file = spec;
    long line = 0;
    const size_t colon = spec.rfind(':');
    if ( colon != std::string::npos && colon > 0 && all_digits(spec.substr(colon + 1)) )
    {
	line = atol(spec.substr(colon + 1).c_str());
	file = spec.substr(0, colon);
	if ( line < 1 )
	{
	    why = "blame's LINE counts from 1";
	    return false;
	}
    }
    GitRepo repo;
    verb_file f;
    if ( !open_repo(repo, file, f, why) )
	return false;
    std::string rel;
    error err;
    if ( !repo.relpath(f.path, rel, &err) )
	return refuse(why, err);
    const std::string &text = live_text(f);
    std::vector<GitBlameRow> rows;
    if ( !repo.blame_buffer(rows, rel, text, line > 0 ? (size_t)line : 1,
			    line > 0 ? 1 : 0, &err) )
	return refuse(why, err);
    std::vector<std::string> lines;
    for ( size_t b = 0; ; )
    {
	const size_t nl = text.find('\n', b);
	lines.push_back(text.substr(b, nl == std::string::npos ? std::string::npos : nl - b));
	if ( nl == std::string::npos )
	    break;
	b = nl + 1;
    }
    const int64_t now = (int64_t)time(0);
    for ( const GitBlameRow &h : rows )
    {
	const std::string who = h.sha.empty()
	    ? std::string("00000000 (Not Committed Yet")
	    : h.sha.substr(0, 8) + " (" + h.author + " " + git_relative_date(h.when, now);
	for ( int64_t ln = h.line; ln < h.line + h.count; ++ln )
	    out += who + " " + std::to_string(ln) + ") "
		 + (ln - 1 < (int64_t)lines.size() ? lines[ln - 1] : std::string()) + "\n";
    }
    return true;
}

// `status`: git status -sb's text — `## BRANCH` (`## HEAD (no branch)` when
// detached, `## No commits yet` before the first commit), then each changed
// file as `XY PATH` (a rename `XY FROM -> PATH`; X the index's state, Y the
// working tree's), the untracked ones (`?? PATH`) last.
bool verb_status(const std::vector<std::string> &w, std::string &out, std::string &why)
{
    if ( w.size() != 1 )
    {
	why = "status takes no arguments";
	return false;
    }
    GitRepo repo;
    verb_file none;
    if ( !open_repo(repo, std::string(), none, why) )
	return false;
    GitRef hd;
    error herr, err;
    const bool head = repo.head(hd, &herr);
    std::vector<GitStatusRow> rows;
    if ( !repo.status(rows, &err) )
	return refuse(why, err);
    out = !head ? std::string("## No commits yet\n")
	: hd.detached ? std::string("## HEAD (no branch)\n")
	: "## " + hd.branch + "\n";
    for ( int pass = 0; pass < 2; ++pass )
	for ( const GitStatusRow &r : rows )
	{
	    const bool untracked = r.worktree == git::file_state::untracked;
	    if ( untracked != (pass == 1) )
		continue;
	    if ( untracked )
		out += "?? " + r.path + "\n";
	    else
		out += std::string(state_letter(r.index)) + state_letter(r.worktree) + " "
		     + (r.from.empty() ? std::string() : r.from + " -> ") + r.path + "\n";
	}
    return true;
}

// Does a status row hold a change git diff HEAD shows (the index or the
// working tree differs; untracked and ignored files are not diffed)?
bool row_diffed(const GitStatusRow &r)
{
    if ( r.worktree == git::file_state::untracked || r.worktree == git::file_state::ignored )
	return false;
    return r.index != git::file_state::unmodified
	|| r.worktree != git::file_state::unmodified;
}

// `diff [FILE]`: FILE as it is now (the host's buffer's live text, else the
// file) against its last commit — git diff HEAD -- FILE, a buffer's unsaved
// edits included; with no FILE, each file git status names as changed, from
// disk (git diff HEAD). Nothing changed prints nothing.
bool verb_diff(const std::vector<std::string> &w, std::string &out, std::string &why)
{
    if ( w.size() > 2 )
    {
	why = "diff takes at most one FILE";
	return false;
    }
    const std::string file = w.size() == 2 ? w[1] : std::string();
    GitRepo repo;
    verb_file f;
    if ( !open_repo(repo, file, f, why) )
	return false;
    error err;
    std::vector<GitHunk> hunks;
    if ( !file.empty() )
    {
	std::string rel;
	if ( !repo.relpath(f.path, rel, &err)
	  || !repo.diff("HEAD", rel, live_text(f), out, hunks, &err) )
	    return refuse(why, err);
	return true;
    }
    std::vector<GitStatusRow> rows;
    if ( !repo.status(rows, &err) )
	return refuse(why, err);
    for ( const GitStatusRow &r : rows )
    {
	if ( !row_diffed(r) )
	    continue;
	std::string text, patch;
	read_file_bytes(repo.workdir() + r.path, text);
	if ( !repo.diff("HEAD", r.path, text, patch, hunks, &err) )
	    return refuse(why, err);
	out += patch;
    }
    return true;
}

} // namespace

bool git_command(const std::string &argument, std::string &out, std::string &why)
{
    std::vector<std::string> words;
    out.clear();
    if ( !ns_common::shell_words(argument, words) )
    {
	why = "a quote is not closed";
	return false;
    }
    if ( words.empty() )
    {
	why = "which verb? " + git_usage();
	return false;
    }
    switch ( git_verb_of(words[0]) )
    {
	case git_verb::log:    return verb_log(words, out, why);
	case git_verb::show:   return verb_show(words, out, why);
	case git_verb::blame:  return verb_blame(words, out, why);
	case git_verb::status: return verb_status(words, out, why);
	case git_verb::diff:   return verb_diff(words, out, why);
	case git_verb::none:   break;
    }
    why = "unknown verb '" + words[0] + "': " + git_usage();
    return false;
}

namespace {

// The registry's handler: the verb's text, shown without its last newline
// (the REPL ends a shown text with one), or `%git: WHY`.
bool git_handler(value &out, const char *argument)
{
    std::string text, why;
    if ( !git_command(argument ? argument : "", text, why) )
    {
	out = value("%git: " + why);
	return false;
    }
    if ( !text.empty() && text[text.size() - 1] == '\n' )
	text.erase(text.size() - 1);
    out = value(text);
    return true;
}

const madc_module_command git_rows[] = {
    { "git", "%git VERB [ARGS]",
      "the repository: log, show, blame, status, diff (%git alone lists them)",
      git_handler },
};

} // namespace

} // namespace madc

// The module's command rows (its description's `commands`): one, `%git`.
const madc_module_command *git::module_commands(std::size_t *count)
{
    *count = sizeof(madc::git_rows) / sizeof(madc::git_rows[0]);
    return madc::git_rows;
}
