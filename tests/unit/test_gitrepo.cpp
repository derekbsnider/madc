// test_gitrepo.cpp — madc::GitRepo (the ONE read-only libgit2 owner) and the
// git source adapter over a fixture repository the test BUILDS through
// libgit2's own write API (the archive carries it; GitRepo never calls it).
// Two commits with fixed author times, so every answer is deterministic:
//   c1: a.txt = "one\n"                        (time 1700000000)
//   c2: a.txt = "one\ntwo\n", b.txt = "bee\n"   (time 1700000100)
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#define MADC_UNIT_TEST
#include "doctest.h"

#include "modules/git/git_repo.h"
#include "libmadc/datasource.h"
#include "modules/git/git_verbs.h"	// %git's verbs (1B.10), the module's tools entry (1C.14)
#include "madc_tools.h"			// the history source the module hands the tools (1C.14)
#include "madc_session.h"		// set_host_buffers: a program's own buffers
#include "madc_documents.h"		// document_buffer: a world's documents answer
#include "madcdis/hub.h"
#include "madcdis/verbs.h"		// mutation_context: an unsaved edit

#include <git2.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#else
#include <ftw.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

thread_local bool madc_verbose = false;
#define DBG(x) do { } while (0)

namespace {

#ifndef _WIN32
int unlink_cb(const char *path, const struct stat *, int, struct FTW *)
{
    return remove(path);
}
#endif

void rm_rf(const std::string &dir)
{
    if ( dir.empty() )
	return;
#ifdef _WIN32
    std::string cmd = "rmdir /s /q \"" + dir + "\"";
    (void)std::system(cmd.c_str());	// CRT-portable; the unit harness pattern
#else
    nftw(dir.c_str(), unlink_cb, 16, FTW_DEPTH | FTW_PHYS);
#endif
}

std::string make_temp_dir()
{
#ifdef _WIN32
    char tmpl[] = "madc_gitrepo_XXXXXX";
    if ( _mktemp_s(tmpl, sizeof tmpl) != 0 || _mkdir(tmpl) != 0 )
	return std::string();
    return tmpl;
#else
    char tmpl[] = "/tmp/madc_gitrepo_XXXXXX";
    char *d = mkdtemp(tmpl);
    return d ? std::string(d) : std::string();
#endif
}

std::string oid_hex(const git_oid *id)
{
    char buf[GIT_OID_SHA1_HEXSIZE + 1];
    git_oid_tostr(buf, sizeof buf, id);
    return buf;
}

// The fixture: built with libgit2's WRITE api (init, index, tree, commit) —
// the oracle, not a second owner (GitRepo stays read-only).
struct Fixture
{
    std::string dir;
    std::string first_sha;
    std::string second_sha;
    git_repository *repo;

    Fixture() : repo((git_repository *)0)
    {
	git_libgit2_init();
	dir = make_temp_dir();
	REQUIRE(!dir.empty());
	REQUIRE(git_repository_init(&repo, dir.c_str(), 0) == 0);
	write("a.txt", "one\n");
	first_sha = commit("first", 1700000000, (const char *)0, 1, "a.txt");
	write("a.txt", "one\ntwo\n");
	write("b.txt", "bee\n");
	second_sha = commit("second", 1700000100, first_sha.c_str(), 2, "a.txt", "b.txt");
    }
    ~Fixture()
    {
	if ( repo )
	    git_repository_free(repo);
	git_libgit2_shutdown();
	rm_rf(dir);
    }

    void write(const char *name, const char *text)
    {
	std::ofstream f((dir + "/" + name).c_str(), std::ios::binary | std::ios::trunc);
	f << text;
    }

