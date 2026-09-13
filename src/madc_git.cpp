// madc_git.cpp — the engine face of the git substrate (Nexus L4a, design
// docs/plans/2026-09-13-nexus-L4-design.md §4.2; plan
// 2026-09-13-nexus-L4a-git-substrate-plan.md task 5): a handle_table of open
// madc::GitRepo objects and the value-shaped answers the dialect publics
// (include/madc/ns_madc git_*) return, bridged through src/parser.cpp exactly
// like the parse_*/graph_* publics. No libgit2 here — GitRepo is the ONE owner
// (scripts/check-one-git-owner.sh); the row shapes are GitRepo's shapers, so
// the adapter and this face never spell a row twice.
//
// Every public answers an object, or {error: <prose>} (the graph_* refusal
// shape the madcide seat turns into isError); git_open answers 0 for a path
// with no repository above it. THREAD CONTRACT: the runtime-eval confinement —
// a handle is used only from the thread that opened it (the parse-handle rule;
// handle_table.h states the table's).

#include "madcdis/git_repo.h"
#include "handle_table.h"
#include "libmadc/value.h"
#include "madc_posix_io.h"	// canonical_path_for_compare — THE path canonicalizer

#include <map>
#include <string>
#include <vector>

namespace madc {

namespace {

struct git_repo_state
{
    GitRepo repo;
};

// THE slot+1 registry rule (include/handle_table.h) — the parse handles'
// discipline; a separate id space.
handle_table<git_repo_state> &git_handles()
{
    static handle_table<git_repo_state> handles;
    return handles;
}

value error_value(const std::string &why)
{
    std::map<std::string, value> f;
    f["error"] = value(why);
    return value::make_object(f);
}

// The state behind a handle, or the refusal every verb answers for a closed /
// unknown one (never a wrong repository).
git_repo_state *state_or_refuse(int64_t handle, value &out)
{
    git_repo_state *st = git_handles().get(handle);
    if ( !st )
	out = error_value("git: no such repository handle");
    return st;
}

value rows_value(const std::vector<value> &rows)
{
    std::map<std::string, value> f;
    f["rows"] = value::make_array(rows);
    return value::make_object(f);
}

} // namespace

int64_t internal_program_git_open(const std::string &path)
{
    git_repo_state *st = new git_repo_state();
    if ( !st->repo.open(path) )
    {
	delete st;
	return 0;
    }
    return git_handles().open(st);
}

bool internal_program_git_close(int64_t handle)
{
    return git_handles().close(handle);
}

bool internal_program_git_head(int64_t handle, value &out)
{
    git_repo_state *st = state_or_refuse(handle, out);
    if ( !st )
	return false;
    GitRef r;
    error err;
    if ( !st->repo.head(r, &err) )
    {
	out = error_value(err.message);
	return false;
    }
    std::map<std::string, value> f;
    f["sha"] = value(r.sha);
    f["branch"] = value(r.branch);
    f["detached"] = value(r.detached);
    out = value::make_object(f);
    return true;
}

bool internal_program_git_revparse(int64_t handle, const std::string &spec, value &out)
{
    git_repo_state *st = state_or_refuse(handle, out);
    if ( !st )
	return false;
    std::string sha;
    error err;
    if ( !st->repo.revparse(spec, sha, &err) )
    {
	out = error_value(err.message);
	return false;
    }
    std::map<std::string, value> f;
    f["sha"] = value(sha);
    out = value::make_object(f);
    return true;
}

bool internal_program_git_log(int64_t handle, const std::string &path, int64_t limit,
			      value &out)
{
    git_repo_state *st = state_or_refuse(handle, out);
    if ( !st )
	return false;
    std::vector<GitCommit> commits;
    error err;
    if ( !st->repo.log(commits, path, limit > 0 ? (size_t)limit : (size_t)100, &err) )
    {
	out = error_value(err.message);
	return false;
    }
    std::vector<value> rows;
    for ( size_t i = 0; i < commits.size(); ++i )
	rows.push_back(git_commit_value(commits[i]));
    out = rows_value(rows);
    return true;
}

bool internal_program_git_show(int64_t handle, const std::string &rev,
			       const std::string &path, value &out)
{
    git_repo_state *st = state_or_refuse(handle, out);
    if ( !st )
	return false;
    std::string text;
    error err;
    if ( !st->repo.show(rev, path, text, &err) )
    {
	out = error_value(err.message);
	return false;
    }
    std::map<std::string, value> f;
    f["text"] = value(text);
    out = value::make_object(f);
    return true;
}

bool internal_program_git_blame(int64_t handle, const std::string &path, int64_t line,
				int64_t count, value &out)
{
    git_repo_state *st = state_or_refuse(handle, out);
    if ( !st )
	return false;
    std::vector<GitBlameRow> hunks;
    error err;
    if ( !st->repo.blame(hunks, path, line > 0 ? (size_t)line : 1,
			 count > 0 ? (size_t)count : 0, &err) )
    {
	out = error_value(err.message);
	return false;
    }
    std::vector<value> rows;
    for ( size_t i = 0; i < hunks.size(); ++i )
	rows.push_back(git_blame_value(hunks[i]));
    out = rows_value(rows);
    return true;
}

// L4b: blame the LIVE buffer text (not the file on disk) against the committed
// history; uncommitted hunks come back with sha "" (the event log covers them).
bool internal_program_git_blame_text(int64_t handle, const std::string &path,
				     const std::string &text, int64_t line, int64_t count,
				     value &out)
{
    git_repo_state *st = state_or_refuse(handle, out);
    if ( !st )
	return false;
    std::vector<GitBlameRow> hunks;
    error err;
    if ( !st->repo.blame_buffer(hunks, path, text, line > 0 ? (size_t)line : 1,
				count > 0 ? (size_t)count : 0, &err) )
    {
	out = error_value(err.message);
	return false;
    }
    std::vector<value> rows;
    for ( size_t i = 0; i < hunks.size(); ++i )
	rows.push_back(git_blame_value(hunks[i]));
    out = rows_value(rows);
    return true;
}

// L4b: a path relative to the repository's working tree, both sides
// canonicalised by the ONE path canonicalizer madc has (the lexer's include
// resolution uses the same one) — never a string prefix test on raw spellings.
bool internal_program_git_relpath(int64_t handle, const std::string &path, value &out)
{
    git_repo_state *st = state_or_refuse(handle, out);
    if ( !st )
	return false;
    std::string wd = detail::canonical_path_for_compare(st->repo.workdir());
    while ( wd.size() > 1 && (wd[wd.size() - 1] == '/' || wd[wd.size() - 1] == '\\') )
	wd.erase(wd.size() - 1);
    std::string p = detail::canonical_path_for_compare(path);
    if ( wd.empty() || p.size() <= wd.size() + 1 || p.compare(0, wd.size(), wd) != 0
	 || (p[wd.size()] != '/' && p[wd.size()] != '\\') )
    {
	out = error_value("git: `" + path + "` is not inside the repository's working tree");
	return false;
    }
    std::map<std::string, value> f;
    f["path"] = value(p.substr(wd.size() + 1));
    out = value::make_object(f);
    return true;
}

bool internal_program_git_dirty(int64_t handle, const std::string &path, value &out)
{
    git_repo_state *st = state_or_refuse(handle, out);
    if ( !st )
	return false;
    bool d = false;
    error err;
    if ( !st->repo.dirty(path, d, &err) )
    {
	out = error_value(err.message);
	return false;
    }
    std::map<std::string, value> f;
    f["dirty"] = value(d);
    out = value::make_object(f);
    return true;
}

} // namespace madc
