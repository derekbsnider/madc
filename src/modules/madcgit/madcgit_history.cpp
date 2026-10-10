// modules/madcgit/madcgit_history.cpp — the version history the engine's
// history tools read (split arc 1C.14; include/madc_tools.h's
// history_source): the served file's state in its repository, the commits
// touching it, its text at a revision and the blame of its live text, over
// GitRepo. The module's tools entry (madc_module_tools.h) hands this source
// to the engine's history tools when a tool server loads the module.
//
// Thread contract: a read runs on its registry's thread; its repository is
// opened and closed inside it.

#include "madcgit_values.h"
#include "madcdis/git_repo.h"
#include "madc_module_tools.h"
#include "madc_tools.h"
#include "madc_posix_io.h"		// host_path_dirname: where the repository is found

#include <map>
#include <string>
#include <vector>

namespace madc {

namespace {

typedef std::map<std::string, value> fields;

bool refuse(value &out, const std::string &why)
{
    out = tools::refusal(why);
    return false;
}

value rows_of(const std::vector<value> &rows)
{
    fields f;
    f["rows"] = value::make_array(rows);
    return value::make_object(f);
}

class git_history : public tools::history_source
{
public:
    bool state(const std::string &file, value &out) override
    {
	GitRepo repo;
	std::string rel;
	GitRef head;
	bool dirty = false;
	error err;
	if ( !open(file, repo, rel, out) )
	    return false;
	if ( !repo.head(head, &err) || !repo.dirty(rel, dirty, &err) )
	    return refuse(out, err.message);
	fields f;
	f["head"] = value(head.sha);
	f["branch"] = value(head.branch);
	f["detached"] = value(head.detached);
	f["dirty"] = value(dirty);
	f["path"] = value(rel);
	out = value::make_object(f);
	return true;
    }

    bool commits(const std::string &file, int64_t limit, value &out) override
    {
	GitRepo repo;
	std::string rel;
	std::vector<GitCommit> commits;
	error err;
	if ( !open(file, repo, rel, out) )
	    return false;
	if ( !repo.log(commits, rel, limit > 0 ? (size_t)limit : 20, &err) )
	    return refuse(out, err.message);
	std::vector<value> rows;
	for ( const GitCommit &c : commits )
	    rows.push_back(git_commit_value(c));
	out = rows_of(rows);
	return true;
    }

    bool show(const std::string &file, const std::string &spec, value &out) override
    {
	GitRepo repo;
	std::string rel, sha, text;
	error err;
	if ( !open(file, repo, rel, out) )
	    return false;
	if ( !repo.revparse(spec, sha, &err) || !repo.show(sha, rel, text, &err) )
	    return refuse(out, err.message);
	fields f;
	f["sha"] = value(sha);
	f["text"] = value(text);
	out = value::make_object(f);
	return true;
    }

    bool blame(const std::string &file, const std::string &text, int64_t line,
	       int64_t count, value &out) override
    {
	GitRepo repo;
	std::string rel;
	std::vector<GitBlameRow> hunks;
	error err;
	if ( !open(file, repo, rel, out) )
	    return false;
	if ( !repo.blame_buffer(hunks, rel, text, line > 0 ? (size_t)line : 1,
				count > 0 ? (size_t)count : 0, &err) )
	    return refuse(out, err.message);
	std::vector<value> rows;
	for ( const GitBlameRow &b : hunks )
	    rows.push_back(git_blame_value(b));
	out = rows_of(rows);
	return true;
    }

private:
    // The repository above `file`, and the file's path in its working tree.
    static bool open(const std::string &file, GitRepo &repo, std::string &rel, value &out)
    {
	std::string dir = detail::host_path_dirname(file);
	error err;
	if ( !repo.open(dir.empty() ? std::string(".") : dir, &err) )
	    return refuse(out, "no repository above " + file);
	if ( !repo.relpath(file, rel, &err) )
	    return refuse(out, err.message);
	return true;
    }
};

} // namespace

} // namespace madc

// The module's tools (its map row's `tools`): the engine's history tools
// over this module's version history.
extern "C" void madcgit_tools(madc::tools::registry &r)
{
    static madc::git_history history;
    madc::tools::add_history_tools(r, history);
}