    // Stage `n` paths, write the tree, commit with a fixed time; returns the sha.
    std::string commit(const char *message, int64_t when, const char *parent_sha,
		       int n, const char *p1, const char *p2 = (const char *)0)
    {
	git_index *idx = (git_index *)0;
	REQUIRE(git_repository_index(&idx, repo) == 0);
	REQUIRE(git_index_add_bypath(idx, p1) == 0);
	if ( n > 1 && p2 )
	    REQUIRE(git_index_add_bypath(idx, p2) == 0);
	REQUIRE(git_index_write(idx) == 0);
	git_oid tree_id;
	REQUIRE(git_index_write_tree(&tree_id, idx) == 0);
	git_index_free(idx);
	git_tree *tree = (git_tree *)0;
	REQUIRE(git_tree_lookup(&tree, repo, &tree_id) == 0);
	git_signature *sig = (git_signature *)0;
	REQUIRE(git_signature_new(&sig, "Fixture", "fixture@example.com",
				  (git_time_t)when, 0) == 0);
	git_oid out;
	if ( parent_sha )
	{
	    git_oid pid;
	    REQUIRE(git_oid_fromstr(&pid, parent_sha) == 0);
	    git_commit *parent = (git_commit *)0;
	    REQUIRE(git_commit_lookup(&parent, repo, &pid) == 0);
	    REQUIRE(git_commit_create_v(&out, repo, "HEAD", sig, sig, (const char *)0,
					message, tree, 1, parent) == 0);
	    git_commit_free(parent);
	}
	else
	    REQUIRE(git_commit_create_v(&out, repo, "HEAD", sig, sig, (const char *)0,
					message, tree, 0) == 0);
	git_signature_free(sig);
	git_tree_free(tree);
	return oid_hex(&out);
    }
};

} // namespace

TEST_CASE("GitRepo opens a repository and answers head/refs/revparse/log/commit/show/blame/dirty")
{
    Fixture fx;
    madc::GitRepo repo;
    madc::error err;
    REQUIRE(repo.open(fx.dir, &err));
    CHECK(repo.is_open());
    CHECK(!repo.workdir().empty());

    madc::GitRef h;
    REQUIRE(repo.head(h, &err));
    CHECK(h.sha == fx.second_sha);
    CHECK(!h.branch.empty());
    CHECK(!h.detached);

    std::vector<madc::GitRefRow> refs;
    REQUIRE(repo.refs(refs, &err));
    REQUIRE(refs.size() >= 1);
    CHECK(refs[0].name.compare(0, 11, "refs/heads/") == 0);
    CHECK(refs[0].sha == fx.second_sha);

    std::string sha;
    REQUIRE(repo.revparse("HEAD~1", sha, &err));
    CHECK(sha == fx.first_sha);
    REQUIRE(repo.revparse("HEAD", sha, &err));
    CHECK(sha == fx.second_sha);

    std::vector<madc::GitCommit> rows;
    REQUIRE(repo.log(rows, "b.txt", 10, &err));
    REQUIRE(rows.size() == 1);		// b.txt appeared in the second commit only
    CHECK(rows[0].sha == fx.second_sha);
    CHECK(rows[0].summary == "second");
    CHECK(rows[0].author == "Fixture");
    CHECK(rows[0].email == "fixture@example.com");
    CHECK(rows[0].when == 1700000100);
    REQUIRE(repo.log(rows, "a.txt", 10, &err));
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].sha == fx.second_sha);
    CHECK(rows[1].sha == fx.first_sha);
    REQUIRE(repo.log(rows, "", 10, &err));	// "" = no path filter
    CHECK(rows.size() == 2);
    REQUIRE(repo.log(rows, "", 1, &err));	// the limit holds
    CHECK(rows.size() == 1);

    madc::GitCommit c;
    REQUIRE(repo.commit("HEAD~1", c, &err));
    CHECK(c.sha == fx.first_sha);
    CHECK(c.summary == "first");
    CHECK(c.when == 1700000000);
    CHECK(!repo.commit("no-such-ref-anywhere", c, &err));

    std::string text;
    REQUIRE(repo.show(fx.first_sha, "a.txt", text, &err));
    CHECK(text == "one\n");
    REQUIRE(repo.show("HEAD", "a.txt", text, &err));
    CHECK(text == "one\ntwo\n");
    REQUIRE(repo.show("HEAD", "b.txt", text, &err));
    CHECK(text == "bee\n");

    std::vector<madc::GitBlameRow> b;
    REQUIRE(repo.blame(b, "a.txt", 1, 2, &err));
    REQUIRE(b.size() == 2);
    CHECK(b[0].line == 1);
    CHECK(b[0].count == 1);
    CHECK(b[0].sha == fx.first_sha);
    CHECK(b[0].summary == "first");
    CHECK(b[0].author == "Fixture");
    CHECK(b[1].line == 2);
    CHECK(b[1].sha == fx.second_sha);
    REQUIRE(repo.blame(b, "a.txt", 2, 0, &err));	// count 0 = to the end
    REQUIRE(b.size() == 1);
    CHECK(b[0].line == 2);

    bool d = true;
    REQUIRE(repo.dirty("a.txt", d, &err));
    CHECK(d == false);
    fx.write("a.txt", "one\ntwo\nthree\n");
    REQUIRE(repo.dirty("a.txt", d, &err));
    CHECK(d == true);
}

