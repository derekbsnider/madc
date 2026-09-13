#ifndef __MADCDIS_GIT_REPO_H
#define __MADCDIS_GIT_REPO_H 1

// madcdis/git_repo.h — the ONE owner of every libgit2 call in madc (design
// docs/plans/2026-09-13-nexus-L4-design.md §4.2; plan
// 2026-09-13-nexus-L4a-git-substrate-plan.md; gate
// scripts/check-one-git-owner.sh). READ-ONLY by construction: no write API is
// wrapped (a commit, a checkout, a fetch stay git's). Records are plain
// structs; the value shapers below turn them into the ONE row shape both the
// source adapter and the engine face (src/madc_git.cpp) answer with.
//
// libgit2 itself is the vendored, unmodified subtree third_party/libgit2
// (network transports OFF — scripts/check-libgit2-features.sh); <git2.h> is
// included by src/madcdis_git_repo.cpp only, never by this header (pimpl).
//
// THREAD-SAFETY CONTRACT (.claude/rules/thread-safety.md): a GitRepo is
// CONFINED to the thread that opened it — libgit2's own objects are not
// shareable, and GIT_THREADS is on only for libgit2's internal correctness.
// git_libgit2_init/shutdown run once per process behind a static guard (the
// first open pays for it; the guard's destructor balances it at exit).

#include "libmadc/error.h"
#include "libmadc/value.h"
#include "madcdis/source_adapter.h"

#include <cstdint>
#include <string>
#include <vector>

namespace madc {

struct GitRef
{
    std::string sha;
    std::string branch;		// shorthand ("master"); "" when detached
    bool detached;
    GitRef() : detached(false) {}
};

struct GitRefRow
{
    std::string name;		// the full name: refs/heads/x, refs/tags/y
    std::string sha;		// the resolved target
};

struct GitCommit
{
    std::string sha;
    std::string author;
    std::string email;
    int64_t when;		// author time, unix seconds
    std::string summary;	// the first line of the message
    GitCommit() : when(0) {}
};

struct GitBlameRow
{
    int64_t line;		// 1-based first line of the hunk in the CURRENT file
    int64_t count;		// lines in the hunk
    std::string sha;		// the commit that last touched them
    std::string author;
    int64_t when;
    std::string summary;
    GitBlameRow() : line(0), count(0), when(0) {}
};

class GitRepo
{
public:
    GitRepo();
    ~GitRepo();

    // Discover the repository upward from `path` (a working-tree path or a
    // .git directory). False + err prose when there is none.
    bool open(const std::string &path, error *err = (error *)0);
    bool is_open() const;
    const std::string &workdir() const;		// "" for a bare repository

    bool head(GitRef &out, error *err = (error *)0) const;
    bool refs(std::vector<GitRefRow> &out, error *err = (error *)0) const;
    bool revparse(const std::string &spec, std::string &sha,
		  error *err = (error *)0) const;
    // `git log [-- path]`, newest first, at most `limit` rows. With a path,
    // a commit is kept iff the blob at that path differs from EVERY parent's
    // (a root commit that has the path counts) — exact, no heuristics.
    bool log(std::vector<GitCommit> &out, const std::string &path, size_t limit,
	     error *err = (error *)0) const;
    // The blob's bytes at `rev` (any rev-parse spec) for `path`.
    bool show(const std::string &rev, const std::string &path, std::string &text,
	      error *err = (error *)0) const;
    // Blame `count` lines from 1-based `line0` (count 0 = to the end).
    bool blame(std::vector<GitBlameRow> &out, const std::string &path,
	       size_t line0, size_t count, error *err = (error *)0) const;
    // True when `path`'s working-tree state differs from HEAD/index.
    bool dirty(const std::string &path, bool &out, error *err = (error *)0) const;

private:
    GitRepo(const GitRepo &);
    GitRepo &operator=(const GitRepo &);
    struct impl;
    impl *_;
};

// The ONE value shape per record, shared by the source adapter and the engine
// face (src/madc_git.cpp) — never two spellings of a row.
//   commit: {sha, author, email, when, summary}
//   ref:    {name, sha}
//   blame:  {line, count, sha, author, when, summary}
value git_commit_value(const GitCommit &c);
value git_ref_value(const GitRefRow &r);
value git_blame_value(const GitBlameRow &b);

// The madcdis face: `git://<repo path>[?path=<file>]` — a `git` scheme row in
// DataSource (storage / file / path_like / local) with record families
// "commit" (the log, path-filtered when ?path= is given), "ref" (every
// branch and tag) and "blame" (over the whole file named by ?path=, required).
// DataSource keeps the location; this adapter owns the one split of its query.
class git_source_adapter : public SourceAdapter
{
public:
    const char *name() const { return "git"; }
    bool can_read(const DataSource &source) const;
    bool discover_types(const DataSource &source,
			std::vector<ExtractedRecordType> &out,
			error *err = (error *)0) const;
    bool extract(const DataSource &source, const std::string &type_name,
		 std::vector<ExtractedRecord> &out,
		 error *err = (error *)0) const;
};

} // namespace madc

#endif // __MADCDIS_GIT_REPO_H
