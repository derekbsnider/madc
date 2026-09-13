// madcdis_git_repo.cpp — madc::GitRepo, the ONE libgit2 caller in madc
// (include/madcdis/git_repo.h states the contract; gate
// scripts/check-one-git-owner.sh), plus the git source adapter over it and the
// shared value shapers. Read-only: nothing here writes a repository.

#include "madcdis/git_repo.h"

#include <git2.h>

#include <cstdint>
#include <cstring>
#include <map>

namespace madc {

namespace {

// Once per process: libgit2 refcounts init/shutdown; the static's destructor
// balances the one init at exit.
struct git_runtime
{
    git_runtime()  { git_libgit2_init(); }
    ~git_runtime() { git_libgit2_shutdown(); }
};

void ensure_runtime()
{
    static git_runtime once;
    (void)once;
}

// libgit2's last error, phrased under `what` — the prose the seat shows.
void set_err(error *err, const char *what)
{
    if ( !err )
	return;
    const git_error *e = git_error_last();
    *err = error(error::severity::error, error::phase::runtime,
		 std::string(what) + ": "
		 + (e && e->message ? e->message : "unknown libgit2 error"));
}

std::string oid_str(const git_oid *id)
{
    char buf[GIT_OID_SHA1_HEXSIZE + 1];
    git_oid_tostr(buf, sizeof buf, id);
    return buf;
}

} // namespace

struct GitRepo::impl
{
    git_repository *repo;
    std::string workdir;
    impl() : repo((git_repository *)0) {}
};

GitRepo::GitRepo() : _(new impl) {}

GitRepo::~GitRepo()
{
    if ( _->repo )
	git_repository_free(_->repo);
    delete _;
}

bool GitRepo::is_open() const { return _->repo != (git_repository *)0; }
const std::string &GitRepo::workdir() const { return _->workdir; }

bool GitRepo::open(const std::string &path, error *err)
{
    ensure_runtime();
    if ( _->repo )
    {
	git_repository_free(_->repo);
	_->repo = (git_repository *)0;
	_->workdir.clear();
    }
    if ( git_repository_open_ext(&_->repo, path.c_str(), 0, NULL) < 0 )
    {
	_->repo = (git_repository *)0;
	set_err(err, "git open");
	return false;
    }
    const char *wd = git_repository_workdir(_->repo);
    _->workdir = wd ? wd : "";
    return true;
}

bool GitRepo::head(GitRef &out, error *err) const
{
    git_reference *ref = (git_reference *)0;
    if ( !_->repo || git_repository_head(&ref, _->repo) < 0 )
    {
	set_err(err, "git head");
	return false;
    }
    const git_oid *id = git_reference_target(ref);
    out.sha = id ? oid_str(id) : "";
    out.detached = git_repository_head_detached(_->repo) == 1;
    const char *sh = out.detached ? (const char *)0 : git_reference_shorthand(ref);
    out.branch = sh ? sh : "";
    git_reference_free(ref);
    return true;
}

bool GitRepo::refs(std::vector<GitRefRow> &out, error *err) const
{
    out.clear();
    git_reference_iterator *it = (git_reference_iterator *)0;
    if ( !_->repo || git_reference_iterator_new(&it, _->repo) < 0 )
    {
	set_err(err, "git refs");
	return false;
    }
    git_reference *ref = (git_reference *)0;
    while ( git_reference_next(&ref, it) == 0 )
    {
	GitRefRow row;
	const char *nm = git_reference_name(ref);
	row.name = nm ? nm : "";
	git_reference *resolved = (git_reference *)0;
	if ( git_reference_resolve(&resolved, ref) == 0 )
	{
	    const git_oid *id = git_reference_target(resolved);
	    row.sha = id ? oid_str(id) : "";
	    git_reference_free(resolved);
	}
	git_reference_free(ref);
	out.push_back(row);
    }
    git_reference_iterator_free(it);
    return true;
}

bool GitRepo::revparse(const std::string &spec, std::string &sha, error *err) const
{
    git_object *obj = (git_object *)0;
    if ( !_->repo || git_revparse_single(&obj, _->repo, spec.c_str()) < 0 )
    {
	set_err(err, "git rev-parse");
	return false;
    }
    sha = oid_str(git_object_id(obj));
    git_object_free(obj);
    return true;
}

namespace {

// The blob id at `path` in `tree`; false when the path is absent.
bool entry_oid_at(git_tree *tree, const char *path, git_oid &out)
{
    git_tree_entry *e = (git_tree_entry *)0;
    if ( !tree || git_tree_entry_bypath(&e, tree, path) < 0 )
	return false;
    git_oid_cpy(&out, git_tree_entry_id(e));
    git_tree_entry_free(e);
    return true;
}

// `git log -- path`'s keep rule: the blob at `path` differs from every
// parent's (a root commit that has the path counts).
bool touches_path(git_commit *c, const char *path)
{
    git_tree *tree = (git_tree *)0;
    git_oid mine;
    bool have = false;
    if ( git_commit_tree(&tree, c) == 0 )
    {
	have = entry_oid_at(tree, path, mine);
	git_tree_free(tree);
    }
    if ( !have )
	return false;
    unsigned n = git_commit_parentcount(c);
    if ( n == 0 )
	return true;
    for ( unsigned i = 0; i < n; ++i )
    {
	git_commit *p = (git_commit *)0;
	git_tree *pt = (git_tree *)0;
	git_oid theirs;
	bool same = false;
	if ( git_commit_parent(&p, c, i) == 0 )
	{
	    if ( git_commit_tree(&pt, p) == 0 )
	    {
		same = entry_oid_at(pt, path, theirs) && git_oid_equal(&theirs, &mine) != 0;
		git_tree_free(pt);
	    }
	    git_commit_free(p);
	}
	if ( !same )
	    return true;
    }
    return false;
}

void fill_commit(GitCommit &row, const git_oid *id, git_commit *c)
{
    row.sha = oid_str(id);
    const git_signature *a = git_commit_author(c);
    row.author = a && a->name ? a->name : "";
    row.email = a && a->email ? a->email : "";
    row.when = (int64_t)git_commit_time(c);
    const char *s = git_commit_summary(c);
    row.summary = s ? s : "";
}

} // namespace

bool GitRepo::log(std::vector<GitCommit> &out, const std::string &path, size_t limit,
		  error *err) const
{
    out.clear();
    git_revwalk *walk = (git_revwalk *)0;
    if ( !_->repo || git_revwalk_new(&walk, _->repo) < 0 )
    {
	set_err(err, "git log");
	return false;
    }
    if ( git_revwalk_push_head(walk) < 0 )
    {
	git_revwalk_free(walk);
	set_err(err, "git log");
	return false;
    }
    git_revwalk_sorting(walk, GIT_SORT_TIME);
    git_oid oid;
    while ( out.size() < limit && git_revwalk_next(&oid, walk) == 0 )
    {
	git_commit *c = (git_commit *)0;
	if ( git_commit_lookup(&c, _->repo, &oid) < 0 )
	    break;
	if ( path.empty() || touches_path(c, path.c_str()) )
	{
	    GitCommit row;
	    fill_commit(row, &oid, c);
	    out.push_back(row);
	}
	git_commit_free(c);
    }
    git_revwalk_free(walk);
    return true;
}

bool GitRepo::show(const std::string &rev, const std::string &path, std::string &text,
		   error *err) const
{
    git_object *obj = (git_object *)0;
    if ( !_->repo || git_revparse_single(&obj, _->repo, rev.c_str()) < 0 )
    {
	set_err(err, "git show");
	return false;
    }
    git_object *tree_obj = (git_object *)0;
    git_tree_entry *e = (git_tree_entry *)0;
    git_blob *blob = (git_blob *)0;
    bool ok = false;
    if ( git_object_peel(&tree_obj, obj, GIT_OBJECT_TREE) == 0
	 && git_tree_entry_bypath(&e, (git_tree *)tree_obj, path.c_str()) == 0
	 && git_blob_lookup(&blob, _->repo, git_tree_entry_id(e)) == 0 )
    {
	text.assign((const char *)git_blob_rawcontent(blob),
		    (size_t)git_blob_rawsize(blob));
	ok = true;
    }
    else
	set_err(err, "git show");
    if ( blob )
	git_blob_free(blob);
    if ( e )
	git_tree_entry_free(e);
    if ( tree_obj )
	git_object_free(tree_obj);
    git_object_free(obj);
    return ok;
}

namespace {

// The ONE hunk -> row mapping, shared by blame (the committed file) and
// blame_buffer (the live text). A zero oid is an uncommitted hunk: sha "",
// no author, no summary.
void blame_rows(git_repository *repo, git_blame *bl, std::vector<GitBlameRow> &out)
{
    uint32_t n = git_blame_get_hunk_count(bl);
    for ( uint32_t i = 0; i < n; ++i )
    {
	const git_blame_hunk *h = git_blame_get_hunk_byindex(bl, i);
	if ( !h )
	    continue;
	GitBlameRow row;
	row.line = (int64_t)h->final_start_line_number;
	row.count = (int64_t)h->lines_in_hunk;
	if ( git_oid_is_zero(&h->final_commit_id) )
	{
	    out.push_back(row);		// uncommitted: sha stays ""
	    continue;
	}
	row.sha = oid_str(&h->final_commit_id);
	if ( h->final_signature )
	{
	    row.author = h->final_signature->name ? h->final_signature->name : "";
	    row.when = (int64_t)h->final_signature->when.time;
	}
	git_commit *c = (git_commit *)0;
	if ( git_commit_lookup(&c, repo, &h->final_commit_id) == 0 )
	{
	    const char *s = git_commit_summary(c);
	    row.summary = s ? s : "";
	    git_commit_free(c);
	}
	out.push_back(row);
    }
}

git_blame_options blame_opts(size_t line0, size_t count)
{
    git_blame_options opts = GIT_BLAME_OPTIONS_INIT;
    opts.min_line = line0;
    opts.max_line = count ? line0 + count - 1 : 0;
    return opts;
}

} // namespace

bool GitRepo::blame(std::vector<GitBlameRow> &out, const std::string &path,
		    size_t line0, size_t count, error *err) const
{
    out.clear();
    git_blame_options opts = blame_opts(line0, count);
    git_blame *bl = (git_blame *)0;
    if ( !_->repo || git_blame_file(&bl, _->repo, path.c_str(), &opts) < 0 )
    {
	set_err(err, "git blame");
	return false;
    }
    blame_rows(_->repo, bl, out);
    git_blame_free(bl);
    return true;
}

bool GitRepo::blame_buffer(std::vector<GitBlameRow> &out, const std::string &path,
			   const std::string &text, size_t line0, size_t count,
			   error *err) const
{
    out.clear();
    // The line range names lines of the BUFFER, not of the committed file, so
    // the reference blame runs over the whole file and the buffer's hunks are
    // clipped to the range afterwards (a hunk straddling an edge is trimmed).
    git_blame_options opts = GIT_BLAME_OPTIONS_INIT;
    git_blame *ref = (git_blame *)0;
    if ( !_->repo || git_blame_file(&ref, _->repo, path.c_str(), &opts) < 0 )
    {
	set_err(err, "git blame");
	return false;
    }
    git_blame *bl = (git_blame *)0;
    if ( git_blame_buffer(&bl, ref, text.data(), text.size()) < 0 )
    {
	git_blame_free(ref);
	set_err(err, "git blame (buffer)");
	return false;
    }
    std::vector<GitBlameRow> all;
    blame_rows(_->repo, bl, all);
    git_blame_free(bl);
    git_blame_free(ref);
    int64_t lo = line0 ? (int64_t)line0 : 1;
    int64_t hi = count ? lo + (int64_t)count - 1 : INT64_MAX;	// inclusive
    for ( size_t i = 0; i < all.size(); ++i )
    {
	GitBlameRow row = all[i];
	int64_t first = row.line;
	int64_t last = row.line + row.count - 1;
	if ( last < lo || first > hi )
	    continue;
	if ( first < lo )
	    first = lo;
	if ( last > hi )
	    last = hi;
	row.line = first;
	row.count = last - first + 1;
	out.push_back(row);
    }
    return true;
}

bool GitRepo::dirty(const std::string &path, bool &out, error *err) const
{
    unsigned int flags = 0;
    if ( !_->repo || git_status_file(&flags, _->repo, path.c_str()) < 0 )
    {
	set_err(err, "git status");
	return false;
    }
    out = flags != GIT_STATUS_CURRENT;
    return true;
}

// ------------------------------------------------------------- value shapers

value git_commit_value(const GitCommit &c)
{
    std::map<std::string, value> f;
    f["sha"] = value(c.sha);
    f["author"] = value(c.author);
    f["email"] = value(c.email);
    f["when"] = value(c.when);
    f["summary"] = value(c.summary);
    return value::make_object(f);
}

value git_ref_value(const GitRefRow &r)
{
    std::map<std::string, value> f;
    f["name"] = value(r.name);
    f["sha"] = value(r.sha);
    return value::make_object(f);
}

value git_blame_value(const GitBlameRow &b)
{
    std::map<std::string, value> f;
    f["line"] = value(b.line);
    f["count"] = value(b.count);
    f["sha"] = value(b.sha);
    f["author"] = value(b.author);
    f["when"] = value(b.when);
    f["summary"] = value(b.summary);
    return value::make_object(f);
}

// ------------------------------------------------------- git_source_adapter

namespace {

// "<repo>?path=<file>" — DataSource keeps the whole string as the path of a
// path_like scheme; the adapter owns the ONE split of its own query.
void split_query(const std::string &location, std::string &repo, std::string &file)
{
    std::size_t q = location.find('?');
    repo = q == std::string::npos ? location : location.substr(0, q);
    file.clear();
    if ( q != std::string::npos && location.compare(q + 1, 5, "path=") == 0 )
	file = location.substr(q + 6);
}

void adapter_err(error *err, const std::string &why)
{
    if ( err )
	*err = error(error::severity::error, error::phase::runtime, "git: " + why);
}

} // namespace

bool git_source_adapter::can_read(const DataSource &source) const
{
    return source.scheme() == "git";
}

bool git_source_adapter::discover_types(const DataSource &,
					std::vector<ExtractedRecordType> &out,
					error *) const
{
    out.push_back(ExtractedRecordType("commit"));
    out.push_back(ExtractedRecordType("ref"));
    out.push_back(ExtractedRecordType("blame"));
    return true;
}

bool git_source_adapter::extract(const DataSource &source, const std::string &type_name,
				 std::vector<ExtractedRecord> &out, error *err) const
{
    out.clear();
    std::string repo_path, file;
    split_query(source.path().empty() ? source.location() : source.path(),
		repo_path, file);
    GitRepo repo;
    if ( !repo.open(repo_path, err) )
	return false;
    if ( type_name == "commit" )
    {
	std::vector<GitCommit> rows;
	if ( !repo.log(rows, file, (size_t)-1, err) )
	    return false;
	for ( size_t i = 0; i < rows.size(); ++i )
	{
	    ExtractedRecord r;
	    r.type_name = "commit";
	    r.record = git_commit_value(rows[i]);
	    r.locator = SourceLocator::at_key_path(rows[i].sha);
	    out.push_back(r);
	}
	return true;
    }
    if ( type_name == "ref" )
    {
	std::vector<GitRefRow> rows;
	if ( !repo.refs(rows, err) )
	    return false;
	for ( size_t i = 0; i < rows.size(); ++i )
	{
	    ExtractedRecord r;
	    r.type_name = "ref";
	    r.record = git_ref_value(rows[i]);
	    r.locator = SourceLocator::at_key_path(rows[i].name);
	    out.push_back(r);
	}
	return true;
    }
    if ( type_name == "blame" )
    {
	if ( file.empty() )
	{
	    adapter_err(err, "the blame family needs ?path=<file> on the source");
	    return false;
	}
	std::vector<GitBlameRow> rows;
	if ( !repo.blame(rows, file, 1, 0, err) )
	    return false;
	for ( size_t i = 0; i < rows.size(); ++i )
	{
	    ExtractedRecord r;
	    r.type_name = "blame";
	    r.record = git_blame_value(rows[i]);
	    r.locator = SourceLocator::at_line_range((size_t)rows[i].line,
						     (size_t)rows[i].count);
	    out.push_back(r);
	}
	return true;
    }
    adapter_err(err, "unknown record family `" + type_name + "`");
    return false;
}

} // namespace madc