namespace {

// A tree of `files` (name, text) written through libgit2's treebuilder.
git_tree *tree_of(git_repository *repo,
		  const std::vector<std::pair<std::string, std::string> > &files)
{
    git_treebuilder *tb = (git_treebuilder *)0;
    REQUIRE(git_treebuilder_new(&tb, repo, (const git_tree *)0) == 0);
    for ( size_t i = 0; i < files.size(); ++i )
    {
	git_oid blob;
	REQUIRE(git_blob_create_from_buffer(&blob, repo, files[i].second.data(),
					    files[i].second.size()) == 0);
	REQUIRE(git_treebuilder_insert((const git_tree_entry **)0, tb,
				       files[i].first.c_str(), &blob,
				       GIT_FILEMODE_BLOB) == 0);
    }
    git_oid id;
    REQUIRE(git_treebuilder_write(&id, tb) == 0);
    git_treebuilder_free(tb);
    git_tree *tree = (git_tree *)0;
    REQUIRE(git_tree_lookup(&tree, repo, &id) == 0);
    return tree;
}

} // namespace

// `git log -- PATH` hides a merge whose PATH is a parent's (git's TREESAME
// rule: shown only when it differs from EVERY parent): merging an unrelated
// history (a subtree import) is no change to a.txt. Oracle, git 2.x: after
// `first` (a.txt), an orphan `side` (x.txt) and `git merge
// --allow-unrelated-histories side`, `git log --format=%s -- a.txt` prints
// `first` alone; `-- x.txt` prints `side` alone.
TEST_CASE("GitRepo log -- path hides a merge that kept the path (git's TREESAME)")
{
    git_libgit2_init();
    const std::string dir = make_temp_dir();
    REQUIRE(!dir.empty());
    git_repository *repo = (git_repository *)0;
    REQUIRE(git_repository_init(&repo, dir.c_str(), 0) == 0);
    git_signature *sig = (git_signature *)0;
    REQUIRE(git_signature_new(&sig, "Fixture", "fixture@example.com", 1700000000, 0) == 0);
    std::vector<std::pair<std::string, std::string> > files;
    files.push_back(std::make_pair(std::string("a.txt"), std::string("one\n")));
    git_tree *t_first = tree_of(repo, files);
    git_oid first_id, side_id, merge_id;
    REQUIRE(git_commit_create(&first_id, repo, (const char *)0, sig, sig, (const char *)0,
			      "first", t_first, 0, (const git_commit **)0) == 0);
    std::vector<std::pair<std::string, std::string> > side_files;
    side_files.push_back(std::make_pair(std::string("x.txt"), std::string("ex\n")));
    git_tree *t_side = tree_of(repo, side_files);
    REQUIRE(git_commit_create(&side_id, repo, (const char *)0, sig, sig, (const char *)0,
			      "side", t_side, 0, (const git_commit **)0) == 0);
    files.push_back(side_files[0]);
    git_tree *t_merge = tree_of(repo, files);
    git_commit *first = (git_commit *)0, *side = (git_commit *)0;
    REQUIRE(git_commit_lookup(&first, repo, &first_id) == 0);
    REQUIRE(git_commit_lookup(&side, repo, &side_id) == 0);
    const git_commit *parents[] = { first, side };
    REQUIRE(git_commit_create(&merge_id, repo, "HEAD", sig, sig, (const char *)0,
			      "merge", t_merge, 2, parents) == 0);
    git_commit_free(first);
    git_commit_free(side);
    git_tree_free(t_first);
    git_tree_free(t_side);
    git_tree_free(t_merge);
    git_signature_free(sig);
    git_repository_free(repo);
    {
	madc::GitRepo r;
	REQUIRE(r.open(dir));
	std::vector<madc::GitCommit> rows;
	REQUIRE(r.log(rows, "a.txt", 10));
	REQUIRE(rows.size() == 1u);
	CHECK(rows[0].summary == "first");
	REQUIRE(r.log(rows, "x.txt", 10));
	REQUIRE(rows.size() == 1u);
	CHECK(rows[0].summary == "side");
	REQUIRE(r.log(rows, "", 10));
	CHECK(rows.size() == 3u);
    }
    git_libgit2_shutdown();
    rm_rf(dir);
}

