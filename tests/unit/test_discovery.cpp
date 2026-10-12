// Unit battery for madc::discovery (split arc 1C item 16;
// include/madc_discovery.h): the advertisement directory, a key's one owner
// and the scan. ORACLE: madcide's session discovery
// (tools/madcide/madcide_discover.inc) for the directory's resolution order
// and the slug; flock(2) for the hold — a second open file's claim on a held
// key is refused in this process as in any other.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <unistd.h>

#include "madc_discovery.h"
#include "libmadc/sysinfo.h"

using madc::discovery::claim;
using madc::discovery::claim_result;

namespace {

std::string temp_dir()
{
    char tmpl[] = "/tmp/madc_discovery_XXXXXX";
    char *made = mkdtemp(tmpl);
    return made ? std::string(made) : std::string();
}

int64_t int_of(const madc::value &rec, const char *key)
{
    return rec.as_object().at(key).as_integer();
}

} // namespace

TEST_CASE("the advertisement directory: MADC_SESSION_DIR, XDG_STATE_HOME, the home")
{
    setenv("MADC_SESSION_DIR", "/over/here", 1);
    CHECK(madc::discovery::session_dir() == "/over/here");
    unsetenv("MADC_SESSION_DIR");
    setenv("XDG_STATE_HOME", "/state", 1);
    CHECK(madc::discovery::session_dir() == "/state/madc/sessions");
    unsetenv("XDG_STATE_HOME");
    setenv("HOME", "/home/someone", 1);
    CHECK(madc::discovery::session_dir() == "/home/someone/.local/state/madc/sessions");
    CHECK(madc::discovery::slug("/work/app/app.prj.json") == "work_app_app.prj.json");
    CHECK(madc::discovery::slug("Z:\\work\\my app") == "Z__work_my_app");
}

TEST_CASE("one owner per key: the second claim is refused with the owner's record")
{
    const std::string dir = temp_dir();
    REQUIRE_FALSE(dir.empty());
    const std::string sessions = dir + "/sessions";
    setenv("MADC_SESSION_DIR", sessions.c_str(), 1);
    const std::string key = "/work/app/app.prj.json";
    std::string path;
    {
	claim first;
	madc::value holder;
	madc::value rec = madc::value::make_object();
	rec.object()["project"] = madc::value(key);
	REQUIRE(first.take(key, rec, holder) == claim_result::owner);	// the directory made
	path = first.path();
	CHECK(path == sessions + "/work_app_app.prj.json.json");

	claim second;
	CHECK(second.take(key, madc::value::make_object(), holder) == claim_result::held_elsewhere);
	REQUIRE(holder.is_object());
	CHECK(int_of(holder, "pid") == madc::sys.pid);
	CHECK(holder.as_object().at("project").as_string() == key);
	CHECK(holder.as_object().at("key").as_string() == key);

	std::vector<madc::value> live = madc::discovery::scan();
	REQUIRE(live.size() == 1u);
	CHECK(live[0].as_object().at("file").as_string() == path);

	claim other;
	CHECK(other.take("/work/other", madc::value::make_object(), holder) == claim_result::owner);
	CHECK(madc::discovery::scan().size() == 2u);
    }
    FILE *gone = std::fopen(path.c_str(), "r");
    CHECK(gone == NULL);					// released: the record goes
    if ( gone )
	std::fclose(gone);
    CHECK(madc::discovery::scan().empty());
    {
	claim again;
	madc::value holder;
	CHECK(again.take(key, madc::value::make_object(), holder) == claim_result::owner);
    }
    unsetenv("MADC_SESSION_DIR");
    rmdir(sessions.c_str());
    rmdir(dir.c_str());
}

TEST_CASE("update restamps the record; find prefers a document's holder to a root; release withdraws")
{
    const std::string dir = temp_dir();
    REQUIRE_FALSE(dir.empty());
    const std::string sessions = dir + "/sessions";
    setenv("MADC_SESSION_DIR", sessions.c_str(), 1);
    CHECK(madc::discovery::find("").is_null());			// an untitled buffer: no one's

    std::vector<madc::value> held(1, madc::value(dir + "/app/main.c"));
    madc::value covers = madc::value::make_object();
    covers.object()["root"] = madc::value(dir);
    covers.object()["endpoint"] = madc::value("root");
    madc::value holds = madc::value::make_object();
    holds.object()["root"] = madc::value(dir + "/app");
    holds.object()["documents"] = madc::value::make_array(held);
    holds.object()["endpoint"] = madc::value("holder");
    madc::value holder;
    claim a, b;
    REQUIRE(a.take("/work/a", covers, holder) == claim_result::owner);
    REQUIRE(b.take("/work/b", holds, holder) == claim_result::owner);
    const int64_t started = int_of(madc::discovery::scan()[0], "started");

    madc::value hit = madc::discovery::find(dir + "/app/main.c");
    REQUIRE(hit.is_object());
    CHECK(hit.as_object().at("endpoint").as_string() == "holder");	// the holder wins
    hit = madc::discovery::find(dir + "/lib/util.c");
    REQUIRE(hit.is_object());
    CHECK(hit.as_object().at("endpoint").as_string() == "root");
    CHECK(madc::discovery::find(dir + "-sibling/x.c").is_null());	// a sibling is not inside

    covers.object()["endpoint"] = madc::value("moved");
    CHECK(a.update(covers));
    hit = madc::discovery::find(dir + "/lib/util.c");
    CHECK(hit.as_object().at("endpoint").as_string() == "moved");
    CHECK(hit.as_object().at("key").as_string() == "/work/a");	// the stamp kept
    CHECK(int_of(hit, "started") == started);

    b.release();
    CHECK(madc::discovery::scan().size() == 1u);
    CHECK(b.update(holds) == false);				// released: holds nothing
    hit = madc::discovery::find(dir + "/app/main.c");
    CHECK(hit.as_object().at("endpoint").as_string() == "moved");	// now only covered
    a.release();
    CHECK(madc::discovery::scan().empty());
    unsetenv("MADC_SESSION_DIR");
    rmdir(sessions.c_str());
    rmdir(dir.c_str());
}
