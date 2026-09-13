// test_gitrepo.cpp — madc::GitRepo (the ONE read-only libgit2 owner) and the
// git source adapter over a fixture repository the test BUILDS through
// libgit2's own write API (the archive carries it; GitRepo never calls it).
// Two commits with fixed author times, so every answer is deterministic:
//   c1: a.txt = "one\n"                        (time 1700000000)
//   c2: a.txt = "one\ntwo\n", b.txt = "bee\n"   (time 1700000100)
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#define MADC_UNIT_TEST
#include "doctest.h"

#include "madcdis/git_repo.h"
#include "libmadc/datasource.h"

#include <git2.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
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

TEST_CASE("GitRepo opens a repository and answers head/refs/revparse/log/show/blame/dirty")
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