TEST_CASE("GitRepo blames a modified buffer against the committed file")
{
    Fixture fx;
    madc::GitRepo repo;
    madc::error err;
    REQUIRE(repo.open(fx.dir, &err));
    std::vector<madc::GitBlameRow> b;
    REQUIRE(repo.blame_buffer(b, "a.txt", "one\ntwo\nthree\n", 1, 0, &err));
    REQUIRE(b.size() == 3);
    CHECK(b[0].line == 1);
    CHECK(b[0].sha == fx.first_sha);
    CHECK(b[1].line == 2);
    CHECK(b[1].sha == fx.second_sha);
    CHECK(b[2].line == 3);
    CHECK(b[2].sha.empty());			// the uncommitted line
    CHECK(b[2].summary.empty());
    REQUIRE(repo.blame_buffer(b, "a.txt", "one\ntwo\nthree\n", 3, 1, &err));
    REQUIRE(b.size() == 1);
    CHECK(b[0].sha.empty());
    CHECK(!repo.blame_buffer(b, "nope.txt", "x\n", 1, 0, &err));
}

// `git status`'s rows (plan §7e): a clean tree has none; a modified file,
// an untracked one and a staged new one each answer their two sides, sorted
// by path. git status --porcelain on the same tree (it prints the untracked
// rows last): ` M a.txt` / `A  d.txt` / `?? c.txt`.
TEST_CASE("GitRepo answers git status's rows")
{
    Fixture fx;
    madc::GitRepo repo;
    madc::error err;
    REQUIRE(repo.open(fx.dir, &err));
    std::vector<madc::GitStatusRow> rows;
    REQUIRE(repo.status(rows, &err));
    CHECK(rows.empty());
    fx.write("a.txt", "one\nTWO\n");
    fx.write("c.txt", "sea\n");
    fx.write("d.txt", "dee\n");
    git_index *idx = (git_index *)0;
    REQUIRE(git_repository_index(&idx, fx.repo) == 0);
    REQUIRE(git_index_add_bypath(idx, "d.txt") == 0);
    REQUIRE(git_index_write(idx) == 0);
    git_index_free(idx);
    REQUIRE(repo.status(rows, &err));
    REQUIRE(rows.size() == 3);
    CHECK(rows[0].path == "a.txt");
    CHECK(rows[0].index == git::file_state::unmodified);
    CHECK(rows[0].worktree == git::file_state::modified);
    CHECK(rows[1].path == "c.txt");
    CHECK(rows[1].worktree == git::file_state::untracked);
    CHECK(rows[2].path == "d.txt");
    CHECK(rows[2].index == git::file_state::added);
    CHECK(rows[2].worktree == git::file_state::unmodified);
    CHECK(rows[2].from.empty());
}

// A file at a revision against a text (plan §7e: View ▸ Changes diffs the
// live buffer): git diff's unified patch and its hunks. `git diff` on the
// same change prints the same hunk header and lines.
TEST_CASE("GitRepo diffs a file at a revision against a text")
{
    Fixture fx;
    madc::GitRepo repo;
    madc::error err;
    REQUIRE(repo.open(fx.dir, &err));
    std::string patch;
    std::vector<madc::GitHunk> hunks;
    REQUIRE(repo.diff("HEAD", "a.txt", "one\nTWO\n", patch, hunks, &err));
    REQUIRE(hunks.size() == 1);
    CHECK(hunks[0].header == "@@ -1,2 +1,2 @@");
    CHECK(hunks[0].old_start == 1);
    CHECK(hunks[0].old_lines == 2);
    CHECK(hunks[0].new_start == 1);
    CHECK(hunks[0].new_lines == 2);
    CHECK(patch.find("--- a/a.txt\n+++ b/a.txt\n@@ -1,2 +1,2 @@\n one\n-two\n+TWO\n")
	  != std::string::npos);
    REQUIRE(repo.diff("HEAD", "a.txt", "one\ntwo\n", patch, hunks, &err));
    CHECK(patch.empty());			// equal: nothing to show
    CHECK(hunks.empty());
    REQUIRE(repo.diff(fx.first_sha, "a.txt", "one\ntwo\n", patch, hunks, &err));
    REQUIRE(hunks.size() == 1);
    CHECK(hunks[0].header == "@@ -1 +1,2 @@");
    CHECK(patch.find("@@ -1 +1,2 @@\n one\n+two\n") != std::string::npos);
    REQUIRE(repo.diff("HEAD", "new.txt", "x\n", patch, hunks, &err));
    REQUIRE(hunks.size() == 1);			// absent at HEAD: every line added
    CHECK(hunks[0].old_start == 0);
    CHECK(hunks[0].old_lines == 0);
    CHECK(hunks[0].new_lines == 1);
    CHECK(patch.find("@@ -0,0 +1 @@\n+x\n") != std::string::npos);
    err = madc::error();
    CHECK(!repo.diff("no-such-ref-anywhere", "a.txt", "", patch, hunks, &err));
    CHECK(err.message.find("git diff") == 0);
}

TEST_CASE("GitRepo refuses a non-repository, a missing path and a bad ref with prose")
{
    madc::GitRepo repo;
    madc::error err;
    CHECK(!repo.open("/", &err));
    CHECK(!repo.is_open());
    CHECK(!err.message.empty());
    madc::GitRef h;
    CHECK(!repo.head(h));			// closed: refuses, never crashes

    Fixture fx;
    REQUIRE(repo.open(fx.dir));
    std::string text;
    err = madc::error();
    CHECK(!repo.show("HEAD", "nope.txt", text, &err));
    CHECK(err.message.find("git show") == 0);
    std::string sha;
    err = madc::error();
    CHECK(!repo.revparse("no-such-ref-anywhere", sha, &err));
    CHECK(err.message.find("git rev-parse") == 0);
}

TEST_CASE("DataSource classifies git:// as local path-like storage")
{
    madc::DataSource s("git:///tmp/somewhere/repo");
    CHECK(s.scheme() == "git");
    CHECK(s.path() == "/tmp/somewhere/repo");
    CHECK(s.is_local());
    CHECK(s.is_file_like());
    CHECK(s.is_storage());
}

TEST_CASE("git_source_adapter extracts commit, ref and blame records")
{
    Fixture fx;
    madc::git_source_adapter ad;
    madc::error err;
    madc::DataSource src("git://" + fx.dir);
    CHECK(ad.can_read(src));
    CHECK(!ad.can_read(madc::DataSource("file:///tmp/x")));

    std::vector<madc::ExtractedRecordType> types;
    REQUIRE(ad.discover_types(src, types, &err));
    REQUIRE(types.size() == 3);
    CHECK(types[0].name() == "commit");
    CHECK(types[1].name() == "ref");
    CHECK(types[2].name() == "blame");

    std::vector<madc::ExtractedRecord> rows;
    REQUIRE(ad.extract(src, "commit", rows, &err));
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].type_name == "commit");
    CHECK(rows[0].record.as_object().at("sha").as_string() == fx.second_sha);
    CHECK(rows[0].record.as_object().at("summary").as_string() == "second");
    CHECK(rows[0].locator.locator_kind == madc::SourceLocator::kind::key_path);
    CHECK(rows[0].locator.path == fx.second_sha);

    REQUIRE(ad.extract(madc::DataSource("git://" + fx.dir + "?path=b.txt"), "commit", rows, &err));
    CHECK(rows.size() == 1);

    REQUIRE(ad.extract(madc::DataSource("git://" + fx.dir + "?path=a.txt"), "blame", rows, &err));
    REQUIRE(rows.size() == 2);
    CHECK(rows[1].locator.locator_kind == madc::SourceLocator::kind::line_range);
    CHECK(rows[1].locator.line_start == 2);
    CHECK(rows[1].locator.line_count == 1);
    CHECK(rows[1].record.as_object().at("sha").as_string() == fx.second_sha);

    REQUIRE(ad.extract(src, "ref", rows, &err));
    REQUIRE(rows.size() >= 1);
    CHECK(rows[0].record.as_object().at("sha").as_string() == fx.second_sha);

    err = madc::error();
    CHECK(!ad.extract(src, "blame", rows, &err));	// blame without ?path= refuses
    CHECK(err.message.find("?path=") != std::string::npos);
    err = madc::error();
    CHECK(!ad.extract(src, "nonsense", rows, &err));
    CHECK(err.message.find("unknown record family") != std::string::npos);
    CHECK(!ad.extract(madc::DataSource("git:///"), "commit", rows, &err));
}

namespace {

// The working directory, changed for a scope (%git with no FILE reads the
// repository above it) and restored after.
struct in_directory
{
    std::string was;
    explicit in_directory(const std::string &dir)
    {
	char buf[4096];
#ifdef _WIN32
	if ( _getcwd(buf, sizeof buf) )
	    was = buf;
	REQUIRE(_chdir(dir.c_str()) == 0);
#else
	if ( getcwd(buf, sizeof buf) )
	    was = buf;
	REQUIRE(chdir(dir.c_str()) == 0);
#endif
    }
    ~in_directory()
    {
#ifdef _WIN32
	(void)_chdir(was.c_str());
#else
	(void)chdir(was.c_str());
#endif
    }
};

bool starts_with(const std::string &s, const std::string &p)
{
    return s.compare(0, p.size(), p) == 0;
}

bool ends_with(const std::string &s, const std::string &p)
{
    return s.size() >= p.size() && s.compare(s.size() - p.size(), p.size(), p) == 0;
}

} // namespace

// %git's verbs (split arc 1B.10, design 2026-10-10-git-module-command-design.md
// decision 1) over the fixture: each prints what git prints for the same
// question — show REV:FILE the blob, show REV the header, log
// --format='%h %s (%ar, %an)', blame --date=relative (an uncommitted line
// git's `00000000 (Not Committed Yet`), diff HEAD, status -sb — and the
// refusals. Outside a session, a FILE is the disk's.
TEST_CASE("%git's verbs over the fixture print what git prints (1B.10)")
{
    Fixture fx;
    const std::string a = fx.dir + "/a.txt";
    std::string out, why;
    CAPTURE(why);
    REQUIRE(madc::git_command("show " + fx.second_sha + ":" + a, out, why));
    CHECK(out == "one\ntwo\n");
    REQUIRE(madc::git_command("log -n 5 " + a, out, why));
    CHECK(starts_with(out, fx.second_sha.substr(0, 7) + " second ("));
    CHECK(out.find("\n" + fx.first_sha.substr(0, 7) + " first (") != std::string::npos);
    CHECK(ends_with(out, " ago, Fixture)\n"));
    REQUIRE(madc::git_command("log -n 1 " + a, out, why));
    CHECK(out.find('\n') == out.size() - 1);	// one row
    REQUIRE(madc::git_command("blame " + a, out, why));
    CHECK(starts_with(out, fx.first_sha.substr(0, 8) + " (Fixture "));
    CHECK(out.find(" 1) one\n" + fx.second_sha.substr(0, 8) + " (Fixture ") != std::string::npos);
    CHECK(ends_with(out, " 2) two\n"));
    REQUIRE(madc::git_command("diff " + a, out, why));
    CHECK(out.empty());				// unchanged: nothing

    fx.write("a.txt", "one\ntwo\nthree\n");
    REQUIRE(madc::git_command("diff " + a, out, why));
    CHECK(out.find("--- a/a.txt\n+++ b/a.txt\n") != std::string::npos);
    CHECK(ends_with(out, "+three\n"));
    const std::string patch = out;
    REQUIRE(madc::git_command("blame " + a + ":3", out, why));
    CHECK(out == "00000000 (Not Committed Yet 3) three\n");
    {
	// No FILE: the working directory's repository.
	in_directory there(fx.dir);
	REQUIRE(madc::git_command("show " + fx.first_sha, out, why));
	CHECK(starts_with(out, "commit " + fx.first_sha
			       + "\nAuthor: Fixture <fixture@example.com>\nDate:   "));
	CHECK(ends_with(out, " ago\n\n    first\n"));
	REQUIRE(madc::git_command("status", out, why));
	CHECK(starts_with(out, "## "));
	CHECK(ends_with(out, "\n M a.txt\n"));
	REQUIRE(madc::git_command("diff", out, why));
	CHECK(out == patch);
    }

    struct { const char *argument, *why; } refused[] = {
	{ "", "which verb? log [-n N] [FILE] | show REV[:FILE] | blame FILE[:LINE] | status | diff [FILE]" },
	{ "frob", "unknown verb 'frob': log [-n N] [FILE] | show REV[:FILE] | blame FILE[:LINE] | status | diff [FILE]" },
	{ "log -n x", "log -n takes a count of commits" },
	{ "log a b", "log takes one FILE" },
	{ "show", "show takes REV or REV:FILE" },
	{ "show :a.txt", "show takes REV or REV:FILE" },
	{ "blame", "blame takes FILE or FILE:LINE" },
	{ "blame a.txt:0", "blame's LINE counts from 1" },
	{ "status now", "status takes no arguments" },
	{ "diff a b", "diff takes at most one FILE" },
	{ "log \"open", "a quote is not closed" },
    };
    for ( size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); ++i )
    {
	CAPTURE(refused[i].argument);
	CHECK_FALSE(madc::git_command(refused[i].argument, out, why));
	CHECK(why == refused[i].why);
    }
}

// A program's own buffers (split arc 1D, madcide's %git): outside a session
// the verbs read the thread's host buffers — a world's documents through
// document_buffer — so an unsaved edit is what diff and blame see, the current
// document names the repository with no FILE, and an empty source returns
// them to the disk. ORACLE: git diff / git blame --contents over the same
// edit (a `+three` hunk; `00000000 (Not Committed Yet 3) three`).
TEST_CASE("%git's verbs read a program's host buffers outside a session (1D)")
{
    Fixture fx;
    const std::string a = fx.dir + "/a.txt";
    madc::hub::world w;
    const madc::hub::entity_id doc = madc::document_open(w, a);
    REQUIRE(doc != 0);
    {
	madc::hub::mutation_context mc(w);
	REQUIRE(mc.text_load(doc, "one\ntwo\nthree\n"));	// unsaved
    }
    madc::set_host_buffers([&w, doc](const madc::dis::istring &name,
				     madc::dis::istring &path, std::string &text,
				     bool &has_text, std::string &why) -> bool {
	return madc::document_buffer(w, doc, name, path, text, has_text, why);
    });
    std::string out, why;
    CAPTURE(why);
    REQUIRE(madc::git_command("diff " + a, out, why));
    CHECK(out.find("--- a/a.txt\n+++ b/a.txt\n") != std::string::npos);
    CHECK(ends_with(out, "+three\n"));
    REQUIRE(madc::git_command("blame " + a + ":3", out, why));
    CHECK(out == "00000000 (Not Committed Yet 3) three\n");
    REQUIRE(madc::git_command("status", out, why));	// no FILE: the current document's
    CHECK(starts_with(out, "## "));
    CHECK(out.find('\n') == out.size() - 1);		// the fixture's: clean on disk
    madc::set_host_buffers(ReplSession::BufferSource());
    REQUIRE(madc::git_command("diff " + a, out, why));	// the disk's: unchanged
    CHECK(out.empty());
}

// git's --date=relative on fixed (when, now) pairs: what git log printed for
// commits exactly that old (madcide's oracle pairs, testmadcide_git).
TEST_CASE("%git's relative dates are git's date.c (1B.10)")
{
    const int64_t now = 1700000000, day = 86400;
    struct { int64_t age; const char *spelled; } pairs[] = {
	{ 1, "1 second ago" }, { 30, "30 seconds ago" }, { 100, "2 minutes ago" },
	{ 5 * 3600, "5 hours ago" }, { 3 * day, "3 days ago" },
	{ 20 * day, "3 weeks ago" }, { 100 * day, "3 months ago" },
	{ 400 * day, "1 year, 1 month ago" }, { 730 * day, "2 years ago" },
	{ 2000 * day, "5 years ago" }, { -10, "in the future" },
    };
    for ( size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); ++i )
	CHECK(madc::git_relative_date(now - pairs[i].age, now) == pairs[i].spelled);
}

// The module's tools entry (1C.14): git::module_tools hands the engine's history
// tools its history source. ORACLE: git over the same fixture — `git log --
// a.txt` (second, first), `git show HEAD~1:a.txt` ("one\n"), `git blame
// --contents` of an edited buffer (first, second, the uncommitted line's
// zero sha), `git status` (clean).
TEST_CASE("the git module's history source answers the history tools over the fixture (1C.14)")
{
    Fixture fx;
    madc::tools::registry r;
    git::module_tools(r);
    const bool has_history = r.history() != 0;
    const bool has_tools = r.find("graph.commits") && r.find("graph.revision")
	&& r.find("graph.diff");
    REQUIRE(has_history);
    CHECK(has_tools);
    madc::tools::history_source &h = *r.history();
    const std::string file = fx.dir + "/a.txt";

    madc::value st;
    REQUIRE(h.state(file, st));
    CHECK(st.as_object().at("head").as_string() == fx.second_sha);
    CHECK(st.as_object().at("path").as_string() == "a.txt");
    CHECK_FALSE(st.as_object().at("dirty").as_boolean());

    madc::value log;
    REQUIRE(h.commits(file, 20, log));
    const std::vector<madc::value> &rows = log.as_object().at("rows").as_array();
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].as_object().at("sha").as_string() == fx.second_sha);
    CHECK(rows[1].as_object().at("summary").as_string() == "first");

    madc::value shown;
    REQUIRE(h.show(file, "HEAD~1", shown));
    CHECK(shown.as_object().at("sha").as_string() == fx.first_sha);
    CHECK(shown.as_object().at("text").as_string() == "one\n");
    CHECK_FALSE(h.show(file, "no-such-rev", shown));
    CHECK(shown.as_object().count("error") == 1);

    madc::value bl;
    REQUIRE(h.blame(file, "one\ntwo\nthree\n", 1, 3, bl));
    const std::vector<madc::value> &hunks = bl.as_object().at("rows").as_array();
    REQUIRE(hunks.size() == 3);
    CHECK(hunks[0].as_object().at("sha").as_string() == fx.first_sha);
    CHECK(hunks[1].as_object().at("sha").as_string() == fx.second_sha);
    CHECK(hunks[2].as_object().at("sha").as_string().empty());

    const std::string bare = make_temp_dir();
    madc::value none;
    CHECK_FALSE(h.state(bare + "/x.c", none));
    CHECK(none.as_object().at("error").as_string().find("no repository above")
	  != std::string::npos);
    rm_rf(bare);
}
